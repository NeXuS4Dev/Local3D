// Memory module tests: allocator behaviour, leak detection and STL/RAII glue.
#include "doctest.h"

#include "local3d/memory/Allocator.hpp"
#include "local3d/memory/LinearAllocator.hpp"
#include "local3d/memory/PoolAllocator.hpp"
#include "local3d/memory/TrackingAllocator.hpp"

#include <string>
#include <vector>

using namespace l3d;

namespace {
/// Counts live instances to verify UniquePtr lifetime handling.
struct Counted {
    static inline int live = 0;
    int value = 0;
    explicit Counted(int v) : value(v) { ++live; }
    ~Counted() { --live; }
};
} // namespace

TEST_SUITE("memory.alignment") {
    TEST_CASE("align up/down and power of two") {
        CHECK(AlignUp(1, 16) == 16);
        CHECK(AlignUp(16, 16) == 16);
        CHECK(AlignUp(17, 16) == 32);
        CHECK(AlignDown(31, 16) == 16);
        CHECK(IsAligned(64, 16));
        CHECK_FALSE(IsAligned(65, 16));
        CHECK(IsPowerOfTwo(1024));
        CHECK_FALSE(IsPowerOfTwo(1000));
        CHECK_FALSE(IsPowerOfTwo(0));
    }
}

TEST_SUITE("memory.default_allocator") {
    TEST_CASE("allocates aligned memory") {
        IAllocator& allocator = DefaultAllocator();
        void* small = allocator.Allocate(1, 16, kTagDefault);
        REQUIRE(small != nullptr);
        CHECK((reinterpret_cast<std::uintptr_t>(small) % 16) == 0);
        allocator.Deallocate(small, 1, 16);

        void* big = allocator.Allocate(4096, 256, kTagDefault);
        REQUIRE(big != nullptr);
        CHECK((reinterpret_cast<std::uintptr_t>(big) % 256) == 0);
        allocator.Deallocate(big, 4096, 256);

        CHECK(allocator.Allocate(0, 16, kTagDefault) == nullptr);
        CHECK(std::string_view(allocator.Name()) == "malloc");
    }
}

TEST_SUITE("memory.linear") {
    TEST_CASE("bumps sequentially with alignment") {
        LinearAllocator allocator(1024, "test");
        void* a = allocator.Allocate(10, 8, kTagFrame);
        void* b = allocator.Allocate(10, 16, kTagFrame);
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        CHECK((reinterpret_cast<std::uintptr_t>(b) % 16) == 0);
        CHECK(allocator.Offset() >= 20);
        CHECK(allocator.PeakOffset() == allocator.Offset());

        allocator.Deallocate(a, 10, 8); // No-op by design.
        CHECK(allocator.Offset() >= 20);

        allocator.Reset();
        CHECK(allocator.Offset() == 0);
        CHECK(allocator.Remaining() == allocator.Capacity());
    }

    TEST_CASE("returns nullptr when exhausted") {
        LinearAllocator allocator(64, "test");
        CHECK(allocator.Allocate(64, 8, kTagFrame) != nullptr);
        CHECK(allocator.Allocate(1, 8, kTagFrame) == nullptr);
    }

    TEST_CASE("scope rewinds on exit") {
        LinearAllocator allocator(1024, "test");
        void* first = allocator.Allocate(64, 8, kTagFrame);
        REQUIRE(first != nullptr);
        {
            LinearAllocator::Scope scope(allocator);
            allocator.Allocate(128, 8, kTagFrame);
            CHECK(allocator.Offset() >= 192);
        }
        CHECK(allocator.Offset() == 64);
    }

    TEST_CASE("frame allocator rotates and resets") {
        FrameAllocator frames(256, 3, "test-frame");
        CHECK(frames.FrameCount() == 3);
        LinearAllocator& frame0 = frames.Current();
        frame0.Allocate(100, 8, kTagFrame);
        CHECK(frame0.Offset() == 100);

        frames.BeginFrame();
        CHECK(frames.Current().Offset() == 0);
        frames.Current().Allocate(50, 8, kTagFrame);

        frames.BeginFrame();
        CHECK(frames.Current().Offset() == 0);
        frames.BeginFrame(); // Wraps back to buffer 0, which gets reset.
        CHECK(frames.Current().Offset() == 0);
    }
}

