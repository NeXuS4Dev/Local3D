#include "local3d/rhi/RhiResources.hpp"

#include "local3d/rhi/RhiDevice.hpp"

namespace l3d::rhi {

void GpuResourceDeleter::operator()(GpuResource* resource) const noexcept {
    if (resource != nullptr) {
        resource->Release();
    }
}

void GpuResource::Release() noexcept {
    // Deferred destruction: the device keeps this object alive until the frames
    // that may still reference it have completed.  See RhiResources.hpp.
    if (device_ != nullptr) {
        device_->ScheduleRelease(this);
    }
}

} // namespace l3d::rhi
