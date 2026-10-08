# render — volumetrics (froxel fog, clouds)

Feature area `volumetrics` of the render module. Code: `engine/render/src/features/volumetrics/`, public headers
`<oxwald/render/features/volumetrics/volumetrics.hpp>` and `<oxwald/render/components/volumetrics.hpp>`, shaders
`engine/shaders/render/volumetrics/`. One feature, **`Volumetrics`** (toggle `r.Feature.Volumetrics`, exclusive group
`Volumetrics`), injected at `Lighting` (order 100) and `AfterOpaque` (order 100, after the Sky at -1000).

## Frame

```
Lighting     Volumetrics.FogInject      froxels: height fog + FogVolume shapes (falloff, scrolling 3D noise) → media, emission/g
             Volumetrics.FogTileDepth   closest opaque depth per froxel column (anti light-leak, see below)
             Volumetrics.FogScatter     sun (CSM) + other directional + clustered point/spot (atlas / cube shadows) + IBL ambient,
                                        Henyey-Greenstein, cloud shadows, [VolumetricFogVisibility]; temporal reprojection → history
             Volumetrics.FogIntegrate   front-to-back (energy conserving) → VolumetricFog (3D RGBA16F)
             Volumetrics.CloudTrace     1 of checker² pixels per frame at cloud resolution (render / downsample)
             Volumetrics.CloudReconstruct  temporal reconstruction → VolumetricClouds / VolumetricCloudsDepth (history)
AfterOpaque  Volumetrics.Composite      SceneColorHDR = dst · T + S: froxels → analytic height fog beyond the grid / sky → clouds
```

* **Grid**: default 160×90×64 (`r.VolumetricFog.GridSize{X,Y,Z}`), uniform in screen space, exponential in view depth
  up to `r.VolumetricFog.Distance` (128 m): `depth(s) = (2^(s·log2(1 + far·k)) − 1) / k`, `k =
  r.VolumetricFog.DepthDistributionScale` (32). `volumetrics::FroxelGrid` is the C++ mirror.
* **Media**: EnvironmentComponent height fog (`fogDensity` = extinction at y = 0, `fogHeightFalloff`, `fogStartDistance`,
  `fogColor` = albedo) + `FogVolumeComponent` (box / sphere / ellipsoid following the entity transform, density,
  albedo, emission, edge falloff, anisotropy, Perlin-Worley noise scrolled by `noiseVelocity` + world wind).
  `VolumetricFogComponent` (optional, first wins) overrides anisotropy, ambient / directional / local light
  intensities, emission and the grid distance.
* **Temporal**: per-frame Halton(2,3,5) jitter of the sample inside the froxel; history = last frame's scatter volume
  sampled at the froxel centre reprojected with the previous camera (`r.VolumetricFog.HistoryWeight`); rejected outside
  the previous frustum, on camera cuts, resize, grid distribution changes; rescaled when pre-exposure changes.
* **Light leaking**: the slice that straddles the closest surface of its froxel column evaluates lights only in its
  visible part (`FogTileDepth`), otherwise oblique walls get bands with the slice period.
* **Composite**: `oxEvaluateVolumetricFogRay` (froxels + analytic height fog from the grid end to the surface; sky
  pixels to `r.VolumetricFog.SkyDistance` = aerial perspective / horizon haze), then clouds (bilateral upsample by
  depth). When the froxel fog is active the forward pass's own height fog is disabled (`VIEW.fogColor.w = 0`).
* **Clouds**: spherical shell (planet radius 6360 km) at `CloudLayerComponent::altitude/thickness`. Density = weather map
  coverage (2D, generated) × height gradient by cloud type × Perlin-Worley base shape (128³) eroded by Worley detail
  (32³); noise generated once by compute (`noise_gen.comp`, `Device::immediateSubmit`). Lighting: sun light march
  (`r.VolumetricClouds.LightSteps`), 3 multiple-scattering octaves, powder, dual-lobe HG, height-dependent sky
  ambient. The sun is the snapshot's sun light (time of day = the world bridge driving that light). Checkerboard: 1/16
  of the pixels per frame (`r.VolumetricClouds.Checkerboard` 16 / 4 / 1), others reprojected at their cloud distance;
  disocclusion by scene depth. Cloud shadows on the fog: weather-map coverage along the sun ray
  (`r.VolumetricFog.CloudShadows`, `CloudLayerComponent::shadowStrength`). Cloud shadows on surfaces: TODO.

## For other teams

**Translucency / water / particles** — `#include <render/volumetrics/fog_sample.glsl>`:

```glsl
// declare .read(R.texture(res::kVolumetricFog), Access::SampledFragment) when present; else pass OX_INVALID_INDEX
vec4 fog = oxEvaluateVolumetricFog(pc.view, pc.scene, fogTexture, gl_FragCoord.xy * VIEW.renderSize.zw, worldPos);
color.rgb = oxApplyVolumetricFog(color.rgb, fog);        // pre-exposed: rgb · T + inscatter
// premultiplied alpha: rgb = rgb · fog.a + fog.rgb · alpha
```
`oxSampleVolumetricFog(view, tex, uv, viewDepth)` returns the froxel part only. When `VIEW.volumetricFogGrid.w == 0`
(feature inactive) the result is (0, 0, 0, 1); fall back to `oxApplyHeightFog` if `VIEW.fogColor.w > 0.5`.

**Ray tracing (Phase 4 hook)** — publish `VolumetricFogVisibility` (`volumetrics::kVolumetricFogVisibility`, 3D
`kVisibilityFormat` = RGBA16F, size `froxelGridFromCVars(&snapshot)`) at `InjectionPoint::Lighting` with order
< `volumetrics::kFogOrder` (or earlier). Channels: r = sun visibility, g = local light visibility (ratio of shadowed to
unshadowed local in-scattering, e.g. a stochastic ray per froxel towards a light picked by contribution), b = sky
visibility, a unused. When present, FogScatter uses it instead of every shadow map lookup. Sample positions:
`oxFroxelWorldPosition(view, vec3(coord) + 0.5 + vec3(jitter), depth)` from `fog_common.glsl` with the grid in
`VIEW.volumetricFogGrid/Depth` (z jitter = `VIEW.volumetricFogDepth.z`; xy jitter may be the producer's own).
`VolumetricsTest.VisibilityHookReplacesShadowLookups` contains a minimal producer.

**World bridge** — call `volumetrics::setWorldWind(snapshot, {dir.x · speed, 0, dir.y · speed})` from the
WorldRenderData extract hook (scrolls fog-volume noise and clouds); drive the sun light from the time of day.

## CVars (Scalability `Volumetrics`: Low / Medium / High / Ultra; defaults = High)

| CVar | Default | Levels |
| --- | --- | --- |
| `r.VolumetricFog` | true | — |
| `r.VolumetricFog.GridSizeX / Y / Z` | 160 / 90 / 64 | 96·54·32, 128·72·48, 160·90·64, 240·135·128 |
| `r.VolumetricFog.Distance` | 128 | 64, 96, 128, 192 |
| `r.VolumetricFog.HistoryWeight` | 0.9 | 0.85, 0.9, 0.9, 0.95 |
| `r.VolumetricFog.LocalLightShadows` | true | off, on, on, on |
| `r.VolumetricFog.CloudShadows` | true | off, off, on, on |
| `r.VolumetricFog.DepthDistributionScale`, `.TemporalReprojection`, `.Anisotropy`, `.SunShadows`, `.MaxVolumes`, `.SkyDistance` | 32, true, 0.6, true, 64, 20000 | — |
| `r.VolumetricClouds` | true | off, on, on, on |
| `r.VolumetricClouds.Downsample` | 2 | 4, 4, 2, 2 |
| `r.VolumetricClouds.Checkerboard` | 16 | 16, 16, 16, 4 |
| `r.VolumetricClouds.Steps` / `.LightSteps` | 64 / 6 | 32·4, 48·5, 64·6, 96·8 |
| `r.VolumetricClouds.MaxDistance` | 40000 | 25000, 30000, 40000, 50000 |
| `r.VolumetricClouds.Temporal` | true | — |

## Performance (Apple M4 Pro, 1080p, `VolumetricsTest.PerfReport1080p`)

Scene: height fog + 8 fog volumes, 64 point lights (4 shadowed), sun + CSM, cloud layer. GPU ms:

| Level | Inject | Scatter | Integrate | Cloud trace | Cloud reconstruct | Composite | Total |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Low | 0.03 | 0.06 | 0.02 | — | — | 0.01 | 0.17 |
| Medium | 0.05 | 0.14 | 0.03 | 0.30 | 0.02 | 0.06 | 0.72 |
| High | 0.09 | 0.28 | 0.06 | 0.41 | 0.07 | 0.06 | 1.14 |
| Ultra | 0.38 | 1.08 | 0.28 | 1.89 | 0.07 | 0.06 | 3.80 |

## Notes / limits

* MoltenVK: shader structs holding 64-bit buffer references must not be copied by value (`FogConstants F = pc.fog.c`
  reads zeros through the copied pointer); access them through the reference (`#define F (pc.fog.c)`).
* Shadow maps are read through `oxShadowTextures2D*` (Metal shadow-sampler rule), one hardware compare per froxel.
* TODO: cloud shadows on opaque surfaces, per-tile fog-volume culling (all volumes are tested per froxel, capped by
  `r.VolumetricFog.MaxVolumes`), async compute for the fog passes, local light shadows use one compare (no PCF).
