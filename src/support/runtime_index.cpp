#include "runtime_index.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <sstream>
#include <utility>

namespace dini {

namespace {

Value valueForColumn(const ItemSnapshot &item, ColumnId columnId)
{
    auto it = std::find_if(item.values.begin(), item.values.end(), [columnId](const ColumnValue &columnValue) {
        return columnValue.column.columnId() == columnId;
    });
    return it == item.values.end() ? Value::null() : it->value;
}

bool columnIsIndexed(const ColumnDefinitionRecord &column)
{
    return column.info.index != IndexKind::None || column.info.association;
}

AggregateIndexOptions aggregateOptionsFor(const ColumnDefinitionRecord &column)
{
    if (column.info.computed) {
        return column.computedDefinition.aggregateIndex;
    }
    if (column.info.variantSpecific) {
        return column.variantDefinition.aggregateIndex;
    }
    return column.normalDefinition.aggregateIndex;
}

bool aggregateEnabled(const AggregateIndexOptions &options) noexcept
{
    return options.sum || options.minMax;
}

std::string stableValueKey(const Value &value)
{
    std::ostringstream stream;
    stream << static_cast<int>(value.type()) << ':';
    std::visit(
        [&](const auto &payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                stream << "null";
            } else if constexpr (std::is_same_v<T, bool>) {
                stream << (payload ? "1" : "0");
            } else if constexpr (std::is_same_v<T, std::string>) {
                stream << payload.size() << ':' << payload;
            } else if constexpr (std::is_same_v<T, ByteArray>) {
                stream << payload.size() << ':';
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

double numericValue(const Value &value)
{
    switch (value.type()) {
        case ValueType::Int64:
            return static_cast<double>(value.asInt64());
        case ValueType::UInt64:
            return static_cast<double>(value.asUInt64());
        case ValueType::Double:
            return value.asDouble();
        default:
            return 0.0;
    }
}

Value sumValues(const Value &lhs, const Value &rhs)
{
    if (lhs.isNull()) {
        switch (rhs.type()) {
            case ValueType::Int64:
            case ValueType::UInt64:
            case ValueType::Double:
                return rhs;
            default:
                return Value(0.0);
        }
    }
    if (lhs.type() == ValueType::Int64 && rhs.type() == ValueType::Int64) {
        return Value(lhs.asInt64() + rhs.asInt64());
    }
    if (lhs.type() == ValueType::UInt64 && rhs.type() == ValueType::UInt64) {
        return Value(lhs.asUInt64() + rhs.asUInt64());
    }
    return Value(numericValue(lhs) + numericValue(rhs));
}

Value subtractValues(const Value &lhs, const Value &rhs)
{
    if (lhs.isNull()) {
        return lhs;
    }
    if (lhs.type() == ValueType::Int64 && rhs.type() == ValueType::Int64) {
        return Value(lhs.asInt64() - rhs.asInt64());
    }
    if (lhs.type() == ValueType::UInt64 && rhs.type() == ValueType::UInt64) {
        return Value(lhs.asUInt64() - rhs.asUInt64());
    }
    return Value(numericValue(lhs) - numericValue(rhs));
}

void addAggregateValue(RuntimeIndexStore::AggregateBucket &bucket,
                       const AggregateIndexOptions &options,
                       const Value &value)
{
    ++bucket.count;
    if (options.sum) {
        bucket.sum = sumValues(bucket.sum, value);
    }
    if (options.minMax) {
        ++bucket.values[ComparableValueKey(value)];
    }
}

void removeAggregateValue(RuntimeIndexStore::AggregateBucket &bucket,
                          const AggregateIndexOptions &options,
                          const Value &value)
{
    if (bucket.count > 0) {
        --bucket.count;
    }
    if (options.sum) {
        bucket.sum = subtractValues(bucket.sum, value);
    }
    if (options.minMax) {
        auto it = bucket.values.find(ComparableValueKey(value));
        if (it != bucket.values.end()) {
            if (it->second <= 1) {
                bucket.values.erase(it);
            } else {
                --it->second;
            }
        }
    }
}

Value minMaxValue(const RuntimeIndexStore::AggregateBucket &bucket, AggregateKind kind)
{
    if (bucket.values.empty()) {
        return Value::null();
    }
    return kind == AggregateKind::Minimum ? bucket.values.begin()->first.value : bucket.values.rbegin()->first.value;
}

void mergeAggregateBucket(RuntimeIndexStore::AggregateBucket &target,
                          const RuntimeIndexStore::AggregateBucket &source,
                          const AggregateIndexOptions &options)
{
    target.count += source.count;
    if (options.sum) {
        target.sum = sumValues(target.sum, source.sum);
    }
    if (options.minMax) {
        for (const auto &[key, count] : source.values) {
            target.values[key] += count;
        }
    }
}

Value aggregateBucketValue(const RuntimeIndexStore::AggregateBucket &bucket, AggregateKind kind)
{
    if (kind == AggregateKind::Sum) {
        return bucket.sum;
    }
    return minMaxValue(bucket, kind);
}

bool constraintContains(const RuntimeRangeConstraint &constraint, long double value)
{
    const auto bound = runtimeNumericValue(constraint.value);
    if (!bound) {
        return false;
    }
    switch (constraint.op) {
        case ComparisonOperator::Equal:
            return value == *bound;
        case ComparisonOperator::NotEqual:
            return value != *bound;
        case ComparisonOperator::Greater:
            return value > *bound;
        case ComparisonOperator::Less:
            return value < *bound;
        case ComparisonOperator::GreaterOrEqual:
            return value >= *bound;
        case ComparisonOperator::LessOrEqual:
            return value <= *bound;
    }
    return false;
}

bool boundsOutside(const RuntimeRangeConstraint &constraint, long double minValue, long double maxValue)
{
    const auto bound = runtimeNumericValue(constraint.value);
    if (!bound) {
        return true;
    }
    switch (constraint.op) {
        case ComparisonOperator::Equal:
            return *bound < minValue || *bound > maxValue;
        case ComparisonOperator::NotEqual:
            return minValue == maxValue && minValue == *bound;
        case ComparisonOperator::Greater:
            return maxValue <= *bound;
        case ComparisonOperator::Less:
            return minValue >= *bound;
        case ComparisonOperator::GreaterOrEqual:
            return maxValue < *bound;
        case ComparisonOperator::LessOrEqual:
            return minValue > *bound;
    }
    return true;
}

bool boundsInside(const RuntimeRangeConstraint &constraint, long double minValue, long double maxValue)
{
    const auto bound = runtimeNumericValue(constraint.value);
    if (!bound) {
        return false;
    }
    switch (constraint.op) {
        case ComparisonOperator::Equal:
            return minValue == maxValue && minValue == *bound;
        case ComparisonOperator::NotEqual:
            return *bound < minValue || *bound > maxValue;
        case ComparisonOperator::Greater:
            return minValue > *bound;
        case ComparisonOperator::Less:
            return maxValue < *bound;
        case ComparisonOperator::GreaterOrEqual:
            return minValue >= *bound;
        case ComparisonOperator::LessOrEqual:
            return maxValue <= *bound;
    }
    return false;
}

} // namespace

bool operator<(const RuntimeIndexedFieldKey &lhs, const RuntimeIndexedFieldKey &rhs) noexcept
{
    return std::tie(lhs.containerId, lhs.kind, lhs.id) < std::tie(rhs.containerId, rhs.kind, rhs.id);
}

bool operator==(const RuntimeIndexedFieldKey &lhs, const RuntimeIndexedFieldKey &rhs) noexcept
{
    return lhs.containerId == rhs.containerId && lhs.kind == rhs.kind && lhs.id == rhs.id;
}

ComparableValueKey::ComparableValueKey(Value value) : value(std::move(value)) {}

bool operator<(const ComparableValueKey &lhs, const ComparableValueKey &rhs)
{
    return compareValues(lhs.value, rhs.value) < 0;
}

bool operator==(const ComparableValueKey &lhs, const ComparableValueKey &rhs)
{
    return compareValues(lhs.value, rhs.value) == 0;
}

void ScalarValueIndex::add(const Value &value, ItemId id)
{
    _buckets[ComparableValueKey(value)].insert(id);
}

void ScalarValueIndex::remove(const Value &value, ItemId id)
{
    auto bucketIt = _buckets.find(ComparableValueKey(value));
    if (bucketIt == _buckets.end()) {
        return;
    }
    bucketIt->second.erase(id);
    if (bucketIt->second.empty()) {
        _buckets.erase(bucketIt);
    }
}

RuntimeItemIdSet ScalarValueIndex::query(ComparisonOperator op, const Value &value) const
{
    RuntimeItemIdSet result;
    const auto key = ComparableValueKey(value);
    auto append = [&](const auto &ids) {
        result.insert(ids.begin(), ids.end());
    };
    switch (op) {
        case ComparisonOperator::Equal: {
            auto it = _buckets.find(key);
            if (it != _buckets.end()) {
                append(it->second);
            }
            break;
        }
        case ComparisonOperator::NotEqual:
            for (const auto &[bucketKey, ids] : _buckets) {
                if (!(bucketKey == key)) {
                    append(ids);
                }
            }
            break;
        case ComparisonOperator::Greater:
            for (auto it = _buckets.upper_bound(key); it != _buckets.end(); ++it) {
                append(it->second);
            }
            break;
        case ComparisonOperator::Less:
            for (auto it = _buckets.begin(); it != _buckets.lower_bound(key); ++it) {
                append(it->second);
            }
            break;
        case ComparisonOperator::GreaterOrEqual:
            for (auto it = _buckets.lower_bound(key); it != _buckets.end(); ++it) {
                append(it->second);
            }
            break;
        case ComparisonOperator::LessOrEqual:
            for (auto it = _buckets.begin(); it != _buckets.upper_bound(key); ++it) {
                append(it->second);
            }
            break;
    }
    return result;
}

void ScalarValueIndex::ordered(const std::function<bool(ItemId)> &visitor, bool descending) const
{
    if (descending) {
        for (auto bucketIt = _buckets.rbegin(); bucketIt != _buckets.rend(); ++bucketIt) {
            for (const auto id : bucketIt->second) {
                if (!visitor(id)) {
                    return;
                }
            }
        }
        return;
    }
    for (const auto &[key, ids] : _buckets) {
        (void)key;
        for (const auto id : ids) {
            if (!visitor(id)) {
                return;
            }
        }
    }
}

std::size_t ScalarValueIndex::count(ComparisonOperator op, const Value &value) const
{
    std::size_t result = 0;
    const auto key = ComparableValueKey(value);
    auto addSize = [&](const auto &ids) {
        result += ids.size();
    };
    switch (op) {
        case ComparisonOperator::Equal: {
            auto it = _buckets.find(key);
            if (it != _buckets.end()) {
                addSize(it->second);
            }
            break;
        }
        case ComparisonOperator::NotEqual:
            for (const auto &[bucketKey, ids] : _buckets) {
                if (!(bucketKey == key)) {
                    addSize(ids);
                }
            }
            break;
        case ComparisonOperator::Greater:
            for (auto it = _buckets.upper_bound(key); it != _buckets.end(); ++it) {
                addSize(it->second);
            }
            break;
        case ComparisonOperator::Less:
            for (auto it = _buckets.begin(); it != _buckets.lower_bound(key); ++it) {
                addSize(it->second);
            }
            break;
        case ComparisonOperator::GreaterOrEqual:
            for (auto it = _buckets.lower_bound(key); it != _buckets.end(); ++it) {
                addSize(it->second);
            }
            break;
        case ComparisonOperator::LessOrEqual:
            for (auto it = _buckets.begin(); it != _buckets.upper_bound(key); ++it) {
                addSize(it->second);
            }
            break;
    }
    return result;
}

bool ScalarValueIndex::empty() const noexcept
{
    return _buckets.empty();
}

void EqualityValueIndex::add(const Value &value, ItemId id)
{
    _buckets[stableValueKey(value)].insert(id);
}

void EqualityValueIndex::remove(const Value &value, ItemId id)
{
    auto it = _buckets.find(stableValueKey(value));
    if (it == _buckets.end()) {
        return;
    }
    it->second.erase(id);
    if (it->second.empty()) {
        _buckets.erase(it);
    }
}

RuntimeItemIdSet EqualityValueIndex::query(ComparisonOperator op,
                                           const Value &value,
                                           const RuntimeItemIdSet &universe) const
{
    RuntimeItemIdSet result;
    const auto key = stableValueKey(value);
    if (op == ComparisonOperator::Equal) {
        auto it = _buckets.find(key);
        if (it != _buckets.end()) {
            result = it->second;
        }
        return result;
    }
    if (op == ComparisonOperator::NotEqual) {
        auto equalIt = _buckets.find(key);
        if (equalIt == _buckets.end()) {
            return universe;
        }
        std::set_difference(universe.begin(),
                            universe.end(),
                            equalIt->second.begin(),
                            equalIt->second.end(),
                            std::inserter(result, result.end()));
    }
    return result;
}

void ContainerRangeIndex::clear()
{
    _containers.clear();
}

void ContainerRangeIndex::add(ContainerId containerId, ItemId id, std::map<RuntimeIndexedFieldKey, Value> values)
{
    Point point;
    point.id = id;
    for (auto &[field, value] : values) {
        auto numeric = runtimeNumericValue(value);
        if (numeric) {
            point.values[field] = *numeric;
        }
    }
    if (point.values.empty()) {
        return;
    }
    auto &container = _containers[containerId];
    container.pointById[id] = std::move(point);
    container.dirty = true;
}

void ContainerRangeIndex::remove(ContainerId containerId, ItemId id)
{
    auto containerIt = _containers.find(containerId);
    if (containerIt == _containers.end()) {
        return;
    }
    containerIt->second.pointById.erase(id);
    containerIt->second.dirty = true;
    if (containerIt->second.pointById.empty()) {
        _containers.erase(containerIt);
    }
}

RuntimeItemIdSet ContainerRangeIndex::query(ContainerId containerId,
                                            const std::vector<RuntimeRangeConstraint> &constraints) const
{
    RuntimeItemIdSet result;
    auto containerIt = _containers.find(containerId);
    if (containerIt == _containers.end()) {
        return result;
    }
    auto &container = containerIt->second;
    if (container.dirty) {
        container.points.clear();
        container.dimensions.clear();
        std::set<RuntimeIndexedFieldKey> dimensionSet;
        for (const auto &[id, point] : container.pointById) {
            (void)id;
            container.points.push_back(point);
            for (const auto &[field, value] : point.values) {
                (void)value;
                dimensionSet.insert(field);
            }
        }
        container.dimensions.assign(dimensionSet.begin(), dimensionSet.end());

        std::function<std::unique_ptr<Node>(std::vector<std::size_t>, std::size_t)> build =
            [&](std::vector<std::size_t> indexes, std::size_t depth) -> std::unique_ptr<Node> {
            if (indexes.empty()) {
                return {};
            }
            auto node = std::make_unique<Node>();
            for (const auto index : indexes) {
                const auto &point = container.points[index];
                node->ids.insert(point.id);
                for (const auto &[field, value] : point.values) {
                    auto boundsIt = node->bounds.find(field);
                    if (boundsIt == node->bounds.end()) {
                        node->bounds.emplace(field, std::pair<long double, long double> {value, value});
                    } else {
                        boundsIt->second.first = std::min(boundsIt->second.first, value);
                        boundsIt->second.second = std::max(boundsIt->second.second, value);
                    }
                }
            }
            constexpr std::size_t leafSize = 32;
            if (indexes.size() <= leafSize || container.dimensions.empty()) {
                node->points = std::move(indexes);
                return node;
            }
            const auto splitField = container.dimensions[depth % container.dimensions.size()];
            std::stable_sort(indexes.begin(), indexes.end(), [&](std::size_t lhs, std::size_t rhs) {
                const auto lhsIt = container.points[lhs].values.find(splitField);
                const auto rhsIt = container.points[rhs].values.find(splitField);
                const auto lhsValue = lhsIt == container.points[lhs].values.end()
                    ? -std::numeric_limits<long double>::infinity()
                    : lhsIt->second;
                const auto rhsValue = rhsIt == container.points[rhs].values.end()
                    ? -std::numeric_limits<long double>::infinity()
                    : rhsIt->second;
                return lhsValue < rhsValue;
            });
            const auto mid = indexes.size() / 2;
            std::vector<std::size_t> left(indexes.begin(), indexes.begin() + static_cast<std::ptrdiff_t>(mid));
            std::vector<std::size_t> right(indexes.begin() + static_cast<std::ptrdiff_t>(mid), indexes.end());
            node->left = build(std::move(left), depth + 1);
            node->right = build(std::move(right), depth + 1);
            return node;
        };

        std::vector<std::size_t> indexes(container.points.size());
        for (std::size_t i = 0; i < indexes.size(); ++i) {
            indexes[i] = i;
        }
        container.root = build(std::move(indexes), 0);
        container.dirty = false;
    }

    std::function<void(const Node *)> visit = [&](const Node *node) {
        if (!node) {
            return;
        }
        bool insideAll = true;
        for (const auto &constraint : constraints) {
            auto boundsIt = node->bounds.find(constraint.field);
            if (boundsIt == node->bounds.end()) {
                return;
            }
            if (boundsOutside(constraint, boundsIt->second.first, boundsIt->second.second)) {
                return;
            }
            insideAll = insideAll && boundsInside(constraint, boundsIt->second.first, boundsIt->second.second);
        }
        if (insideAll) {
            result.insert(node->ids.begin(), node->ids.end());
            return;
        }
        if (!node->points.empty()) {
            for (const auto index : node->points) {
                const auto &point = container.points[index];
                bool matches = true;
                for (const auto &constraint : constraints) {
                    auto valueIt = point.values.find(constraint.field);
                    if (valueIt == point.values.end() || !constraintContains(constraint, valueIt->second)) {
                        matches = false;
                        break;
                    }
                }
                if (matches) {
                    result.insert(point.id);
                }
            }
            return;
        }
        visit(node->left.get());
        visit(node->right.get());
    };
    visit(container.root.get());
    return result;
}

namespace {

std::vector<ComparableValueKey> keyValuesForColumns(const ItemSnapshot &snapshot,
                                                    const std::vector<ColumnHandle> &columns)
{
    std::vector<ComparableValueKey> values;
    values.reserve(columns.size());
    for (const auto &column : columns) {
        const auto value = valueForColumn(snapshot, column.columnId());
        if (value.isNull()) {
            return {};
        }
        values.push_back(ComparableValueKey(value));
    }
    return values;
}

std::vector<ComparableValueKey> keyValuesFromValues(const std::vector<Value> &values)
{
    std::vector<ComparableValueKey> result;
    result.reserve(values.size());
    for (const auto &value : values) {
        if (value.isNull()) {
            return {};
        }
        result.push_back(ComparableValueKey(value));
    }
    return result;
}

std::uint32_t priorityForInterval(ItemId id, const Value &start, const Value &end)
{
    std::uint64_t hash = id ^ 0x9e3779b97f4a7c15ULL;
    auto mix = [&](std::uint64_t value) {
        hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
    };
    const std::hash<std::string> hashString;
    mix(static_cast<std::uint64_t>(hashString(stableValueKey(start))));
    mix(static_cast<std::uint64_t>(hashString(stableValueKey(end))));
    hash ^= hash >> 33U;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33U;
    return static_cast<std::uint32_t>(hash);
}

} // namespace

bool operator<(const OrderedRuntimeIndex::GroupKey &lhs, const OrderedRuntimeIndex::GroupKey &rhs)
{
    return std::lexicographical_compare(lhs.values.begin(), lhs.values.end(), rhs.values.begin(), rhs.values.end());
}

bool operator<(const OrderedRuntimeIndex::OrderedEntry &lhs, const OrderedRuntimeIndex::OrderedEntry &rhs)
{
    if (std::lexicographical_compare(lhs.orderValues.begin(), lhs.orderValues.end(), rhs.orderValues.begin(), rhs.orderValues.end())) {
        return true;
    }
    if (std::lexicographical_compare(rhs.orderValues.begin(), rhs.orderValues.end(), lhs.orderValues.begin(), lhs.orderValues.end())) {
        return false;
    }
    return lhs.id < rhs.id;
}

void OrderedRuntimeIndex::add(const OrderedIndexDefinitionRecord &definition, const ItemSnapshot &snapshot)
{
    auto groupValues = keyValuesForColumns(snapshot, definition.groupBy);
    if (groupValues.size() != definition.groupBy.size()) {
        return;
    }
    auto orderValues = keyValuesForColumns(snapshot, definition.orderBy);
    if (orderValues.size() != definition.orderBy.size()) {
        return;
    }
    _groups[GroupKey {std::move(groupValues)}].insert(OrderedEntry {
        .orderValues = std::move(orderValues),
        .id = snapshot.id,
        .tieBreakById = definition.tieBreakById,
    });
}

void OrderedRuntimeIndex::remove(const OrderedIndexDefinitionRecord &definition, const ItemSnapshot &snapshot)
{
    auto groupValues = keyValuesForColumns(snapshot, definition.groupBy);
    if (groupValues.size() != definition.groupBy.size()) {
        return;
    }
    auto orderValues = keyValuesForColumns(snapshot, definition.orderBy);
    if (orderValues.size() != definition.orderBy.size()) {
        return;
    }
    auto groupIt = _groups.find(GroupKey {std::move(groupValues)});
    if (groupIt == _groups.end()) {
        return;
    }
    groupIt->second.erase(OrderedEntry {
        .orderValues = std::move(orderValues),
        .id = snapshot.id,
        .tieBreakById = definition.tieBreakById,
    });
    if (groupIt->second.empty()) {
        _groups.erase(groupIt);
    }
}

std::optional<ItemId> OrderedRuntimeIndex::previous(const OrderedIndexDefinitionRecord &definition,
                                                    const ItemSnapshot &probe,
                                                    const std::set<ItemId> &excludedIds) const
{
    auto groupValues = keyValuesForColumns(probe, definition.groupBy);
    if (groupValues.size() != definition.groupBy.size()) {
        return {};
    }
    auto orderValues = keyValuesForColumns(probe, definition.orderBy);
    if (orderValues.size() != definition.orderBy.size()) {
        return {};
    }
    auto groupIt = _groups.find(GroupKey {std::move(groupValues)});
    if (groupIt == _groups.end()) {
        return {};
    }
    const auto probeEntry = OrderedEntry {
        .orderValues = std::move(orderValues),
        .id = probe.id,
        .tieBreakById = definition.tieBreakById,
    };
    auto it = groupIt->second.lower_bound(probeEntry);
    while (it != groupIt->second.begin()) {
        --it;
        if (excludedIds.find(it->id) == excludedIds.end()) {
            return it->id;
        }
    }
    return {};
}

std::optional<ItemId> OrderedRuntimeIndex::next(const OrderedIndexDefinitionRecord &definition,
                                                const ItemSnapshot &probe,
                                                const std::set<ItemId> &excludedIds) const
{
    auto groupValues = keyValuesForColumns(probe, definition.groupBy);
    if (groupValues.size() != definition.groupBy.size()) {
        return {};
    }
    auto orderValues = keyValuesForColumns(probe, definition.orderBy);
    if (orderValues.size() != definition.orderBy.size()) {
        return {};
    }
    auto groupIt = _groups.find(GroupKey {std::move(groupValues)});
    if (groupIt == _groups.end()) {
        return {};
    }
    const auto probeEntry = OrderedEntry {
        .orderValues = std::move(orderValues),
        .id = probe.id,
        .tieBreakById = definition.tieBreakById,
    };
    for (auto it = groupIt->second.upper_bound(probeEntry); it != groupIt->second.end(); ++it) {
        if (excludedIds.find(it->id) == excludedIds.end()) {
            return it->id;
        }
    }
    return {};
}

void OrderedRuntimeIndex::ordered(const OrderedIndexDefinitionRecord &definition,
                                  const std::vector<Value> &groupKey,
                                  bool descending,
                                  const std::function<bool(ItemId)> &visitor) const
{
    auto groupValues = keyValuesFromValues(groupKey);
    if (groupValues.size() != definition.groupBy.size()) {
        return;
    }
    auto groupIt = _groups.find(GroupKey {std::move(groupValues)});
    if (groupIt == _groups.end()) {
        return;
    }
    if (descending) {
        for (auto it = groupIt->second.rbegin(); it != groupIt->second.rend(); ++it) {
            if (!visitor(it->id)) {
                return;
            }
        }
        return;
    }
    for (const auto &entry : groupIt->second) {
        if (!visitor(entry.id)) {
            return;
        }
    }
}

bool operator<(const IntervalRuntimeIndex::GroupKey &lhs, const IntervalRuntimeIndex::GroupKey &rhs)
{
    return std::lexicographical_compare(lhs.values.begin(), lhs.values.end(), rhs.values.begin(), rhs.values.end());
}

bool operator<(const IntervalRuntimeIndex::IntervalEntry &lhs, const IntervalRuntimeIndex::IntervalEntry &rhs)
{
    return std::tie(lhs.start, lhs.end, lhs.id) < std::tie(rhs.start, rhs.end, rhs.id);
}

struct IntervalRuntimeIndex::Node {
    IntervalEntry entry;
    Value subtreeMaxEnd;
    std::uint32_t priority = 0;
    std::unique_ptr<Node> left;
    std::unique_ptr<Node> right;

    explicit Node(IntervalEntry entry)
        : entry(std::move(entry)),
          subtreeMaxEnd(this->entry.end.value),
          priority(priorityForInterval(this->entry.id, this->entry.start.value, this->entry.end.value))
    {
    }
};

namespace {

std::unique_ptr<IntervalRuntimeIndex::Node> cloneIntervalNode(const IntervalRuntimeIndex::Node *node)
{
    if (!node) {
        return {};
    }
    auto copy = std::make_unique<IntervalRuntimeIndex::Node>(node->entry);
    copy->subtreeMaxEnd = node->subtreeMaxEnd;
    copy->priority = node->priority;
    copy->left = cloneIntervalNode(node->left.get());
    copy->right = cloneIntervalNode(node->right.get());
    return copy;
}

bool valueLess(const Value &lhs, const Value &rhs)
{
    return ComparableValueKey(lhs) < ComparableValueKey(rhs);
}

void updateIntervalNode(IntervalRuntimeIndex::Node *node)
{
    if (!node) {
        return;
    }
    node->subtreeMaxEnd = node->entry.end.value;
    if (node->left && valueLess(node->subtreeMaxEnd, node->left->subtreeMaxEnd)) {
        node->subtreeMaxEnd = node->left->subtreeMaxEnd;
    }
    if (node->right && valueLess(node->subtreeMaxEnd, node->right->subtreeMaxEnd)) {
        node->subtreeMaxEnd = node->right->subtreeMaxEnd;
    }
}

void rotateIntervalRight(std::unique_ptr<IntervalRuntimeIndex::Node> &node)
{
    auto left = std::move(node->left);
    node->left = std::move(left->right);
    updateIntervalNode(node.get());
    left->right = std::move(node);
    updateIntervalNode(left->right.get());
    updateIntervalNode(left.get());
    node = std::move(left);
}

void rotateIntervalLeft(std::unique_ptr<IntervalRuntimeIndex::Node> &node)
{
    auto right = std::move(node->right);
    node->right = std::move(right->left);
    updateIntervalNode(node.get());
    right->left = std::move(node);
    updateIntervalNode(right->left.get());
    updateIntervalNode(right.get());
    node = std::move(right);
}

void insertIntervalNode(std::unique_ptr<IntervalRuntimeIndex::Node> &node, IntervalRuntimeIndex::IntervalEntry entry)
{
    if (!node) {
        node = std::make_unique<IntervalRuntimeIndex::Node>(std::move(entry));
        return;
    }
    if (entry < node->entry) {
        insertIntervalNode(node->left, std::move(entry));
        if (node->left && node->left->priority < node->priority) {
            rotateIntervalRight(node);
        }
    } else if (node->entry < entry) {
        insertIntervalNode(node->right, std::move(entry));
        if (node->right && node->right->priority < node->priority) {
            rotateIntervalLeft(node);
        }
    } else {
        return;
    }
    updateIntervalNode(node.get());
}

void eraseIntervalNode(std::unique_ptr<IntervalRuntimeIndex::Node> &node, const IntervalRuntimeIndex::IntervalEntry &entry)
{
    if (!node) {
        return;
    }
    if (entry < node->entry) {
        eraseIntervalNode(node->left, entry);
    } else if (node->entry < entry) {
        eraseIntervalNode(node->right, entry);
    } else if (!node->left) {
        node = std::move(node->right);
    } else if (!node->right) {
        node = std::move(node->left);
    } else if (node->left->priority < node->right->priority) {
        rotateIntervalRight(node);
        eraseIntervalNode(node->right, entry);
    } else {
        rotateIntervalLeft(node);
        eraseIntervalNode(node->left, entry);
    }
    updateIntervalNode(node.get());
}

void queryIntervalNode(const IntervalRuntimeIndex::Node *node,
                       const Value &probeStart,
                       const Value &probeEnd,
                       const std::set<ItemId> &excludedIds,
                       RuntimeItemIdSet &result)
{
    if (!node || !valueLess(probeStart, node->subtreeMaxEnd)) {
        return;
    }
    queryIntervalNode(node->left.get(), probeStart, probeEnd, excludedIds, result);
    if (valueLess(node->entry.start.value, probeEnd)) {
        if (valueLess(probeStart, node->entry.end.value) && excludedIds.find(node->entry.id) == excludedIds.end()) {
            result.insert(node->entry.id);
        }
        queryIntervalNode(node->right.get(), probeStart, probeEnd, excludedIds, result);
    }
}

IntervalRuntimeIndex::IntervalEntry intervalEntryFor(const IntervalIndexDefinitionRecord &definition,
                                                     const ItemSnapshot &snapshot)
{
    return IntervalRuntimeIndex::IntervalEntry {
        .start = ComparableValueKey(valueForColumn(snapshot, definition.start.columnId())),
        .end = ComparableValueKey(valueForColumn(snapshot, definition.end.columnId())),
        .id = snapshot.id,
    };
}

} // namespace

IntervalRuntimeIndex::IntervalRuntimeIndex() = default;
IntervalRuntimeIndex::IntervalRuntimeIndex(const IntervalRuntimeIndex &other) = default;
IntervalRuntimeIndex &IntervalRuntimeIndex::operator=(const IntervalRuntimeIndex &other) = default;
IntervalRuntimeIndex::IntervalRuntimeIndex(IntervalRuntimeIndex &&other) noexcept = default;
IntervalRuntimeIndex &IntervalRuntimeIndex::operator=(IntervalRuntimeIndex &&other) noexcept = default;
IntervalRuntimeIndex::~IntervalRuntimeIndex() = default;

IntervalRuntimeIndex::IntervalTree::IntervalTree() = default;
IntervalRuntimeIndex::IntervalTree::IntervalTree(const IntervalTree &other) : root(cloneIntervalNode(other.root.get())) {}
IntervalRuntimeIndex::IntervalTree &IntervalRuntimeIndex::IntervalTree::operator=(const IntervalTree &other)
{
    if (this != &other) {
        root = cloneIntervalNode(other.root.get());
    }
    return *this;
}
IntervalRuntimeIndex::IntervalTree::IntervalTree(IntervalTree &&other) noexcept = default;
IntervalRuntimeIndex::IntervalTree &IntervalRuntimeIndex::IntervalTree::operator=(IntervalTree &&other) noexcept = default;
IntervalRuntimeIndex::IntervalTree::~IntervalTree() = default;

void IntervalRuntimeIndex::IntervalTree::insert(IntervalEntry entry)
{
    insertIntervalNode(root, std::move(entry));
}

void IntervalRuntimeIndex::IntervalTree::erase(const IntervalEntry &entry)
{
    eraseIntervalNode(root, entry);
}

RuntimeItemIdSet IntervalRuntimeIndex::IntervalTree::query(const Value &start,
                                                           const Value &end,
                                                           const std::set<ItemId> &excludedIds) const
{
    RuntimeItemIdSet result;
    if (!valueLess(start, end)) {
        return result;
    }
    queryIntervalNode(root.get(), start, end, excludedIds, result);
    return result;
}

bool IntervalRuntimeIndex::IntervalTree::empty() const noexcept
{
    return !root;
}

void IntervalRuntimeIndex::add(const IntervalIndexDefinitionRecord &definition, const ItemSnapshot &snapshot)
{
    auto groupValues = keyValuesForColumns(snapshot, definition.groupBy);
    if (groupValues.size() != definition.groupBy.size()) {
        return;
    }
    auto entry = intervalEntryFor(definition, snapshot);
    if (entry.start.value.isNull() || entry.end.value.isNull() || !valueLess(entry.start.value, entry.end.value)) {
        return;
    }
    _groups[GroupKey {std::move(groupValues)}].insert(std::move(entry));
}

void IntervalRuntimeIndex::remove(const IntervalIndexDefinitionRecord &definition, const ItemSnapshot &snapshot)
{
    auto groupValues = keyValuesForColumns(snapshot, definition.groupBy);
    if (groupValues.size() != definition.groupBy.size()) {
        return;
    }
    auto groupIt = _groups.find(GroupKey {std::move(groupValues)});
    if (groupIt == _groups.end()) {
        return;
    }
    groupIt->second.erase(intervalEntryFor(definition, snapshot));
    if (groupIt->second.empty()) {
        _groups.erase(groupIt);
    }
}

std::vector<ItemId> IntervalRuntimeIndex::overlapping(const IntervalIndexDefinitionRecord &definition,
                                                      const ItemSnapshot &probe,
                                                      const std::set<ItemId> &excludedIds) const
{
    auto groupValues = keyValuesForColumns(probe, definition.groupBy);
    if (groupValues.size() != definition.groupBy.size()) {
        return {};
    }
    auto groupIt = _groups.find(GroupKey {std::move(groupValues)});
    if (groupIt == _groups.end()) {
        return {};
    }
    auto excluded = excludedIds;
    excluded.insert(probe.id);
    auto ids = groupIt->second.query(valueForColumn(probe, definition.start.columnId()),
                                     valueForColumn(probe, definition.end.columnId()),
                                     excluded);
    return std::vector<ItemId>(ids.begin(), ids.end());
}

RuntimeItemIdSet IntervalRuntimeIndex::query(const IntervalIndexDefinitionRecord &definition,
                                             const std::vector<Value> &groupKey,
                                             const Value &start,
                                             const Value &end) const
{
    auto groupValues = keyValuesFromValues(groupKey);
    if (groupValues.size() != definition.groupBy.size()) {
        return {};
    }
    auto groupIt = _groups.find(GroupKey {std::move(groupValues)});
    if (groupIt == _groups.end()) {
        return {};
    }
    return groupIt->second.query(start, end, {});
}

void RuntimeIndexStore::clear()
{
    _containerItems.clear();
    _scalarIndexes.clear();
    _rangeIndex.clear();
    _orderedIndexes.clear();
    _intervalIndexes.clear();
    _aggregateColumns.clear();
    _parentCounts.clear();
}

void RuntimeIndexStore::addItem(const SchemaDefinitionData &schemaDefinition, const ItemSnapshot &snapshot)
{
    _containerItems[snapshot.containerId].insert(snapshot.id);
    std::map<RuntimeIndexedFieldKey, Value> rangeValues;
    std::set<ColumnId> rangeColumnIds;
    auto addValue = [&](RuntimeIndexedFieldKey field, const Value &value) {
        _scalarIndexes[field].add(value, snapshot.id);
    };

    addValue(runtimeIdField(snapshot.containerId), Value(static_cast<std::uint64_t>(snapshot.id)));
    addValue(runtimeVariantField(snapshot.containerId),
             snapshot.variant ? Value(static_cast<std::uint64_t>(snapshot.variant->variantId())) : Value::null());

    const auto *container = findContainer(schemaDefinition, snapshot.containerId);
    if (!container) {
        return;
    }
    for (const auto &rangeIndex : container->rangeIndexes) {
        for (const auto &column : rangeIndex.columns) {
            rangeColumnIds.insert(column.columnId());
        }
    }
    for (const auto &orderedIndex : container->orderedIndexes) {
        _orderedIndexes[{snapshot.containerId, orderedIndex.handle.indexId()}].add(orderedIndex, snapshot);
    }
    for (const auto &intervalIndex : container->intervalIndexes) {
        _intervalIndexes[{snapshot.containerId, intervalIndex.handle.indexId()}].add(intervalIndex, snapshot);
    }
    for (const auto &relation : container->relations) {
        const auto groupValue = valueForColumn(snapshot, relation.info.column.columnId());
        auto &bucket = _parentCounts[snapshot.containerId][relation.info.column.columnId()][stableValueKey(groupValue)];
        bucket.groupValue = groupValue;
        ++bucket.count;
    }
    for (const auto &column : container->columns) {
        if (!columnIsIndexed(column)) {
            continue;
        }
        if (column.info.variantSpecific &&
            (!snapshot.variant || snapshot.variant->variantId() != column.variantId)) {
            continue;
        }
        const auto value = valueForColumn(snapshot, column.info.id);
        addValue(runtimeColumnField(snapshot.containerId, column.info.id), value);
        if (rangeColumnIds.find(column.info.id) != rangeColumnIds.end() && runtimeNumericValue(value)) {
            rangeValues[runtimeColumnField(snapshot.containerId, column.info.id)] = value;
        }
        const auto aggregateOptions = aggregateOptionsFor(column);
        if (aggregateEnabled(aggregateOptions)) {
            auto &aggregate = _aggregateColumns[{snapshot.containerId, column.info.id}];
            aggregate.options = aggregateOptions;
            addAggregateValue(aggregate.total, aggregateOptions, value);
            if (aggregateOptions.byParent) {
                for (const auto &relation : container->relations) {
                    const auto groupValue = valueForColumn(snapshot, relation.info.column.columnId());
                    auto &group = aggregate.byParent[relation.info.column.columnId()][stableValueKey(groupValue)];
                    group.groupValue = groupValue;
                    addAggregateValue(group.bucket, aggregateOptions, value);
                }
            }
        }
    }
    _rangeIndex.add(snapshot.containerId, snapshot.id, std::move(rangeValues));
}

void RuntimeIndexStore::removeItem(const SchemaDefinitionData &schemaDefinition, const ItemSnapshot &snapshot)
{
    auto containerIt = _containerItems.find(snapshot.containerId);
    if (containerIt != _containerItems.end()) {
        containerIt->second.erase(snapshot.id);
        if (containerIt->second.empty()) {
            _containerItems.erase(containerIt);
        }
    }

    auto removeValue = [&](RuntimeIndexedFieldKey field, const Value &value) {
        auto indexIt = _scalarIndexes.find(field);
        if (indexIt == _scalarIndexes.end()) {
            return;
        }
        indexIt->second.remove(value, snapshot.id);
        if (indexIt->second.empty()) {
            _scalarIndexes.erase(indexIt);
        }
    };

    removeValue(runtimeIdField(snapshot.containerId), Value(static_cast<std::uint64_t>(snapshot.id)));
    removeValue(runtimeVariantField(snapshot.containerId),
                snapshot.variant ? Value(static_cast<std::uint64_t>(snapshot.variant->variantId())) : Value::null());

    const auto *container = findContainer(schemaDefinition, snapshot.containerId);
    if (container) {
        for (const auto &orderedIndex : container->orderedIndexes) {
            auto indexIt = _orderedIndexes.find({snapshot.containerId, orderedIndex.handle.indexId()});
            if (indexIt != _orderedIndexes.end()) {
                indexIt->second.remove(orderedIndex, snapshot);
            }
        }
        for (const auto &intervalIndex : container->intervalIndexes) {
            auto indexIt = _intervalIndexes.find({snapshot.containerId, intervalIndex.handle.indexId()});
            if (indexIt != _intervalIndexes.end()) {
                indexIt->second.remove(intervalIndex, snapshot);
            }
        }
        for (const auto &relation : container->relations) {
            const auto groupValue = valueForColumn(snapshot, relation.info.column.columnId());
            auto relationIt = _parentCounts.find(snapshot.containerId);
            if (relationIt != _parentCounts.end()) {
                auto columnIt = relationIt->second.find(relation.info.column.columnId());
                if (columnIt != relationIt->second.end()) {
                    auto bucketIt = columnIt->second.find(stableValueKey(groupValue));
                    if (bucketIt != columnIt->second.end()) {
                        if (bucketIt->second.count <= 1) {
                            columnIt->second.erase(bucketIt);
                        } else {
                            --bucketIt->second.count;
                        }
                    }
                    if (columnIt->second.empty()) {
                        relationIt->second.erase(columnIt);
                    }
                }
                if (relationIt->second.empty()) {
                    _parentCounts.erase(relationIt);
                }
            }
        }
        for (const auto &column : container->columns) {
            if (!columnIsIndexed(column)) {
                continue;
            }
            if (column.info.variantSpecific &&
                (!snapshot.variant || snapshot.variant->variantId() != column.variantId)) {
                continue;
            }
            const auto value = valueForColumn(snapshot, column.info.id);
            removeValue(runtimeColumnField(snapshot.containerId, column.info.id), value);
            const auto aggregateOptions = aggregateOptionsFor(column);
            if (aggregateEnabled(aggregateOptions)) {
                auto aggregateIt = _aggregateColumns.find({snapshot.containerId, column.info.id});
                if (aggregateIt != _aggregateColumns.end()) {
                    removeAggregateValue(aggregateIt->second.total, aggregateIt->second.options, value);
                    if (aggregateIt->second.options.byParent) {
                        for (const auto &relation : container->relations) {
                            const auto groupValue = valueForColumn(snapshot, relation.info.column.columnId());
                            auto relationIt = aggregateIt->second.byParent.find(relation.info.column.columnId());
                            if (relationIt == aggregateIt->second.byParent.end()) {
                                continue;
                            }
                            auto groupIt = relationIt->second.find(stableValueKey(groupValue));
                            if (groupIt == relationIt->second.end()) {
                                continue;
                            }
                            removeAggregateValue(groupIt->second.bucket, aggregateIt->second.options, value);
                            if (groupIt->second.bucket.count == 0) {
                                relationIt->second.erase(groupIt);
                            }
                            if (relationIt->second.empty()) {
                                aggregateIt->second.byParent.erase(relationIt);
                            }
                        }
                    }
                    if (aggregateIt->second.total.count == 0) {
                        _aggregateColumns.erase(aggregateIt);
                    }
                }
            }
        }
    }
    _rangeIndex.remove(snapshot.containerId, snapshot.id);
}

const RuntimeItemIdSet &RuntimeIndexStore::containerItems(ContainerId containerId) const
{
    auto it = _containerItems.find(containerId);
    return it == _containerItems.end() ? emptySet() : it->second;
}

RuntimeItemIdSet RuntimeIndexStore::queryField(const RuntimeIndexedFieldKey &field,
                                               ComparisonOperator op,
                                               const Value &value) const
{
    auto it = _scalarIndexes.find(field);
    if (it == _scalarIndexes.end()) {
        return {};
    }
    return it->second.query(op, value);
}

RuntimeItemIdSet RuntimeIndexStore::queryRange(ContainerId containerId,
                                               const std::vector<RuntimeRangeConstraint> &constraints) const
{
    return _rangeIndex.query(containerId, constraints);
}

RuntimeItemIdSet RuntimeIndexStore::queryInterval(const IntervalIndexDefinitionRecord &definition,
                                                  const std::vector<Value> &groupKey,
                                                  const Value &start,
                                                  const Value &end) const
{
    auto it = _intervalIndexes.find({definition.handle.containerId(), definition.handle.indexId()});
    if (it == _intervalIndexes.end()) {
        return {};
    }
    return it->second.query(definition, groupKey, start, end);
}

void RuntimeIndexStore::orderedField(const RuntimeIndexedFieldKey &field,
                                     bool descending,
                                     const std::function<bool(ItemId)> &visitor) const
{
    auto it = _scalarIndexes.find(field);
    if (it == _scalarIndexes.end()) {
        return;
    }
    it->second.ordered(visitor, descending);
}

void RuntimeIndexStore::orderedIndex(const OrderedIndexDefinitionRecord &definition,
                                     const std::vector<Value> &groupKey,
                                     bool descending,
                                     const std::function<bool(ItemId)> &visitor) const
{
    auto it = _orderedIndexes.find({definition.handle.containerId(), definition.handle.indexId()});
    if (it == _orderedIndexes.end()) {
        return;
    }
    it->second.ordered(definition, groupKey, descending, visitor);
}

std::optional<ItemId> RuntimeIndexStore::previous(const OrderedIndexDefinitionRecord &definition,
                                                  const ItemSnapshot &probe,
                                                  const std::set<ItemId> &excludedIds) const
{
    auto it = _orderedIndexes.find({definition.handle.containerId(), definition.handle.indexId()});
    if (it == _orderedIndexes.end()) {
        return {};
    }
    return it->second.previous(definition, probe, excludedIds);
}

std::optional<ItemId> RuntimeIndexStore::next(const OrderedIndexDefinitionRecord &definition,
                                              const ItemSnapshot &probe,
                                              const std::set<ItemId> &excludedIds) const
{
    auto it = _orderedIndexes.find({definition.handle.containerId(), definition.handle.indexId()});
    if (it == _orderedIndexes.end()) {
        return {};
    }
    return it->second.next(definition, probe, excludedIds);
}

std::vector<ItemId> RuntimeIndexStore::overlapping(const IntervalIndexDefinitionRecord &definition,
                                                   const ItemSnapshot &probe,
                                                   const std::set<ItemId> &excludedIds) const
{
    auto it = _intervalIndexes.find({definition.handle.containerId(), definition.handle.indexId()});
    if (it == _intervalIndexes.end()) {
        return {};
    }
    return it->second.overlapping(definition, probe, excludedIds);
}

std::size_t RuntimeIndexStore::countField(const RuntimeIndexedFieldKey &field,
                                          ComparisonOperator op,
                                          const Value &value) const
{
    auto it = _scalarIndexes.find(field);
    return it == _scalarIndexes.end() ? 0 : it->second.count(op, value);
}

std::size_t RuntimeIndexStore::countParent(ContainerId containerId,
                                           ColumnId relationColumnId,
                                           const std::vector<Value> &parentValues) const
{
    auto containerIt = _parentCounts.find(containerId);
    if (containerIt == _parentCounts.end()) {
        return 0;
    }
    auto relationIt = containerIt->second.find(relationColumnId);
    if (relationIt == containerIt->second.end()) {
        return 0;
    }
    std::size_t result = 0;
    for (const auto &parentValue : parentValues) {
        auto bucketIt = relationIt->second.find(stableValueKey(parentValue));
        if (bucketIt != relationIt->second.end()) {
            result += bucketIt->second.count;
        }
    }
    return result;
}

std::optional<std::vector<AggregationResult>> RuntimeIndexStore::aggregate(const SchemaDefinitionData &schemaDefinition,
                                                                           ContainerId containerId,
                                                                           const AggregationSpec &spec) const
{
    (void)schemaDefinition;
    if (spec.kind == AggregateKind::Count) {
        if (!spec.groupBy) {
            return std::vector<AggregationResult> {
                AggregationResult {std::nullopt, Value(static_cast<std::uint64_t>(containerItems(containerId).size()))},
            };
        }
        if (spec.groupBy->kind() != FieldKind::Parent) {
            return std::nullopt;
        }
        const auto relationColumn = spec.groupBy->relation().column();
        auto containerIt = _parentCounts.find(containerId);
        if (containerIt == _parentCounts.end()) {
            return std::vector<AggregationResult> {};
        }
        auto relationIt = containerIt->second.find(relationColumn.columnId());
        if (relationIt == containerIt->second.end()) {
            return std::vector<AggregationResult> {};
        }
        std::vector<AggregationResult> result;
        result.reserve(relationIt->second.size());
        for (const auto &[key, bucket] : relationIt->second) {
            (void)key;
            result.push_back(AggregationResult {
                bucket.groupValue,
                Value(static_cast<std::uint64_t>(bucket.count)),
            });
        }
        return result;
    }

    if (!spec.valueField || spec.valueField->kind() != FieldKind::Column) {
        return std::nullopt;
    }
    const auto valueColumn = spec.valueField->column();
    if (valueColumn.containerId() != containerId) {
        return std::nullopt;
    }
    auto aggregateIt = _aggregateColumns.find({containerId, valueColumn.columnId()});
    if (aggregateIt == _aggregateColumns.end()) {
        return std::nullopt;
    }
    const auto &aggregate = aggregateIt->second;
    if (spec.kind == AggregateKind::Sum && !aggregate.options.sum) {
        return std::nullopt;
    }
    if ((spec.kind == AggregateKind::Minimum || spec.kind == AggregateKind::Maximum) && !aggregate.options.minMax) {
        return std::nullopt;
    }

    auto bucketValue = [&](const AggregateBucket &bucket) {
        if (spec.kind == AggregateKind::Sum) {
            return bucket.sum;
        }
        return minMaxValue(bucket, spec.kind);
    };

    if (!spec.groupBy) {
        return std::vector<AggregationResult> {
            AggregationResult {std::nullopt, bucketValue(aggregate.total)},
        };
    }
    if (spec.groupBy->kind() != FieldKind::Parent || !aggregate.options.byParent) {
        return std::nullopt;
    }
    const auto relationColumn = spec.groupBy->relation().column();
    auto relationIt = aggregate.byParent.find(relationColumn.columnId());
    if (relationIt == aggregate.byParent.end()) {
        return std::vector<AggregationResult> {};
    }
    std::vector<AggregationResult> result;
    result.reserve(relationIt->second.size());
    for (const auto &[key, group] : relationIt->second) {
        (void)key;
        result.push_back(AggregationResult {
            group.groupValue,
            bucketValue(group.bucket),
        });
    }
    return result;
}

std::optional<std::vector<AggregationResult>> RuntimeIndexStore::aggregateParentSelection(
    const SchemaDefinitionData &schemaDefinition,
    ContainerId containerId,
    ColumnId filterRelationColumnId,
    const std::vector<Value> &parentValues,
    const AggregationSpec &spec) const
{
    (void)schemaDefinition;
    if (spec.groupBy) {
        if (spec.groupBy->kind() != FieldKind::Parent) {
            return std::nullopt;
        }
        const auto groupRelationColumn = spec.groupBy->relation().column();
        if (groupRelationColumn.containerId() != containerId ||
            groupRelationColumn.columnId() != filterRelationColumnId) {
            return std::nullopt;
        }
    }

    auto pushCountRows = [&]() -> std::vector<AggregationResult> {
        std::vector<AggregationResult> result;
        auto containerIt = _parentCounts.find(containerId);
        if (containerIt == _parentCounts.end()) {
            return result;
        }
        auto relationIt = containerIt->second.find(filterRelationColumnId);
        if (relationIt == containerIt->second.end()) {
            return result;
        }
        result.reserve(parentValues.size());
        for (const auto &parentValue : parentValues) {
            auto bucketIt = relationIt->second.find(stableValueKey(parentValue));
            if (bucketIt == relationIt->second.end()) {
                continue;
            }
            result.push_back(AggregationResult {
                bucketIt->second.groupValue,
                Value(static_cast<std::uint64_t>(bucketIt->second.count)),
            });
        }
        return result;
    };

    if (spec.kind == AggregateKind::Count) {
        if (spec.groupBy) {
            return pushCountRows();
        }
        const auto total = countParent(containerId, filterRelationColumnId, parentValues);
        if (total == 0) {
            return std::nullopt;
        }
        return std::vector<AggregationResult> {
            AggregationResult {std::nullopt, Value(static_cast<std::uint64_t>(total))},
        };
    }

    if (!spec.valueField || spec.valueField->kind() != FieldKind::Column) {
        return std::nullopt;
    }
    const auto valueColumn = spec.valueField->column();
    if (valueColumn.containerId() != containerId) {
        return std::nullopt;
    }
    auto aggregateIt = _aggregateColumns.find({containerId, valueColumn.columnId()});
    if (aggregateIt == _aggregateColumns.end()) {
        return std::nullopt;
    }
    const auto &aggregate = aggregateIt->second;
    if (!aggregate.options.byParent) {
        return std::nullopt;
    }
    if (spec.kind == AggregateKind::Sum && !aggregate.options.sum) {
        return std::nullopt;
    }
    if ((spec.kind == AggregateKind::Minimum || spec.kind == AggregateKind::Maximum) && !aggregate.options.minMax) {
        return std::nullopt;
    }
    auto relationIt = aggregate.byParent.find(filterRelationColumnId);
    if (relationIt == aggregate.byParent.end()) {
        return std::nullopt;
    }

    if (spec.groupBy) {
        std::vector<AggregationResult> result;
        result.reserve(parentValues.size());
        for (const auto &parentValue : parentValues) {
            auto groupIt = relationIt->second.find(stableValueKey(parentValue));
            if (groupIt == relationIt->second.end()) {
                continue;
            }
            result.push_back(AggregationResult {
                groupIt->second.groupValue,
                aggregateBucketValue(groupIt->second.bucket, spec.kind),
            });
        }
        return result;
    }

    AggregateBucket merged;
    for (const auto &parentValue : parentValues) {
        auto groupIt = relationIt->second.find(stableValueKey(parentValue));
        if (groupIt != relationIt->second.end()) {
            mergeAggregateBucket(merged, groupIt->second.bucket, aggregate.options);
        }
    }
    if (merged.count == 0) {
        return std::nullopt;
    }
    return std::vector<AggregationResult> {
        AggregationResult {std::nullopt, aggregateBucketValue(merged, spec.kind)},
    };
}

const RuntimeItemIdSet &RuntimeIndexStore::emptySet()
{
    static const RuntimeItemIdSet empty;
    return empty;
}

bool runtimeValueSupportsOrdering(ValueType type) noexcept
{
    return type == ValueType::Int64 || type == ValueType::UInt64 || type == ValueType::Double;
}

bool runtimeValueSupportsOnlyEquality(ValueType type) noexcept
{
    return type == ValueType::Null || type == ValueType::Bool ||
           type == ValueType::String || type == ValueType::Binary;
}

std::optional<long double> runtimeNumericValue(const Value &value)
{
    switch (value.type()) {
        case ValueType::Int64:
            return static_cast<long double>(value.asInt64());
        case ValueType::UInt64:
            return static_cast<long double>(value.asUInt64());
        case ValueType::Double:
            if (std::isfinite(value.asDouble())) {
                return static_cast<long double>(value.asDouble());
            }
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

RuntimeIndexedFieldKey runtimeIdField(ContainerId containerId)
{
    return RuntimeIndexedFieldKey {containerId, RuntimeIndexedFieldKind::Id, 0};
}

RuntimeIndexedFieldKey runtimeVariantField(ContainerId containerId)
{
    return RuntimeIndexedFieldKey {containerId, RuntimeIndexedFieldKind::Variant, 0};
}

RuntimeIndexedFieldKey runtimeColumnField(ContainerId containerId, ColumnId columnId)
{
    return RuntimeIndexedFieldKey {containerId, RuntimeIndexedFieldKind::Column, columnId};
}

} // namespace dini
