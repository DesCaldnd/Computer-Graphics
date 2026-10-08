#pragma once

#include <oxwald/core/services.hpp>
#include <oxwald/core/time.hpp>
#include <oxwald/core/types.hpp>

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ox {

class World;

// Frame phases in execution order. FixedUpdate runs 0..N times per frame at the fixed rate (physics/logic);
// PostUpdate propagates transforms; Extract builds the render snapshot.
enum class SystemPhase : u8 { PreUpdate, FixedUpdate, Update, PostUpdate, Extract, Count };
inline constexpr usize kSystemPhaseCount = static_cast<usize>(SystemPhase::Count);
[[nodiscard]] std::string_view phaseName(SystemPhase phase);

struct SystemContext {
    World& world;
    Services& services;
    f32 dt = 0.0f;      // variable frame delta (FixedUpdate: equals fixedDt)
    f32 fixedDt = 0.0f; // fixed simulation step
    f32 alpha = 0.0f;   // interpolation factor between the last two fixed steps (for rendering)
    u64 frame = 0;
    u32 fixedStep = 0; // index of the fixed step within this frame
    SystemPhase phase = SystemPhase::Update;
    bool playing = false;
};

class ISystem {
public:
    virtual ~ISystem() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual SystemPhase phase() const = 0;
    // Lower runs first within a phase (ties keep registration order).
    [[nodiscard]] virtual i32 order() const { return 0; }
    // Skipped while the scheduler is not in play mode (editor edit mode).
    [[nodiscard]] virtual bool playModeOnly() const { return false; }
    virtual void onAttach(World&, Services&) {}
    virtual void onDetach(World&, Services&) {}
    virtual void update(SystemContext& ctx) = 0;
};

// Propagates hierarchy transforms (PostUpdate, order -1000 so later PostUpdate systems see fresh matrices).
class TransformSystem final : public ISystem {
public:
    [[nodiscard]] std::string_view name() const override { return "Transform"; }
    [[nodiscard]] SystemPhase phase() const override { return SystemPhase::PostUpdate; }
    [[nodiscard]] i32 order() const override { return -1000; }
    void update(SystemContext& ctx) override;
};

class SystemScheduler {
public:
    explicit SystemScheduler(f64 fixedDt = 1.0 / 60.0, u32 maxFixedSteps = 8);
    ~SystemScheduler();
    SystemScheduler(const SystemScheduler&) = delete;
    SystemScheduler& operator=(const SystemScheduler&) = delete;

    ISystem& add(std::unique_ptr<ISystem> system);
    template <class T, class... Args>
    T& emplace(Args&&... args) {
        return static_cast<T&>(add(std::make_unique<T>(std::forward<Args>(args)...)));
    }
    bool remove(std::string_view name);
    [[nodiscard]] ISystem* find(std::string_view name) const;
    void setEnabled(std::string_view name, bool enabled);
    [[nodiscard]] bool isEnabled(std::string_view name) const;
    // Systems of one phase in execution order.
    [[nodiscard]] std::vector<ISystem*> systems(SystemPhase phase) const;

    // Calls onAttach for systems added before and after this call.
    void attach(World& world, Services& services);
    void detach();

    void setPlaying(bool playing) { m_playing = playing; }
    [[nodiscard]] bool playing() const { return m_playing; }
    [[nodiscard]] FixedTimestep& fixedTimestep() { return m_fixed; }
    [[nodiscard]] u64 frame() const { return m_frame; }
    // Milliseconds spent in a system during the last tick.
    [[nodiscard]] f64 lastTimeMs(std::string_view name) const;

    // One frame: snapshot previous transforms, PreUpdate, FixedUpdate x N, Update, PostUpdate, Extract, then
    // destroy entities scheduled for destruction.
    void tick(World& world, Services& services, f64 frameDt);
    void runPhase(SystemPhase phase, SystemContext& ctx);

private:
    struct Entry {
        std::unique_ptr<ISystem> system;
        u64 sequence = 0;
        bool enabled = true;
        bool attached = false;
        f64 lastMs = 0.0;
    };
    void sort();
    Entry* entry(std::string_view name);
    const Entry* entry(std::string_view name) const;

    std::vector<Entry> m_entries;
    u64 m_sequence = 0;
    FixedTimestep m_fixed;
    u64 m_frame = 0;
    bool m_playing = false;
    World* m_world = nullptr;
    Services* m_services = nullptr;
};

} // namespace ox