TEST_SUITE("memory.pool") {
    TEST_CASE("hands out and recycles fixed blocks") {
        PoolAllocator pool(64, 4, "test");
        std::vector<void*> blocks;
        for (int i = 0; i < 4; ++i) {
            void* block = pool.Allocate(64, 8, kTagDefault);
            REQUIRE(block != nullptr);
            blocks.push_back(block);
        }
        CHECK(pool.FreeBlocks() == 0);
        CHECK(pool.Allocate(64, 8, kTagDefault) == nullptr);

        pool.Deallocate(blocks[1], 64, 8);
        CHECK(pool.FreeBlocks() == 1);
        void* recycled = pool.Allocate(64, 8, kTagDefault);
        CHECK(recycled == blocks[1]);
        CHECK(pool.Owns(recycled));
    }

    TEST_CASE("rejects oversized and over-aligned requests") {
        PoolAllocator pool(32, 2, "test");
        CHECK(pool.Allocate(64, 8, kTagDefault) == nullptr);
        CHECK(pool.Allocate(16, 4096, kTagDefault) == nullptr);
    }

    TEST_CASE("stats track usage") {
        PoolAllocator pool(128, 8, "test");
        void* a = pool.Allocate(128, 8, kTagDefault);
        void* b = pool.Allocate(128, 8, kTagDefault);
        const MemoryStats stats = pool.Snapshot();
        CHECK(stats.allocatedBytes == 256);
        CHECK(stats.allocationCount == 2);
        CHECK(stats.totalAllocations == 2);
        pool.Deallocate(a, 128, 8);
        pool.Deallocate(b, 128, 8);
        CHECK(pool.Snapshot().allocatedBytes == 0);
    }
}

TEST_SUITE("memory.tracking") {
    TEST_CASE("detects leaks") {
        TrackingAllocator tracker(DefaultAllocator(), "test");
        void* leaked = tracker.Allocate(100, 16, kTagAsset);
        REQUIRE(leaked != nullptr);
        CHECK(tracker.HasLeaks());
        const MemoryStats stats = tracker.Snapshot();
        CHECK(stats.allocatedBytes == 100);
        CHECK(stats.allocationCount == 1);

        tracker.Deallocate(leaked, 100, 16);
        CHECK_FALSE(tracker.HasLeaks());
        CHECK(tracker.Snapshot().allocatedBytes == 0);
        CHECK(tracker.ErrorCount() == 0);
    }

    TEST_CASE("reports double free as an error") {
        TrackingAllocator tracker(DefaultAllocator(), "test");
        void* memory = tracker.Allocate(64, 16, kTagDefault);
        tracker.Deallocate(memory, 64, 16);
        tracker.Deallocate(memory, 64, 16); // Recorded, not fatal.
        CHECK(tracker.ErrorCount() == 1);
        CHECK_FALSE(tracker.HasLeaks());
    }

    TEST_CASE("reports by tag sorted by bytes") {
        TrackingAllocator tracker(DefaultAllocator(), "test");
        void* asset = tracker.Allocate(1000, 16, kTagAsset);
        void* render = tracker.Allocate(100, 16, kTagRender);
        const auto report = tracker.ReportByTag();
        REQUIRE(report.size() == 2);
        CHECK(report[0].tag == "asset");
        CHECK(report[0].bytes == 1000);
        CHECK(report[1].tag == "render");
        tracker.Deallocate(asset, 1000, 16);
        tracker.Deallocate(render, 100, 16);
        CHECK_FALSE(tracker.HasLeaks());
    }
}

TEST_SUITE("memory.stl_adapter") {
    TEST_CASE("std::vector allocates through the tracked allocator") {
        TrackingAllocator tracker(DefaultAllocator(), "stl");
        {
            std::vector<int, L3DAllocator<int>> values{L3DAllocator<int>(tracker, kTagDefault)};
            for (int i = 0; i < 1000; ++i) {
                values.push_back(i);
            }
            CHECK(values.size() == 1000);
            CHECK(values[999] == 999);
            CHECK(tracker.Snapshot().allocationCount > 0);
        }
        CHECK_FALSE(tracker.HasLeaks());
    }

    TEST_CASE("std::string allocates through the tracked allocator") {
        TrackingAllocator tracker(DefaultAllocator(), "stl");
        {
            using TrackedString =
                std::basic_string<char, std::char_traits<char>, L3DAllocator<char>>;
            TrackedString text(L3DAllocator<char>(tracker, kTagDefault));
            text += "local3d allocates through an interface";
            CHECK(text.size() > 30);
        }
        CHECK_FALSE(tracker.HasLeaks());
    }
}

TEST_SUITE("memory.unique_ptr") {
    TEST_CASE("constructs and destroys through the allocator") {
        TrackingAllocator tracker(DefaultAllocator(), "unique");
        {
            auto object = UniquePtr<Counted>::Create(tracker, kTagDefault, 42);
            REQUIRE(static_cast<bool>(object));
            CHECK(object->value == 42);
            CHECK(Counted::live == 1);
        }
        CHECK(Counted::live == 0);
        CHECK_FALSE(tracker.HasLeaks());
    }

    TEST_CASE("move transfers ownership") {
        TrackingAllocator tracker(DefaultAllocator(), "unique");
        auto first = UniquePtr<Counted>::Create(tracker, kTagDefault, 7);
        auto second = std::move(first);
        CHECK_FALSE(static_cast<bool>(first));
        CHECK(second->value == 7);
        CHECK(Counted::live == 1);
        CHECK(tracker.HasLeaks()); // `second` is still alive here.
        second.Reset();
        CHECK_FALSE(tracker.HasLeaks());
        CHECK(Counted::live == 0);
    }
}
