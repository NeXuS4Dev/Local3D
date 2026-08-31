// Core module tests: assertions, formatting, status/expected, hashing, uuid,
// static strings, time, profiling, logging, the job system and the C ABI.
#include "doctest.h"

#include "local3d/core/Assert.hpp"
#include "local3d/core/ConcurrentQueue.hpp"
#include "local3d/core/Format.hpp"
#include "local3d/core/Hash.hpp"
#include "local3d/core/Log.hpp"
#include "local3d/core/Profiler.hpp"
#include "local3d/core/Result.hpp"
#include "local3d/core/Signal.hpp"
#include "local3d/core/StaticString.hpp"
#include "local3d/core/Status.hpp"
#include "local3d/core/Time.hpp"
#include "local3d/core/Uuid.hpp"
#include "local3d/core/c/l3d_allocator.h"
#include "local3d/core/c/l3d_log.h"
#include "local3d/core/jobs/JobSystem.hpp"

#include <atomic>
#include <cstring>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using namespace l3d;

TEST_SUITE("core.format") {
    TEST_CASE("substitutes positional arguments") {
        CHECK(fmt::Format("hello {}", "world") == "hello world");
        CHECK(fmt::Format("{} + {} = {}", 1, 2, 3) == "1 + 2 = 3");
        CHECK(fmt::Format("{1} before {0}", "second", "first") == "first before second");
    }

    TEST_CASE("handles escapes and literals") {
        CHECK(fmt::Format("{{literal}}") == "{literal}");
        CHECK(fmt::Format("no args") == "no args");
        CHECK(fmt::Format("trailing {") == "trailing {");
    }

    TEST_CASE("formats numbers") {
        CHECK(fmt::Format("{}", 255) == "255");
        CHECK(fmt::Format("{:x}", 255) == "ff");
        CHECK(fmt::Format("{}", -42) == "-42");
        CHECK(fmt::Format("{:.2f}", 3.14159) == "3.14");
        CHECK(fmt::Format("{}", true) == "true");
        CHECK(fmt::Format("{}", 'x') == "x");
        CHECK(fmt::Format("{}", static_cast<const char*>(nullptr)) == "(null)");
    }

    TEST_CASE("reports out of range indices instead of crashing") {
        CHECK(fmt::Format("{} {}", 1) == "1 <missing-arg>");
    }

    TEST_CASE("buffer formatting truncates safely") {
        char buffer[8]{};
        const usize written = fmt::FormatToBuffer(buffer, "0123456789abc");
        CHECK(written == 7);
        CHECK(std::string(buffer) == "0123456");
    }
}

TEST_SUITE("core.status") {
    TEST_CASE("ok by default") {
        const Status status;
        CHECK(status.IsOk());
        CHECK(status.Code() == StatusCode::Ok);
        CHECK(status.ToString() == "Ok");
    }

    TEST_CASE("carries a truncated message") {
        const Status status(StatusCode::NotFound, "asset missing");
        CHECK(status.IsError());
        CHECK(status.Message() == "asset missing");
        CHECK(status.ToString() == "NotFound: asset missing");

        const std::string longMessage(200, 'x');
        const Status truncated(StatusCode::IoError, longMessage);
        CHECK(truncated.Message().size() == kStatusMessageCapacity);
    }

    TEST_CASE("code names are stable") {
        CHECK(StatusCodeName(StatusCode::InvalidArgument) == "InvalidArgument");
        CHECK(StatusCodeName(static_cast<StatusCode>(999)) == "UnknownStatus");
    }
}

