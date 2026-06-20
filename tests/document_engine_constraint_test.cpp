#include <cstdint>
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

Value idValue(ItemId id)
{
    return Value(static_cast<std::uint64_t>(id));
}

struct TestSchema {
    EngineSchema schema;
    TableHandle itemTable;
    ColumnHandle itemName;
    ColumnHandle itemValue;
    ColumnHandle itemNotes;
    ColumnHandle itemCode;
    TableHandle polyItemTable;
    VariantHandle polyVariantX;
    VariantHandle polyVariantY;
    ColumnHandle polyXVal;
    ColumnHandle polyYVal;
};

TestSchema createTestSchema()
{
    TestSchema ts;

    SchemaBuilder builder;

    auto itemTable = builder.createTable("Item");
    ts.itemName = itemTable.addColumn(stringColumn("name", IndexKind::Normal));
    ts.itemValue = itemTable.addColumn(ColumnDefinition {
        .debugName = "value",
        .type = ValueType::Int64,
        .index = IndexKind::Normal,
        .defaultValue = Value(std::int64_t {0}),
        .nullable = false,
        .check = [](const Value &value) {
            const auto v = value.asInt64();
            return v >= 0 && v <= 100;
        },
    });
    ts.itemNotes = itemTable.addColumn(stringColumn("notes", IndexKind::None));
    ts.itemCode = itemTable.addColumn(ColumnDefinition {
        .debugName = "code",
        .type = ValueType::String,
        .index = IndexKind::Unique,
        .defaultValue = Value(""),
        .nullable = false,
    });
    ts.itemTable = itemTable.handle();

    auto polyItemTable = builder.createTable("PolyItem");
    ts.polyVariantX = polyItemTable.addVariant("X");
    ts.polyVariantY = polyItemTable.addVariant("Y");
    ts.polyXVal = polyItemTable.addVariantColumn(VariantColumnDefinition {
        .debugName = "x_val",
        .variant = ts.polyVariantX,
        .type = ValueType::Int64,
        .index = IndexKind::Normal,
        .defaultValue = Value(std::int64_t {0}),
        .nullable = false,
    });
    ts.polyYVal = polyItemTable.addVariantColumn(VariantColumnDefinition {
        .debugName = "y_val",
        .variant = ts.polyVariantY,
        .type = ValueType::Int64,
        .index = IndexKind::Normal,
        .defaultValue = Value(std::int64_t {0}),
        .nullable = false,
    });
    ts.polyItemTable = polyItemTable.handle();

    ts.schema = builder.freeze();
    return ts;
}

struct TestSchema2 {
    EngineSchema schema;
    TableHandle otherTable;
    ColumnHandle otherCol;
};

TestSchema2 createTestSchema2()
{
    TestSchema2 ts2;

    SchemaBuilder builder;
    auto otherTable = builder.createTable("Other");
    ts2.otherCol = otherTable.addColumn(stringColumn("label", IndexKind::Normal));
    ts2.otherTable = otherTable.handle();

    ts2.schema = builder.freeze();
    return ts2;
}

} // namespace

TEST(DocumentEngineConstraintTest, CheckPredicateAcceptsValidValue)
{
    auto ts = createTestSchema();
    DocumentEngine engine(ts.schema);

    ItemId itemId = 0;
    {
        auto tx = engine.beginTransaction();
        itemId = tx.insert(ts.itemTable, {
            ColumnValue {.column = ts.itemName, .value = Value("test")},
            ColumnValue {.column = ts.itemValue, .value = Value(std::int64_t {10})},
            ColumnValue {.column = ts.itemCode, .value = Value("A")},
        });
        tx.commit();
    }

    {
        auto tx = engine.beginTransaction();
        tx.update(itemId, ts.itemValue, Value(std::int64_t {50}));
        tx.commit();
    }

    EXPECT_EQ(engine.read(itemId, ts.itemValue).asInt64(), 50);
}

TEST(DocumentEngineConstraintTest, CheckPredicateRejectsInvalidValue)
{
    auto ts = createTestSchema();
    DocumentEngine engine(ts.schema);

    ItemId itemId = 0;
    {
        auto tx = engine.beginTransaction();
        itemId = tx.insert(ts.itemTable, {
            ColumnValue {.column = ts.itemName, .value = Value("test")},
            ColumnValue {.column = ts.itemValue, .value = Value(std::int64_t {10})},
            ColumnValue {.column = ts.itemCode, .value = Value("A")},
        });
        tx.commit();
    }

    auto tx = engine.beginTransaction();
    EXPECT_THROW(
        tx.update(itemId, ts.itemValue, Value(std::int64_t {200})),
        ConstraintError);
}

TEST(DocumentEngineConstraintTest, CheckOnInsertRejects)
{
    auto ts = createTestSchema();
    DocumentEngine engine(ts.schema);

    auto tx = engine.beginTransaction();
    EXPECT_THROW(
        tx.insert(ts.itemTable, {
            ColumnValue {.column = ts.itemName, .value = Value("bad")},
            ColumnValue {.column = ts.itemValue, .value = Value(std::int64_t {-5})},
            ColumnValue {.column = ts.itemCode, .value = Value("X")},
        }),
        ConstraintError);
}

TEST(DocumentEngineConstraintTest, ReadVariantColumnOnWrongVariant)
{
    auto ts = createTestSchema();
    DocumentEngine engine(ts.schema);

    ItemId yItem = 0;
    {
        auto tx = engine.beginTransaction();
        yItem = tx.insert(ts.polyItemTable, {
            ColumnValue {.column = ts.polyYVal, .value = Value(std::int64_t {42})},
        }, ts.polyVariantY);
        tx.commit();
    }

    EXPECT_THROW(engine.read(yItem, ts.polyXVal), DiniError);
}

