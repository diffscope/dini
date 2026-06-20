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

struct UniqueTestSchema {
    EngineSchema schema;
    TableHandle orphanTable;
    ColumnHandle orphanName;
    ColumnHandle orphanValue;
    TableHandle groupTable;
    ColumnHandle groupIdCol;
    TableHandle memberTable;
    RelationHandle memberParent;
    ColumnHandle memberName;
    ColumnHandle memberValue;
};

UniqueTestSchema buildUniqueTestSchema()
{
    UniqueTestSchema s;
    SchemaBuilder builder;

    // Orphan: no parent, name has global Unique index.
    auto orphanTable = builder.createTable("Orphan");
    s.orphanName = orphanTable.addColumn(stringColumn("name", IndexKind::Unique));
    s.orphanValue = orphanTable.addColumn(ColumnDefinition {
        .debugName = "value",
        .type = ValueType::Int64,
        .index = IndexKind::Normal,
        .defaultValue = Value(std::int64_t {0}),
        .nullable = false,
    });
    s.orphanTable = orphanTable.handle();

    // Group: simple identity anchor for parent-scoped unique tests.
    auto groupTable = builder.createTable("Group");
    s.groupIdCol = groupTable.addColumn(ColumnDefinition {
        .debugName = "id_col",
        .type = ValueType::UInt64,
        .index = IndexKind::Normal,
        .defaultValue = Value(std::uint64_t {0}),
        .nullable = false,
    });
    s.groupTable = groupTable.handle();

    // Member: parent-scoped unique on name. Nulls do not participate in
    // uniqueness so that multiple items with no-name are legal.
    auto memberTable = builder.createTable("Member");
    s.memberParent = memberTable.addAssociation(AssociationDefinition {
        .debugName = "group",
        .target = groupTable.handle(),
    });
    s.memberName = memberTable.addColumn(ColumnDefinition {
        .debugName = "name",
        .type = ValueType::String,
        .index = IndexKind::Unique,
        .defaultValue = Value(""),
        .nullable = true,
        .participatesInUniqueWhenNull = false,
    });
    s.memberValue = memberTable.addColumn(ColumnDefinition {
        .debugName = "value",
        .type = ValueType::Int64,
        .index = IndexKind::Normal,
        .defaultValue = Value(std::int64_t {0}),
        .nullable = false,
    });
    s.memberTable = memberTable.handle();

    s.schema = builder.freeze();
    return s;
}

TEST(DocumentEngineUniqueTest, UniqueNoParentNoConflict)
{
    auto s = buildUniqueTestSchema();
    DocumentEngine engine(s.schema);

    auto tx = engine.beginTransaction();
    ItemId alpha = tx.insert(s.orphanTable, {
        ColumnValue {.column = s.orphanName, .value = Value("alpha")},
        ColumnValue {.column = s.orphanValue, .value = Value(std::int64_t {1})},
    });
    ItemId beta = tx.insert(s.orphanTable, {
        ColumnValue {.column = s.orphanName, .value = Value("beta")},
        ColumnValue {.column = s.orphanValue, .value = Value(std::int64_t {2})},
    });
    ItemId gamma = tx.insert(s.orphanTable, {
        ColumnValue {.column = s.orphanName, .value = Value("gamma")},
        ColumnValue {.column = s.orphanValue, .value = Value(std::int64_t {3})},
    });
    tx.commit();

    EXPECT_TRUE(engine.contains(alpha));
    EXPECT_TRUE(engine.contains(beta));
    EXPECT_TRUE(engine.contains(gamma));
    EXPECT_EQ(engine.read(alpha, s.orphanName).asString(), "alpha");
    EXPECT_EQ(engine.read(beta, s.orphanName).asString(), "beta");
    EXPECT_EQ(engine.read(gamma, s.orphanName).asString(), "gamma");
}

TEST(DocumentEngineUniqueTest, UniqueNoParentConflict)
{
    auto s = buildUniqueTestSchema();
    DocumentEngine engine(s.schema);

    auto tx = engine.beginTransaction();
    ItemId alpha = tx.insert(s.orphanTable, {
        ColumnValue {.column = s.orphanName, .value = Value("alpha")},
        ColumnValue {.column = s.orphanValue, .value = Value(std::int64_t {1})},
    });
    tx.commit();
    EXPECT_TRUE(engine.contains(alpha));

    // Second insert with the same name must be rejected by the unique index.
    auto tx2 = engine.beginTransaction();
    EXPECT_THROW(
        tx2.insert(s.orphanTable, {
            ColumnValue {.column = s.orphanName, .value = Value("alpha")},
            ColumnValue {.column = s.orphanValue, .value = Value(std::int64_t {2})},
        }),
        ConstraintError);
}

