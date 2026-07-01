#include <cstdint>
#include <string>
#include <vector>

#include <dini/engine.h>
#include <dini/errors.h>

#include <gtest/gtest.h>

namespace {

using namespace dini;

// ---------------------------------------------------------------------------
// Column helpers -- mirror the smoke-test factory style
// ---------------------------------------------------------------------------

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
        .defaultValue = Value(std::int64_t{0}),
        .nullable = false,
    };
}

ColumnDefinition uint64Column(const char *debugName, IndexKind index = IndexKind::None)
{
    return ColumnDefinition {
        .debugName = debugName,
        .type = ValueType::UInt64,
        .index = index,
        .defaultValue = Value(std::uint64_t{0}),
        .nullable = false,
    };
}

Value idValue(ItemId id)
{
    return Value(static_cast<std::uint64_t>(id));
}

// ---------------------------------------------------------------------------
// Test schema -- built once per test so each test owns its handles
// ---------------------------------------------------------------------------

struct TestSchema {
    EngineSchema schema;

    // Table "Item"
    TableHandle itemTable;
    ColumnHandle itemName;
    ColumnHandle itemValue;
    ColumnHandle itemTag;
    ColumnHandle itemCode;        // Unique-indexed column for test 4

    // Table "State"
    TableHandle stateTable;
    ColumnHandle stateColData;

    // Table "Group" -- association target for the list
    TableHandle groupTable;
    ColumnHandle groupId;

    // List "ItemGrouping"
    ListHandle itemList;
    ColumnHandle listRank;
};

TestSchema buildTestSchema()
{
    TestSchema s;

    SchemaBuilder builder;

    // ---- Item ----
    auto itemTbl = builder.createTable("Item");
    s.itemName = itemTbl.addColumn(stringColumn("name", IndexKind::Normal));
    s.itemValue = itemTbl.addColumn(int64Column("value", IndexKind::Normal));
    s.itemTag = itemTbl.addColumn(ColumnDefinition {
        .debugName = "tag",
        .type = ValueType::String,
        .index = IndexKind::None,
        .defaultValue = Value(""),
        .nullable = false,
    });
    s.itemCode = itemTbl.addColumn(ColumnDefinition {
        .debugName = "code",
        .type = ValueType::String,
        .index = IndexKind::Unique,
        .defaultValue = Value(""),
        .nullable = false,
    });
    s.itemTable = itemTbl.handle();

    // ---- State ----
    auto stateTbl = builder.createTable("State");
    s.stateColData = stateTbl.addColumn(stringColumn("data"));
    s.stateTable = stateTbl.handle();

    // ---- Group ----
    auto grpTbl = builder.createTable("Group");
    s.groupId = grpTbl.addColumn(uint64Column("id", IndexKind::Normal));
    s.groupTable = grpTbl.handle();

    // ---- ItemGrouping list ----
    auto lst = builder.createList("ItemGrouping");
    lst.setAssociation(AssociationDefinition {
        .debugName = "group",
        .target = s.groupTable,
    });
    s.listRank = lst.addColumn(int64Column("rank", IndexKind::Normal));
    s.itemList = lst.handle();

    s.schema = builder.freeze();
    return s;
}

// ---------------------------------------------------------------------------
// Seed helpers
// ---------------------------------------------------------------------------

struct SeedResult {
    ItemId item1Id;
    ItemId item2Id;
    ItemId item3Id;
    ItemId groupId;
    ItemId listElem1Id;
    ItemId listElem2Id;
};

