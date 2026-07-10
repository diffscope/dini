#include <cstdint>
#include <optional>
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

Value idValue(ItemId id)
{
    return Value(static_cast<std::uint64_t>(id));
}

Value itemValue(const ItemSnapshot &item, ColumnHandle column)
{
    for (const auto &columnValue : item.values) {
        if (columnValue.column == column) {
            return columnValue.value;
        }
    }
    return Value::null();
}

struct PendingUpdateSchema {
    EngineSchema schema;
    TableHandle table;
    ColumnHandle value;
    ColumnHandle mirror;
    ColumnHandle total;
};

PendingUpdateSchema buildPendingUpdateSchema()
{
    SchemaBuilder builder;
    auto table = builder.createTable("Item");
    auto value = table.addColumn(int64Column("value", IndexKind::Normal));
    auto mirror = table.addColumn(int64Column("mirror", IndexKind::Normal));
    auto total = table.addComputedColumn(ComputedColumnDefinition {
        .debugName = "total",
        .type = ValueType::Int64,
        .index = IndexKind::Normal,
        .nullable = false,
        .dependsOn = {value, mirror},
        .compute = [](const std::vector<Value> &values) {
            return Value(values[0].asInt64() + values[1].asInt64());
        },
    });
    table.addHook(HookDefinition {
        .stage = HookStage::BeforeApply,
        .callback = [value, mirror](TransactionContext &ctx, const ChangeSet &changeSet) {
            for (const auto &operation : changeSet.operations()) {
                if (operation.kind() != ChangeOperationKind::ColumnUpdated) {
                    continue;
                }
                const auto &updated = std::get<ColumnUpdatedChange>(operation.payload());
                if (updated.column == value) {
                    ctx.update(updated.itemId, mirror, Value(updated.newValue.asInt64() * 2));
                }
            }
        },
    });

    return {
        .schema = builder.freeze(),
        .table = table.handle(),
        .value = value,
        .mirror = mirror,
        .total = total,
    };
}

ItemId seedPendingUpdateEngine(DocumentEngine &engine, const PendingUpdateSchema &schema)
{
    auto txn = engine.beginTransaction();
    auto id = txn.insert(schema.table, {
        ColumnValue {.column = schema.value, .value = Value(std::int64_t {1})},
        ColumnValue {.column = schema.mirror, .value = Value(std::int64_t {2})},
    });
    txn.commit();
    engine.clearUndoHistory();
    return id;
}

TEST(DocumentEnginePendingSourceHookTest, BeforeApplyHookInitializesPendingInsertedRow)
{
    SchemaBuilder builder;
    auto table = builder.createTable("Item");
    auto value = table.addColumn(int64Column("value", IndexKind::Normal));
    auto initialized = table.addColumn(int64Column("initialized", IndexKind::Normal));
    table.addHook(HookDefinition {
        .stage = HookStage::BeforeApply,
        .callback = [initialized](TransactionContext &ctx, const ChangeSet &changeSet) {
            for (const auto &operation : changeSet.operations()) {
                if (operation.kind() == ChangeOperationKind::ItemInserted) {
                    const auto &inserted = std::get<ItemInsertedChange>(operation.payload());
                    ctx.update(inserted.item.id, initialized, Value(std::int64_t {7}));
                }
            }
        },
    });
    auto schema = builder.freeze();
    DocumentEngine engine(schema);

    auto txn = engine.beginTransaction();
    auto itemId = txn.insert(table.handle(), {
        ColumnValue {.column = value, .value = Value(std::int64_t {3})},
    });
    auto result = txn.commit();

    EXPECT_EQ(engine.read(itemId, initialized).asInt64(), 7);
    ASSERT_EQ(result.changeSet.operations().size(), 1U);
    const auto &inserted = std::get<ItemInsertedChange>(result.changeSet.operations().front().payload());
    EXPECT_EQ(itemValue(inserted.item, initialized).asInt64(), 7);
}

TEST(DocumentEnginePendingSourceHookTest, BeforeApplyHookUpdatesPendingSourceRowAndDerivedLinks)
{
    auto schema = buildPendingUpdateSchema();
    DocumentEngine engine(schema.schema);
    auto itemId = seedPendingUpdateEngine(engine, schema);

    auto txn = engine.beginTransaction();
    txn.update(itemId, schema.value, Value(std::int64_t {5}));
    auto result = txn.commit();

    EXPECT_EQ(engine.read(itemId, schema.value).asInt64(), 5);
    EXPECT_EQ(engine.read(itemId, schema.mirror).asInt64(), 10);
    EXPECT_EQ(engine.read(itemId, schema.total).asInt64(), 15);

    std::optional<std::size_t> sourceOperation;
    std::optional<std::size_t> derivedMirrorOperation;
    std::optional<std::size_t> computedOperation;
    for (std::size_t i = 0; i < result.changeSet.operations().size(); ++i) {
        const auto &operation = result.changeSet.operations()[i];
        if (operation.kind() == ChangeOperationKind::ColumnUpdated) {
            const auto &updated = std::get<ColumnUpdatedChange>(operation.payload());
            if (updated.column == schema.value) {
                sourceOperation = i;
            } else if (updated.column == schema.mirror) {
                derivedMirrorOperation = i;
            }
        } else if (operation.kind() == ChangeOperationKind::ComputedColumnUpdated) {
            const auto &updated = std::get<ComputedColumnUpdatedChange>(operation.payload());
            if (updated.column == schema.total) {
                computedOperation = i;
            }
        }
    }
    ASSERT_TRUE(sourceOperation.has_value());
    ASSERT_TRUE(derivedMirrorOperation.has_value());
    ASSERT_TRUE(computedOperation.has_value());

    bool linkedMirrorToSource = false;
    bool linkedComputedToSource = false;
    for (const auto &link : result.changeSet.derivedLinks()) {
        if (link.sourceOperation == *sourceOperation && link.derivedOperation == *derivedMirrorOperation) {
            linkedMirrorToSource = true;
        }
        if (link.sourceOperation == *sourceOperation && link.derivedOperation == *computedOperation) {
            linkedComputedToSource = true;
        }
    }
    EXPECT_TRUE(linkedMirrorToSource);
    EXPECT_TRUE(linkedComputedToSource);
}

