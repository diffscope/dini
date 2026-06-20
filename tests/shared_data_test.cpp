#include <utility>

#include <dini/support/shareddata.h>

#include <gtest/gtest.h>

// ---------------------------------------------------------------------------
// Test payload types
// ---------------------------------------------------------------------------

// Simple data that derives from SharedData
struct TestData : public dini::SharedData {
    int value;
    explicit TestData(int v = 0) : value(v) {}
};

// Data with destructor tracking for lifetime tests
struct TrackedData : public dini::SharedData {
    static int alive_count;

    TrackedData() { ++alive_count; }
    TrackedData(const TrackedData &other) : SharedData(other) { ++alive_count; }
    ~TrackedData() override { --alive_count; }
};

int TrackedData::alive_count = 0;

// Derived type used by get_impl_helper tests
struct DerivedData : public TestData {
    int extra;
    DerivedData(int v, int e) : TestData(v), extra(e) {}
};

// ===========================================================================
// SharedData tests
// ===========================================================================

TEST(SharedDataTest, DefaultConstructRefCountIsOne) {
    dini::SharedData sd;
    EXPECT_EQ(sd.refCount(), 1u);
}

TEST(SharedDataTest, CopyConstructRefCountIsOne) {
    dini::SharedData sd;
    dini::SharedData sd2(sd);
    EXPECT_EQ(sd.refCount(), 1u);
    EXPECT_EQ(sd2.refCount(), 1u);
}

TEST(SharedDataTest, AssignmentDoesNotChangeRefCount) {
    dini::SharedData sd;
    dini::SharedData sd2;
    sd2 = sd;
    EXPECT_EQ(sd.refCount(), 1u);
    EXPECT_EQ(sd2.refCount(), 1u);
}

// ===========================================================================
// SharedDataPointer tests
// ===========================================================================

// --- Construction ----------------------------------------------------------

TEST(SharedDataTest, DefaultConstructorIsNull) {
    dini::SharedDataPointer<TestData> ptr;
    EXPECT_TRUE(ptr.isNull());
    EXPECT_FALSE(ptr);
    EXPECT_EQ(ptr.constData(), nullptr);
}

TEST(SharedDataTest, RawPointerConstructorTakesOwnership) {
    auto *raw = new TestData(42);
    dini::SharedDataPointer<TestData> ptr(raw);
    EXPECT_FALSE(ptr.isNull());
    EXPECT_TRUE(ptr);
    EXPECT_EQ(ptr.constData(), raw);
    EXPECT_EQ(raw->refCount(), 1u);
}

TEST(SharedDataTest, NullptrConstructionIsNull) {
    dini::SharedDataPointer<TestData> ptr(nullptr);
    EXPECT_TRUE(ptr.isNull());
}

// --- Copy ------------------------------------------------------------------

TEST(SharedDataTest, CopyConstructorSharesData) {
    auto *raw = new TestData(42);
    dini::SharedDataPointer<TestData> ptr1(raw);
    dini::SharedDataPointer<TestData> ptr2(ptr1);

    EXPECT_EQ(ptr1.constData(), ptr2.constData());
    EXPECT_EQ(raw->refCount(), 2u);
}

TEST(SharedDataTest, CopyConstructorFromNull) {
    dini::SharedDataPointer<TestData> null_ptr;
    dini::SharedDataPointer<TestData> ptr2(null_ptr);
    EXPECT_TRUE(ptr2.isNull());
}

TEST(SharedDataTest, CopyAssignmentSharesData) {
    auto *raw = new TestData(42);
    dini::SharedDataPointer<TestData> ptr1(raw);
    dini::SharedDataPointer<TestData> ptr2;
    ptr2 = ptr1;

    EXPECT_EQ(ptr1.constData(), ptr2.constData());
    EXPECT_EQ(raw->refCount(), 2u);
}

TEST(SharedDataTest, CopyAssignmentSelfAssign) {
    auto *raw = new TestData(42);
    dini::SharedDataPointer<TestData> ptr1(raw);
    ptr1 = ptr1;   // self-assignment
    EXPECT_EQ(raw->refCount(), 1u);
}

TEST(SharedDataTest, CopyAssignmentReleasesOldResource) {
    TrackedData::alive_count = 0;
    {
        dini::SharedDataPointer<TrackedData> ptr1(new TrackedData());
        EXPECT_EQ(TrackedData::alive_count, 1);

        dini::SharedDataPointer<TrackedData> ptr2(new TrackedData());
        EXPECT_EQ(TrackedData::alive_count, 2);

        ptr2 = ptr1;   // old ptr2 data should be destroyed
        EXPECT_EQ(TrackedData::alive_count, 1);
        EXPECT_EQ(ptr1.constData(), ptr2.constData());
    }
    EXPECT_EQ(TrackedData::alive_count, 0);
}

// --- Move ------------------------------------------------------------------