TEST(DocumentEngineUniqueTest, UniqueWithParentNoConflict)
{
    auto s = buildUniqueTestSchema();
    DocumentEngine engine(s.schema);

    auto tx = engine.beginTransaction();
    ItemId group1 = tx.insert(s.groupTable, {
        ColumnValue {.column = s.groupIdCol, .value = Value(std::uint64_t {1})},
    });
    ItemId group2 = tx.insert(s.groupTable, {
        ColumnValue {.column = s.groupIdCol, .value = Value(std::uint64_t {2})},
    });
    ItemId m1 = tx.insert(s.memberTable, {
        ColumnValue {.column = s.memberParent.column(), .value = idValue(group1)},
        ColumnValue {.column = s.memberName, .value = Value("alice")},
        ColumnValue {.column = s.memberValue, .value = Value(std::int64_t {10})},
    });
    // Same name under a different parent is a different unique scope.
    ItemId m2 = tx.insert(s.memberTable, {
        ColumnValue {.column = s.memberParent.column(), .value = idValue(group2)},
        ColumnValue {.column = s.memberName, .value = Value("alice")},
        ColumnValue {.column = s.memberValue, .value = Value(std::int64_t {20})},
    });
    tx.commit();

    EXPECT_TRUE(engine.contains(m1));
    EXPECT_TRUE(engine.contains(m2));
    EXPECT_EQ(engine.read(m1, s.memberName).asString(), "alice");
    EXPECT_EQ(engine.read(m2, s.memberName).asString(), "alice");
    EXPECT_EQ(engine.read(m1, s.memberParent.column()).asUInt64(),
              static_cast<std::uint64_t>(group1));
    EXPECT_EQ(engine.read(m2, s.memberParent.column()).asUInt64(),
              static_cast<std::uint64_t>(group2));
}

TEST(DocumentEngineUniqueTest, UniqueWithParentConflict)
{
    auto s = buildUniqueTestSchema();
    DocumentEngine engine(s.schema);

    auto tx = engine.beginTransaction();
    ItemId group1 = tx.insert(s.groupTable, {
        ColumnValue {.column = s.groupIdCol, .value = Value(std::uint64_t {1})},
    });
    tx.insert(s.memberTable, {
        ColumnValue {.column = s.memberParent.column(), .value = idValue(group1)},
        ColumnValue {.column = s.memberName, .value = Value("alice")},
        ColumnValue {.column = s.memberValue, .value = Value(std::int64_t {10})},
    });
    tx.commit();

    // Same name under the same parent must be rejected.
    auto tx2 = engine.beginTransaction();
    EXPECT_THROW(
        tx2.insert(s.memberTable, {
            ColumnValue {.column = s.memberParent.column(), .value = idValue(group1)},
            ColumnValue {.column = s.memberName, .value = Value("alice")},
            ColumnValue {.column = s.memberValue, .value = Value(std::int64_t {99})},
        }),
        ConstraintError);
}

TEST(DocumentEngineUniqueTest, UniqueNullParticipates)
{
    auto s = buildUniqueTestSchema();
    DocumentEngine engine(s.schema);

    auto tx = engine.beginTransaction();
    ItemId group1 = tx.insert(s.groupTable, {
        ColumnValue {.column = s.groupIdCol, .value = Value(std::uint64_t {1})},
    });
    tx.commit();

    // Multiple null name values are legal when participatesInUniqueWhenNull is false.
    auto tx2 = engine.beginTransaction();
    ItemId m1 = tx2.insert(s.memberTable, {
        ColumnValue {.column = s.memberParent.column(), .value = idValue(group1)},
        ColumnValue {.column = s.memberName, .value = Value::null()},
        ColumnValue {.column = s.memberValue, .value = Value(std::int64_t {1})},
    });
    ItemId m2 = tx2.insert(s.memberTable, {
        ColumnValue {.column = s.memberParent.column(), .value = idValue(group1)},
        ColumnValue {.column = s.memberName, .value = Value::null()},
        ColumnValue {.column = s.memberValue, .value = Value(std::int64_t {2})},
    });
    tx2.commit();

    EXPECT_TRUE(engine.contains(m1));
    EXPECT_TRUE(engine.contains(m2));
    EXPECT_TRUE(engine.read(m1, s.memberName).isNull());
    EXPECT_TRUE(engine.read(m2, s.memberName).isNull());
}

