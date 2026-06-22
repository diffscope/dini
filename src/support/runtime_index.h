#ifndef DINI_RUNTIME_INDEX_H
#define DINI_RUNTIME_INDEX_H

#include <functional>
#include <map>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <dini/change.h>
#include <dini/query.h>
#include <dini/schema.h>

#include "../schema_p.h"

namespace dini {

using RuntimeItemIdSet = std::set<ItemId>;

enum class RuntimeIndexedFieldKind {
    Id,
    Variant,
    Column,
};

struct RuntimeIndexedFieldKey {
    ContainerId containerId = 0;
    RuntimeIndexedFieldKind kind = RuntimeIndexedFieldKind::Column;
    std::uint32_t id = 0;

    friend bool operator<(const RuntimeIndexedFieldKey &lhs, const RuntimeIndexedFieldKey &rhs) noexcept;
    friend bool operator==(const RuntimeIndexedFieldKey &lhs, const RuntimeIndexedFieldKey &rhs) noexcept;
};

struct ComparableValueKey {
    Value value;

    ComparableValueKey() = default;
    explicit ComparableValueKey(Value value);

    friend bool operator<(const ComparableValueKey &lhs, const ComparableValueKey &rhs);
    friend bool operator==(const ComparableValueKey &lhs, const ComparableValueKey &rhs);
};

struct RuntimeRangeConstraint {
    RuntimeIndexedFieldKey field;
    ComparisonOperator op = ComparisonOperator::Equal;
    Value value;
};

class ScalarValueIndex {
public:
    void add(const Value &value, ItemId id);
    void remove(const Value &value, ItemId id);
    RuntimeItemIdSet query(ComparisonOperator op, const Value &value) const;
    void ordered(const std::function<bool(ItemId)> &visitor, bool descending = false) const;
    std::size_t count(ComparisonOperator op, const Value &value) const;
    bool empty() const noexcept;

private:
    std::map<ComparableValueKey, RuntimeItemIdSet> _buckets;
};

class EqualityValueIndex {
public:
    void add(const Value &value, ItemId id);
    void remove(const Value &value, ItemId id);
    RuntimeItemIdSet query(ComparisonOperator op, const Value &value, const RuntimeItemIdSet &universe) const;

private:
    std::unordered_map<std::string, RuntimeItemIdSet> _buckets;
};

class ContainerRangeIndex {
public:
    void clear();
    void add(ContainerId containerId, ItemId id, std::map<RuntimeIndexedFieldKey, Value> values);
    void remove(ContainerId containerId, ItemId id);
    RuntimeItemIdSet query(ContainerId containerId, const std::vector<RuntimeRangeConstraint> &constraints) const;

private:
    struct Point {
        ItemId id = 0;
        std::map<RuntimeIndexedFieldKey, long double> values;
    };
    struct Node {
        RuntimeItemIdSet ids;
        std::map<RuntimeIndexedFieldKey, std::pair<long double, long double>> bounds;
        std::unique_ptr<Node> left;
        std::unique_ptr<Node> right;
        std::vector<std::size_t> points;
    };
    struct ContainerData {
        std::map<ItemId, Point> pointById;
        mutable std::vector<Point> points;
        mutable std::vector<RuntimeIndexedFieldKey> dimensions;
        mutable std::unique_ptr<Node> root;
        mutable bool dirty = true;

        ContainerData() = default;
        ContainerData(const ContainerData &other) : pointById(other.pointById), dirty(true) {}
        ContainerData &operator=(const ContainerData &other)
        {
            if (this != &other) {
                pointById = other.pointById;
                points.clear();
                dimensions.clear();
                root.reset();
                dirty = true;
            }
            return *this;
        }
    };

    std::map<ContainerId, ContainerData> _containers;
};

class RuntimeIndexStore {
public:
    void clear();
    void addItem(const SchemaDefinitionData &schemaDefinition, const ItemSnapshot &snapshot);
    void removeItem(const SchemaDefinitionData &schemaDefinition, const ItemSnapshot &snapshot);

    const RuntimeItemIdSet &containerItems(ContainerId containerId) const;
    RuntimeItemIdSet queryField(const RuntimeIndexedFieldKey &field, ComparisonOperator op, const Value &value) const;
    RuntimeItemIdSet queryRange(ContainerId containerId, const std::vector<RuntimeRangeConstraint> &constraints) const;
    void orderedField(const RuntimeIndexedFieldKey &field,
                      bool descending,
                      const std::function<bool(ItemId)> &visitor) const;
    std::size_t countField(const RuntimeIndexedFieldKey &field, ComparisonOperator op, const Value &value) const;
    std::optional<std::vector<AggregationResult>> aggregate(const SchemaDefinitionData &schemaDefinition,
                                                            ContainerId containerId,
                                                            const AggregationSpec &spec) const;

    struct AggregateBucket {
        std::size_t count = 0;
        Value sum;
        std::map<ComparableValueKey, std::size_t> values;
    };
    struct GroupAggregateBucket {
        Value groupValue;
        AggregateBucket bucket;
    };
    struct AggregateColumnData {
        AggregateIndexOptions options;
        AggregateBucket total;
        std::map<ColumnId, std::map<std::string, GroupAggregateBucket>> byParent;
    };
    struct ParentCountBucket {
        Value groupValue;
        std::size_t count = 0;
    };

private:
    std::map<ContainerId, RuntimeItemIdSet> _containerItems;
    std::map<RuntimeIndexedFieldKey, ScalarValueIndex> _scalarIndexes;
    ContainerRangeIndex _rangeIndex;
    std::map<std::pair<ContainerId, ColumnId>, AggregateColumnData> _aggregateColumns;
    std::map<ContainerId, std::map<ColumnId, std::map<std::string, ParentCountBucket>>> _parentCounts;

    static const RuntimeItemIdSet &emptySet();
};

bool runtimeValueSupportsOrdering(ValueType type) noexcept;
bool runtimeValueSupportsOnlyEquality(ValueType type) noexcept;
std::optional<long double> runtimeNumericValue(const Value &value);
RuntimeIndexedFieldKey runtimeIdField(ContainerId containerId);
RuntimeIndexedFieldKey runtimeVariantField(ContainerId containerId);
RuntimeIndexedFieldKey runtimeColumnField(ContainerId containerId, ColumnId columnId);

} // namespace dini

#endif // DINI_RUNTIME_INDEX_H
