#include <cstdint>
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <dini/engine.h>
#include <dini/errors.h>

#include <gtest/gtest.h>

namespace {

using namespace dini;

ColumnDefinition stringColumn(const char *debugName, IndexKind index = IndexKind::None)
{
    return ColumnDefinition {
        .debugName = debugName,
        .type = ValueType::String,
        .index = index,
        .defaultValue = Value(""),
        .nullable = false,
    };
}

ColumnDefinition int64Column(const char *debugName, IndexKind index = IndexKind::None)
{
    return ColumnDefinition {
        .debugName = debugName,
        .type = ValueType::Int64,
        .index = index,
        .defaultValue = Value(std::int64_t {0}),
        .nullable = false,
    };
}

ColumnDefinition doubleColumn(const char *debugName, IndexKind index = IndexKind::None)
{
    return ColumnDefinition {
        .debugName = debugName,
        .type = ValueType::Double,
        .index = index,
        .defaultValue = Value(0.0),
        .nullable = false,
    };
}

ColumnDefinition uint64Column(const char *debugName, IndexKind index = IndexKind::None)
{
    return ColumnDefinition {
        .debugName = debugName,
        .type = ValueType::UInt64,
        .index = index,
        .defaultValue = Value(std::uint64_t {0}),
        .nullable = false,
    };
}

Value idValue(ItemId id)
{
    return Value(static_cast<std::uint64_t>(id));
}

struct TestContext {
    EngineSchema schema;
    std::unique_ptr<DocumentEngine> engine;

    TableHandle itemTable;
    ColumnHandle itemName;
    ColumnHandle itemValue;
    ColumnHandle itemScore;
    ColumnHandle itemTag;

    TableHandle groupTable;
    ColumnHandle groupId;

    TableHandle groupedItemTable;
    RelationHandle groupedItemParent;
    ColumnHandle groupedItemName;
    ColumnHandle groupedItemValue;
    VariantHandle typeAVariant;
    VariantHandle typeBVariant;
    ColumnHandle typeACol;
    ColumnHandle typeBCol;

    ListHandle rankingList;
    ColumnHandle rankingRank;

