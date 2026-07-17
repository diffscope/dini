#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include <dini/engine.h>

namespace {

using namespace dini;

constexpr std::size_t rowCount = 4;
constexpr std::size_t columnCount = 4;

struct BenchmarkData {
    EngineSchema schema;
    TableHandle table;
    std::array<ColumnHandle, columnCount> columns;
    std::unique_ptr<DocumentEngine> engine;
    std::array<ItemId, rowCount> itemIds;
};

struct UpdateTarget {
    std::size_t row = 0;
    std::size_t column = 0;
};

ColumnDefinition integerColumn(std::string debugName)
{
    return ColumnDefinition {
        .debugName = std::move(debugName),
        .type = ValueType::Int64,
        .defaultValue = Value(std::int64_t {0}),
        .nullable = false,
    };
}

BenchmarkData makeBenchmarkData()
{
    BenchmarkData data;

    SchemaBuilder builder;
    auto tableBuilder = builder.createTable("ChangeSetMergeBenchmark");
    data.table = tableBuilder.handle();
    for (std::size_t column = 0; column < columnCount; ++column) {
        data.columns[column] = tableBuilder.addColumn(
            integerColumn("value-" + std::to_string(column)));
    }

    data.schema = builder.freeze();
    data.engine = std::make_unique<DocumentEngine>(data.schema);

    auto seed = data.engine->beginTransaction(TransactionOptions {.undoable = false});
    for (std::size_t row = 0; row < rowCount; ++row) {
        std::vector<ColumnValue> values;
        values.reserve(columnCount);
        for (const auto &column : data.columns) {
            values.push_back(ColumnValue {
                .column = column,
                .value = Value(std::int64_t {0}),
            });
        }
        data.itemIds[row] = seed.insert(data.table, std::move(values));
    }
    seed.commit();

    return data;
}

std::vector<UpdateTarget> makeUpdateTargets(std::size_t count)
{
    // A fixed seed keeps every batching variant for the same total update count
    // comparable while still distributing updates across all 16 table cells.
    std::mt19937 random(0xD1A1u);
    std::uniform_int_distribution<std::size_t> rowDistribution(0, rowCount - 1);
    std::uniform_int_distribution<std::size_t> columnDistribution(0, columnCount - 1);

    std::vector<UpdateTarget> targets;
    targets.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        targets.push_back(UpdateTarget {
            .row = rowDistribution(random),
            .column = columnDistribution(random),
        });
    }
    return targets;
}

void BM_ChangeSetMergeTransactionBatching(benchmark::State &state)
{
    const auto totalUpdates = static_cast<std::size_t>(state.range(0));
    const auto updatesPerTransaction = static_cast<std::size_t>(state.range(1));
    if (updatesPerTransaction == 0 || totalUpdates % updatesPerTransaction != 0) {
        state.SkipWithError("total updates must be divisible by updates per transaction");
        return;
    }

    state.PauseTiming();
    auto data = makeBenchmarkData();
    const auto targets = makeUpdateTargets(totalUpdates);
    const auto transactionCount = totalUpdates / updatesPerTransaction;
    std::int64_t nextValue = 1'000'000;
    state.ResumeTiming();

    for (auto _ : state) {
        std::size_t updateIndex = 0;
        for (std::size_t transactionIndex = 0;
             transactionIndex < transactionCount;
             ++transactionIndex) {
            auto transaction = data.engine->beginTransaction(
                TransactionOptions {.undoable = false});

            for (std::size_t update = 0; update < updatesPerTransaction; ++update) {
                const auto &target = targets[updateIndex++];
                transaction.update(data.itemIds[target.row],
                                   data.columns[target.column],
                                   Value(nextValue++));
            }

            auto result = transaction.commit();
            benchmark::DoNotOptimize(result.changeSet.operations().size());
        }
        benchmark::ClobberMemory();
    }

    state.PauseTiming();
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(totalUpdates));
}

BENCHMARK(BM_ChangeSetMergeTransactionBatching)
    ->Args({10, 1})
    ->Iterations(10'000)
    ->ArgNames({"total_updates", "updates_per_transaction"});
BENCHMARK(BM_ChangeSetMergeTransactionBatching)
    ->Args({10, 10})
    ->Iterations(10'000)
    ->ArgNames({"total_updates", "updates_per_transaction"});

BENCHMARK(BM_ChangeSetMergeTransactionBatching)
    ->Args({100, 1})
    ->Iterations(1'000)
    ->ArgNames({"total_updates", "updates_per_transaction"});
BENCHMARK(BM_ChangeSetMergeTransactionBatching)
    ->Args({100, 10})
    ->Iterations(1'000)
    ->ArgNames({"total_updates", "updates_per_transaction"});
BENCHMARK(BM_ChangeSetMergeTransactionBatching)
    ->Args({100, 100})
    ->Iterations(1'000)
    ->ArgNames({"total_updates", "updates_per_transaction"});

BENCHMARK(BM_ChangeSetMergeTransactionBatching)
    ->Args({1'000, 1})
    ->Iterations(100)
    ->ArgNames({"total_updates", "updates_per_transaction"});
BENCHMARK(BM_ChangeSetMergeTransactionBatching)
    ->Args({1'000, 10})
    ->Iterations(100)
    ->ArgNames({"total_updates", "updates_per_transaction"});
BENCHMARK(BM_ChangeSetMergeTransactionBatching)
    ->Args({1'000, 100})
    ->Iterations(100)
    ->ArgNames({"total_updates", "updates_per_transaction"});
BENCHMARK(BM_ChangeSetMergeTransactionBatching)
    ->Args({1'000, 1'000})
    ->Iterations(100)
    ->ArgNames({"total_updates", "updates_per_transaction"});

} // namespace
