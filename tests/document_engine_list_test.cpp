#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
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

ColumnDefinition uInt64Column(const char *debugName, IndexKind index = IndexKind::None)
{
    return ColumnDefinition {
        .debugName = debugName,
        .type = ValueType::UInt64,
        .index = index,
        .defaultValue = Value(static_cast<std::uint64_t>(0)),
        .nullable = false,
    };
}

ColumnDefinition int64Column(const char *debugName, IndexKind index = IndexKind::None)
{
    return ColumnDefinition {
        .debugName = debugName,
        .type = ValueType::Int64,
        .index = index,
        .defaultValue = Value(static_cast<std::int64_t>(0)),
        .nullable = false,
    };
}

Value idValue(ItemId id)
{
    return Value(static_cast<std::uint64_t>(id));
}

ItemId insertListItem(Transaction &transaction,
                      ListHandle list,
                      Value associationValue,
                      std::size_t index,
                      std::vector<ColumnValue> values)
{
    return transaction.insert(list,
                              std::move(associationValue),
                              index,
                              {{.values = std::move(values)}}).front();
}

struct PlaylistSchema {
    EngineSchema schema;
    TableHandle songTable;
    ColumnHandle songIdCol;
    ColumnHandle songTitle;
    ListHandle playlistList;
    RelationHandle playlistAssociation;
    ColumnHandle playlistPosition;

    PlaylistSchema()
    {
        SchemaBuilder builder;

        auto table = builder.createTable("Song");
        songIdCol = table.addColumn(uInt64Column("id_col", IndexKind::Normal));
        songTitle = table.addColumn(stringColumn("title", IndexKind::Normal));
        songTable = table.handle();

        auto list = builder.createList("Playlist");
        playlistAssociation = list.setAssociation(AssociationDefinition {
            .debugName = "song",
            .target = table.handle(),
        });
        playlistPosition = list.addColumn(int64Column("position", IndexKind::Normal));
        playlistList = list.handle();

        schema = builder.freeze();
    }
};

struct SeededSongs {
    ItemId song1 = 0;
    ItemId song2 = 0;
    ItemId song3 = 0;
};

SeededSongs seedThreeSongs(DocumentEngine &engine, const PlaylistSchema &ps)
{
    auto tx = engine.beginTransaction();
    const SeededSongs ids {
        .song1 = tx.insert(ps.songTable, {
            ColumnValue {.column = ps.songIdCol, .value = Value(static_cast<std::uint64_t>(1))},
            ColumnValue {.column = ps.songTitle, .value = Value("Song One")},
        }),
        .song2 = tx.insert(ps.songTable, {
            ColumnValue {.column = ps.songIdCol, .value = Value(static_cast<std::uint64_t>(2))},
            ColumnValue {.column = ps.songTitle, .value = Value("Song Two")},
        }),
        .song3 = tx.insert(ps.songTable, {
            ColumnValue {.column = ps.songIdCol, .value = Value(static_cast<std::uint64_t>(3))},
            ColumnValue {.column = ps.songTitle, .value = Value("Song Three")},
        }),
    };
    tx.commit();
    return ids;
}

// Collects list element IDs for one association value in view-order, skipping
// items whose listAssociationValue does not match the expected association.
std::vector<ItemId> orderedIdsForAssociation(
    const View &view, const Value &associationValue)
{
    std::vector<ItemId> ids;
    view.forEach([&](const ItemSnapshot &item) {
        if (item.listAssociationValue.has_value()
            && item.listAssociationValue.value() == associationValue) {
            ids.push_back(item.id);
        }
    });
    return ids;
}

} // namespace

TEST(DocumentEngineListTest, ListInsertAtPositions)
{
    const PlaylistSchema ps;
    DocumentEngine engine(ps.schema);
    const auto songs = seedThreeSongs(engine, ps);

    // Insert at index 0 (head), at a middle index, and at index == length (tail).
    const auto &songId = songs.song1;
    const auto assocValue = idValue(songId);

    ItemId first = 0;
    ItemId second = 0;
    ItemId third = 0;
    {
        auto tx = engine.beginTransaction();
        first = insertListItem(tx, ps.playlistList, assocValue, 0, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(10))},
        });
        tx.commit();
    }
    EXPECT_EQ(engine.listLength(ps.playlistList, assocValue), 1U);

    {
        // Insert at index 0 shifts the existing element to index 1.
        auto tx = engine.beginTransaction();
        second = insertListItem(tx, ps.playlistList, assocValue, 0, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(20))},
        });
        tx.commit();
    }
    EXPECT_EQ(engine.listLength(ps.playlistList, assocValue), 2U);

    {
        // Insert at index == length appends.
        auto tx = engine.beginTransaction();
        third = insertListItem(tx, ps.playlistList, assocValue, 2, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(30))},
        });
        tx.commit();
    }
    EXPECT_EQ(engine.listLength(ps.playlistList, assocValue), 3U);

    // Verify order: second (idx 0), first (idx 1), third (idx 2).
    const auto view = engine.view(ps.playlistList);
    const auto ids = orderedIdsForAssociation(view, assocValue);
    ASSERT_EQ(ids.size(), 3U);
    EXPECT_EQ(ids[0], second);
    EXPECT_EQ(ids[1], first);
    EXPECT_EQ(ids[2], third);

    // Also verify via toVector list-index values.
    const auto items = view.toVector();
    std::size_t found = 0;
    for (const auto &item : items) {
        if (item.listAssociationValue.has_value()
            && item.listAssociationValue.value() == assocValue) {
            if (item.id == second) {
                ASSERT_TRUE(item.listIndex.has_value());
                EXPECT_EQ(item.listIndex.value(), 0U);
            } else if (item.id == first) {
                ASSERT_TRUE(item.listIndex.has_value());
                EXPECT_EQ(item.listIndex.value(), 1U);
            } else if (item.id == third) {
                ASSERT_TRUE(item.listIndex.has_value());
                EXPECT_EQ(item.listIndex.value(), 2U);
            }
            ++found;
        }
    }
    EXPECT_EQ(found, 3U);
}

