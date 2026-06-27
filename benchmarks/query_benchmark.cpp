#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <benchmark/benchmark.h>

#include <dini/engine.h>
#include <dini/errors.h>

namespace {

using namespace dini;

constexpr int smallSize = 1'000;
constexpr int mediumSize = 10'000;
constexpr int largeSize = 100'000;

struct BenchmarkData {
    EngineSchema schema;
    TableHandle parentTable;
    ColumnHandle parentGroup;
    TableHandle itemTable;
    RelationHandle itemParent;
    ColumnHandle x;
    ColumnHandle y;
    ColumnHandle score;
    ColumnHandle flag;
    ColumnHandle name;
    ColumnHandle blob;
    ColumnHandle optionalValue;
    std::unique_ptr<DocumentEngine> engine;
    std::vector<ItemId> parentIds;
    std::vector<ItemId> itemIds;
};

struct DataOptions {
    bool rangeIndex = false;
    bool aggregateHints = false;

    friend bool operator<(const DataOptions &lhs, const DataOptions &rhs) noexcept
    {
        return std::tie(lhs.rangeIndex, lhs.aggregateHints) <
               std::tie(rhs.rangeIndex, rhs.aggregateHints);
    }
};

ColumnDefinition column(std::string name, ValueType type, IndexKind index = IndexKind::Normal)
{
    Value defaultValue;
    switch (type) {
        case ValueType::Bool:
            defaultValue = Value(false);
            break;
        case ValueType::Int64:
            defaultValue = Value(std::int64_t {0});
            break;
        case ValueType::UInt64:
            defaultValue = Value(std::uint64_t {0});
            break;
        case ValueType::Double:
            defaultValue = Value(0.0);
            break;
        case ValueType::String:
            defaultValue = Value("");
            break;
        case ValueType::Binary:
            defaultValue = Value(ByteArray {});
            break;
        case ValueType::Null:
            defaultValue = Value::null();
            break;
    }
    return ColumnDefinition {
        .debugName = std::move(name),
        .type = type,
        .index = index,
        .defaultValue = defaultValue,
        .nullable = false,
    };
}

ColumnDefinition aggregateColumn(std::string name, ValueType type, bool aggregateHints)
{
    auto definition = column(std::move(name), type);
    if (aggregateHints) {
        definition.aggregateIndex = AggregateIndexOptions {
            .sum = true,
            .minMax = true,
            .byParent = true,
        };
    }
    return definition;
}

BenchmarkData makeData(int rowCount, DataOptions options = {})
{
    BenchmarkData data;

    SchemaBuilder builder;
    auto parentBuilder = builder.createTable("Parent");
    data.parentTable = parentBuilder.handle();
    data.parentGroup = parentBuilder.addColumn(column("group", ValueType::Int64));

    auto itemBuilder = builder.createTable("Item");
    data.itemTable = itemBuilder.handle();
    data.itemParent = itemBuilder.addAssociation(AssociationDefinition {
        .debugName = "parent",
        .target = data.parentTable,
    });
    data.x = itemBuilder.addColumn(aggregateColumn("x", ValueType::Int64, options.aggregateHints));
    data.y = itemBuilder.addColumn(column("y", ValueType::UInt64));
    data.score = itemBuilder.addColumn(aggregateColumn("score", ValueType::Double, options.aggregateHints));
    data.flag = itemBuilder.addColumn(column("flag", ValueType::Bool));
    data.name = itemBuilder.addColumn(column("name", ValueType::String));
    data.blob = itemBuilder.addColumn(column("blob", ValueType::Binary));
    auto nullableColumn = column("optional", ValueType::Int64);
    nullableColumn.defaultValue = Value::null();
    nullableColumn.nullable = true;
    data.optionalValue = itemBuilder.addColumn(nullableColumn);
    if (options.rangeIndex) {
        itemBuilder.addRangeIndex(RangeIndexDefinition {
            .debugName = "xy",
            .columns = {data.x, data.y},
        });
    }

    data.schema = builder.freeze();
    data.engine = std::make_unique<DocumentEngine>(data.schema);
    data.parentIds.reserve(10);
    data.itemIds.reserve(static_cast<std::size_t>(rowCount));

    auto txn = data.engine->beginTransaction();
    for (int i = 0; i < 10; ++i) {
        data.parentIds.push_back(txn.insert(data.parentTable, {
            ColumnValue {.column = data.parentGroup, .value = Value(std::int64_t {i})},
        }));
    }
    for (int i = 0; i < rowCount; ++i) {
        const auto parentId = data.parentIds[static_cast<std::size_t>(i % static_cast<int>(data.parentIds.size()))];
        const auto name = std::string("name-") + std::to_string(i % 97);
        const auto blob = ByteArray {
            static_cast<std::uint8_t>(i & 0xff),
            static_cast<std::uint8_t>((i / 7) & 0xff),
            static_cast<std::uint8_t>((i / 17) & 0xff),
        };
        data.itemIds.push_back(txn.insert(data.itemTable, {
            ColumnValue {.column = data.itemParent.column(), .value = Value(static_cast<std::uint64_t>(parentId))},
            ColumnValue {.column = data.x, .value = Value(std::int64_t {i})},
            ColumnValue {.column = data.y, .value = Value(static_cast<std::uint64_t>(rowCount - i))},
            ColumnValue {.column = data.score, .value = Value(static_cast<double>(i) * 0.5)},
            ColumnValue {.column = data.flag, .value = Value((i % 2) == 0)},
            ColumnValue {.column = data.name, .value = Value(name)},
            ColumnValue {.column = data.blob, .value = Value(blob)},
            ColumnValue {.column = data.optionalValue, .value = (i % 5 == 0) ? Value::null() : Value(std::int64_t {i % 31})},
        }));
    }
    txn.commit();
    return data;
}

const BenchmarkData &dataForSize(int rowCount, DataOptions options = {})
{
    static std::map<std::tuple<int, DataOptions>, std::unique_ptr<BenchmarkData>> cache;
    const auto key = std::make_tuple(rowCount, options);
    auto it = cache.find(key);
    if (it == cache.end()) {
        it = cache.emplace(key, std::make_unique<BenchmarkData>(makeData(rowCount, options))).first;
    }
    return *it->second;
}

void addSizes(benchmark::internal::Benchmark *bench)
{
    bench->Arg(smallSize)->Arg(mediumSize)->Arg(largeSize);
}

QuerySpec singleFilter(FieldRef field, ComparisonOperator op, Value value)
{
    return QuerySpec {
        .filter = FilterExpression(Filter(std::move(field), op, std::move(value))),
    };
}

std::size_t linearCountXGreaterAndYLess(const BenchmarkData &data, int lower, std::uint64_t upper)
{
    std::size_t count = 0;
    for (const auto id : data.itemIds) {
        const auto x = data.engine->read(id, data.x).asInt64();
        const auto y = data.engine->read(id, data.y).asUInt64();
        if (x > lower && y < upper) {
            ++count;
        }
    }
    return count;
}

void BM_QueryIdEqual(benchmark::State &state)
{
    const auto &data = dataForSize(static_cast<int>(state.range(0)));
    const auto targetId = data.itemIds[data.itemIds.size() / 2];
    const auto spec = singleFilter(FieldRef::id(), ComparisonOperator::Equal, Value(static_cast<std::uint64_t>(targetId)));
    for (auto _ : state) {
        auto count = data.engine->query(data.itemTable, spec).count();
        benchmark::DoNotOptimize(count);
    }
    state.SetItemsProcessed(state.iterations());
}

void BM_QueryParentEqual(benchmark::State &state)
{
    const auto &data = dataForSize(static_cast<int>(state.range(0)));
    const auto parent = data.parentIds[3];
    const auto spec = singleFilter(FieldRef::parent(data.itemParent),
                                   ComparisonOperator::Equal,
                                   Value(static_cast<std::uint64_t>(parent)));
    for (auto _ : state) {
        auto count = data.engine->query(data.itemTable, spec).count();
        benchmark::DoNotOptimize(count);
    }
}

void BM_QueryNumericSingleGreater(benchmark::State &state)
{
    const auto rowCount = static_cast<int>(state.range(0));
    const auto &data = dataForSize(rowCount);
    const auto spec = singleFilter(FieldRef::column(data.x),
                                   ComparisonOperator::Greater,
                                   Value(std::int64_t {rowCount - 10}));
    for (auto _ : state) {
        auto count = data.engine->query(data.itemTable, spec).count();
        benchmark::DoNotOptimize(count);
    }
}

QuerySpec numericAndSelectiveSpec(const BenchmarkData &data, int rowCount)
{
    return QuerySpec {
        .filter = FilterExpression::all({
            FilterExpression(Filter(FieldRef::column(data.x),
                                    ComparisonOperator::Greater,
                                    Value(std::int64_t {rowCount - 100}))),
            FilterExpression(Filter(FieldRef::column(data.y),
                                    ComparisonOperator::Less,
                                    Value(std::uint64_t {100}))),
        }),
    };
}

void BM_QueryNumericAndSelectiveScalarFallback(benchmark::State &state)
{
    const auto rowCount = static_cast<int>(state.range(0));
    const auto &data = dataForSize(rowCount, DataOptions {.rangeIndex = false});
    const auto spec = numericAndSelectiveSpec(data, rowCount);
    for (auto _ : state) {
        auto count = data.engine->query(data.itemTable, spec).count();
        benchmark::DoNotOptimize(count);
    }
}

void BM_QueryNumericAndSelectiveRangeIndex(benchmark::State &state)
{
    const auto rowCount = static_cast<int>(state.range(0));
    const auto &data = dataForSize(rowCount, DataOptions {.rangeIndex = true});
    const auto spec = numericAndSelectiveSpec(data, rowCount);
    for (auto _ : state) {
        auto count = data.engine->query(data.itemTable, spec).count();
        benchmark::DoNotOptimize(count);
    }
}

void BM_BaselineLinearNumericAndSelective(benchmark::State &state)
{
    const auto rowCount = static_cast<int>(state.range(0));
    const auto &data = dataForSize(rowCount);
    for (auto _ : state) {
        auto count = linearCountXGreaterAndYLess(data, rowCount - 100, 100);
        benchmark::DoNotOptimize(count);
    }
}

void BM_QueryNumericOr(benchmark::State &state)
{
    const auto rowCount = static_cast<int>(state.range(0));
    const auto &data = dataForSize(rowCount);
    const auto spec = QuerySpec {
        .filter = FilterExpression::any({
            FilterExpression(Filter(FieldRef::column(data.x),
                                    ComparisonOperator::Less,
                                    Value(std::int64_t {10}))),
            FilterExpression(Filter(FieldRef::column(data.y),
                                    ComparisonOperator::Less,
                                    Value(std::uint64_t {10}))),
        }),
    };
    for (auto _ : state) {
        auto count = data.engine->query(data.itemTable, spec).count();
        benchmark::DoNotOptimize(count);
    }
}

void BM_QueryNegatedRange(benchmark::State &state)
{
    const auto rowCount = static_cast<int>(state.range(0));
    const auto &data = dataForSize(rowCount);
    const auto spec = QuerySpec {
        .filter = FilterExpression::negate(FilterExpression(Filter(FieldRef::column(data.x),
                                                                   ComparisonOperator::Less,
                                                                   Value(std::int64_t {rowCount - 10})))),
    };
    for (auto _ : state) {
        auto count = data.engine->query(data.itemTable, spec).count();
        benchmark::DoNotOptimize(count);
    }
}

void BM_QueryStringEqual(benchmark::State &state)
{
    const auto &data = dataForSize(static_cast<int>(state.range(0)));
    const auto spec = singleFilter(FieldRef::column(data.name),
                                   ComparisonOperator::Equal,
                                   Value("name-42"));
    for (auto _ : state) {
        auto count = data.engine->query(data.itemTable, spec).count();
        benchmark::DoNotOptimize(count);
    }
}

void BM_QueryBoolNotEqual(benchmark::State &state)
{
    const auto &data = dataForSize(static_cast<int>(state.range(0)));
    const auto spec = singleFilter(FieldRef::column(data.flag),
                                   ComparisonOperator::NotEqual,
                                   Value(false));
    for (auto _ : state) {
        auto count = data.engine->query(data.itemTable, spec).count();
        benchmark::DoNotOptimize(count);
    }
}

void BM_QueryBinaryEqual(benchmark::State &state)
{
    const auto rowCount = static_cast<int>(state.range(0));
    const auto &data = dataForSize(rowCount);
    const int target = rowCount / 2;
    const ByteArray blob {
        static_cast<std::uint8_t>(target & 0xff),
        static_cast<std::uint8_t>((target / 7) & 0xff),
        static_cast<std::uint8_t>((target / 17) & 0xff),
    };
    const auto spec = singleFilter(FieldRef::column(data.blob),
                                   ComparisonOperator::Equal,
                                   Value(blob));
    for (auto _ : state) {
        auto count = data.engine->query(data.itemTable, spec).count();
        benchmark::DoNotOptimize(count);
    }
}

void BM_QueryNullEqual(benchmark::State &state)
{
    const auto &data = dataForSize(static_cast<int>(state.range(0)));
    const auto spec = singleFilter(FieldRef::column(data.optionalValue),
                                   ComparisonOperator::Equal,
                                   Value::null());
    for (auto _ : state) {
        auto count = data.engine->query(data.itemTable, spec).count();
        benchmark::DoNotOptimize(count);
    }
}

void BM_QueryLimitOne(benchmark::State &state)
{
    const auto rowCount = static_cast<int>(state.range(0));
    const auto &data = dataForSize(rowCount);
    const auto spec = singleFilter(FieldRef::column(data.x),
                                   ComparisonOperator::Greater,
                                   Value(std::int64_t {rowCount - 10}));
    for (auto _ : state) {
        auto count = data.engine->query(data.itemTable, spec).limit(1).count();
        benchmark::DoNotOptimize(count);
    }
}

void BM_QuerySortSingleKeyLimitOne(benchmark::State &state)
{
    const auto &data = dataForSize(static_cast<int>(state.range(0)));
    const auto spec = QuerySpec {
        .sortKeys = {
            SortKey {.field = FieldRef::column(data.x), .direction = SortDirection::Descending},
        },
    };
    for (auto _ : state) {
        auto count = data.engine->query(data.itemTable, spec).limit(1).count();
        benchmark::DoNotOptimize(count);
    }
}

void BM_AggregateCount(benchmark::State &state)
{
    const auto &data = dataForSize(static_cast<int>(state.range(0)));
    const auto aggregate = AggregationSpec {.kind = AggregateKind::Count};
    for (auto _ : state) {
        auto result = data.engine->view(data.itemTable).aggregate(aggregate).toVector();
        benchmark::DoNotOptimize(result);
    }
}

void BM_AggregateSumFallback(benchmark::State &state)
{
    const auto &data = dataForSize(static_cast<int>(state.range(0)),
                                   DataOptions {.aggregateHints = false});
    const auto aggregate = AggregationSpec {
        .kind = AggregateKind::Sum,
        .valueField = FieldRef::column(data.x),
    };
    for (auto _ : state) {
        auto result = data.engine->view(data.itemTable).aggregate(aggregate).toVector();
        benchmark::DoNotOptimize(result);
    }
}

void BM_AggregateSumHinted(benchmark::State &state)
{
    const auto &data = dataForSize(static_cast<int>(state.range(0)),
                                   DataOptions {.aggregateHints = true});
    const auto aggregate = AggregationSpec {
        .kind = AggregateKind::Sum,
        .valueField = FieldRef::column(data.x),
    };
    for (auto _ : state) {
        auto result = data.engine->view(data.itemTable).aggregate(aggregate).toVector();
        benchmark::DoNotOptimize(result);
    }
}

void BM_AggregateMaxFallback(benchmark::State &state)
{
    const auto &data = dataForSize(static_cast<int>(state.range(0)),
                                   DataOptions {.aggregateHints = false});
    const auto aggregate = AggregationSpec {
        .kind = AggregateKind::Maximum,
        .valueField = FieldRef::column(data.x),
    };
    for (auto _ : state) {
        auto result = data.engine->view(data.itemTable).aggregate(aggregate).toVector();
        benchmark::DoNotOptimize(result);
    }
}

void BM_AggregateMaxHinted(benchmark::State &state)
{
    const auto &data = dataForSize(static_cast<int>(state.range(0)),
                                   DataOptions {.aggregateHints = true});
    const auto aggregate = AggregationSpec {
        .kind = AggregateKind::Maximum,
        .valueField = FieldRef::column(data.x),
    };
    for (auto _ : state) {
        auto result = data.engine->view(data.itemTable).aggregate(aggregate).toVector();
        benchmark::DoNotOptimize(result);
    }
}

void BM_AggregateCountByParent(benchmark::State &state)
{
    const auto &data = dataForSize(static_cast<int>(state.range(0)));
    const auto aggregate = AggregationSpec {
        .kind = AggregateKind::Count,
        .groupBy = FieldRef::parent(data.itemParent),
    };
    for (auto _ : state) {
        auto result = data.engine->view(data.itemTable).aggregate(aggregate).toVector();
        benchmark::DoNotOptimize(result);
    }
}

void BM_AggregateQueryParentThenCount(benchmark::State &state)
{
    const auto &data = dataForSize(static_cast<int>(state.range(0)),
                                   DataOptions {.aggregateHints = true});
    const auto aggregate = AggregationSpec {
        .kind = AggregateKind::Count,
        .groupBy = FieldRef::parent(data.itemParent),
    };
    const auto spec = QuerySpec {
        .filter = FilterExpression::any({
            FilterExpression(Filter(FieldRef::parent(data.itemParent), ComparisonOperator::Equal, Value(data.parentIds[3]))),
            FilterExpression(Filter(FieldRef::parent(data.itemParent), ComparisonOperator::Equal, Value(data.parentIds[4]))),
        })
    };
    for (auto _ : state) {
        auto result = data.engine->query(data.itemTable, spec).aggregate(aggregate).toVector();
        benchmark::DoNotOptimize(result);
    }
}

void BM_AggregateSumByParentFallback(benchmark::State &state)
{
    const auto &data = dataForSize(static_cast<int>(state.range(0)),
                                   DataOptions {.aggregateHints = false});
    const auto aggregate = AggregationSpec {
        .kind = AggregateKind::Sum,
        .valueField = FieldRef::column(data.x),
        .groupBy = FieldRef::parent(data.itemParent),
    };
    for (auto _ : state) {
        auto result = data.engine->view(data.itemTable).aggregate(aggregate).toVector();
        benchmark::DoNotOptimize(result);
    }
}

void BM_AggregateSumByParentHinted(benchmark::State &state)
{
    const auto &data = dataForSize(static_cast<int>(state.range(0)),
                                   DataOptions {.aggregateHints = true});
    const auto aggregate = AggregationSpec {
        .kind = AggregateKind::Sum,
        .valueField = FieldRef::column(data.x),
        .groupBy = FieldRef::parent(data.itemParent),
    };
    for (auto _ : state) {
        auto result = data.engine->view(data.itemTable).aggregate(aggregate).toVector();
        benchmark::DoNotOptimize(result);
    }
}

void BM_AggregateQueryParentThenSumHinted(benchmark::State &state)
{
    const auto &data = dataForSize(static_cast<int>(state.range(0)),
                                   DataOptions {.aggregateHints = true});
    const auto aggregate = AggregationSpec {
        .kind = AggregateKind::Sum,
        .valueField = FieldRef::column(data.x),
        .groupBy = FieldRef::parent(data.itemParent),
    };
    const auto spec = QuerySpec {
        .filter = FilterExpression::any({
            FilterExpression(Filter(FieldRef::parent(data.itemParent), ComparisonOperator::Equal, Value(data.parentIds[3]))),
            FilterExpression(Filter(FieldRef::parent(data.itemParent), ComparisonOperator::Equal, Value(data.parentIds[4]))),
        })
    };
    for (auto _ : state) {
        auto result = data.engine->query(data.itemTable, spec).aggregate(aggregate).toVector();
        benchmark::DoNotOptimize(result);
    }
}

void BM_AggregateMaxByParentFallback(benchmark::State &state)
{
    const auto &data = dataForSize(static_cast<int>(state.range(0)),
                                   DataOptions {.aggregateHints = false});
    const auto aggregate = AggregationSpec {
        .kind = AggregateKind::Maximum,
        .valueField = FieldRef::column(data.score),
        .groupBy = FieldRef::parent(data.itemParent),
    };
    for (auto _ : state) {
        auto result = data.engine->view(data.itemTable).aggregate(aggregate).toVector();
        benchmark::DoNotOptimize(result);
    }
}

void BM_AggregateMaxByParentHinted(benchmark::State &state)
{
    const auto &data = dataForSize(static_cast<int>(state.range(0)),
                                   DataOptions {.aggregateHints = true});
    const auto aggregate = AggregationSpec {
        .kind = AggregateKind::Maximum,
        .valueField = FieldRef::column(data.score),
        .groupBy = FieldRef::parent(data.itemParent),
    };
    for (auto _ : state) {
        auto result = data.engine->view(data.itemTable).aggregate(aggregate).toVector();
        benchmark::DoNotOptimize(result);
    }
}

BENCHMARK(BM_QueryIdEqual)->Apply(addSizes);
BENCHMARK(BM_QueryParentEqual)->Apply(addSizes);
BENCHMARK(BM_QueryNumericSingleGreater)->Apply(addSizes);
BENCHMARK(BM_QueryNumericAndSelectiveScalarFallback)->Apply(addSizes);
BENCHMARK(BM_QueryNumericAndSelectiveRangeIndex)->Apply(addSizes);
BENCHMARK(BM_BaselineLinearNumericAndSelective)->Apply(addSizes);
BENCHMARK(BM_QueryNumericOr)->Apply(addSizes);
BENCHMARK(BM_QueryNegatedRange)->Apply(addSizes);
BENCHMARK(BM_QueryStringEqual)->Apply(addSizes);
BENCHMARK(BM_QueryBoolNotEqual)->Apply(addSizes);
BENCHMARK(BM_QueryBinaryEqual)->Apply(addSizes);
BENCHMARK(BM_QueryNullEqual)->Apply(addSizes);
BENCHMARK(BM_QueryLimitOne)->Apply(addSizes);
BENCHMARK(BM_QuerySortSingleKeyLimitOne)->Apply(addSizes);
BENCHMARK(BM_AggregateCount)->Apply(addSizes);
BENCHMARK(BM_AggregateSumFallback)->Apply(addSizes);
BENCHMARK(BM_AggregateSumHinted)->Apply(addSizes);
BENCHMARK(BM_AggregateMaxFallback)->Apply(addSizes);
BENCHMARK(BM_AggregateMaxHinted)->Apply(addSizes);
BENCHMARK(BM_AggregateCountByParent)->Apply(addSizes);
BENCHMARK(BM_AggregateQueryParentThenCount)->Apply(addSizes);
BENCHMARK(BM_AggregateSumByParentFallback)->Apply(addSizes);
BENCHMARK(BM_AggregateSumByParentHinted)->Apply(addSizes);
BENCHMARK(BM_AggregateQueryParentThenSumHinted)->Apply(addSizes);
BENCHMARK(BM_AggregateMaxByParentFallback)->Apply(addSizes);
BENCHMARK(BM_AggregateMaxByParentHinted)->Apply(addSizes);

} // namespace
