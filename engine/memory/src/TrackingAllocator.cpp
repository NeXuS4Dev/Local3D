#include "local3d/memory/TrackingAllocator.hpp"

#include "local3d/core/Log.hpp"

#include <algorithm>

namespace l3d {

TrackingAllocator::TrackingAllocator(IAllocator& upstream, const char* name)
    : upstream_(upstream), name_(name) {}

TrackingAllocator::~TrackingAllocator() {
    if (!live_.empty()) {
        L3D_LOG_ERROR(LogCategory::Memory, "TrackingAllocator '{}': {} allocations ({} bytes) leaked",
                      name_, live_.size(), stats_.allocatedBytes);
    }
}

void* TrackingAllocator::Allocate(usize size, usize alignment, AllocationTag tag) {
    void* ptr = upstream_.Allocate(size, alignment, tag);
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.totalAllocations++;
    if (ptr != nullptr) {
        live_[ptr] = Record{size, alignment, tag.name};
        stats_.allocatedBytes += size;
        stats_.allocationCount++;
        if (stats_.allocatedBytes > stats_.peakBytes) {
            stats_.peakBytes = stats_.allocatedBytes;
        }
    }
    return ptr;
}

void TrackingAllocator::Deallocate(void* ptr, usize size, usize alignment) {
    if (ptr == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.totalDeallocations++;
        const auto found = live_.find(ptr);
        if (found == live_.end()) {
            ++errors_;
            L3D_LOG_ERROR(LogCategory::Memory,
                          "TrackingAllocator '{}': freeing unknown or already freed pointer {}",
                          name_, ptr);
            return;
        }
        if (found->second.size != size) {
            ++errors_;
            L3D_LOG_ERROR(LogCategory::Memory,
                          "TrackingAllocator '{}': size mismatch on free ({} requested, {} live)",
                          name_, size, found->second.size);
        }
        stats_.allocatedBytes -= found->second.size;
        stats_.allocationCount--;
        live_.erase(found);
    }
    upstream_.Deallocate(ptr, size, alignment);
}

MemoryStats TrackingAllocator::Snapshot() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

std::vector<TrackingAllocator::TagReport> TrackingAllocator::ReportByTag() const {
    std::unordered_map<std::string, TagReport> grouped;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : live_) {
            TagReport& report = grouped[entry.second.tag];
            report.tag = entry.second.tag;
            report.bytes += entry.second.size;
            report.count += 1;
        }
    }
    std::vector<TagReport> reports;
    reports.reserve(grouped.size());
    for (auto& entry : grouped) {
        reports.push_back(std::move(entry.second));
    }
    std::sort(reports.begin(), reports.end(),
              [](const TagReport& a, const TagReport& b) { return a.bytes > b.bytes; });
    return reports;
}

bool TrackingAllocator::HasLeaks() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return !live_.empty();
}

u64 TrackingAllocator::ErrorCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return errors_;
}

} // namespace l3d