SeedResult seedEngine(DocumentEngine &engine, const TestSchema &s)
{
    SeedResult r{};

    {
        auto txn = engine.beginTransaction();

        r.item1Id = txn.insert(s.itemTable, {
            ColumnValue{.column = s.itemName, .value = Value("Alpha")},
            ColumnValue{.column = s.itemValue, .value = Value(std::int64_t{100})},
            ColumnValue{.column = s.itemTag, .value = Value("tag-a")},
            ColumnValue{.column = s.itemCode, .value = Value("C1")},
        });

        r.item2Id = txn.insert(s.itemTable, {
            ColumnValue{.column = s.itemName, .value = Value("Beta")},
            ColumnValue{.column = s.itemValue, .value = Value(std::int64_t{200})},
            ColumnValue{.column = s.itemTag, .value = Value("tag-b")},
            ColumnValue{.column = s.itemCode, .value = Value("C2")},
        });

        r.item3Id = txn.insert(s.itemTable, {
            ColumnValue{.column = s.itemName, .value = Value("Gamma")},
            ColumnValue{.column = s.itemValue, .value = Value(std::int64_t{300})},
            ColumnValue{.column = s.itemTag, .value = Value("tag-c")},
            ColumnValue{.column = s.itemCode, .value = Value("C3")},
        });

        r.groupId = txn.insert(s.groupTable, {
            ColumnValue{.column = s.groupId, .value = Value(std::uint64_t{42})},
        });

        auto assocValue = idValue(r.groupId);

        r.listElem1Id = txn.insert(s.itemList, assocValue, 0, {
            ColumnValue{.column = s.listRank, .value = Value(std::int64_t{10})},
        });

        r.listElem2Id = txn.insert(s.itemList, assocValue, 1, {
            ColumnValue{.column = s.listRank, .value = Value(std::int64_t{20})},
        });

        txn.commit();
    }

    return r;
}

// ---------------------------------------------------------------------------
// Test 1: SnapshotIncludesRegularColumn
// ---------------------------------------------------------------------------

TEST(DocumentEngineLogSnapshotTest, SnapshotIncludesRegularColumn)
{
    auto s = buildTestSchema();
    DocumentEngine engine(s.schema);

    ItemId itemId = 0;
    {
        auto txn = engine.beginTransaction();
        itemId = txn.insert(s.itemTable, {
            ColumnValue{.column = s.itemName, .value = Value("Kilo")},
            ColumnValue{.column = s.itemValue, .value = Value(std::int64_t{500})},
            ColumnValue{.column = s.itemTag, .value = Value("persistent-data")},
            ColumnValue{.column = s.itemCode, .value = Value("K1")},
        });
        txn.commit();
    }

    EXPECT_EQ(engine.read(itemId, s.itemTag).asString(), "persistent-data");
    EXPECT_EQ(engine.read(itemId, s.itemName).asString(), "Kilo");
    EXPECT_EQ(engine.read(itemId, s.itemValue).asInt64(), 500);

    const auto snapshot = engine.createSnapshot();

    DocumentEngine restored(s.schema);
    restored.restoreSnapshot(snapshot);

    EXPECT_EQ(restored.read(itemId, s.itemName).asString(), "Kilo");
    EXPECT_EQ(restored.read(itemId, s.itemValue).asInt64(), 500);
    EXPECT_EQ(restored.read(itemId, s.itemCode).asString(), "K1");
    EXPECT_EQ(restored.read(itemId, s.itemTag).asString(), "persistent-data");
}

// ---------------------------------------------------------------------------
// Test 2: SnapshotIncludesRegularTable
// ---------------------------------------------------------------------------

TEST(DocumentEngineLogSnapshotTest, SnapshotIncludesRegularTable)
{
    auto s = buildTestSchema();
    DocumentEngine engine(s.schema);

    ItemId stateId = 0;
    {
        auto txn = engine.beginTransaction();
        stateId = txn.insert(s.stateTable, {
            ColumnValue{.column = s.stateColData, .value = Value("persistent")},
        });
        txn.commit();
    }

    EXPECT_EQ(engine.view(s.stateTable).count(), 1U);
    EXPECT_EQ(engine.read(stateId, s.stateColData).asString(), "persistent");

    const auto snapshot = engine.createSnapshot();

    DocumentEngine restored(s.schema);
    restored.restoreSnapshot(snapshot);

    EXPECT_EQ(restored.view(s.stateTable).count(), 1U);
    EXPECT_TRUE(restored.contains(stateId));
    EXPECT_EQ(restored.read(stateId, s.stateColData).asString(), "persistent");
}

// ---------------------------------------------------------------------------
// Test 3: SnapshotDoesNotContainUndoHistory
// ---------------------------------------------------------------------------

