#pragma once

// ECS components of the translucency-water-particles area: GPU particle emitters and water surfaces.
// Reflected and registered by registerTranslucencyTypes() (called from render::registerRenderTypes()); extracted into
// the RenderSnapshot by the area's extract hook (see features/translucency/translucency.hpp).

#include <oxwald/core/math.hpp>
#include <oxwald/core/types.hpp>
#include <oxwald/core/uuid.hpp>

#include <vector>

namespace ox::render {

// --- particles ------------------------------------------------------------------------------------------------

enum class ParticleShape : u8 { Point, Sphere, Cone, Box, MeshSurface };
enum class ParticleBlend : u8 { Additive, Alpha, Premultiplied };
enum class ParticleRenderMode : u8 { Billboard, StretchedBillboard, Mesh };
enum class ParticleCollision : u8 { None, Bounce, Kill };
// Procedural sprites used when `texture` is not set.
enum class ParticleSprite : u8 { SoftCircle, Smoke, Spark };

struct ParticleBurst {
    f32 time = 0.0f;     // seconds after the emitter (re)started
    u32 count = 10;
    u32 cycles = 1;      // 0 = repeat forever
    f32 interval = 1.0f; // seconds between cycles
};

struct ParticleCurveKey {
    f32 time = 0.0f; // normalised age 0..1
    f32 value = 1.0f;
};

struct ParticleColorKey {
    f32 time = 0.0f;               // normalised age 0..1
    glm::vec4 color{1.0f};         // linear rgb, a = opacity
};

// GPU simulated particle system. Spawn shapes are in the entity's local space (+Y is the emission axis); particles
// simulate in world space. Size/colour curves are baked into a small gradient texture per emitter.
struct ParticleEmitterComponent {
    bool enabled = true;
    u32 maxParticles = 1024; // clamped by r.Particles.Budget
    u32 seed = 1;

    // Spawning.
    f32 spawnRate = 50.0f; // particles per second
    std::vector<ParticleBurst> bursts;
    bool loop = true;
    f32 duration = 5.0f; // seconds of one emission cycle (bursts repeat with it when looping)
    glm::vec2 lifetime{1.5f, 2.5f}; // min, max seconds

    // Shape.
    ParticleShape shape = ParticleShape::Point;
    f32 radius = 0.5f;            // sphere / cone base radius
    f32 coneAngle = 25.0f;        // degrees (half angle)
    glm::vec3 boxExtents{0.5f};   // half extents
    Uuid shapeMesh;               // MeshSurface: emits from the triangles of this mesh (first submesh)
    bool emitFromShell = false;   // sphere: surface only

    // Motion.
    glm::vec2 speed{1.0f, 2.0f};  // initial speed along the shape direction (min, max)
    glm::vec3 velocity{0.0f};     // added initial velocity (local space)
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    f32 gravityScale = 0.0f;
    f32 drag = 0.0f;              // 1/s
    f32 turbulence = 0.0f;        // curl-noise acceleration (m/s²)
    f32 turbulenceFrequency = 1.0f; // 1/m
    f32 turbulenceSpeed = 0.5f;     // noise field animation

    // Appearance.
    glm::vec2 size{0.1f, 0.2f};   // start size range (metres)
    std::vector<ParticleCurveKey> sizeOverLife;   // multiplier (empty = 1)
    glm::vec4 color{1.0f};        // multiplied with colourOverLife
    std::vector<ParticleColorKey> colorOverLife;  // empty = white
    f32 emissive = 1.0f;          // emission, display relative (1 = colour at the current exposure); lit smoke: 0
    glm::vec2 rotation{0.0f, 360.0f};     // start rotation range (degrees)
    glm::vec2 rotationSpeed{0.0f, 0.0f};  // degrees per second range
    Uuid texture;                 // sprite / flipbook atlas (invalid = procedural `sprite`)
    ParticleSprite sprite = ParticleSprite::SoftCircle;
    u32 atlasColumns = 1;
    u32 atlasRows = 1;
    f32 flipbookFps = 0.0f;       // 0 = play the atlas once over the lifetime
    bool randomStartFrame = false;
    ParticleBlend blend = ParticleBlend::Alpha;
    ParticleRenderMode renderMode = ParticleRenderMode::Billboard;
    f32 stretch = 0.05f;          // stretched billboards: extra length per m/s
    Uuid mesh;                    // Mesh render mode
    bool lit = false;             // clustered lights + sun shadow + ambient
    f32 softDistance = 0.3f;      // soft particle fade distance (m), 0 = hard
    bool sort = false;            // back-to-front sort of this emitter's particles (r.Particles.Sorting)

    // Depth buffer collisions (r.Particles.Collision).
    ParticleCollision collision = ParticleCollision::None;
    f32 bounce = 0.4f;            // restitution
    f32 friction = 0.2f;          // tangential velocity loss on impact
};

// --- water ----------------------------------------------------------------------------------------------------

// Mirrors world::GerstnerWave (same packing, see engine/shaders/world/gerstner.glsl) so the render module does not
// depend on the world module.
struct WaterWave {
    glm::vec2 direction{1.0f, 0.0f};
    f32 wavelength = 10.0f;
    f32 amplitude = 0.2f;
    f32 steepness = 0.5f;
    f32 phase = 0.0f;
    f32 speedScale = 1.0f;
};

// Rendered water surface. Base height = entity world Y, `size` = XZ extent centred on the entity (<= 0 = unbounded).
// Gameplay's WaterComponent (buoyancy) uses the same Gerstner math; worlds driven by gameplay::WorldRenderData can
// also submit surfaces directly with addWaterSurface() (features/translucency/translucency.hpp).
struct WaterSurfaceComponent {
    bool visible = true;
    std::vector<WaterWave> waves; // empty = a gentle default swell
    glm::vec2 size{200.0f, 200.0f};

    // Optics (per metre of water along the view ray).
    glm::vec3 absorption{0.45f, 0.09f, 0.06f};   // Beer-Lambert coefficients (1/m)
    glm::vec3 scatterColor{0.02f, 0.11f, 0.13f}; // in-scattered colour of deep water (albedo-like, 0..1)
    f32 refractionStrength = 0.05f;
    f32 roughness = 0.04f;
    Uuid normalMap;                   // detail normal map (invalid = procedural)
    f32 detailNormalStrength = 0.35f;
    f32 detailScale = 0.12f;          // tiles per metre
    f32 detailSpeed = 0.04f;          // m/s scroll

    // Foam.
    f32 shoreFoamDistance = 0.35f;     // metres of water depth that still show foam
    f32 crestFoam = 0.5f;             // 0 = none
    f32 foamIntensity = 1.0f;

    // Caustics on underwater geometry.
    f32 causticsIntensity = 1.0f;
    f32 causticsScale = 0.3f;         // tiles per metre
    f32 causticsFalloff = 0.25f;      // attenuation per metre of depth

    // Underwater view.
    glm::vec3 underwaterColor{0.03f, 0.16f, 0.2f};
    f32 underwaterDensity = 0.06f;    // 1/m
};

// Reflection + ComponentRegistry registration (idempotent).
void registerTranslucencyTypes();

} // namespace ox::render