    std::vector<ItemId> itemIds;
    std::vector<ItemId> groupIds;
    std::vector<ItemId> groupedItemIds;
};

TestContext createSeededEngine()
{
    TestContext ctx;

    SchemaBuilder builder;

    // Table "Item"
    auto itemTbl = builder.createTable("Item");
    ctx.itemTable = itemTbl.handle();
    ctx.itemName = itemTbl.addColumn(stringColumn("name", IndexKind::Normal));
    ctx.itemValue = itemTbl.addColumn(int64Column("value", IndexKind::Normal));
    ctx.itemScore = itemTbl.addColumn(doubleColumn("score", IndexKind::Normal));
    ctx.itemTag = itemTbl.addColumn(stringColumn("tag", IndexKind::None));

    // Table "Group"
    auto groupTbl = builder.createTable("Group");
    ctx.groupTable = groupTbl.handle();
    ctx.groupId = groupTbl.addColumn(uint64Column("id", IndexKind::Normal));

    // Table "GroupedItem" — polymorphic with parent relation to Group
    auto groupedItemTbl = builder.createTable("GroupedItem");
    ctx.groupedItemTable = groupedItemTbl.handle();
    ctx.groupedItemParent = groupedItemTbl.addAssociation(AssociationDefinition {
        .debugName = "parent",
        .target = ctx.groupTable,
    });
    ctx.groupedItemName = groupedItemTbl.addColumn(stringColumn("name", IndexKind::Normal));
    ctx.groupedItemValue = groupedItemTbl.addColumn(int64Column("value", IndexKind::Normal));
    ctx.typeAVariant = groupedItemTbl.addVariant("TypeA");
    ctx.typeBVariant = groupedItemTbl.addVariant("TypeB");
    ctx.typeACol = groupedItemTbl.addVariantColumn(VariantColumnDefinition {
        .debugName = "a_col",
        .variant = ctx.typeAVariant,
        .type = ValueType::Int64,
        .index = IndexKind::Normal,
        .defaultValue = Value(std::int64_t {0}),
        .nullable = false,
    });
    ctx.typeBCol = groupedItemTbl.addVariantColumn(VariantColumnDefinition {
        .debugName = "b_col",
        .variant = ctx.typeBVariant,
        .type = ValueType::String,
        .index = IndexKind::Normal,
        .defaultValue = Value(""),
        .nullable = false,
    });

    // List "Ranking"
    auto rankingListBdr = builder.createList("Ranking");
    ctx.rankingList = rankingListBdr.handle();
    rankingListBdr.setAssociation(AssociationDefinition {
        .debugName = "group",
        .target = ctx.groupTable,
    });
    ctx.rankingRank = rankingListBdr.addColumn(int64Column("rank", IndexKind::Normal));

    ctx.schema = builder.freeze();
    ctx.engine = std::make_unique<DocumentEngine>(ctx.schema);

    // Seed data
    {
        auto txn = ctx.engine->beginTransaction();

        // 8 Items
        ctx.itemIds.push_back(txn.insert(ctx.itemTable, {
            ColumnValue {.column = ctx.itemName, .value = Value("Alpha")},
            ColumnValue {.column = ctx.itemValue, .value = Value(std::int64_t {10})},
            ColumnValue {.column = ctx.itemScore, .value = Value(1.5)},
            ColumnValue {.column = ctx.itemTag, .value = Value("a")},
        }));
        ctx.itemIds.push_back(txn.insert(ctx.itemTable, {
            ColumnValue {.column = ctx.itemName, .value = Value("Beta")},
            ColumnValue {.column = ctx.itemValue, .value = Value(std::int64_t {20})},
            ColumnValue {.column = ctx.itemScore, .value = Value(2.5)},
            ColumnValue {.column = ctx.itemTag, .value = Value("a")},
        }));
        ctx.itemIds.push_back(txn.insert(ctx.itemTable, {
            ColumnValue {.column = ctx.itemName, .value = Value("Gamma")},
            ColumnValue {.column = ctx.itemValue, .value = Value(std::int64_t {30})},
            ColumnValue {.column = ctx.itemScore, .value = Value(3.5)},
            ColumnValue {.column = ctx.itemTag, .value = Value("b")},
        }));
        ctx.itemIds.push_back(txn.insert(ctx.itemTable, {
            ColumnValue {.column = ctx.itemName, .value = Value("Delta")},
            ColumnValue {.column = ctx.itemValue, .value = Value(std::int64_t {40})},
            ColumnValue {.column = ctx.itemScore, .value = Value(4.5)},
            ColumnValue {.column = ctx.itemTag, .value = Value("b")},
        }));
        ctx.itemIds.push_back(txn.insert(ctx.itemTable, {
            ColumnValue {.column = ctx.itemName, .value = Value("Epsilon")},
            ColumnValue {.column = ctx.itemValue, .value = Value(std::int64_t {50})},
            ColumnValue {.column = ctx.itemScore, .value = Value(5.5)},
            ColumnValue {.column = ctx.itemTag, .value = Value("c")},
        }));
        ctx.itemIds.push_back(txn.insert(ctx.itemTable, {
            ColumnValue {.column = ctx.itemName, .value = Value("Zeta")},
            ColumnValue {.column = ctx.itemValue, .value = Value(std::int64_t {60})},
            ColumnValue {.column = ctx.itemScore, .value = Value(6.5)},
            ColumnValue {.column = ctx.itemTag, .value = Value("c")},
        }));
        ctx.itemIds.push_back(txn.insert(ctx.itemTable, {
            ColumnValue {.column = ctx.itemName, .value = Value("Eta")},
            ColumnValue {.column = ctx.itemValue, .value = Value(std::int64_t {70})},
            ColumnValue {.column = ctx.itemScore, .value = Value(7.5)},
            ColumnValue {.column = ctx.itemTag, .value = Value("d")},
        }));
        ctx.itemIds.push_back(txn.insert(ctx.itemTable, {
            ColumnValue {.column = ctx.itemName, .value = Value("Theta")},
            ColumnValue {.column = ctx.itemValue, .value = Value(std::int64_t {80})},
            ColumnValue {.column = ctx.itemScore, .value = Value(8.5)},
            ColumnValue {.column = ctx.itemTag, .value = Value("d")},
        }));

        // 2 Groups
        ctx.groupIds.push_back(txn.insert(ctx.groupTable, {
            ColumnValue {.column = ctx.groupId, .value = Value(std::uint64_t {100})},
        }));
        ctx.groupIds.push_back(txn.insert(ctx.groupTable, {
            ColumnValue {.column = ctx.groupId, .value = Value(std::uint64_t {200})},
        }));

        // 4 GroupedItems (2 TypeA under group 0, 2 TypeB under group 1)
        ctx.groupedItemIds.push_back(txn.insert(ctx.groupedItemTable, {
            ColumnValue {.column = ctx.groupedItemParent.column(), .value = idValue(ctx.groupIds[0])},
            ColumnValue {.column = ctx.groupedItemName, .value = Value("GA1")},
            ColumnValue {.column = ctx.groupedItemValue, .value = Value(std::int64_t {10})},
            ColumnValue {.column = ctx.typeACol, .value = Value(std::int64_t {100})},
        }, ctx.typeAVariant));
        ctx.groupedItemIds.push_back(txn.insert(ctx.groupedItemTable, {
            ColumnValue {.column = ctx.groupedItemParent.column(), .value = idValue(ctx.groupIds[0])},
            ColumnValue {.column = ctx.groupedItemName, .value = Value("GA2")},
            ColumnValue {.column = ctx.groupedItemValue, .value = Value(std::int64_t {20})},
            ColumnValue {.column = ctx.typeACol, .value = Value(std::int64_t {200})},
        }, ctx.typeAVariant));
        ctx.groupedItemIds.push_back(txn.insert(ctx.groupedItemTable, {
            ColumnValue {.column = ctx.groupedItemParent.column(), .value = idValue(ctx.groupIds[1])},
            ColumnValue {.column = ctx.groupedItemName, .value = Value("GB1")},
            ColumnValue {.column = ctx.groupedItemValue, .value = Value(std::int64_t {30})},
            ColumnValue {.column = ctx.typeBCol, .value = Value("hello")},
        }, ctx.typeBVariant));
        ctx.groupedItemIds.push_back(txn.insert(ctx.groupedItemTable, {
            ColumnValue {.column = ctx.groupedItemParent.column(), .value = idValue(ctx.groupIds[1])},
            ColumnValue {.column = ctx.groupedItemName, .value = Value("GB2")},
            ColumnValue {.column = ctx.groupedItemValue, .value = Value(std::int64_t {40})},
            ColumnValue {.column = ctx.typeBCol, .value = Value("world")},
        }, ctx.typeBVariant));

        // Ranking list entries: 3 under group 0, 2 under group 1
        txn.insert(ctx.rankingList, idValue(ctx.groupIds[0]), 0, {
            ColumnValue {.column = ctx.rankingRank, .value = Value(std::int64_t {1})},
        });
        txn.insert(ctx.rankingList, idValue(ctx.groupIds[0]), 1, {
            ColumnValue {.column = ctx.rankingRank, .value = Value(std::int64_t {2})},
        });
        txn.insert(ctx.rankingList, idValue(ctx.groupIds[0]), 2, {
            ColumnValue {.column = ctx.rankingRank, .value = Value(std::int64_t {3})},
        });
        txn.insert(ctx.rankingList, idValue(ctx.groupIds[1]), 0, {
            ColumnValue {.column = ctx.rankingRank, .value = Value(std::int64_t {10})},
        });
        txn.insert(ctx.rankingList, idValue(ctx.groupIds[1]), 1, {
            ColumnValue {.column = ctx.rankingRank, .value = Value(std::int64_t {20})},
        });

        txn.commit();
    }

    return ctx;
}

bool containsId(const std::vector<ItemSnapshot> &results, ItemId id)
{
    for (const auto &snap : results) {
        if (snap.id == id) {
            return true;
        }
    }
    return false;
}

struct HintContext {
    EngineSchema schema;
    std::unique_ptr<DocumentEngine> engine;
    TableHandle parentTable;
    ColumnHandle parentKey;
    TableHandle childTable;
    RelationHandle childParent;
    ColumnHandle childX;
    ColumnHandle childY;
    ColumnHandle childAmount;
    ColumnHandle childScore;
    std::vector<ItemId> parentIds;
    std::vector<ItemId> childIds;
};

struct HintRow {
    std::int64_t parentKey = -1;
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t amount = 0;
    double score = 0.0;