TEST(DocumentEngineConstraintTest, WriteVariantColumnOnWrongVariant)
{
    auto ts = createTestSchema();
    DocumentEngine engine(ts.schema);

    ItemId yItem = 0;
    {
        auto tx = engine.beginTransaction();
        yItem = tx.insert(ts.polyItemTable, {
            ColumnValue {.column = ts.polyYVal, .value = Value(std::int64_t {42})},
        }, ts.polyVariantY);
        tx.commit();
    }

    auto tx = engine.beginTransaction();
    EXPECT_THROW(
        tx.update(yItem, ts.polyXVal, Value(std::int64_t {99})),
        DiniError);
}

TEST(DocumentEngineConstraintTest, HandleCrossSchemaUse)
{
    auto ts = createTestSchema();
    auto ts2 = createTestSchema2();

    DocumentEngine engine1(ts.schema);
    DocumentEngine engine2(ts2.schema);

    EXPECT_THROW(engine2.view(ts.itemTable), HandleError);
}

TEST(DocumentEngineConstraintTest, HandleCrossContainerUse)
{
    auto ts = createTestSchema();
    DocumentEngine engine(ts.schema);

    ListHandle fakeList(ts.schema.schemaId(), ts.itemTable.containerId(), "fake");

    EXPECT_THROW(engine.view(fakeList), HandleError);
}

TEST(DocumentEngineConstraintTest, QueryNonIndexedColumn)
{
    auto ts = createTestSchema();
    DocumentEngine engine(ts.schema);

    {
        auto tx = engine.beginTransaction();
        tx.insert(ts.itemTable, {
            ColumnValue {.column = ts.itemName, .value = Value("test")},
            ColumnValue {.column = ts.itemValue, .value = Value(std::int64_t {10})},
            ColumnValue {.column = ts.itemNotes, .value = Value("some notes")},
            ColumnValue {.column = ts.itemCode, .value = Value("A")},
        });
        tx.commit();
    }

    const QuerySpec spec {
        .filter = FilterExpression(Filter(
            FieldRef::column(ts.itemNotes),
            ComparisonOperator::Equal,
            Value("some notes"))),
    };

    EXPECT_THROW(engine.query(ts.itemTable, spec), QueryError);
}

TEST(DocumentEngineConstraintTest, SortNonIndexedColumn)
{
    auto ts = createTestSchema();
    DocumentEngine engine(ts.schema);

    const QuerySpec spec {
        .sortKeys = {
            SortKey {
                .field = FieldRef::column(ts.itemNotes),
                .direction = SortDirection::Ascending,
            },
        },
    };

    EXPECT_THROW(engine.query(ts.itemTable, spec), QueryError);
}

TEST(DocumentEngineConstraintTest, AggregateGroupByNonIndexedColumn)
{
    auto ts = createTestSchema();
    DocumentEngine engine(ts.schema);

    {
        auto tx = engine.beginTransaction();
        tx.insert(ts.itemTable, {
            ColumnValue {.column = ts.itemName, .value = Value("test")},
            ColumnValue {.column = ts.itemValue, .value = Value(std::int64_t {10})},
            ColumnValue {.column = ts.itemNotes, .value = Value("some notes")},
            ColumnValue {.column = ts.itemCode, .value = Value("A")},
        });
        tx.commit();
    }

    const AggregationSpec spec {
        .kind = AggregateKind::Count,
        .groupBy = FieldRef::column(ts.itemNotes),
    };

    EXPECT_THROW(engine.view(ts.itemTable).aggregate(spec), QueryError);
}

TEST(DocumentEngineConstraintTest, UniqueViolationOnUpdate)
{
    auto ts = createTestSchema();
    DocumentEngine engine(ts.schema);

    ItemId item1 = 0;
    ItemId item2 = 0;
    {
        auto tx = engine.beginTransaction();
        item1 = tx.insert(ts.itemTable, {
            ColumnValue {.column = ts.itemName, .value = Value("first")},
            ColumnValue {.column = ts.itemValue, .value = Value(std::int64_t {10})},
            ColumnValue {.column = ts.itemCode, .value = Value("ABC")},
        });
        item2 = tx.insert(ts.itemTable, {
            ColumnValue {.column = ts.itemName, .value = Value("second")},
            ColumnValue {.column = ts.itemValue, .value = Value(std::int64_t {20})},
            ColumnValue {.column = ts.itemCode, .value = Value("XYZ")},
        });
        tx.commit();
    }

    auto tx = engine.beginTransaction();
    EXPECT_THROW(
        tx.update(item2, ts.itemCode, Value("ABC")),
        ConstraintError);
}

TEST(DocumentEngineConstraintTest, UniqueViolationOnInsert)
{
    auto ts = createTestSchema();
    DocumentEngine engine(ts.schema);

    {
        auto tx = engine.beginTransaction();
        tx.insert(ts.itemTable, {
            ColumnValue {.column = ts.itemName, .value = Value("first")},
            ColumnValue {.column = ts.itemValue, .value = Value(std::int64_t {10})},
            ColumnValue {.column = ts.itemCode, .value = Value("UNIQUE")},
        });
        tx.commit();
    }

    auto tx = engine.beginTransaction();
    EXPECT_THROW(
        tx.insert(ts.itemTable, {
            ColumnValue {.column = ts.itemName, .value = Value("second")},
            ColumnValue {.column = ts.itemValue, .value = Value(std::int64_t {20})},
            ColumnValue {.column = ts.itemCode, .value = Value("UNIQUE")},
        }),
        ConstraintError);
}
