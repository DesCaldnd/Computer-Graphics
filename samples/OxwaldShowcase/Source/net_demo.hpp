#pragma once

#include "showcase.hpp"

#include <oxwald/core/uuid.hpp>

#include <memory>
#include <string>
#include <unordered_map>

namespace ox::showcase {

struct NetDemoStats {
    bool running = false;
    bool connected = false;
    f32 rttMs = 0.f;
    f32 sendKBps = 0.f;    // server -> bot
    f32 receiveKBps = 0.f; // bot -> server
    u32 snapshotBytes = 0;
    u32 objects = 0;       // replicas on the bot client
    f32 interpolationDelayMs = 0.f;
    f32 latencyMs = 0.f;   // simulated one-way latency
    f32 lossPercent = 0.f; // simulated loss
    f32 tickRate = 0.f;
    f32 errorCm = 0.f;     // distance server avatar <-> its interpolated replica (latency + interpolation delay)
};

// Listen-server demo of the "Сеть" station (activated by an entity named "NetDemo" in play mode).
// Server = the station world (the engine's gameplay NetworkRuntime). Client = an in-process "bot" with its own
// World/Services/SystemScheduler (gameplay systems with networking only), connected through net::MemoryNetwork with
// simulated latency/loss. Every replica the bot receives is mirrored into the station world as a translucent ghost
// (entity "Ghost.<name>") so the interpolated remote view is visible next to the authoritative objects.
class NetDemo {
public:
    explicit NetDemo(ShowcaseModule& module);
    ~NetDemo();

    void onWorldChanged(World& world);
    void preUpdate(const FrameTime& time);
    void stop();

    void setConditions(f32 latencyMs, f32 lossPercent, f32 jitterMs);
    [[nodiscard]] NetDemoStats stats() const;

private:
    struct Client;
    bool start(World& world);
    void mirrorGhosts(World& world);

    ShowcaseModule& m_module;
    World* m_world = nullptr;
    bool m_pending = false;
    u32 m_waitFrames = 0;
    std::unique_ptr<Client> m_client;
    std::unordered_map<u32, Uuid> m_ghosts; // replica net id -> ghost entity in the station world
    f32 m_latencyMs = 60.f;
    f32 m_lossPercent = 5.f;
    f32 m_jitterMs = 10.f;
    f32 m_errorCm = 0.f;
};

} // namespace ox::showcase