TEST(DocumentEngineLogSnapshotTest, SnapshotDoesNotContainUndoHistory)
{
    auto s = buildTestSchema();
    DocumentEngine engine(s.schema);

    // Commit multiple transactions to build undo history
    ItemId itemId = 0;
    {
        auto txn = engine.beginTransaction();
        itemId = txn.insert(s.itemTable, {
            ColumnValue{.column = s.itemName, .value = Value("Delta")},
            ColumnValue{.column = s.itemValue, .value = Value(std::int64_t{10})},
            ColumnValue{.column = s.itemCode, .value = Value("D1")},
        });
        txn.commit();
    }

    {
        auto txn = engine.beginTransaction();
        txn.update(itemId, s.itemValue, Value(std::int64_t{20}));
        txn.commit();
    }

    {
        auto txn = engine.beginTransaction();
        txn.update(itemId, s.itemValue, Value(std::int64_t{30}));
        txn.commit();
    }

    // Engine with multiple undoable commits should have undo history
    ASSERT_TRUE(engine.canUndo());
    EXPECT_FALSE(engine.canRedo());

    const auto snapshot = engine.createSnapshot();

    DocumentEngine restored(s.schema);
    restored.restoreSnapshot(snapshot);

    // Data survived
    EXPECT_EQ(restored.read(itemId, s.itemValue).asInt64(), 30);

    // Undo/redo history is cleared by snapshot restore
    EXPECT_FALSE(restored.canUndo());
    EXPECT_FALSE(restored.canRedo());
    EXPECT_TRUE(restored.undoHistory().empty());
    EXPECT_TRUE(restored.redoHistory().empty());
}

// ---------------------------------------------------------------------------
// Test 4: RestoreThenInsertUniqueOk
// ---------------------------------------------------------------------------

TEST(DocumentEngineLogSnapshotTest, RestoreThenInsertUniqueOk)
{
    auto s = buildTestSchema();
    DocumentEngine engine(s.schema);

    auto seed = seedEngine(engine, s);

    const auto snapshot = engine.createSnapshot();

    DocumentEngine restored(s.schema);
    restored.restoreSnapshot(snapshot);

    // Unique-indexed data is present
    EXPECT_EQ(restored.read(seed.item1Id, s.itemCode).asString(), "C1");
    EXPECT_EQ(restored.read(seed.item2Id, s.itemCode).asString(), "C2");
    EXPECT_EQ(restored.read(seed.item3Id, s.itemCode).asString(), "C3");

    // Inserting a duplicate unique value must be rejected
    {
        auto txn = restored.beginTransaction();
        EXPECT_THROW(
            txn.insert(s.itemTable, {
                ColumnValue{.column = s.itemName, .value = Value("Duplicate")},
                ColumnValue{.column = s.itemValue, .value = Value(std::int64_t{999})},
                ColumnValue{.column = s.itemCode, .value = Value("C1")},
            }),
            ConstraintError);
        // Transaction must be rolled back explicitly after failure
        txn.rollback();
    }

    // Inserting a new unique value succeeds and the ID continues from the
    // restored ID-generation state (no reuse)
    ItemId newId = 0;
    {
        auto txn = restored.beginTransaction();
        newId = txn.insert(s.itemTable, {
            ColumnValue{.column = s.itemName, .value = Value("Epsilon")},
            ColumnValue{.column = s.itemValue, .value = Value(std::int64_t{400})},
            ColumnValue{.column = s.itemCode, .value = Value("C4")},
        });
        txn.commit();
    }

    EXPECT_TRUE(restored.contains(newId));
    EXPECT_EQ(restored.read(newId, s.itemCode).asString(), "C4");
    // New ID must be greater than any seed ID (ID generation moved forward)
    EXPECT_GT(newId, seed.item3Id);

    // View count reflects the added item
    EXPECT_EQ(restored.view(s.itemTable).count(), 4U);
}

// ---------------------------------------------------------------------------
// Test 5: ReplaySingleLog
// ---------------------------------------------------------------------------

