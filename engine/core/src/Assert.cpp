#include "local3d/core/Assert.hpp"

#include "local3d/core/Log.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace l3d {
namespace {

/// Single global slot, installed at start-up.  A relaxed atomic makes the
/// pointer swap visible to other threads without a lock on the assert path.
std::atomic<AssertHandlerFn> gAssertHandler{nullptr};
std::atomic<void*> gAssertUserData{nullptr};

AssertAction DefaultAssertHandler(const char* expression, const char* file, int line,
                                  const char* function, const char* message,
                                  void* /*userData*/) {
    std::fprintf(stderr, "\n[Local3D] ASSERTION FAILED\n  expression: %s\n  location:   %s:%d\n  function:   %s\n",
                 expression != nullptr ? expression : "(null)", file != nullptr ? file : "?", line,
                 function != nullptr ? function : "?");
    if (message != nullptr && message[0] != '\0') {
        std::fprintf(stderr, "  message:    %s\n", message);
    }
    std::fflush(stderr);
#if defined(L3D_DEBUG) && L3D_DEBUG
    return AssertAction::Break;
#else
    return AssertAction::Abort;
#endif
}

} // namespace

AssertHandlerFn SetAssertHandler(AssertHandlerFn handler, void* userData) noexcept {
    AssertHandlerFn previous = gAssertHandler.load(std::memory_order_relaxed);
    gAssertUserData.store(userData, std::memory_order_relaxed);
    gAssertHandler.store(handler, std::memory_order_relaxed);
    return previous;
}

void ReportAssertFailure(const char* expression, const char* file, int line, const char* function,
                         const char* message) noexcept {
    const AssertHandlerFn handler = gAssertHandler.load(std::memory_order_relaxed);
    void* userData = gAssertUserData.load(std::memory_order_relaxed);

    const AssertAction action = (handler != nullptr)
                                    ? handler(expression, file, line, function, message, userData)
                                    : DefaultAssertHandler(expression, file, line, function,
                                                           message, nullptr);

    switch (action) {
        case AssertAction::Ignore:
            return;
        case AssertAction::Break:
            L3D_DEBUG_BREAK();
            return;
        case AssertAction::Abort:
            break;
    }
    // Make sure the failure is in the log before the process disappears.
    LogWrite(LogLevel::Fatal, LogCategory::Core, "Assertion '{}' failed at {}:{}",
             expression != nullptr ? expression : "(null)", file != nullptr ? file : "?", line);
    std::abort();
}

void FatalError(const char* file, int line, const char* function, const char* message) noexcept {
    std::fprintf(stderr, "\n[Local3D] FATAL: %s\n  at %s:%d (%s)\n",
                 message != nullptr ? message : "(no message)", file != nullptr ? file : "?", line,
                 function != nullptr ? function : "?");
    std::fflush(stderr);
    std::abort();
}

} // namespace l3d
