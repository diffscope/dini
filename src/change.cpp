#include "change_p.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <stdcorelib/pimpl.h>

#include <dini/errors.h>

namespace dini {

    namespace {

    template <typename... T>
    struct Overloaded : T... {
        using T::operator()...;
    };

    template <typename... T>
    Overloaded(T...) -> Overloaded<T...>;

    void hashCombine(std::size_t &seed, std::size_t value) noexcept
    {
        seed ^= value + std::size_t {0x9e3779b9U} + (seed << 6U) + (seed >> 2U);
    }

    struct UpdateKey {
        ChangeOperationKind kind;
        ItemId itemId;
        ColumnHandle column;

        friend bool operator==(const UpdateKey &lhs, const UpdateKey &rhs)
        {
            return lhs.kind == rhs.kind && lhs.itemId == rhs.itemId && lhs.column == rhs.column;
        }
    };

    struct UpdateKeyHash {
        std::size_t operator()(const UpdateKey &key) const
        {
            std::size_t result = 0;
            hashCombine(result, std::hash<std::size_t> {}(static_cast<std::size_t>(key.kind)));
            hashCombine(result, std::hash<ItemId> {}(key.itemId));
            hashCombine(result, std::hash<SchemaId> {}(key.column.schemaId()));
            hashCombine(result, std::hash<ContainerId> {}(key.column.containerId()));
            hashCombine(result, std::hash<ColumnId> {}(key.column.columnId()));
            hashCombine(result, std::hash<std::string> {}(key.column.debugName()));
            return result;
        }
    };

    struct OperationIndexPairHash {
        std::size_t operator()(const std::pair<std::size_t, std::size_t> &value) const noexcept
        {
            std::size_t result = 0;
            hashCombine(result, std::hash<std::size_t> {}(value.first));
            hashCombine(result, std::hash<std::size_t> {}(value.second));
            return result;
        }
    };

    ChangeOperationKind operationKind(const ChangeOperation::Payload &payload) noexcept
    {
        return std::visit(Overloaded {
                              [](const ItemInsertedChange &) { return ChangeOperationKind::ItemInserted; },
                              [](const ItemRemovedChange &) { return ChangeOperationKind::ItemRemoved; },
                              [](const ColumnUpdatedChange &) { return ChangeOperationKind::ColumnUpdated; },
                              [](const ComputedColumnUpdatedChange &) {
                                  return ChangeOperationKind::ComputedColumnUpdated;
                              },
                              [](const CascadeRemovedChange &) { return ChangeOperationKind::CascadeRemoved; },
                              [](const ListInsertedChange &) { return ChangeOperationKind::ListInserted; },
                              [](const ListRemovedChange &) { return ChangeOperationKind::ListRemoved; },
                              [](const ListRotatedChange &) { return ChangeOperationKind::ListRotated; },
                          },
                          payload);
    }

