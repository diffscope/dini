#include "engine_p.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

#include <dini/errors.h>

#include "change_p.h"
#include "query_p.h"
#include "view_p.h"

namespace dini {

    namespace {

    constexpr std::uint8_t snapshotMagic[] = {'D', 'I', 'N', 'I', 'S', 'N', 'A', 'P', '2'};
    constexpr std::uint8_t commitLogEnvelopeMagic[] = {'D', 'I', 'N', 'I', 'L', 'O', 'G', '2'};
    constexpr std::uint32_t snapshotFormatVersion = 2;
    constexpr std::uint32_t commitLogEnvelopeVersion = 1;
    constexpr std::size_t maxHookDepth = 32;

    std::string valueKey(const Value &value)
    {
        std::ostringstream stream;
        stream << static_cast<int>(value.type()) << ':';
        std::visit(
            [&](const auto &payload) {
                using T = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<T, std::monostate>) {
                    stream << "null";
                } else if constexpr (std::is_same_v<T, bool>) {
                    stream << (payload ? "true" : "false");
                } else if constexpr (std::is_same_v<T, std::string>) {
                    stream << payload;
                } else if constexpr (std::is_same_v<T, ByteArray>) {
                    for (auto byte : payload) {
                        stream << static_cast<unsigned int>(byte) << ',';
                    }
                } else {
                    stream << payload;
                }
            },
            value.storage());
        return stream.str();
    }

    Value idValue(ItemId id)
    {
        return Value(static_cast<std::uint64_t>(id));
    }

    ContainerId associationTargetContainerId(const AssociationTarget &target)
    {
        return std::visit([](const auto &handle) { return handle.containerId(); }, target);
    }

    void hashCombine(std::uint64_t &seed, std::uint64_t value) noexcept
    {
        seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
    }

    void hashString(std::uint64_t &seed, const std::string &value) noexcept
    {
        for (const auto ch : value) {
            hashCombine(seed, static_cast<unsigned char>(ch));
        }
    }