    friend bool operator<(const HintRow &lhs, const HintRow &rhs)
    {
        return std::tie(lhs.parentKey, lhs.x, lhs.y, lhs.amount, lhs.score) <
               std::tie(rhs.parentKey, rhs.x, rhs.y, rhs.amount, rhs.score);
    }

    friend bool operator==(const HintRow &lhs, const HintRow &rhs)
    {
        return std::tie(lhs.parentKey, lhs.x, lhs.y, lhs.amount, lhs.score) ==
               std::tie(rhs.parentKey, rhs.x, rhs.y, rhs.amount, rhs.score);
    }
};

ColumnDefinition aggregateHintedInt64Column(const char *debugName, bool aggregateHint)
{
    auto definition = int64Column(debugName, IndexKind::Normal);
    if (aggregateHint) {
        definition.aggregateIndex = AggregateIndexOptions {
            .sum = true,
            .minMax = true,
            .byParent = true,
        };
    }
    return definition;
}

ColumnDefinition aggregateHintedDoubleColumn(const char *debugName, bool aggregateHint)
{
    auto definition = doubleColumn(debugName, IndexKind::Normal);
    if (aggregateHint) {
        definition.aggregateIndex = AggregateIndexOptions {
            .sum = true,
            .minMax = true,
            .byParent = true,
        };
    }
    return definition;
}

HintContext createHintContext(bool hints)
{
    HintContext ctx;

    SchemaBuilder builder;
    auto parent = builder.createTable("HintParent");
    ctx.parentTable = parent.handle();
    ctx.parentKey = parent.addColumn(int64Column("key", IndexKind::Normal));

    auto child = builder.createTable("HintChild");
    ctx.childTable = child.handle();
    ctx.childParent = child.addAssociation(AssociationDefinition {
        .debugName = "parent",
        .target = ctx.parentTable,
    });
    ctx.childX = child.addColumn(int64Column("x", IndexKind::Normal));
    ctx.childY = child.addColumn(int64Column("y", IndexKind::Normal));
    ctx.childAmount = child.addColumn(aggregateHintedInt64Column("amount", hints));
    ctx.childScore = child.addColumn(aggregateHintedDoubleColumn("score", hints));
    if (hints) {
        child.addRangeIndex(RangeIndexDefinition {
            .debugName = "xy",
            .columns = {ctx.childX, ctx.childY},
        });
    }

    ctx.schema = builder.freeze();
    ctx.engine = std::make_unique<DocumentEngine>(ctx.schema);

    auto txn = ctx.engine->beginTransaction();
    for (const auto key : {std::int64_t {10}, std::int64_t {20}, std::int64_t {30}}) {
        ctx.parentIds.push_back(txn.insert(ctx.parentTable, {
            ColumnValue {.column = ctx.parentKey, .value = Value(key)},
        }));
    }

    auto insertChild = [&](std::optional<std::size_t> parentIndex,
                           std::int64_t x,
                           std::int64_t y,
                           std::int64_t amount,
                           double score) {
        std::vector<ColumnValue> values {
            ColumnValue {
                .column = ctx.childParent.column(),
                .value = parentIndex ? idValue(ctx.parentIds[*parentIndex]) : Value::null(),
            },
            ColumnValue {.column = ctx.childX, .value = Value(x)},
            ColumnValue {.column = ctx.childY, .value = Value(y)},
            ColumnValue {.column = ctx.childAmount, .value = Value(amount)},
            ColumnValue {.column = ctx.childScore, .value = Value(score)},
        };
        ctx.childIds.push_back(txn.insert(ctx.childTable, std::move(values)));
    };

    insertChild(0, -10, 100, 5, 1.5);
    insertChild(0, 0, 50, 7, 9.0);
    insertChild(1, 10, 20, -3, 2.5);
    insertChild(1, 20, 10, 12, 2.5);
    insertChild(2, 30, 0, 0, -1.0);
    insertChild(2, 40, -10, 18, 8.0);
    insertChild(std::nullopt, 50, -20, 4, 3.5);
    txn.commit();

    return ctx;
}

std::int64_t parentBusinessKey(const HintContext &ctx, const Value &parentValue)
{
    if (parentValue.isNull()) {
        return -1;
    }
    return ctx.engine->read(static_cast<ItemId>(parentValue.asUInt64()), ctx.parentKey).asInt64();
}

std::vector<HintRow> queryRows(const HintContext &ctx, const QuerySpec &spec)
{
    auto snapshots = ctx.engine->query(ctx.childTable, spec).toVector();
    std::vector<HintRow> rows;
    rows.reserve(snapshots.size());
    for (const auto &snapshot : snapshots) {
        const auto parentValue = ctx.engine->read(snapshot.id, ctx.childParent.column());
        rows.push_back(HintRow {
            .parentKey = parentBusinessKey(ctx, parentValue),
            .x = ctx.engine->read(snapshot.id, ctx.childX).asInt64(),
            .y = ctx.engine->read(snapshot.id, ctx.childY).asInt64(),
            .amount = ctx.engine->read(snapshot.id, ctx.childAmount).asInt64(),
            .score = ctx.engine->read(snapshot.id, ctx.childScore).asDouble(),
        });
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

Value ungroupedAggregateValue(const HintContext &ctx, const AggregationSpec &spec)
{
    auto rows = ctx.engine->view(ctx.childTable).aggregate(spec).toVector();
    EXPECT_EQ(rows.size(), 1U);
    return rows.empty() ? Value::null() : rows.front().value;
}

std::map<std::int64_t, Value> parentAggregateRows(const HintContext &ctx, const AggregationSpec &spec)
{
    auto rows = ctx.engine->view(ctx.childTable).aggregate(spec).toVector();
    std::map<std::int64_t, Value> result;
    for (const auto &row : rows) {
        result[parentBusinessKey(ctx, row.groupKey.value_or(Value::null()))] = row.value;
    }
    return result;
}

void expectSameValue(const Value &lhs, const Value &rhs)
{
    EXPECT_TRUE(lhs == rhs);
}

void expectSameParentAggregates(const HintContext &fallback, const HintContext &hinted)
{
    const std::vector<AggregationSpec> specs {
        AggregationSpec {
            .kind = AggregateKind::Count,
            .groupBy = FieldRef::parent(fallback.childParent),
        },
        AggregationSpec {
            .kind = AggregateKind::Sum,
            .valueField = FieldRef::column(fallback.childAmount),
            .groupBy = FieldRef::parent(fallback.childParent),
        },
        AggregationSpec {
            .kind = AggregateKind::Minimum,
            .valueField = FieldRef::column(fallback.childScore),
            .groupBy = FieldRef::parent(fallback.childParent),
        },
        AggregationSpec {
            .kind = AggregateKind::Maximum,
            .valueField = FieldRef::column(fallback.childScore),
            .groupBy = FieldRef::parent(fallback.childParent),
        },
    };
    const std::vector<AggregationSpec> hintedSpecs {
        AggregationSpec {
            .kind = AggregateKind::Count,
            .groupBy = FieldRef::parent(hinted.childParent),
        },
        AggregationSpec {
            .kind = AggregateKind::Sum,
            .valueField = FieldRef::column(hinted.childAmount),
            .groupBy = FieldRef::parent(hinted.childParent),
        },
        AggregationSpec {
            .kind = AggregateKind::Minimum,
            .valueField = FieldRef::column(hinted.childScore),
            .groupBy = FieldRef::parent(hinted.childParent),
        },
        AggregationSpec {
            .kind = AggregateKind::Maximum,
            .valueField = FieldRef::column(hinted.childScore),
            .groupBy = FieldRef::parent(hinted.childParent),
        },
    };

    for (std::size_t i = 0; i < specs.size(); ++i) {
        const auto fallbackRows = parentAggregateRows(fallback, specs[i]);
        const auto hintedRows = parentAggregateRows(hinted, hintedSpecs[i]);
        ASSERT_EQ(fallbackRows.size(), hintedRows.size());
        for (const auto &[key, value] : fallbackRows) {
            auto hintedIt = hintedRows.find(key);
            ASSERT_NE(hintedIt, hintedRows.end());
            expectSameValue(value, hintedIt->second);
        }
    }
}

TEST(DocumentEngineQueryTest, QueryAllComparisonOperators)
{
    auto ctx = createSeededEngine();
    const auto &ids = ctx.itemIds;

    // Equal: value == 30 -> Gamma
    {
        const QuerySpec spec {
            .filter = FilterExpression(Filter(
                FieldRef::column(ctx.itemValue),
                ComparisonOperator::Equal,
                Value(std::int64_t {30}))),
        };
        auto results = ctx.engine->query(ctx.itemTable, spec).toVector();
        ASSERT_EQ(results.size(), 1U);
        EXPECT_EQ(results[0].id, ids[2]);
    }

    // NotEqual: value != 30 -> all except Gamma
    {
        const QuerySpec spec {
            .filter = FilterExpression(Filter(
                FieldRef::column(ctx.itemValue),
                ComparisonOperator::NotEqual,
                Value(std::int64_t {30}))),
        };
        auto results = ctx.engine->query(ctx.itemTable, spec).toVector();
        EXPECT_EQ(results.size(), 7U);
        EXPECT_FALSE(containsId(results, ids[2]));
    }

    // Greater: value > 40 -> Epsilon(50), Zeta(60), Eta(70), Theta(80)
    {
        const QuerySpec spec {
            .filter = FilterExpression(Filter(
                FieldRef::column(ctx.itemValue),
                ComparisonOperator::Greater,
                Value(std::int64_t {40}))),
        };
        auto results = ctx.engine->query(ctx.itemTable, spec).toVector();
        ASSERT_EQ(results.size(), 4U);
        EXPECT_TRUE(containsId(results, ids[4]));
        EXPECT_TRUE(containsId(results, ids[5]));
        EXPECT_TRUE(containsId(results, ids[6]));
        EXPECT_TRUE(containsId(results, ids[7]));
    }

    // Less: value < 30 -> Alpha(10), Beta(20)
    {
        const QuerySpec spec {
            .filter = FilterExpression(Filter(
                FieldRef::column(ctx.itemValue),
                ComparisonOperator::Less,
                Value(std::int64_t {30}))),
        };
        auto results = ctx.engine->query(ctx.itemTable, spec).toVector();
        ASSERT_EQ(results.size(), 2U);
        EXPECT_TRUE(containsId(results, ids[0]));
        EXPECT_TRUE(containsId(results, ids[1]));
    }

    // GreaterOrEqual: value >= 70 -> Eta(70), Theta(80)
    {
        const QuerySpec spec {
            .filter = FilterExpression(Filter(
                FieldRef::column(ctx.itemValue),
                ComparisonOperator::GreaterOrEqual,
                Value(std::int64_t {70}))),
        };
        auto results = ctx.engine->query(ctx.itemTable, spec).toVector();
        ASSERT_EQ(results.size(), 2U);
        EXPECT_TRUE(containsId(results, ids[6]));
        EXPECT_TRUE(containsId(results, ids[7]));
    }

    // LessOrEqual: value <= 20 -> Alpha(10), Beta(20)
    {
        const QuerySpec spec {
            .filter = FilterExpression(Filter(
                FieldRef::column(ctx.itemValue),
                ComparisonOperator::LessOrEqual,
                Value(std::int64_t {20}))),
        };
        auto results = ctx.engine->query(ctx.itemTable, spec).toVector();
        ASSERT_EQ(results.size(), 2U);
        EXPECT_TRUE(containsId(results, ids[0]));
        EXPECT_TRUE(containsId(results, ids[1]));
    }
}

TEST(DocumentEngineQueryTest, QueryStringComparison)
{
    auto ctx = createSeededEngine();
    const auto &ids = ctx.itemIds;

    // Equal: name == "Delta"
    {
        const QuerySpec spec {
            .filter = FilterExpression(Filter(
                FieldRef::column(ctx.itemName),
                ComparisonOperator::Equal,
                Value("Delta"))),
        };
        auto results = ctx.engine->query(ctx.itemTable, spec).toVector();
        ASSERT_EQ(results.size(), 1U);
        EXPECT_EQ(results[0].id, ids[3]);
    }

    // NotEqual: name != "Alpha"
    {
        const QuerySpec spec {
            .filter = FilterExpression(Filter(
                FieldRef::column(ctx.itemName),
                ComparisonOperator::NotEqual,
                Value("Alpha"))),
        };
        auto results = ctx.engine->query(ctx.itemTable, spec).toVector();
        EXPECT_EQ(results.size(), 7U);
        EXPECT_FALSE(containsId(results, ids[0]));
    }
}

TEST(DocumentEngineQueryTest, QueryAndCombination)
{
    auto ctx = createSeededEngine();
    const auto &ids = ctx.itemIds;

    // value > 20 AND value < 60 AND score > 2.0
    // -> Gamma(30, 3.5), Delta(40, 4.5), Epsilon(50, 5.5)
    const QuerySpec spec {
        .filter = FilterExpression::all({
            FilterExpression(Filter(
                FieldRef::column(ctx.itemValue),
                ComparisonOperator::Greater,
                Value(std::int64_t {20}))),
            FilterExpression(Filter(
                FieldRef::column(ctx.itemValue),
                ComparisonOperator::Less,
                Value(std::int64_t {60}))),
            FilterExpression(Filter(
                FieldRef::column(ctx.itemScore),
                ComparisonOperator::Greater,
                Value(2.0))),
        }),
    };
    auto results = ctx.engine->query(ctx.itemTable, spec).toVector();
    ASSERT_EQ(results.size(), 3U);
    EXPECT_TRUE(containsId(results, ids[2]));
    EXPECT_TRUE(containsId(results, ids[3]));
    EXPECT_TRUE(containsId(results, ids[4]));
}

TEST(DocumentEngineQueryTest, QueryOrCombination)
{
    auto ctx = createSeededEngine();
    const auto &ids = ctx.itemIds;

    // name == "Alpha" OR name == "Gamma" OR name == "Epsilon"
    const QuerySpec spec {
        .filter = FilterExpression::any({
            FilterExpression(Filter(
                FieldRef::column(ctx.itemName),
                ComparisonOperator::Equal,
                Value("Alpha"))),
            FilterExpression(Filter(
                FieldRef::column(ctx.itemName),
                ComparisonOperator::Equal,
                Value("Gamma"))),
            FilterExpression(Filter(
                FieldRef::column(ctx.itemName),
                ComparisonOperator::Equal,
                Value("Epsilon"))),
        }),
    };
    auto results = ctx.engine->query(ctx.itemTable, spec).toVector();
    ASSERT_EQ(results.size(), 3U);
    EXPECT_TRUE(containsId(results, ids[0]));
    EXPECT_TRUE(containsId(results, ids[2]));
    EXPECT_TRUE(containsId(results, ids[4]));
}

TEST(DocumentEngineQueryTest, QueryNotCombination)
{
    auto ctx = createSeededEngine();
    const auto &ids = ctx.itemIds;

    // NOT (value < 30)  -> value >= 30 -> Gamma through Theta (6 items)
    const QuerySpec spec {
        .filter = FilterExpression::negate(
            FilterExpression(Filter(
                FieldRef::column(ctx.itemValue),
                ComparisonOperator::Less,
                Value(std::int64_t {30})))),
    };
    auto results = ctx.engine->query(ctx.itemTable, spec).toVector();
    ASSERT_EQ(results.size(), 6U);
    EXPECT_FALSE(containsId(results, ids[0]));
    EXPECT_FALSE(containsId(results, ids[1]));
}

TEST(DocumentEngineQueryTest, QueryMixedAndOrNot)
{
    auto ctx = createSeededEngine();
    const auto &ids = ctx.itemIds;

    // AND(OR(name == "Alpha", name == "Beta"), NOT(value > 15))
    // OR -> {Alpha, Beta}; NOT(value > 15) -> value <= 15 -> {Alpha}
    // Intersection -> {Alpha}
    const QuerySpec spec {
        .filter = FilterExpression::all({
            FilterExpression::any({
                FilterExpression(Filter(
                    FieldRef::column(ctx.itemName),
                    ComparisonOperator::Equal,
                    Value("Alpha"))),
                FilterExpression(Filter(
                    FieldRef::column(ctx.itemName),
                    ComparisonOperator::Equal,
                    Value("Beta"))),
            }),
            FilterExpression::negate(
                FilterExpression(Filter(
                    FieldRef::column(ctx.itemValue),
                    ComparisonOperator::Greater,
                    Value(std::int64_t {15})))),
        }),
    };
    auto results = ctx.engine->query(ctx.itemTable, spec).toVector();
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0].id, ids[0]);
}

TEST(DocumentEngineQueryTest, QueryById)
{
    auto ctx = createSeededEngine();
    const auto &ids = ctx.itemIds;

    const QuerySpec spec {
        .filter = FilterExpression(Filter(
            FieldRef::id(),
            ComparisonOperator::Equal,
            idValue(ids[4]))),
    };
    auto results = ctx.engine->query(ctx.itemTable, spec).toVector();
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0].id, ids[4]);
}