TEST(DocumentEnginePendingSourceHookTest, PendingSourceUpdatesSurviveUndoRedoAndReplay)
{
    auto schema = buildPendingUpdateSchema();
    DocumentEngine engine(schema.schema);
    auto itemId = seedPendingUpdateEngine(engine, schema);
    auto snapshot = engine.createSnapshot();

    CommitResult result;
    {
        auto txn = engine.beginTransaction();
        txn.update(itemId, schema.value, Value(std::int64_t {5}));
        result = txn.commit();
    }

    ASSERT_TRUE(engine.canUndo());
    engine.undo();
    EXPECT_EQ(engine.read(itemId, schema.value).asInt64(), 1);
    EXPECT_EQ(engine.read(itemId, schema.mirror).asInt64(), 2);
    EXPECT_EQ(engine.read(itemId, schema.total).asInt64(), 3);

    ASSERT_TRUE(engine.canRedo());
    engine.redo();
    EXPECT_EQ(engine.read(itemId, schema.value).asInt64(), 5);
    EXPECT_EQ(engine.read(itemId, schema.mirror).asInt64(), 10);
    EXPECT_EQ(engine.read(itemId, schema.total).asInt64(), 15);

    DocumentEngine replay(schema.schema);
    replay.restoreSnapshot(snapshot);
    replay.replayChangeSet(ChangeSet::deserialize(result.changeSet.serialize()));
    EXPECT_EQ(replay.read(itemId, schema.value).asInt64(), 5);
    EXPECT_EQ(replay.read(itemId, schema.mirror).asInt64(), 10);
    EXPECT_EQ(replay.read(itemId, schema.total).asInt64(), 15);
}

TEST(DocumentEnginePendingSourceHookTest, PendingSourceUpdatesDoNotBypassComputedColumnProtection)
{
    SchemaBuilder builder;
    auto table = builder.createTable("Item");
    auto value = table.addColumn(int64Column("value", IndexKind::Normal));
    auto computed = table.addComputedColumn(ComputedColumnDefinition {
        .debugName = "computed",
        .type = ValueType::Int64,
        .index = IndexKind::Normal,
        .nullable = false,
        .dependsOn = {value},
        .compute = [](const std::vector<Value> &values) {
            return Value(values[0].asInt64() + 1);
        },
    });
    table.addHook(HookDefinition {
        .stage = HookStage::BeforeApply,
        .callback = [computed](TransactionContext &ctx, const ChangeSet &changeSet) {
            for (const auto &operation : changeSet.operations()) {
                if (operation.kind() == ChangeOperationKind::ItemInserted) {
                    const auto &inserted = std::get<ItemInsertedChange>(operation.payload());
                    ctx.update(inserted.item.id, computed, Value(std::int64_t {9}));
                }
            }
        },
    });
    auto schema = builder.freeze();
    DocumentEngine engine(schema);

    auto txn = engine.beginTransaction();
    EXPECT_THROW((void)txn.insert(table.handle(), std::vector<ColumnValue> {
        ColumnValue {.column = value, .value = Value(std::int64_t {1})},
    }), ConstraintError);
}

TEST(DocumentEnginePendingSourceHookTest, PendingSourceUpdatesDoNotBypassAssociationRules)
{
    SchemaBuilder builder;
    auto parentTable = builder.createTable("Parent");
    auto parentValue = parentTable.addColumn(int64Column("value", IndexKind::Normal));
    auto childTable = builder.createTable("Child");
    auto childParent = childTable.addAssociation({
        .debugName = "parent",
        .target = parentTable.handle(),
    });
    auto childValue = childTable.addColumn(int64Column("value", IndexKind::Normal));
    childTable.addHook(HookDefinition {
        .stage = HookStage::BeforeApply,
        .callback = [childParent](TransactionContext &ctx, const ChangeSet &changeSet) {
            for (const auto &operation : changeSet.operations()) {
                if (operation.kind() == ChangeOperationKind::ItemInserted) {
                    const auto &inserted = std::get<ItemInsertedChange>(operation.payload());
                    ctx.update(inserted.item.id, childParent.column(), Value::null());
                }
            }
        },
    });
    auto schema = builder.freeze();
    DocumentEngine engine(schema);

    auto seed = engine.beginTransaction();
    auto parentId = seed.insert(parentTable.handle(), {
        ColumnValue {.column = parentValue, .value = Value(std::int64_t {1})},
    });
    seed.commit();

    auto txn = engine.beginTransaction();
    EXPECT_THROW((void)txn.insert(childTable.handle(), std::vector<ColumnValue> {
        ColumnValue {.column = childParent.column(), .value = idValue(parentId)},
        ColumnValue {.column = childValue, .value = Value(std::int64_t {2})},
    }), ConstraintError);
}

} // namespace