TEST(DocumentEngineListTest, ListInsertAtEnd)
{
    const PlaylistSchema ps;
    DocumentEngine engine(ps.schema);
    const auto songs = seedThreeSongs(engine, ps);
    const auto assocValue = idValue(songs.song1);

    ItemId a = 0;
    ItemId b = 0;
    {
        auto tx = engine.beginTransaction();
        a = insertListItem(tx, ps.playlistList, assocValue, 0, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(100))},
        });
        tx.commit();
    }
    EXPECT_EQ(engine.listLength(ps.playlistList, assocValue), 1U);

    {
        auto tx = engine.beginTransaction();
        const auto currentLen = engine.listLength(ps.playlistList, assocValue);
        b = insertListItem(tx, ps.playlistList, assocValue, currentLen, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(200))},
        });
        tx.commit();
    }
    EXPECT_EQ(engine.listLength(ps.playlistList, assocValue), 2U);

    const auto ids = orderedIdsForAssociation(engine.view(ps.playlistList), assocValue);
    ASSERT_EQ(ids.size(), 2U);
    EXPECT_EQ(ids[0], a);
    EXPECT_EQ(ids[1], b);
}

TEST(DocumentEngineListTest, ListRemoveAt)
{
    const PlaylistSchema ps;
    DocumentEngine engine(ps.schema);
    const auto songs = seedThreeSongs(engine, ps);
    const auto assocValue = idValue(songs.song1);

    ItemId first = 0;
    ItemId middle = 0;
    ItemId last = 0;
    {
        auto tx = engine.beginTransaction();
        first = insertListItem(tx, ps.playlistList, assocValue, 0, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(10))},
        });
        middle = insertListItem(tx, ps.playlistList, assocValue, 1, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(20))},
        });
        last = insertListItem(tx, ps.playlistList, assocValue, 2, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(30))},
        });
        tx.commit();
    }
    EXPECT_EQ(engine.listLength(ps.playlistList, assocValue), 3U);

    // Remove the middle element at index 1.  Remaining items must stay contiguous.
    {
        auto tx = engine.beginTransaction();
        tx.removeAt(ps.playlistList, assocValue, 1, 1);
        tx.commit();
    }
    EXPECT_EQ(engine.listLength(ps.playlistList, assocValue), 2U);
    EXPECT_FALSE(engine.contains(middle));
    EXPECT_TRUE(engine.contains(first));
    EXPECT_TRUE(engine.contains(last));

    const auto ids = orderedIdsForAssociation(engine.view(ps.playlistList), assocValue);
    ASSERT_EQ(ids.size(), 2U);
    EXPECT_EQ(ids[0], first);
    EXPECT_EQ(ids[1], last);

    // Verify toVector listIndex values are contiguous (0, 1).
    const auto items = engine.view(ps.playlistList).toVector();
    for (const auto &item : items) {
        if (item.listAssociationValue.has_value()
            && item.listAssociationValue.value() == assocValue) {
            ASSERT_TRUE(item.listIndex.has_value());
            if (item.id == first) {
                EXPECT_EQ(item.listIndex.value(), 0U);
            } else if (item.id == last) {
                EXPECT_EQ(item.listIndex.value(), 1U);
            }
        }
    }
}

