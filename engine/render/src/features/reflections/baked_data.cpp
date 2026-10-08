// Baked reflection probe / irradiance volume files: install on load (editor viewport, runtime renderer) and save
// after a bake (editor "Bake Probes").
#include <oxwald/core/log.hpp>
#include <oxwald/render/features/reflections/reflections.hpp>

namespace ox::render::reflections {

std::string bakedProbeFileName(const Uuid& probe) { return probe.toString() + ".oxcube"; }
std::string bakedVolumeFileName(const Uuid& volume) { return volume.toString() + ".oxirr"; }

usize installBakedData(Renderer& renderer, const RenderSnapshot& snapshot, const BakedFileReader& read,
                       std::unordered_set<Uuid>& attempted) {
    const auto* refl = snapshot.findExtension<ReflectionSnapshot>();
    if (!refl || !read) return 0;
    usize installed = 0;
    for (const SnapshotReflectionProbe& p : refl->probes) {
        if (!p.uuid.isValid() || p.probe.update != ReflectionProbeUpdate::Baked || !attempted.insert(p.uuid).second) continue;
        const std::string file = bakedProbeFileName(p.uuid);
        auto bytes = read(file);
        if (!bytes) continue; // not baked yet: captured at runtime instead
        auto cube = decodeOxCube(*bytes, file);
        if (!cube) {
            OX_LOG_WARN("render", "baked probe {}: {}", file, cube.error().message);
            continue;
        }
        setBakedProbe(renderer, p.uuid, std::move(*cube));
        ++installed;
    }
    for (const SnapshotIrradianceVolume& v : refl->volumes) {
        if (!v.uuid.isValid() || !attempted.insert(v.uuid).second) continue;
        const std::string file = bakedVolumeFileName(v.uuid);
        auto bytes = read(file);
        if (!bytes) continue;
        auto volume = decodeOxIrradiance(*bytes, file);
        if (!volume) {
            OX_LOG_WARN("render", "baked irradiance volume {}: {}", file, volume.error().message);
            continue;
        }
        setBakedVolume(renderer, v.uuid, std::move(*volume));
        ++installed;
    }
    return installed;
}

Result<usize> saveBakedData(Renderer& renderer, const std::filesystem::path& directory) {
    usize written = 0;
    for (auto& [uuid, cube] : readBakedProbes(renderer)) {
        if (!uuid.isValid() || !cube.valid()) continue;
        if (auto st = saveOxCube(directory / bakedProbeFileName(uuid), cube); !st) return st.error();
        ++written;
    }
    for (auto& [uuid, volume] : readBakedVolumes(renderer)) {
        if (!uuid.isValid() || !volume.valid()) continue;
        if (auto st = saveOxIrradiance(directory / bakedVolumeFileName(uuid), volume); !st) return st.error();
        ++written;
    }
    return written;
}

} // namespace ox::render::reflections
