#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include <dini/engine.h>
#include <dini/errors.h>

#include <gtest/gtest.h>

namespace {

using namespace dini;

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

Value idValue(ItemId id)
{
    return Value(static_cast<std::uint64_t>(id));
}

bool containsId(const std::vector<ItemSnapshot> &items, ItemId id)
{
    return std::any_of(items.begin(), items.end(), [id](const auto &item) {
        return item.id == id;
    });
}

struct SpecialIndexContext {
    EngineSchema schema;
    std::unique_ptr<DocumentEngine> engine;
    TableHandle groupTable;
    TableHandle itemTable;
    ColumnHandle groupName;
    RelationHandle itemParent;
    ColumnHandle rank;
    ColumnHandle subrank;
    ColumnHandle start;
    ColumnHandle end;
    OrderedIndexHandle orderIndex;
    IntervalIndexHandle intervalIndex;
    ItemId groupA = 0;
    ItemId groupB = 0;
};

SpecialIndexContext createSpecialIndexContext()
{
    SpecialIndexContext ctx;
    SchemaBuilder builder;
    auto group = builder.createTable("Group");
    ctx.groupTable = group.handle();
    ctx.groupName = group.addColumn(stringColumn("name"));

    auto item = builder.createTable("Item");
    ctx.itemTable = item.handle();
    ctx.itemParent = item.addAssociation(AssociationDefinition {
        .debugName = "group",
        .target = ctx.groupTable,
        .nullable = false,
    });
    ctx.rank = item.addColumn(int64Column("rank"));
    ctx.subrank = item.addColumn(int64Column("subrank"));
    ctx.start = item.addColumn(int64Column("start"));
    ctx.end = item.addColumn(int64Column("end"));
    ctx.orderIndex = item.addOrderedIndex(OrderedIndexDefinition {
        .debugName = "itemOrder",
        .groupBy = {ctx.itemParent.column()},
        .orderBy = {ctx.rank, ctx.subrank},
    });
    ctx.intervalIndex = item.addIntervalIndex(IntervalIndexDefinition {
        .debugName = "itemInterval",
        .groupBy = {ctx.itemParent.column()},
        .start = ctx.start,
        .end = ctx.end,
    });

    ctx.schema = builder.freeze();
    ctx.engine = std::make_unique<DocumentEngine>(ctx.schema);
    {
        auto txn = ctx.engine->beginTransaction();
        ctx.groupA = txn.insert(ctx.groupTable, {{.column = ctx.groupName, .value = Value("A")}});
        ctx.groupB = txn.insert(ctx.groupTable, {{.column = ctx.groupName, .value = Value("B")}});
        txn.commit();
    }
    return ctx;
}

ItemId insertItem(SpecialIndexContext &ctx,
                  ItemId group,
                  std::int64_t rank,
                  std::int64_t subrank,
                  std::int64_t start,
                  std::int64_t end)
{
    auto txn = ctx.engine->beginTransaction();
    auto id = txn.insert(ctx.itemTable, {
        {.column = ctx.itemParent.column(), .value = idValue(group)},
        {.column = ctx.rank, .value = Value(rank)},
        {.column = ctx.subrank, .value = Value(subrank)},
        {.column = ctx.start, .value = Value(start)},
        {.column = ctx.end, .value = Value(end)},
    });
    txn.commit();
    return id;
}

struct OrderedRangeContext {
    EngineSchema schema;
    std::unique_ptr<DocumentEngine> engine;
    TableHandle parentTable;
    TableHandle itemTable;
    ColumnHandle parentName;
    RelationHandle itemParent;
    ColumnHandle value;
    ItemId parentA = 0;
    ItemId parentB = 0;
    std::vector<ItemId> parentAItems;
    ItemId parentBItem = 0;
};

OrderedRangeContext createOrderedRangeContext()
{
    OrderedRangeContext ctx;
    SchemaBuilder builder;
    auto parent = builder.createTable("RangeParent");
    ctx.parentTable = parent.handle();
    ctx.parentName = parent.addColumn(stringColumn("name"));

    auto item = builder.createTable("RangeItem");
    ctx.itemTable = item.handle();
    ctx.itemParent = item.addAssociation(AssociationDefinition {
        .debugName = "parent",
        .target = ctx.parentTable,
        .nullable = false,
    });
    ctx.value = item.addColumn(int64Column("value", IndexKind::Normal));
    item.addOrderedIndex(OrderedIndexDefinition {
        .debugName = "rangeOrder",
        .groupBy = {ctx.itemParent.column()},
        .orderBy = {ctx.value},
        .tieBreakById = true,
    });

    ctx.schema = builder.freeze();
    ctx.engine = std::make_unique<DocumentEngine>(ctx.schema);
    auto txn = ctx.engine->beginTransaction();
    ctx.parentA = txn.insert(ctx.parentTable, {{.column = ctx.parentName, .value = Value("A")}});
    ctx.parentB = txn.insert(ctx.parentTable, {{.column = ctx.parentName, .value = Value("B")}});
    for (const auto value : {std::int64_t {0}, std::int64_t {10}, std::int64_t {10},
                             std::int64_t {20}, std::int64_t {30}, std::int64_t {40},
                             std::int64_t {50}, std::int64_t {60}, std::int64_t {70},
                             std::int64_t {80}, std::int64_t {90}}) {
        ctx.parentAItems.push_back(txn.insert(ctx.itemTable, {
            {.column = ctx.itemParent.column(), .value = idValue(ctx.parentA)},
            {.column = ctx.value, .value = Value(value)},
        }));
    }
    ctx.parentBItem = txn.insert(ctx.itemTable, {
        {.column = ctx.itemParent.column(), .value = idValue(ctx.parentB)},
        {.column = ctx.value, .value = Value(std::int64_t {20})},
    });
    txn.commit();
    return ctx;
}

FilterExpression parentFilter(const OrderedRangeContext &ctx, ItemId parent)
{
    return FilterExpression(Filter(FieldRef::parent(ctx.itemParent),
                                   ComparisonOperator::Equal,
                                   idValue(parent)));
}

FilterExpression valueFilter(const OrderedRangeContext &ctx,
                             ComparisonOperator op,
                             std::int64_t value)
{
    return FilterExpression(Filter(FieldRef::column(ctx.value), op, Value(value)));
}

std::vector<ItemSnapshot> orderedRangeRows(const OrderedRangeContext &ctx,
                                           FilterExpression filter,
                                           SortDirection direction = SortDirection::Ascending)
{
    return ctx.engine->query(ctx.itemTable, {
        .filter = std::move(filter),
        .sortKeys = {
            {.field = FieldRef::column(ctx.value), .direction = direction},
            {.field = FieldRef::id(), .direction = direction},
        },
    }).toVector();
}

std::vector<std::int64_t> valuesOf(const OrderedRangeContext &ctx,
                                   const std::vector<ItemSnapshot> &rows)
{
    std::vector<std::int64_t> result;
    result.reserve(rows.size());
    for (const auto &row : rows) {
        result.push_back(ctx.engine->read(row.id, ctx.value).asInt64());
    }
    return result;
}

TEST(DocumentEngineSpecialIndexTest, SchemaValidationRejectsInvalidOrderedAndIntervalIndexes)
{
    EXPECT_THROW(([] {
        SchemaBuilder builder;
        auto table = builder.createTable("Item");
        auto value = table.addColumn(int64Column("value"));
        table.addOrderedIndex({.debugName = "bad", .orderBy = {}});
        (void)value;
        (void)builder.freeze();
    }()), SchemaError);

    EXPECT_THROW(([] {
        SchemaBuilder builder;
        auto table = builder.createTable("Item");
        auto start = table.addColumn(stringColumn("start"));
        auto end = table.addColumn(int64Column("end"));
        table.addIntervalIndex({.debugName = "bad", .start = start, .end = end});
        (void)builder.freeze();
    }()), SchemaError);

    EXPECT_THROW(([] {
        SchemaBuilder builder;
        auto other = builder.createTable("Other");
        auto otherValue = other.addColumn(int64Column("value"));
        auto table = builder.createTable("Item");
        auto value = table.addColumn(int64Column("value"));
        table.addOrderedIndex({.debugName = "bad", .groupBy = {otherValue}, .orderBy = {value}});
        (void)builder.freeze();
    }()), SchemaError);
}

TEST(DocumentEngineSpecialIndexTest, OrderedIndexFindsNeighborsByCompositeOrderAndGroup)
{
    auto ctx = createSpecialIndexContext();
    const auto first = insertItem(ctx, ctx.groupA, 1, 10, 0, 10);
    const auto second = insertItem(ctx, ctx.groupA, 1, 20, 10, 20);
    const auto third = insertItem(ctx, ctx.groupA, 2, 0, 20, 30);
    const auto otherGroup = insertItem(ctx, ctx.groupB, 1, 15, 0, 10);

    const auto secondSnapshot = ctx.engine->read(second);
    auto previous = ctx.engine->previous(ctx.orderIndex, secondSnapshot);
    auto next = ctx.engine->next(ctx.orderIndex, secondSnapshot);
    ASSERT_TRUE(previous.has_value());
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(previous->id, first);
    EXPECT_EQ(next->id, third);

    previous = ctx.engine->previous(ctx.orderIndex, secondSnapshot, {first});
    EXPECT_FALSE(previous.has_value());
    EXPECT_FALSE(ctx.engine->next(ctx.orderIndex, ctx.engine->read(otherGroup)).has_value());
}

TEST(DocumentEngineSpecialIndexTest, OrderedIndexReflectsUpdateAndRemove)
{
    auto ctx = createSpecialIndexContext();
    const auto first = insertItem(ctx, ctx.groupA, 1, 0, 0, 10);
    const auto second = insertItem(ctx, ctx.groupA, 2, 0, 10, 20);
    const auto third = insertItem(ctx, ctx.groupA, 3, 0, 20, 30);

    {
        auto txn = ctx.engine->beginTransaction();
        txn.update(third, ctx.rank, Value(std::int64_t {0}));
        txn.commit();
    }
    auto previous = ctx.engine->previous(ctx.orderIndex, ctx.engine->read(first));
    ASSERT_TRUE(previous.has_value());
    EXPECT_EQ(previous->id, third);

    {
        auto txn = ctx.engine->beginTransaction();
        txn.remove(first);
        txn.commit();
    }
    auto next = ctx.engine->next(ctx.orderIndex, ctx.engine->read(third));
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->id, second);
}

