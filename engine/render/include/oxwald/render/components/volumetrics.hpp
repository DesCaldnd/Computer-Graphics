#pragma once

// ECS components of the volumetrics feature area (froxel fog volumes, global fog overrides, volumetric clouds).
// Reflected and registered by ox::render::volumetrics::registerVolumetricsTypes() (called from
// ox::render::registerRenderTypes()); extracted into RenderSnapshot through VolumetricsSnapshot
// (features/volumetrics/volumetrics.hpp).
//
// Global height fog itself stays on EnvironmentComponent (fogEnabled, fogColor = scattering albedo, fogDensity =
// extinction in 1/m at world height 0, fogHeightFalloff, fogStartDistance).

#include <oxwald/core/math.hpp>
#include <oxwald/core/types.hpp>

namespace ox::render {

enum class FogVolumeShape : u8 { Box, Sphere, Ellipsoid };

// Local participating medium. The shape is centred on the entity and follows its world transform (rotation and
// scale included); `extents` are the local half sizes (Sphere uses extents.x as the radius).
struct FogVolumeComponent {
    FogVolumeShape shape = FogVolumeShape::Box;
    glm::vec3 extents{2.0f};
    f32 density = 0.1f;         // extinction coefficient (1/m) at the centre
    glm::vec3 albedo{1.0f};     // single scattering albedo (scattering = density · albedo)
    glm::vec3 emission{0.0f};   // emitted radiance of an optically thick volume, display-relative like material
                                // emissive (1 = white at the current exposure)
    f32 falloff = 0.25f;        // fraction of the shape (from the boundary inwards) over which density fades in
    f32 anisotropy = 0.0f;      // Henyey-Greenstein g (-0.95 back … 0.95 forward scattering)
    f32 noiseIntensity = 0.0f;  // 0 = homogeneous, 1 = fully modulated by the 3D Perlin-Worley noise
    f32 noiseScale = 6.0f;      // metres per noise tile
    glm::vec3 noiseVelocity{0.0f}; // m/s scroll of the noise (in addition to the world wind)
    f32 windInfluence = 1.0f;   // multiplier of the world wind (WindComponent via the world bridge)
};

// Optional global settings of the froxel fog (first active instance in the world wins). Without it the
// r.VolumetricFog.* cvars and EnvironmentComponent fog values are used.
struct VolumetricFogComponent {
    f32 anisotropy = 0.6f;          // HG g of the height fog (fog volumes have their own)
    f32 ambientIntensity = 1.0f;    // sky light (IBL irradiance) scattered by the fog
    f32 directionalIntensity = 1.0f;
    f32 localLightIntensity = 1.0f;
    glm::vec3 emission{0.0f};       // emission of the height fog (display-relative), scaled by its density
    f32 distance = 0.0f;            // froxel grid far distance in metres, 0 = r.VolumetricFog.Distance
};

// Ray-marched cloud layer (Nubis-like). The first active layer is rendered; the sun is the scene's sun light
// (Environment::sun or the brightest directional light, driven by the world's time of day when present).
struct CloudLayerComponent {
    f32 altitude = 1500.0f;      // base of the layer above world y = 0 (m)
    f32 thickness = 1800.0f;     // m
    f32 coverage = 0.45f;        // 0 = clear sky, 1 = overcast
    f32 cloudType = 0.5f;        // 0 = stratus (flat), 0.5 = cumulus, 1 = cumulonimbus (towering)
    f32 density = 1.0f;          // extinction multiplier (1 ≈ 0.02 / m in the cloud cores)
    glm::vec3 albedo{1.0f};
    glm::vec2 windDirection{1.0f, 0.0f}; // XZ
    f32 windSpeed = 8.0f;        // m/s; world wind is added when present
    f32 weatherScale = 24000.0f; // metres per weather map tile (coverage / type variation)
    f32 shapeScale = 4500.0f;    // metres per base shape noise tile
    f32 detailScale = 600.0f;    // metres per detail noise tile
    f32 detailStrength = 0.35f;  // erosion of the edges by the detail noise
    f32 ambientIntensity = 1.0f;
    f32 sunIntensity = 1.0f;
    f32 forwardScattering = 0.75f; // HG g of the silver lining lobe
    f32 shadowStrength = 0.8f;     // cloud shadows on the volumetric fog (0 = off)
    glm::vec2 weatherOffset{0.0f}; // shifts the weather map (variety between levels)
};

} // namespace ox::render