TEST(DocumentEngineQueryTest, QueryByParentRelation)
{
    auto ctx = createSeededEngine();

    // GroupedItems with parent == groupIds[0] -> GA1, GA2 (two TypeA items)
    const QuerySpec spec {
        .filter = FilterExpression(Filter(
            FieldRef::parent(ctx.groupedItemParent),
            ComparisonOperator::Equal,
            idValue(ctx.groupIds[0]))),
    };
    auto results = ctx.engine->query(ctx.groupedItemTable, spec).toVector();
    ASSERT_EQ(results.size(), 2U);
    EXPECT_TRUE(containsId(results, ctx.groupedItemIds[0]));
    EXPECT_TRUE(containsId(results, ctx.groupedItemIds[1]));
}

TEST(DocumentEngineQueryTest, QueryByVariant)
{
    auto ctx = createSeededEngine();

    // GroupedItems of variant TypeA
    const QuerySpec spec {
        .filter = FilterExpression(Filter(
            FieldRef::variant(ctx.typeAVariant),
            ComparisonOperator::Equal,
            Value(static_cast<std::uint64_t>(ctx.typeAVariant.variantId())))),
    };
    auto results = ctx.engine->query(ctx.groupedItemTable, spec).toVector();
    ASSERT_EQ(results.size(), 2U);
    EXPECT_TRUE(containsId(results, ctx.groupedItemIds[0]));
    EXPECT_TRUE(containsId(results, ctx.groupedItemIds[1]));
}