    std::uint64_t schemaFingerprint(const SchemaDefinitionData &schemaDefinition)
    {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const auto &container : schemaDefinition.containers) {
            hashCombine(hash, static_cast<std::uint64_t>(container.info.kind));
            hashCombine(hash, container.info.id);
            hashString(hash, container.info.debugName);
            hashCombine(hash, container.listAssociation.value_or(0));
            for (const auto &column : container.columns) {
                hashCombine(hash, column.info.id);
                hashString(hash, column.info.debugName);
                hashCombine(hash, static_cast<std::uint64_t>(column.info.type));
                hashCombine(hash, static_cast<std::uint64_t>(column.info.index));
                hashCombine(hash, column.info.computed ? 1 : 0);
                hashCombine(hash, column.info.association ? 1 : 0);
                hashCombine(hash, column.info.variantSpecific ? 1 : 0);
                hashCombine(hash, column.variantId.value_or(0));
                const auto aggregate = column.info.computed
                    ? column.computedDefinition.aggregateIndex
                    : (column.info.variantSpecific
                           ? column.variantDefinition.aggregateIndex
                           : column.normalDefinition.aggregateIndex);
                hashCombine(hash, aggregate.sum ? 1 : 0);
                hashCombine(hash, aggregate.minMax ? 1 : 0);
                hashCombine(hash, aggregate.byParent ? 1 : 0);
                for (const auto &dependency : column.computedDefinition.dependsOn) {
                    hashCombine(hash, dependency.containerId());
                    hashCombine(hash, dependency.columnId());
                }
            }
            for (const auto &rangeIndex : container.rangeIndexes) {
                hashString(hash, rangeIndex.debugName);
                hashCombine(hash, rangeIndex.columns.size());
                for (const auto &column : rangeIndex.columns) {
                    hashCombine(hash, column.containerId());
                    hashCombine(hash, column.columnId());
                }
            }
            for (const auto &relation : container.relations) {
                hashCombine(hash, relation.info.id);
                hashCombine(hash, relation.info.column.columnId());
                hashCombine(hash, associationTargetContainerId(relation.info.target));
            }
            for (const auto &variant : container.variants) {
                hashCombine(hash, variant.id);
                hashString(hash, variant.debugName);
            }
        }
        return hash;
    }

    std::uint64_t unixSecondsNow()
    {
        using namespace std::chrono;
        return static_cast<std::uint64_t>(duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
    }

    ItemId generateItemId(DocumentEngine::Impl &engine)
    {
        const auto now = unixSecondsNow();
        const auto elapsed64 = now > engine.epochSeconds ? now - engine.epochSeconds : 0;
        auto elapsed = static_cast<std::uint32_t>(std::min<std::uint64_t>(elapsed64, 0xffffffffULL));
        if (elapsed <= engine.currentElapsedSecond) {
            elapsed = engine.currentElapsedSecond;
            ++engine.currentSecondCounter;
            if (engine.currentSecondCounter == 0) {
                ++elapsed;
                engine.currentSecondCounter = 1;
            }
        } else {
            engine.currentElapsedSecond = elapsed;
            engine.currentSecondCounter = 1;
        }
        engine.currentElapsedSecond = elapsed;
        return (static_cast<ItemId>(elapsed) << 32) | engine.currentSecondCounter;
    }

    void observeItemId(DocumentEngine::Impl &engine, ItemId id)
    {
        const auto elapsed = static_cast<std::uint32_t>(id >> 32);
        const auto counter = static_cast<std::uint32_t>(id & 0xffffffffULL);
        if (elapsed > engine.currentElapsedSecond ||
            (elapsed == engine.currentElapsedSecond && counter > engine.currentSecondCounter)) {
            engine.currentElapsedSecond = elapsed;
            engine.currentSecondCounter = counter;
        }
    }

    std::optional<ItemId> itemIdFromValue(const Value &value)
    {
        if (value.isNull()) {
            return {};
        }
        if (value.type() == ValueType::UInt64) {
            return static_cast<ItemId>(value.asUInt64());
        }
        if (value.type() == ValueType::Int64 && value.asInt64() >= 0) {
            return static_cast<ItemId>(value.asInt64());
        }
        return {};
    }

    int compareValues(const Value &lhs, const Value &rhs)
    {
        if (lhs.storage() == rhs.storage()) {
            return 0;
        }
        if (lhs.type() != rhs.type()) {
            return static_cast<int>(lhs.type()) < static_cast<int>(rhs.type()) ? -1 : 1;
        }
        return std::visit(
            [](const auto &a, const auto &b) -> int {
                using A = std::decay_t<decltype(a)>;
                using B = std::decay_t<decltype(b)>;
                if constexpr (!std::is_same_v<A, B>) {
                    return 0;
                } else if constexpr (std::is_same_v<A, std::monostate>) {
                    return 0;
                } else {
                    return a < b ? -1 : 1;
                }
            },
            lhs.storage(),
            rhs.storage());
    }

    bool comparePredicate(const Value &lhs, ComparisonOperator op, const Value &rhs)
    {
        const auto comparison = compareValues(lhs, rhs);
        switch (op) {
            case ComparisonOperator::Equal:
                return comparison == 0;
            case ComparisonOperator::NotEqual:
                return comparison != 0;
            case ComparisonOperator::Greater:
                return comparison > 0;
            case ComparisonOperator::Less:
                return comparison < 0;
            case ComparisonOperator::GreaterOrEqual:
                return comparison >= 0;
            case ComparisonOperator::LessOrEqual:
                return comparison <= 0;
        }
        return false;
    }

    bool columnIsIndexed(const ColumnDefinitionRecord &column)
    {
        return column.info.index != IndexKind::None || column.info.association;
    }

    const ContainerDefinitionRecord &containerFor(const SchemaDefinitionData &schemaDefinition, ContainerId containerId)
    {
        const auto *container = findContainer(schemaDefinition, containerId);
        if (!container) {
            throw HandleError("container does not belong to schema");
        }
        return *container;
    }

    const ColumnDefinitionRecord &columnFor(const SchemaDefinitionData &schemaDefinition, ColumnHandle column)
    {
        const auto *record = findColumn(schemaDefinition, column.containerId(), column.columnId());
        if (!record) {
            throw HandleError("column does not belong to schema");
        }
        return *record;
    }

    Value valueForColumn(const ItemSnapshot &item, ColumnId columnId)
    {
        auto it = std::find_if(item.values.begin(), item.values.end(), [columnId](const ColumnValue &columnValue) {
            return columnValue.column.columnId() == columnId;
        });
        return it == item.values.end() ? Value::null() : it->value;
    }

    bool hasColumnValue(const ItemSnapshot &item, ColumnId columnId)
    {
        return std::any_of(item.values.begin(), item.values.end(), [columnId](const ColumnValue &columnValue) {
            return columnValue.column.columnId() == columnId;
        });
    }

    void setColumnValue(ItemSnapshot &item, ColumnHandle column, Value value)
    {
        auto it = std::find_if(item.values.begin(), item.values.end(), [&](const ColumnValue &columnValue) {
            return columnValue.column.columnId() == column.columnId();
        });
        if (it == item.values.end()) {
            item.values.push_back(ColumnValue {.column = std::move(column), .value = std::move(value)});
        } else {
            it->value = std::move(value);
        }
    }

    void addItemToIndexes(DocumentEngine::Impl &engine, const ItemSnapshot &snapshot)
    {
        engine.indexes.addItem(schemaData(engine.schema), snapshot);
    }

    void removeItemFromIndexes(DocumentEngine::Impl &engine, const ItemSnapshot &snapshot)
    {
        engine.indexes.removeItem(schemaData(engine.schema), snapshot);
    }

    void rebuildIndexes(DocumentEngine::Impl &engine)
    {
        engine.indexes.clear();
        for (const auto &[id, item] : engine.items) {
            (void)id;
            addItemToIndexes(engine, item.snapshot);
        }
    }

    bool valueMatchesType(const Value &value, ValueType type, bool nullable)
    {
        if (value.isNull()) {
            return nullable;
        }
        return value.type() == type;
    }

    void validateValueForColumn(const ColumnDefinitionRecord &column, const Value &value)
    {
        bool nullable = true;
        std::function<bool(const Value &)> check;
        if (column.info.computed) {
            nullable = column.computedDefinition.nullable;
        } else if (column.info.variantSpecific) {
            nullable = column.variantDefinition.nullable;
            check = column.variantDefinition.check;
        } else {
            nullable = column.normalDefinition.nullable;
            check = column.normalDefinition.check;
        }
        if (!valueMatchesType(value, column.info.type, nullable)) {
            throw ConstraintError("value type does not match column definition");
        }
        if (check && !value.isNull() && !check(value)) {
            throw ConstraintError("column check predicate failed");
        }
    }

    void applyRelationMetadata(const SchemaDefinitionData &schemaDefinition, ItemSnapshot &snapshot)
    {
        snapshot.parentId.reset();
        const auto &container = containerFor(schemaDefinition, snapshot.containerId);
        for (const auto &relation : container.relations) {
            const auto value = valueForColumn(snapshot, relation.info.column.columnId());
            snapshot.parentId = itemIdFromValue(value);
        }
    }

    void refreshListIndexes(DocumentEngine::Impl &engine, ContainerId listId, const std::string &associationKey)
    {
        auto groupIt = engine.listGroups.find({listId, associationKey});
        if (groupIt == engine.listGroups.end()) {
            return;
        }
        for (std::size_t i = 0; i < groupIt->second.size(); ++i) {
            auto itemIt = engine.items.find(groupIt->second[i]);
            if (itemIt != engine.items.end()) {
                itemIt->second.snapshot.listIndex = i;
            }
        }
    }

    void removeFromListGroup(DocumentEngine::Impl &engine, const ItemSnapshot &snapshot)
    {
        if (snapshot.containerKind != ContainerKind::List || !snapshot.listAssociationValue ||
            snapshot.listAssociationValue->isNull()) {
            return;
        }
        const auto key = valueKey(*snapshot.listAssociationValue);
        auto groupIt = engine.listGroups.find({snapshot.containerId, key});
        if (groupIt == engine.listGroups.end()) {
            return;
        }
        auto &group = groupIt->second;
        group.erase(std::remove(group.begin(), group.end(), snapshot.id), group.end());
        refreshListIndexes(engine, snapshot.containerId, key);
    }

    void insertIntoListGroup(DocumentEngine::Impl &engine, const ItemSnapshot &snapshot)
    {
        if (snapshot.containerKind != ContainerKind::List || !snapshot.listAssociationValue ||
            snapshot.listAssociationValue->isNull()) {
            return;
        }
        const auto key = valueKey(*snapshot.listAssociationValue);
        auto &group = engine.listGroups[{snapshot.containerId, key}];
        auto index = snapshot.listIndex.value_or(group.size());
        if (index > group.size()) {
            throw ConstraintError("list insertion index is out of range");
        }
        group.insert(group.begin() + static_cast<std::ptrdiff_t>(index), snapshot.id);
        refreshListIndexes(engine, snapshot.containerId, key);
    }

    void publish(DocumentEngine::Impl &engine, EventKind kind, EventOrigin origin, const ChangeSet &changeSet)
    {
        EngineEvent event {.kind = kind, .origin = origin, .changeSet = changeSet};
        for (auto it = engine.subscriptions.begin(); it != engine.subscriptions.end();) {
            auto state = it->lock();
            if (!state) {
                it = engine.subscriptions.erase(it);
                continue;
            }
            if (state->connected && state->callback) {
                state->callback(event);
            }
            ++it;
        }
    }

    ContainerId operationContainerId(const ChangeOperation &operation)
    {
        return std::visit(
            [](const auto &change) -> ContainerId {
                using T = std::decay_t<decltype(change)>;
                if constexpr (std::is_same_v<T, ColumnUpdatedChange> ||
                              std::is_same_v<T, ComputedColumnUpdatedChange>) {
                    return change.column.containerId();
                } else if constexpr (std::is_same_v<T, ListRotatedChange>) {
                    return change.list.containerId();
                } else {
                    return change.item.containerId;
                }
            },
            operation.payload());
    }

    std::set<ContainerId> affectedContainers(const ChangeSet &changeSet)
    {
        std::set<ContainerId> result;
        for (const auto &operation : changeSet.operations()) {
            result.insert(operationContainerId(operation));
        }
        return result;
    }

    ChangeSet changeSetForContainer(const ChangeSet &changeSet, ContainerId containerId)
    {
        ChangeSet scoped;
        std::vector<std::size_t> indexMap(changeSet.operations().size(), changeSet.operations().size());
        for (std::size_t i = 0; i < changeSet.operations().size(); ++i) {
            if (operationContainerId(changeSet.operations()[i]) == containerId) {
                indexMap[i] = scoped.operations().size();
                scoped.append(changeSet.operations()[i]);
            }
        }
        for (const auto &link : changeSet.derivedLinks()) {
            if (link.sourceOperation >= indexMap.size() || link.derivedOperation >= indexMap.size()) {
                continue;
            }
            const auto source = indexMap[link.sourceOperation];
            const auto derived = indexMap[link.derivedOperation];
            if (source != changeSet.operations().size() && derived != changeSet.operations().size()) {
                scoped.addDerivedLink(DerivedChangeLink {.sourceOperation = source, .derivedOperation = derived});
            }
        }
        return scoped;
    }

    void runHooks(DocumentEngine::Impl &engine,
                  HookStage stage,
                  TransactionContext &context,
                  const ChangeSet &changeSet,
                  bool mutationAllowed)
    {
        if (engine.hookDepth >= maxHookDepth) {
            throw HookError("maximum hook recursion depth exceeded");
        }
        ++engine.hookDepth;
        auto previousStage = engine.hookStage;
        engine.hookStage = stage;
        (void)context;
        (void)mutationAllowed;
        const auto &schemaDefinition = schemaData(engine.schema);
        const auto containers = affectedContainers(changeSet);
        try {
            for (const auto &container : schemaDefinition.containers) {
                if (containers.find(container.info.id) == containers.end()) {
                    continue;
                }
                auto scopedChangeSet = changeSetForContainer(changeSet, container.info.id);
                if (scopedChangeSet.empty()) {
                    continue;
                }
                for (const auto &hook : container.hooks) {
                    if (hook.stage == stage && hook.callback) {
                        hook.callback(context, scopedChangeSet);
                    }
                }
            }
            engine.hookStage = previousStage;
            --engine.hookDepth;
        } catch (...) {
            engine.hookStage = previousStage;
            --engine.hookDepth;
            throw;
        }
    }

    void appendWithDerivedLinks(Transaction::Impl &transaction,
                                const ChangeSet &operationChange,
                                std::size_t derivedStart,
                                std::size_t derivedEnd)
    {
        const auto sourceStart = transaction.changeSet.operations().size();
        for (const auto &operation : operationChange.operations()) {
            transaction.changeSet.append(operation);
        }

        if (operationChange.operations().empty()) {
            return;
        }

        for (const auto &link : operationChange.derivedLinks()) {
            transaction.changeSet.addDerivedLink(DerivedChangeLink {
                .sourceOperation = sourceStart + link.sourceOperation,
                .derivedOperation = sourceStart + link.derivedOperation,
            });
        }

        for (auto derived = derivedStart; derived < derivedEnd; ++derived) {
            transaction.changeSet.addDerivedLink(DerivedChangeLink {
                .sourceOperation = sourceStart,
                .derivedOperation = derived,
            });
        }

        for (std::size_t derived = 1; derived < operationChange.operations().size(); ++derived) {
            transaction.changeSet.addDerivedLink(DerivedChangeLink {
                .sourceOperation = sourceStart,
                .derivedOperation = sourceStart + derived,
            });
        }
    }

    void requireActive(Transaction::Impl *impl)
    {
        if (!impl || impl->state != TransactionState::Active || !impl->engine) {
            throw TransactionError("transaction is not active");
        }
    }

    void markFailed(Transaction::Impl &transaction)
    {
        if (transaction.state == TransactionState::Active) {
            transaction.state = TransactionState::Failed;
            if (transaction.engine && !transaction.rollbackApplied) {
                transaction.engine->items = transaction.rollbackItems;
                transaction.engine->listGroups = transaction.rollbackListGroups;
                transaction.engine->indexes = transaction.rollbackIndexes;
                transaction.engine->activeTransaction = false;
                transaction.rollbackApplied = true;
            }
        }
    }

    void ensureNoCycle(const DocumentEngine::Impl &engine, ItemId itemId, std::optional<ItemId> parentId)
    {
        std::set<ItemId> seen;
        while (parentId) {
            if (*parentId == itemId || seen.find(*parentId) != seen.end()) {
                throw ConstraintError("parent relation would create a cycle");
            }
            seen.insert(*parentId);
            auto it = engine.items.find(*parentId);
            if (it == engine.items.end()) {
                throw ConstraintError("parent item does not exist");
            }
            parentId = it->second.snapshot.parentId;
        }
    }

    void validateParentRelations(const DocumentEngine::Impl &engine, const ItemSnapshot &snapshot)
    {
        const auto &schemaDefinition = schemaData(engine.schema);
        const auto &container = containerFor(schemaDefinition, snapshot.containerId);
        for (const auto &relation : container.relations) {
            const auto value = valueForColumn(snapshot, relation.info.column.columnId());
            if (value.isNull()) {
                continue;
            }
            const auto targetId = itemIdFromValue(value);
            if (!targetId) {
                throw ConstraintError("association value must be an item id");
            }
            const auto targetIt = engine.items.find(*targetId);
            if (targetIt == engine.items.end()) {
                throw ConstraintError("association target item does not exist");
            }
            if (targetIt->second.snapshot.containerId != associationTargetContainerId(relation.info.target)) {
                throw ConstraintError("association target container does not match schema");
            }
        }
        if (snapshot.parentId) {
            ensureNoCycle(engine, snapshot.id, snapshot.parentId);
        }
    }

    bool participatesInUnique(const ColumnDefinitionRecord &column, const Value &value)
    {
        if (column.info.index != IndexKind::Unique) {
            return false;
        }
        if (!value.isNull()) {
            return true;
        }
        if (column.info.variantSpecific) {
            return column.variantDefinition.nullable;
        }
        return column.normalDefinition.participatesInUniqueWhenNull;
    }

    void validateUniqueColumns(const DocumentEngine::Impl &engine, const ItemSnapshot &snapshot)
    {
        const auto &schemaDefinition = schemaData(engine.schema);
        const auto &container = containerFor(schemaDefinition, snapshot.containerId);
        const bool parentScopedUnique = !container.relations.empty();
        if (parentScopedUnique && !snapshot.parentId) {
            return;
        }
        for (const auto &column : container.columns) {
            if (column.info.index != IndexKind::Unique) {
                continue;
            }
            const auto value = valueForColumn(snapshot, column.info.id);
            if (!participatesInUnique(column, value)) {
                continue;
            }
            for (const auto &[otherId, other] : engine.items) {
                if (otherId == snapshot.id || other.snapshot.containerId != snapshot.containerId) {
                    continue;
                }
                if (parentScopedUnique) {
                    if (snapshot.parentId != other.snapshot.parentId) {
                        continue;
                    }
                }
                if (column.info.variantSpecific &&
                    (!other.snapshot.variant || other.snapshot.variant->variantId() != column.variantId)) {
                    continue;
                }
                if (valueForColumn(other.snapshot, column.info.id) == value) {
                    throw ConstraintError("unique column constraint failed");
                }
            }
        }
    }

    void fillDefaults(const SchemaDefinitionData &schemaDefinition, ItemSnapshot &snapshot)
    {
        const auto &container = containerFor(schemaDefinition, snapshot.containerId);
        for (const auto &column : container.columns) {
            if (hasColumnValue(snapshot, column.info.id)) {
                continue;
            }
            if (column.info.computed || column.info.association) {
                continue;
            }
            if (column.info.variantSpecific) {
                if (!snapshot.variant || snapshot.variant->variantId() != column.variantId) {
                    continue;
                }
                if (column.variantDefinition.defaultValue) {
                    setColumnValue(snapshot,
                                   ColumnHandle(schemaDefinition.schemaId,
                                                snapshot.containerId,
                                                column.info.id,
                                                column.info.debugName),
                                   *column.variantDefinition.defaultValue);
                }
            } else if (column.normalDefinition.defaultValue) {
                setColumnValue(snapshot,
                               ColumnHandle(schemaDefinition.schemaId,
                                            snapshot.containerId,
                                            column.info.id,
                                            column.info.debugName),
                               *column.normalDefinition.defaultValue);
            }
        }
    }

    std::vector<ComputedColumnUpdatedChange> recomputeComputedColumns(const SchemaDefinitionData &schemaDefinition,
                                                                      ItemSnapshot &snapshot)
    {
        std::vector<ComputedColumnUpdatedChange> changes;
        const auto &container = containerFor(schemaDefinition, snapshot.containerId);
        for (const auto &column : container.columns) {
            if (!column.info.computed || !column.computedDefinition.compute) {
                continue;
            }
            std::vector<Value> arguments;
            arguments.reserve(column.computedDefinition.dependsOn.size());
            for (const auto &dependency : column.computedDefinition.dependsOn) {
                if (dependency.containerId() != snapshot.containerId) {
                    throw SchemaError("computed column dependency belongs to another container");
                }
                arguments.push_back(valueForColumn(snapshot, dependency.columnId()));
            }
            auto newValue = column.computedDefinition.compute(arguments);
            validateValueForColumn(column, newValue);
            const auto oldValue = valueForColumn(snapshot, column.info.id);
            if (oldValue != newValue) {
                auto handle = ColumnHandle(schemaDefinition.schemaId,
                                           snapshot.containerId,
                                           column.info.id,
                                           column.info.debugName);
                setColumnValue(snapshot, handle, newValue);
                changes.push_back(ComputedColumnUpdatedChange {
                    .itemId = snapshot.id,
                    .column = handle,
                    .oldValue = oldValue,
                    .newValue = std::move(newValue),
                });
            }
        }
        return changes;
    }

    void validateSnapshot(DocumentEngine::Impl &engine, ItemSnapshot &snapshot)
    {
        const auto &schemaDefinition = schemaData(engine.schema);
        const auto &container = containerFor(schemaDefinition, snapshot.containerId);
        if (snapshot.containerKind != container.info.kind) {
            throw ConstraintError("item container kind does not match schema");
        }
        if (!container.variants.empty() && !snapshot.variant) {
            throw ConstraintError("polymorphic item requires an explicit variant");
        }
        if (snapshot.variant) {
            if (snapshot.variant->schemaId() != schemaDefinition.schemaId ||
                snapshot.variant->containerId() != snapshot.containerId) {
                throw ConstraintError("variant does not belong to item container");
            }
            bool found = false;
            for (const auto &variant : container.variants) {
                found = found || variant.id == snapshot.variant->variantId();
            }
            if (!found) {
                throw ConstraintError("variant does not belong to container");
            }
        }
        fillDefaults(schemaDefinition, snapshot);
        for (const auto &columnValue : snapshot.values) {
            if (columnValue.column.schemaId() != schemaDefinition.schemaId ||
                columnValue.column.containerId() != snapshot.containerId) {
                throw ConstraintError("column does not belong to item container");
            }
            const auto &column = columnFor(schemaDefinition, columnValue.column);
            if (column.info.variantSpecific &&
                (!snapshot.variant || snapshot.variant->variantId() != column.variantId)) {
                throw ConstraintError("variant-specific column does not match item variant");
            }
            validateValueForColumn(column, columnValue.value);
        }
        for (const auto &column : container.columns) {
            if (column.info.computed) {
                continue;
            }
            if (column.info.variantSpecific &&
                (!snapshot.variant || snapshot.variant->variantId() != column.variantId)) {
                continue;
            }
            validateValueForColumn(column, valueForColumn(snapshot, column.info.id));
        }
        applyRelationMetadata(schemaDefinition, snapshot);
        validateParentRelations(engine, snapshot);
        validateUniqueColumns(engine, snapshot);
    }

    struct PendingBeforeApplyScope {
        Transaction::Impl &transaction;
        ItemSnapshot *previousSnapshot = nullptr;
        bool previousInsert = false;
        ColumnId previousPrimaryColumn = 0;

        PendingBeforeApplyScope(Transaction::Impl &transaction,
                                ItemSnapshot &snapshot,
                                bool insert,
                                ColumnId primaryColumn)
            : transaction(transaction),
              previousSnapshot(transaction.pendingBeforeApplySnapshot),
              previousInsert(transaction.pendingBeforeApplyInsert),
              previousPrimaryColumn(transaction.pendingBeforeApplyPrimaryColumn)
        {
            transaction.pendingBeforeApplySnapshot = &snapshot;
            transaction.pendingBeforeApplyInsert = insert;
            transaction.pendingBeforeApplyPrimaryColumn = primaryColumn;
        }

        ~PendingBeforeApplyScope()
        {
            transaction.pendingBeforeApplySnapshot = previousSnapshot;
            transaction.pendingBeforeApplyInsert = previousInsert;
            transaction.pendingBeforeApplyPrimaryColumn = previousPrimaryColumn;
        }
    };

    void resetComputedColumnValues(const SchemaDefinitionData &schemaDefinition,
                                   const ItemSnapshot &oldSnapshot,
                                   ItemSnapshot &snapshot)
    {
        const auto &container = containerFor(schemaDefinition, snapshot.containerId);
        for (const auto &column : container.columns) {
            if (!column.info.computed) {
                continue;
            }
            setColumnValue(snapshot,
                           ColumnHandle(schemaDefinition.schemaId,
                                        snapshot.containerId,
                                        column.info.id,
                                        column.info.debugName),
                           valueForColumn(oldSnapshot, column.info.id));
        }
    }

    void resetComputedColumnValuesToNull(const SchemaDefinitionData &schemaDefinition,
                                         ItemSnapshot &snapshot)
    {
        const auto &container = containerFor(schemaDefinition, snapshot.containerId);
        for (const auto &column : container.columns) {
            if (!column.info.computed) {
                continue;
            }
            setColumnValue(snapshot,
                           ColumnHandle(schemaDefinition.schemaId,
                                        snapshot.containerId,
                                        column.info.id,
                                        column.info.debugName),
                           Value::null());
        }
    }

    ChangeSet insertChangeSetForSnapshot(const SchemaDefinitionData &schemaDefinition,
                                         ItemSnapshot &snapshot)
    {
        resetComputedColumnValuesToNull(schemaDefinition, snapshot);
        auto computedChanges = recomputeComputedColumns(schemaDefinition, snapshot);
        ChangeSet operationChange;
        operationChange.append(ChangeOperation(ItemInsertedChange {
            .item = snapshot,
        }));
        for (const auto &change : computedChanges) {
            operationChange.append(ChangeOperation(change));
        }
        return operationChange;
    }

    ChangeSet listInsertChangeSetForSnapshot(const SchemaDefinitionData &schemaDefinition,
                                             ListHandle list,
                                             const Value &associationValue,
                                             std::size_t index,
                                             ItemSnapshot &snapshot)
    {
        resetComputedColumnValuesToNull(schemaDefinition, snapshot);
        auto computedChanges = recomputeComputedColumns(schemaDefinition, snapshot);
        ChangeSet operationChange;
        operationChange.append(ChangeOperation(ListInsertedChange {
            .list = std::move(list),
            .associationValue = associationValue,
            .index = index,
            .item = snapshot,
        }));
        for (const auto &change : computedChanges) {
            operationChange.append(ChangeOperation(change));
        }
        return operationChange;
    }

    ChangeSet updateChangeSetForSnapshot(const SchemaDefinitionData &schemaDefinition,
                                         const ItemSnapshot &oldSnapshot,
                                         ItemId itemId,
                                         ColumnHandle column,
                                         Value oldValue,
                                         Value newValue,
                                         AssociationUpdateOptions options,
                                         std::optional<std::size_t> oldListIndex,
                                         ItemSnapshot &snapshot)
    {
        resetComputedColumnValues(schemaDefinition, oldSnapshot, snapshot);
        auto computedChanges = recomputeComputedColumns(schemaDefinition, snapshot);
        ChangeSet operationChange;
        operationChange.append(ChangeOperation(ColumnUpdatedChange {
            .itemId = itemId,
            .column = std::move(column),
            .oldValue = std::move(oldValue),
            .newValue = std::move(newValue),
            .associationOptions = options,
            .oldListIndex = oldListIndex,
        }));
        for (const auto &change : computedChanges) {
            operationChange.append(ChangeOperation(change));
        }
        return operationChange;
    }

    bool updatePendingBeforeApplySource(Transaction::Impl &transaction,
                                        ItemId itemId,
                                        ColumnHandle column,
                                        Value value,
                                        AssociationUpdateOptions options)
    {
        auto *engine = transaction.engine;
        auto *snapshot = transaction.pendingBeforeApplySnapshot;
        if (!engine || !snapshot || engine->hookStage != HookStage::BeforeApply || snapshot->id != itemId) {
            return false;
        }
        if (snapshot->containerId != column.containerId()) {
            throw QueryError("column does not belong to item");
        }
        if (options.targetIndex) {
            throw ConstraintError("pending source updates only support normal columns");
        }
        const auto &schemaDefinition = schemaData(engine->schema);
        const auto &columnRecord = columnFor(schemaDefinition, column);
        if (columnRecord.info.computed || columnRecord.info.association) {
            throw ConstraintError("pending source updates only support normal columns");
        }
        if (!transaction.pendingBeforeApplyInsert &&
            transaction.pendingBeforeApplyPrimaryColumn == column.columnId()) {
            throw ConstraintError("pending source update cannot overwrite the triggering column");
        }
        validateValueForColumn(columnRecord, value);

        auto updatedSnapshot = *snapshot;
        const auto oldValue = valueForColumn(updatedSnapshot, column.columnId());
        setColumnValue(updatedSnapshot, column, value);
        validateSnapshot(*engine, updatedSnapshot);
        *snapshot = updatedSnapshot;

        if (!transaction.pendingBeforeApplyInsert) {
            ChangeSet operationChange;
            operationChange.append(ChangeOperation(ColumnUpdatedChange {
                .itemId = itemId,
                .column = std::move(column),
                .oldValue = oldValue,
                .newValue = std::move(value),
            }));
            const auto derivedIndex = transaction.changeSet.operations().size();
            appendWithDerivedLinks(transaction, operationChange, derivedIndex, derivedIndex);
        }
        return true;
    }

    void insertSnapshot(DocumentEngine::Impl &engine, ItemSnapshot snapshot)
    {
        validateSnapshot(engine, snapshot);
        const auto id = snapshot.id;
        insertIntoListGroup(engine, snapshot);
        engine.items[id] = RuntimeItem {std::move(snapshot)};
        addItemToIndexes(engine, engine.items.at(id).snapshot);
        observeItemId(engine, id);
    }

    void eraseSnapshot(DocumentEngine::Impl &engine, const ItemSnapshot &snapshot)
    {
        removeItemFromIndexes(engine, snapshot);
        removeFromListGroup(engine, snapshot);
        engine.items.erase(snapshot.id);
    }

    void applyOperation(DocumentEngine::Impl &engine, const ChangeOperation &operation)
    {
        std::visit(
            [&](const auto &change) {
                using T = std::decay_t<decltype(change)>;
                if constexpr (std::is_same_v<T, ItemInsertedChange>) {
                    insertSnapshot(engine, change.item);
                } else if constexpr (std::is_same_v<T, ItemRemovedChange>) {
                    eraseSnapshot(engine, change.item);
                } else if constexpr (std::is_same_v<T, CascadeRemovedChange>) {
                    eraseSnapshot(engine, change.item);
                } else if constexpr (std::is_same_v<T, ColumnUpdatedChange>) {
                    engine.schema.validate(change.column);
                    auto it = engine.items.find(change.itemId);
                    if (it == engine.items.end()) {
                        throw RecoveryError("updated item does not exist");
                    }
                    auto oldSnapshot = it->second.snapshot;
                    auto snapshot = oldSnapshot;
                    setColumnValue(snapshot, change.column, change.newValue);
                    const auto &schemaDefinition = schemaData(engine.schema);
                    auto relation = findRelationByColumn(const_cast<SchemaDefinitionData &>(schemaDefinition),
                                                         change.column.containerId(),
                                                         change.column.columnId());
                    if (relation && snapshot.containerKind == ContainerKind::List &&
                        containerFor(schemaDefinition, snapshot.containerId).listAssociation == relation->info.id) {
                        if (change.newValue.isNull()) {
                            snapshot.listAssociationValue.reset();
                            snapshot.listIndex.reset();
                        } else {
                            if (!change.associationOptions.targetIndex) {
                                throw RecoveryError("target index is required for list association replay");
                            }
                            snapshot.listAssociationValue = change.newValue;
                            snapshot.listIndex = *change.associationOptions.targetIndex;
                        }
                    }
                    applyRelationMetadata(schemaDefinition, snapshot);
                    validateSnapshot(engine, snapshot);
                    removeItemFromIndexes(engine, oldSnapshot);
                    removeFromListGroup(engine, oldSnapshot);
                    it->second.snapshot = snapshot;
                    insertIntoListGroup(engine, snapshot);
                    addItemToIndexes(engine, snapshot);
                } else if constexpr (std::is_same_v<T, ComputedColumnUpdatedChange>) {
                    engine.schema.validate(change.column);
                    auto it = engine.items.find(change.itemId);
                    if (it == engine.items.end()) {
                        throw RecoveryError("updated item does not exist");
                    }
                    removeItemFromIndexes(engine, it->second.snapshot);
                    setColumnValue(it->second.snapshot, change.column, change.newValue);
                    addItemToIndexes(engine, it->second.snapshot);
                } else if constexpr (std::is_same_v<T, ListInsertedChange>) {
                    insertSnapshot(engine, change.item);
                } else if constexpr (std::is_same_v<T, ListRemovedChange>) {
                    eraseSnapshot(engine, change.item);
                } else if constexpr (std::is_same_v<T, ListRotatedChange>) {
                    engine.schema.validate(change.list);
                    auto key = valueKey(change.associationValue);
                    auto &group = engine.listGroups[{change.list.containerId(), key}];
                    if (change.rotation.count > 0 &&
                        change.rotation.startIndex + change.rotation.count <= group.size()) {
                        auto first = group.begin() + static_cast<std::ptrdiff_t>(change.rotation.startIndex);
                        auto last = first + static_cast<std::ptrdiff_t>(change.rotation.count);
                        auto shift = change.rotation.offset % static_cast<std::ptrdiff_t>(change.rotation.count);
                        if (shift < 0) {
                            shift += static_cast<std::ptrdiff_t>(change.rotation.count);
                        }
                        std::rotate(first, first + shift, last);
                        refreshListIndexes(engine, change.list.containerId(), key);
                    }
                }
            },
            operation.payload());
    }

    void applyChangeSet(DocumentEngine::Impl &engine, const ChangeSet &changeSet)
    {
        std::vector<ChangeOperation> pendingInserts;
        for (const auto &operation : changeSet.operations()) {
            try {
                applyOperation(engine, operation);
            } catch (const DiniError &) {
                const auto kind = operation.kind();
                if (kind != ChangeOperationKind::ItemInserted && kind != ChangeOperationKind::ListInserted) {
                    throw;
                }
                pendingInserts.push_back(operation);
            }
        }

        while (!pendingInserts.empty()) {
            bool progressed = false;
            for (auto it = pendingInserts.begin(); it != pendingInserts.end();) {
                try {
                    applyOperation(engine, *it);
                    it = pendingInserts.erase(it);
                    progressed = true;
                } catch (const DiniError &) {
                    ++it;
                }
            }
            if (!progressed) {
                applyOperation(engine, pendingInserts.front());
            }
        }
    }

    bool createsUndoStep(const ChangeSet &changeSet)
    {
        if (changeSet.empty()) {
            return false;
        }

        bool hasInsert = false;
        bool hasOnlyInsertInitialization = true;
        for (const auto &operation : changeSet.operations()) {
            switch (operation.kind()) {
                case ChangeOperationKind::ItemInserted:
                case ChangeOperationKind::ListInserted:
                    hasInsert = true;
                    break;
                case ChangeOperationKind::ComputedColumnUpdated:
                    break;
                default:
                    hasOnlyInsertInitialization = false;
                    break;
            }
        }

        if (hasInsert && hasOnlyInsertInitialization) {
            return changeSet.operations().size() == 1;
        }
        return true;
    }

    Value fieldValue(const SchemaDefinitionData &schemaDefinition,
                     const ItemSnapshot &item,
                     const FieldRef &field,
                     bool *fieldApplies = nullptr)
    {
        if (fieldApplies) {
            *fieldApplies = true;
        }
        switch (field.kind()) {
            case FieldKind::Id:
                return idValue(item.id);
            case FieldKind::Parent:
                return valueForColumn(item, field.relation().column().columnId());
            case FieldKind::Variant:
                return item.variant ? Value(static_cast<std::uint64_t>(item.variant->variantId())) : Value::null();
            case FieldKind::Column: {
                const auto &column = columnFor(schemaDefinition, field.column());
                if (column.info.variantSpecific &&
                    (!item.variant || item.variant->variantId() != column.variantId)) {
                    if (fieldApplies) {
                        *fieldApplies = false;
                    }
                    return Value::null();
                }
                return valueForColumn(item, field.column().columnId());
            }
        }
        return Value::null();
    }

    bool matchesExpression(const SchemaDefinitionData &schemaDefinition,
                           const ItemSnapshot &item,
                           const FilterExpression &expression)
    {
        const auto &impl = filterExpressionImpl(expression);
        if (impl.empty) {
            return true;
        }
        if (impl.filter) {
            bool fieldApplies = true;
            const auto leftValue = fieldValue(schemaDefinition, item, impl.filter->field(), &fieldApplies);
            return fieldApplies &&
                   comparePredicate(leftValue,
                                    impl.filter->comparisonOperator(),
                                    impl.filter->value());
        }
        if (impl.op == FilterOperator::And) {
            return std::all_of(impl.children.begin(), impl.children.end(), [&](const auto &child) {
                return matchesExpression(schemaDefinition, item, child);
            });
        }
        if (impl.op == FilterOperator::Or) {
            return std::any_of(impl.children.begin(), impl.children.end(), [&](const auto &child) {
                return matchesExpression(schemaDefinition, item, child);
            });
        }
        if (impl.op == FilterOperator::Not) {
            return impl.children.empty() || !matchesExpression(schemaDefinition, item, impl.children.front());
        }
        return true;
    }

    void validateQueryableField(const EngineSchema &schema, ContainerId containerId, const FieldRef &field)
    {
        const auto &schemaDefinition = schemaData(schema);
        switch (field.kind()) {
            case FieldKind::Id:
                return;
            case FieldKind::Parent: {
                const auto relation = field.relation();
                schema.validate(relation);
                if (relation.containerId() != containerId) {
                    throw QueryError("relation field does not belong to queried container");
                }
                return;
            }
            case FieldKind::Variant: {
                const auto variant = field.variant();
                const auto &container = containerFor(schemaDefinition, containerId);
                const auto found = std::any_of(container.variants.begin(),
                                               container.variants.end(),
                                               [&](const auto &record) {
                                                   return variant.isValid() &&
                                                          variant.schemaId() == schemaDefinition.schemaId &&
                                                          variant.containerId() == containerId &&
                                                          variant.variantId() == record.id;
                                               });
                if (!found) {
                    throw QueryError("variant field does not belong to queried container");
                }
                return;
            }
            case FieldKind::Column: {
                const auto column = field.column();
                schema.validate(column);
                if (column.containerId() != containerId) {
                    throw QueryError("column field does not belong to queried container");
                }
                const auto &record = columnFor(schemaDefinition, column);
                if (record.info.index == IndexKind::None) {
                    throw QueryError("column field is not indexed");
                }
                return;
            }
        }
    }

    ValueType queryableFieldType(const EngineSchema &schema, ContainerId containerId, const FieldRef &field)
    {
        const auto &schemaDefinition = schemaData(schema);
        switch (field.kind()) {
            case FieldKind::Id:
            case FieldKind::Parent:
            case FieldKind::Variant:
                return ValueType::UInt64;
            case FieldKind::Column: {
                const auto column = field.column();
                if (column.containerId() != containerId) {
                    throw QueryError("column field does not belong to queried container");
                }
                return columnFor(schemaDefinition, column).info.type;
            }
        }
        return ValueType::Null;
    }

    bool isEqualityOperator(ComparisonOperator op) noexcept
    {
        return op == ComparisonOperator::Equal || op == ComparisonOperator::NotEqual;
    }

    void validateFilterOperatorForField(const EngineSchema &schema, ContainerId containerId, const Filter &filter)
    {
        const auto op = filter.comparisonOperator();
        if (isEqualityOperator(op)) {
            return;
        }
        const auto fieldType = queryableFieldType(schema, containerId, filter.field());
        if (!runtimeValueSupportsOrdering(fieldType) || !runtimeValueSupportsOrdering(filter.value().type())) {
            throw QueryError("field only supports equality comparison");
        }
    }

    void validateFilterExpression(const EngineSchema &schema, ContainerId containerId, const FilterExpression &expression)
    {
        const auto &impl = filterExpressionImpl(expression);
        if (impl.empty) {
            return;
        }
        if (impl.filter) {
            validateQueryableField(schema, containerId, impl.filter->field());
            validateFilterOperatorForField(schema, containerId, *impl.filter);
            return;
        }
        for (const auto &child : impl.children) {
            validateFilterExpression(schema, containerId, child);
        }
    }

    void validateQuerySpec(const EngineSchema &schema, ContainerId containerId, const QuerySpec &spec)
    {
        validateFilterExpression(schema, containerId, spec.filter);
        for (const auto &sortKey : spec.sortKeys) {
            validateQueryableField(schema, containerId, sortKey.field);
        }
    }

    ComparisonOperator reverseComparisonOperator(ComparisonOperator op) noexcept
    {
        switch (op) {
            case ComparisonOperator::Equal:
                return ComparisonOperator::NotEqual;
            case ComparisonOperator::NotEqual:
                return ComparisonOperator::Equal;
            case ComparisonOperator::Greater:
                return ComparisonOperator::LessOrEqual;
            case ComparisonOperator::Less:
                return ComparisonOperator::GreaterOrEqual;
            case ComparisonOperator::GreaterOrEqual:
                return ComparisonOperator::Less;
            case ComparisonOperator::LessOrEqual:
                return ComparisonOperator::Greater;
        }
        return ComparisonOperator::NotEqual;
    }

    bool fieldAlwaysApplies(const SchemaDefinitionData &schemaDefinition,
                            ContainerId containerId,
                            const FieldRef &field)
    {
        if (field.kind() != FieldKind::Column) {
            return true;
        }
        const auto column = field.column();
        if (column.containerId() != containerId) {
            return false;
        }
        return !columnFor(schemaDefinition, column).info.variantSpecific;
    }

    FilterExpression normalizeNegation(const SchemaDefinitionData &schemaDefinition,
                                       ContainerId containerId,
                                       const FilterExpression &expression,
                                       bool negated = false)
    {
        const auto &impl = filterExpressionImpl(expression);
        if (impl.empty) {
            return negated ? FilterExpression::negate(FilterExpression {}) : expression;
        }
        if (impl.filter) {
            if (!negated) {
                return expression;
            }
            if (!fieldAlwaysApplies(schemaDefinition, containerId, impl.filter->field())) {
                return FilterExpression::negate(expression);
            }
            return FilterExpression(Filter(impl.filter->field(),
                                           reverseComparisonOperator(impl.filter->comparisonOperator()),
                                           impl.filter->value()));
        }
        if (impl.op == FilterOperator::Not) {
            if (impl.children.empty()) {
                return negated ? FilterExpression {} : expression;
            }
            return normalizeNegation(schemaDefinition, containerId, impl.children.front(), !negated);
        }

        std::vector<FilterExpression> children;
        children.reserve(impl.children.size());
        for (const auto &child : impl.children) {
            children.push_back(normalizeNegation(schemaDefinition, containerId, child, negated));
        }
        const auto op = impl.op.value_or(FilterOperator::And);
        if ((!negated && op == FilterOperator::And) || (negated && op == FilterOperator::Or)) {
            return FilterExpression::all(std::move(children));
        }
        return FilterExpression::any(std::move(children));
    }

    QuerySpec normalizedQuerySpec(const EngineSchema &schema, ContainerId containerId, const QuerySpec &spec)
    {
        return QuerySpec {
            .filter = normalizeNegation(schemaData(schema), containerId, spec.filter),
            .sortKeys = spec.sortKeys,
        };
    }

    std::optional<RuntimeIndexedFieldKey> indexedFieldKeyForFilter(ContainerId containerId, const Filter &filter)
    {
        if (filter.field().kind() == FieldKind::Id) {
            return runtimeIdField(containerId);
        }
        if (filter.field().kind() == FieldKind::Variant) {
            return runtimeVariantField(containerId);
        }
        std::optional<ColumnHandle> column;
        if (filter.field().kind() == FieldKind::Column) {
            column = filter.field().column();
        } else if (filter.field().kind() == FieldKind::Parent) {
            column = filter.field().relation().column();
        }
        if (!column || column->containerId() != containerId) {
            return std::nullopt;
        }
        return runtimeColumnField(containerId, column->columnId());
    }

    std::optional<RuntimeIndexedFieldKey> indexedFieldKeyForSort(ContainerId containerId, const FieldRef &field)
    {
        if (field.kind() == FieldKind::Id) {
            return runtimeIdField(containerId);
        }
        if (field.kind() == FieldKind::Variant) {
            return runtimeVariantField(containerId);
        }
        if (field.kind() == FieldKind::Column) {
            const auto column = field.column();
            return column.containerId() == containerId
                ? std::optional<RuntimeIndexedFieldKey>(runtimeColumnField(containerId, column.columnId()))
                : std::nullopt;
        }
        if (field.kind() == FieldKind::Parent) {
            const auto column = field.relation().column();
            return column.containerId() == containerId
                ? std::optional<RuntimeIndexedFieldKey>(runtimeColumnField(containerId, column.columnId()))
                : std::nullopt;
        }
        return std::nullopt;
    }

    struct ParentSelection {
        ColumnId relationColumnId = 0;
        std::vector<Value> values;
    };

    void appendUniqueValue(std::vector<Value> &values, const Value &value)
    {
        const auto exists = std::any_of(values.begin(), values.end(), [&](const auto &candidate) {
            return compareValues(candidate, value) == 0;
        });
        if (!exists) {
            values.push_back(value);
        }
    }

    std::optional<ParentSelection> parentSelectionForFilter(ContainerId containerId, const Filter &filter)
    {
        if (filter.field().kind() != FieldKind::Parent ||
            filter.comparisonOperator() != ComparisonOperator::Equal) {
            return std::nullopt;
        }
        const auto relationColumn = filter.field().relation().column();
        if (relationColumn.containerId() != containerId) {
            return std::nullopt;
        }
        ParentSelection selection;
        selection.relationColumnId = relationColumn.columnId();
        selection.values.push_back(filter.value());
        return selection;
    }

    std::optional<ParentSelection> parentSelectionForExpression(ContainerId containerId,
                                                                const FilterExpression &expression)
    {
        const auto &impl = filterExpressionImpl(expression);
        if (impl.empty) {
            return std::nullopt;
        }
        if (impl.filter) {
            return parentSelectionForFilter(containerId, *impl.filter);
        }
        if (impl.op != FilterOperator::Or || impl.children.empty()) {
            return std::nullopt;
        }

        std::optional<ParentSelection> accumulated;
        for (const auto &child : impl.children) {
            auto childSelection = parentSelectionForExpression(containerId, child);
            if (!childSelection) {
                return std::nullopt;
            }
            if (!accumulated) {
                accumulated = std::move(childSelection);
                continue;
            }
            if (accumulated->relationColumnId != childSelection->relationColumnId) {
                return std::nullopt;
            }
            for (const auto &value : childSelection->values) {
                appendUniqueValue(accumulated->values, value);
            }
        }
        return accumulated;
    }

    std::size_t countWithOffsetLimit(std::size_t total, std::size_t offset, std::optional<std::size_t> limit)
    {
        if (offset >= total) {
            return 0;
        }
        const auto remaining = total - offset;
        return limit ? std::min(*limit, remaining) : remaining;
    }

    std::optional<RuntimeRangeConstraint> rangeConstraintForFilter(const EngineSchema &schema,
                                                                  ContainerId containerId,
                                                                  const Filter &filter)
    {
        if (!runtimeValueSupportsOrdering(queryableFieldType(schema, containerId, filter.field())) ||
            !runtimeValueSupportsOrdering(filter.value().type())) {
            return std::nullopt;
        }
        auto field = indexedFieldKeyForFilter(containerId, filter);
        if (!field) {
            return std::nullopt;
        }
        return RuntimeRangeConstraint {*field, filter.comparisonOperator(), filter.value()};
    }

    bool collectPureAndRangeConstraints(const EngineSchema &schema,
                                        ContainerId containerId,
                                        const FilterExpression &expression,
                                        std::vector<RuntimeRangeConstraint> &constraints)
    {
        const auto &impl = filterExpressionImpl(expression);
        if (impl.empty) {
            return true;
        }
        if (impl.filter) {
            auto constraint = rangeConstraintForFilter(schema, containerId, *impl.filter);
            if (!constraint) {
                return false;
            }
            constraints.push_back(std::move(*constraint));
            return true;
        }
        if (impl.op != FilterOperator::And) {
            return false;
        }
        for (const auto &child : impl.children) {
            if (!collectPureAndRangeConstraints(schema, containerId, child, constraints)) {
                return false;
            }
        }
        return true;
    }

    bool hasDeclaredRangeIndexFor(const SchemaDefinitionData &schemaDefinition,
                                  ContainerId containerId,
                                  const std::vector<RuntimeRangeConstraint> &constraints)
    {
        const auto *container = findContainer(schemaDefinition, containerId);
        if (!container) {
            return false;
        }
        for (const auto &rangeIndex : container->rangeIndexes) {
            bool coversAll = true;
            for (const auto &constraint : constraints) {
                if (constraint.field.kind != RuntimeIndexedFieldKind::Column ||
                    constraint.field.containerId != containerId) {
                    coversAll = false;
                    break;
                }
                const auto found = std::any_of(rangeIndex.columns.begin(),
                                               rangeIndex.columns.end(),
                                               [&](const auto &column) {
                                                   return column.columnId() == constraint.field.id;
                                               });
                if (!found) {
                    coversAll = false;
                    break;
                }
            }
            if (coversAll) {
                return true;
            }
        }
        return false;
    }

    std::optional<std::set<ItemId>> indexedCandidatesForFilter(const DocumentEngine::Impl &engine,
                                                               ContainerId containerId,
                                                               const Filter &filter)
    {
        const auto field = indexedFieldKeyForFilter(containerId, filter);
        if (!field) {
            return std::nullopt;
        }
        return engine.indexes.queryField(*field, filter.comparisonOperator(), filter.value());
    }

    std::optional<std::set<ItemId>> indexedCandidatesForExpression(const DocumentEngine::Impl &engine,
                                                                   ContainerId containerId,
                                                                   const FilterExpression &expression)
    {
        const auto &impl = filterExpressionImpl(expression);
        if (impl.empty) {
            return std::nullopt;
        }
        if (impl.filter) {
            return indexedCandidatesForFilter(engine, containerId, *impl.filter);
        }
        if (impl.op == FilterOperator::Not) {
            if (impl.children.empty()) {
                return std::set<ItemId> {};
            }
            auto childCandidates = indexedCandidatesForExpression(engine, containerId, impl.children.front());
            if (!childCandidates) {
                return std::set<ItemId> {};
            }
            std::set<ItemId> result;
            const auto &universe = engine.indexes.containerItems(containerId);
            std::set_difference(universe.begin(),
                                universe.end(),
                                childCandidates->begin(),
                                childCandidates->end(),
                                std::inserter(result, result.end()));
            return result;
        }
        if (impl.op == FilterOperator::And) {
            std::vector<RuntimeRangeConstraint> constraints;
            if (collectPureAndRangeConstraints(engine.schema, containerId, expression, constraints) &&
                constraints.size() > 1 &&
                hasDeclaredRangeIndexFor(schemaData(engine.schema), containerId, constraints)) {
                return engine.indexes.queryRange(containerId, constraints);
            }
        }
        std::optional<std::set<ItemId>> accumulated;
        for (const auto &child : impl.children) {
            const auto childCandidates = indexedCandidatesForExpression(engine, containerId, child);
            if (!childCandidates) {
                if (impl.op == FilterOperator::And) {
                    continue;
                }
                return std::nullopt;
            }
            if (!accumulated) {
                accumulated = *childCandidates;
                continue;
            }
            std::set<ItemId> merged;
            if (impl.op == FilterOperator::And) {
                std::set_intersection(accumulated->begin(),
                                      accumulated->end(),
                                      childCandidates->begin(),
                                      childCandidates->end(),
                                      std::inserter(merged, merged.end()));
            } else {
                std::set_union(accumulated->begin(),
                               accumulated->end(),
                               childCandidates->begin(),
                               childCandidates->end(),
                               std::inserter(merged, merged.end()));
            }
            accumulated = std::move(merged);
        }
        return accumulated;
    }

    void applyQuerySpec(const SchemaDefinitionData &schemaDefinition, std::vector<ItemSnapshot> &items, const QuerySpec &spec)
    {
        items.erase(std::remove_if(items.begin(),
                                   items.end(),
                                   [&](const auto &item) {
                                       return !matchesExpression(schemaDefinition, item, spec.filter);
                                   }),
                    items.end());
        for (auto sortIt = spec.sortKeys.rbegin(); sortIt != spec.sortKeys.rend(); ++sortIt) {
            std::stable_sort(items.begin(), items.end(), [&](const auto &lhs, const auto &rhs) {
                const auto comparison = compareValues(fieldValue(schemaDefinition, lhs, sortIt->field),
                                                      fieldValue(schemaDefinition, rhs, sortIt->field));
                return sortIt->direction == SortDirection::Ascending ? comparison < 0 : comparison > 0;
            });
        }
    }

    void executeQueryResults(const DocumentEngine::Impl &engine,
                             ContainerId containerId,
                             const QuerySpec &spec,
                             bool useListDefaultOrder,
                             const std::function<bool(const ItemSnapshot &)> &visitor,
                             std::size_t offset = 0,
                             std::optional<std::size_t> limit = std::nullopt)
    {
        const auto candidates = indexedCandidatesForExpression(engine, containerId, spec.filter);
        std::size_t skipped = 0;
        std::size_t emitted = 0;
        auto emit = [&](const ItemSnapshot &snapshot) {
            if (!matchesExpression(schemaData(engine.schema), snapshot, spec.filter)) {
                return true;
            }
            if (skipped < offset) {
                ++skipped;
                return true;
            }
            if (limit && emitted >= *limit) {
                return false;
            }
            ++emitted;
            if (!visitor(snapshot)) {
                return false;
            }
            return !limit || emitted < *limit;
        };
        auto emitId = [&](ItemId id) {
            auto itemIt = engine.items.find(id);
            if (itemIt == engine.items.end() || itemIt->second.snapshot.containerId != containerId) {
                return true;
            }
            return emit(itemIt->second.snapshot);
        };

        const bool canStreamInIndexOrder = !useListDefaultOrder &&
            (spec.sortKeys.empty() || spec.sortKeys.size() == 1);
        if (canStreamInIndexOrder) {
            if (spec.sortKeys.size() == 1) {
                if (const auto sortField = indexedFieldKeyForSort(containerId, spec.sortKeys.front().field)) {
                    const auto descending = spec.sortKeys.front().direction == SortDirection::Descending;
                    bool keepGoing = true;
                    engine.indexes.orderedField(*sortField, descending, [&](ItemId id) {
                        if (candidates && candidates->find(id) == candidates->end()) {
                            return true;
                        }
                        keepGoing = emitId(id);
                        return keepGoing;
                    });
                    return;
                }
            } else {
                const auto &ids = candidates ? *candidates : engine.indexes.containerItems(containerId);
                for (const auto id : ids) {
                    if (!emitId(id)) {
                        return;
                    }
                }
                return;
            }
        }

        std::vector<ItemSnapshot> materialized;
        const auto &ids = candidates ? *candidates : engine.indexes.containerItems(containerId);
        materialized.reserve(ids.size());
        for (const auto id : ids) {
            auto itemIt = engine.items.find(id);
            if (itemIt != engine.items.end() && itemIt->second.snapshot.containerId == containerId) {
                materialized.push_back(itemIt->second.snapshot);
            }
        }
        if (useListDefaultOrder && spec.sortKeys.empty()) {
            std::stable_sort(materialized.begin(), materialized.end(), [](const auto &lhs, const auto &rhs) {
                return lhs.listIndex.value_or(0) < rhs.listIndex.value_or(0);
            });
        }
        applyQuerySpec(schemaData(engine.schema), materialized, spec);
        for (const auto &item : materialized) {
            if (skipped < offset) {
                ++skipped;
                continue;
            }
            if (limit && emitted >= *limit) {
                return;
            }
            ++emitted;
            if (!visitor(item)) {
                return;
            }
        }
    }

    void writeSnapshotMagic(BinaryWriter &writer)
    {
        for (auto byte : snapshotMagic) {
            writer.writeByte(byte);
        }
    }

    void readSnapshotMagic(BinaryReader &reader)
    {
        for (auto expected : snapshotMagic) {
            if (reader.readByte() != expected) {
                throw RecoveryError("invalid snapshot magic");
            }
        }
    }

    void writeCommitLogEnvelopeMagic(BinaryWriter &writer)
    {
        for (auto byte : commitLogEnvelopeMagic) {
            writer.writeByte(byte);
        }
    }

    bool hasCommitLogEnvelopeMagic(const ByteArray &bytes)
    {
        if (bytes.size() < sizeof(commitLogEnvelopeMagic)) {
            return false;
        }
        for (std::size_t i = 0; i < sizeof(commitLogEnvelopeMagic); ++i) {
            if (bytes[i] != commitLogEnvelopeMagic[i]) {
                return false;
            }
        }
        return true;
    }

    void readCommitLogEnvelopeMagic(BinaryReader &reader)
    {
        for (auto expected : commitLogEnvelopeMagic) {
            if (reader.readByte() != expected) {
                throw RecoveryError("invalid commit log envelope magic");
            }
        }
    }

    ByteArray serializeEngineCommitLog(const EngineSchema &schema, const ChangeSet &changeSet)
    {
        BinaryWriter writer;
        writeCommitLogEnvelopeMagic(writer);
        writer.writeUInt32(commitLogEnvelopeVersion);
        writer.writeUInt64(schema.schemaId());
        writer.writeUInt64(schemaFingerprint(schemaData(schema)));
        writer.writeBytes(changeSet.serializeForLog());
        return std::move(writer.bytes);
    }

    ChangeSet deserializeEngineCommitLog(const EngineSchema &schema, const ByteArray &bytes)
    {
        if (!hasCommitLogEnvelopeMagic(bytes)) {
            return deserializeChangeSetFromLog(bytes);
        }
        BinaryReader reader(bytes);
        readCommitLogEnvelopeMagic(reader);
        const auto version = reader.readUInt32();
        if (version != commitLogEnvelopeVersion) {
            throw RecoveryError("unsupported commit log envelope version");
        }
        const auto schemaId = reader.readUInt64();
        const auto fingerprint = reader.readUInt64();
        if (schemaId != schema.schemaId() || fingerprint != schemaFingerprint(schemaData(schema))) {
            throw RecoveryError("commit log schema does not match engine schema");
        }
        const auto innerLog = reader.readBytes();
        if (!reader.atEnd()) {
            throw RecoveryError("trailing bytes after commit log envelope");
        }
        return deserializeChangeSetFromLog(innerLog);
    }

    } // namespace