TEST(DocumentEngineSpecialIndexTest, OrderedIndexStreamsCompositeSortForLimitedQuery)
{
    auto ctx = createSpecialIndexContext();
    const auto late = insertItem(ctx, ctx.groupA, 2, 0, 0, 10);
    const auto first = insertItem(ctx, ctx.groupA, 1, 10, 10, 20);
    const auto second = insertItem(ctx, ctx.groupA, 1, 20, 20, 30);
    insertItem(ctx, ctx.groupB, 1, 0, 0, 10);

    auto rows = ctx.engine->query(ctx.itemTable, {
        .filter = FilterExpression(Filter(FieldRef::parent(ctx.itemParent),
                                          ComparisonOperator::Equal,
                                          idValue(ctx.groupA))),
        .sortKeys = {
            {.field = FieldRef::column(ctx.rank), .direction = SortDirection::Ascending},
            {.field = FieldRef::column(ctx.subrank), .direction = SortDirection::Ascending},
        },
    }).limit(2).toVector();

    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[0].id, first);
    EXPECT_EQ(rows[1].id, second);
    (void)late;
}

TEST(DocumentEngineSpecialIndexTest, OrderedRangePlanTightensBoundsAndHonorsGroupOrderAndWindow)
{
    auto ctx = createOrderedRangeContext();
    auto filter = FilterExpression::all({
        parentFilter(ctx, ctx.parentA),
        valueFilter(ctx, ComparisonOperator::GreaterOrEqual, 10),
        valueFilter(ctx, ComparisonOperator::GreaterOrEqual, 20),
        valueFilter(ctx, ComparisonOperator::GreaterOrEqual, 20),
        valueFilter(ctx, ComparisonOperator::LessOrEqual, 40),
        valueFilter(ctx, ComparisonOperator::LessOrEqual, 40),
        valueFilter(ctx, ComparisonOperator::Less, 50),
    });

    const auto ascending = orderedRangeRows(ctx, filter);
    EXPECT_EQ(valuesOf(ctx, ascending), (std::vector<std::int64_t> {20, 30, 40}));
    EXPECT_FALSE(containsId(ascending, ctx.parentBItem));

    const auto descending = orderedRangeRows(ctx, filter, SortDirection::Descending);
    EXPECT_EQ(valuesOf(ctx, descending), (std::vector<std::int64_t> {40, 30, 20}));

    auto window = ctx.engine->query(ctx.itemTable, {
        .filter = filter,
        .sortKeys = {
            {.field = FieldRef::column(ctx.value), .direction = SortDirection::Ascending},
            {.field = FieldRef::id(), .direction = SortDirection::Ascending},
        },
    }).offset(1).limit(1).toVector();
    ASSERT_EQ(window.size(), 1U);
    EXPECT_EQ(ctx.engine->read(window.front().id, ctx.value).asInt64(), 30);

    const auto duplicateRows = orderedRangeRows(ctx, FilterExpression::all({
        parentFilter(ctx, ctx.parentA),
        valueFilter(ctx, ComparisonOperator::Equal, 10),
    }));
    ASSERT_EQ(duplicateRows.size(), 2U);
    EXPECT_LT(duplicateRows[0].id, duplicateRows[1].id);
    const auto reverseDuplicateRows = orderedRangeRows(ctx,
                                                       FilterExpression::all({
                                                           parentFilter(ctx, ctx.parentA),
                                                           valueFilter(ctx, ComparisonOperator::Equal, 10),
                                                       }),
                                                       SortDirection::Descending);
    ASSERT_EQ(reverseDuplicateRows.size(), 2U);
    EXPECT_GT(reverseDuplicateRows[0].id, reverseDuplicateRows[1].id);
}