    ChangeOperation invertOperation(const ChangeOperation &operation)
    {
        return std::visit(Overloaded {
                              [](const ItemInsertedChange &change) -> ChangeOperation {
                                  return ChangeOperation(ItemRemovedChange {
                                      .item = change.item,
                                      .cascade = false,
                                  });
                              },
                              [](const ItemRemovedChange &change) -> ChangeOperation {
                                  return ChangeOperation(ItemInsertedChange {
                                      .item = change.item,
                                  });
                              },
                              [](const ColumnUpdatedChange &change) -> ChangeOperation {
                                  auto inverseOptions = change.associationOptions;
                                  if (!change.oldValue.isNull()) {
                                      inverseOptions.targetIndex = change.oldListIndex;
                                  } else {
                                      inverseOptions.targetIndex.reset();
                                  }
                                  return ChangeOperation(ColumnUpdatedChange {
                                      .itemId = change.itemId,
                                      .column = change.column,
                                      .oldValue = change.newValue,
                                      .newValue = change.oldValue,
                                      .associationOptions = inverseOptions,
                                      .oldListIndex = change.associationOptions.targetIndex,
                                  });
                              },
                              [](const ComputedColumnUpdatedChange &change) -> ChangeOperation {
                                  return ChangeOperation(ComputedColumnUpdatedChange {
                                      .itemId = change.itemId,
                                      .column = change.column,
                                      .oldValue = change.newValue,
                                      .newValue = change.oldValue,
                                  });
                              },
                              [](const CascadeRemovedChange &change) -> ChangeOperation {
                                  return ChangeOperation(ItemInsertedChange {
                                      .item = change.item,
                                  });
                              },
                              [](const ListInsertedChange &change) -> ChangeOperation {
                                  return ChangeOperation(ListRemovedChange {
                                      .list = change.list,
                                      .associationValue = change.associationValue,
                                      .index = change.index,
                                      .item = change.item,
                                  });
                              },
                              [](const ListRemovedChange &change) -> ChangeOperation {
                                  return ChangeOperation(ListInsertedChange {
                                      .list = change.list,
                                      .associationValue = change.associationValue,
                                      .index = change.index,
                                      .item = change.item,
                                  });
                              },
                              [](const ListRotatedChange &change) -> ChangeOperation {
                                  auto rotation = change.rotation;
                                  rotation.offset = -rotation.offset;
                                  return ChangeOperation(ListRotatedChange {
                                      .list = change.list,
                                      .associationValue = change.associationValue,
                                      .rotation = rotation,
                                  });
                              },
                          },
                          operation.payload());
    }

    } // namespace

ChangeOperation::ChangeOperation() : _impl(new Impl(this))
{
}

ChangeOperation::ChangeOperation(Payload payload) : _impl(new Impl(this, std::move(payload)))
{
}

ChangeOperation::~ChangeOperation() = default;

ChangeOperation::ChangeOperation(const ChangeOperation &other) = default;

ChangeOperation::ChangeOperation(ChangeOperation &&other) noexcept = default;

ChangeOperation &ChangeOperation::operator=(const ChangeOperation &other) = default;

ChangeOperation &ChangeOperation::operator=(ChangeOperation &&other) noexcept = default;

ChangeOperationKind ChangeOperation::kind() const noexcept
{
    stdc_impl_t;
    return operationKind(impl.payload);
}

const ChangeOperation::Payload &ChangeOperation::payload() const noexcept
{
    stdc_impl_t;
    return impl.payload;
}

ChangeSet::ChangeSet() : _impl(new Impl(this))
{
}

ChangeSet::ChangeSet(std::vector<ChangeOperation> operations) : _impl(new Impl(this, std::move(operations)))
{
}

ChangeSet::~ChangeSet() = default;

ChangeSet::ChangeSet(const ChangeSet &other) = default;

ChangeSet::ChangeSet(ChangeSet &&other) noexcept = default;

ChangeSet &ChangeSet::operator=(const ChangeSet &other) = default;

ChangeSet &ChangeSet::operator=(ChangeSet &&other) noexcept = default;

bool ChangeSet::empty() const noexcept
{
    stdc_impl_t;
    return impl.operations.empty();
}

const std::vector<ChangeOperation> &ChangeSet::operations() const noexcept
{
    stdc_impl_t;
    return impl.operations;
}

const std::vector<DerivedChangeLink> &ChangeSet::derivedLinks() const noexcept
{
    stdc_impl_t;
    return impl.derivedLinks;
}

void ChangeSet::append(ChangeOperation operation)
{
    stdc_impl_t;
    impl.operations.push_back(std::move(operation));
}

void ChangeSet::addDerivedLink(DerivedChangeLink link)
{
    stdc_impl_t;
    if (link.sourceOperation >= impl.operations.size() || link.derivedOperation >= impl.operations.size()) {
        throw std::out_of_range("derived change link operation index is outside the change set");
    }
    impl.derivedLinks.push_back(link);
}

ChangeSet ChangeSet::invert() const
{
    stdc_impl_t;

    std::vector<ChangeOperation> invertedOperations;
    invertedOperations.reserve(impl.operations.size());

    for (auto it = impl.operations.rbegin(); it != impl.operations.rend(); ++it) {
        invertedOperations.push_back(invertOperation(*it));
    }

    return ChangeSet(std::move(invertedOperations));
}

ByteArray ChangeSet::serialize() const
{
    return serializeChangeSet(*this);
}

ChangeSet ChangeSet::deserialize(const ByteArray &bytes, const EngineSchema &schema)
{
    return deserializeChangeSet(bytes, schema);
}

ChangeSet ChangeSet::merge(const std::vector<ChangeSet> &changes)
{
    std::vector<ChangeOperation> flattenedOperations;
    std::vector<DerivedChangeLink> flattenedLinks;

    for (const auto &changeSet : changes) {
        const auto operationOffset = flattenedOperations.size();

        for (const auto &operation : changeSet.operations()) {
            flattenedOperations.push_back(operation);
        }

        for (const auto &link : changeSet.derivedLinks()) {
            flattenedLinks.push_back(DerivedChangeLink {
                .sourceOperation = operationOffset + link.sourceOperation,
                .derivedOperation = operationOffset + link.derivedOperation,
            });
        }
    }

    std::vector<ChangeOperation> mergedOperations;
    std::vector<std::size_t> mergedIndexBySource(flattenedOperations.size());
    std::unordered_map<UpdateKey, std::size_t, UpdateKeyHash> mergedIndexByUpdate;
    mergedIndexByUpdate.reserve(flattenedOperations.size());

    for (std::size_t sourceIndex = 0; sourceIndex < flattenedOperations.size(); ++sourceIndex) {
        const auto &operation = flattenedOperations[sourceIndex];
        std::optional<UpdateKey> updateKey;
        std::optional<std::size_t> mergedIndex;

        if (const auto *change = std::get_if<ColumnUpdatedChange>(&operation.payload())) {
            updateKey.emplace(UpdateKey {
                .kind = ChangeOperationKind::ColumnUpdated,
                .itemId = change->itemId,
                .column = change->column,
            });
            const auto existingIndex = mergedIndexByUpdate.find(*updateKey);

            if (existingIndex != mergedIndexByUpdate.end()) {
                mergedIndex = existingIndex->second;
                const auto existing = std::get<ColumnUpdatedChange>(mergedOperations[*mergedIndex].payload());
                mergedOperations[*mergedIndex] = ChangeOperation(ChangeOperation::Payload {ColumnUpdatedChange {
                    .itemId = change->itemId,
                    .column = change->column,
                    .oldValue = existing.oldValue,
                    .newValue = change->newValue,
                    .associationOptions = change->associationOptions,
                    .oldListIndex = existing.oldListIndex,
                }});
            }
        } else if (const auto *change = std::get_if<ComputedColumnUpdatedChange>(&operation.payload())) {
            updateKey.emplace(UpdateKey {
                .kind = ChangeOperationKind::ComputedColumnUpdated,
                .itemId = change->itemId,
                .column = change->column,
            });
            const auto existingIndex = mergedIndexByUpdate.find(*updateKey);

            if (existingIndex != mergedIndexByUpdate.end()) {
                mergedIndex = existingIndex->second;
                const auto existing = std::get<ComputedColumnUpdatedChange>(mergedOperations[*mergedIndex].payload());
                mergedOperations[*mergedIndex] = ChangeOperation(ChangeOperation::Payload {ComputedColumnUpdatedChange {
                    .itemId = change->itemId,
                    .column = change->column,
                    .oldValue = existing.oldValue,
                    .newValue = change->newValue,
                }});
            }
        }

        if (!mergedIndex.has_value()) {
            mergedOperations.push_back(operation);
            mergedIndex = mergedOperations.size() - 1;
            if (updateKey.has_value()) {
                mergedIndexByUpdate.emplace(std::move(*updateKey), *mergedIndex);
            }
        }

        mergedIndexBySource[sourceIndex] = *mergedIndex;
    }

    ChangeSet merged;
    for (const auto &operation : mergedOperations) {
        merged.append(operation);
    }

    std::unordered_set<std::pair<std::size_t, std::size_t>, OperationIndexPairHash> addedLinks;
    addedLinks.reserve(flattenedLinks.size());
    for (const auto &link : flattenedLinks) {
        const auto mergedLink = std::pair {
            mergedIndexBySource[link.sourceOperation],
            mergedIndexBySource[link.derivedOperation],
        };
        if (addedLinks.insert(mergedLink).second) {
            merged.addDerivedLink(DerivedChangeLink {
                .sourceOperation = mergedLink.first,
                .derivedOperation = mergedLink.second,
            });
        }
    }

    return merged;
}

UndoStep::UndoStep() : _impl(new Impl(this))
{
}

UndoStep::UndoStep(ChangeSet changeSet, std::string label)
    : _impl(new Impl(this, std::move(changeSet), std::move(label)))
{
}

UndoStep::~UndoStep() = default;

UndoStep::UndoStep(const UndoStep &other) = default;

UndoStep::UndoStep(UndoStep &&other) noexcept = default;

UndoStep &UndoStep::operator=(const UndoStep &other) = default;

UndoStep &UndoStep::operator=(UndoStep &&other) noexcept = default;

const ChangeSet &UndoStep::changeSet() const noexcept
{
    stdc_impl_t;
    return impl.changeSet;
}

const std::string &UndoStep::label() const noexcept
{
    stdc_impl_t;
    return impl.label;
}

} // namespace dini