TEST(DocumentEngineLogSnapshotTest, ReplaySingleLog)
{
    auto s = buildTestSchema();
    DocumentEngine engineA(s.schema);

    auto seed = seedEngine(engineA, s);

    const auto snapshot = engineA.createSnapshot();

    // One more commit on engine A after the snapshot
    ByteArray commitLog;
    {
        auto txn = engineA.beginTransaction();
        txn.update(seed.item1Id, s.itemValue, Value(std::int64_t{150}));
        auto result = txn.commit();
        commitLog = result.commitLog;
    }

    ASSERT_FALSE(commitLog.empty());

    // Engine B starts from the snapshot
    DocumentEngine engineB(s.schema);
    engineB.restoreSnapshot(snapshot);

    // Before replay, value is still the seed value
    EXPECT_EQ(engineB.read(seed.item1Id, s.itemValue).asInt64(), 100);

    engineB.replayCommitLog(commitLog);

    // After replay, value matches engine A's post-commit state
    EXPECT_EQ(engineB.read(seed.item1Id, s.itemValue).asInt64(), 150);

    // Other items are unaffected
    EXPECT_EQ(engineB.read(seed.item2Id, s.itemName).asString(), "Beta");
    EXPECT_EQ(engineB.read(seed.item3Id, s.itemCode).asString(), "C3");
}

// ---------------------------------------------------------------------------
// Test 6: ReplayMultipleLogs
// ---------------------------------------------------------------------------

TEST(DocumentEngineLogSnapshotTest, ReplayMultipleLogs)
{
    auto s = buildTestSchema();
    DocumentEngine engineA(s.schema);

    auto seed = seedEngine(engineA, s);

    const auto snapshot = engineA.createSnapshot();

    // Several commits after the snapshot
    ByteArray log1, log2, log3;

    {
        auto txn = engineA.beginTransaction();
        txn.update(seed.item1Id, s.itemName, Value("AlphaPrime"));
        auto result = txn.commit();
        log1 = result.commitLog;
    }

    {
        auto txn = engineA.beginTransaction();
        txn.update(seed.item2Id, s.itemValue, Value(std::int64_t{250}));
        auto result = txn.commit();
        log2 = result.commitLog;
    }

    {
        auto txn = engineA.beginTransaction();
        txn.remove(seed.item3Id);
        auto result = txn.commit();
        log3 = result.commitLog;
    }

    // New engine starts from snapshot
    DocumentEngine engineB(s.schema);
    engineB.restoreSnapshot(snapshot);

    // Replay in order
    engineB.replayCommitLog(log1);
    engineB.replayCommitLog(log2);
    engineB.replayCommitLog(log3);

    // Final state matches engine A
    EXPECT_EQ(engineB.read(seed.item1Id, s.itemName).asString(), "AlphaPrime");
    EXPECT_EQ(engineB.read(seed.item2Id, s.itemValue).asInt64(), 250);
    EXPECT_FALSE(engineB.contains(seed.item3Id));

    // View count decreased after the remove
    EXPECT_EQ(engineB.view(s.itemTable).count(), 2U);
}

// ---------------------------------------------------------------------------
// Test 7: RecoveryEngineStateEqualToOriginal
// ---------------------------------------------------------------------------