TEST(DocumentEngineSpecialIndexTest, OrderedRangePlanSimplifiesBooleanRangeSets)
{
    auto ctx = createOrderedRangeContext();

    const auto contradictory = orderedRangeRows(ctx, FilterExpression::all({
        parentFilter(ctx, ctx.parentA),
        valueFilter(ctx, ComparisonOperator::GreaterOrEqual, 50),
        valueFilter(ctx, ComparisonOperator::Less, 50),
    }));
    EXPECT_TRUE(contradictory.empty());

    const auto exclusiveLower = orderedRangeRows(ctx, FilterExpression::all({
        parentFilter(ctx, ctx.parentA),
        valueFilter(ctx, ComparisonOperator::Greater, 20),
        valueFilter(ctx, ComparisonOperator::LessOrEqual, 40),
    }));
    EXPECT_EQ(valuesOf(ctx, exclusiveLower), (std::vector<std::int64_t> {30, 40}));

    const auto overlappingOr = orderedRangeRows(ctx, FilterExpression::all({
        parentFilter(ctx, ctx.parentA),
        FilterExpression::any({
            FilterExpression::all({
                valueFilter(ctx, ComparisonOperator::GreaterOrEqual, 10),
                valueFilter(ctx, ComparisonOperator::Less, 30),
            }),
            FilterExpression::all({
                valueFilter(ctx, ComparisonOperator::GreaterOrEqual, 20),
                valueFilter(ctx, ComparisonOperator::LessOrEqual, 40),
            }),
        }),
    }));
    EXPECT_EQ(valuesOf(ctx, overlappingOr),
              (std::vector<std::int64_t> {10, 10, 20, 30, 40}));

    const auto adjacentOr = orderedRangeRows(ctx, FilterExpression::all({
        parentFilter(ctx, ctx.parentA),
        FilterExpression::any({
            FilterExpression::all({
                valueFilter(ctx, ComparisonOperator::GreaterOrEqual, 50),
                valueFilter(ctx, ComparisonOperator::Less, 60),
            }),
            FilterExpression::all({
                valueFilter(ctx, ComparisonOperator::GreaterOrEqual, 60),
                valueFilter(ctx, ComparisonOperator::Less, 70),
            }),
        }),
    }));
    EXPECT_EQ(valuesOf(ctx, adjacentOr), (std::vector<std::int64_t> {50, 60}));

    const auto notEqual = orderedRangeRows(ctx, FilterExpression::all({
        parentFilter(ctx, ctx.parentA),
        valueFilter(ctx, ComparisonOperator::NotEqual, 20),
    }));
    EXPECT_EQ(notEqual.size(), ctx.parentAItems.size() - 1);
    const auto notEqualValues = valuesOf(ctx, notEqual);
    EXPECT_EQ(std::find(notEqualValues.begin(), notEqualValues.end(), 20), notEqualValues.end());

    const auto nestedNot = orderedRangeRows(ctx, FilterExpression::all({
        parentFilter(ctx, ctx.parentA),
        FilterExpression::negate(FilterExpression::any({
            valueFilter(ctx, ComparisonOperator::Less, 20),
            valueFilter(ctx, ComparisonOperator::GreaterOrEqual, 50),
        })),
    }));
    EXPECT_EQ(valuesOf(ctx, nestedNot), (std::vector<std::int64_t> {20, 30, 40}));

    const auto fullRange = orderedRangeRows(ctx, FilterExpression::all({
        parentFilter(ctx, ctx.parentA),
        FilterExpression::any({
            valueFilter(ctx, ComparisonOperator::Less, 30),
            valueFilter(ctx, ComparisonOperator::GreaterOrEqual, 30),
        }),
    }));
    EXPECT_EQ(fullRange.size(), ctx.parentAItems.size());
}