TEST_SUITE("core.expected") {
    static Result<int> Divide(int a, int b) {
        if (b == 0) {
            return Unexpected(Status{StatusCode::InvalidArgument, "division by zero"});
        }
        return a / b;
    }

    TEST_CASE("success path") {
        const auto result = Divide(10, 2);
        REQUIRE(result.HasValue());
        CHECK(*result == 5);
        CHECK(result.ValueOr(-1) == 5);
    }

    TEST_CASE("error path") {
        const auto result = Divide(10, 0);
        REQUIRE(result.IsError());
        CHECK(result.Error().Code() == StatusCode::InvalidArgument);
        CHECK(result.ValueOr(-1) == -1);
    }

    TEST_CASE("monadic combinators") {
        const auto doubled = Divide(10, 2).Map([](int value) { return value * 2; });
        REQUIRE(doubled.HasValue());
        CHECK(*doubled == 10);

        const auto propagated = Divide(1, 0).Map([](int value) { return value * 2; });
        REQUIRE(propagated.IsError());
        CHECK(propagated.Error().Code() == StatusCode::InvalidArgument);

        const auto chained = Divide(10, 2).AndThen([](int value) { return Divide(value, 5); });
        REQUIRE(chained.HasValue());
        CHECK(*chained == 1);
    }

    TEST_CASE("void specialisation") {
        const OperationResult ok;
        CHECK(ok.HasValue());
        const OperationResult failed{Unexpected(Status{StatusCode::Timeout, "too slow"})};
        REQUIRE(failed.IsError());
        CHECK(failed.Error().Code() == StatusCode::Timeout);
    }

    TEST_CASE("L3D_TRY early returns the error") {
        const auto wrapper = [](int b) -> Result<int> {
            L3D_TRY(value, Divide(100, b));
            return value + 1;
        };
        CHECK(*wrapper(10) == 11);
        const auto failed = wrapper(0);
        REQUIRE(failed.IsError());
        CHECK(failed.Error().Code() == StatusCode::InvalidArgument);
    }
}

TEST_SUITE("core.hash") {
    TEST_CASE("fnv1a matches the reference vector for the empty string") {
        CHECK(HashString("") == kFnvOffsetBasis);
        CHECK(HashString("a") == 0xAF63DC4C8601EC8CULL);
        CHECK(HashString("foobar") == 0x85944171F73967E8ULL);
    }

    TEST_CASE("hashing is case insensitive for paths") {
        CHECK(HashStringCaseInsensitive("Assets/Mesh.glb") ==
              HashStringCaseInsensitive("assets/mesh.glb"));
    }

    TEST_CASE("HashOf is order dependent") {
        CHECK(HashOf(1ULL, 2ULL) != HashOf(2ULL, 1ULL));
    }
}

TEST_SUITE("core.uuid") {
    TEST_CASE("round trips through text") {
        const Uuid original = Uuid::Generate();
        const std::string text = original.ToString();
        REQUIRE(text.size() == 36);

        Uuid parsed;
        REQUIRE(Uuid::Parse(text, parsed));
        CHECK(parsed == original);
    }

    TEST_CASE("rejects malformed text") {
        Uuid out;
        CHECK_FALSE(Uuid::Parse("not-a-uuid", out));
        CHECK_FALSE(Uuid::Parse("", out));
        CHECK_FALSE(Uuid::Parse("00000000-0000-0000-0000-00000000000", out));
    }

    TEST_CASE("FromName is deterministic") {
        CHECK(Uuid::FromName("engine/default-material") == Uuid::FromName("engine/default-material"));
        CHECK(Uuid::FromName("a") != Uuid::FromName("b"));
    }

    TEST_CASE("null uuid and ordering") {
        CHECK(kNullUuid.IsNull());
        const Uuid a{1, 2};
        const Uuid b{1, 3};
        CHECK(a < b);
        CHECK(a.Hash() != b.Hash());
    }
}

TEST_SUITE("core.static_string") {
    TEST_CASE("assign and append") {
        StaticString<16> text{"hello"};
        CHECK(text.View() == "hello");
        text.Append(" world");
        CHECK(text.View() == "hello world");
        CHECK(text.Size() == 11);
    }

    TEST_CASE("truncates instead of overflowing") {
        StaticString<4> text{"abcdef"};
        CHECK(text.View() == "abc");
        CHECK(std::strlen(text.CStr()) == 3);
    }

    TEST_CASE("format produces a bounded string") {
        const auto text = PathString::Format("{}/{}", "assets", 42);
        CHECK(text.View() == "assets/42");
    }
}

