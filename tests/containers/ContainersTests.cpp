// Container tests: SmallVector growth, SparseSet semantics, SlotMap handles,
// BitSet, and the SPSC ring buffer under two threads.
#include "doctest.h"

#include "local3d/containers/Containers.hpp"
#include "local3d/containers/SmallVector.hpp"
#include "local3d/containers/SlotMap.hpp"
#include "local3d/containers/SparseSet.hpp"

#include <string>
#include <thread>

using namespace l3d;

namespace {
/// Counts live instances so the test can prove SmallVector destroys elements.
struct LifetimeTracker {
    static inline int live = 0;
    LifetimeTracker() { ++live; }
    LifetimeTracker(const LifetimeTracker&) { ++live; }
    LifetimeTracker(LifetimeTracker&&) noexcept { ++live; }
    LifetimeTracker& operator=(const LifetimeTracker&) = default;
    LifetimeTracker& operator=(LifetimeTracker&&) noexcept = default;
    ~LifetimeTracker() { --live; }
};
} // namespace

TEST_SUITE("containers.small_vector") {
    TEST_CASE("stays inline until capacity is exceeded") {
        SmallVector<int, 4> values;
        CHECK(values.IsInline());
        for (int i = 0; i < 4; ++i) {
            values.PushBack(i);
        }
        CHECK(values.IsInline());
        CHECK(values.Size() == 4);

        values.PushBack(99);
        CHECK_FALSE(values.IsInline());
        CHECK(values.Size() == 5);
        CHECK(values[4] == 99);
        CHECK(values.MemoryBytes() >= 8 * sizeof(int));
    }

    TEST_CASE("preserves contents when growing") {
        SmallVector<int, 2> values;
        for (int i = 0; i < 100; ++i) {
            values.PushBack(i * 3);
        }
        REQUIRE(values.Size() == 100);
        for (int i = 0; i < 100; ++i) {
            CHECK(values[static_cast<usize>(i)] == i * 3);
        }
    }

    TEST_CASE("insert, erase and unordered erase") {
        SmallVector<int, 8> values{1, 2, 3, 4};
        values.Insert(1, 99);
        CHECK(values.Size() == 5);
        CHECK(values[1] == 99);
        CHECK(values[2] == 2);

        values.Erase(0);
        CHECK(values[0] == 99);

        values.EraseUnordered(0); // Swap the tail into slot 0.
        CHECK(values.Size() == 3);
        bool stillHas99 = false;
        for (const int value : values) {
            stillHas99 = stillHas99 || value == 99;
        }
        CHECK_FALSE(stillHas99);
    }

    TEST_CASE("copy and move") {
        SmallVector<std::string, 2> source;
        source.PushBack("alpha");
        source.PushBack("beta");
        source.PushBack("gamma");

        SmallVector<std::string, 2> copy = source;
        CHECK(copy.Size() == 3);
        CHECK(copy[2] == "gamma");

        SmallVector<std::string, 2> moved = std::move(source);
        CHECK(moved.Size() == 3);
        CHECK(moved[0] == "alpha");
    }

    TEST_CASE("destroys elements on pop and clear") {
        {
            SmallVector<LifetimeTracker, 2> values;
            for (int i = 0; i < 5; ++i) {
                values.EmplaceBack();
            }
            CHECK(LifetimeTracker::live >= 5);
            values.PopBack();
            values.Clear();
            CHECK(LifetimeTracker::live == 0);
        }
        CHECK(LifetimeTracker::live == 0);
    }

    TEST_CASE("range based for") {
        SmallVector<int, 4> values{1, 2, 3};
        int sum = 0;
        for (const int value : values) {
            sum += value;
        }
        CHECK(sum == 6);
    }
}

TEST_SUITE("containers.sparse_set") {
    TEST_CASE("insert, find, contains, remove") {
        SparseSet<int> set;
        set.InsertOrAssign(7, 100);
        set.InsertOrAssign(42, 200);
        CHECK(set.Size() == 2);
        CHECK(set.Contains(7));
        CHECK_FALSE(set.Contains(8));
        CHECK(*set.Find(42) == 200);
        CHECK(set.Find(999) == nullptr);

        set.InsertOrAssign(7, 999);
        CHECK(*set.Find(7) == 999);
        CHECK(set.Size() == 2);

        CHECK(set.Remove(7));
        CHECK_FALSE(set.Remove(7));
        CHECK(set.Size() == 1);
        CHECK(set.Contains(42));
    }

    TEST_CASE("removal keeps the dense array valid") {
        SparseSet<int> set;
        for (u32 i = 0; i < 100; ++i) {
            set.InsertOrAssign(i, static_cast<int>(i));
        }
        for (u32 i = 0; i < 100; i += 2) {
            CHECK(set.Remove(i));
        }
        CHECK(set.Size() == 50);
        for (const auto& entry : set) {
            CHECK(entry.id % 2 == 1);
            CHECK(entry.value == static_cast<int>(entry.id));
            CHECK(*set.Find(entry.id) == entry.value);
        }
    }

    TEST_CASE("clear resets both arrays") {
        SparseSet<int> set;
        set.InsertOrAssign(3, 1);
        set.Clear();
        CHECK(set.Empty());
        CHECK_FALSE(set.Contains(3));
        set.InsertOrAssign(3, 2);
        CHECK(*set.Find(3) == 2);
    }
}