TEST(DocumentEngineUniqueTest, UniqueNullParticipatesEnabled)
{
    // Build a second schema where a unique column has participatesInUniqueWhenNull
    // set to true. Only one null value is allowed across the unique scope.
    SchemaBuilder builder;
    auto table = builder.createTable("StrictNull");
    auto nameCol = table.addColumn(ColumnDefinition {
        .debugName = "name",
        .type = ValueType::String,
        .index = IndexKind::Unique,
        .defaultValue = Value(""),
        .nullable = true,
        .participatesInUniqueWhenNull = true,
    });
    auto schema = builder.freeze();
    DocumentEngine engine(schema);

    auto tx = engine.beginTransaction();
    tx.insert(table.handle(), {
        ColumnValue {.column = nameCol, .value = Value::null()},
    });
    tx.commit();

    auto tx2 = engine.beginTransaction();
    EXPECT_THROW(
        tx2.insert(table.handle(), {
            ColumnValue {.column = nameCol, .value = Value::null()},
        }),
        ConstraintError);
}

TEST(DocumentEngineUniqueTest, UniqueParentNullScope)
{
    auto s = buildUniqueTestSchema();
    DocumentEngine engine(s.schema);

    // When the parent is null, uniqueness is checked globally across all
    // null-parent items instead of per-parent.
    auto tx = engine.beginTransaction();
    ItemId m1 = tx.insert(s.memberTable, {
        ColumnValue {.column = s.memberParent.column(), .value = Value::null()},
        ColumnValue {.column = s.memberName, .value = Value("x")},
        ColumnValue {.column = s.memberValue, .value = Value(std::int64_t {1})},
    });
    tx.commit();
    EXPECT_TRUE(engine.contains(m1));

    // Another null-parent item with the same name must conflict globally.
    auto tx2 = engine.beginTransaction();
    EXPECT_THROW(
        tx2.insert(s.memberTable, {
            ColumnValue {.column = s.memberParent.column(), .value = Value::null()},
            ColumnValue {.column = s.memberName, .value = Value("x")},
            ColumnValue {.column = s.memberValue, .value = Value(std::int64_t {2})},
        }),
        ConstraintError);

    // Same name under a non-null parent is a different unique scope and must succeed.
    auto tx3 = engine.beginTransaction();
    ItemId group1 = tx3.insert(s.groupTable, {
        ColumnValue {.column = s.groupIdCol, .value = Value(std::uint64_t {1})},
    });
    ItemId m3 = tx3.insert(s.memberTable, {
        ColumnValue {.column = s.memberParent.column(), .value = idValue(group1)},
        ColumnValue {.column = s.memberName, .value = Value("x")},
        ColumnValue {.column = s.memberValue, .value = Value(std::int64_t {3})},
    });
    tx3.commit();
    EXPECT_TRUE(engine.contains(m3));
}

TEST(DocumentEngineUniqueTest, UniqueParentIdChange)
{
    auto s = buildUniqueTestSchema();
    DocumentEngine engine(s.schema);

    auto tx = engine.beginTransaction();
    ItemId group1 = tx.insert(s.groupTable, {
        ColumnValue {.column = s.groupIdCol, .value = Value(std::uint64_t {1})},
    });
    ItemId group2 = tx.insert(s.groupTable, {
        ColumnValue {.column = s.groupIdCol, .value = Value(std::uint64_t {2})},
    });
    tx.commit();

    // Insert a member under group2 with name "x".
    ItemId memberX;
    {
        auto tx2 = engine.beginTransaction();
        memberX = tx2.insert(s.memberTable, {
            ColumnValue {.column = s.memberParent.column(), .value = idValue(group2)},
            ColumnValue {.column = s.memberName, .value = Value("x")},
            ColumnValue {.column = s.memberValue, .value = Value(std::int64_t {1})},
        });
        tx2.commit();
    }

    // Move the member to group1. Since group1 has no item named "x", this succeeds.
    {
        auto tx3 = engine.beginTransaction();
        tx3.update(memberX, s.memberParent.column(), idValue(group1));
        tx3.commit();
    }

    EXPECT_EQ(engine.read(memberX, s.memberParent.column()).asUInt64(),
              static_cast<std::uint64_t>(group1));
    EXPECT_EQ(engine.read(memberX, s.memberName).asString(), "x");
}