TEST(DocumentEngineSpecialIndexTest, OrderedRangePlanUsesEightLeafBudgetAndFallsBackBeyondIt)
{
    auto ctx = createOrderedRangeContext();
    std::vector<FilterExpression> eightValues;
    for (const auto value : {std::int64_t {0}, std::int64_t {20}, std::int64_t {30},
                             std::int64_t {40}, std::int64_t {50}, std::int64_t {60},
                             std::int64_t {70}, std::int64_t {80}}) {
        eightValues.push_back(valueFilter(ctx, ComparisonOperator::Equal, value));
    }
    const auto budgetBoundary = orderedRangeRows(ctx, FilterExpression::all({
        parentFilter(ctx, ctx.parentA),
        FilterExpression::any(eightValues),
    }));
    EXPECT_EQ(valuesOf(ctx, budgetBoundary),
              (std::vector<std::int64_t> {0, 20, 30, 40, 50, 60, 70, 80}));

    eightValues.push_back(valueFilter(ctx, ComparisonOperator::Equal, 90));
    const auto overBudgetFallback = orderedRangeRows(ctx, FilterExpression::all({
        parentFilter(ctx, ctx.parentA),
        FilterExpression::any(eightValues),
    }));
    EXPECT_EQ(valuesOf(ctx, overBudgetFallback),
              (std::vector<std::int64_t> {0, 20, 30, 40, 50, 60, 70, 80, 90}));

    std::vector<FilterExpression> eightIntervals {parentFilter(ctx, ctx.parentA)};
    for (std::int64_t value = 10; value <= 70; value += 10) {
        eightIntervals.push_back(valueFilter(ctx, ComparisonOperator::NotEqual, value));
    }
    const auto intervalBudgetBoundary = orderedRangeRows(ctx,
                                                         FilterExpression::all(eightIntervals));
    EXPECT_EQ(valuesOf(ctx, intervalBudgetBoundary),
              (std::vector<std::int64_t> {0, 80, 90}));

    eightIntervals.push_back(valueFilter(ctx, ComparisonOperator::NotEqual, 80));
    const auto intervalOverBudgetFallback = orderedRangeRows(ctx,
                                                             FilterExpression::all(eightIntervals));
    EXPECT_EQ(valuesOf(ctx, intervalOverBudgetFallback),
              (std::vector<std::int64_t> {0, 90}));
}

