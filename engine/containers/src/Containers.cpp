// Translation unit anchor for the header-only Containers module.  Keeping one
// .cpp means the library always has something to compile and a place to put
// future out-of-line definitions without touching every CMakeLists.txt.
#include "local3d/containers/Containers.hpp"

namespace l3d {
namespace {
[[maybe_unused]] constexpr int kContainersModuleAnchor = 1;
}
} // namespace l3d
