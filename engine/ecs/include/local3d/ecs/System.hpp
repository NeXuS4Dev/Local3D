#pragma once
/// @file System.hpp
/// @brief System interface and the scheduler that runs them each frame.
///
/// A system is a stateless-ish function over the world.  Keeping them small and
/// phase ordered (instead of an arbitrary dependency graph) keeps the frame
/// predictable, which matters more than perfect scheduling at this scale.

#include "local3d/core/Common.hpp"
#include "local3d/core/Profiler.hpp"
#include "local3d/ecs/World.hpp"

#include <memory>
#include <string>
#include <vector>

namespace l3d::ecs {

/// Where in the frame a system runs.
enum class SystemPhase : u8 {
    PreUpdate = 0,  ///< Input, scripting, gameplay intent.
    Update,         ///< Gameplay simulation.
    FixedUpdate,    ///< Physics and other fixed rate steps.
    PostUpdate,     ///< Hierarchy, bounds, visibility prep.
    PreRender,      ///< Culling, render data upload.
    Count,
};

[[nodiscard]] std::string_view SystemPhaseName(SystemPhase phase) noexcept;

/// Everything a system may need for one tick.  Passed by reference; systems do
/// not own any of it.
struct SystemContext {
    World* world = nullptr;
    f32 deltaTime = 0.0f;      ///< Seconds since the previous frame.
    f32 fixedDeltaTime = 0.0f; ///< Physics step size (only meaningful in FixedUpdate).
    u64 frameIndex = 0;
    bool isEditorMode = false; ///< True when the editor (not a cooked game) is driving.
    void* userData = nullptr;  ///< Application specific context (game state, UI, ...).
};

class ISystem {
public:
    virtual ~ISystem() = default;

    [[nodiscard]] virtual const char* Name() const noexcept = 0;
    [[nodiscard]] virtual SystemPhase Phase() const noexcept { return SystemPhase::Update; }

    /// Higher priority runs earlier within a phase.
    [[nodiscard]] virtual i32 Priority() const noexcept { return 0; }

    /// Systems can be toggled (editor-only systems, debug visualisation).
    [[nodiscard]] virtual bool IsEnabled() const noexcept { return enabled_; }
    virtual void SetEnabled(bool enabled) noexcept { enabled_ = enabled; }

    virtual void Execute(SystemContext& context) = 0;

private:
    bool enabled_ = true;
};

/// Convenience base for systems implemented as a lambda.
class FunctionSystem final : public ISystem {
public:
    using Body = std::function<void(SystemContext&)>;

    FunctionSystem(std::string name, SystemPhase phase, Body body, i32 priority = 0)
        : name_(std::move(name)), phase_(phase), body_(std::move(body)), priority_(priority) {}

    [[nodiscard]] const char* Name() const noexcept override { return name_.c_str(); }
    [[nodiscard]] SystemPhase Phase() const noexcept override { return phase_; }
    [[nodiscard]] i32 Priority() const noexcept override { return priority_; }
    void Execute(SystemContext& context) override {
        if (body_) {
            body_(context);
        }
    }

private:
    std::string name_;
    SystemPhase phase_;
    Body body_;
    i32 priority_;
};

/// Runs registered systems in (phase, priority, registration order).
class SystemScheduler {
public:
    /// Ownership transfers to the scheduler.
    void Register(std::unique_ptr<ISystem> system);

    template <typename T, typename... Args>
    T& RegisterSystem(Args&&... args) {
        auto system = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = system.get();
        Register(std::move(system));
        return *raw;
    }

    void ExecutePhase(SystemPhase phase, SystemContext& context);
    void ExecuteAll(SystemContext& context);

    [[nodiscard]] ISystem* Find(std::string_view name) const noexcept;
    [[nodiscard]] usize SystemCount() const noexcept { return systems_.size(); }
    void RemoveAll() { systems_.clear(); }

    /// Per system timing from the last frame, for the editor profiler panel.
    struct Timing {
        std::string name;
        SystemPhase phase = SystemPhase::Update;
        f64 milliseconds = 0.0;
    };
    [[nodiscard]] const std::vector<Timing>& LastFrameTimings() const noexcept { return timings_; }

private:
    std::vector<std::unique_ptr<ISystem>> systems_;
    std::vector<Timing> timings_;
};

} // namespace l3d::ecs
