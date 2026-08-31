#include "local3d/ecs/System.hpp"

#include "local3d/core/Time.hpp"

#include <algorithm>

namespace l3d::ecs {

std::string_view SystemPhaseName(SystemPhase phase) noexcept {
    switch (phase) {
        case SystemPhase::PreUpdate: return "PreUpdate";
        case SystemPhase::Update: return "Update";
        case SystemPhase::FixedUpdate: return "FixedUpdate";
        case SystemPhase::PostUpdate: return "PostUpdate";
        case SystemPhase::PreRender: return "PreRender";
        case SystemPhase::Count: return "Count";
    }
    return "Unknown";
}

void SystemScheduler::Register(std::unique_ptr<ISystem> system) {
    if (system == nullptr) {
        return;
    }
    systems_.push_back(std::move(system));
    // Stable sort keeps registration order for equal priorities.
    std::stable_sort(systems_.begin(), systems_.end(),
                     [](const std::unique_ptr<ISystem>& a, const std::unique_ptr<ISystem>& b) {
                         if (a->Phase() != b->Phase()) {
                             return a->Phase() < b->Phase();
                         }
                         return a->Priority() > b->Priority();
                     });
}

void SystemScheduler::ExecutePhase(SystemPhase phase, SystemContext& context) {
    timings_.clear();
    for (const auto& system : systems_) {
        if (system->Phase() != phase || !system->IsEnabled()) {
            continue;
        }
        const u64 startNs = Clock::NowNs();
        {
            L3D_PROFILE_SCOPE(system->Name());
            system->Execute(context);
        }
        const u64 elapsed = Clock::NowNs() - startNs;
        timings_.push_back(Timing{system->Name(), phase, static_cast<f64>(elapsed) / 1.0e6});
    }
}

void SystemScheduler::ExecuteAll(SystemContext& context) {
    for (u8 phase = 0; phase < static_cast<u8>(SystemPhase::Count); ++phase) {
        ExecutePhase(static_cast<SystemPhase>(phase), context);
    }
}

ISystem* SystemScheduler::Find(std::string_view name) const noexcept {
    for (const auto& system : systems_) {
        if (name == system->Name()) {
            return system.get();
        }
    }
    return nullptr;
}

} // namespace l3d::ecs
