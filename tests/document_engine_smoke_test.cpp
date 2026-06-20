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

Value idValue(ItemId id)
{
    return Value(static_cast<std::uint64_t>(id));
}

TEST(DocumentEngineSmokeTest, DefinesSchemaAndExercisesBasicDocumentOperations)
{
    int beforeApplyHookCalls = 0;
    int afterCommitHookCalls = 0;

    SchemaBuilder builder;

    // A root table gives the Alice list a concrete association group. Lists in
    // this API are not global sequences; every non-null association value owns an
    // independent ordered list instance.
    auto documentTable = builder.createTable("Document");
    auto documentName = documentTable.addColumn(stringColumn("name"));

    auto aliceList = builder.createList("Alice");
    auto aliceDocument = aliceList.setAssociation(AssociationDefinition {
        .debugName = "document",
        .target = documentTable.handle(),
    });
    auto aliceName = aliceList.addColumn(stringColumn("name", IndexKind::Normal));

    // The hook body is intentionally small. This smoke test only verifies that
    // hooks can be registered and are expected to observe normal write traffic.
    aliceList.addHook(HookDefinition {
        .stage = HookStage::BeforeApply,
        .callback = [&beforeApplyHookCalls](TransactionContext &, const ChangeSet &) {
            ++beforeApplyHookCalls;
        },
    });

    // Bob is modeled as a table entity whose parent relation points to an Alice
    // list element. Its variant-specific fields exercise the polymorphic column
    // contract without introducing a larger domain model.
    auto bobTable = builder.createTable("Bob");
    auto bobParent = bobTable.addAssociation(AssociationDefinition {
        .debugName = "alice",
        .target = aliceList.handle(),
    });
    auto bobVariantZero = bobTable.addVariant("kind0");
    auto bobVariantOne = bobTable.addVariant("kind1");
    auto bobName = bobTable.addColumn(stringColumn("name", IndexKind::Normal));
    auto bobFoo = bobTable.addVariantColumn(VariantColumnDefinition {
        .debugName = "foo",
        .variant = bobVariantZero,
        .type = ValueType::Bool,
        .index = IndexKind::None,
        .defaultValue = Value(false),
        .nullable = false,
        .volatileData = false,
    });

    // The indexed bar column makes it legal to query by bar later in the test.
    // The check predicate expresses a simple schema-level value constraint.
    auto bobBar = bobTable.addVariantColumn(VariantColumnDefinition {
        .debugName = "bar",
        .variant = bobVariantOne,
        .type = ValueType::Int64,
        .index = IndexKind::Normal,
        .defaultValue = Value(std::int64_t {0}),
        .nullable = false,
        .volatileData = false,
        .check = [](const Value &value) {
            const auto bar = value.asInt64();
            return bar >= 0 && bar <= 10;
        },
    });
    bobTable.addHook(HookDefinition {
        .stage = HookStage::AfterCommit,
        .callback = [&afterCommitHookCalls](TransactionContext &, const ChangeSet &) {
            ++afterCommitHookCalls;
        },
    });

    auto schema = builder.freeze();
    DocumentEngine engine(schema);

    ItemId documentId = 0;
    ItemId aliceId = 0;
    ItemId alicePeerId = 0;

    {
        auto transaction = engine.beginTransaction();

        // Insert the root document first so Alice list elements can use its ID as
        // their explicit list association value.
        documentId = transaction.insert(documentTable.handle(), {
            ColumnValue {.column = documentName, .value = Value("root")},
        });

        // Exercise first-class list operations: positional insert, range rotate,
        // and positional remove inside one association group.
        aliceId = transaction.insert(aliceList.handle(), idValue(documentId), 0, {
            ColumnValue {.column = aliceName, .value = Value("Alice")},
        });
        alicePeerId = transaction.insert(aliceList.handle(), idValue(documentId), 1, {
            ColumnValue {.column = aliceName, .value = Value("Alicia")},
        });
        transaction.insert(aliceList.handle(), idValue(documentId), 2, {
            ColumnValue {.column = aliceName, .value = Value("Ally")},
        });

        const ListRotation rotation {
            .startIndex = 0,
            .count = 3,
            .offset = 1,
        };
        transaction.rotate(aliceList.handle(), idValue(documentId), rotation);
        transaction.removeAt(aliceList.handle(), idValue(documentId), 1);

        transaction.commit();
    }

    EXPECT_TRUE(engine.contains(aliceId));
    EXPECT_EQ(engine.listLength(aliceList.handle(), idValue(documentId)), 2U);

    ItemId bobKind0 = 0;
    ItemId bobKind1 = 0;

    {
        auto transaction = engine.beginTransaction();

        // Insert one Bob for each variant. The parent relation is supplied using
        // the relation's storage column, matching the ordinary column-update
        // semantics used by parent changes.
        bobKind0 = transaction.insert(bobTable.handle(), {
            ColumnValue {.column = bobParent.column(), .value = idValue(aliceId)},
            ColumnValue {.column = bobName, .value = Value("Bob Zero")},
            ColumnValue {.column = bobFoo, .value = Value(true)},
        }, bobVariantZero);

        bobKind1 = transaction.insert(bobTable.handle(), {
            ColumnValue {.column = bobParent.column(), .value = idValue(aliceId)},
            ColumnValue {.column = bobName, .value = Value("Bob One")},
            ColumnValue {.column = bobBar, .value = Value(std::int64_t {4})},
        }, bobVariantOne);

        transaction.commit();
    }

    EXPECT_TRUE(engine.contains(bobKind0));
    EXPECT_TRUE(engine.contains(bobKind1));
    EXPECT_EQ(engine.read(bobKind1, bobName).asString(), "Bob One");

    {
        auto transaction = engine.beginTransaction();

        // Cover ordinary property updates, variant-specific property updates,
        // parent detachment, parent reassignment, and table removal in one simple
        // committed transaction.
        transaction.update(bobKind1, bobName, Value("Bob Prime"));
        transaction.update(bobKind1, bobBar, Value(std::int64_t {7}));
        transaction.update(bobKind0, bobParent.column(), Value::null());
        transaction.update(bobKind0, bobParent.column(), idValue(alicePeerId));
        transaction.remove(bobKind0);

        transaction.commit();
    }

    EXPECT_FALSE(engine.contains(bobKind0));
    EXPECT_EQ(engine.read(bobKind1, bobBar).asInt64(), 7);

    // Basic ID lookup: this verifies the query API shape for a special built-in
    // query field without depending on any user-defined index.
    const QuerySpec findBobById {
        .filter = FilterExpression(Filter(
            FieldRef::id(),
            ComparisonOperator::Equal,
            idValue(bobKind1))),
    };
    auto bobById = engine.query(bobTable.handle(), findBobById).toVector();
    ASSERT_EQ(bobById.size(), 1U);
    EXPECT_EQ(bobById.front().id, bobKind1);

    // Combined query over parent relation, polymorphic variant, and indexed
    // variant-specific column. This is the minimum shape needed by the document
    // model for parent-scoped polymorphic lookups.
    const QuerySpec kindOneBobsForAlice {
        .filter = FilterExpression::all({
            FilterExpression(Filter(
                FieldRef::parent(bobParent),
                ComparisonOperator::Equal,
                idValue(aliceId))),
            FilterExpression(Filter(
                FieldRef::variant(bobVariantOne),
                ComparisonOperator::Equal,
                Value(static_cast<std::uint64_t>(bobVariantOne.variantId())))),
            FilterExpression(Filter(
                FieldRef::column(bobBar),
                ComparisonOperator::Greater,
                Value(std::int64_t {3}))),
        }),
        .sortKeys = {
            SortKey {
                .field = FieldRef::column(bobName),
                .direction = SortDirection::Ascending,
            },
        },
    };

    auto matchingBobs = engine.query(bobTable.handle(), kindOneBobsForAlice).toVector();
    ASSERT_EQ(matchingBobs.size(), 1U);
    EXPECT_EQ(matchingBobs.front().id, bobKind1);

    const QuerySpec findAliceByIndexedName {
        .filter = FilterExpression(Filter(
            FieldRef::column(aliceName),
            ComparisonOperator::Equal,
            Value("Alice"))),
    };
    auto matchingAlices = engine.query(aliceList.handle(), findAliceByIndexedName).toVector();
    ASSERT_EQ(matchingAlices.size(), 1U);
    EXPECT_EQ(matchingAlices.front().id, aliceId);

    auto bobCount = engine.view(bobTable.handle())
                        .aggregate(AggregationSpec {.kind = AggregateKind::Count})
                        .toVector();
    ASSERT_EQ(bobCount.size(), 1U);
    EXPECT_EQ(bobCount.front().value.asUInt64(), 1U);

    const auto snapshot = engine.createSnapshot();
    DocumentEngine restored(schema);
    restored.restoreSnapshot(snapshot);
    EXPECT_EQ(restored.read(bobKind1, bobName).asString(), "Bob Prime");
    EXPECT_EQ(restored.read(bobKind1, bobBar).asInt64(), 7);
    EXPECT_FALSE(restored.canUndo());
    EXPECT_FALSE(restored.canRedo());

    CommitResult replayedUpdate;
    {
        auto transaction = engine.beginTransaction();
        transaction.update(bobKind1, bobBar, Value(std::int64_t {9}));
        replayedUpdate = transaction.commit();
    }
    restored.replayCommitLog(replayedUpdate.commitLog);
    EXPECT_EQ(restored.read(bobKind1, bobBar).asInt64(), 9);

    ASSERT_TRUE(engine.canUndo());
    engine.undo();
    EXPECT_EQ(engine.read(bobKind1, bobBar).asInt64(), 7);
    ASSERT_TRUE(engine.canRedo());
    engine.redo();
    EXPECT_EQ(engine.read(bobKind1, bobBar).asInt64(), 9);

    EXPECT_GT(beforeApplyHookCalls, 0);
    EXPECT_GT(afterCommitHookCalls, 0);

    (void)aliceDocument;
}

} // namespace