DocumentEngine::DocumentEngine(EngineSchema schema) : _impl(std::make_unique<Impl>(std::move(schema)))
{
    if (!_impl->schema.isValid()) {
        throw SchemaError("document engine requires a valid schema");
    }
    _impl->epochSeconds = unixSecondsNow();
}

DocumentEngine::~DocumentEngine() = default;
DocumentEngine::DocumentEngine(DocumentEngine &&other) noexcept = default;
DocumentEngine &DocumentEngine::operator=(DocumentEngine &&other) noexcept = default;

EngineSchema DocumentEngine::schema() const
{
    if (!_impl || !_impl->valid) {
        throw SchemaError("document engine is invalid");
    }
    return _impl->schema;
}

Transaction DocumentEngine::beginTransaction(TransactionOptions options)
{
    if (!_impl || _impl->activeTransaction) {
        throw TransactionError("another transaction is already active");
    }
    auto data = std::make_unique<Transaction::Impl>();
    data->owner = this;
    data->engine = _impl.get();
    data->options = options;
    data->state = TransactionState::Active;
    data->rollbackItems = _impl->items;
    data->rollbackListGroups = _impl->listGroups;
    data->rollbackIndexes = _impl->indexes;
    _impl->activeTransaction = true;
    return Transaction(std::move(data));
}

