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
    TableHandle parentTable;
    ColumnHandle parentName;
    TableHandle childTable;
    ColumnHandle childName;
    RelationHandle childParent;
};

TestSchema buildSchema()
{
    SchemaBuilder builder;

    auto itemBuilder = builder.createTable("Item");
    auto itemName = itemBuilder.addColumn(stringColumn("name", IndexKind::Normal));

    auto parentBuilder = builder.createTable("Parent");
    auto parentName = parentBuilder.addColumn(stringColumn("name", IndexKind::Normal));

    auto childBuilder = builder.createTable("Child");
    auto childParent = childBuilder.addAssociation(AssociationDefinition {
        .debugName = "parent",
        .target = parentBuilder.handle(),
    });
    auto childName = childBuilder.addColumn(stringColumn("name", IndexKind::Normal));

    auto schema = builder.freeze();
    return TestSchema {
        schema,
        itemBuilder.handle(),
        itemName,
        parentBuilder.handle(),
        parentName,
        childBuilder.handle(),
        childName,
        childParent,
    };
}

struct SeededData {
    ItemId itemAId;
    ItemId itemBId;
    ItemId parentPId;
};

SeededData seedEngine(DocumentEngine &engine, const TestSchema &s)
{
    auto txn = engine.beginTransaction();
    const auto itemAId = txn.insert(s.itemTable, {
        ColumnValue {.column = s.itemName, .value = Value("A")},
    });
    const auto itemBId = txn.insert(s.itemTable, {
        ColumnValue {.column = s.itemName, .value = Value("B")},
    });
    const auto parentPId = txn.insert(s.parentTable, {
        ColumnValue {.column = s.parentName, .value = Value("ParentP")},
    });
    txn.commit();
    return SeededData {itemAId, itemBId, parentPId};
}

TEST(DocumentEngineUndoRedoTest, UndoAfterOneCommit)
{
    auto s = buildSchema();
    DocumentEngine engine(s.schema);
    auto ids = seedEngine(engine, s);

    EXPECT_FALSE(engine.canUndo());
    EXPECT_FALSE(engine.canRedo());

    {
        auto txn = engine.beginTransaction();
        txn.update(ids.itemAId, s.itemName, Value("NewA"));
        txn.commit();
    }

    EXPECT_EQ(engine.read(ids.itemAId, s.itemName).asString(), "NewA");
    ASSERT_TRUE(engine.canUndo());
    EXPECT_FALSE(engine.canRedo());

    engine.undo();

    EXPECT_EQ(engine.read(ids.itemAId, s.itemName).asString(), "A");
    EXPECT_FALSE(engine.canUndo());
    ASSERT_TRUE(engine.canRedo());

    EXPECT_EQ(engine.read(ids.itemBId, s.itemName).asString(), "B");
    EXPECT_EQ(engine.read(ids.parentPId, s.parentName).asString(), "ParentP");
}

TEST(DocumentEngineUndoRedoTest, RedoAfterUndo)
{
    auto s = buildSchema();
    DocumentEngine engine(s.schema);
    auto ids = seedEngine(engine, s);

    {
        auto txn = engine.beginTransaction();
        txn.update(ids.itemAId, s.itemName, Value("NewA"));
        txn.commit();
    }

    ASSERT_TRUE(engine.canUndo());
    engine.undo();
    EXPECT_EQ(engine.read(ids.itemAId, s.itemName).asString(), "A");
    ASSERT_TRUE(engine.canRedo());

    engine.redo();

    EXPECT_EQ(engine.read(ids.itemAId, s.itemName).asString(), "NewA");
    EXPECT_TRUE(engine.canUndo());
    EXPECT_FALSE(engine.canRedo());
}

