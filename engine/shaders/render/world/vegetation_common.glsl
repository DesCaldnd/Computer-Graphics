// OxwaldEngine render / world: vegetation data (mirrors features/world/vegetation.cpp), wind and interaction.
#ifndef OX_RENDER_WORLD_VEGETATION_COMMON_GLSL
#define OX_RENDER_WORLD_VEGETATION_COMMON_GLSL

// world_common (math.glsl declares OX_TWO_PI as a constant) must come before wind.glsl (which #defines it).
#include "world_common.glsl"
#include <world/wind.glsl>

// world::VegetationInstanceGpu (64 B): rows of the 3x4 object→world matrix.
struct VegInstance {
    vec4 t0;
    vec4 t1;
    vec4 t2;
    uint tint;
    float random;
    uint prototypeLayerFlags;
    float boundingRadius;
};

const uint OX_VEG_KIND_GRASS = 0u;
const uint OX_VEG_KIND_DETAIL = 1u;
const uint OX_VEG_KIND_TREE = 2u;

struct VegFrame {
    vec4 wind[2];        // OxWind at `time`
    vec4 windPrev[2];    // OxWind at `prevTime` (motion vectors)
    vec4 interactors[8]; // xyz position, w radius
    uint interactorCount;
    float time;
    float prevTime;
    uint hasWind;
};

struct VegProto {
    vec4 wind;     // sway, flutter, translucency, object height (m, unscaled)
    vec4 impostor; // frames per side, bounding radius (object), centre height (object), alpha cutoff
    uint albedoAtlas;
    uint normalAtlas;
    uint kind;     // OX_VEG_KIND_*
    uint pad;
};

OX_READONLY_BUFFER(VegInstanceBuffer, { VegInstance i[]; });
OX_READONLY_BUFFER(VegRecordBuffer, { uvec2 r[]; }); // x = instance index, y = fade code (see oxWorldDitherKeep)
OX_READONLY_BUFFER(VegFrameBuffer, { VegFrame f; });
OX_READONLY_BUFFER(VegProtoBuffer, { VegProto p[]; });

mat4 oxVegMatrix(VegInstance inst) { return transpose(mat4(inst.t0, inst.t1, inst.t2, vec4(0.0, 0.0, 0.0, 1.0))); }

OxWind oxVegWind(VegFrameBuffer fb, bool previous) {
    OxWind w;
    w.dirSpeedTime = previous ? fb.f.windPrev[0] : fb.f.wind[0];
    w.gust = previous ? fb.f.windPrev[1] : fb.f.wind[1];
    return w;
}

