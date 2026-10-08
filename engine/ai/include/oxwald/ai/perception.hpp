#pragma once

#include <oxwald/ai/nav_types.hpp>

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ox::ai {

using TeamId = u8;
enum class Attitude : u8 { Hostile, Neutral, Friendly };

// Team → team attitude table. Default: same team friendly, different teams hostile, team 0 neutral to all.
class TeamAttitudes {
public:
    TeamAttitudes();
    void set(TeamId a, TeamId b, Attitude attitude, bool symmetric = true);
    [[nodiscard]] Attitude get(TeamId observer, TeamId target) const;

private:
    std::array<std::array<Attitude, 256>, 256> m_table;
};

enum class Sense : u8 { Sight, Hearing };

struct SightConfig {
    bool enabled = true;
    f32 range = 20.f;              // main cone range
    f32 fovDegrees = 90.f;         // full angle of the main cone
    f32 peripheralFovDegrees = 160.f;
    f32 peripheralRange = 6.f;     // peripheral vision only notices close targets
    f32 loseSightRange = 25.f;     // once seen, targets stay visible up to this range (hysteresis)
    f32 eyeHeight = 1.7f;
    f32 targetHeight = 1.f;        // aim point above the target's position for the LOS test
    f32 forgetAfter = 8.f;         // seconds a lost target is remembered (last known position)
};

struct HearingConfig {
    bool enabled = true;
    f32 rangeMultiplier = 1.f;     // scales every noise radius for this listener
    f32 threshold = 0.05f;         // minimum perceived loudness
    bool useOcclusion = true;      // walls (line-of-sight callback) attenuate noises
    f32 occludedFactor = 0.4f;
    f32 forgetAfter = 5.f;
};

struct PerceptionListenerDesc {
    glm::vec3 position{0.f};
    glm::vec3 forward{0.f, 0.f, -1.f};
    TeamId team = 1;
    u64 selfId = 0; // the listener's own source id (never perceives itself)
    SightConfig sight;
    HearingConfig hearing;
    // Which attitudes are reported (e.g. only hostiles).
    bool detectHostile = true, detectNeutral = true, detectFriendly = false;
};

struct PerceptionSource {
    u64 id = 0;
    glm::vec3 position{0.f};
    TeamId team = 0;
    bool visible = true; // e.g. false while cloaked
};

struct NoiseEvent {
    glm::vec3 position{0.f};
    f32 loudness = 1.f;    // 0..1 at the source
    f32 radius = 10.f;     // audible distance at loudness 1
    u64 instigator = 0;    // source id (0 = anonymous noise)
    TeamId team = 0;
    std::string tag;       // "footstep", "gunshot", ...
};

// What a listener knows about a stimulus source.
struct PerceivedStimulus {
    u64 sourceId = 0;
    Sense sense = Sense::Sight;
    glm::vec3 lastKnownPosition{0.f};
    f32 age = 0.f;              // seconds since last sensed (0 while currently sensed)
    f32 strength = 0.f;         // 0..1, decays with age
    bool currentlySensed = false;
    Attitude attitude = Attitude::Neutral;
    TeamId team = 0;
    std::string tag;
};

using PerceptionListenerId = u32;

// Sight + hearing + memory for AI agents. CPU-only: the world supplies positions, a line-of-sight callback
// (physics raycast) and noise events; query results per listener after update().
class PerceptionSystem {
public:
    // Returns true when the segment is blocked.
    using RaycastFn = std::function<bool(const glm::vec3& from, const glm::vec3& to)>;
    using EventFn = std::function<void(PerceptionListenerId, const PerceivedStimulus&, bool gained)>;

    void setRaycast(RaycastFn fn) { m_raycast = std::move(fn); }
    void setEventCallback(EventFn fn) { m_onEvent = std::move(fn); }
    TeamAttitudes& attitudes() { return m_attitudes; }

    PerceptionListenerId addListener(const PerceptionListenerDesc& desc);
    void removeListener(PerceptionListenerId id);
    void setListenerTransform(PerceptionListenerId id, const glm::vec3& position, const glm::vec3& forward);
    [[nodiscard]] PerceptionListenerDesc* listener(PerceptionListenerId id);

    void setSource(const PerceptionSource& source); // add or update
    void removeSource(u64 id);

    void reportNoise(const NoiseEvent& noise); // processed on next update

    void update(f32 dt);

    [[nodiscard]] const std::vector<PerceivedStimulus>& perceived(PerceptionListenerId id) const;
    [[nodiscard]] std::optional<PerceivedStimulus> knowledgeOf(PerceptionListenerId id, u64 sourceId, Sense sense) const;
    [[nodiscard]] bool canSee(PerceptionListenerId id, u64 sourceId) const;
    // Highest-strength currently sensed hostile (sight preferred), if any.
    [[nodiscard]] std::optional<PerceivedStimulus> bestHostile(PerceptionListenerId id) const;

    // Sight cones (green main, dark green peripheral), memory markers (yellow = last known positions).
    void debugDraw(PerceptionListenerId id, const DebugLineFn& line) const;

private:
    struct ListenerState {
        PerceptionListenerDesc desc;
        std::vector<PerceivedStimulus> memory;
    };
    bool detects(const ListenerState& l, Attitude a) const;
    PerceivedStimulus* findMemory(ListenerState& l, u64 sourceId, Sense sense);

    std::unordered_map<PerceptionListenerId, ListenerState> m_listeners;
    std::unordered_map<u64, PerceptionSource> m_sources;
    std::vector<NoiseEvent> m_pendingNoises;
    PerceptionListenerId m_nextListener = 1;
    RaycastFn m_raycast;
    EventFn m_onEvent;
    TeamAttitudes m_attitudes;
    u64 m_anonymousNoiseId = 1ull << 63;
};

} // namespace ox::ai