ItemSnapshot DocumentEngine::read(ItemId itemId) const
{
    auto it = _impl->items.find(itemId);
    if (it == _impl->items.end()) {
        throw QueryError("item does not exist");
    }
    return it->second.snapshot;
}

Value DocumentEngine::read(ItemId itemId, ColumnHandle column) const
{
    _impl->schema.validate(column);
    const auto snapshot = read(itemId);
    if (snapshot.containerId != column.containerId()) {
        throw QueryError("column does not belong to item container");
    }
    const auto &columnRecord = columnFor(schemaData(_impl->schema), column);
    if (columnRecord.info.variantSpecific &&
        (!snapshot.variant || snapshot.variant->variantId() != columnRecord.variantId)) {
        throw QueryError("variant-specific column is not valid for this item");
    }
    return valueForColumn(snapshot, column.columnId());
}

bool DocumentEngine::contains(ItemId itemId) const
{
    return _impl && _impl->items.find(itemId) != _impl->items.end();
}

std::size_t DocumentEngine::listLength(ListHandle list, const Value &associationValue) const
{
    _impl->schema.validate(list);
    auto it = _impl->listGroups.find({list.containerId(), valueKey(associationValue)});
    return it == _impl->listGroups.end() ? 0 : it->second.size();
}

View DocumentEngine::view(TableHandle table) const
{
    return query(table, QuerySpec {});
}