TEST(DocumentEngineListTest, ListBatchInsertAndRemovePreserveOrderAndUndo)
{
    const PlaylistSchema ps;
    DocumentEngine engine(ps.schema);
    const auto songs = seedThreeSongs(engine, ps);
    const auto assocValue = idValue(songs.song1);

    std::vector<ItemId> outer;
    {
        auto tx = engine.beginTransaction();
        outer = tx.insert(ps.playlistList,
                          assocValue,
                          0,
                          {
                              {.values = {
                                   {.column = ps.playlistPosition, .value = Value(std::int64_t {10})},
                               }},
                              {.values = {
                                   {.column = ps.playlistPosition, .value = Value(std::int64_t {40})},
                               }},
                          });
        tx.commit();
    }

    std::vector<ItemId> middle;
    CommitResult insertResult;
    {
        auto tx = engine.beginTransaction();
        middle = tx.insert(ps.playlistList,
                           assocValue,
                           1,
                           {
                               {.values = {
                                    {.column = ps.playlistPosition, .value = Value(std::int64_t {20})},
                                }},
                               {.values = {
                                    {.column = ps.playlistPosition, .value = Value(std::int64_t {30})},
                                }},
                           });
        insertResult = tx.commit();
    }

    ASSERT_EQ(outer.size(), 2U);
    ASSERT_EQ(middle.size(), 2U);
    EXPECT_EQ(orderedIdsForAssociation(engine.view(ps.playlistList), assocValue),
              (std::vector<ItemId> {outer[0], middle[0], middle[1], outer[1]}));
    EXPECT_EQ(std::count_if(insertResult.changeSet.operations().begin(),
                            insertResult.changeSet.operations().end(),
                            [](const ChangeOperation &operation) {
                                return operation.kind() == ChangeOperationKind::ListInserted;
                            }),
              2);

    CommitResult removeResult;
    {
        auto tx = engine.beginTransaction();
        tx.removeAt(ps.playlistList, assocValue, 1, 2);
        removeResult = tx.commit();
    }
    EXPECT_EQ(orderedIdsForAssociation(engine.view(ps.playlistList), assocValue), outer);
    ASSERT_EQ(removeResult.changeSet.operations().size(), 2U);
    EXPECT_EQ(std::get<ListRemovedChange>(removeResult.changeSet.operations()[0].payload()).index, 2U);
    EXPECT_EQ(std::get<ListRemovedChange>(removeResult.changeSet.operations()[1].payload()).index, 1U);

    ASSERT_TRUE(engine.canUndo());
    engine.undo();
    EXPECT_EQ(orderedIdsForAssociation(engine.view(ps.playlistList), assocValue),
              (std::vector<ItemId> {outer[0], middle[0], middle[1], outer[1]}));
}

TEST(DocumentEngineListTest, ListRemoveById)
{
    const PlaylistSchema ps;
    DocumentEngine engine(ps.schema);
    const auto songs = seedThreeSongs(engine, ps);
    const auto assocValue = idValue(songs.song1);

    ItemId a = 0;
    ItemId b = 0;
    {
        auto tx = engine.beginTransaction();
        a = insertListItem(tx, ps.playlistList, assocValue, 0, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(10))},
        });
        b = insertListItem(tx, ps.playlistList, assocValue, 1, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(20))},
        });
        tx.commit();
    }
    EXPECT_EQ(engine.listLength(ps.playlistList, assocValue), 2U);

    {
        auto tx = engine.beginTransaction();
        tx.remove(b);
        tx.commit();
    }
    EXPECT_FALSE(engine.contains(b));
    EXPECT_EQ(engine.listLength(ps.playlistList, assocValue), 1U);

    const auto ids = orderedIdsForAssociation(engine.view(ps.playlistList), assocValue);
    ASSERT_EQ(ids.size(), 1U);
    EXPECT_EQ(ids[0], a);
}

TEST(DocumentEngineListTest, ListRotateForward)
{
    const PlaylistSchema ps;
    DocumentEngine engine(ps.schema);
    const auto songs = seedThreeSongs(engine, ps);
    const auto assocValue = idValue(songs.song1);

    std::vector<ItemId> elements(5);
    {
        auto tx = engine.beginTransaction();
        for (std::size_t i = 0; i < 5; ++i) {
            elements[i] = insertListItem(tx, ps.playlistList, assocValue, i, {
                ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(static_cast<std::int64_t>(i) * 10))},
            });
        }
        tx.commit();
    }
    EXPECT_EQ(engine.listLength(ps.playlistList, assocValue), 5U);

    // Rotate forward: positive offset 2 moves first 2 elements to the end.
    // Expected order: [2, 3, 4, 0, 1].
    {
        auto tx = engine.beginTransaction();
        const ListRotation rotation {.startIndex = 0, .count = 5, .offset = 2};
        tx.rotate(ps.playlistList, assocValue, rotation);
        tx.commit();
    }

    const auto ids = orderedIdsForAssociation(engine.view(ps.playlistList), assocValue);
    ASSERT_EQ(ids.size(), 5U);
    EXPECT_EQ(ids[0], elements[2]);
    EXPECT_EQ(ids[1], elements[3]);
    EXPECT_EQ(ids[2], elements[4]);
    EXPECT_EQ(ids[3], elements[0]);
    EXPECT_EQ(ids[4], elements[1]);
}