TEST(DocumentEngineLogSnapshotTest, RecoveryEngineStateEqualToOriginal)
{
    auto s = buildTestSchema();
    DocumentEngine engineA(s.schema);

    auto seed = seedEngine(engineA, s);

    const auto snapshot = engineA.createSnapshot();

    // More operations post-snapshot
    ByteArray log1, log2;

    {
        auto txn = engineA.beginTransaction();
        txn.update(seed.item1Id, s.itemValue, Value(std::int64_t{111}));
        auto result = txn.commit();
        log1 = result.commitLog;
    }

    ItemId extraItemId = 0;
    {
        auto txn = engineA.beginTransaction();
        extraItemId = txn.insert(s.itemTable, {
            ColumnValue{.column = s.itemName, .value = Value("Zeta")},
            ColumnValue{.column = s.itemValue, .value = Value(std::int64_t{600})},
            ColumnValue{.column = s.itemCode, .value = Value("Z1")},
        });
        txn.update(seed.item3Id, s.itemCode, Value("C3a"));
        auto result = txn.commit();
        log2 = result.commitLog;
    }

    // Recover: engine B <- snapshot + all logs
    DocumentEngine engineB(s.schema);
    engineB.restoreSnapshot(snapshot);
    engineB.replayCommitLog(log1);
    engineB.replayCommitLog(log2);

    // ---- Compare every item ----
    auto itemsA = engineA.view(s.itemTable).toVector();
    auto itemsB = engineB.view(s.itemTable).toVector();

    ASSERT_EQ(itemsA.size(), itemsB.size());

    for (const auto &snapA : itemsA)
    {
        ASSERT_TRUE(engineB.contains(snapA.id));

        // Name
        EXPECT_EQ(engineB.read(snapA.id, s.itemName).asString(),
                  engineA.read(snapA.id, s.itemName).asString());
        // Value
        EXPECT_EQ(engineB.read(snapA.id, s.itemValue).asInt64(),
                  engineA.read(snapA.id, s.itemValue).asInt64());
        // Code
        EXPECT_EQ(engineB.read(snapA.id, s.itemCode).asString(),
                  engineA.read(snapA.id, s.itemCode).asString());
    }

    // ---- View counts ----
    EXPECT_EQ(engineB.view(s.itemTable).count(),
              engineA.view(s.itemTable).count());

    // ---- List length ----
    auto assocValue = idValue(seed.groupId);
    EXPECT_EQ(engineB.listLength(s.itemList, assocValue),
              engineA.listLength(s.itemList, assocValue));

    // ---- List element values ----
    auto listA = engineA.view(s.itemList).toVector();
    auto listB = engineB.view(s.itemList).toVector();
    ASSERT_EQ(listA.size(), listB.size());
    for (std::size_t i = 0; i < listA.size(); ++i)
    {
        EXPECT_EQ(listA[i].id, listB[i].id);
    }

    // ---- Query ----
    const QuerySpec queryByValue {
        .filter = FilterExpression(Filter(
            FieldRef::column(s.itemValue),
            ComparisonOperator::Greater,
            Value(std::int64_t{200}))),
        .sortKeys = {
            SortKey{.field = FieldRef::column(s.itemName), .direction = SortDirection::Ascending},
        },
    };
    auto qA = engineA.query(s.itemTable, queryByValue).toVector();
    auto qB = engineB.query(s.itemTable, queryByValue).toVector();
    ASSERT_EQ(qA.size(), qB.size());
    for (std::size_t i = 0; i < qA.size(); ++i)
    {
        EXPECT_EQ(qA[i].id, qB[i].id);
    }
}

// ---------------------------------------------------------------------------
// Test 8: ReplayWithEmptyLog
// ---------------------------------------------------------------------------

TEST(DocumentEngineLogSnapshotTest, ReplayWithEmptyLog)
{
    auto s = buildTestSchema();
    DocumentEngine engine(s.schema);

    auto seed = seedEngine(engine, s);

    const auto snapshot = engine.createSnapshot();

    DocumentEngine restored(s.schema);
    restored.restoreSnapshot(snapshot);

    // Replaying an empty ByteArray must succeed without changing state
    const ByteArray emptyLog;
    EXPECT_NO_THROW(restored.replayCommitLog(emptyLog));

    // State unchanged
    EXPECT_EQ(restored.view(s.itemTable).count(), 3U);
    EXPECT_EQ(restored.read(seed.item1Id, s.itemName).asString(), "Alpha");
    EXPECT_EQ(restored.read(seed.item2Id, s.itemValue).asInt64(), 200);
    EXPECT_EQ(restored.read(seed.item3Id, s.itemCode).asString(), "C3");

    EXPECT_EQ(restored.listLength(s.itemList, idValue(seed.groupId)), 2U);
}

// ---------------------------------------------------------------------------
// Test 9: SnapshotPreservesListOrder
// ---------------------------------------------------------------------------