TEST(DocumentEngineQueryTest, QuerySortAscendingDescending)
{
    auto ctx = createSeededEngine();
    const auto &ids = ctx.itemIds;

    // Single key ASC: sort by name
    {
        const QuerySpec spec {
            .sortKeys = {
                SortKey {.field = FieldRef::column(ctx.itemName), .direction = SortDirection::Ascending},
            },
        };
        auto results = ctx.engine->query(ctx.itemTable, spec).toVector();
        ASSERT_EQ(results.size(), 8U);
        EXPECT_EQ(results[0].id, ids[0]);  // Alpha
        EXPECT_EQ(results[7].id, ids[5]);  // Zeta
        EXPECT_EQ(results[1].id, ids[1]);  // Beta
        EXPECT_EQ(results[2].id, ids[3]);  // Delta
    }

    // Single key DESC: sort by value descending
    {
        const QuerySpec spec {
            .sortKeys = {
                SortKey {.field = FieldRef::column(ctx.itemValue), .direction = SortDirection::Descending},
            },
        };
        auto results = ctx.engine->query(ctx.itemTable, spec).toVector();
        ASSERT_EQ(results.size(), 8U);
        EXPECT_EQ(results[0].id, ids[7]);  // Theta 80
        EXPECT_EQ(results[1].id, ids[6]);  // Eta 70
        EXPECT_EQ(results[7].id, ids[0]);  // Alpha 10
    }

    // Multi-key: name ASC, value DESC
    {
        const QuerySpec spec {
            .sortKeys = {
                SortKey {.field = FieldRef::column(ctx.itemName), .direction = SortDirection::Ascending},
                SortKey {.field = FieldRef::column(ctx.itemValue), .direction = SortDirection::Descending},
            },
        };
        auto results = ctx.engine->query(ctx.itemTable, spec).toVector();
        ASSERT_EQ(results.size(), 8U);
        EXPECT_EQ(results[0].id, ids[0]);  // Alpha
        EXPECT_EQ(results[7].id, ids[5]);  // Zeta
    }
}