TEST(SharedDataTest, MoveConstructorTransfersOwnership) {
    auto *raw = new TestData(42);
    dini::SharedDataPointer<TestData> ptr1(raw);
    dini::SharedDataPointer<TestData> ptr2(std::move(ptr1));

    EXPECT_TRUE(ptr1.isNull());
    EXPECT_FALSE(ptr2.isNull());
    EXPECT_EQ(ptr2.constData(), raw);
    EXPECT_EQ(raw->refCount(), 1u);
}

TEST(SharedDataTest, MoveAssignmentTransfersOwnership) {
    auto *raw = new TestData(42);
    dini::SharedDataPointer<TestData> ptr1(raw);
    dini::SharedDataPointer<TestData> ptr2;
    ptr2 = std::move(ptr1);

    EXPECT_TRUE(ptr1.isNull());
    EXPECT_FALSE(ptr2.isNull());
    EXPECT_EQ(raw->refCount(), 1u);
}

TEST(SharedDataTest, MoveAssignmentSelfAssign) {
    auto *raw = new TestData(42);
    dini::SharedDataPointer<TestData> ptr1(raw);
    ptr1 = std::move(ptr1);   // self-move
    EXPECT_FALSE(ptr1.isNull());
    EXPECT_EQ(raw->refCount(), 1u);
}

TEST(SharedDataTest, MoveAssignmentReleasesOldResource) {
    TrackedData::alive_count = 0;
    {
        dini::SharedDataPointer<TrackedData> ptr1(new TrackedData());  // count = 1
        dini::SharedDataPointer<TrackedData> ptr2(new TrackedData());  // count = 2
        ptr2 = std::move(ptr1);   // ptr2's old data is destroyed
        EXPECT_EQ(TrackedData::alive_count, 1);
        EXPECT_TRUE(ptr1.isNull());
    }
    EXPECT_EQ(TrackedData::alive_count, 0);
}

// --- Const access (no detach) ----------------------------------------------

TEST(SharedDataTest, ConstDataDoesNotDetach) {
    auto *raw = new TestData(42);
    dini::SharedDataPointer<TestData> ptr1(raw);
    dini::SharedDataPointer<TestData> ptr2(ptr1);
    EXPECT_EQ(raw->refCount(), 2u);

    EXPECT_EQ(ptr1.constData()->value, 42);
    EXPECT_EQ(ptr2.constData()->value, 42);
    EXPECT_EQ(raw->refCount(), 2u);   // unchanged
}

TEST(SharedDataTest, ConstArrowDoesNotDetach) {
    auto *raw = new TestData(42);
    dini::SharedDataPointer<TestData> ptr1(raw);
    dini::SharedDataPointer<TestData> ptr2(ptr1);
    EXPECT_EQ(raw->refCount(), 2u);

    const dini::SharedDataPointer<TestData> &cptr = ptr1;
    EXPECT_EQ(cptr->value, 42);
    EXPECT_EQ(raw->refCount(), 2u);
}

TEST(SharedDataTest, ConstStarDoesNotDetach) {
    auto *raw = new TestData(42);
    dini::SharedDataPointer<TestData> ptr1(raw);
    dini::SharedDataPointer<TestData> ptr2(ptr1);
    EXPECT_EQ(raw->refCount(), 2u);

    const dini::SharedDataPointer<TestData> &cptr = ptr1;
    EXPECT_EQ((*cptr).value, 42);
    EXPECT_EQ(raw->refCount(), 2u);
}

// --- Mutable access (detach when shared) -----------------------------------

TEST(SharedDataTest, MutableDataDetachesWhenShared) {
    auto *raw = new TestData(42);
    dini::SharedDataPointer<TestData> ptr1(raw);
    dini::SharedDataPointer<TestData> ptr2(ptr1);
    EXPECT_EQ(raw->refCount(), 2u);

    TestData *detached = ptr1.data();
    EXPECT_NE(detached, raw);             // new block
    EXPECT_EQ(detached->refCount(), 1u);  // uniquely owned
    EXPECT_EQ(raw->refCount(), 1u);       // still referenced by ptr2

    detached->value = 100;
    EXPECT_EQ(ptr1->value, 100);
    EXPECT_EQ(ptr2->value, 42);           // original unchanged
}

TEST(SharedDataTest, MutableArrowDetachesWhenShared) {
    auto *raw = new TestData(42);
    dini::SharedDataPointer<TestData> ptr1(raw);
    dini::SharedDataPointer<TestData> ptr2(ptr1);
    EXPECT_EQ(raw->refCount(), 2u);

    ptr1->value = 100;                    // detaches internally
    EXPECT_EQ(ptr1->value, 100);
    EXPECT_EQ(ptr2->value, 42);
    EXPECT_EQ(raw->refCount(), 1u);
    EXPECT_EQ(ptr1.constData()->refCount(), 1u);
}

TEST(SharedDataTest, MutableStarDetachesWhenShared) {
    auto *raw = new TestData(42);
    dini::SharedDataPointer<TestData> ptr1(raw);
    dini::SharedDataPointer<TestData> ptr2(ptr1);
    EXPECT_EQ(raw->refCount(), 2u);

    (*ptr1).value = 100;
    EXPECT_EQ(ptr1->value, 100);
    EXPECT_EQ(ptr2->value, 42);
    EXPECT_EQ(raw->refCount(), 1u);
}

