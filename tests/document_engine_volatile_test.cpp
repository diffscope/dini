#include <cstdint>
#include <vector>

#include <dini/engine.h>

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

ColumnDefinition uint64Column(const char *debugName, IndexKind index = IndexKind::None)
{
    return ColumnDefinition {
        .debugName = debugName,
        .type = ValueType::UInt64,
        .index = index,
        .defaultValue = Value(static_cast<std::uint64_t>(0)),
        .nullable = false,
    };
}

Value idValue(ItemId id)
{
    return Value(static_cast<std::uint64_t>(id));
}

struct VolatileTestSchema {
    EngineSchema schema;
    TableHandle itemTable;
    ColumnHandle itemName;
    ColumnHandle volatileTag;
    TableHandle volatileItemTable;
    ColumnHandle volatileItemName;
    TableHandle groupTable;
    ColumnHandle groupIdCol;
    ListHandle groupList;
    ColumnHandle groupListData;
};

VolatileTestSchema buildVolatileSchema()
{
    SchemaBuilder builder;

    auto itemBuilder = builder.createTable("Item");
    auto itemName = itemBuilder.addColumn(stringColumn("name", IndexKind::Normal));
    auto volatileTag = itemBuilder.addColumn(ColumnDefinition {
        .debugName = "volatileTag",
        .type = ValueType::String,
        .index = IndexKind::None,
        .defaultValue = Value(""),
        .nullable = false,
        .volatileData = true,
    });

    auto volatileItemBuilder = builder.createTable("VolatileItem");
    volatileItemBuilder.setVolatile(true);
    auto volatileItemName = volatileItemBuilder.addColumn(stringColumn("name", IndexKind::Normal));

    auto groupBuilder = builder.createTable("Group");
    auto groupIdCol = groupBuilder.addColumn(uint64Column("id", IndexKind::Normal));

    auto groupListBuilder = builder.createList("GroupList");
    groupListBuilder.setVolatile(true);
    auto groupListAssoc = groupListBuilder.setAssociation(AssociationDefinition {
        .debugName = "group",
        .target = groupBuilder.handle(),
    });
    auto groupListData = groupListBuilder.addColumn(stringColumn("data"));

    auto schema = builder.freeze();
    return VolatileTestSchema {
        schema,
        itemBuilder.handle(),
        itemName,
        volatileTag,
        volatileItemBuilder.handle(),
        volatileItemName,
        groupBuilder.handle(),
        groupIdCol,
        groupListBuilder.handle(),
        groupListData,
    };
}

struct VolatileSeed {
    ItemId itemId;
    ItemId groupId;
};

VolatileSeed seedVolatileEngine(DocumentEngine &engine, const VolatileTestSchema &s)
{
    auto txn = engine.beginTransaction();
    const auto itemId = txn.insert(s.itemTable, {
        ColumnValue {.column = s.itemName, .value = Value("hello")},
        ColumnValue {.column = s.volatileTag, .value = Value("initial")},
    });
    const auto groupId = txn.insert(s.groupTable, {
        ColumnValue {.column = s.groupIdCol, .value = Value(static_cast<std::uint64_t>(1))},
    });
    txn.commit();
    return VolatileSeed {itemId, groupId};
}

TEST(DocumentEngineVolatileTest, VolatileColumnSkipsSnapshot)
{
    auto s = buildVolatileSchema();
    DocumentEngine engine(s.schema);

    ItemId itemId = 0;
    {
        auto txn = engine.beginTransaction();
        itemId = txn.insert(s.itemTable, {
            ColumnValue {.column = s.itemName, .value = Value("hello")},
            ColumnValue {.column = s.volatileTag, .value = Value("secret")},
        });
        txn.commit();
    }

    EXPECT_EQ(engine.read(itemId, s.itemName).asString(), "hello");
    EXPECT_EQ(engine.read(itemId, s.volatileTag).asString(), "secret");

    const auto snapshot = engine.createSnapshot();
    DocumentEngine restored(s.schema);
    restored.restoreSnapshot(snapshot);

    EXPECT_TRUE(restored.contains(itemId));
    EXPECT_EQ(restored.read(itemId, s.itemName).asString(), "hello");
    EXPECT_EQ(restored.read(itemId, s.volatileTag).asString(), "");
}

