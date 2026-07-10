#include <cstdint>
#include <vector>

#include <dini/engine.h>

#include <gtest/gtest.h>

namespace {

using namespace dini;

ColumnDefinition int64Column(const char *debugName, IndexKind index = IndexKind::None)
{
    return ColumnDefinition {
        .debugName = debugName,
        .type = ValueType::Int64,
        .index = index,
        .defaultValue = Value(std::int64_t{0}),
        .nullable = false,
    };
}

// ---------------------------------------------------------------------------
// ComputedColumnInitialValue
// ---------------------------------------------------------------------------

TEST(DocumentEngineComputedColumnTest, ComputedColumnInitialValue)
{
    SchemaBuilder builder;
    auto itemTable = builder.createTable("Item");

    auto a = itemTable.addColumn(int64Column("a", IndexKind::Normal));
    auto b = itemTable.addColumn(int64Column("b", IndexKind::Normal));

    auto sum_col = itemTable.addComputedColumn(ComputedColumnDefinition {
        .debugName = "sum_col",
        .type = ValueType::Int64,
        .index = IndexKind::Normal,
        .nullable = false,
        .dependsOn = {a, b},
        .compute = [](const std::vector<Value> &deps) -> Value {
            return Value(deps[0].asInt64() + deps[1].asInt64());
        },
    });

    auto product_col = itemTable.addComputedColumn(ComputedColumnDefinition {
        .debugName = "product_col",
        .type = ValueType::Int64,
        .index = IndexKind::Normal,
        .nullable = false,
        .dependsOn = {a, b, sum_col},
        .compute = [](const std::vector<Value> &deps) -> Value {
            return Value(deps[0].asInt64() * deps[1].asInt64() * deps[2].asInt64());
        },
    });

    auto schema = builder.freeze();
    DocumentEngine engine(schema);

    // --- Arrange & Act ---
    ItemId itemId = 0;
    {
        auto txn = engine.beginTransaction();
        itemId = txn.insert(itemTable.handle(), {
            ColumnValue{.column = a, .value = Value(std::int64_t{3})},
            ColumnValue{.column = b, .value = Value(std::int64_t{4})},
        });
        txn.commit();
    }

    // --- Assert ---
    EXPECT_EQ(engine.read(itemId, sum_col).asInt64(), 7);
    EXPECT_EQ(engine.read(itemId, product_col).asInt64(), 3 * 4 * 7);   // 84
}

// ---------------------------------------------------------------------------
// ComputedColumnDepUpdate
// ---------------------------------------------------------------------------

TEST(DocumentEngineComputedColumnTest, ComputedColumnDepUpdate)
{
    SchemaBuilder builder;
    auto itemTable = builder.createTable("Item");

    auto a = itemTable.addColumn(int64Column("a", IndexKind::Normal));
    auto b = itemTable.addColumn(int64Column("b", IndexKind::Normal));

    auto sum_col = itemTable.addComputedColumn(ComputedColumnDefinition {
        .debugName = "sum_col",
        .type = ValueType::Int64,
        .index = IndexKind::Normal,
        .nullable = false,
        .dependsOn = {a, b},
        .compute = [](const std::vector<Value> &deps) -> Value {
            return Value(deps[0].asInt64() + deps[1].asInt64());
        },
    });

    auto product_col = itemTable.addComputedColumn(ComputedColumnDefinition {
        .debugName = "product_col",
        .type = ValueType::Int64,
        .index = IndexKind::Normal,
        .nullable = false,
        .dependsOn = {a, b, sum_col},
        .compute = [](const std::vector<Value> &deps) -> Value {
            return Value(deps[0].asInt64() * deps[1].asInt64() * deps[2].asInt64());
        },
    });

    auto schema = builder.freeze();
    DocumentEngine engine(schema);

    // --- Arrange ---
    ItemId itemId = 0;
    {
        auto txn = engine.beginTransaction();
        itemId = txn.insert(itemTable.handle(), {
            ColumnValue{.column = a, .value = Value(std::int64_t{3})},
            ColumnValue{.column = b, .value = Value(std::int64_t{4})},
        });
        txn.commit();
    }

    // --- Act ---
    {
        auto txn = engine.beginTransaction();
        txn.update(itemId, a, Value(std::int64_t{5}));
        txn.commit();
    }

    // --- Assert ---
    EXPECT_EQ(engine.read(itemId, sum_col).asInt64(), 5 + 4);          // 9
    EXPECT_EQ(engine.read(itemId, product_col).asInt64(), 5 * 4 * 9);  // 180
}

// ---------------------------------------------------------------------------
// ComputedColumnMultiDep
// ---------------------------------------------------------------------------

TEST(DocumentEngineComputedColumnTest, ComputedColumnMultiDep)
{
    SchemaBuilder builder;
    auto itemTable = builder.createTable("Item");

    auto a = itemTable.addColumn(int64Column("a", IndexKind::Normal));
    auto b = itemTable.addColumn(int64Column("b", IndexKind::Normal));

    auto sum_col = itemTable.addComputedColumn(ComputedColumnDefinition {
        .debugName = "sum_col",
        .type = ValueType::Int64,
        .index = IndexKind::Normal,
        .nullable = false,
        .dependsOn = {a, b},
        .compute = [](const std::vector<Value> &deps) -> Value {
            return Value(deps[0].asInt64() + deps[1].asInt64());
        },
    });

    auto product_col = itemTable.addComputedColumn(ComputedColumnDefinition {
        .debugName = "product_col",
        .type = ValueType::Int64,
        .index = IndexKind::Normal,
        .nullable = false,
        .dependsOn = {a, b, sum_col},
        .compute = [](const std::vector<Value> &deps) -> Value {
            return Value(deps[0].asInt64() * deps[1].asInt64() * deps[2].asInt64());
        },
    });

    auto schema = builder.freeze();
    DocumentEngine engine(schema);

    // --- Arrange ---
    ItemId itemId = 0;
    {
        auto txn = engine.beginTransaction();
        itemId = txn.insert(itemTable.handle(), {
            ColumnValue{.column = a, .value = Value(std::int64_t{2})},
            ColumnValue{.column = b, .value = Value(std::int64_t{3})},
        });
        txn.commit();
    }

    EXPECT_EQ(engine.read(itemId, sum_col).asInt64(), 5);
    EXPECT_EQ(engine.read(itemId, product_col).asInt64(), 2 * 3 * 5);  // 30

    // --- Act: update b — sum_col re-computes, which then cascades into product_col ---
    {
        auto txn = engine.beginTransaction();
        txn.update(itemId, b, Value(std::int64_t{6}));
        txn.commit();
    }

    // --- Assert ---
    EXPECT_EQ(engine.read(itemId, sum_col).asInt64(), 2 + 6);           // 8
    EXPECT_EQ(engine.read(itemId, product_col).asInt64(), 2 * 6 * 8);   // 96
}

// ---------------------------------------------------------------------------
// ComputedColumnInChangeset
// ---------------------------------------------------------------------------

TEST(DocumentEngineComputedColumnTest, ComputedColumnInChangeset)
{
    SchemaBuilder builder;
    auto itemTable = builder.createTable("Item");

    auto a = itemTable.addColumn(int64Column("a", IndexKind::Normal));
    auto b = itemTable.addColumn(int64Column("b", IndexKind::Normal));

    auto sum_col = itemTable.addComputedColumn(ComputedColumnDefinition {
        .debugName = "sum_col",
        .type = ValueType::Int64,
        .index = IndexKind::Normal,
        .nullable = false,
        .dependsOn = {a, b},
        .compute = [](const std::vector<Value> &deps) -> Value {
            return Value(deps[0].asInt64() + deps[1].asInt64());
        },
    });

    auto schema = builder.freeze();
    DocumentEngine engine(schema);

    // --- Arrange ---
    ItemId itemId = 0;
    {
        auto txn = engine.beginTransaction();
        itemId = txn.insert(itemTable.handle(), {
            ColumnValue{.column = a, .value = Value(std::int64_t{3})},
            ColumnValue{.column = b, .value = Value(std::int64_t{4})},
        });
        txn.commit();
    }

    // --- Act ---
    CommitResult result;
    {
        auto txn = engine.beginTransaction();
        txn.update(itemId, a, Value(std::int64_t{5}));
        result = txn.commit();
    }

    // --- Assert ---
    bool foundComputedUpdate = false;
    for (const auto &op : result.changeSet.operations())
    {
        if (op.kind() == ChangeOperationKind::ComputedColumnUpdated)
        {
            auto payload = std::get<ComputedColumnUpdatedChange>(op.payload());
            EXPECT_EQ(payload.itemId, itemId);
            foundComputedUpdate = true;
        }
    }
    EXPECT_TRUE(foundComputedUpdate);
}

// ---------------------------------------------------------------------------
// ComputedColumnUndoRedo
// ---------------------------------------------------------------------------

TEST(DocumentEngineComputedColumnTest, ComputedColumnUndoRedo)
{
    SchemaBuilder builder;
    auto itemTable = builder.createTable("Item");

    auto a = itemTable.addColumn(int64Column("a", IndexKind::Normal));
    auto b = itemTable.addColumn(int64Column("b", IndexKind::Normal));

    auto sum_col = itemTable.addComputedColumn(ComputedColumnDefinition {
        .debugName = "sum_col",
        .type = ValueType::Int64,
        .index = IndexKind::Normal,
        .nullable = false,
        .dependsOn = {a, b},
        .compute = [](const std::vector<Value> &deps) -> Value {
            return Value(deps[0].asInt64() + deps[1].asInt64());
        },
    });

    auto product_col = itemTable.addComputedColumn(ComputedColumnDefinition {
        .debugName = "product_col",
        .type = ValueType::Int64,
        .index = IndexKind::Normal,
        .nullable = false,
        .dependsOn = {a, b, sum_col},
        .compute = [](const std::vector<Value> &deps) -> Value {
            return Value(deps[0].asInt64() * deps[1].asInt64() * deps[2].asInt64());
        },
    });

    auto schema = builder.freeze();
    DocumentEngine engine(schema);

    // --- Arrange ---
    ItemId itemId = 0;
    {
        auto txn = engine.beginTransaction();
        itemId = txn.insert(itemTable.handle(), {
            ColumnValue{.column = a, .value = Value(std::int64_t{3})},
            ColumnValue{.column = b, .value = Value(std::int64_t{4})},
        });
        txn.commit();
    }

    // --- Act: update a ---
    {
        auto txn = engine.beginTransaction();
        txn.update(itemId, a, Value(std::int64_t{5}));
        txn.commit();
    }

    EXPECT_EQ(engine.read(itemId, sum_col).asInt64(), 9);
    EXPECT_EQ(engine.read(itemId, product_col).asInt64(), 180);

    // --- Act: undo ---
    ASSERT_TRUE(engine.canUndo());
    engine.undo();

    // --- Assert: computed columns restored to old values ---
    EXPECT_EQ(engine.read(itemId, a).asInt64(), 3);
    EXPECT_EQ(engine.read(itemId, sum_col).asInt64(), 7);
    EXPECT_EQ(engine.read(itemId, product_col).asInt64(), 84);
    EXPECT_FALSE(engine.canUndo());
    ASSERT_TRUE(engine.canRedo());

    // --- Act: redo ---
    engine.redo();

    // --- Assert: computed columns restored to new values ---
    EXPECT_EQ(engine.read(itemId, a).asInt64(), 5);
    EXPECT_EQ(engine.read(itemId, sum_col).asInt64(), 9);
    EXPECT_EQ(engine.read(itemId, product_col).asInt64(), 180);
    EXPECT_TRUE(engine.canUndo());
    EXPECT_FALSE(engine.canRedo());
}

// ---------------------------------------------------------------------------
// ComputedColumnIndexQuery
// ---------------------------------------------------------------------------

TEST(DocumentEngineComputedColumnTest, ComputedColumnIndexQuery)
{
    SchemaBuilder builder;
    auto itemTable = builder.createTable("Item");

    auto a = itemTable.addColumn(int64Column("a", IndexKind::Normal));
    auto b = itemTable.addColumn(int64Column("b", IndexKind::Normal));

    auto sum_col = itemTable.addComputedColumn(ComputedColumnDefinition {
        .debugName = "sum_col",
        .type = ValueType::Int64,
        .index = IndexKind::Normal,
        .nullable = false,
        .dependsOn = {a, b},
        .compute = [](const std::vector<Value> &deps) -> Value {
            return Value(deps[0].asInt64() + deps[1].asInt64());
        },
    });

    auto schema = builder.freeze();
    DocumentEngine engine(schema);

    // --- Arrange: two items with different sums ---
    ItemId item1 = 0;
    ItemId item2 = 0;
    {
        auto txn = engine.beginTransaction();
        item1 = txn.insert(itemTable.handle(), {
            ColumnValue{.column = a, .value = Value(std::int64_t{3})},
            ColumnValue{.column = b, .value = Value(std::int64_t{4})},
        });
        item2 = txn.insert(itemTable.handle(), {
            ColumnValue{.column = a, .value = Value(std::int64_t{10})},
            ColumnValue{.column = b, .value = Value(std::int64_t{20})},
        });
        txn.commit();
    }

    // --- Act: query by sum_col ---
    const QuerySpec queryBySum {
        .filter = FilterExpression(Filter(
            FieldRef::column(sum_col),
            ComparisonOperator::Equal,
            Value(std::int64_t{7}))),
    };

    auto results = engine.query(itemTable.handle(), queryBySum).toVector();

    // --- Assert ---
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results.front().id, item1);
}

