#pragma once
/// @file Assert.hpp
/// @brief Assertions with a pluggable handler.
///
/// Assertions are a *development* contract, not error handling: they express
/// invariants that must never be violated.  Recoverable failures use
/// l3d::Status / l3d::Expected instead (see Result.hpp).
///
/// The handler is pluggable so that the test-suite can capture assertion
/// failures instead of aborting the process, and so the editor can route them
/// into its console.

#include "local3d/core/Common.hpp"

namespace l3d {

/// What the engine does after reporting an assertion failure.
enum class AssertAction : u8 {
    Ignore,  ///< Log and continue.  Used by tests and by user overrides.
    Break,   ///< Trigger a debugger trap, then continue.
    Abort,   ///< Terminate the process.  Default in Debug/Development.
};

/// Signature of an assertion handler.  Must not throw.
using AssertHandlerFn = AssertAction (*)(const char* expression, const char* file, int line,
                                         const char* function, const char* message, void* userData);

/// Install a custom handler.  Returns the previous one.  Thread safety: this is
/// a single global slot; install handlers at start-up, not per frame.
AssertHandlerFn SetAssertHandler(AssertHandlerFn handler, void* userData) noexcept;

/// Report a failed assertion through the installed handler and act on the
/// result.  Never returns when the action is Abort.
void ReportAssertFailure(const char* expression, const char* file, int line, const char* function,
                         const char* message) noexcept;

/// Report an unconditional fatal error and terminate.
[[noreturn]] void FatalError(const char* file, int line, const char* function,
                             const char* message) noexcept;

} // namespace l3d

#if defined(L3D_ASSERTS_ENABLED) && L3D_ASSERTS_ENABLED
#    define L3D_ASSERT(cond)                                                                       \
        do {                                                                                       \
            if (L3D_UNLIKELY(!(cond))) {                                                           \
                ::l3d::ReportAssertFailure(#cond, __FILE__, __LINE__, __func__, "");                \
            }                                                                                      \
        } while (false)

#    define L3D_ASSERT_MSG(cond, msg)                                                              \
        do {                                                                                       \
            if (L3D_UNLIKELY(!(cond))) {                                                           \
                ::l3d::ReportAssertFailure(#cond, __FILE__, __LINE__, __func__, (msg));             \
            }                                                                                      \
        } while (false)
#else
#    define L3D_ASSERT(cond) L3D_ASSUME(cond)
#    define L3D_ASSERT_MSG(cond, msg) L3D_ASSUME(cond)
#endif

/// Always evaluated, even in Release.  For invariants whose violation would
/// corrupt memory if execution continued.
#define L3D_ASSERT_ALWAYS(cond)                                                                    \
    do {                                                                                           \
        if (L3D_UNLIKELY(!(cond))) {                                                               \
            ::l3d::ReportAssertFailure(#cond, __FILE__, __LINE__, __func__, "");                    \
        }                                                                                          \
    } while (false)

#define L3D_FATAL(msg) ::l3d::FatalError(__FILE__, __LINE__, __func__, (msg))

/// Mark unreachable code while still checking in development builds.
#define L3D_UNREACHABLE_MSG(msg)                                                                   \
    do {                                                                                           \
        ::l3d::ReportAssertFailure("unreachable", __FILE__, __LINE__, __func__, (msg));             \
        L3D_UNREACHABLE();                                                                         \
    } while (false)