TEST(DocumentEngineUndoRedoTest, UndoMultipleSteps)
{
    auto s = buildSchema();
    DocumentEngine engine(s.schema);
    auto ids = seedEngine(engine, s);

    {
        auto txn = engine.beginTransaction();
        txn.update(ids.itemAId, s.itemName, Value("Step1"));
        txn.commit();
    }
    {
        auto txn = engine.beginTransaction();
        txn.update(ids.itemAId, s.itemName, Value("Step2"));
        txn.commit();
    }
    {
        auto txn = engine.beginTransaction();
        txn.update(ids.itemAId, s.itemName, Value("Step3"));
        txn.commit();
    }

    EXPECT_EQ(engine.read(ids.itemAId, s.itemName).asString(), "Step3");

    engine.undo();
    EXPECT_EQ(engine.read(ids.itemAId, s.itemName).asString(), "Step2");
    EXPECT_TRUE(engine.canUndo());
    EXPECT_TRUE(engine.canRedo());

    engine.undo();
    EXPECT_EQ(engine.read(ids.itemAId, s.itemName).asString(), "Step1");
    EXPECT_TRUE(engine.canUndo());
    EXPECT_TRUE(engine.canRedo());

    engine.undo();
    EXPECT_EQ(engine.read(ids.itemAId, s.itemName).asString(), "A");
    EXPECT_FALSE(engine.canUndo());
    EXPECT_TRUE(engine.canRedo());
}

TEST(DocumentEngineUndoRedoTest, RedoMultipleSteps)
{
    auto s = buildSchema();
    DocumentEngine engine(s.schema);
    auto ids = seedEngine(engine, s);

    {
        auto txn = engine.beginTransaction();
        txn.update(ids.itemAId, s.itemName, Value("Step1"));
        txn.commit();
    }
    {
        auto txn = engine.beginTransaction();
        txn.update(ids.itemAId, s.itemName, Value("Step2"));
        txn.commit();
    }
    {
        auto txn = engine.beginTransaction();
        txn.update(ids.itemAId, s.itemName, Value("Step3"));
        txn.commit();
    }

    engine.undo();
    engine.undo();
    engine.undo();
    EXPECT_EQ(engine.read(ids.itemAId, s.itemName).asString(), "A");
    ASSERT_TRUE(engine.canRedo());

    engine.redo();
    EXPECT_EQ(engine.read(ids.itemAId, s.itemName).asString(), "Step1");
    EXPECT_TRUE(engine.canUndo());
    EXPECT_TRUE(engine.canRedo());

    engine.redo();
    EXPECT_EQ(engine.read(ids.itemAId, s.itemName).asString(), "Step2");

    engine.redo();
    EXPECT_EQ(engine.read(ids.itemAId, s.itemName).asString(), "Step3");
    EXPECT_TRUE(engine.canUndo());
    EXPECT_FALSE(engine.canRedo());
}

TEST(DocumentEngineUndoRedoTest, UndoInsertRemovesItem)
{
    auto s = buildSchema();
    DocumentEngine engine(s.schema);
    seedEngine(engine, s);

    ItemId itemCId = 0;
    {
        auto txn = engine.beginTransaction();
        itemCId = txn.insert(s.itemTable, {
            ColumnValue {.column = s.itemName, .value = Value("C")},
        });
        txn.commit();
    }

    EXPECT_TRUE(engine.contains(itemCId));
    EXPECT_EQ(engine.read(itemCId, s.itemName).asString(), "C");

    engine.undo();

    EXPECT_FALSE(engine.contains(itemCId));
}

TEST(DocumentEngineUndoRedoTest, RedoInsertRestoresId)
{
    auto s = buildSchema();
    DocumentEngine engine(s.schema);
    seedEngine(engine, s);

    ItemId itemCId = 0;
    {
        auto txn = engine.beginTransaction();
        itemCId = txn.insert(s.itemTable, {
            ColumnValue {.column = s.itemName, .value = Value("C")},
        });
        txn.commit();
    }

    engine.undo();
    EXPECT_FALSE(engine.contains(itemCId));

    engine.redo();

    EXPECT_TRUE(engine.contains(itemCId));
    EXPECT_EQ(engine.read(itemCId, s.itemName).asString(), "C");
}

