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

struct EventTestSchema {
    EngineSchema schema;
    TableHandle itemTable;
    ColumnHandle itemName;
};

EventTestSchema buildEventSchema()
{
    SchemaBuilder builder;

    auto itemBuilder = builder.createTable("Item");
    auto itemName = itemBuilder.addColumn(stringColumn("name", IndexKind::Normal));

    auto schema = builder.freeze();
    return EventTestSchema {
        schema,
        itemBuilder.handle(),
        itemName,
    };
}

ItemId seedEventEngine(DocumentEngine &engine, const EventTestSchema &s)
{
    auto txn = engine.beginTransaction();
    const auto itemId = txn.insert(s.itemTable, {
        ColumnValue {.column = s.itemName, .value = Value("alpha")},
    });
    txn.commit();
    return itemId;
}

TEST(DocumentEngineEventTest, EventAfterApplyOnCommit)
{
    auto s = buildEventSchema();
    DocumentEngine engine(s.schema);

    std::vector<EngineEvent> events;
    auto sub = engine.subscribe([&events](const EngineEvent &e) {
        events.push_back(e);
    });

    ItemId itemId = 0;
    {
        auto txn = engine.beginTransaction();
        itemId = txn.insert(s.itemTable, {
            ColumnValue {.column = s.itemName, .value = Value("beta")},
        });
        txn.commit();
    }

    bool sawAfterApply = false;
    bool changeSetNonEmpty = false;
    for (const auto &e : events) {
        if (e.kind == EventKind::AfterApply) {
            sawAfterApply = true;
            if (!e.changeSet.empty()) {
                changeSetNonEmpty = true;
            }
        }
    }

    EXPECT_TRUE(sawAfterApply);
    EXPECT_TRUE(changeSetNonEmpty);
    EXPECT_TRUE(engine.contains(itemId));
}

TEST(DocumentEngineEventTest, EventAfterCommitOnCommit)
{
    auto s = buildEventSchema();
    DocumentEngine engine(s.schema);

    std::vector<EngineEvent> events;
    auto sub = engine.subscribe([&events](const EngineEvent &e) {
        events.push_back(e);
    });

    {
        auto txn = engine.beginTransaction();
        txn.insert(s.itemTable, {
            ColumnValue {.column = s.itemName, .value = Value("gamma")},
        });
        txn.commit();
    }

    bool sawAfterCommit = false;
    for (const auto &e : events) {
        if (e.kind == EventKind::AfterCommit) {
            sawAfterCommit = true;
        }
    }

    EXPECT_TRUE(sawAfterCommit);
}

TEST(DocumentEngineEventTest, EventRollbackOnRollback)
{
    auto s = buildEventSchema();
    DocumentEngine engine(s.schema);

    std::vector<EngineEvent> events;
    auto sub = engine.subscribe([&events](const EngineEvent &e) {
        events.push_back(e);
    });

    ItemId itemId = 0;
    {
        auto txn = engine.beginTransaction();
        itemId = txn.insert(s.itemTable, {
            ColumnValue {.column = s.itemName, .value = Value("delta")},
        });
        txn.rollback();
    }

    bool sawRollback = false;
    bool changeSetNonEmpty = false;
    for (const auto &e : events) {
        if (e.kind == EventKind::Rollback) {
            sawRollback = true;
            if (!e.changeSet.empty()) {
                changeSetNonEmpty = true;
            }
        }
    }

    EXPECT_TRUE(sawRollback);
    EXPECT_TRUE(changeSetNonEmpty);
    EXPECT_FALSE(engine.contains(itemId));
}

TEST(DocumentEngineEventTest, EventOriginNormal)
{
    auto s = buildEventSchema();
    DocumentEngine engine(s.schema);
    auto ids = seedEventEngine(engine, s);

    EventOrigin lastOrigin = EventOrigin::Undo;
    EventKind lastKind = EventKind::Rollback;
    auto sub = engine.subscribe([&lastOrigin, &lastKind](const EngineEvent &e) {
        lastOrigin = e.origin;
        lastKind = e.kind;
    });

    {
        auto txn = engine.beginTransaction();
        txn.update(ids, s.itemName, Value("epsilon"));
        txn.commit();
    }

    EXPECT_EQ(lastOrigin, EventOrigin::Normal);
    EXPECT_NE(lastKind, EventKind::Rollback);
}

TEST(DocumentEngineEventTest, EventOriginUndo)
{
    auto s = buildEventSchema();
    DocumentEngine engine(s.schema);
    auto ids = seedEventEngine(engine, s);

    {
        auto txn = engine.beginTransaction();
        txn.update(ids, s.itemName, Value("zeta"));
        txn.commit();
    }

    EventOrigin lastOrigin = EventOrigin::Normal;
    auto sub = engine.subscribe([&lastOrigin](const EngineEvent &e) {
        lastOrigin = e.origin;
    });

    ASSERT_TRUE(engine.canUndo());
    engine.undo();

    EXPECT_EQ(lastOrigin, EventOrigin::Undo);
}

TEST(DocumentEngineEventTest, EventOriginRedo)
{
    auto s = buildEventSchema();
    DocumentEngine engine(s.schema);
    auto ids = seedEventEngine(engine, s);

    {
        auto txn = engine.beginTransaction();
        txn.update(ids, s.itemName, Value("eta"));
        txn.commit();
    }

    ASSERT_TRUE(engine.canUndo());
    engine.undo();

    EventOrigin lastOrigin = EventOrigin::Undo;
    auto sub = engine.subscribe([&lastOrigin](const EngineEvent &e) {
        lastOrigin = e.origin;
    });

    ASSERT_TRUE(engine.canRedo());
    engine.redo();

    EXPECT_EQ(lastOrigin, EventOrigin::Redo);
}

TEST(DocumentEngineEventTest, SubscriptionDisconnectStopsEvents)
{
    auto s = buildEventSchema();
    DocumentEngine engine(s.schema);
    auto ids = seedEventEngine(engine, s);

    int eventCount = 0;
    auto sub = engine.subscribe([&eventCount](const EngineEvent &) {
        ++eventCount;
    });

    sub.disconnect();
    EXPECT_FALSE(sub.connected());

    {
        auto txn = engine.beginTransaction();
        txn.update(ids, s.itemName, Value("theta"));
        txn.commit();
    }

    EXPECT_EQ(eventCount, 0);
}

TEST(DocumentEngineEventTest, SubscriptionDestructorDisconnects)
{
    auto s = buildEventSchema();
    DocumentEngine engine(s.schema);
    auto ids = seedEventEngine(engine, s);

    int eventCount = 0;
    {
        auto sub = engine.subscribe([&eventCount](const EngineEvent &) {
            ++eventCount;
        });
        EXPECT_TRUE(sub.connected());
    }

    {
        auto txn = engine.beginTransaction();
        txn.update(ids, s.itemName, Value("iota"));
        txn.commit();
    }

    EXPECT_EQ(eventCount, 0);
}

TEST(DocumentEngineEventTest, MultipleSubscriptions)
{
    auto s = buildEventSchema();
    DocumentEngine engine(s.schema);

    int count1 = 0;
    int count2 = 0;
    auto sub1 = engine.subscribe([&count1](const EngineEvent &) {
        ++count1;
    });
    auto sub2 = engine.subscribe([&count2](const EngineEvent &) {
        ++count2;
    });

    {
        auto txn = engine.beginTransaction();
        txn.insert(s.itemTable, {
            ColumnValue {.column = s.itemName, .value = Value("kappa")},
        });
        txn.commit();
    }

    EXPECT_GT(count1, 0);
    EXPECT_EQ(count1, count2);
}

} // namespace
