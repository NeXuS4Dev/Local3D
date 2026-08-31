#pragma once
/// @file Log.hpp
/// @brief Levelled, categorised logging.
///
/// Design notes (docs/architecture/logging.md):
///  * Formatting happens on the calling thread into a stack buffer, so a
///    filtered-out log call costs a level comparison and nothing else.
///  * Sinks are invoked under a mutex; sinks must be cheap and must not log.
///  * The logger never throws and never allocates on the fast path.

#include "local3d/core/Common.hpp"
#include "local3d/core/Enum.hpp"
#include "local3d/core/Format.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string_view>
#include <vector>

namespace l3d {

enum class LogLevel : u8 {
    Trace = 0,
    Debug,
    Info,
    Warning,
    Error,
    Fatal,
    Off,
};

/// One category per engine subsystem.  Categories let a developer turn on
/// renderer spam without drowning in core noise.
enum class LogCategory : u8 {
    Core = 0,
    Memory,
    Containers,
    Math,
    Reflection,
    Serialization,
    Platform,
    Ecs,
    Assets,
    Rhi,
    RenderGraph,
    Renderer,
    Scene,
    Physics,
    Animation,
    Audio,
    Input,
    Scripting,
    Networking,
    Jobs,
    Runtime,
    Editor,
    Tools,
    Count,
};

[[nodiscard]] std::string_view LogLevelName(LogLevel level) noexcept;
[[nodiscard]] std::string_view LogCategoryName(LogCategory category) noexcept;

/// A fully formatted log record handed to sinks.
struct LogMessage {
    LogLevel level = LogLevel::Info;
    LogCategory category = LogCategory::Core;
    std::string_view text;
    u64 timestampNs = 0;
    u32 threadId = 0;
};

using LogSinkFn = std::function<void(const LogMessage&)>;

class Logger {
public:
    /// Process wide logger.  This is the one piece of global mutable state the
    /// engine allows, because logging must be reachable from static
    /// destructors and crash handlers where no engine object is available.
    [[nodiscard]] static Logger& Instance() noexcept;

    /// Set the global minimum level.
    void SetLevel(LogLevel level) noexcept;
    [[nodiscard]] LogLevel GetLevel() const noexcept { return level_.load(std::memory_order_relaxed); }

    /// Per-category override (a level of Off means "use the global level").
    void SetCategoryLevel(LogCategory category, LogLevel level) noexcept;
    [[nodiscard]] LogLevel GetCategoryLevel(LogCategory category) const noexcept;

    /// Register a sink.  Returns a handle usable with RemoveSink.
    u64 AddSink(LogSinkFn sink);
    void RemoveSink(u64 handle);
    void ClearSinks();

    [[nodiscard]] bool ShouldLog(LogLevel level, LogCategory category) const noexcept {
        if (level == LogLevel::Off) {
            return false;
        }
        if (level < level_.load(std::memory_order_relaxed)) {
            return false;
        }
        // A category override of Off means "no override, use the global level".
        const LogLevel override =
            categoryLevels_[static_cast<usize>(category)].load(std::memory_order_relaxed);
        if (override == LogLevel::Off) {
            return true;
        }
        return level >= override;
    }

    /// Emit an already formatted message.  Called by the LogXxx helpers.
    void Write(LogLevel level, LogCategory category, std::string_view text);

private:
    Logger() noexcept;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    struct SinkEntry {
        u64 handle = 0;
        LogSinkFn fn;
    };

    std::atomic<LogLevel> level_{LogLevel::Info};
    std::array<std::atomic<LogLevel>, static_cast<usize>(LogCategory::Count)> categoryLevels_{};
    std::mutex mutex_;
    std::vector<SinkEntry> sinks_;
    u64 nextHandle_ = 1;
};

/// Attach the standard stderr sink (with ANSI colour when attached to a TTY).
u64 AddConsoleSink();

/// Attach a sink that appends to a file.  Returns 0 on failure.
u64 AddFileSink(std::string_view path);

/// Formatted logging entry point.
template <typename... Args>
void LogWrite(LogLevel level, LogCategory category, std::string_view format, const Args&... args) {
    auto& logger = Logger::Instance();
    if (!logger.ShouldLog(level, category)) {
        return;
    }
    char buffer[1024]{};
    fmt::FormatToBuffer(buffer, format, args...);
    logger.Write(level, category, std::string_view(buffer));
}

} // namespace l3d

#define L3D_LOG(cat, level, ...) ::l3d::LogWrite(level, cat, __VA_ARGS__)
#define L3D_LOG_TRACE(cat, ...) ::l3d::LogWrite(::l3d::LogLevel::Trace, cat, __VA_ARGS__)
#define L3D_LOG_DEBUG(cat, ...) ::l3d::LogWrite(::l3d::LogLevel::Debug, cat, __VA_ARGS__)
#define L3D_LOG_INFO(cat, ...) ::l3d::LogWrite(::l3d::LogLevel::Info, cat, __VA_ARGS__)
#define L3D_LOG_WARN(cat, ...) ::l3d::LogWrite(::l3d::LogLevel::Warning, cat, __VA_ARGS__)
#define L3D_LOG_ERROR(cat, ...) ::l3d::LogWrite(::l3d::LogLevel::Error, cat, __VA_ARGS__)
#define L3D_LOG_FATAL(cat, ...) ::l3d::LogWrite(::l3d::LogLevel::Fatal, cat, __VA_ARGS__)