TEST(DocumentEngineUndoRedoTest, UndoRemoveRestoresItemWithId)
{
    auto s = buildSchema();
    DocumentEngine engine(s.schema);
    auto ids = seedEngine(engine, s);

    const auto originalId = ids.itemAId;
    const std::string originalName = engine.read(originalId, s.itemName).asString();

    {
        auto txn = engine.beginTransaction();
        txn.remove(ids.itemAId);
        txn.commit();
    }

    EXPECT_FALSE(engine.contains(originalId));

    engine.undo();

    EXPECT_TRUE(engine.contains(originalId));
    EXPECT_EQ(engine.read(originalId, s.itemName).asString(), originalName);
}

TEST(DocumentEngineUndoRedoTest, UndoCascadeRemove)
{
    auto s = buildSchema();
    DocumentEngine engine(s.schema);
    auto ids = seedEngine(engine, s);

    ItemId childId = 0;
    {
        auto txn = engine.beginTransaction();
        childId = txn.insert(s.childTable, {
            ColumnValue {.column = s.childParent.column(), .value = idValue(ids.parentPId)},
            ColumnValue {.column = s.childName, .value = Value("ChildC")},
        });
        txn.commit();
    }

    EXPECT_TRUE(engine.contains(ids.parentPId));
    EXPECT_TRUE(engine.contains(childId));

    {
        auto txn = engine.beginTransaction();
        txn.remove(ids.parentPId);
        txn.commit();
    }

    EXPECT_FALSE(engine.contains(ids.parentPId));
    EXPECT_FALSE(engine.contains(childId));

    engine.undo();

    EXPECT_TRUE(engine.contains(ids.parentPId));
    EXPECT_EQ(engine.read(ids.parentPId, s.parentName).asString(), "ParentP");
    EXPECT_TRUE(engine.contains(childId));
    EXPECT_EQ(engine.read(childId, s.childName).asString(), "ChildC");
}

TEST(DocumentEngineUndoRedoTest, NewTransactionAfterUndoClearsRedo)
{
    auto s = buildSchema();
    DocumentEngine engine(s.schema);
    auto ids = seedEngine(engine, s);

    {
        auto txn = engine.beginTransaction();
        txn.update(ids.itemAId, s.itemName, Value("NewA"));
        txn.commit();
    }

    ASSERT_TRUE(engine.canUndo());
    EXPECT_FALSE(engine.canRedo());

    engine.undo();

    EXPECT_FALSE(engine.canUndo());
    ASSERT_TRUE(engine.canRedo());

    {
        auto txn = engine.beginTransaction();
        txn.update(ids.itemBId, s.itemName, Value("NewB"));
        txn.commit();
    }

    EXPECT_FALSE(engine.canRedo());
    EXPECT_TRUE(engine.canUndo());
    EXPECT_EQ(engine.read(ids.itemBId, s.itemName).asString(), "NewB");
}

TEST(DocumentEngineUndoRedoTest, UndoRedoDoNotCreateNewUndoStep)
{
    auto s = buildSchema();
    DocumentEngine engine(s.schema);
    auto ids = seedEngine(engine, s);

    {
        auto txn = engine.beginTransaction();
        txn.update(ids.itemAId, s.itemName, Value("NewA"));
        txn.commit();
    }

    const auto undoSizeAfterCommit = engine.undoHistory().size();
    EXPECT_EQ(undoSizeAfterCommit, 1U);
    EXPECT_EQ(engine.redoHistory().size(), 0U);

    engine.undo();
    EXPECT_EQ(engine.undoHistory().size(), 0U);
    EXPECT_EQ(engine.redoHistory().size(), 1U);

    engine.redo();
    EXPECT_EQ(engine.undoHistory().size(), undoSizeAfterCommit);
    EXPECT_EQ(engine.redoHistory().size(), 0U);
}

