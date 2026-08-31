// Platform tests.  The headless backend is a real window system in miniature:
// geometry, close requests, resize events, swap counting and event delivery.
// These tests exercise it directly; the SDL3 backend is only compiled when SDL3
// is present, so it is covered through the fallback contract instead.
#include "doctest.h"

#include "local3d/platform/Events.hpp"
#include "local3d/platform/Platform.hpp"

#include <string>
#include <variant>
#include <vector>

using namespace l3d;
using namespace l3d::platform;

namespace {

/// Owns the backend and exposes it as a HeadlessPlatform.  The raw view must not
/// outlive the fixture, so it is handed out by reference only.
class PlatformFixture {
public:
    PlatformFixture() {
        bool usedFallback = true;
        auto result = CreatePlatformBackend(BackendType::Headless, &usedFallback);
        REQUIRE_MESSAGE(result.HasValue(), "Failed to create the headless backend");
        CHECK_FALSE(usedFallback);
        backend_ = std::move(*result);
        platform_ = dynamic_cast<HeadlessPlatform*>(backend_.get());
        REQUIRE_MESSAGE(platform_ != nullptr, "Headless request returned another backend");
    }

    [[nodiscard]] HeadlessPlatform& operator*() const { return *platform_; }
    [[nodiscard]] HeadlessPlatform* operator->() const { return platform_; }

private:
    std::unique_ptr<IPlatformBackend> backend_;
    HeadlessPlatform* platform_ = nullptr;
};

[[nodiscard]] HeadlessWindow& AsHeadless(IWindow& window) {
    auto* headless = dynamic_cast<HeadlessWindow*>(&window);
    REQUIRE_MESSAGE(headless != nullptr, "Headless backend created a foreign window type");
    return *headless;
}

} // namespace

TEST_SUITE("platform.backend") {
    TEST_CASE("reports its identity") {
        PlatformFixture platform;
        CHECK(platform->Type() == BackendType::Headless);
        CHECK(platform->Name() == "headless");
        CHECK(BackendTypeName(BackendType::Headless) == "headless");
        CHECK(BackendTypeName(BackendType::Sdl3) == "sdl3");
        CHECK(BackendTypeName(static_cast<BackendType>(200)) == "unknown");
    }

    TEST_CASE("always returns a usable backend, falling back when needed") {
        bool usedFallback = false;
        auto backend = CreatePlatformBackend(BackendType::Sdl3, &usedFallback);
        REQUIRE(backend.HasValue());
        // Without SDL3 compiled in the request falls back to headless; with it the
        // real backend is used.  Either way the caller gets a working platform.
        if (usedFallback) {
            CHECK((*backend)->Type() == BackendType::Headless);
        } else {
            CHECK((*backend)->Type() == BackendType::Sdl3);
        }
    }

    TEST_CASE("elapsed time never goes backwards") {
        PlatformFixture platform;
        const u64 first = platform->ElapsedMs();
        u64 burn = 0;
        for (u64 i = 0; i < 100000; ++i) {
            burn += i;
        }
        CHECK(burn > 0); // Keeps the loop from being optimised away.
        const u64 second = platform->ElapsedMs();
        CHECK(second >= first);
        platform->Sleep(1); // A no-op in the headless backend, but must be callable.
        CHECK(platform->ElapsedMs() >= first);
    }
}

TEST_SUITE("platform.window") {
    TEST_CASE("creates a window with the requested geometry") {
        PlatformFixture platform;
        WindowDesc desc;
        desc.title = "Local3D Sandbox";
        desc.width = 1600;
        desc.height = 900;
        auto window = platform->CreateWindow(desc);
        REQUIRE(window.HasValue());
        CHECK((*window)->Title() == "Local3D Sandbox");
        CHECK((*window)->Width() == 1600);
        CHECK((*window)->Height() == 900);
        CHECK((*window)->Size().x == doctest::Approx(1600.0f));
        CHECK((*window)->Size().y == doctest::Approx(900.0f));
        CHECK((*window)->AspectRatio() == doctest::Approx(16.0f / 9.0f));
        CHECK_FALSE((*window)->ShouldClose());
        CHECK((*window)->NativeHandle() == nullptr);
    }

    TEST_CASE("rejects a zero sized window") {
        PlatformFixture platform;
        WindowDesc desc;
        desc.width = 0;
        auto window = platform->CreateWindow(desc);
        REQUIRE(window.IsError());
        CHECK(window.Error().Code() == StatusCode::InvalidArgument);
    }

    TEST_CASE("counts live windows") {
        PlatformFixture platform;
        CHECK(platform->WindowCount() == 0);
        WindowDesc desc;
        auto first = platform->CreateWindow(desc);
        REQUIRE(first.HasValue());
        CHECK(platform->WindowCount() == 1);
        {
            auto second = platform->CreateWindow(desc);
            REQUIRE(second.HasValue());
            CHECK(platform->WindowCount() == 2);
        } // Second window destroyed here.
        CHECK(platform->WindowCount() == 1);
        first->reset();
        CHECK(platform->WindowCount() == 0);
    }

    TEST_CASE("renames the window") {
        PlatformFixture platform;
        WindowDesc desc;
        desc.title = "before";
        auto window = platform->CreateWindow(desc);
        REQUIRE(window.HasValue());
        (*window)->SetTitle("after");
        CHECK((*window)->Title() == "after");
    }

    TEST_CASE("resizing updates the geometry and emits an event") {
        PlatformFixture platform;
        WindowDesc desc;
        desc.width = 1280;
        desc.height = 720;
        auto window = platform->CreateWindow(desc);
        REQUIRE(window.HasValue());

        (*window)->Resize(800, 600);
        CHECK((*window)->Width() == 800);
        CHECK((*window)->Height() == 600);
        CHECK(platform->PendingEventCount() == 1);

        std::vector<Event> events;
        platform->PollEvents(events);
        REQUIRE(events.size() == 1);
        const auto* resized = std::get_if<WindowResizedEvent>(&events[0]);
        REQUIRE(resized != nullptr);
        CHECK(resized->width == 800);
        CHECK(resized->height == 600);

        // Resizing to the same size is not an event.
        (*window)->Resize(800, 600);
        CHECK(platform->PendingEventCount() == 0);
    }

    TEST_CASE("a close request raises the flag and emits an event") {
        PlatformFixture platform;
        WindowDesc desc;
        auto window = platform->CreateWindow(desc);
        REQUIRE(window.HasValue());
        CHECK_FALSE((*window)->ShouldClose());

        (*window)->RequestClose();
        CHECK((*window)->ShouldClose());

        std::vector<Event> events;
        platform->PollEvents(events);
        REQUIRE(events.size() == 1);
        CHECK(std::holds_alternative<WindowCloseEvent>(events[0]));
    }

    TEST_CASE("counts presented frames") {
        PlatformFixture platform;
        WindowDesc desc;
        auto window = platform->CreateWindow(desc);
        REQUIRE(window.HasValue());
        auto& headless = AsHeadless(**window);
        CHECK(headless.SwapCount() == 0);
        for (int i = 0; i < 3; ++i) {
            (*window)->SwapBuffers();
        }
        CHECK(headless.SwapCount() == 3);
        CHECK(headless.Desc().width == desc.width);
    }
}