// Wind + interaction displacement of a world-space vertex.
//   local: object-space position (y = height above the base), worldBase: instance origin, heightWorld: scaled height
//   masks: vertex colour (r branch flutter, g leaf flutter, b phase), n: world normal.
vec3 oxVegAnimate(VegFrameBuffer fb, VegProto proto, vec3 world, vec3 local, vec3 worldBase, float heightWorld,
                  vec4 masks, vec3 n, float rnd, bool previous) {
    float t = previous ? fb.f.prevTime : fb.f.time;
    float h = clamp(local.y / max(proto.wind.w, 1e-3), 0.0, 1.5);
    vec3 offset = vec3(0.0);
    if (fb.f.hasWind != 0u) {
        OxWind w = oxVegWind(fb, previous);
        vec3 v = oxWindSample(w, worldBase); // m/s, horizontal
        float speed = length(v);
        vec3 dir = speed > 1e-4 ? v / speed : vec3(1.0, 0.0, 0.0);
        bool grass = proto.kind == OX_VEG_KIND_GRASS;
        // Trunk / stem sway: static lean + oscillation, quadratic along the height.
        float lean = speed * (grass ? 0.045 : 0.012);
        float osc = sin(t * (grass ? 2.4 : 1.1) + rnd * 6.2831) * speed * (grass ? 0.03 : 0.008);
        float sway = (lean + osc) * proto.wind.x * h * h * heightWorld;
        offset += dir * sway;
        // Branch / leaf flutter along the normal, phase from the vertex colour and position.
        float phase = masks.b * 6.2831 + rnd * 12.0 + dot(world, vec3(0.37, 0.21, 0.29));
        float flutter = (masks.r * 0.035 * sin(t * 4.3 + phase) + masks.g * 0.02 * sin(t * 11.7 + phase * 1.7)) *
                        proto.wind.y * min(speed * 0.25, 2.0) * heightWorld * 0.25;
        offset += n * flutter;
    }
    // Grass bends away from interactors (characters).
    if (proto.kind == OX_VEG_KIND_GRASS) {
        for (uint i = 0u; i < min(fb.f.interactorCount, 8u); ++i) {
            vec4 it = fb.f.interactors[i];
            vec2 d = worldBase.xz - it.xz;
            float dist = length(d);
            float f = clamp(1.0 - dist / max(it.w, 1e-3), 0.0, 1.0) * step(abs(world.y - it.y), it.w + heightWorld);
            if (f > 0.0) {
                vec2 away = dist > 1e-3 ? d / dist : vec2(1.0, 0.0);
                offset.xz += away * f * h * heightWorld * 0.8;
                offset.y -= f * f * h * heightWorld * 0.5;
            }
        }
    }
    // Keep the stem length roughly constant: lower the vertex by the horizontal displacement.
    float horiz = dot(offset.xz, offset.xz);
    offset.y -= horiz / max(2.0 * max(local.y, 0.05) * (heightWorld / max(proto.wind.w, 1e-3)), 1e-3) * step(0.0, local.y);
    return world + offset;
}

// Foliage shading normal. Cards carry canopy-outward normals (volumetric look); flipping them on back faces would
// point half the leaves into the crown. Instead the normal is mirrored onto the viewer's side of the card plane
// (faceN = geometric normal of the card), kept away from grazing (N·V ≥ 0.3), and when the sun (Ls = direction
// towards it, zero = none) is on the other side of the card the sun component is removed: a back-lit leaf receives
// the sun only through the translucency term, never as a (grazing, Fresnel-boosted) reflection.
vec3 oxFoliageNormal(vec3 n, vec3 faceN, vec3 V, vec3 Ls) {
    n = normalize(n);
    if (dot(faceN, V) < 0.0) faceN = -faceN;
    float d = dot(n, faceN);
    if (d < 0.0) n -= 2.0 * d * faceN;
    if (dot(faceN, Ls) < 0.0) {
        float nl = dot(n, Ls);
        if (nl > 0.0) n -= Ls * (nl * 1.02);
    }
    n = normalize(n);
    float nv = dot(n, V);
    return nv < 0.3 ? normalize(n + V * (0.3 - nv)) : n;
}

vec3 oxSunDirection(ViewBuffer vb, SceneBuffer sb) {
    int sun = vb.v.sunLight;
    return sun >= 0 ? -sb.s.lights.l[sun].direction : vec3(0.0);
}

// Hemi-octahedral mapping (upper hemisphere) for impostor frames.
vec2 oxHemiOctEncode(vec3 d) {
    d.y = max(d.y, 0.0);
    d /= (abs(d.x) + abs(d.z) + d.y);
    return vec2(d.x + d.z, d.x - d.z);
}
vec3 oxHemiOctDecode(vec2 e) {
    vec2 t = vec2(e.x + e.y, e.x - e.y) * 0.5;
    return normalize(vec3(t.x, 1.0 - abs(t.x) - abs(t.y), t.y));
}
// Billboard basis of an impostor frame looking along -d (shared by the bake and the runtime).
void oxImpostorBasis(vec3 d, out vec3 right, out vec3 up) {
    vec3 refUp = abs(d.y) > 0.999 ? vec3(0.0, 0.0, -1.0) : vec3(0.0, 1.0, 0.0);
    right = normalize(cross(refUp, d));
    up = cross(d, right);
}

#endif
