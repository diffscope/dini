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

} // namespace