// ---------------------------------------------------------------------------
// ComputedColumnSnapshotRecovery
// ---------------------------------------------------------------------------

TEST(DocumentEngineComputedColumnTest, ComputedColumnSnapshotRecovery)
{
    SchemaBuilder builder;
    auto itemTable = builder.createTable("Item");

    auto a = itemTable.addColumn(int64Column("a", IndexKind::Normal));
    auto b = itemTable.addColumn(int64Column("b", IndexKind::Normal));

    auto sum_col = itemTable.addComputedColumn(ComputedColumnDefinition {
        .debugName = "sum_col",
        .type = ValueType::Int64,
        .index = IndexKind::Normal,
        .nullable = false,
        .dependsOn = {a, b},
        .compute = [](const std::vector<Value> &deps) -> Value {
            return Value(deps[0].asInt64() + deps[1].asInt64());
        },
    });

    auto product_col = itemTable.addComputedColumn(ComputedColumnDefinition {
        .debugName = "product_col",
        .type = ValueType::Int64,
        .index = IndexKind::Normal,
        .nullable = false,
        .dependsOn = {a, b, sum_col},
        .compute = [](const std::vector<Value> &deps) -> Value {
            return Value(deps[0].asInt64() * deps[1].asInt64() * deps[2].asInt64());
        },
    });

    auto schema = builder.freeze();
    DocumentEngine engine(schema);

    // --- Arrange ---
    ItemId itemId = 0;
    {
        auto txn = engine.beginTransaction();
        itemId = txn.insert(itemTable.handle(), {
            ColumnValue{.column = a, .value = Value(std::int64_t{3})},
            ColumnValue{.column = b, .value = Value(std::int64_t{4})},
        });
        txn.commit();
    }

    // --- Act: snapshot then restore into a new engine ---
    const auto snapshot = engine.createSnapshot();

    DocumentEngine restored(schema);
    restored.restoreSnapshot(snapshot);

    // --- Assert: computed column values are correct after restore ---
    EXPECT_EQ(restored.read(itemId, sum_col).asInt64(), 7);
    EXPECT_EQ(restored.read(itemId, product_col).asInt64(), 84);
}