TEST(DocumentEngineUndoRedoTest, ClearUndoHistory)
{
    auto s = buildSchema();
    DocumentEngine engine(s.schema);
    auto ids = seedEngine(engine, s);

    {
        auto txn = engine.beginTransaction();
        txn.update(ids.itemAId, s.itemName, Value("T1"));
        txn.commit();
    }
    {
        auto txn = engine.beginTransaction();
        txn.update(ids.itemAId, s.itemName, Value("T2"));
        txn.commit();
    }

    EXPECT_TRUE(engine.canUndo());
    EXPECT_FALSE(engine.canRedo());

    engine.clearUndoHistory();

    EXPECT_FALSE(engine.canUndo());
    EXPECT_FALSE(engine.canRedo());

    EXPECT_EQ(engine.read(ids.itemAId, s.itemName).asString(), "T2");
    EXPECT_EQ(engine.read(ids.itemBId, s.itemName).asString(), "B");
    EXPECT_TRUE(engine.contains(ids.parentPId));
}

TEST(DocumentEngineUndoRedoTest, UndoRedoWithActiveTransactionRejected)
{
    auto s = buildSchema();
    DocumentEngine engine(s.schema);
    seedEngine(engine, s);

    auto txn = engine.beginTransaction();
    const auto items = engine.view(s.itemTable).toVector();
    ASSERT_FALSE(items.empty());
    txn.update(items.at(0).id, s.itemName, Value("newValue"));

    EXPECT_THROW(engine.undo(), TransactionError);
    EXPECT_THROW(engine.redo(), TransactionError);

    txn.rollback();
}

TEST(DocumentEngineUndoRedoTest, UndoRedoOriginInChangeset)
{
    auto s = buildSchema();
    DocumentEngine engine(s.schema);
    auto ids = seedEngine(engine, s);

    CommitResult normalResult;
    {
        auto txn = engine.beginTransaction();
        txn.update(ids.itemAId, s.itemName, Value("NewA"));
        normalResult = txn.commit();
    }
    EXPECT_EQ(normalResult.origin, EventOrigin::Normal);

    const CommitResult undoResult = engine.undo();
    EXPECT_EQ(undoResult.origin, EventOrigin::Undo);

    const CommitResult redoResult = engine.redo();
    EXPECT_EQ(redoResult.origin, EventOrigin::Redo);
}

TEST(DocumentEngineUndoRedoTest, UndoRedoCommitResultProduced)
{
    auto s = buildSchema();
    DocumentEngine engine(s.schema);
    auto ids = seedEngine(engine, s);

    {
        auto txn = engine.beginTransaction();
        txn.update(ids.itemAId, s.itemName, Value("NewA"));
        txn.commit();
    }

    const CommitResult undoResult = engine.undo();
    EXPECT_FALSE(undoResult.changeSet.empty());

    const CommitResult redoResult = engine.redo();
    EXPECT_FALSE(redoResult.changeSet.empty());
}

TEST(DocumentEngineUndoRedoTest, UndoRedoWithNonUndoableBetween)
{
    auto s = buildSchema();
    DocumentEngine engine(s.schema);
    auto ids = seedEngine(engine, s);

    // Undoable transaction
    {
        auto txn = engine.beginTransaction();
        txn.update(ids.itemAId, s.itemName, Value("T1"));
        txn.commit();
    }
    EXPECT_EQ(engine.read(ids.itemAId, s.itemName).asString(), "T1");

    // Non-undoable transaction
    {
        auto txn = engine.beginTransaction(TransactionOptions {.undoable = false});
        txn.update(ids.itemAId, s.itemName, Value("T2"));
        txn.commit();
    }
    EXPECT_EQ(engine.read(ids.itemAId, s.itemName).asString(), "T2");

    // The undoable transaction (T1) is still on the undo stack.
    EXPECT_TRUE(engine.canUndo());
    EXPECT_FALSE(engine.canRedo());

    engine.undo();
    EXPECT_EQ(engine.read(ids.itemAId, s.itemName).asString(), "A");
    EXPECT_FALSE(engine.canUndo());
}

} // namespace
