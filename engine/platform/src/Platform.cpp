#include "local3d/platform/Platform.hpp"

#include "local3d/core/Log.hpp"
#include "local3d/core/Time.hpp"

#include <array>
#include <string>

namespace l3d::platform {

HeadlessWindow::HeadlessWindow(const WindowDesc& desc, HeadlessPlatform* owner)
    : desc_(desc), owner_(owner) {
    if (owner_ != nullptr) {
        owner_->NotifyWindowCreated();
    }
}

HeadlessWindow::~HeadlessWindow() {
    if (owner_ != nullptr) {
        owner_->NotifyWindowDestroyed();
    }
}

std::string_view HeadlessWindow::Title() const noexcept { return desc_.title; }
u32 HeadlessWindow::Width() const noexcept { return desc_.width; }
u32 HeadlessWindow::Height() const noexcept { return desc_.height; }
bool HeadlessWindow::ShouldClose() const noexcept { return shouldClose_; }

void HeadlessWindow::RequestClose() {
    shouldClose_ = true;
    if (owner_ != nullptr) {
        owner_->InjectEvent(WindowCloseEvent{});
    }
}

void HeadlessWindow::SetTitle(std::string_view title) { desc_.title = std::string(title); }

void HeadlessWindow::Resize(u32 width, u32 height) {
    if (width == desc_.width && height == desc_.height) {
        return;
    }
    desc_.width = width;
    desc_.height = height;
    if (owner_ != nullptr) {
        owner_->InjectEvent(WindowResizedEvent{width, height, desc_.fullscreen});
    }
}

void* HeadlessWindow::NativeHandle() const noexcept { return nullptr; }

void HeadlessWindow::SwapBuffers() { ++swapCount_; }

std::string_view BackendTypeName(BackendType type) noexcept {
    switch (type) {
        case BackendType::Headless: return "headless";
        case BackendType::Sdl3: return "sdl3";
    }
    return "unknown";
}

HeadlessPlatform::HeadlessPlatform() : startNs_(Clock::NowNs()) {}
HeadlessPlatform::~HeadlessPlatform() = default;

Result<std::unique_ptr<IWindow>> HeadlessPlatform::CreateWindow(const WindowDesc& desc) {
    if (desc.width == 0 || desc.height == 0) {
        return Unexpected(Status{StatusCode::InvalidArgument, "Window size must be non-zero"});
    }
    return std::unique_ptr<IWindow>(new HeadlessWindow(desc, this));
}

void HeadlessPlatform::PollEvents(std::vector<Event>& outEvents) {
    outEvents.insert(outEvents.end(), pendingEvents_.begin(), pendingEvents_.end());
    pendingEvents_.clear();
}

u64 HeadlessPlatform::ElapsedMs() const noexcept {
    return (Clock::NowNs() - startNs_) / 1'000'000ULL;
}

void HeadlessPlatform::Sleep(u32 ms) {
    // Sleeping in the headless backend is a no-op: tests and tools want to run
    // as fast as possible, and there is no display to pace.
    L3D_UNUSED(ms);
}

void HeadlessPlatform::InjectEvent(Event event) { pendingEvents_.push_back(std::move(event)); }

void HeadlessPlatform::InjectEvents(std::vector<Event> events) {
    for (Event& event : events) {
        pendingEvents_.push_back(std::move(event));
    }
}

Result<std::unique_ptr<IPlatformBackend>> CreatePlatformBackend(BackendType preferred,
                                                                bool* usedFallback) {
    if (usedFallback != nullptr) {
        *usedFallback = false;
    }
#if defined(L3D_PLATFORM_SDL3)
    if (preferred == BackendType::Sdl3) {
        // Implemented in Sdl3Platform.cpp; only compiled when SDL3 is present.
        return CreateSdl3PlatformBackend();
    }
#else
    if (preferred == BackendType::Sdl3) {
        L3D_LOG_WARN(LogCategory::Platform,
                     "SDL3 backend requested but not built in; falling back to headless");
        if (usedFallback != nullptr) {
            *usedFallback = true;
        }
    }
#endif
    return std::unique_ptr<IPlatformBackend>(new HeadlessPlatform());
}

std::string_view KeyName(Key key) noexcept {
    if (key >= Key::A && key <= Key::Z) {
        static std::array<std::string, 26> names = [] {
            std::array<std::string, 26> generated{};
            for (usize i = 0; i < generated.size(); ++i) {
                generated[i] = std::string(1, static_cast<char>('A' + i));
            }
            return generated;
        }();
        return names[static_cast<usize>(key) - static_cast<usize>(Key::A)];
    }
    if (key >= Key::Num0 && key <= Key::Num9) {
        static std::array<std::string, 10> digits = [] {
            std::array<std::string, 10> generated{};
            for (usize i = 0; i < generated.size(); ++i) {
                generated[i] = std::string(1, static_cast<char>('0' + i));
            }
            return generated;
        }();
        return digits[static_cast<usize>(key) - static_cast<usize>(Key::Num0)];
    }
    if (key >= Key::F1 && key <= Key::F12) {
        static std::array<std::string, 12> functionKeys = [] {
            std::array<std::string, 12> generated{};
            for (usize i = 0; i < generated.size(); ++i) {
                generated[i] = "F" + std::to_string(i + 1);
            }
            return generated;
        }();
        return functionKeys[static_cast<usize>(key) - static_cast<usize>(Key::F1)];
    }
    switch (key) {
        case Key::Backspace: return "Backspace";
        case Key::Delete: return "Delete";
        case Key::Insert: return "Insert";
        case Key::Home: return "Home";
        case Key::End: return "End";
        case Key::PageUp: return "PageUp";
        case Key::PageDown: return "PageDown";
        case Key::LeftSuper: return "LeftSuper";
        case Key::RightSuper: return "RightSuper";
        case Key::Escape: return "Escape";
        case Key::Enter: return "Enter";
        case Key::Space: return "Space";
        case Key::Tab: return "Tab";
        case Key::Up: return "Up";
        case Key::Down: return "Down";
        case Key::Left: return "Left";
        case Key::Right: return "Right";
        case Key::LeftShift: return "LeftShift";
        case Key::RightShift: return "RightShift";
        case Key::LeftCtrl: return "LeftCtrl";
        case Key::RightCtrl: return "RightCtrl";
        case Key::LeftAlt: return "LeftAlt";
        case Key::RightAlt: return "RightAlt";
        default: return "Unknown";
    }
}

} // namespace l3d::platform