// ---------------------------------------------------------------------------
// ComputedColumnChangeSetReplay
// ---------------------------------------------------------------------------

TEST(DocumentEngineComputedColumnTest, ComputedColumnChangeSetReplay)
{
    SchemaBuilder builder;
    auto itemTable = builder.createTable("Item");

    auto a = itemTable.addColumn(int64Column("a", IndexKind::Normal));
    auto b = itemTable.addColumn(int64Column("b", IndexKind::Normal));

    auto sum_col = itemTable.addComputedColumn(ComputedColumnDefinition {
        .debugName = "sum_col",
        .type = ValueType::Int64,
        .index = IndexKind::Normal,
        .nullable = false,
        .dependsOn = {a, b},
        .compute = [](const std::vector<Value> &deps) -> Value {
            return Value(deps[0].asInt64() + deps[1].asInt64());
        },
    });

    auto product_col = itemTable.addComputedColumn(ComputedColumnDefinition {
        .debugName = "product_col",
        .type = ValueType::Int64,
        .index = IndexKind::Normal,
        .nullable = false,
        .dependsOn = {a, b, sum_col},
        .compute = [](const std::vector<Value> &deps) -> Value {
            return Value(deps[0].asInt64() * deps[1].asInt64() * deps[2].asInt64());
        },
    });

    auto schema = builder.freeze();

    // --- Arrange: engine A inserts, snapshots, then updates ---
    DocumentEngine engineA(schema);

    ItemId itemId = 0;
    {
        auto txn = engineA.beginTransaction();
        itemId = txn.insert(itemTable.handle(), {
            ColumnValue{.column = a, .value = Value(std::int64_t{3})},
            ColumnValue{.column = b, .value = Value(std::int64_t{4})},
        });
        txn.commit();
    }

    const auto snapshot = engineA.createSnapshot();

    ByteArray changeSetBytes;
    {
        auto txn = engineA.beginTransaction();
        txn.update(itemId, a, Value(std::int64_t{5}));
        auto result = txn.commit();
        changeSetBytes = result.changeSet.serialize();
    }

    ASSERT_FALSE(changeSetBytes.empty());

    // --- Act: engine B recovers from snapshot + replay ---
    DocumentEngine engineB(schema);
    engineB.restoreSnapshot(snapshot);
    engineB.replayChangeSet(ChangeSet::deserialize(changeSetBytes));

    // --- Assert: computed column values match engine A ---
    EXPECT_EQ(engineB.read(itemId, a).asInt64(), 5);
    EXPECT_EQ(engineB.read(itemId, sum_col).asInt64(), 5 + 4);          // 9
    EXPECT_EQ(engineB.read(itemId, product_col).asInt64(), 5 * 4 * 9);  // 180

    // Cross-verify against engine A
    EXPECT_EQ(engineB.read(itemId, sum_col).asInt64(),
              engineA.read(itemId, sum_col).asInt64());
    EXPECT_EQ(engineB.read(itemId, product_col).asInt64(),
              engineA.read(itemId, product_col).asInt64());
}

} // namespace