TEST(DocumentEngineListTest, ListRotateBackward)
{
    const PlaylistSchema ps;
    DocumentEngine engine(ps.schema);
    const auto songs = seedThreeSongs(engine, ps);
    const auto assocValue = idValue(songs.song1);

    std::vector<ItemId> elements(5);
    {
        auto tx = engine.beginTransaction();
        for (std::size_t i = 0; i < 5; ++i) {
            elements[i] = insertListItem(tx, ps.playlistList, assocValue, i, {
                ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(static_cast<std::int64_t>(i) * 10))},
            });
        }
        tx.commit();
    }

    // Rotate backward: offset -1 → normalized (5 - 1) = 4.
    // Expected order: [4, 0, 1, 2, 3].
    {
        auto tx = engine.beginTransaction();
        const ListRotation rotation {.startIndex = 0, .count = 5, .offset = -1};
        tx.rotate(ps.playlistList, assocValue, rotation);
        tx.commit();
    }

    const auto ids = orderedIdsForAssociation(engine.view(ps.playlistList), assocValue);
    ASSERT_EQ(ids.size(), 5U);
    EXPECT_EQ(ids[0], elements[4]);
    EXPECT_EQ(ids[1], elements[0]);
    EXPECT_EQ(ids[2], elements[1]);
    EXPECT_EQ(ids[3], elements[2]);
    EXPECT_EQ(ids[4], elements[3]);
}

TEST(DocumentEngineListTest, ListRotateSingleElement)
{
    const PlaylistSchema ps;
    DocumentEngine engine(ps.schema);
    const auto songs = seedThreeSongs(engine, ps);
    const auto assocValue = idValue(songs.song1);

    ItemId solo = 0;
    {
        auto tx = engine.beginTransaction();
        solo = insertListItem(tx, ps.playlistList, assocValue, 0, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(42))},
        });
        tx.commit();
    }

    // count=1: offset modulo 1 is 0, no effect.
    {
        auto tx = engine.beginTransaction();
        const ListRotation rotation {.startIndex = 0, .count = 1, .offset = 7};
        tx.rotate(ps.playlistList, assocValue, rotation);
        tx.commit();
    }

    const auto ids = orderedIdsForAssociation(engine.view(ps.playlistList), assocValue);
    ASSERT_EQ(ids.size(), 1U);
    EXPECT_EQ(ids[0], solo);
}

TEST(DocumentEngineListTest, ListRotateFullRange)
{
    const PlaylistSchema ps;
    DocumentEngine engine(ps.schema);
    const auto songs = seedThreeSongs(engine, ps);
    const auto assocValue = idValue(songs.song1);

    std::vector<ItemId> elements(4);
    {
        auto tx = engine.beginTransaction();
        for (std::size_t i = 0; i < 4; ++i) {
            elements[i] = insertListItem(tx, ps.playlistList, assocValue, i, {
                ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(static_cast<std::int64_t>(i) * 10))},
            });
        }
        tx.commit();
    }

    // offset == count → normalized to 0, order unchanged.
    {
        auto tx = engine.beginTransaction();
        const ListRotation rotation {.startIndex = 0, .count = 4, .offset = 4};
        tx.rotate(ps.playlistList, assocValue, rotation);
        tx.commit();
    }

    const auto ids = orderedIdsForAssociation(engine.view(ps.playlistList), assocValue);
    ASSERT_EQ(ids.size(), 4U);
    EXPECT_EQ(ids[0], elements[0]);
    EXPECT_EQ(ids[1], elements[1]);
    EXPECT_EQ(ids[2], elements[2]);
    EXPECT_EQ(ids[3], elements[3]);
}

TEST(DocumentEngineListTest, ListMultipleInstances)
{
    const PlaylistSchema ps;
    DocumentEngine engine(ps.schema);
    const auto songs = seedThreeSongs(engine, ps);

    const auto assoc1 = idValue(songs.song1);
    const auto assoc2 = idValue(songs.song2);

    ItemId a1 = 0;
    ItemId a2 = 0;
    ItemId b1 = 0;
    {
        auto tx = engine.beginTransaction();
        a1 = insertListItem(tx, ps.playlistList, assoc1, 0, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(10))},
        });
        a2 = insertListItem(tx, ps.playlistList, assoc1, 1, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(20))},
        });
        b1 = insertListItem(tx, ps.playlistList, assoc2, 0, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(100))},
        });
        tx.commit();
    }

    EXPECT_EQ(engine.listLength(ps.playlistList, assoc1), 2U);
    EXPECT_EQ(engine.listLength(ps.playlistList, assoc2), 1U);

    // Remove from association 1 – association 2 must remain unchanged.
    {
        auto tx = engine.beginTransaction();
        tx.removeAt(ps.playlistList, assoc1, 0, 1);
        tx.commit();
    }

    EXPECT_EQ(engine.listLength(ps.playlistList, assoc1), 1U);
    EXPECT_EQ(engine.listLength(ps.playlistList, assoc2), 1U);
    EXPECT_FALSE(engine.contains(a1));
    EXPECT_TRUE(engine.contains(b1));

    const auto ids1 = orderedIdsForAssociation(engine.view(ps.playlistList), assoc1);
    ASSERT_EQ(ids1.size(), 1U);
    EXPECT_EQ(ids1[0], a2);

    const auto ids2 = orderedIdsForAssociation(engine.view(ps.playlistList), assoc2);
    ASSERT_EQ(ids2.size(), 1U);
    EXPECT_EQ(ids2[0], b1);
}