TEST_SUITE("containers.slot_map") {
    TEST_CASE("handles detect reuse") {
        SlotMap<std::string> map;
        const auto first = map.Emplace("first");
        const auto second = map.Emplace("second");
        CHECK(*map.Get(first) == "first");
        CHECK(*map.Get(second) == "second");

        CHECK(map.Remove(first));
        CHECK(map.Get(first) == nullptr);
        CHECK_FALSE(map.IsAlive(first));

        const auto third = map.Emplace("third");
        CHECK(*map.Get(third) == "third");
        CHECK(map.Get(first) == nullptr); // Stale handle must not resolve.
        CHECK(first.index == third.index);
        CHECK(first.generation != third.generation);
    }

    TEST_CASE("invalid handles are rejected") {
        SlotMap<int> map;
        CHECK(map.Get(kInvalidSlotHandle) == nullptr);
        CHECK_FALSE(map.Remove(kInvalidSlotHandle));
        CHECK_FALSE(map.IsAlive(SlotHandle{999, 0}));
    }

    TEST_CASE("for each visits live entries only") {
        SlotMap<int> map;
        const auto a = map.Emplace(1);
        const auto b = map.Emplace(2);
        const auto c = map.Emplace(3);
        map.Remove(b);

        int sum = 0;
        usize count = 0;
        map.ForEach([&sum, &count](SlotHandle handle, const int& value) {
            L3D_UNUSED(handle);
            sum += value;
            ++count;
        });
        CHECK(sum == 4);
        CHECK(count == 2);
        CHECK(map.Size() == 2);
        CHECK(map.Get(a) != nullptr);
        CHECK(map.Get(c) != nullptr);
    }
}

TEST_SUITE("containers.bitset") {
    TEST_CASE("set, test and count") {
        BitSet<128> bits;
        CHECK_FALSE(bits.Any());
        bits.Set(0);
        bits.Set(63);
        bits.Set(64);
        bits.Set(127);
        CHECK(bits.Test(0));
        CHECK(bits.Test(63));
        CHECK(bits.Test(64));
        CHECK(bits.Test(127));
        CHECK_FALSE(bits.Test(1));
        CHECK(bits.Count() == 4);
        CHECK(bits.Any());

        bits.Set(63, false);
        CHECK_FALSE(bits.Test(63));
        CHECK(bits.Count() == 3);

        bits.Clear();
        CHECK(bits.Count() == 0);
    }

    TEST_CASE("out of range access is safe") {
        BitSet<8> bits;
        bits.Set(1000);
        CHECK_FALSE(bits.Test(1000));
        CHECK_FALSE(bits.Any());
    }
}

TEST_SUITE("containers.spsc_ring") {
    TEST_CASE("push and pop preserve order") {
        SpscRingBuffer<int, 8> ring;
        CHECK(ring.Empty());
        for (int i = 0; i < 7; ++i) {
            REQUIRE(ring.Push(i));
        }
        CHECK_FALSE(ring.Push(100)); // One slot is reserved to disambiguate full/empty.

        for (int i = 0; i < 7; ++i) {
            int value = -1;
            REQUIRE(ring.Pop(value));
            CHECK(value == i);
        }
        int unused = 0;
        CHECK_FALSE(ring.Pop(unused));
    }

    TEST_CASE("transfers between two threads without loss") {
        constexpr int kCount = 100000;
        SpscRingBuffer<int, 1024> ring;

        std::thread producer([&ring] {
            for (int i = 0; i < kCount; ++i) {
                while (!ring.Push(i)) {
                    std::this_thread::yield();
                }
            }
        });

        long long sum = 0;
        int received = 0;
        while (received < kCount) {
            int value = 0;
            if (ring.Pop(value)) {
                CHECK(value == received);
                sum += value;
                ++received;
            } else {
                std::this_thread::yield();
            }
        }
        producer.join();
        CHECK(received == kCount);
        CHECK(sum == static_cast<long long>(kCount - 1) * kCount / 2);
    }
}