TEST(DocumentEngineQueryTest, QueryOnListContainer)
{
    auto ctx = createSeededEngine();

    // Ranking list entries with rank > 2, sorted by rank descending
    const QuerySpec spec {
        .filter = FilterExpression(Filter(
            FieldRef::column(ctx.rankingRank),
            ComparisonOperator::Greater,
            Value(std::int64_t {2}))),
        .sortKeys = {
            SortKey {.field = FieldRef::column(ctx.rankingRank), .direction = SortDirection::Descending},
        },
    };
    auto results = ctx.engine->query(ctx.rankingList, spec).toVector();
    ASSERT_EQ(results.size(), 3U);
    // Expected: rank 20, rank 10, rank 3
    EXPECT_EQ(results[0].values[0].value.asInt64(), 20);
    EXPECT_EQ(results[1].values[0].value.asInt64(), 10);
    EXPECT_EQ(results[2].values[0].value.asInt64(), 3);
}

TEST(DocumentEngineQueryTest, QueryEmptyResult)
{
    auto ctx = createSeededEngine();

    // value > 1000 -> no items match
    const QuerySpec spec {
        .filter = FilterExpression(Filter(
            FieldRef::column(ctx.itemValue),
            ComparisonOperator::Greater,
            Value(std::int64_t {1000}))),
    };
    auto results = ctx.engine->query(ctx.itemTable, spec).toVector();
    EXPECT_EQ(results.size(), 0U);
}

TEST(DocumentEngineQueryTest, QueryMatchAll)
{
    auto ctx = createSeededEngine();

    // Empty FilterExpression -> matches all items
    const QuerySpec spec {
        .filter = FilterExpression(),
    };
    auto results = ctx.engine->query(ctx.itemTable, spec).toVector();
    EXPECT_EQ(results.size(), 8U);
}

TEST(DocumentEngineQueryTest, QueryRejectsNonIndexedColumn)
{
    auto ctx = createSeededEngine();

    // tag has IndexKind::None -> filtering on it must throw QueryError
    const QuerySpec spec {
        .filter = FilterExpression(Filter(
            FieldRef::column(ctx.itemTag),
            ComparisonOperator::Equal,
            Value("a"))),
    };
    EXPECT_THROW(ctx.engine->query(ctx.itemTable, spec), QueryError);
}

TEST(DocumentEngineQueryTest, AggregateCount)
{
    auto ctx = createSeededEngine();

    const AggregationSpec spec {
        .kind = AggregateKind::Count,
    };
    auto results = ctx.engine->view(ctx.itemTable).aggregate(spec).toVector();
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0].value.asUInt64(), 8U);
}

TEST(DocumentEngineQueryTest, AggregateSum)
{
    auto ctx = createSeededEngine();

    const AggregationSpec spec {
        .kind = AggregateKind::Sum,
        .valueField = FieldRef::column(ctx.itemValue),
    };
    auto results = ctx.engine->view(ctx.itemTable).aggregate(spec).toVector();
    ASSERT_EQ(results.size(), 1U);
    // 10+20+30+40+50+60+70+80 = 360
    EXPECT_EQ(results[0].value.asInt64(), 360);
}

TEST(DocumentEngineQueryTest, AggregateMinMax)
{
    auto ctx = createSeededEngine();

    // Minimum
    {
        const AggregationSpec spec {
            .kind = AggregateKind::Minimum,
            .valueField = FieldRef::column(ctx.itemValue),
        };
        auto results = ctx.engine->view(ctx.itemTable).aggregate(spec).toVector();
        ASSERT_EQ(results.size(), 1U);
        EXPECT_EQ(results[0].value.asInt64(), 10);
    }

    // Maximum
    {
        const AggregationSpec spec {
            .kind = AggregateKind::Maximum,
            .valueField = FieldRef::column(ctx.itemValue),
        };
        auto results = ctx.engine->view(ctx.itemTable).aggregate(spec).toVector();
        ASSERT_EQ(results.size(), 1U);
        EXPECT_EQ(results[0].value.asInt64(), 80);
    }
}

TEST(DocumentEngineQueryTest, AggregateGroupByVariant)
{
    auto ctx = createSeededEngine();

    const AggregationSpec spec {
        .kind = AggregateKind::Count,
        .groupBy = FieldRef::variant(ctx.typeAVariant),
    };
    auto results = ctx.engine->view(ctx.groupedItemTable).aggregate(spec).toVector();
    ASSERT_EQ(results.size(), 2U);

    // One group for each variant, 2 items each
    std::uint64_t totalCount = 0;
    for (const auto &row : results) {
        ASSERT_TRUE(row.groupKey.has_value());
        auto vid = row.groupKey->asUInt64();
        EXPECT_TRUE(vid == static_cast<std::uint64_t>(ctx.typeAVariant.variantId())
                 || vid == static_cast<std::uint64_t>(ctx.typeBVariant.variantId()));
        EXPECT_EQ(row.value.asUInt64(), 2U);
        totalCount += row.value.asUInt64();
    }
    EXPECT_EQ(totalCount, 4U);
}