TEST_SUITE("core.time") {
    TEST_CASE("clock is monotonic") {
        const u64 first = Clock::NowNs();
        const u64 second = Clock::NowNs();
        CHECK(second >= first);
    }

    TEST_CASE("fixed timestep accumulates whole steps") {
        FixedTimestep step(1.0f / 60.0f, 5);
        CHECK(step.Accumulate(0.01f) == 0);
        CHECK(step.Accumulate(0.01f) == 1); // ~0.02s accumulated
        CHECK(step.Accumulate(1.0f) == 5);  // clamped to maxStepsPerFrame
        CHECK(step.Accumulate(1.0f) == 5);
    }

    TEST_CASE("frame clock smooths fps") {
        FrameClock clock;
        clock.BeginFrame();
        clock.EndFrame();
        CHECK(clock.FrameCount() == 1);
        CHECK(clock.FramesPerSecond() > 0.0);
    }
}

TEST_SUITE("core.assert") {
    TEST_CASE("a failing assert is reported to the installed handler") {
        // The test main installs a capturing handler; see tests/TestMain.cpp.
        L3D_ASSERT(1 == 1);
        L3D_ASSERT_MSG(true, "should not fire");
        CHECK(true);
    }
}

TEST_SUITE("core.profiler") {
    TEST_CASE("records nested markers for a frame") {
        auto& profiler = prof::Profiler::Instance();
        profiler.BeginFrame();
        {
            L3D_PROFILE_SCOPE("Outer");
            {
                L3D_PROFILE_SCOPE("Inner");
            }
        }
        const std::vector<prof::Marker> markers = profiler.CollectMarkers();
        bool sawOuter = false;
        bool sawInner = false;
        for (const prof::Marker& marker : markers) {
            if (std::string_view(marker.name) == "Outer") {
                sawOuter = true;
                CHECK(marker.depth == 0);
            }
            if (std::string_view(marker.name) == "Inner") {
                sawInner = true;
                CHECK(marker.depth == 1);
            }
        }
        CHECK(sawOuter);
        CHECK(sawInner);
    }

    TEST_CASE("counters accumulate") {
        auto& profiler = prof::Profiler::Instance();
        profiler.SetCounter("test.draws", 0);
        profiler.IncrementCounter("test.draws", 5);
        profiler.IncrementCounter("test.draws", 7);
        CHECK(profiler.GetCounter("test.draws") == 12);
    }
}

TEST_SUITE("core.logging") {
    TEST_CASE("sinks receive filtered messages") {
        auto& logger = Logger::Instance();
        const LogLevel previous = logger.GetLevel();
        logger.SetLevel(LogLevel::Trace);

        std::vector<std::string> captured;
        const u64 handle = logger.AddSink([&captured](const LogMessage& message) {
            captured.emplace_back(message.text);
        });

        L3D_LOG_INFO(LogCategory::Core, "hello {}", 42);
        L3D_LOG_ERROR(LogCategory::Assets, "failed to load {}", "a.glb");

        logger.RemoveSink(handle);
        logger.SetLevel(previous);

        REQUIRE(captured.size() == 2);
        CHECK(captured[0] == "hello 42");
        CHECK(captured[1] == "failed to load a.glb");
    }

    TEST_CASE("level filtering drops quiet messages") {
        auto& logger = Logger::Instance();
        const LogLevel previous = logger.GetLevel();
        logger.SetLevel(LogLevel::Error);

        int count = 0;
        const u64 handle = logger.AddSink([&count](const LogMessage&) { ++count; });

        L3D_LOG_INFO(LogCategory::Core, "should be dropped");
        L3D_LOG_ERROR(LogCategory::Core, "should pass");

        logger.RemoveSink(handle);
        logger.SetLevel(previous);
        CHECK(count == 1);
    }

    TEST_CASE("category names are unique and stable") {
        CHECK(LogCategoryName(LogCategory::Renderer) == "render");
        CHECK(LogLevelName(LogLevel::Warning) == "WARN");
    }
}

