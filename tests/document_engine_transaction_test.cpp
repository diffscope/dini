#include <cstdint>
#include <stdexcept>
#include <vector>

#include <dini/engine.h>
#include <dini/errors.h>

#include <gtest/gtest.h>

namespace {

using namespace dini;

ColumnDefinition stringColumn(const char *debugName, IndexKind index = IndexKind::None)
{
    return ColumnDefinition{
        .debugName = debugName,
        .type = ValueType::String,
        .index = index,
        .defaultValue = Value(""),
        .nullable = false,
    };
}

ColumnDefinition int64Column(const char *debugName, IndexKind index = IndexKind::None)
{
    return ColumnDefinition{
        .debugName = debugName,
        .type = ValueType::Int64,
        .index = index,
        .defaultValue = Value(std::int64_t{0}),
        .nullable = false,
    };
}

struct ItemSchema {
    EngineSchema schema;
    TableHandle table;
    ColumnHandle name;
    ColumnHandle value;
};

ItemSchema buildItemSchema()
{
    SchemaBuilder builder;
    auto table = builder.createTable("Item");
    auto name = table.addColumn(stringColumn("name", IndexKind::Normal));
    auto value = table.addColumn(int64Column("value", IndexKind::Normal));
    auto tableHandle = table.handle();
    auto schema = builder.freeze();
    return {schema, tableHandle, name, value};
}

// Inserts one seed row {name = "initial", value = 0} in a transaction and
// returns the assigned item ID.
ItemId seedInitialRow(DocumentEngine &engine, const ItemSchema &s)
{
    auto t = engine.beginTransaction();
    ItemId id = t.insert(s.table, {
        ColumnValue{.column = s.name, .value = Value("initial")},
        ColumnValue{.column = s.value, .value = Value(std::int64_t{0})},
    });
    t.commit();
    return id;
}

TEST(DocumentEngineTransactionTest, TransactionLifecycleInsertCommit)
{
    auto s = buildItemSchema();
    DocumentEngine engine(s.schema);

    auto t = engine.beginTransaction();
    ItemId id = t.insert(s.table, {
        ColumnValue{.column = s.name, .value = Value("committed")},
        ColumnValue{.column = s.value, .value = Value(std::int64_t{1})},
    });
    t.commit();

    EXPECT_EQ(t.state(), TransactionState::Committed);
    EXPECT_TRUE(engine.contains(id));
}

TEST(DocumentEngineTransactionTest, TransactionRollbackRevertsChanges)
{
    auto s = buildItemSchema();
    DocumentEngine engine(s.schema);
    ItemId initialId = seedInitialRow(engine, s);

    const std::int64_t oldValue = engine.read(initialId, s.value).asInt64();
    EXPECT_EQ(oldValue, 0);

    ItemId newId = 0;
    {
        auto t = engine.beginTransaction();
        t.update(initialId, s.value, Value(std::int64_t{42}));
        newId = t.insert(s.table, {
            ColumnValue{.column = s.name, .value = Value("ephemeral")},
            ColumnValue{.column = s.value, .value = Value(std::int64_t{99})},
        });
        t.rollback();
    }

    // Update was reverted to the original value.
    EXPECT_EQ(engine.read(initialId, s.value).asInt64(), oldValue);

    // New item inserted during the rolled-back transaction does not exist.
    EXPECT_FALSE(engine.contains(newId));
}

TEST(DocumentEngineTransactionTest, TransactionRollbackInsert)
{
    auto s = buildItemSchema();
    DocumentEngine engine(s.schema);
    seedInitialRow(engine, s);

    ItemId insertedId = 0;
    {
        auto t = engine.beginTransaction();
        insertedId = t.insert(s.table, {
            ColumnValue{.column = s.name, .value = Value("doomed")},
            ColumnValue{.column = s.value, .value = Value(std::int64_t{55})},
        });
        t.rollback();
    }

    EXPECT_FALSE(engine.contains(insertedId));
}

TEST(DocumentEngineTransactionTest, TransactionRollbackRemove)
{
    auto s = buildItemSchema();
    DocumentEngine engine(s.schema);
    ItemId initialId = seedInitialRow(engine, s);

    const std::string oldName = engine.read(initialId, s.name).asString();
    const std::int64_t oldValue = engine.read(initialId, s.value).asInt64();

    {
        auto t = engine.beginTransaction();
        t.remove(initialId);
        t.rollback();
    }

    // The removed item exists again with its original values.
    EXPECT_TRUE(engine.contains(initialId));
    EXPECT_EQ(engine.read(initialId, s.name).asString(), oldName);
    EXPECT_EQ(engine.read(initialId, s.value).asInt64(), oldValue);
}

TEST(DocumentEngineTransactionTest, TransactionFailedCantCommit)
{
    // Build a dedicated schema whose BeforeCommit hook throws unconditionally
    // so the first commit attempt fails and leaves the transaction in Failed state.
    SchemaBuilder builder;
    auto table = builder.createTable("Item");
    auto name = table.addColumn(stringColumn("name", IndexKind::Normal));
    auto value = table.addColumn(int64Column("value", IndexKind::Normal));
    table.addHook(HookDefinition{
        .stage = HookStage::BeforeCommit,
        .callback = [](TransactionContext &, const ChangeSet &) {
            throw std::runtime_error("forced failure");
        },
    });
    auto schema = builder.freeze();

    DocumentEngine engine(schema);

    auto t = engine.beginTransaction();
    t.insert(table.handle(), {
        ColumnValue{.column = name, .value = Value("test")},
        ColumnValue{.column = value, .value = Value(std::int64_t{42})},
    });

    // The BeforeCommit hook throws during commit, leaving the transaction Failed.
    EXPECT_ANY_THROW(t.commit());
    EXPECT_EQ(t.state(), TransactionState::Failed);
    EXPECT_THROW(t.commit(), TransactionError);
}

TEST(DocumentEngineTransactionTest, TransactionFailedCantUpdate)
{
    SchemaBuilder builder;
    auto table = builder.createTable("Item");
    auto name = table.addColumn(stringColumn("name", IndexKind::Normal));
    auto value = table.addColumn(int64Column("value", IndexKind::Normal));
    table.addHook(HookDefinition{
        .stage = HookStage::BeforeCommit,
        .callback = [](TransactionContext &, const ChangeSet &) {
            throw std::runtime_error("forced failure");
        },
    });
    auto schema = builder.freeze();

    DocumentEngine engine(schema);

    auto t = engine.beginTransaction();
    ItemId id = t.insert(table.handle(), {
        ColumnValue{.column = name, .value = Value("test")},
        ColumnValue{.column = value, .value = Value(std::int64_t{42})},
    });

    EXPECT_ANY_THROW(t.commit());
    EXPECT_EQ(t.state(), TransactionState::Failed);
    EXPECT_THROW(t.update(id, value, Value(std::int64_t{99})), TransactionError);
}

TEST(DocumentEngineTransactionTest, TransactionRollbackClearsFailedState)
{
    SchemaBuilder builder;
    auto table = builder.createTable("Item");
    auto name = table.addColumn(stringColumn("name", IndexKind::Normal));
    auto value = table.addColumn(int64Column("value", IndexKind::Normal));
    table.addHook(HookDefinition{
        .stage = HookStage::BeforeCommit,
        .callback = [](TransactionContext &, const ChangeSet &) {
            throw std::runtime_error("forced failure");
        },
    });
    auto schema = builder.freeze();

    DocumentEngine engine(schema);

    auto t = engine.beginTransaction();
    t.insert(table.handle(), {
        ColumnValue{.column = name, .value = Value("test")},
        ColumnValue{.column = value, .value = Value(std::int64_t{42})},
    });

    EXPECT_ANY_THROW(t.commit());
    EXPECT_EQ(t.state(), TransactionState::Failed);
    t.rollback();
    EXPECT_EQ(t.state(), TransactionState::RolledBack);
}

TEST(DocumentEngineTransactionTest, TransactionCommitAfterRollbackError)
{
    auto s = buildItemSchema();
    DocumentEngine engine(s.schema);

    auto t = engine.beginTransaction();
    t.rollback();
    EXPECT_THROW(t.commit(), TransactionError);
}

TEST(DocumentEngineTransactionTest, TransactionDestructorRollsBack)
{
    auto s = buildItemSchema();
    DocumentEngine engine(s.schema);

    ItemId id = 0;
    {
        auto t = engine.beginTransaction();
        id = t.insert(s.table, {
            ColumnValue{.column = s.name, .value = Value("transient")},
            ColumnValue{.column = s.value, .value = Value(std::int64_t{1})},
        });
        // t goes out of scope; destructor auto-rollbacks.
    }

    EXPECT_FALSE(engine.contains(id));
}

TEST(DocumentEngineTransactionTest, TransactionNoConcurrentWrite)
{
    auto s = buildItemSchema();
    DocumentEngine engine(s.schema);

    auto t1 = engine.beginTransaction();
    // A second write transaction must be rejected while t1 is active.
    EXPECT_THROW(engine.beginTransaction(), TransactionError);

    // Release t1 so the engine can accept new transactions again.
    t1.rollback();

    auto t2 = engine.beginTransaction();
    t2.rollback();
}

TEST(DocumentEngineTransactionTest, TransactionNonUndoable)
{
    auto s = buildItemSchema();
    DocumentEngine engine(s.schema);

    {
        auto t = engine.beginTransaction(TransactionOptions{.undoable = false});
        t.insert(s.table, {
            ColumnValue{.column = s.name, .value = Value("nonUndoable")},
            ColumnValue{.column = s.value, .value = Value(std::int64_t{1})},
        });
        t.commit();
    }

    EXPECT_FALSE(engine.canUndo());
}

TEST(DocumentEngineTransactionTest, TransactionUndoableDefault)
{
    auto s = buildItemSchema();
    DocumentEngine engine(s.schema);

    {
        // Default TransactionOptions has undoable = true.
        auto t = engine.beginTransaction();
        t.insert(s.table, {
            ColumnValue{.column = s.name, .value = Value("undoable")},
            ColumnValue{.column = s.value, .value = Value(std::int64_t{1})},
        });
        t.commit();
    }

    EXPECT_TRUE(engine.canUndo());
}

TEST(DocumentEngineTransactionTest, TransactionChangesetNotEmpty)
{
    auto s = buildItemSchema();
    DocumentEngine engine(s.schema);

    auto t = engine.beginTransaction();
    t.insert(s.table, {
        ColumnValue{.column = s.name, .value = Value("changesetTest")},
        ColumnValue{.column = s.value, .value = Value(std::int64_t{7})},
    });
    auto result = t.commit();

    EXPECT_FALSE(result.changeSet.empty());
    const auto &ops = result.changeSet.operations();
    EXPECT_FALSE(ops.empty());

    // The change set must contain an ItemInserted operation.
    bool hasInsert = false;
    for (const auto &op : ops) {
        if (op.kind() == ChangeOperationKind::ItemInserted) {
            hasInsert = true;
            break;
        }
    }
    EXPECT_TRUE(hasInsert);
}

TEST(DocumentEngineTransactionTest, TransactionCommitLogNotEmpty)
{
    auto s = buildItemSchema();
    DocumentEngine engine(s.schema);

    auto t = engine.beginTransaction();
    t.insert(s.table, {
        ColumnValue{.column = s.name, .value = Value("logTest")},
        ColumnValue{.column = s.value, .value = Value(std::int64_t{3})},
    });
    auto result = t.commit();

    EXPECT_FALSE(result.commitLog.empty());
}

TEST(DocumentEngineTransactionTest, TransactionRollbackInverseChangeset)
{
    auto s = buildItemSchema();
    DocumentEngine engine(s.schema);

    // Subscribe to engine events and capture the rollback payload.
    bool receivedRollback = false;
    ChangeSet rollbackChangeSet;
    auto sub = engine.subscribe([&](const EngineEvent &e) {
        if (e.kind == EventKind::Rollback) {
            receivedRollback = true;
            rollbackChangeSet = e.changeSet;
        }
    });

    auto t = engine.beginTransaction();
    t.insert(s.table, {
        ColumnValue{.column = s.name, .value = Value("rollbackEvent")},
        ColumnValue{.column = s.value, .value = Value(std::int64_t{11})},
    });
    t.rollback();

    EXPECT_TRUE(receivedRollback);

    // The rollback event should carry a non-empty change set.
    EXPECT_FALSE(rollbackChangeSet.empty());
}

} // namespace