TEST(DocumentEngineQueryTest, AggregateGroupByParent)
{
    auto ctx = createSeededEngine();

    const AggregationSpec spec {
        .kind = AggregateKind::Count,
        .groupBy = FieldRef::parent(ctx.groupedItemParent),
    };
    auto results = ctx.engine->view(ctx.groupedItemTable).aggregate(spec).toVector();
    ASSERT_EQ(results.size(), 2U);

    // Each group has 2 children
    for (const auto &row : results) {
        ASSERT_TRUE(row.groupKey.has_value());
        EXPECT_EQ(row.value.asUInt64(), 2U);
    }
}

TEST(DocumentEngineQueryTest, QueryRangeHintAndFallbackReturnSameResults)
{
    auto fallback = createHintContext(false);
    auto hinted = createHintContext(true);

    auto makeAnd = [](ColumnHandle x, ColumnHandle y, ComparisonOperator xOp, Value xValue,
                      ComparisonOperator yOp, Value yValue) {
        return QuerySpec {
            .filter = FilterExpression::all({
                FilterExpression(Filter(FieldRef::column(x), xOp, std::move(xValue))),
                FilterExpression(Filter(FieldRef::column(y), yOp, std::move(yValue))),
            }),
        };
    };

    const auto fallbackAnd = makeAnd(fallback.childX,
                                     fallback.childY,
                                     ComparisonOperator::Greater,
                                     Value(std::int64_t {0}),
                                     ComparisonOperator::Less,
                                     Value(std::int64_t {20}));
    const auto hintedAnd = makeAnd(hinted.childX,
                                   hinted.childY,
                                   ComparisonOperator::Greater,
                                   Value(std::int64_t {0}),
                                   ComparisonOperator::Less,
                                   Value(std::int64_t {20}));
    EXPECT_EQ(queryRows(fallback, fallbackAnd), queryRows(hinted, hintedAnd));

    const auto fallbackClosed = makeAnd(fallback.childX,
                                        fallback.childY,
                                        ComparisonOperator::GreaterOrEqual,
                                        Value(std::int64_t {0}),
                                        ComparisonOperator::LessOrEqual,
                                        Value(std::int64_t {20}));
    const auto hintedClosed = makeAnd(hinted.childX,
                                      hinted.childY,
                                      ComparisonOperator::GreaterOrEqual,
                                      Value(std::int64_t {0}),
                                      ComparisonOperator::LessOrEqual,
                                      Value(std::int64_t {20}));
    EXPECT_EQ(queryRows(fallback, fallbackClosed), queryRows(hinted, hintedClosed));

    const QuerySpec fallbackNegated {
        .filter = FilterExpression::negate(fallbackAnd.filter),
    };
    const QuerySpec hintedNegated {
        .filter = FilterExpression::negate(hintedAnd.filter),
    };
    EXPECT_EQ(queryRows(fallback, fallbackNegated), queryRows(hinted, hintedNegated));

    const QuerySpec fallbackMixed {
        .filter = FilterExpression::any({
            FilterExpression::all({
                FilterExpression(Filter(FieldRef::column(fallback.childX),
                                        ComparisonOperator::Less,
                                        Value(std::int64_t {0}))),
                FilterExpression(Filter(FieldRef::column(fallback.childY),
                                        ComparisonOperator::Greater,
                                        Value(std::int64_t {90}))),
            }),
            FilterExpression::all({
                FilterExpression(Filter(FieldRef::column(fallback.childX),
                                        ComparisonOperator::Greater,
                                        Value(std::int64_t {35}))),
                FilterExpression(Filter(FieldRef::column(fallback.childAmount),
                                        ComparisonOperator::GreaterOrEqual,
                                        Value(std::int64_t {4}))),
            }),
        }),
    };
    const QuerySpec hintedMixed {
        .filter = FilterExpression::any({
            FilterExpression::all({
                FilterExpression(Filter(FieldRef::column(hinted.childX),
                                        ComparisonOperator::Less,
                                        Value(std::int64_t {0}))),
                FilterExpression(Filter(FieldRef::column(hinted.childY),
                                        ComparisonOperator::Greater,
                                        Value(std::int64_t {90}))),
            }),
            FilterExpression::all({
                FilterExpression(Filter(FieldRef::column(hinted.childX),
                                        ComparisonOperator::Greater,
                                        Value(std::int64_t {35}))),
                FilterExpression(Filter(FieldRef::column(hinted.childAmount),
                                        ComparisonOperator::GreaterOrEqual,
                                        Value(std::int64_t {4}))),
            }),
        }),
    };
    EXPECT_EQ(queryRows(fallback, fallbackMixed), queryRows(hinted, hintedMixed));
}

TEST(DocumentEngineQueryTest, AggregateHintAndFallbackReturnSameUngroupedResults)
{
    auto fallback = createHintContext(false);
    auto hinted = createHintContext(true);

    const std::vector<AggregationSpec> fallbackSpecs {
        AggregationSpec {.kind = AggregateKind::Count},
        AggregationSpec {.kind = AggregateKind::Sum, .valueField = FieldRef::column(fallback.childAmount)},
        AggregationSpec {.kind = AggregateKind::Minimum, .valueField = FieldRef::column(fallback.childScore)},
        AggregationSpec {.kind = AggregateKind::Maximum, .valueField = FieldRef::column(fallback.childScore)},
    };
    const std::vector<AggregationSpec> hintedSpecs {
        AggregationSpec {.kind = AggregateKind::Count},
        AggregationSpec {.kind = AggregateKind::Sum, .valueField = FieldRef::column(hinted.childAmount)},
        AggregationSpec {.kind = AggregateKind::Minimum, .valueField = FieldRef::column(hinted.childScore)},
        AggregationSpec {.kind = AggregateKind::Maximum, .valueField = FieldRef::column(hinted.childScore)},
    };

    for (std::size_t i = 0; i < fallbackSpecs.size(); ++i) {
        expectSameValue(ungroupedAggregateValue(fallback, fallbackSpecs[i]),
                        ungroupedAggregateValue(hinted, hintedSpecs[i]));
    }
}

TEST(DocumentEngineQueryTest, AggregateHintAndFallbackReturnSameParentGroupedResults)
{
    auto fallback = createHintContext(false);
    auto hinted = createHintContext(true);

    expectSameParentAggregates(fallback, hinted);
}