TEST_SUITE("core.concurrent_queue") {
    TEST_CASE("single producer single consumer") {
        ConcurrentQueue<int> queue(64);
        for (int i = 0; i < 64; ++i) {
            REQUIRE(queue.TryEnqueue(i));
        }
        CHECK_FALSE(queue.TryEnqueue(999)); // full

        int sum = 0;
        int value = 0;
        while (queue.TryDequeue(value)) {
            sum += value;
        }
        CHECK(sum == (63 * 64) / 2);
        CHECK(queue.SizeApprox() == 0);
    }

    TEST_CASE("multi producer multi consumer conserves every item") {
        constexpr u32 kProducers = 4;
        constexpr u32 kPerProducer = 5000;

        ConcurrentQueue<u32> queue(1024);
        std::atomic<u64> producedSum{0};
        std::atomic<u64> consumedSum{0};
        std::atomic<u32> consumedCount{0};
        std::atomic<bool> producersDone{false};

        std::vector<std::thread> producers;
        for (u32 p = 0; p < kProducers; ++p) {
            producers.emplace_back([&queue, &producedSum, p] {
                for (u32 i = 0; i < kPerProducer; ++i) {
                    const u32 value = p * kPerProducer + i;
                    while (!queue.TryEnqueue(value)) {
                        std::this_thread::yield();
                    }
                    producedSum.fetch_add(value, std::memory_order_relaxed);
                }
            });
        }

        std::vector<std::thread> consumers;
        for (u32 c = 0; c < 2; ++c) {
            consumers.emplace_back([&] {
                u32 value = 0;
                for (;;) {
                    if (queue.TryDequeue(value)) {
                        consumedSum.fetch_add(value, std::memory_order_relaxed);
                        consumedCount.fetch_add(1, std::memory_order_relaxed);
                    } else if (producersDone.load(std::memory_order_acquire) &&
                               queue.SizeApprox() == 0) {
                        // One more attempt guards against a race with a producer
                        // that has pushed but not yet flagged done.
                        if (!queue.TryDequeue(value)) {
                            return;
                        }
                        consumedSum.fetch_add(value, std::memory_order_relaxed);
                        consumedCount.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        std::this_thread::yield();
                    }
                }
            });
        }

        for (auto& producer : producers) {
            producer.join();
        }
        producersDone.store(true, std::memory_order_release);
        for (auto& consumer : consumers) {
            consumer.join();
        }

        CHECK(consumedCount.load() == kProducers * kPerProducer);
        CHECK(consumedSum.load() == producedSum.load());
    }
}

TEST_SUITE("core.job_system") {
    TEST_CASE("submits and waits for a single job") {
        auto jobs = jobs::JobSystem::Create({2, 256, "l3d-test"});
        REQUIRE(jobs != nullptr);

        std::atomic<int> counter{0};
        jobs->Submit([&counter] { counter.fetch_add(1); }).Wait();
        CHECK(counter.load() == 1);
    }

    TEST_CASE("parallel for covers every index exactly once") {
        auto jobs = jobs::JobSystem::Create({4, 4096, "l3d-test"});
        REQUIRE(jobs != nullptr);

        constexpr u32 kItems = 20000;
        std::vector<std::atomic<u32>> hits(kItems);
        for (auto& hit : hits) {
            hit.store(0);
        }

        jobs::ParallelForBlocking(*jobs, kItems, 64, [&hits](u32 begin, u32 end) {
            for (u32 i = begin; i < end; ++i) {
                hits[i].fetch_add(1);
            }
        });

        u32 total = 0;
        for (const auto& hit : hits) {
            total += hit.load();
        }
        CHECK(total == kItems);
    }

    TEST_CASE("many jobs all complete under contention") {
        auto jobs = jobs::JobSystem::Create({4, 128, "l3d-test"});
        REQUIRE(jobs != nullptr);

        std::atomic<u64> counter{0};
        std::vector<jobs::JobHandle> handles;
        for (int i = 0; i < 2000; ++i) {
            handles.push_back(jobs->Submit([&counter] { counter.fetch_add(1); }));
        }
        for (const auto& handle : handles) {
            handle.Wait();
        }
        CHECK(counter.load() == 2000);

        const auto stats = jobs->Stats();
        CHECK(stats.submitted == 2000);
        CHECK(stats.executed + stats.executedInline == 2000);
    }

    TEST_CASE("wait all drains the queue") {
        auto jobs = jobs::JobSystem::Create({3, 512, "l3d-test"});
        REQUIRE(jobs != nullptr);
        std::atomic<u32> counter{0};
        for (int i = 0; i < 500; ++i) {
            jobs->Submit([&counter] { counter.fetch_add(1); });
        }
        jobs->WaitAll();
        CHECK(counter.load() == 500);
    }

    TEST_CASE("shutdown completes pending work") {
        std::atomic<u32> counter{0};
        {
            auto jobs = jobs::JobSystem::Create({2, 1024, "l3d-test"});
            REQUIRE(jobs != nullptr);
            for (int i = 0; i < 100; ++i) {
                jobs->Submit([&counter] { counter.fetch_add(1); });
            }
        }
        CHECK(counter.load() == 100);
    }
}

