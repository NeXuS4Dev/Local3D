#include "null/NullDevice.hpp"

#include "local3d/rhi/RhiDevice.hpp"

namespace l3d::rhi::null {
namespace {

[[nodiscard]] Result<std::unique_ptr<IDevice>> CreateNullDevice(const DeviceDesc& desc) {
    return std::unique_ptr<IDevice>(std::make_unique<NullDevice>(desc));
}

} // namespace

void RegisterNullBackend() {
    // Idempotent: CreateDevice calls this on every request so that the factory is
    // registered whether or not anything else in this translation unit was used.
    // A static initialiser would be dropped by the linker when the RHI is built as
    // a static library and nothing references this object file.
    static const bool registered = [] {
        RegisterDeviceFactory(BackendType::Null, &CreateNullDevice);
        return true;
    }();
    L3D_UNUSED(registered);
}

} // namespace l3d::rhi::null