TEST_SUITE("platform.events") {
    TEST_CASE("delivers injected events in order and drains the queue") {
        PlatformFixture platform;
        KeyEvent down;
        down.key = Key::W;
        down.pressed = true;
        KeyEvent up;
        up.key = Key::W;
        up.pressed = false;
        MouseMoveEvent move;
        move.x = 100.0f;
        move.y = 200.0f;
        platform->InjectEvent(down);
        platform->InjectEvent(up);
        platform->InjectEvent(move);
        CHECK(platform->PendingEventCount() == 3);

        std::vector<Event> events;
        platform->PollEvents(events);
        REQUIRE(events.size() == 3);
        const auto* first = std::get_if<KeyEvent>(&events[0]);
        REQUIRE(first != nullptr);
        CHECK(first->key == Key::W);
        CHECK(first->pressed);
        const auto* second = std::get_if<KeyEvent>(&events[1]);
        REQUIRE(second != nullptr);
        CHECK_FALSE(second->pressed);
        const auto* third = std::get_if<MouseMoveEvent>(&events[2]);
        REQUIRE(third != nullptr);
        CHECK(third->x == doctest::Approx(100.0f));

        // The queue is empty afterwards; polling again appends nothing.
        CHECK(platform->PendingEventCount() == 0);
        platform->PollEvents(events);
        CHECK(events.size() == 3);
    }

    TEST_CASE("InjectEvents appends a whole frame of input") {
        PlatformFixture platform;
        std::vector<Event> batch;
        KeyEvent key;
        key.key = Key::Space;
        key.pressed = true;
        batch.emplace_back(key);
        MouseWheelEvent wheel;
        wheel.deltaY = 1.0f;
        batch.emplace_back(wheel);
        GamepadAxisEvent axis;
        axis.axis = GamepadAxis::LeftX;
        axis.value = 0.5f;
        batch.emplace_back(axis);
        GamepadConnectedEvent connected;
        connected.connected = true;
        connected.name = "Virtual Pad";
        batch.emplace_back(connected);
        TextInputEvent text;
        text.text = "héllo";
        batch.emplace_back(text);

        platform->InjectEvents(std::move(batch));
        CHECK(platform->PendingEventCount() == 5);

        std::vector<Event> events;
        platform->PollEvents(events);
        REQUIRE(events.size() == 5);
        CHECK(std::holds_alternative<KeyEvent>(events[0]));
        CHECK(std::holds_alternative<MouseWheelEvent>(events[1]));
        CHECK(std::holds_alternative<GamepadAxisEvent>(events[2]));
        CHECK(std::holds_alternative<GamepadConnectedEvent>(events[3]));
        const auto* typed = std::get_if<TextInputEvent>(&events[4]);
        REQUIRE(typed != nullptr);
        CHECK(typed->text == "héllo");
    }

    TEST_CASE("polling appends to a non-empty caller vector") {
        PlatformFixture platform;
        KeyEvent key;
        key.key = Key::Escape;
        key.pressed = true;
        platform->InjectEvent(key);

        std::vector<Event> events;
        QuitEvent quit;
        events.emplace_back(quit);
        platform->PollEvents(events);
        REQUIRE(events.size() == 2);
        CHECK(std::holds_alternative<QuitEvent>(events[0]));
        CHECK(std::holds_alternative<KeyEvent>(events[1]));
    }

    TEST_CASE("every key has a readable name") {
        CHECK(KeyName(Key::A) == "A");
        CHECK(KeyName(Key::Z) == "Z");
        CHECK(KeyName(Key::Num0) == "0");
        CHECK(KeyName(Key::Num9) == "9");
        CHECK(KeyName(Key::F1) == "F1");
        CHECK(KeyName(Key::F12) == "F12");
        CHECK(KeyName(Key::Escape) == "Escape");
        CHECK(KeyName(Key::Space) == "Space");
        CHECK(KeyName(Key::Backspace) == "Backspace");
        CHECK(KeyName(Key::PageUp) == "PageUp");
        CHECK(KeyName(Key::LeftShift) == "LeftShift");
        CHECK(KeyName(Key::RightSuper) == "RightSuper");
        CHECK(KeyName(Key::Unknown) == "Unknown");
    }
}