TEST(DocumentEngineQueryTest, AggregateHintsStayConsistentAfterMutations)
{
    auto fallback = createHintContext(false);
    auto hinted = createHintContext(true);

    auto insertExtra = [](HintContext &ctx) {
        auto txn = ctx.engine->beginTransaction();
        ctx.childIds.push_back(txn.insert(ctx.childTable, {
            ColumnValue {.column = ctx.childParent.column(), .value = idValue(ctx.parentIds[0])},
            ColumnValue {.column = ctx.childX, .value = Value(std::int64_t {60})},
            ColumnValue {.column = ctx.childY, .value = Value(std::int64_t {-60})},
            ColumnValue {.column = ctx.childAmount, .value = Value(std::int64_t {11})},
            ColumnValue {.column = ctx.childScore, .value = Value(11.5)},
        }));
        txn.commit();
    };
    insertExtra(fallback);
    insertExtra(hinted);
    expectSameParentAggregates(fallback, hinted);

    auto updateValues = [](HintContext &ctx) {
        auto txn = ctx.engine->beginTransaction();
        txn.update(ctx.childIds[0], ctx.childAmount, Value(std::int64_t {15}));
        txn.update(ctx.childIds[0], ctx.childScore, Value(-2.0));
        txn.commit();
    };
    updateValues(fallback);
    updateValues(hinted);
    expectSameParentAggregates(fallback, hinted);

    auto reassignParent = [](HintContext &ctx) {
        auto txn = ctx.engine->beginTransaction();
        txn.update(ctx.childIds[1], ctx.childParent.column(), idValue(ctx.parentIds[2]));
        txn.commit();
    };
    reassignParent(fallback);
    reassignParent(hinted);
    expectSameParentAggregates(fallback, hinted);

    auto removeChild = [](HintContext &ctx) {
        auto txn = ctx.engine->beginTransaction();
        txn.remove(ctx.childIds[3]);
        txn.commit();
    };
    removeChild(fallback);
    removeChild(hinted);
    expectSameParentAggregates(fallback, hinted);
}

TEST(DocumentEngineQueryTest, ViewFilterChaining)
{
    auto ctx = createSeededEngine();
    const auto &ids = ctx.itemIds;

    // view.filter(A).filter(B) equivalent to AND
    // A: value > 15,  B: value < 45 -> Beta(20), Gamma(30), Delta(40)
    auto results = ctx.engine->view(ctx.itemTable)
                       .filter(FilterExpression(Filter(
                           FieldRef::column(ctx.itemValue),
                           ComparisonOperator::Greater,
                           Value(std::int64_t {15}))))
                       .filter(FilterExpression(Filter(
                           FieldRef::column(ctx.itemValue),
                           ComparisonOperator::Less,
                           Value(std::int64_t {45}))))
                       .toVector();
    ASSERT_EQ(results.size(), 3U);
    EXPECT_TRUE(containsId(results, ids[1]));
    EXPECT_TRUE(containsId(results, ids[2]));
    EXPECT_TRUE(containsId(results, ids[3]));
}

TEST(DocumentEngineQueryTest, ViewSortChaining)
{
    auto ctx = createSeededEngine();
    const auto &ids = ctx.itemIds;

    auto results = ctx.engine->view(ctx.itemTable)
                       .sort({
                           SortKey {.field = FieldRef::column(ctx.itemName), .direction = SortDirection::Ascending},
                       })
                       .toVector();
    ASSERT_EQ(results.size(), 8U);
    EXPECT_EQ(results[0].id, ids[0]);  // Alpha
    EXPECT_EQ(results[7].id, ids[5]);  // Zeta
}

TEST(DocumentEngineQueryTest, ViewFromQueryThenFilter)
{
    auto ctx = createSeededEngine();
    const auto &ids = ctx.itemIds;

    // query with value > 20, then additional filter value < 60
    // -> Gamma(30), Delta(40), Epsilon(50)
    const QuerySpec spec {
        .filter = FilterExpression(Filter(
            FieldRef::column(ctx.itemValue),
            ComparisonOperator::Greater,
            Value(std::int64_t {20}))),
    };
    auto view = ctx.engine->query(ctx.itemTable, spec);
    auto results = view.filter(FilterExpression(Filter(
        FieldRef::column(ctx.itemValue),
        ComparisonOperator::Less,
        Value(std::int64_t {60}))))
        .toVector();
    ASSERT_EQ(results.size(), 3U);
    EXPECT_TRUE(containsId(results, ids[2]));
    EXPECT_TRUE(containsId(results, ids[3]));
    EXPECT_TRUE(containsId(results, ids[4]));
}

TEST(DocumentEngineQueryTest, QueryLiveViewSeesCommittedChanges)
{
    auto ctx = createSeededEngine();

    // View over value > 75 -> initially only Theta(80)
    auto view = ctx.engine->view(ctx.itemTable)
                     .filter(FilterExpression(Filter(
                         FieldRef::column(ctx.itemValue),
                         ComparisonOperator::Greater,
                         Value(std::int64_t {75}))));
    EXPECT_EQ(view.toVector().size(), 1U);

    // Committed update: change Eta(70) to value 85
    {
        auto txn = ctx.engine->beginTransaction();
        txn.update(ctx.itemIds[6], ctx.itemValue, Value(std::int64_t {85}));
        txn.commit();
    }

    // Now the live view sees both Theta and Eta
    auto results = view.toVector();
    ASSERT_EQ(results.size(), 2U);
    EXPECT_TRUE(containsId(results, ctx.itemIds[6]));  // Eta
    EXPECT_TRUE(containsId(results, ctx.itemIds[7]));  // Theta
}

TEST(DocumentEngineQueryTest, QueryLiveViewSeesRollback)
{
    auto ctx = createSeededEngine();

    // View over value > 75 -> initially only Theta(80)
    auto view = ctx.engine->view(ctx.itemTable)
                     .filter(FilterExpression(Filter(
                         FieldRef::column(ctx.itemValue),
                         ComparisonOperator::Greater,
                         Value(std::int64_t {75}))));
    EXPECT_EQ(view.toVector().size(), 1U);

    // Write a change and roll it back
    {
        auto txn = ctx.engine->beginTransaction();
        txn.update(ctx.itemIds[5], ctx.itemValue, Value(std::int64_t {90}));
        txn.rollback();
    }

    // The live view still sees only Theta
    auto results = view.toVector();
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0].id, ctx.itemIds[7]);
}

} // namespace