TEST(DocumentEngineListTest, ListInsertEmptyInstance)
{
    const PlaylistSchema ps;
    DocumentEngine engine(ps.schema);
    const auto songs = seedThreeSongs(engine, ps);

    // song3 has never been used as a list association — the instance is empty.
    const auto assocValue = idValue(songs.song3);
    EXPECT_EQ(engine.listLength(ps.playlistList, assocValue), 0U);

    ItemId element = 0;
    {
        auto tx = engine.beginTransaction();
        element = insertListItem(tx, ps.playlistList, assocValue, 0, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(77))},
        });
        tx.commit();
    }

    EXPECT_EQ(engine.listLength(ps.playlistList, assocValue), 1U);
    EXPECT_TRUE(engine.contains(element));

    const auto ids = orderedIdsForAssociation(engine.view(ps.playlistList), assocValue);
    ASSERT_EQ(ids.size(), 1U);
    EXPECT_EQ(ids[0], element);
}

TEST(DocumentEngineListTest, ListClearThroughRepeatedRemove)
{
    const PlaylistSchema ps;
    DocumentEngine engine(ps.schema);
    const auto songs = seedThreeSongs(engine, ps);
    const auto assocValue = idValue(songs.song1);

    {
        auto tx = engine.beginTransaction();
        for (std::size_t i = 0; i < 5; ++i) {
            insertListItem(tx, ps.playlistList, assocValue, i, {
                ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(static_cast<std::int64_t>(i) * 10))},
            });
        }
        tx.commit();
    }
    EXPECT_EQ(engine.listLength(ps.playlistList, assocValue), 5U);

    // Repeated removeAt(0) until empty.
    {
        auto tx = engine.beginTransaction();
        std::size_t remaining = engine.listLength(ps.playlistList, assocValue);
        while (remaining > 0) {
            tx.removeAt(ps.playlistList, assocValue, 0, 1);
            --remaining;
        }
        tx.commit();
    }

    EXPECT_EQ(engine.listLength(ps.playlistList, assocValue), 0U);
    const auto ids = orderedIdsForAssociation(engine.view(ps.playlistList), assocValue);
    EXPECT_TRUE(ids.empty());
}

TEST(DocumentEngineListTest, ListUndoInsertRestoresIndex)
{
    const PlaylistSchema ps;
    DocumentEngine engine(ps.schema);
    const auto songs = seedThreeSongs(engine, ps);
    const auto assocValue = idValue(songs.song1);

    // Establish a baseline of 2 elements.
    {
        auto tx = engine.beginTransaction();
        insertListItem(tx, ps.playlistList, assocValue, 0, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(10))},
        });
        insertListItem(tx, ps.playlistList, assocValue, 1, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(20))},
        });
        tx.commit();
    }
    EXPECT_EQ(engine.listLength(ps.playlistList, assocValue), 2U);
    const auto beforeIds = orderedIdsForAssociation(engine.view(ps.playlistList), assocValue);
    ASSERT_EQ(beforeIds.size(), 2U);

    // Insert one more element.
    {
        auto tx = engine.beginTransaction();
        insertListItem(tx, ps.playlistList, assocValue, 1, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(15))},
        });
        tx.commit();
    }
    EXPECT_EQ(engine.listLength(ps.playlistList, assocValue), 3U);

    ASSERT_TRUE(engine.canUndo());
    engine.undo();

    EXPECT_EQ(engine.listLength(ps.playlistList, assocValue), 2U);
    const auto afterIds = orderedIdsForAssociation(engine.view(ps.playlistList), assocValue);
    ASSERT_EQ(afterIds.size(), 2U);
    EXPECT_EQ(afterIds[0], beforeIds[0]);
    EXPECT_EQ(afterIds[1], beforeIds[1]);
}