TEST(DocumentEngineVolatileTest, VolatileTableSkipsSnapshot)
{
    auto s = buildVolatileSchema();
    DocumentEngine engine(s.schema);

    ItemId volatileItemId = 0;
    {
        auto txn = engine.beginTransaction();
        volatileItemId = txn.insert(s.volatileItemTable, {
            ColumnValue {.column = s.volatileItemName, .value = Value("volatileRow")},
        });
        txn.commit();
    }

    EXPECT_TRUE(engine.contains(volatileItemId));
    EXPECT_EQ(engine.read(volatileItemId, s.volatileItemName).asString(), "volatileRow");

    const auto snapshot = engine.createSnapshot();
    DocumentEngine restored(s.schema);
    restored.restoreSnapshot(snapshot);

    EXPECT_FALSE(restored.contains(volatileItemId));
    const auto allItems = restored.view(s.volatileItemTable).toVector();
    EXPECT_TRUE(allItems.empty());
}

TEST(DocumentEngineVolatileTest, VolatileColumnSkipsRollback)
{
    auto s = buildVolatileSchema();
    DocumentEngine engine(s.schema);
    auto ids = seedVolatileEngine(engine, s);

    {
        auto txn = engine.beginTransaction();
        txn.update(ids.itemId, s.volatileTag, Value("newValue"));
        txn.rollback();
    }

    EXPECT_EQ(engine.read(ids.itemId, s.volatileTag).asString(), "newValue");
    EXPECT_EQ(engine.read(ids.itemId, s.itemName).asString(), "hello");
    EXPECT_TRUE(engine.contains(ids.itemId));
}

TEST(DocumentEngineVolatileTest, VolatileColumnSkipsUndo)
{
    auto s = buildVolatileSchema();
    DocumentEngine engine(s.schema);
    auto ids = seedVolatileEngine(engine, s);

    {
        auto txn = engine.beginTransaction();
        txn.update(ids.itemId, s.volatileTag, Value("afterCommit"));
        txn.commit();
    }

    EXPECT_EQ(engine.read(ids.itemId, s.volatileTag).asString(), "afterCommit");

    ASSERT_TRUE(engine.canUndo());
    engine.undo();

    EXPECT_EQ(engine.read(ids.itemId, s.volatileTag).asString(), "afterCommit");
    EXPECT_EQ(engine.read(ids.itemId, s.itemName).asString(), "hello");
}

TEST(DocumentEngineVolatileTest, VolatileListSkipsSnapshot)
{
    auto s = buildVolatileSchema();
    DocumentEngine engine(s.schema);

    ItemId groupId = 0;
    {
        auto txn = engine.beginTransaction();
        groupId = txn.insert(s.groupTable, {
            ColumnValue {.column = s.groupIdCol, .value = Value(static_cast<std::uint64_t>(1))},
        });
        txn.insert(s.groupList, idValue(groupId), 0, {
            ColumnValue {.column = s.groupListData, .value = Value("element1")},
        });
        txn.insert(s.groupList, idValue(groupId), 1, {
            ColumnValue {.column = s.groupListData, .value = Value("element2")},
        });
        txn.commit();
    }

    EXPECT_EQ(engine.listLength(s.groupList, idValue(groupId)), 2U);

    const auto snapshot = engine.createSnapshot();
    DocumentEngine restored(s.schema);
    restored.restoreSnapshot(snapshot);

    EXPECT_TRUE(restored.contains(groupId));
    EXPECT_EQ(restored.listLength(s.groupList, idValue(groupId)), 0U);
}

TEST(DocumentEngineVolatileTest, FilterVolatile)
{
    auto s = buildVolatileSchema();
    DocumentEngine engine(s.schema);
    auto ids = seedVolatileEngine(engine, s);

    CommitResult result;
    {
        auto txn = engine.beginTransaction();
        txn.update(ids.itemId, s.itemName, Value("updated"));
        txn.update(ids.itemId, s.volatileTag, Value("tag2"));
        result = txn.commit();
    }

    const auto originalSize = result.changeSet.operations().size();
    ASSERT_GT(originalSize, 0U);

    const auto filtered = result.changeSet.filterVolatile();
    const auto filteredSize = filtered.operations().size();

    EXPECT_LT(filteredSize, originalSize);
    EXPECT_FALSE(filtered.empty());
}

} // namespace