View DocumentEngine::view(ListHandle list) const
{
    return query(list, QuerySpec {});
}

View DocumentEngine::query(TableHandle table, const QuerySpec &spec) const
{
    _impl->schema.validate(table);
    validateQuerySpec(_impl->schema, table.containerId(), spec);
    auto *engine = _impl.get();
    const auto normalizedSpec = normalizedQuerySpec(_impl->schema, table.containerId(), spec);
    const auto parentSelection = parentSelectionForExpression(table.containerId(), normalizedSpec.filter);
    auto data = SharedDataPointer<View::Impl>(new View::Impl(nullptr, [engine, table, normalizedSpec]() {
        std::vector<ItemSnapshot> result;
        executeQueryResults(*engine, table.containerId(), normalizedSpec, false, [&](const ItemSnapshot &item) {
            result.push_back(item);
            return true;
        });
        return result;
    }, _impl->schema, table.containerId()));
    data.data()->streamer = [engine, table, normalizedSpec](const std::function<bool(const ItemSnapshot &)> &visitor,
                                                            std::size_t offset,
                                                            std::optional<std::size_t> limit) {
        executeQueryResults(*engine, table.containerId(), normalizedSpec, false, visitor, offset, limit);
    };
    if (normalizedSpec.filter.isEmpty()) {
        data.data()->aggregator = [engine, table](const AggregationSpec &aggregate,
                                                  std::size_t offset,
                                                  std::optional<std::size_t> limit)
            -> std::optional<std::vector<AggregationResult>> {
            if (offset != 0 || limit) {
                return std::nullopt;
            }
            return engine->indexes.aggregate(schemaData(engine->schema), table.containerId(), aggregate);
        };
    } else if (parentSelection) {
        data.data()->counter = [engine, table, selection = *parentSelection](
                                   std::size_t offset,
                                   std::optional<std::size_t> limit) -> std::optional<std::size_t> {
            const auto total = engine->indexes.countParent(table.containerId(),
                                                           selection.relationColumnId,
                                                           selection.values);
            return countWithOffsetLimit(total, offset, limit);
        };
        data.data()->aggregator = [engine, table, selection = *parentSelection](
                                      const AggregationSpec &aggregate,
                                      std::size_t offset,
                                      std::optional<std::size_t> limit)
            -> std::optional<std::vector<AggregationResult>> {
            if (offset != 0 || limit) {
                return std::nullopt;
            }
            return engine->indexes.aggregateParentSelection(schemaData(engine->schema),
                                                            table.containerId(),
                                                            selection.relationColumnId,
                                                            selection.values,
                                                            aggregate);
        };
    }
    return View(std::move(data));
}

