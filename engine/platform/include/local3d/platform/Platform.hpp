#pragma once
/// @file Platform.hpp
/// @brief Window and event abstraction.
///
/// Two backends:
///  * Headless (always available): a fully functional in-memory window with
///    injectable events.  This is what CI, tools, automated tests and the
///    editor's -headless mode use, and it is why the engine can be exercised
///    end to end without a display server.
///  * SDL3 (when found at configure time): real windows and real input.
///
/// Threading: all platform calls happen on the main thread.

#include "local3d/core/Result.hpp"
#include "local3d/platform/Events.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace l3d::platform {

enum class BackendType : u8 {
    Headless = 0,
    Sdl3,
};

[[nodiscard]] std::string_view BackendTypeName(BackendType type) noexcept;

struct WindowDesc {
    std::string title = "Local3D";
    u32 width = 1280;
    u32 height = 720;
    bool resizable = true;
    bool fullscreen = false;
    bool visible = true;
    /// Request a graphics surface.  The headless backend records the request so
    /// the RHI can be tested without a real surface.
    bool enableGraphics = true;
};

class IWindow {
public:
    virtual ~IWindow() = default;

    [[nodiscard]] virtual std::string_view Title() const noexcept = 0;
    [[nodiscard]] virtual u32 Width() const noexcept = 0;
    [[nodiscard]] virtual u32 Height() const noexcept = 0;
    [[nodiscard]] virtual math::Vec2 Size() const noexcept {
        return {static_cast<f32>(Width()), static_cast<f32>(Height())};
    }
    [[nodiscard]] virtual f32 AspectRatio() const noexcept {
        return Height() > 0 ? static_cast<f32>(Width()) / static_cast<f32>(Height()) : 1.0f;
    }
    [[nodiscard]] virtual bool ShouldClose() const noexcept = 0;
    virtual void RequestClose() = 0;
    virtual void SetTitle(std::string_view title) = 0;
    virtual void Resize(u32 width, u32 height) = 0;

    /// Opaque handle handed to the RHI to create a surface.  Null for headless.
    [[nodiscard]] virtual void* NativeHandle() const noexcept = 0;

    /// Present the back buffer (no-op without a real display).
    virtual void SwapBuffers() = 0;
};

class HeadlessPlatform;

/// In-memory window used by the headless backend.  It behaves like a real window
/// for everything except pixels: it has a size, a title, a close flag and a swap
/// counter, and it reports resizes and close requests as events.  Public so that
/// tests and headless tools can drive it directly.
class HeadlessWindow final : public IWindow {
public:
    HeadlessWindow(const WindowDesc& desc, HeadlessPlatform* owner);
    ~HeadlessWindow() override;

    [[nodiscard]] std::string_view Title() const noexcept override;
    [[nodiscard]] u32 Width() const noexcept override;
    [[nodiscard]] u32 Height() const noexcept override;
    [[nodiscard]] bool ShouldClose() const noexcept override;
    void RequestClose() override;
    void SetTitle(std::string_view title) override;
    void Resize(u32 width, u32 height) override;
    [[nodiscard]] void* NativeHandle() const noexcept override;
    void SwapBuffers() override;

    /// Number of times the (imaginary) back buffer was presented.
    [[nodiscard]] u64 SwapCount() const noexcept { return swapCount_; }
    [[nodiscard]] const WindowDesc& Desc() const noexcept { return desc_; }

private:
    WindowDesc desc_;
    HeadlessPlatform* owner_;
    bool shouldClose_ = false;
    u64 swapCount_ = 0;
};

class IPlatformBackend {
public:
    virtual ~IPlatformBackend() = default;

    [[nodiscard]] virtual BackendType Type() const noexcept = 0;
    [[nodiscard]] virtual std::string_view Name() const noexcept = 0;

    [[nodiscard]] virtual Result<std::unique_ptr<IWindow>> CreateWindow(const WindowDesc& desc) = 0;
    void DestroyWindow(std::unique_ptr<IWindow> window) { window.reset(); }

    /// Pump the OS event queue into `outEvents`.
    virtual void PollEvents(std::vector<Event>& outEvents) = 0;

    /// Milliseconds since backend creation.
    [[nodiscard]] virtual u64 ElapsedMs() const noexcept = 0;

    /// Sleep for approximately `ms` (used to cap the frame rate).
    virtual void Sleep(u32 ms) = 0;
};

/// Create the requested backend, falling back to headless when unavailable.
/// `outFallback` reports whether the fallback was used.
[[nodiscard]] Result<std::unique_ptr<IPlatformBackend>> CreatePlatformBackend(
    BackendType preferred, bool* usedFallback = nullptr);

/// Headless backend with event injection, exposed for tests and tools.
/// Windows created here are HeadlessWindow instances.
class HeadlessPlatform final : public IPlatformBackend {
public:
    HeadlessPlatform();
    ~HeadlessPlatform() override;

    [[nodiscard]] BackendType Type() const noexcept override { return BackendType::Headless; }
    [[nodiscard]] std::string_view Name() const noexcept override { return "headless"; }
    [[nodiscard]] Result<std::unique_ptr<IWindow>> CreateWindow(const WindowDesc& desc) override;
    void PollEvents(std::vector<Event>& outEvents) override;
    [[nodiscard]] u64 ElapsedMs() const noexcept override;
    void Sleep(u32 ms) override;

    /// Queue an event to be delivered by the next PollEvents call.
    void InjectEvent(Event event);

    /// Queue a full frame's worth of events (key down + up, mouse move, ...).
    void InjectEvents(std::vector<Event> events);

    [[nodiscard]] usize PendingEventCount() const noexcept { return pendingEvents_.size(); }

    /// Number of windows created and not yet destroyed.
    [[nodiscard]] usize WindowCount() const noexcept { return windowCount_; }

    /// Internal bookkeeping used by the window implementation.  Not part of the
    /// public contract; called only from this module's translation unit.
    void NotifyWindowCreated() noexcept { ++windowCount_; }
    void NotifyWindowDestroyed() noexcept {
        if (windowCount_ > 0) {
            --windowCount_;
        }
    }

private:
    std::vector<Event> pendingEvents_;
    usize windowCount_ = 0;
    u64 startNs_ = 0;
};

} // namespace l3d::platform