TEST(DocumentEngineListTest, ListUndoRemoveRestoresItemAndIndex)
{
    const PlaylistSchema ps;
    DocumentEngine engine(ps.schema);
    const auto songs = seedThreeSongs(engine, ps);
    const auto assocValue = idValue(songs.song1);

    ItemId a = 0;
    ItemId b = 0;
    ItemId c = 0;
    {
        auto tx = engine.beginTransaction();
        a = insertListItem(tx, ps.playlistList, assocValue, 0, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(10))},
        });
        b = insertListItem(tx, ps.playlistList, assocValue, 1, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(20))},
        });
        c = insertListItem(tx, ps.playlistList, assocValue, 2, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(30))},
        });
        tx.commit();
    }

    // Remove the middle element.
    {
        auto tx = engine.beginTransaction();
        tx.removeAt(ps.playlistList, assocValue, 1, 1);
        tx.commit();
    }
    EXPECT_EQ(engine.listLength(ps.playlistList, assocValue), 2U);
    EXPECT_FALSE(engine.contains(b));

    ASSERT_TRUE(engine.canUndo());
    engine.undo();

    // Item b is restored with its original ID and index 1.
    EXPECT_EQ(engine.listLength(ps.playlistList, assocValue), 3U);
    EXPECT_TRUE(engine.contains(b));

    const auto ids = orderedIdsForAssociation(engine.view(ps.playlistList), assocValue);
    ASSERT_EQ(ids.size(), 3U);
    EXPECT_EQ(ids[0], a);
    EXPECT_EQ(ids[1], b);
    EXPECT_EQ(ids[2], c);
}

TEST(DocumentEngineListTest, ListUndoRotate)
{
    const PlaylistSchema ps;
    DocumentEngine engine(ps.schema);
    const auto songs = seedThreeSongs(engine, ps);
    const auto assocValue = idValue(songs.song1);

    std::vector<ItemId> original(5);
    {
        auto tx = engine.beginTransaction();
        for (std::size_t i = 0; i < 5; ++i) {
            original[i] = insertListItem(tx, ps.playlistList, assocValue, i, {
                ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(static_cast<std::int64_t>(i) * 10))},
            });
        }
        tx.commit();
    }

    {
        auto tx = engine.beginTransaction();
        const ListRotation rotation {.startIndex = 0, .count = 5, .offset = 2};
        tx.rotate(ps.playlistList, assocValue, rotation);
        tx.commit();
    }

    // Verify the rotate actually changed order.
    {
        const auto rotatedIds = orderedIdsForAssociation(engine.view(ps.playlistList), assocValue);
        ASSERT_EQ(rotatedIds.size(), 5U);
        EXPECT_EQ(rotatedIds[0], original[2]);
    }

    ASSERT_TRUE(engine.canUndo());
    engine.undo();

    // Undo restores original order.
    const auto restoredIds = orderedIdsForAssociation(engine.view(ps.playlistList), assocValue);
    ASSERT_EQ(restoredIds.size(), 5U);
    for (std::size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(restoredIds[i], original[i]);
    }
}

TEST(DocumentEngineListTest, ListMoveViaAssociationUpdate)
{
    const PlaylistSchema ps;
    DocumentEngine engine(ps.schema);
    const auto songs = seedThreeSongs(engine, ps);

    const auto assoc1 = idValue(songs.song1);
    const auto assoc2 = idValue(songs.song2);

    ItemId element = 0;
    {
        auto tx = engine.beginTransaction();
        element = insertListItem(tx, ps.playlistList, assoc1, 0, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(10))},
        });
        tx.commit();
    }
    EXPECT_EQ(engine.listLength(ps.playlistList, assoc1), 1U);
    EXPECT_EQ(engine.listLength(ps.playlistList, assoc2), 0U);

    // Move the element to song2's list via association column update.
    {
        auto tx = engine.beginTransaction();
        tx.update(element, ps.playlistAssociation.column(), assoc2,
                  AssociationUpdateOptions {.targetIndex = 0});
        tx.commit();
    }

    EXPECT_EQ(engine.listLength(ps.playlistList, assoc1), 0U);
    EXPECT_EQ(engine.listLength(ps.playlistList, assoc2), 1U);

    const auto ids2 = orderedIdsForAssociation(engine.view(ps.playlistList), assoc2);
    ASSERT_EQ(ids2.size(), 1U);
    EXPECT_EQ(ids2[0], element);
}

TEST(DocumentEngineListTest, ListInsertWithNullAssociationCreatesUnattachedItem)
{
    const PlaylistSchema ps;
    DocumentEngine engine(ps.schema);
    const auto songs = seedThreeSongs(engine, ps);
    const auto assocValue = idValue(songs.song1);

    ItemId element = 0;
    CommitResult result;
    {
        auto tx = engine.beginTransaction();
        element = insertListItem(tx, ps.playlistList, Value::null(), 0, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(10))},
        });
        result = tx.commit();
    }

    EXPECT_TRUE(engine.contains(element));
    EXPECT_EQ(engine.listLength(ps.playlistList, assocValue), 0U);
    EXPECT_TRUE(orderedIdsForAssociation(engine.view(ps.playlistList), assocValue).empty());

    const auto snapshot = engine.read(element);
    EXPECT_EQ(snapshot.containerKind, ContainerKind::List);
    EXPECT_FALSE(snapshot.parentId.has_value());
    EXPECT_FALSE(snapshot.listAssociationValue.has_value());
    EXPECT_FALSE(snapshot.listIndex.has_value());
    EXPECT_TRUE(engine.read(element, ps.playlistAssociation.column()).isNull());

    const auto &ops = result.changeSet.operations();
    bool foundItemInserted = false;
    bool foundListInserted = false;
    for (const auto &op : ops) {
        foundItemInserted = foundItemInserted || op.kind() == ChangeOperationKind::ItemInserted;
        foundListInserted = foundListInserted || op.kind() == ChangeOperationKind::ListInserted;
    }
    EXPECT_TRUE(foundItemInserted);
    EXPECT_FALSE(foundListInserted);
}