View DocumentEngine::query(ListHandle list, const QuerySpec &spec) const
{
    _impl->schema.validate(list);
    validateQuerySpec(_impl->schema, list.containerId(), spec);
    auto *engine = _impl.get();
    const auto normalizedSpec = normalizedQuerySpec(_impl->schema, list.containerId(), spec);
    const auto parentSelection = parentSelectionForExpression(list.containerId(), normalizedSpec.filter);
    auto data = SharedDataPointer<View::Impl>(new View::Impl(nullptr, [engine, list, normalizedSpec]() {
        std::vector<ItemSnapshot> result;
        executeQueryResults(*engine, list.containerId(), normalizedSpec, true, [&](const ItemSnapshot &item) {
            result.push_back(item);
            return true;
        });
        return result;
    }, _impl->schema, list.containerId()));
    data.data()->streamer = [engine, list, normalizedSpec](const std::function<bool(const ItemSnapshot &)> &visitor,
                                                           std::size_t offset,
                                                           std::optional<std::size_t> limit) {
        executeQueryResults(*engine, list.containerId(), normalizedSpec, true, visitor, offset, limit);
    };
    if (normalizedSpec.filter.isEmpty()) {
        data.data()->aggregator = [engine, list](const AggregationSpec &aggregate,
                                                 std::size_t offset,
                                                 std::optional<std::size_t> limit)
            -> std::optional<std::vector<AggregationResult>> {
            if (offset != 0 || limit) {
                return std::nullopt;
            }
            return engine->indexes.aggregate(schemaData(engine->schema), list.containerId(), aggregate);
        };
    } else if (parentSelection) {
        data.data()->counter = [engine, list, selection = *parentSelection](
                                   std::size_t offset,
                                   std::optional<std::size_t> limit) -> std::optional<std::size_t> {
            const auto total = engine->indexes.countParent(list.containerId(),
                                                           selection.relationColumnId,
                                                           selection.values);
            return countWithOffsetLimit(total, offset, limit);
        };
        data.data()->aggregator = [engine, list, selection = *parentSelection](
                                      const AggregationSpec &aggregate,
                                      std::size_t offset,
                                      std::optional<std::size_t> limit)
            -> std::optional<std::vector<AggregationResult>> {
            if (offset != 0 || limit) {
                return std::nullopt;
            }
            return engine->indexes.aggregateParentSelection(schemaData(engine->schema),
                                                            list.containerId(),
                                                            selection.relationColumnId,
                                                            selection.values,
                                                            aggregate);
        };
    }
    return View(std::move(data));
}

ByteArray DocumentEngine::createSnapshot() const
{
    BinaryWriter writer;
    writeSnapshotMagic(writer);
    writer.writeUInt32(snapshotFormatVersion);
    writer.writeUInt64(_impl->schema.schemaId());
    writer.writeUInt64(schemaFingerprint(schemaData(_impl->schema)));
    writer.writeUInt64(_impl->epochSeconds);
    writer.writeUInt32(_impl->currentElapsedSecond);
    writer.writeUInt32(_impl->currentSecondCounter);
    std::vector<ItemSnapshot> snapshots;
    snapshots.reserve(_impl->items.size());
    for (const auto &[id, item] : _impl->items) {
        (void)id;
        snapshots.push_back(item.snapshot);
    }
    writer.writeSize(snapshots.size());
    for (const auto &snapshot : snapshots) {
        writeItemSnapshot(writer, snapshot);
    }
    return std::move(writer.bytes);
}

void DocumentEngine::restoreSnapshot(const ByteArray &snapshot)
{
    try {
        BinaryReader reader(snapshot);
        readSnapshotMagic(reader);
        const auto version = reader.readUInt32();
        if (version != snapshotFormatVersion) {
            throw RecoveryError("unsupported snapshot format version");
        }
        const auto schemaId = reader.readUInt64();
        const auto fingerprint = reader.readUInt64();
        if (schemaId != _impl->schema.schemaId() ||
            fingerprint != schemaFingerprint(schemaData(_impl->schema))) {
            throw RecoveryError("snapshot schema does not match engine schema");
        }
        _impl->epochSeconds = reader.readUInt64();
        _impl->currentElapsedSecond = reader.readUInt32();
        _impl->currentSecondCounter = reader.readUInt32();
        _impl->items.clear();
        _impl->listGroups.clear();
        _impl->indexes.clear();
        const auto itemCount = reader.readSize();
        std::vector<ItemSnapshot> snapshots;
        snapshots.reserve(itemCount);
        for (std::size_t i = 0; i < itemCount; ++i) {
            snapshots.push_back(readItemSnapshot(reader));
        }
        if (!reader.atEnd()) {
            throw RecoveryError("trailing bytes after snapshot");
        }
        while (!snapshots.empty()) {
            bool progressed = false;
            for (auto it = snapshots.begin(); it != snapshots.end();) {
                const bool parentReady = !it->parentId || contains(*it->parentId);
                bool listReady = true;
                if (it->containerKind == ContainerKind::List && it->listAssociationValue && it->listIndex) {
                    const auto key = valueKey(*it->listAssociationValue);
                    const auto groupIt = _impl->listGroups.find({it->containerId, key});
                    const auto currentSize = groupIt == _impl->listGroups.end() ? 0 : groupIt->second.size();
                    listReady = *it->listIndex <= currentSize;
                }
                if (parentReady && listReady) {
                    insertSnapshot(*_impl, *it);
                    it = snapshots.erase(it);
                    progressed = true;
                } else {
                    ++it;
                }
            }
            if (!progressed) {
                throw RecoveryError("snapshot contains unresolved parent or list ordering dependencies");
            }
        }
        rebuildIndexes(*_impl);
        clearUndoHistory();
    } catch (const DiniError &) {
        throw;
    } catch (const std::exception &error) {
        throw RecoveryError(error.what());
    }
}

void DocumentEngine::replayCommitLog(const ByteArray &commitLog)
{
    if (commitLog.empty()) {
        return;
    }
    try {
        auto changeSet = deserializeEngineCommitLog(_impl->schema, commitLog);
        applyChangeSet(*_impl, changeSet);
    } catch (const DiniError &) {
        throw;
    } catch (const std::exception &error) {
        throw RecoveryError(error.what());
    }
}

bool DocumentEngine::canUndo() const noexcept
{
    return _impl && !_impl->undoStack.empty();
}

bool DocumentEngine::canRedo() const noexcept
{
    return _impl && !_impl->redoStack.empty();
}

