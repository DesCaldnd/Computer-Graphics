#include <oxwald/core/hash.hpp>
#include <oxwald/core/reflect.hpp>
#include <oxwald/gameplay/world.hpp>
#include <oxwald/scene/component_registry.hpp>
#include <oxwald/scene/scene.hpp>

namespace ox {

namespace {

using namespace ox::gameplay;
using attr::AssetRef;
using attr::Category;
using attr::Color;
using attr::Hidden;
using attr::Meta;
using attr::NoSerialize;
using attr::Range;
using attr::ReadOnly;
using attr::SaveGame;
using attr::Step;
using attr::Tooltip;

void registerWorldEnums() {
    OX_REFLECT_ENUM(TerrainSource, "TerrainSource")
        .value("Procedural", TerrainSource::Procedural)
        .value("Heightmap", TerrainSource::Heightmap)
        .value("Flat", TerrainSource::Flat)
        .value("External", TerrainSource::External);
    OX_REFLECT_ENUM(world::HeightFormat, "world.HeightFormat")
        .value("Float32", world::HeightFormat::Float32)
        .value("UNorm16", world::HeightFormat::UNorm16);
    OX_REFLECT_ENUM(world::NoiseBasis, "world.NoiseBasis")
        .value("Perlin", world::NoiseBasis::Perlin)
        .value("Simplex", world::NoiseBasis::Simplex);
    OX_REFLECT_ENUM(world::FractalType, "world.FractalType")
        .value("Fbm", world::FractalType::Fbm)
        .value("Ridged", world::FractalType::Ridged)
        .value("Billow", world::FractalType::Billow);
    OX_REFLECT_ENUM(world::VegetationKind, "world.VegetationKind")
        .value("Grass", world::VegetationKind::Grass)
        .value("Detail", world::VegetationKind::Detail)
        .value("Tree", world::VegetationKind::Tree);
    OX_REFLECT_ENUM(world::VegetationColliderShape, "world.VegetationColliderShape")
        .value("Capsule", world::VegetationColliderShape::Capsule)
        .value("Cylinder", world::VegetationColliderShape::Cylinder)
        .value("Box", world::VegetationColliderShape::Box);
    OX_REFLECT_ENUM(world::ExclusionZone::Shape, "world.ExclusionShape")
        .value("Circle", world::ExclusionZone::Shape::Circle)
        .value("Rect", world::ExclusionZone::Shape::Rect);
    OX_REFLECT_ENUM(world::CurveDriver, "world.CurveDriver")
        .value("SunElevation", world::CurveDriver::SunElevation)
        .value("LocalHour", world::CurveDriver::LocalHour);
    OX_REFLECT_ENUM(world::CurveInterp, "world.CurveInterp")
        .value("Step", world::CurveInterp::Step)
        .value("Linear", world::CurveInterp::Linear)
        .value("Smooth", world::CurveInterp::Smooth);
}

void registerWorldStructs() {
    OX_REFLECT_TYPE(world::FractalSettings, "world.FractalSettings")
        .field("basis", &world::FractalSettings::basis)
        .field("type", &world::FractalSettings::type)
        .field("seed", &world::FractalSettings::seed)
        .field("frequency", &world::FractalSettings::frequency, Range{0.0, 1.0}, Tooltip{"Cycles per metre (first octave)"})
        .field("octaves", &world::FractalSettings::octaves, Range{1.0, 16.0})
        .field("lacunarity", &world::FractalSettings::lacunarity, Range{1.0, 4.0})
        .field("gain", &world::FractalSettings::gain, Range{0.0, 1.0})
        .field("offset", &world::FractalSettings::offset)
        .field("warpStrength", &world::FractalSettings::warpStrength, Range{0.0, 10000.0}, Tooltip{"Domain warp (metres)"})
        .field("warpFrequency", &world::FractalSettings::warpFrequency, Range{0.0, 1.0})
        .field("warpOctaves", &world::FractalSettings::warpOctaves, Range{1.0, 8.0});
    OX_REFLECT_TYPE(world::TerrainNoiseSettings, "world.TerrainNoiseSettings")
        .field("fractal", &world::TerrainNoiseSettings::fractal)
        .field("exponent", &world::TerrainNoiseSettings::exponent, Range{0.1, 8.0})
        .field("normalizeRange", &world::TerrainNoiseSettings::normalizeRange);
    OX_REFLECT_TYPE(world::HydraulicErosionSettings, "world.HydraulicErosionSettings")
        .field("droplets", &world::HydraulicErosionSettings::droplets, Range{0.0, 5000000.0})
        .field("seed", &world::HydraulicErosionSettings::seed)
        .field("maxLifetime", &world::HydraulicErosionSettings::maxLifetime, Range{1.0, 1000.0})
        .field("inertia", &world::HydraulicErosionSettings::inertia, Range{0.0, 1.0})
        .field("sedimentCapacity", &world::HydraulicErosionSettings::sedimentCapacity, Range{0.0, 64.0})
        .field("minSlope", &world::HydraulicErosionSettings::minSlope, Range{0.0, 1.0})
        .field("erodeSpeed", &world::HydraulicErosionSettings::erodeSpeed, Range{0.0, 1.0})
        .field("depositSpeed", &world::HydraulicErosionSettings::depositSpeed, Range{0.0, 1.0})
        .field("evaporateSpeed", &world::HydraulicErosionSettings::evaporateSpeed, Range{0.0, 1.0})
        .field("gravity", &world::HydraulicErosionSettings::gravity, Range{0.0, 100.0})
        .field("initialWater", &world::HydraulicErosionSettings::initialWater, Range{0.0, 100.0})
        .field("initialSpeed", &world::HydraulicErosionSettings::initialSpeed, Range{0.0, 100.0})
        .field("erosionRadius", &world::HydraulicErosionSettings::erosionRadius, Range{1.0, 16.0})
        .field("depositRemainder", &world::HydraulicErosionSettings::depositRemainder)
        .field("loseSedimentAtBorder", &world::HydraulicErosionSettings::loseSedimentAtBorder);
    OX_REFLECT_TYPE(world::ThermalErosionSettings, "world.ThermalErosionSettings")
        .field("iterations", &world::ThermalErosionSettings::iterations, Range{0.0, 10000.0})
        .field("talusAngleDeg", &world::ThermalErosionSettings::talusAngleDeg, Range{0.0, 90.0})
        .field("rate", &world::ThermalErosionSettings::rate, Range{0.0, 1.0});
    OX_REFLECT_TYPE(world::SplatRule, "world.SplatRule")
        .field("layer", &world::SplatRule::layer, Range{0.0, 7.0})
        .field("minHeight", &world::SplatRule::minHeight)
        .field("maxHeight", &world::SplatRule::maxHeight)
        .field("heightBlend", &world::SplatRule::heightBlend, Range{0.0, 10000.0})
        .field("minSlopeDeg", &world::SplatRule::minSlopeDeg, Range{0.0, 90.0})
        .field("maxSlopeDeg", &world::SplatRule::maxSlopeDeg, Range{0.0, 90.0})
        .field("slopeBlendDeg", &world::SplatRule::slopeBlendDeg, Range{0.0, 90.0})
        .field("strength", &world::SplatRule::strength, Range{0.0, 1.0})
        .field("noiseAmount", &world::SplatRule::noiseAmount, Range{0.0, 1.0})
        .field("noiseFrequency", &world::SplatRule::noiseFrequency, Range{0.0, 10.0})
        .field("noiseSeed", &world::SplatRule::noiseSeed);
    OX_REFLECT_TYPE(world::TerrainLodSettings, "world.TerrainLodSettings")
        .field("leafNodeSize", &world::TerrainLodSettings::leafNodeSize, Range{2.0, 256.0}, Tooltip{"Quads per LOD-0 node (even)"})
        .field("lodCount", &world::TerrainLodSettings::lodCount, Range{1.0, 12.0})
        .field("viewDistance", &world::TerrainLodSettings::viewDistance, Range{1.0, 1000000.0})
        .field("detailBalance", &world::TerrainLodSettings::detailBalance, Range{1.0, 8.0})
        .field("morphStartRatio", &world::TerrainLodSettings::morphStartRatio, Range{0.0, 0.99});

    {
        auto b = OX_REFLECT_TYPE(world::VegetationLodSettings, "world.VegetationLodSettings");
        // `f32 lodDistances[2]` is a C array (not reflectable): exposed as a vec2 with the same layout.
        static_assert(sizeof(glm::vec2) == sizeof(world::VegetationLodSettings::lodDistances));
        reflect::FieldInfo f;
        f.name = "lodDistances";
        f.nameHash = fnv1a64(f.name);
        f.type = &reflect::typeOf<glm::vec2>();
        f.access = [](void* o) -> void* { return static_cast<world::VegetationLodSettings*>(o)->lodDistances; };
        f.attributes.tooltip = "End of mesh LOD 0 and LOD 1 (metres)";
        b.info().fields.push_back(std::move(f));
        b.field("impostorDistance", &world::VegetationLodSettings::impostorDistance, Range{0.0, 100000.0})
            .field("cullDistance", &world::VegetationLodSettings::cullDistance, Range{0.0, 100000.0})
            .field("fadeRange", &world::VegetationLodSettings::fadeRange, Range{0.0, 1000.0});
    }
    OX_REFLECT_TYPE(world::VegetationLayer, "world.VegetationLayer")
        .field("name", &world::VegetationLayer::name)
        .field("kind", &world::VegetationLayer::kind)
        .field("prototype", &world::VegetationLayer::prototype, Tooltip{"Renderer mesh/material prototype index"})
        .field("seed", &world::VegetationLayer::seed)
        .field("minDistance", &world::VegetationLayer::minDistance, Range{0.05, 1000.0})
        .field("density", &world::VegetationLayer::density, Range{0.0, 1.0})
        .field("minHeight", &world::VegetationLayer::minHeight)
        .field("maxHeight", &world::VegetationLayer::maxHeight)
        .field("minSlopeDeg", &world::VegetationLayer::minSlopeDeg, Range{0.0, 90.0})
        .field("maxSlopeDeg", &world::VegetationLayer::maxSlopeDeg, Range{0.0, 90.0})
        .field("splatLayer", &world::VegetationLayer::splatLayer, Range{-1.0, 7.0})
        .field("minSplatWeight", &world::VegetationLayer::minSplatWeight, Range{0.0, 1.0})
        .field("densityMapIndex", &world::VegetationLayer::densityMapIndex)
        .field("minScale", &world::VegetationLayer::minScale, Range{0.0, 100.0})
        .field("maxScale", &world::VegetationLayer::maxScale, Range{0.0, 100.0})
        .field("randomYaw", &world::VegetationLayer::randomYaw)
        .field("alignToNormal", &world::VegetationLayer::alignToNormal, Range{0.0, 1.0})
        .field("sinkDepth", &world::VegetationLayer::sinkDepth, Range{0.0, 10.0})
        .field("tintA", &world::VegetationLayer::tintA, Color{})
        .field("tintB", &world::VegetationLayer::tintB, Color{})
        .field("boundingRadius", &world::VegetationLayer::boundingRadius, Range{0.0, 1000.0})
        .field("lod", &world::VegetationLayer::lod)
        .field("collider", &world::VegetationLayer::collider)
        .field("colliderShape", &world::VegetationLayer::colliderShape)
        .field("colliderRadius", &world::VegetationLayer::colliderRadius, Range{0.0, 100.0})
        .field("colliderHalfHeight", &world::VegetationLayer::colliderHalfHeight, Range{0.0, 100.0});
    OX_REFLECT_TYPE(world::ExclusionZone, "world.ExclusionZone")
        .field("shape", &world::ExclusionZone::shape)
        .field("center", &world::ExclusionZone::center)
        .field("halfExtents", &world::ExclusionZone::halfExtents, Tooltip{"Circle: x = radius"})
        .field("layerMask", &world::ExclusionZone::layerMask, Tooltip{"Bit i excludes layer i"});
    OX_REFLECT_TYPE(world::CurveKey, "world.CurveKey")
        .field("time", &world::CurveKey::time)
        .field("value", &world::CurveKey::value);
    OX_REFLECT_TYPE(world::GradientKey, "world.GradientKey")
        .field("time", &world::GradientKey::time)
        .field("color", &world::GradientKey::color, Color{true});
    OX_REFLECT_TYPE(world::GerstnerWave, "world.GerstnerWave")
        .field("direction", &world::GerstnerWave::direction)
        .field("wavelength", &world::GerstnerWave::wavelength, Range{0.01, 10000.0})
        .field("amplitude", &world::GerstnerWave::amplitude, Range{0.0, 100.0})
        .field("steepness", &world::GerstnerWave::steepness, Range{0.0, 1.0})
        .field("phase", &world::GerstnerWave::phase)
        .field("speedScale", &world::GerstnerWave::speedScale, Range{0.0, 10.0});
    OX_REFLECT_TYPE(world::WeatherPreset, "world.WeatherPreset")
        .field("cloudCover", &world::WeatherPreset::cloudCover, Range{0.0, 1.0})
        .field("rain", &world::WeatherPreset::rain, Range{0.0, 1.0})
        .field("snow", &world::WeatherPreset::snow, Range{0.0, 1.0})
        .field("fogDensityBoost", &world::WeatherPreset::fogDensityBoost, Range{0.0, 1.0})
        .field("windSpeed", &world::WeatherPreset::windSpeed, Range{0.0, 100.0})
        .field("gustStrength", &world::WeatherPreset::gustStrength, Range{0.0, 2.0});
    OX_REFLECT_TYPE(world::WeatherState, "world.WeatherState")
        .field("current", &world::WeatherState::current)
        .field("wetness", &world::WeatherState::wetness)
        .field("snowCover", &world::WeatherState::snowCover)
        .field("transition", &world::WeatherState::transition);
    OX_REFLECT_TYPE(world::BuoyancyPoint, "world.BuoyancyPoint")
        .field("localPosition", &world::BuoyancyPoint::localPosition)
        .field("volume", &world::BuoyancyPoint::volume, Range{0.0, 100000.0})
        .field("height", &world::BuoyancyPoint::height, Range{0.001, 1000.0});
    OX_REFLECT_TYPE(world::ChunkStreamerSettings, "world.ChunkStreamerSettings")
        .field("chunkSize", &world::ChunkStreamerSettings::chunkSize, Range{1.0, 100000.0})
        .field("loadRadius", &world::ChunkStreamerSettings::loadRadius, Range{0.0, 1000000.0})
        .field("unloadRadius", &world::ChunkStreamerSettings::unloadRadius, Range{0.0, 1000000.0})
        .field("maxLoadRequestsPerUpdate", &world::ChunkStreamerSettings::maxLoadRequestsPerUpdate)
        .field("maxInFlightLoads", &world::ChunkStreamerSettings::maxInFlightLoads)
        .field("maxActivationsPerUpdate", &world::ChunkStreamerSettings::maxActivationsPerUpdate)
        .field("maxUnloadsPerUpdate", &world::ChunkStreamerSettings::maxUnloadsPerUpdate)
        .field("viewDirectionWeight", &world::ChunkStreamerSettings::viewDirectionWeight, Range{0.0, 1.0})
        .field("teleportDistance", &world::ChunkStreamerSettings::teleportDistance)
        .field("teleportBudgetMultiplier", &world::ChunkStreamerSettings::teleportBudgetMultiplier)
        .field("failedRetryUpdates", &world::ChunkStreamerSettings::failedRetryUpdates);
}

void registerWorldComponents(ComponentRegistry& reg) {
    OX_REFLECT_TYPE(TerrainComponent, "Terrain")
        .attributes(Category{"World"}, Meta{"icon", "mountain"}, Tooltip{"Heightfield terrain (CDLOD rendering, physics tiles)"})
        .field("source", &TerrainComponent::source)
        .field("heightmap", &TerrainComponent::heightmap, AssetRef{"Heightmap"})
        .field("noise", &TerrainComponent::noise)
        .field("hydraulicErosion", &TerrainComponent::hydraulicErosion)
        .field("hydraulic", &TerrainComponent::hydraulic)
        .field("thermalErosion", &TerrainComponent::thermalErosion)
        .field("thermal", &TerrainComponent::thermal)
        .field("resolution", &TerrainComponent::resolution, Range{2.0, 16385.0}, Tooltip{"Samples per side"})
        .field("worldSize", &TerrainComponent::worldSize, Range{1.0, 1000000.0}, Tooltip{"Metres along X and Z"})
        .field("heightScale", &TerrainComponent::heightScale, Range{0.0, 100000.0})
        .field("heightOffset", &TerrainComponent::heightOffset)
        .field("format", &TerrainComponent::format)
        .field("centered", &TerrainComponent::centered, Tooltip{"Grid centred on the entity XZ; otherwise sample (0,0) at entity XZ + offset"})
        .field("offset", &TerrainComponent::offset)
        .field("layers", &TerrainComponent::layers, AssetRef{"Material"}, Tooltip{"Material per splat layer (max 8)"})
        .field("splatRules", &TerrainComponent::splatRules)
        .field("splatResolution", &TerrainComponent::splatResolution, Tooltip{"0 = heightfield resolution"})
        .field("lod", &TerrainComponent::lod)
        .field("collision", &TerrainComponent::collision)
        .field("physicsTileQuads", &TerrainComponent::physicsTileQuads, Range{4.0, 1024.0})
        .field("friction", &TerrainComponent::friction, Range{0.0, 2.0}, Step{0.01})
        .field("restitution", &TerrainComponent::restitution, Range{0.0, 1.0}, Step{0.01})
        .field("builtResolution", &TerrainComponent::builtResolution, NoSerialize{}, ReadOnly{})
        .field("minHeight", &TerrainComponent::minHeight, NoSerialize{}, ReadOnly{})
        .field("maxHeight", &TerrainComponent::maxHeight, NoSerialize{}, ReadOnly{});
    reg.add<TerrainComponent>({.icon = "mountain"});

    OX_REFLECT_TYPE(VegetationComponent, "Vegetation")
        .attributes(Category{"World"}, Meta{"icon", "tree"}, Tooltip{"Procedural vegetation scattered on a terrain"})
        .field("layers", &VegetationComponent::layers)
        .field("terrain", &VegetationComponent::terrain, Tooltip{"Terrain entity; empty = this entity"})
        .field("chunkSize", &VegetationComponent::chunkSize, Range{4.0, 4096.0})
        .field("cellSize", &VegetationComponent::cellSize, Range{1.0, 4096.0})
        .field("patternPeriod", &VegetationComponent::patternPeriod, Range{8.0, 4096.0})
        .field("scatterRadius", &VegetationComponent::scatterRadius, Range{0.0, 100000.0})
        .field("colliders", &VegetationComponent::colliders)
        .field("exclusions", &VegetationComponent::exclusions)
        .field("chunkCount", &VegetationComponent::chunkCount, NoSerialize{}, ReadOnly{})
        .field("instanceCount", &VegetationComponent::instanceCount, NoSerialize{}, ReadOnly{});
    reg.add<VegetationComponent>({.icon = "tree"});

    OX_REFLECT_TYPE(SkyComponent, "Sky")
        .attributes(Category{"World"}, Meta{"icon", "cloud-sun"}, Tooltip{"Analytic sky, sun disc, moon and stars"})
        .field("turbidity", &SkyComponent::turbidity, Range{1.7, 10.0}, Tooltip{"Used without a TimeOfDay"})
        .field("sunDirection", &SkyComponent::sunDirection, Tooltip{"Towards the sun, used without a TimeOfDay"})
        .field("skyIntensity", &SkyComponent::skyIntensity, Range{0.0, 100.0})
        .field("sunDisc", &SkyComponent::sunDisc)
        .field("sunDiscIntensity", &SkyComponent::sunDiscIntensity, Range{0.0, 100.0})
        .field("sunAngularDiameterDeg", &SkyComponent::sunAngularDiameterDeg, Range{0.0, 20.0})
        .field("moon", &SkyComponent::moon)
        .field("moonIntensity", &SkyComponent::moonIntensity, Range{0.0, 100.0})
        .field("moonAngularDiameterDeg", &SkyComponent::moonAngularDiameterDeg, Range{0.0, 20.0})
        .field("stars", &SkyComponent::stars)
        .field("starsIntensity", &SkyComponent::starsIntensity, Range{0.0, 100.0});
    reg.add<SkyComponent>({.icon = "cloud-sun"});

    OX_REFLECT_TYPE(TimeOfDayComponent, "TimeOfDay")
        .attributes(Category{"World"}, Meta{"icon", "clock"}, Tooltip{"Day/night cycle driving the sun light, fog and ambient"})
        .field("latitudeDeg", &TimeOfDayComponent::latitudeDeg, Range{-90.0, 90.0})
        .field("longitudeDeg", &TimeOfDayComponent::longitudeDeg, Range{-180.0, 180.0})
        .field("year", &TimeOfDayComponent::year)
        .field("month", &TimeOfDayComponent::month, Range{1.0, 12.0})
        .field("day", &TimeOfDayComponent::day, Range{1.0, 31.0})
        .field("localHours", &TimeOfDayComponent::localHours, Range{0.0, 24.0}, SaveGame{})
        .field("utcOffsetHours", &TimeOfDayComponent::utcOffsetHours, Range{-14.0, 14.0})
        .field("timeScale", &TimeOfDayComponent::timeScale, Range{0.0, 100000.0}, Tooltip{"Game seconds per real second"})
        .field("paused", &TimeOfDayComponent::paused, SaveGame{})
        .field("turbidity", &TimeOfDayComponent::turbidity, Range{1.7, 10.0})
        .field("sun", &TimeOfDayComponent::sun, Tooltip{"Directional light; empty = Environment.sun or the first directional light"})
        .field("driveLight", &TimeOfDayComponent::driveLight)
        .field("illuminanceScale", &TimeOfDayComponent::illuminanceScale, Range{0.0, 10.0})
        .field("driveEnvironment", &TimeOfDayComponent::driveEnvironment)
        .field("driveExposure", &TimeOfDayComponent::driveExposure)
        .field("curveDriver", &TimeOfDayComponent::curveDriver)
        .field("curveInterp", &TimeOfDayComponent::curveInterp)
        .field("fogDensityCurve", &TimeOfDayComponent::fogDensityCurve)
        .field("ambientIntensityCurve", &TimeOfDayComponent::ambientIntensityCurve)
        .field("exposureCurve", &TimeOfDayComponent::exposureCurve)
        .field("starsCurve", &TimeOfDayComponent::starsCurve)
        .field("ambientColorGradient", &TimeOfDayComponent::ambientColorGradient)
        .field("fogColorGradient", &TimeOfDayComponent::fogColorGradient)
        .field("sunElevationDeg", &TimeOfDayComponent::sunElevationDeg, NoSerialize{}, ReadOnly{})
        .field("isDay", &TimeOfDayComponent::isDay, NoSerialize{}, ReadOnly{})
        .field("moonIllumination", &TimeOfDayComponent::moonIllumination, NoSerialize{}, ReadOnly{});
    reg.add<TimeOfDayComponent>({.icon = "clock"});

    OX_REFLECT_TYPE(WaterComponent, "Water")
        .attributes(Category{"World"}, Meta{"icon", "water"}, Tooltip{"Gerstner water surface (base height = entity Y)"})
        .field("waves", &WaterComponent::waves, Tooltip{"Empty = generated from the wind fields below"})
        .field("windDirection", &WaterComponent::windDirection)
        .field("windSpeed", &WaterComponent::windSpeed, Range{0.0, 100.0})
        .field("waveCount", &WaterComponent::waveCount, Range{0.0, 16.0})
        .field("seed", &WaterComponent::seed)
        .field("steepness", &WaterComponent::steepness, Range{0.0, 1.0})
        .field("size", &WaterComponent::size, Tooltip{"XZ extent centred on the entity (<= 0: unbounded)"})
        .field("fluidDensity", &WaterComponent::fluidDensity, Range{0.0, 20000.0})
        .field("current", &WaterComponent::current);
    reg.add<WaterComponent>({.icon = "water"});

    OX_REFLECT_TYPE(WindComponent, "Wind")
        .attributes(Category{"World"}, Meta{"icon", "wind"}, Tooltip{"Global wind (first active) and weather"})
        .field("active", &WindComponent::active)
        .field("direction", &WindComponent::direction)
        .field("speed", &WindComponent::speed, Range{0.0, 100.0})
        .field("gustStrength", &WindComponent::gustStrength, Range{0.0, 2.0})
        .field("gustWavelength", &WindComponent::gustWavelength, Range{0.1, 10000.0})
        .field("turbulence", &WindComponent::turbulence, Range{0.0, 2.0})
        .field("turbulenceFrequency", &WindComponent::turbulenceFrequency, Range{0.0, 20.0})
        .field("weatherEnabled", &WindComponent::weatherEnabled)
        .field("weather", &WindComponent::weather, SaveGame{})
        .field("transitionSeconds", &WindComponent::transitionSeconds, Range{0.0, 3600.0})
        .field("temperatureCelsius", &WindComponent::temperatureCelsius, Range{-80.0, 60.0})
        .field("weatherState", &WindComponent::weatherState, NoSerialize{}, ReadOnly{});
    reg.add<WindComponent>({.icon = "wind"});

    OX_REFLECT_TYPE(BuoyancyComponent, "Buoyancy")
        .attributes(Category{"World"}, Meta{"icon", "life-ring"}, Tooltip{"Floats a dynamic rigid body on water"})
        .field("halfExtents", &BuoyancyComponent::halfExtents)
        .field("subdivisions", &BuoyancyComponent::subdivisions, Range{1.0, 8.0})
        .field("points", &BuoyancyComponent::points, Tooltip{"Explicit sample points (relative to the centre of mass)"})
        .field("fluidDensity", &BuoyancyComponent::fluidDensity, Range{0.0, 20000.0}, Tooltip{"0 = from the water"})
        .field("linearDrag", &BuoyancyComponent::linearDrag, Range{0.0, 100.0})
        .field("angularDrag", &BuoyancyComponent::angularDrag, Range{0.0, 100.0})
        .field("submergedFraction", &BuoyancyComponent::submergedFraction, NoSerialize{}, ReadOnly{});
    reg.add<BuoyancyComponent>({.icon = "life-ring"});

    OX_REFLECT_TYPE(StreamingSourceComponent, "StreamingSource")
        .attributes(Category{"World"}, Meta{"icon", "satellite-dish"}, Tooltip{"Viewer for chunk streaming and vegetation"})
        .field("enabled", &StreamingSourceComponent::enabled)
        .field("radiusScale", &StreamingSourceComponent::radiusScale, Range{0.0, 10.0});
    reg.add<StreamingSourceComponent>({.icon = "satellite-dish"});

    OX_REFLECT_TYPE(WorldStreamingComponent, "WorldStreaming")
        .attributes(Category{"World"}, Meta{"icon", "border-all"}, Tooltip{"Chunk streaming (ChunkData files and/or prefabs)"})
        .field("enabled", &WorldStreamingComponent::enabled)
        .field("settings", &WorldStreamingComponent::settings)
        .field("chunkPath", &WorldStreamingComponent::chunkPath, Tooltip{"e.g. project://World/chunk_{x}_{z}.oxchunk"})
        .field("prefabPattern", &WorldStreamingComponent::prefabPattern, Tooltip{"e.g. Chunks/chunk_{x}_{z}"})
        .field("useCameraAsViewer", &WorldStreamingComponent::useCameraAsViewer)
        .field("tileLod", &WorldStreamingComponent::tileLod)
        .field("tileCollision", &WorldStreamingComponent::tileCollision)
        .field("tilePhysicsQuads", &WorldStreamingComponent::tilePhysicsQuads, Range{4.0, 1024.0})
        .field("tileLayers", &WorldStreamingComponent::tileLayers, AssetRef{"Material"})
        .field("vegetationLayers", &WorldStreamingComponent::vegetationLayers)
        .field("loadedChunks", &WorldStreamingComponent::loadedChunks, NoSerialize{}, ReadOnly{});
    reg.add<WorldStreamingComponent>({.icon = "border-all"});
}

} // namespace

void registerWorldGameplayTypes() {
    registerSceneTypes();
    registerWorldEnums();
    registerWorldStructs();
    registerWorldComponents(ComponentRegistry::instance());
}

} // namespace ox