TEST(DocumentEngineSpecialIndexTest, IntervalIndexUsesHalfOpenOverlapSemanticsAndGroups)
{
    auto ctx = createSpecialIndexContext();
    const auto base = insertItem(ctx, ctx.groupA, 1, 0, 0, 10);
    const auto touching = insertItem(ctx, ctx.groupA, 2, 0, 10, 20);
    const auto overlapping = insertItem(ctx, ctx.groupA, 3, 0, 5, 15);
    const auto otherGroup = insertItem(ctx, ctx.groupB, 1, 0, 5, 15);

    auto overlaps = ctx.engine->overlapping(ctx.intervalIndex, ctx.engine->read(base));
    ASSERT_EQ(overlaps.size(), 1U);
    EXPECT_EQ(overlaps.front().id, overlapping);
    EXPECT_FALSE(containsId(overlaps, touching));
    EXPECT_FALSE(containsId(overlaps, otherGroup));

    overlaps = ctx.engine->overlapping(ctx.intervalIndex, ctx.engine->read(base), {overlapping});
    EXPECT_TRUE(overlaps.empty());
}

TEST(DocumentEngineSpecialIndexTest, IntervalIndexReflectsUpdateAndRemove)
{
    auto ctx = createSpecialIndexContext();
    const auto base = insertItem(ctx, ctx.groupA, 1, 0, 0, 10);
    const auto moving = insertItem(ctx, ctx.groupA, 2, 0, 20, 30);

    EXPECT_TRUE(ctx.engine->overlapping(ctx.intervalIndex, ctx.engine->read(base)).empty());
    {
        auto txn = ctx.engine->beginTransaction();
        txn.update(moving, {
            {.column = ctx.start, .value = Value(std::int64_t {5})},
            {.column = ctx.end, .value = Value(std::int64_t {15})},
        });
        txn.commit();
    }
    EXPECT_EQ(ctx.engine->overlapping(ctx.intervalIndex, ctx.engine->read(base)).size(), 1U);

    {
        auto txn = ctx.engine->beginTransaction();
        txn.remove(moving);
        txn.commit();
    }
    EXPECT_TRUE(ctx.engine->overlapping(ctx.intervalIndex, ctx.engine->read(base)).empty());
}