CommitResult DocumentEngine::undo()
{
    if (!canUndo() || _impl->activeTransaction) {
        throw TransactionError("no undo step is available");
    }
    const auto step = _impl->undoStack.back();
    auto inverse = step.changeSet().invert();
    _impl->activeTransaction = true;
    try {
        applyChangeSet(*_impl, inverse);
    } catch (...) {
        _impl->activeTransaction = false;
        throw;
    }
    _impl->activeTransaction = false;
    _impl->undoStack.pop_back();
    _impl->redoStack.push_back(step);
    auto data = std::make_unique<TransactionContext::Impl>();
    data->origin = EventOrigin::Undo;
    TransactionContext context(std::move(data));
    runHooks(*_impl, HookStage::AfterApply, context, inverse, false);
    publish(*_impl, EventKind::AfterApply, EventOrigin::Undo, inverse);
    runHooks(*_impl, HookStage::AfterCommit, context, inverse, false);
    publish(*_impl, EventKind::AfterCommit, EventOrigin::Undo, inverse);
    return CommitResult {
        .changeSet = inverse,
        .commitLog = serializeEngineCommitLog(_impl->schema, inverse),
        .origin = EventOrigin::Undo,
    };
}

CommitResult DocumentEngine::redo()
{
    if (!canRedo() || _impl->activeTransaction) {
        throw TransactionError("no redo step is available");
    }
    const auto step = _impl->redoStack.back();
    _impl->activeTransaction = true;
    try {
        applyChangeSet(*_impl, step.changeSet());
    } catch (...) {
        _impl->activeTransaction = false;
        throw;
    }
    _impl->activeTransaction = false;
    _impl->redoStack.pop_back();
    _impl->undoStack.push_back(step);
    auto data = std::make_unique<TransactionContext::Impl>();
    data->origin = EventOrigin::Redo;
    TransactionContext context(std::move(data));
    runHooks(*_impl, HookStage::AfterApply, context, step.changeSet(), false);
    publish(*_impl, EventKind::AfterApply, EventOrigin::Redo, step.changeSet());
    runHooks(*_impl, HookStage::AfterCommit, context, step.changeSet(), false);
    publish(*_impl, EventKind::AfterCommit, EventOrigin::Redo, step.changeSet());
    return CommitResult {
        .changeSet = step.changeSet(),
        .commitLog = serializeEngineCommitLog(_impl->schema, step.changeSet()),
        .origin = EventOrigin::Redo,
    };
}

void DocumentEngine::clearUndoHistory()
{
    _impl->undoStack.clear();
    _impl->redoStack.clear();
}

std::vector<UndoStep> DocumentEngine::undoHistory() const
{
    return _impl->undoStack;
}

std::vector<UndoStep> DocumentEngine::redoHistory() const
{
    return _impl->redoStack;
}

Subscription DocumentEngine::subscribe(EventCallback callback)
{
    auto state = std::make_shared<SubscriptionState>();
    state->callback = std::move(callback);
    _impl->subscriptions.push_back(state);
    auto data = std::make_unique<Subscription::Impl>();
    data->state = std::move(state);
    return Subscription(std::move(data));
}

TransactionContext::TransactionContext() : _impl(std::make_unique<Impl>()) {}
TransactionContext::TransactionContext(std::unique_ptr<Impl> data) : _impl(std::move(data)) {}
TransactionContext::~TransactionContext() = default;
TransactionContext::TransactionContext(TransactionContext &&other) noexcept = default;
TransactionContext &TransactionContext::operator=(TransactionContext &&other) noexcept = default;

EventOrigin TransactionContext::origin() const
{
    if (!_impl) {
        throw TransactionError("transaction context is invalid");
    }
    return _impl->origin;
}

DocumentEngine &TransactionContext::engine()
{
    if (!_impl || !_impl->transaction || !_impl->transaction->owner) {
        throw TransactionError("transaction context is invalid");
    }
    return *_impl->transaction->owner;
}

const DocumentEngine &TransactionContext::engine() const
{
    if (!_impl || !_impl->transaction || !_impl->transaction->owner) {
        throw TransactionError("transaction context is invalid");
    }
    return *_impl->transaction->owner;
}

ItemId TransactionContext::insert(TableHandle table, std::vector<ColumnValue> values, std::optional<VariantHandle> variant)
{
    if (!_impl || !_impl->mutationAllowed) {
        throw HookError("mutation is not allowed in this hook stage");
    }
    Transaction transaction(std::make_unique<Transaction::Impl>(*_impl->transaction));
    try {
        auto id = transaction.insert(std::move(table), std::move(values), std::move(variant));
        *_impl->transaction = *transaction._impl;
        transaction._impl->state = TransactionState::Committed;
        return id;
    } catch (...) {
        markFailed(*_impl->transaction);
        throw;
    }
}

ItemId TransactionContext::insert(ListHandle list,
                                  Value associationValue,
                                  std::size_t index,
                                  std::vector<ColumnValue> values,
                                  std::optional<VariantHandle> variant)
{
    if (!_impl || !_impl->mutationAllowed) {
        throw HookError("mutation is not allowed in this hook stage");
    }
    Transaction transaction(std::make_unique<Transaction::Impl>(*_impl->transaction));
    try {
        auto id = transaction.insert(std::move(list), std::move(associationValue), index, std::move(values), std::move(variant));
        *_impl->transaction = *transaction._impl;
        transaction._impl->state = TransactionState::Committed;
        return id;
    } catch (...) {
        markFailed(*_impl->transaction);
        throw;
    }
}

void TransactionContext::remove(ItemId itemId)
{
    if (!_impl || !_impl->mutationAllowed) {
        throw HookError("mutation is not allowed in this hook stage");
    }
    Transaction transaction(std::make_unique<Transaction::Impl>(*_impl->transaction));
    try {
        transaction.remove(itemId);
        *_impl->transaction = *transaction._impl;
        transaction._impl->state = TransactionState::Committed;
    } catch (...) {
        markFailed(*_impl->transaction);
        throw;
    }
}

void TransactionContext::update(ItemId itemId, ColumnHandle column, Value value, AssociationUpdateOptions options)
{
    if (!_impl || !_impl->mutationAllowed) {
        throw HookError("mutation is not allowed in this hook stage");
    }
    Transaction transaction(std::make_unique<Transaction::Impl>(*_impl->transaction));
    try {
        transaction.update(itemId, std::move(column), std::move(value), options);
        *_impl->transaction = *transaction._impl;
        transaction._impl->state = TransactionState::Committed;
    } catch (...) {
        markFailed(*_impl->transaction);
        throw;
    }
}

void TransactionContext::rotate(ListHandle list, Value associationValue, ListRotation rotation)
{
    if (!_impl || !_impl->mutationAllowed) {
        throw HookError("mutation is not allowed in this hook stage");
    }
    Transaction transaction(std::make_unique<Transaction::Impl>(*_impl->transaction));
    try {
        transaction.rotate(std::move(list), std::move(associationValue), rotation);
        *_impl->transaction = *transaction._impl;
        transaction._impl->state = TransactionState::Committed;
    } catch (...) {
        markFailed(*_impl->transaction);
        throw;
    }
}

Transaction::Transaction() : _impl(std::make_unique<Impl>()) {}
Transaction::Transaction(std::unique_ptr<Impl> data) : _impl(std::move(data)) {}

Transaction::~Transaction()
{
    if (_impl && _impl->state == TransactionState::Active) {
        try {
            rollback();
        } catch (...) {
        }
    }
}

Transaction::Transaction(Transaction &&other) noexcept = default;

Transaction &Transaction::operator=(Transaction &&other) noexcept
{
    if (this != &other) {
        if (_impl && _impl->state == TransactionState::Active) {
            try {
                rollback();
            } catch (...) {
            }
        }
        _impl = std::move(other._impl);
    }
    return *this;
}

TransactionState Transaction::state() const noexcept
{
    return _impl ? _impl->state : TransactionState::Failed;
}

DocumentEngine &Transaction::engine()
{
    if (!_impl || !_impl->owner) {
        throw TransactionError("transaction is invalid");
    }
    return *_impl->owner;
}

const DocumentEngine &Transaction::engine() const
{
    if (!_impl || !_impl->owner) {
        throw TransactionError("transaction is invalid");
    }
    return *_impl->owner;
}

const ChangeSet &Transaction::changeSet() const
{
    if (!_impl) {
        throw TransactionError("transaction is invalid");
    }
    return _impl->changeSet;
}

ItemId Transaction::insert(TableHandle table, std::vector<ColumnValue> values, std::optional<VariantHandle> variant)
{
    requireActive(_impl.get());
    auto &engine = *_impl->engine;
    try {
        engine.schema.validate(table);
        auto makeContext = [&]() {
            auto data = std::make_unique<TransactionContext::Impl>();
            data->transaction = _impl.get();
            data->origin = _impl->origin;
            return TransactionContext(std::move(data));
        };
        const auto &schemaDefinition = schemaData(engine.schema);
        ItemSnapshot snapshot;
        snapshot.id = generateItemId(engine);
        snapshot.containerKind = ContainerKind::Table;
        snapshot.containerId = table.containerId();
        snapshot.variant = std::move(variant);
        snapshot.values = std::move(values);
        validateSnapshot(engine, snapshot);
        auto operationChange = insertChangeSetForSnapshot(schemaDefinition, snapshot);
        auto beforeContext = makeContext();
        beforeContext._impl->mutationAllowed = true;
        const auto derivedStart = _impl->changeSet.operations().size();
        {
            PendingBeforeApplyScope pending(*_impl, snapshot, true, 0);
            runHooks(engine, HookStage::BeforeApply, beforeContext, operationChange, true);
        }
        const auto derivedEnd = _impl->changeSet.operations().size();
        operationChange = insertChangeSetForSnapshot(schemaDefinition, snapshot);
        insertSnapshot(engine, snapshot);
        appendWithDerivedLinks(*_impl, operationChange, derivedStart, derivedEnd);
        auto afterContext = makeContext();
        runHooks(engine, HookStage::AfterApply, afterContext, operationChange, false);
        publish(engine, EventKind::AfterApply, _impl->origin, operationChange);
        return snapshot.id;
    } catch (...) {
        markFailed(*_impl);
        throw;
    }
}

ItemId Transaction::insert(ListHandle list,
                           Value associationValue,
                           std::size_t index,
                           std::vector<ColumnValue> values,
                           std::optional<VariantHandle> variant)
{
    requireActive(_impl.get());
    auto &engine = *_impl->engine;
    try {
        engine.schema.validate(list);
        auto makeContext = [&]() {
            auto data = std::make_unique<TransactionContext::Impl>();
            data->transaction = _impl.get();
            data->origin = _impl->origin;
            return TransactionContext(std::move(data));
        };
        const auto &schemaDefinition = schemaData(engine.schema);
        const auto &container = containerFor(schemaDefinition, list.containerId());
        if (!container.listAssociation) {
            throw ConstraintError("list association is not defined");
        }
        const auto *relation = findRelation(schemaDefinition, list.containerId(), *container.listAssociation);
        if (!associationValue.isNull()) {
            auto groupKey = valueKey(associationValue);
            auto &group = engine.listGroups[{list.containerId(), groupKey}];
            if (index > group.size()) {
                throw ConstraintError("list insertion index is out of range");
            }
        } else if (index != 0) {
            throw ConstraintError("null list association insertion index must be zero");
        }
        ItemSnapshot snapshot;
        snapshot.id = generateItemId(engine);
        snapshot.containerKind = ContainerKind::List;
        snapshot.containerId = list.containerId();
        snapshot.variant = std::move(variant);
        snapshot.values = std::move(values);
        setColumnValue(snapshot, relation->info.column, associationValue);
        if (!associationValue.isNull()) {
            snapshot.listAssociationValue = associationValue;
            snapshot.listIndex = index;
        }
        validateSnapshot(engine, snapshot);
        auto operationChange = associationValue.isNull()
                                   ? insertChangeSetForSnapshot(schemaDefinition, snapshot)
                                   : listInsertChangeSetForSnapshot(schemaDefinition,
                                                                    list,
                                                                    associationValue,
                                                                    index,
                                                                    snapshot);
        auto beforeContext = makeContext();
        beforeContext._impl->mutationAllowed = true;
        const auto derivedStart = _impl->changeSet.operations().size();
        {
            PendingBeforeApplyScope pending(*_impl, snapshot, true, 0);
            runHooks(engine, HookStage::BeforeApply, beforeContext, operationChange, true);
        }
        const auto derivedEnd = _impl->changeSet.operations().size();
        operationChange = associationValue.isNull()
                              ? insertChangeSetForSnapshot(schemaDefinition, snapshot)
                              : listInsertChangeSetForSnapshot(schemaDefinition,
                                                               list,
                                                               associationValue,
                                                               index,
                                                               snapshot);
        insertSnapshot(engine, snapshot);
        appendWithDerivedLinks(*_impl, operationChange, derivedStart, derivedEnd);
        auto afterContext = makeContext();
        runHooks(engine, HookStage::AfterApply, afterContext, operationChange, false);
        publish(engine, EventKind::AfterApply, _impl->origin, operationChange);
        return snapshot.id;
    } catch (...) {
        markFailed(*_impl);
        throw;
    }
}