TEST(DocumentEngineListTest, ListNullAssociationInsertRequiresZeroIndex)
{
    const PlaylistSchema ps;
    DocumentEngine engine(ps.schema);
    seedThreeSongs(engine, ps);

    auto tx = engine.beginTransaction();
    EXPECT_THROW(
        insertListItem(tx, ps.playlistList, Value::null(), 1, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(10))},
        }),
        ConstraintError);
}

TEST(DocumentEngineListTest, ListAttachAndDetachUnattachedItem)
{
    const PlaylistSchema ps;
    DocumentEngine engine(ps.schema);
    const auto songs = seedThreeSongs(engine, ps);
    const auto assocValue = idValue(songs.song1);

    ItemId element = 0;
    {
        auto tx = engine.beginTransaction();
        element = insertListItem(tx, ps.playlistList, Value::null(), 0, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(10))},
        });
        tx.commit();
    }
    EXPECT_EQ(engine.listLength(ps.playlistList, assocValue), 0U);

    {
        auto tx = engine.beginTransaction();
        tx.update(element, ps.playlistAssociation.column(), assocValue,
                  AssociationUpdateOptions {.targetIndex = 0});
        tx.commit();
    }
    EXPECT_EQ(engine.listLength(ps.playlistList, assocValue), 1U);
    {
        const auto ids = orderedIdsForAssociation(engine.view(ps.playlistList), assocValue);
        ASSERT_EQ(ids.size(), 1U);
        EXPECT_EQ(ids[0], element);
    }

    {
        auto tx = engine.beginTransaction();
        tx.update(element, ps.playlistAssociation.column(), Value::null());
        tx.commit();
    }
    EXPECT_EQ(engine.listLength(ps.playlistList, assocValue), 0U);
    EXPECT_TRUE(engine.read(element, ps.playlistAssociation.column()).isNull());
    const auto snapshot = engine.read(element);
    EXPECT_FALSE(snapshot.listAssociationValue.has_value());
    EXPECT_FALSE(snapshot.listIndex.has_value());
}

TEST(DocumentEngineListTest, ListUndoRedoNullAssociationInsert)
{
    const PlaylistSchema ps;
    DocumentEngine engine(ps.schema);
    seedThreeSongs(engine, ps);

    ItemId element = 0;
    {
        auto tx = engine.beginTransaction();
        element = insertListItem(tx, ps.playlistList, Value::null(), 0, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(10))},
        });
        tx.commit();
    }
    EXPECT_TRUE(engine.contains(element));

    ASSERT_TRUE(engine.canUndo());
    engine.undo();
    EXPECT_FALSE(engine.contains(element));

    ASSERT_TRUE(engine.canRedo());
    engine.redo();
    EXPECT_TRUE(engine.contains(element));
    EXPECT_TRUE(engine.read(element, ps.playlistAssociation.column()).isNull());
    EXPECT_FALSE(engine.read(element).listAssociationValue.has_value());
}

TEST(DocumentEngineListTest, ListNullAssociationSurvivesSnapshotAndChangeSetReplay)
{
    const PlaylistSchema ps;
    DocumentEngine engine(ps.schema);

    ItemId element = 0;
    CommitResult result;
    {
        auto tx = engine.beginTransaction();
        element = insertListItem(tx, ps.playlistList, Value::null(), 0, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(10))},
        });
        result = tx.commit();
    }

    const auto snapshot = engine.createSnapshot();
    DocumentEngine restored(ps.schema);
    restored.restoreSnapshot(snapshot);
    EXPECT_TRUE(restored.contains(element));
    EXPECT_TRUE(restored.read(element, ps.playlistAssociation.column()).isNull());
    EXPECT_FALSE(restored.read(element).listAssociationValue.has_value());

    DocumentEngine replayed(ps.schema);
    replayed.replayChangeSet(ChangeSet::deserialize(result.changeSet.serialize(), replayed.schema()));
    EXPECT_TRUE(replayed.contains(element));
    EXPECT_TRUE(replayed.read(element, ps.playlistAssociation.column()).isNull());
    EXPECT_FALSE(replayed.read(element).listAssociationValue.has_value());
}