TEST(DocumentEngineUniqueTest, UniqueParentIdChangeConflict)
{
    auto s = buildUniqueTestSchema();
    DocumentEngine engine(s.schema);

    auto tx = engine.beginTransaction();
    ItemId group1 = tx.insert(s.groupTable, {
        ColumnValue {.column = s.groupIdCol, .value = Value(std::uint64_t {1})},
    });
    ItemId group2 = tx.insert(s.groupTable, {
        ColumnValue {.column = s.groupIdCol, .value = Value(std::uint64_t {2})},
    });
    tx.commit();

    // Insert "x" under group1 and another "x" under group2.
    ItemId memberXinGroup2;
    {
        auto tx2 = engine.beginTransaction();
        tx2.insert(s.memberTable, {
            ColumnValue {.column = s.memberParent.column(), .value = idValue(group1)},
            ColumnValue {.column = s.memberName, .value = Value("x")},
            ColumnValue {.column = s.memberValue, .value = Value(std::int64_t {1})},
        });
        memberXinGroup2 = tx2.insert(s.memberTable, {
            ColumnValue {.column = s.memberParent.column(), .value = idValue(group2)},
            ColumnValue {.column = s.memberName, .value = Value("x")},
            ColumnValue {.column = s.memberValue, .value = Value(std::int64_t {2})},
        });
        tx2.commit();
    }

    // Moving the group2 "x" to group1 must conflict because group1 already has "x".
    auto tx3 = engine.beginTransaction();
    EXPECT_THROW(
        tx3.update(memberXinGroup2, s.memberParent.column(), idValue(group1)),
        ConstraintError);
}

TEST(DocumentEngineUniqueTest, UniqueColumnUpdate)
{
    auto s = buildUniqueTestSchema();
    DocumentEngine engine(s.schema);

    auto tx = engine.beginTransaction();
    ItemId a = tx.insert(s.orphanTable, {
        ColumnValue {.column = s.orphanName, .value = Value("alpha")},
        ColumnValue {.column = s.orphanValue, .value = Value(std::int64_t {1})},
    });
    ItemId b = tx.insert(s.orphanTable, {
        ColumnValue {.column = s.orphanName, .value = Value("beta")},
        ColumnValue {.column = s.orphanValue, .value = Value(std::int64_t {2})},
    });
    tx.commit();

    // Updating "beta" to "alpha" must be rejected because "alpha" already exists.
    auto tx2 = engine.beginTransaction();
    EXPECT_THROW(
        tx2.update(b, s.orphanName, Value("alpha")),
        ConstraintError);

    // Updating to a non-conflicting value must succeed.
    auto tx3 = engine.beginTransaction();
    tx3.update(b, s.orphanName, Value("gamma"));
    tx3.commit();

    EXPECT_EQ(engine.read(a, s.orphanName).asString(), "alpha");
    EXPECT_EQ(engine.read(b, s.orphanName).asString(), "gamma");
}

TEST(DocumentEngineUniqueTest, UniqueUndoRestoresState)
{
    auto s = buildUniqueTestSchema();
    DocumentEngine engine(s.schema);

    // Insert the group as a non-undoable transaction so only the member
    // insert occupies the undo stack.
    ItemId group1;
    {
        auto tx = engine.beginTransaction(TransactionOptions {.undoable = false});
        group1 = tx.insert(s.groupTable, {
            ColumnValue {.column = s.groupIdCol, .value = Value(std::uint64_t {1})},
        });
        tx.commit();
    }

    // Insert a member with a unique name under group1.
    {
        auto tx = engine.beginTransaction();
        tx.insert(s.memberTable, {
            ColumnValue {.column = s.memberParent.column(), .value = idValue(group1)},
            ColumnValue {.column = s.memberName, .value = Value("x")},
            ColumnValue {.column = s.memberValue, .value = Value(std::int64_t {1})},
        });
        tx.commit();
    }

    ASSERT_TRUE(engine.canUndo());
    engine.undo();
    EXPECT_FALSE(engine.canUndo());

    // After undo the unique slot is free; re-inserting the same name succeeds.
    ItemId memberId;
    {
        auto tx = engine.beginTransaction();
        memberId = tx.insert(s.memberTable, {
            ColumnValue {.column = s.memberParent.column(), .value = idValue(group1)},
            ColumnValue {.column = s.memberName, .value = Value("x")},
            ColumnValue {.column = s.memberValue, .value = Value(std::int64_t {2})},
        });
        tx.commit();
    }

    EXPECT_TRUE(engine.contains(memberId));
    EXPECT_EQ(engine.read(memberId, s.memberName).asString(), "x");
}

} // namespace