void Transaction::remove(ItemId itemId)
{
    requireActive(_impl.get());
    auto &engine = *_impl->engine;
    try {
        auto makeContext = [&]() {
            auto data = std::make_unique<TransactionContext::Impl>();
            data->transaction = _impl.get();
            data->origin = _impl->origin;
            return TransactionContext(std::move(data));
        };
        auto it = engine.items.find(itemId);
        if (it == engine.items.end()) {
            throw QueryError("item does not exist");
        }
        std::vector<ItemSnapshot> toRemove;
        std::function<void(ItemId)> collect = [&](ItemId id) {
            auto itemIt = engine.items.find(id);
            if (itemIt == engine.items.end()) {
                return;
            }
            toRemove.push_back(itemIt->second.snapshot);
            for (const auto &[candidateId, candidate] : engine.items) {
                if (candidate.snapshot.parentId == id) {
                    collect(candidateId);
                }
            }
        };
        collect(itemId);
        ChangeSet operationChange;
        bool first = true;
        for (const auto &snapshot : toRemove) {
            if (first) {
                operationChange.append(ChangeOperation(ItemRemovedChange {
                    .item = snapshot,
                    .cascade = false,
                }));
                first = false;
            } else {
                operationChange.append(ChangeOperation(CascadeRemovedChange {
                    .item = snapshot,
                    .ancestorId = itemId,
                }));
            }
        }
        auto beforeContext = makeContext();
        beforeContext._impl->mutationAllowed = true;
        const auto derivedStart = _impl->changeSet.operations().size();
        runHooks(engine, HookStage::BeforeApply, beforeContext, operationChange, true);
        const auto derivedEnd = _impl->changeSet.operations().size();
        for (const auto &snapshot : toRemove) {
            eraseSnapshot(engine, snapshot);
        }
        appendWithDerivedLinks(*_impl, operationChange, derivedStart, derivedEnd);
        auto afterContext = makeContext();
        runHooks(engine, HookStage::AfterApply, afterContext, operationChange, false);
        publish(engine, EventKind::AfterApply, _impl->origin, operationChange);
    } catch (...) {
        markFailed(*_impl);
        throw;
    }
}

void Transaction::removeAt(ListHandle list, Value associationValue, std::size_t index)
{
    requireActive(_impl.get());
    auto &engine = *_impl->engine;
    engine.schema.validate(list);
    const auto key = valueKey(associationValue);
    auto groupIt = engine.listGroups.find({list.containerId(), key});
    if (groupIt == engine.listGroups.end() || index >= groupIt->second.size()) {
        throw QueryError("list remove index is out of range");
    }
    const auto itemId = groupIt->second[index];
    auto snapshot = engine.items.at(itemId).snapshot;
    try {
        auto makeContext = [&]() {
            auto data = std::make_unique<TransactionContext::Impl>();
            data->transaction = _impl.get();
            data->origin = _impl->origin;
            return TransactionContext(std::move(data));
        };
        ChangeSet operationChange;
        operationChange.append(ChangeOperation(ListRemovedChange {
            .list = list,
            .associationValue = associationValue,
            .index = index,
            .item = snapshot,
        }));
        auto beforeContext = makeContext();
        beforeContext._impl->mutationAllowed = true;
        const auto derivedStart = _impl->changeSet.operations().size();
        runHooks(engine, HookStage::BeforeApply, beforeContext, operationChange, true);
        const auto derivedEnd = _impl->changeSet.operations().size();
        eraseSnapshot(engine, snapshot);
        appendWithDerivedLinks(*_impl, operationChange, derivedStart, derivedEnd);
        auto afterContext = makeContext();
        runHooks(engine, HookStage::AfterApply, afterContext, operationChange, false);
        publish(engine, EventKind::AfterApply, _impl->origin, operationChange);
    } catch (...) {
        markFailed(*_impl);
        throw;
    }
}

void Transaction::update(ItemId itemId, ColumnHandle column, Value value, AssociationUpdateOptions options)
{
    requireActive(_impl.get());
    auto &engine = *_impl->engine;
    try {
        engine.schema.validate(column);
        if (updatePendingBeforeApplySource(*_impl, itemId, column, value, options)) {
            return;
        }
        auto makeContext = [&]() {
            auto data = std::make_unique<TransactionContext::Impl>();
            data->transaction = _impl.get();
            data->origin = _impl->origin;
            return TransactionContext(std::move(data));
        };
        auto it = engine.items.find(itemId);
        if (it == engine.items.end()) {
            throw QueryError("item does not exist");
        }
        auto snapshot = it->second.snapshot;
        if (snapshot.containerId != column.containerId()) {
            throw QueryError("column does not belong to item");
        }
        const auto &schemaDefinition = schemaData(engine.schema);
        const auto &columnRecord = columnFor(schemaDefinition, column);
        if (columnRecord.info.computed) {
            throw ConstraintError("computed columns are not writable");
        }
        validateValueForColumn(columnRecord, value);
        auto oldValue = valueForColumn(snapshot, column.columnId());
        auto oldSnapshot = snapshot;
        auto relation = findRelationByColumn(const_cast<SchemaDefinitionData &>(schemaDefinition),
                                             column.containerId(),
                                             column.columnId());
        setColumnValue(snapshot, column, value);
        if (relation && snapshot.containerKind == ContainerKind::List &&
            containerFor(schemaDefinition, snapshot.containerId).listAssociation == relation->info.id) {
            if (value.isNull()) {
                snapshot.listAssociationValue.reset();
                snapshot.listIndex.reset();
            } else {
                if (!options.targetIndex) {
                    throw ConstraintError("target index is required when assigning a list association");
                }
                snapshot.listAssociationValue = value;
                snapshot.listIndex = *options.targetIndex;
            }
        }
        applyRelationMetadata(schemaDefinition, snapshot);
        validateSnapshot(engine, snapshot);
        auto operationChange = updateChangeSetForSnapshot(schemaDefinition,
                                                          oldSnapshot,
                                                          itemId,
                                                          column,
                                                          oldValue,
                                                          value,
                                                          options,
                                                          oldSnapshot.listIndex,
                                                          snapshot);
        auto beforeContext = makeContext();
        beforeContext._impl->mutationAllowed = true;
        const auto derivedStart = _impl->changeSet.operations().size();
        {
            PendingBeforeApplyScope pending(*_impl, snapshot, false, column.columnId());
            runHooks(engine, HookStage::BeforeApply, beforeContext, operationChange, true);
        }
        const auto derivedEnd = _impl->changeSet.operations().size();
        operationChange = updateChangeSetForSnapshot(schemaDefinition,
                                                     oldSnapshot,
                                                     itemId,
                                                     column,
                                                     oldValue,
                                                     value,
                                                     options,
                                                     oldSnapshot.listIndex,
                                                     snapshot);
        removeItemFromIndexes(engine, oldSnapshot);
        removeFromListGroup(engine, oldSnapshot);
        it->second.snapshot = snapshot;
        insertIntoListGroup(engine, snapshot);
        addItemToIndexes(engine, snapshot);
        appendWithDerivedLinks(*_impl, operationChange, derivedStart, derivedEnd);
        auto afterContext = makeContext();
        runHooks(engine, HookStage::AfterApply, afterContext, operationChange, false);
        publish(engine, EventKind::AfterApply, _impl->origin, operationChange);
    } catch (...) {
        markFailed(*_impl);
        throw;
    }
}

void Transaction::rotate(ListHandle list, Value associationValue, ListRotation rotation)
{
    requireActive(_impl.get());
    auto &engine = *_impl->engine;
    try {
        engine.schema.validate(list);
        auto makeContext = [&]() {
            auto data = std::make_unique<TransactionContext::Impl>();
            data->transaction = _impl.get();
            data->origin = _impl->origin;
            return TransactionContext(std::move(data));
        };
        auto key = valueKey(associationValue);
        auto &group = engine.listGroups[{list.containerId(), key}];
        if (rotation.startIndex + rotation.count > group.size()) {
            throw QueryError("list rotation range is out of range");
        }
        ChangeSet operationChange;
        operationChange.append(ChangeOperation(ListRotatedChange {
            .list = list,
            .associationValue = associationValue,
            .rotation = rotation,
        }));
        auto beforeContext = makeContext();
        beforeContext._impl->mutationAllowed = true;
        const auto derivedStart = _impl->changeSet.operations().size();
        runHooks(engine, HookStage::BeforeApply, beforeContext, operationChange, true);
        const auto derivedEnd = _impl->changeSet.operations().size();
        if (rotation.count > 0) {
            auto first = group.begin() + static_cast<std::ptrdiff_t>(rotation.startIndex);
            auto last = first + static_cast<std::ptrdiff_t>(rotation.count);
            auto shift = rotation.offset % static_cast<std::ptrdiff_t>(rotation.count);
            if (shift < 0) {
                shift += static_cast<std::ptrdiff_t>(rotation.count);
            }
            std::rotate(first, first + shift, last);
            refreshListIndexes(engine, list.containerId(), key);
        }
        appendWithDerivedLinks(*_impl, operationChange, derivedStart, derivedEnd);
        auto afterContext = makeContext();
        runHooks(engine, HookStage::AfterApply, afterContext, operationChange, false);
        publish(engine, EventKind::AfterApply, _impl->origin, operationChange);
    } catch (...) {
        markFailed(*_impl);
        throw;
    }
}

CommitResult Transaction::commit()
{
    requireActive(_impl.get());
    auto &engine = *_impl->engine;
    try {
        auto makeContext = [&]() {
            auto data = std::make_unique<TransactionContext::Impl>();
            data->transaction = _impl.get();
            data->origin = _impl->origin;
            return TransactionContext(std::move(data));
        };
        auto beforeContext = makeContext();
        runHooks(engine, HookStage::BeforeCommit, beforeContext, _impl->changeSet, false);
        CommitResult result {
            .changeSet = _impl->changeSet,
            .commitLog = serializeEngineCommitLog(engine.schema, _impl->changeSet),
            .origin = _impl->origin,
        };
        if (_impl->options.undoable && createsUndoStep(_impl->changeSet)) {
            engine.undoStack.push_back(UndoStep(_impl->changeSet));
            result.createdUndoStep = true;
            if (!engine.redoStack.empty()) {
                engine.redoStack.clear();
                result.clearedRedoStack = true;
            }
        }
        _impl->state = TransactionState::Committed;
        engine.activeTransaction = false;
        auto afterContext = makeContext();
        runHooks(engine, HookStage::AfterCommit, afterContext, _impl->changeSet, false);
        publish(engine, EventKind::AfterCommit, _impl->origin, _impl->changeSet);
        return result;
    } catch (...) {
        markFailed(*_impl);
        throw;
    }
}

void Transaction::rollback()
{
    if (!_impl || !_impl->engine ||
        (_impl->state != TransactionState::Active && _impl->state != TransactionState::Failed)) {
        throw TransactionError("transaction cannot be rolled back");
    }
    auto &engine = *_impl->engine;
    if (_impl->rollbackApplied) {
        _impl->state = TransactionState::RolledBack;
        engine.activeTransaction = false;
        return;
    }
    auto inverse = _impl->changeSet.invert();
    try {
        applyChangeSet(engine, inverse);
    } catch (...) {
        engine.items = _impl->rollbackItems;
        engine.listGroups = _impl->rollbackListGroups;
        engine.indexes = _impl->rollbackIndexes;
        throw;
    }
    _impl->state = TransactionState::RolledBack;
    engine.activeTransaction = false;
    publish(engine, EventKind::Rollback, _impl->origin, inverse);
}

} // namespace dini