TEST(DocumentEngineSpecialIndexTest, IntervalIndexCanServeOverlapShapedQuery)
{
    auto ctx = createSpecialIndexContext();
    const auto base = insertItem(ctx, ctx.groupA, 1, 0, 0, 10);
    const auto overlapping = insertItem(ctx, ctx.groupA, 2, 0, 5, 15);
    insertItem(ctx, ctx.groupA, 3, 0, 10, 20);

    auto rows = ctx.engine->query(ctx.itemTable, {
        .filter = FilterExpression::all({
            FilterExpression(Filter(FieldRef::parent(ctx.itemParent),
                                    ComparisonOperator::Equal,
                                    idValue(ctx.groupA))),
            FilterExpression(Filter(FieldRef::column(ctx.start),
                                    ComparisonOperator::Less,
                                    Value(std::int64_t {10}))),
            FilterExpression(Filter(FieldRef::column(ctx.end),
                                    ComparisonOperator::Greater,
                                    Value(std::int64_t {0}))),
        }),
    }).toVector();

    EXPECT_TRUE(containsId(rows, base));
    EXPECT_TRUE(containsId(rows, overlapping));
    EXPECT_EQ(rows.size(), 2U);
}

TEST(DocumentEngineSpecialIndexTest, IntervalCandidatesUseEquivalentSmallAndLargeOrderedOutputPaths)
{
    auto ctx = createSpecialIndexContext();
    insertItem(ctx, ctx.groupA, 2, 0, 0, 10);
    insertItem(ctx, ctx.groupA, 1, 0, 10, 20);
    for (std::int64_t rank = 3; rank < 9; ++rank) {
        insertItem(ctx, ctx.groupA, rank, 0, 100 + rank * 10, 109 + rank * 10);
    }
    insertItem(ctx, ctx.groupB, 2, 0, 0, 10);
    insertItem(ctx, ctx.groupB, 1, 0, 10, 20);

    const auto queryGroup = [&](ItemId group) {
        return ctx.engine->query(ctx.itemTable, {
            .filter = FilterExpression::all({
                FilterExpression(Filter(FieldRef::parent(ctx.itemParent),
                                        ComparisonOperator::Equal,
                                        idValue(group))),
                FilterExpression(Filter(FieldRef::column(ctx.start),
                                        ComparisonOperator::Less,
                                        Value(std::int64_t {20}))),
                FilterExpression(Filter(FieldRef::column(ctx.end),
                                        ComparisonOperator::Greater,
                                        Value(std::int64_t {0}))),
            }),
            .sortKeys = {
                {.field = FieldRef::column(ctx.rank), .direction = SortDirection::Ascending},
                {.field = FieldRef::column(ctx.subrank), .direction = SortDirection::Ascending},
                {.field = FieldRef::id(), .direction = SortDirection::Ascending},
            },
        }).toVector();
    };

    const auto smallCandidatePath = queryGroup(ctx.groupA);
    const auto largeCandidatePath = queryGroup(ctx.groupB);
    ASSERT_EQ(smallCandidatePath.size(), 2U);
    ASSERT_EQ(largeCandidatePath.size(), 2U);
    for (std::size_t i = 0; i < 2; ++i) {
        EXPECT_EQ(ctx.engine->read(smallCandidatePath[i].id, ctx.rank).asInt64(),
                  ctx.engine->read(largeCandidatePath[i].id, ctx.rank).asInt64());
        EXPECT_EQ(ctx.engine->read(smallCandidatePath[i].id, ctx.subrank).asInt64(),
                  ctx.engine->read(largeCandidatePath[i].id, ctx.subrank).asInt64());
    }
    EXPECT_EQ(ctx.engine->read(smallCandidatePath[0].id, ctx.rank).asInt64(), 1);
    EXPECT_EQ(ctx.engine->read(smallCandidatePath[1].id, ctx.rank).asInt64(), 2);
}

} // namespace