TEST(DocumentEngineLogSnapshotTest, SnapshotPreservesListOrder)
{
    auto s = buildTestSchema();
    DocumentEngine engine(s.schema);

    ItemId groupId = 0;
    ItemId elem0 = 0, elem1 = 0, elem2 = 0;

    {
        auto txn = engine.beginTransaction();
        groupId = txn.insert(s.groupTable, {
            ColumnValue{.column = s.groupId, .value = Value(std::uint64_t{99})},
        });

        auto av = idValue(groupId);
        elem0 = txn.insert(s.itemList, av, 0, {
            ColumnValue{.column = s.listRank, .value = Value(std::int64_t{1})},
        });
        elem1 = txn.insert(s.itemList, av, 1, {
            ColumnValue{.column = s.listRank, .value = Value(std::int64_t{4})},
        });
        elem2 = txn.insert(s.itemList, av, 2, {
            ColumnValue{.column = s.listRank, .value = Value(std::int64_t{9})},
        });

        txn.commit();
    }

    const auto snapshot = engine.createSnapshot();
    DocumentEngine restored(s.schema);
    restored.restoreSnapshot(snapshot);

    auto av = idValue(groupId);

    // Same length
    EXPECT_EQ(restored.listLength(s.itemList, av), 3U);

    // Same elements in same order
    auto listItems = restored.view(s.itemList).toVector();
    ASSERT_EQ(listItems.size(), 3U);

    EXPECT_EQ(listItems[0].id, elem0);
    EXPECT_EQ(restored.read(elem0, s.listRank).asInt64(), 1);

    EXPECT_EQ(listItems[1].id, elem1);
    EXPECT_EQ(restored.read(elem1, s.listRank).asInt64(), 4);

    EXPECT_EQ(listItems[2].id, elem2);
    EXPECT_EQ(restored.read(elem2, s.listRank).asInt64(), 9);
}

// ---------------------------------------------------------------------------
// Test 10: SnapshotPreservesMultiTableState
// ---------------------------------------------------------------------------

TEST(DocumentEngineLogSnapshotTest, SnapshotPreservesMultiTableState)
{
    auto s = buildTestSchema();
    DocumentEngine engine(s.schema);

    // Populate all containers
    auto seed = seedEngine(engine, s);

    // Also populate the state table in the original engine
    ItemId stateId = 0;
    {
        auto txn = engine.beginTransaction();
        stateId = txn.insert(s.stateTable, {
            ColumnValue{.column = s.stateColData, .value = Value("transient")},
        });
        txn.commit();
    }

    const auto snapshot = engine.createSnapshot();

    DocumentEngine restored(s.schema);
    restored.restoreSnapshot(snapshot);

    // ---- Item table ----
    EXPECT_EQ(restored.view(s.itemTable).count(), 3U);

    EXPECT_TRUE(restored.contains(seed.item1Id));
    EXPECT_EQ(restored.read(seed.item1Id, s.itemName).asString(), "Alpha");
    EXPECT_EQ(restored.read(seed.item1Id, s.itemValue).asInt64(), 100);
    EXPECT_EQ(restored.read(seed.item1Id, s.itemCode).asString(), "C1");

    EXPECT_TRUE(restored.contains(seed.item2Id));
    EXPECT_EQ(restored.read(seed.item2Id, s.itemName).asString(), "Beta");
    EXPECT_EQ(restored.read(seed.item2Id, s.itemValue).asInt64(), 200);
    EXPECT_EQ(restored.read(seed.item2Id, s.itemCode).asString(), "C2");

    EXPECT_TRUE(restored.contains(seed.item3Id));
    EXPECT_EQ(restored.read(seed.item3Id, s.itemName).asString(), "Gamma");
    EXPECT_EQ(restored.read(seed.item3Id, s.itemValue).asInt64(), 300);
    EXPECT_EQ(restored.read(seed.item3Id, s.itemCode).asString(), "C3");

    // ---- State table ----
    EXPECT_EQ(restored.view(s.stateTable).count(), 1U);
    EXPECT_TRUE(restored.contains(stateId));
    EXPECT_EQ(restored.read(stateId, s.stateColData).asString(), "transient");

    // ---- Group table ----
    EXPECT_EQ(restored.view(s.groupTable).count(), 1U);
    EXPECT_TRUE(restored.contains(seed.groupId));
    EXPECT_EQ(restored.read(seed.groupId, s.groupId).asUInt64(), 42U);

    // ---- List ----
    auto assocValue = idValue(seed.groupId);
    EXPECT_EQ(restored.listLength(s.itemList, assocValue), 2U);

    auto listItems = restored.view(s.itemList).toVector();
    ASSERT_EQ(listItems.size(), 2U);

    EXPECT_EQ(listItems[0].id, seed.listElem1Id);
    EXPECT_EQ(restored.read(seed.listElem1Id, s.listRank).asInt64(), 10);

    EXPECT_EQ(listItems[1].id, seed.listElem2Id);
    EXPECT_EQ(restored.read(seed.listElem2Id, s.listRank).asInt64(), 20);
}

} // namespace