TEST(SharedDataTest, NoDetachWhenUnique) {
    auto *raw = new TestData(42);
    dini::SharedDataPointer<TestData> ptr1(raw);
    EXPECT_EQ(raw->refCount(), 1u);

    TestData *data = ptr1.data();
    EXPECT_EQ(data, raw);                 // same block
    ptr1->value = 100;
    EXPECT_EQ(raw->value, 100);
}

TEST(SharedDataTest, DetachOnNullDoesNothing) {
    dini::SharedDataPointer<TestData> ptr;
    EXPECT_EQ(ptr.data(), nullptr);       // detach on null is safe
    EXPECT_TRUE(ptr.isNull());
}

// --- reset -----------------------------------------------------------------

TEST(SharedDataTest, ResetToNullReleasesOld) {
    TrackedData::alive_count = 0;
    dini::SharedDataPointer<TrackedData> ptr(new TrackedData());
    EXPECT_EQ(TrackedData::alive_count, 1);

    ptr.reset(nullptr);
    EXPECT_TRUE(ptr.isNull());
    EXPECT_EQ(TrackedData::alive_count, 0);
}

TEST(SharedDataTest, ResetToNewReleasesOldAndTakesNew) {
    TrackedData::alive_count = 0;
    auto *old_data = new TrackedData();
    dini::SharedDataPointer<TrackedData> ptr(old_data);
    EXPECT_EQ(TrackedData::alive_count, 1);

    auto *new_data = new TrackedData();
    ptr.reset(new_data);
    EXPECT_EQ(ptr.constData(), new_data);
    EXPECT_EQ(new_data->refCount(), 1u);
    EXPECT_EQ(TrackedData::alive_count, 1);   // old destroyed, new alive
}

TEST(SharedDataTest, ResetSamePointerNoOp) {
    TrackedData::alive_count = 0;
    auto *data = new TrackedData();
    dini::SharedDataPointer<TrackedData> ptr(data);
    EXPECT_EQ(TrackedData::alive_count, 1);

    ptr.reset(data);                            // same pointer
    EXPECT_EQ(ptr.constData(), data);
    EXPECT_EQ(TrackedData::alive_count, 1);
    EXPECT_EQ(data->refCount(), 1u);
}

TEST(SharedDataTest, ResetAfterShareReleasesCorrectly) {
    TrackedData::alive_count = 0;
    auto *data = new TrackedData();
    {
        dini::SharedDataPointer<TrackedData> ptr1(data);
        dini::SharedDataPointer<TrackedData> ptr2(ptr1);
        EXPECT_EQ(TrackedData::alive_count, 1);
        ptr1.reset();   // ptr1 drops its reference, block still alive via ptr2
        EXPECT_EQ(TrackedData::alive_count, 1);
    }
    EXPECT_EQ(TrackedData::alive_count, 0);
}

// --- Destruction -----------------------------------------------------------

TEST(SharedDataTest, DestructorDeletesWhenLastRef) {
    TrackedData::alive_count = 0;
    {
        dini::SharedDataPointer<TrackedData> ptr1(new TrackedData());
        EXPECT_EQ(TrackedData::alive_count, 1);
        {
            dini::SharedDataPointer<TrackedData> ptr2(ptr1);
            EXPECT_EQ(TrackedData::alive_count, 1);
        }
        EXPECT_EQ(TrackedData::alive_count, 1);
    }
    EXPECT_EQ(TrackedData::alive_count, 0);
}

// --- get_impl_helper -------------------------------------------------------

TEST(SharedDataTest, GetImplHelperConstAndMutable) {
    auto *derived = new DerivedData(10, 20);
    dini::SharedDataPointer<DerivedData> ptr(derived);
    const dini::SharedDataPointer<DerivedData> &cptr = ptr;

    const TestData *ctd = stdc::pimpl::_private_::get_impl_helper<TestData>(cptr);
    EXPECT_EQ(ctd, derived);
    EXPECT_EQ(ctd->value, 10);

    // Mutable helper returns pointer to the (possibly detached) block.
    // Here refCount == 1 so no detach occurs.
    TestData *td = stdc::pimpl::_private_::get_impl_helper<TestData>(ptr);
    EXPECT_EQ(td, derived);
    td->value = 42;
    EXPECT_EQ(derived->value, 42);
}

TEST(SharedDataTest, GetImplHelperMutableDetachesWhenShared) {
    auto *derived = new DerivedData(10, 20);
    dini::SharedDataPointer<DerivedData> ptr1(derived);
    dini::SharedDataPointer<DerivedData> ptr2(ptr1);
    EXPECT_EQ(derived->refCount(), 2u);

    // Mutable get_impl_helper calls data() which detaches.
    TestData *td = stdc::pimpl::_private_::get_impl_helper<TestData>(ptr1);
    EXPECT_NE(td, derived);                 // new copy
    EXPECT_EQ(td->refCount(), 1u);
    EXPECT_EQ(derived->refCount(), 1u);     // still owned by ptr2
}