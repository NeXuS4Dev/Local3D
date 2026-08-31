#include "local3d/core/Log.hpp"

#include "local3d/core/ThreadId.hpp"
#include "local3d/core/Time.hpp"

#include <cstdio>
#include <memory>
#include <string>

#if !defined(_WIN32)
#    include <unistd.h>
#endif

namespace l3d {
namespace {

constexpr std::pair<LogLevel, std::string_view> kLevelNames[] = {
    {LogLevel::Trace, "TRACE"}, {LogLevel::Debug, "DEBUG"}, {LogLevel::Info, "INFO"},
    {LogLevel::Warning, "WARN"}, {LogLevel::Error, "ERROR"}, {LogLevel::Fatal, "FATAL"},
    {LogLevel::Off, "OFF"},
};

constexpr std::pair<LogCategory, std::string_view> kCategoryNames[] = {
    {LogCategory::Core, "core"},           {LogCategory::Memory, "memory"},
    {LogCategory::Containers, "container"}, {LogCategory::Math, "math"},
    {LogCategory::Reflection, "reflect"},  {LogCategory::Serialization, "serial"},
    {LogCategory::Platform, "platform"},   {LogCategory::Ecs, "ecs"},
    {LogCategory::Assets, "assets"},       {LogCategory::Rhi, "rhi"},
    {LogCategory::RenderGraph, "graph"},   {LogCategory::Renderer, "render"},
    {LogCategory::Scene, "scene"},         {LogCategory::Physics, "physics"},
    {LogCategory::Animation, "anim"},      {LogCategory::Audio, "audio"},
    {LogCategory::Input, "input"},         {LogCategory::Scripting, "script"},
    {LogCategory::Networking, "net"},      {LogCategory::Jobs, "jobs"},
    {LogCategory::Runtime, "runtime"},     {LogCategory::Editor, "editor"},
    {LogCategory::Tools, "tools"},
};

[[nodiscard]] const char* LevelColor(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
        case LogLevel::Debug:
            return "\x1b[90m"; // bright black
        case LogLevel::Info:
            return "\x1b[0m";
        case LogLevel::Warning:
            return "\x1b[33m"; // yellow
        case LogLevel::Error:
            return "\x1b[31m"; // red
        case LogLevel::Fatal:
            return "\x1b[1;31m"; // bold red
        case LogLevel::Off:
            return "\x1b[0m";
    }
    return "\x1b[0m";
}

} // namespace

std::string_view LogLevelName(LogLevel level) noexcept {
    return EnumName(level, kLevelNames, "?");
}

std::string_view LogCategoryName(LogCategory category) noexcept {
    return EnumName(category, kCategoryNames, "?");
}

Logger& Logger::Instance() noexcept {
    // Meyers singleton: initialised on first use, destroyed at exit.  Logging
    // must work before and after the engine object exists.
    static Logger instance;
    return instance;
}

Logger::Logger() noexcept {
    for (auto& level : categoryLevels_) {
        level.store(LogLevel::Off, std::memory_order_relaxed);
    }
}

Logger::~Logger() {
    std::lock_guard<std::mutex> lock(mutex_);
    sinks_.clear();
}

void Logger::SetLevel(LogLevel level) noexcept { level_.store(level, std::memory_order_relaxed); }

void Logger::SetCategoryLevel(LogCategory category, LogLevel level) noexcept {
    categoryLevels_[static_cast<usize>(category)].store(level, std::memory_order_relaxed);
}

LogLevel Logger::GetCategoryLevel(LogCategory category) const noexcept {
    return categoryLevels_[static_cast<usize>(category)].load(std::memory_order_relaxed);
}

u64 Logger::AddSink(LogSinkFn sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    const u64 handle = nextHandle_++;
    sinks_.push_back(SinkEntry{handle, std::move(sink)});
    return handle;
}

void Logger::RemoveSink(u64 handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (usize i = 0; i < sinks_.size(); ++i) {
        if (sinks_[i].handle == handle) {
            sinks_.erase(sinks_.begin() + static_cast<isize>(i));
            return;
        }
    }
}

void Logger::ClearSinks() {
    std::lock_guard<std::mutex> lock(mutex_);
    sinks_.clear();
}

void Logger::Write(LogLevel level, LogCategory category, std::string_view text) {
    const LogMessage message{
        level, category, text, Clock::NowNs(), CurrentThreadId(),
    };

    std::lock_guard<std::mutex> lock(mutex_);
    // Iterate by index: a sink may add/remove sinks while we dispatch, which
    // invalidates iterators but not indices (removal only shrinks the vector).
    for (usize i = 0; i < sinks_.size(); ++i) {
        if (sinks_[i].fn) {
            sinks_[i].fn(message);
        }
    }
}

u64 AddConsoleSink() {
    return Logger::Instance().AddSink([](const LogMessage& message) {
        // Colour only when stderr is a terminal; CI logs stay clean.
        static const bool useColor = [] {
#if defined(_WIN32)
            return false;
#else
            return ::isatty(::fileno(stderr)) != 0;
#endif
        }();

        if (useColor) {
            std::fprintf(stderr, "%s%-5s\x1b[0m [%-8s] %.*s\n", LevelColor(message.level),
                         std::string(LogLevelName(message.level)).c_str(),
                         std::string(LogCategoryName(message.category)).c_str(),
                         static_cast<int>(message.text.size()), message.text.data());
        } else {
            std::fprintf(stderr, "%-5s [%-8s] %.*s\n",
                         std::string(LogLevelName(message.level)).c_str(),
                         std::string(LogCategoryName(message.category)).c_str(),
                         static_cast<int>(message.text.size()), message.text.data());
        }
        std::fflush(stderr);
    });
}

u64 AddFileSink(std::string_view path) {
    const std::string pathString(path);
    std::FILE* file = std::fopen(pathString.c_str(), "ab");
    if (file == nullptr) {
        return 0;
    }
    // Shared so the sink outlives the caller that installed it.
    std::shared_ptr<std::FILE> owned(file, [](std::FILE* f) {
        if (f != nullptr) {
            std::fflush(f);
            std::fclose(f);
        }
    });
    return Logger::Instance().AddSink([owned](const LogMessage& message) {
        std::fprintf(owned.get(), "[%12llu] %-5s [%-8s] %.*s\n",
                     static_cast<unsigned long long>(message.timestampNs),
                     std::string(LogLevelName(message.level)).c_str(),
                     std::string(LogCategoryName(message.category)).c_str(),
                     static_cast<int>(message.text.size()), message.text.data());
        std::fflush(owned.get());
    });
}

} // namespace l3d
