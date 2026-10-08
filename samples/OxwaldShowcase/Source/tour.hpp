#pragma once

#include "showcase.hpp"

#include <oxwald/async/scheduler.hpp>

#include <atomic>
#include <memory>

namespace ox::showcase {

// Guided tour (`-- --tour`) and photo mode (`-- --photo`).
//
// Tour: the hub and then every station in order. Each level gets one C++ coroutine (level changes cancel all
// coroutines, so the tour is a chain of per-level coroutines): wait until the station has loaded its assets and the
// temporal effects converged, show the caption, optionally write <shots>/<id>.png, fly the camera from the
// station's "TourCam.A" to "TourCam.B", then request the next level. After the last station: "Tour finished" and quit
// (headless or --quit).
// Photo: the camera is placed at "TourCam.A" of whatever scene is loaded and kept there (player --screenshot).
class Tour {
public:
    explicit Tour(ShowcaseModule& module);
    ~Tour();

    void preUpdate(const FrameTime& time);
    void onWorldChanged(World& world);

    [[nodiscard]] bool finished() const { return m_finished; }
    [[nodiscard]] i32 currentIndex() const { return m_index; }

private:
    Task<> stationShow(World* world, i32 index);
    [[nodiscard]] i32 nextIndex(i32 from) const;
    void requestStation(i32 index);
    [[nodiscard]] u32 pendingAssetLoads() const;

    ShowcaseModule& m_module;
    std::vector<StationInfo> m_order; // hub + stations
    bool m_started = false;
    bool m_finished = false;
    i32 m_index = -1;
    std::shared_ptr<std::atomic<int>> m_shotState; // -1 pending, 0 failed, 1 written
};

} // namespace ox::showcase
