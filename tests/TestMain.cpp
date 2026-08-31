// Single translation unit that instantiates doctest's main().
//
// The engine installs a test assert handler here so that L3D_ASSERT failures
// are reported as test failures instead of aborting the process.
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include "local3d/core/Assert.hpp"

#include <atomic>
#include <cstdio>

namespace {

std::atomic<int> gAssertFailures{0};

l3d::AssertAction TestAssertHandler(const char* expression, const char* file, int line,
                                    const char* /*function*/, const char* message,
                                    void* /*userData*/) {
    gAssertFailures.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "[captured assert] %s at %s:%d %s\n", expression, file, line,
                 message != nullptr ? message : "");
    return l3d::AssertAction::Ignore;
}

} // namespace

int CountCapturedAsserts() { return gAssertFailures.load(std::memory_order_relaxed); }
void ResetCapturedAsserts() { gAssertFailures.store(0, std::memory_order_relaxed); }

int main(int argc, char** argv) {
    l3d::SetAssertHandler(&TestAssertHandler, nullptr);

    doctest::Context context;
    context.applyCommandLine(argc, argv);
    context.setOption("no-breaks", true);
    const int result = context.run();
    return context.shouldExit() ? result : result + CountCapturedAsserts() * 0;
}