TEST(DocumentEngineListTest, ListInsertChangesetKind)
{
    const PlaylistSchema ps;
    DocumentEngine engine(ps.schema);
    const auto songs = seedThreeSongs(engine, ps);
    const auto assocValue = idValue(songs.song1);

    CommitResult result;
    {
        auto tx = engine.beginTransaction();
        insertListItem(tx, ps.playlistList, assocValue, 0, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(10))},
        });
        result = tx.commit();
    }

    const auto &ops = result.changeSet.operations();
    bool foundListInserted = false;
    bool foundItemInserted = false;
    for (const auto &op : ops) {
        if (op.kind() == ChangeOperationKind::ListInserted) {
            foundListInserted = true;
        }
        if (op.kind() == ChangeOperationKind::ItemInserted) {
            foundItemInserted = true;
        }
    }
    EXPECT_TRUE(foundListInserted);
    EXPECT_FALSE(foundItemInserted);
}

TEST(DocumentEngineListTest, ListRemoveAtChangesetKind)
{
    const PlaylistSchema ps;
    DocumentEngine engine(ps.schema);
    const auto songs = seedThreeSongs(engine, ps);
    const auto assocValue = idValue(songs.song1);

    // Insert first so there is something to remove.
    {
        auto tx = engine.beginTransaction();
        insertListItem(tx, ps.playlistList, assocValue, 0, {
            ColumnValue {.column = ps.playlistPosition, .value = Value(static_cast<std::int64_t>(10))},
        });
        tx.commit();
    }

    CommitResult result;
    {
        auto tx = engine.beginTransaction();
        tx.removeAt(ps.playlistList, assocValue, 0, 1);
        result = tx.commit();
    }

    const auto &ops = result.changeSet.operations();
    bool foundListRemoved = false;
    for (const auto &op : ops) {
        if (op.kind() == ChangeOperationKind::ListRemoved) {
            foundListRemoved = true;
        }
    }
    EXPECT_TRUE(foundListRemoved);
}

TEST(DocumentEngineListTest, ListChangesSurviveSerializationAndReplay)
{
    const PlaylistSchema ps;
    DocumentEngine engine(ps.schema);
    const auto songs = seedThreeSongs(engine, ps);
    const auto association = idValue(songs.song1);
    DocumentEngine replayed(ps.schema);
    replayed.restoreSnapshot(engine.createSnapshot());

    auto insert = engine.beginTransaction();
    const auto first = insertListItem(insert, ps.playlistList, association, 0, {
        ColumnValue {.column = ps.playlistPosition, .value = Value(std::int64_t {10})},
    });
    const auto second = insertListItem(insert, ps.playlistList, association, 1, {
        ColumnValue {.column = ps.playlistPosition, .value = Value(std::int64_t {20})},
    });
    const auto inserted = ChangeSet::deserialize(insert.commit().changeSet.serialize(), replayed.schema());
    ASSERT_EQ(inserted.operations().size(), 2U);
    const auto &insertion = std::get<ListInsertedChange>(inserted.operations().back().payload());
    EXPECT_EQ(insertion.list, ps.playlistList);
    EXPECT_EQ(insertion.associationValue, association);
    EXPECT_EQ(insertion.index, 1U);
    EXPECT_EQ(insertion.item.parentId, songs.song1);
    replayed.replayChangeSet(inserted);
    EXPECT_EQ(orderedIdsForAssociation(replayed.view(ps.playlistList), association), (std::vector<ItemId> {first, second}));

    auto rotate = engine.beginTransaction();
    rotate.rotate(ps.playlistList, association, ListRotation {.startIndex = 0, .count = 2, .offset = 1});
    replayed.replayChangeSet(ChangeSet::deserialize(rotate.commit().changeSet.serialize(), replayed.schema()));
    EXPECT_EQ(orderedIdsForAssociation(replayed.view(ps.playlistList), association), (std::vector<ItemId> {second, first}));

    auto remove = engine.beginTransaction();
    remove.removeAt(ps.playlistList, association, 1, 1);
    const auto removed = ChangeSet::deserialize(remove.commit().changeSet.serialize(), replayed.schema());
    ASSERT_EQ(removed.operations().size(), 1U);
    const auto &removal = std::get<ListRemovedChange>(removed.operations().front().payload());
    EXPECT_EQ(removal.list, ps.playlistList);
    EXPECT_EQ(removal.associationValue, association);
    EXPECT_EQ(removal.index, 1U);
    EXPECT_EQ(removal.item.id, first);
    replayed.replayChangeSet(removed);
    EXPECT_EQ(orderedIdsForAssociation(replayed.view(ps.playlistList), association), (std::vector<ItemId> {second}));

    replayed.replayChangeSet(removed.invert());
    EXPECT_EQ(orderedIdsForAssociation(replayed.view(ps.playlistList), association), (std::vector<ItemId> {second, first}));
}