TEST_SUITE("core.signal") {
    TEST_CASE("dispatches to every connection in order") {
        Signal<int> signal;
        std::vector<int> seen;
        const auto first = signal.Connect([&seen](int value) { seen.push_back(value); });
        signal.Connect([&seen](int value) { seen.push_back(value * 2); });

        signal.Emit(21);
        REQUIRE(seen.size() == 2);
        CHECK(seen[0] == 21);
        CHECK(seen[1] == 42);

        signal.Disconnect(first);
        seen.clear();
        signal.Emit(1);
        REQUIRE(seen.size() == 1);
        CHECK(seen[0] == 2);
    }

    TEST_CASE("a slot may disconnect during dispatch") {
        Signal<int> signal;
        SignalConnection self;
        int calls = 0;
        self = signal.Connect([&](int) {
            ++calls;
            signal.Disconnect(self);
        });
        signal.Emit(1);
        signal.Emit(1);
        CHECK(calls == 1);
    }
}

TEST_SUITE("core.c_abi") {
    TEST_CASE("c logging bridge forwards to the c++ logger") {
        std::vector<std::string> captured;
        const u64 handle = Logger::Instance().AddSink(
            [&captured](const LogMessage& message) { captured.emplace_back(message.text); });

        l3d_log_write(L3D_LOG_LEVEL_INFO, L3D_LOG_CATEGORY_CORE, "from c");
        Logger::Instance().RemoveSink(handle);

        REQUIRE(captured.size() == 1);
        CHECK(captured[0] == "from c");
        CHECK(std::string(l3d_log_level_name(L3D_LOG_LEVEL_WARNING)) == "WARN");
    }

    TEST_CASE("c observers are called") {
        struct Context {
            int count = 0;
        };
        static Context context;
        context.count = 0;

        const auto callback = +[](const l3d_log_record* record, void* user_data) {
            auto* ctx = static_cast<Context*>(user_data);
            if (record != nullptr && record->level == L3D_LOG_LEVEL_ERROR) {
                ++ctx->count;
            }
        };
        const u64 observer = l3d_log_add_observer(callback, &context);
        REQUIRE(observer != 0);

        l3d_log_write(L3D_LOG_LEVEL_ERROR, L3D_LOG_CATEGORY_RENDERER, "c observer test");
        l3d_log_remove_observer(observer);
        CHECK(context.count == 1);
    }

    TEST_CASE("system allocator honours alignment") {
        l3d_allocator* allocator = l3d_allocator_system();
        REQUIRE(allocator != nullptr);
        REQUIRE(allocator->abi_version == L3D_ALLOCATOR_ABI_VERSION);

        void* memory = allocator->allocate(allocator, 1024, 64, "test");
        REQUIRE(memory != nullptr);
        CHECK((reinterpret_cast<std::uintptr_t>(memory) % 64) == 0);
        allocator->deallocate(allocator, memory, 1024, 64);
    }

    TEST_CASE("linear allocator bumps and resets") {
        std::vector<std::byte> block(4096);
        l3d_allocator* allocator = l3d_allocator_linear_create(block.data(), block.size());
        REQUIRE(allocator != nullptr);

        void* first = allocator->allocate(allocator, 100, 16, "a");
        void* second = allocator->allocate(allocator, 100, 16, "b");
        REQUIRE(first != nullptr);
        REQUIRE(second != nullptr);
        CHECK(first != second);

        uint64_t bytesInUse = 0;
        uint64_t allocations = 0;
        allocator->stats(allocator, &bytesInUse, &allocations);
        CHECK(bytesInUse >= 200);

        // Exhaust it.
        void* overflow = allocator->allocate(allocator, 1 << 20, 16, "too big");
        CHECK(overflow == nullptr);

        l3d_allocator_linear_reset(allocator);
        allocator->stats(allocator, &bytesInUse, &allocations);
        CHECK(bytesInUse == 0);

        l3d_allocator_destroy(allocator);
    }
}
