// OxwaldEngine ray tracing: scene lookup tables for hit shading, random numbers and sampling. No ray query here,
// so the RT pipeline stages (closest-hit / any-hit) include it as well. Tracing helpers: rt_query.glsl.
//
// Instance → geometry/material lookup ("bindless hit shading"):
//   gl_InstanceCustomIndexEXT / rayQueryGetIntersectionInstanceCustomIndexEXT = RtInstance index (dense TLAS order)
//   RtInstance.gpuInstance → SceneHeader.instances (world matrix), RtInstance.material → SceneHeader.materials
//   triangle k of the hit primitive: RtSceneHeader.indices[firstIndex + 3 * primitive + k] + vertexOffset
//                                   → SceneHeader.positions / attributes (shared arenas)
#ifndef OX_RT_COMMON_GLSL
#define OX_RT_COMMON_GLSL

#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

#include <render/common/scene.glsl>
#include <render/common/pbr.glsl>
#include <render/common/sky.glsl>
#include "ddgi.glsl"

// --- tables (mirror ox::render::rt::RtInstanceGpu / RtSceneHeaderGpu) ---
struct RtInstance {
    uint gpuInstance;
    uint meshInfo;
    uint material;
    uint firstIndex;
    int vertexOffset;
    uint flags; // bit 0 double sided, bit 1 skinned, bits 8-15 blend mode
    uint mask;
    uint pad;
};
OX_READONLY_BUFFER(RtInstanceBuffer, { RtInstance i[]; });
OX_READONLY_BUFFER(RtIndexBuffer, { uint i[]; });

struct RtSceneHeader {
    uint64_t tlas; // acceleration structure device address
    RtInstanceBuffer instances;
    RtIndexBuffer indices;
    uint instanceCount;
    uint frame;
    DdgiVolume ddgi; // enabled == 0 when ray traced GI is off
};
// 8-byte aligned: holds 64-bit addresses (the SPIR-V validator rejects 64-bit loads through 4-byte aligned pointers).
layout(buffer_reference, scalar, buffer_reference_align = 8) readonly buffer RtSceneBuffer { RtSceneHeader h; };

// Ray masks (ox::render::rt::RtInstanceMask).
const uint OX_RT_MASK_OPAQUE = 1u;
const uint OX_RT_MASK_ALPHA_TESTED = 2u;
const uint OX_RT_MASK_TRANSLUCENT = 4u;
const uint OX_RT_MASK_SHADOW_OPAQUE = 8u;
const uint OX_RT_MASK_SHADOW_TRANSLUCENT = 16u;
const uint OX_RT_MASK_SOLID = 3u;
const uint OX_RT_MASK_ALL = 7u;

// ShadowsRT sets the view flag OX_VIEW_RT_SHADOW_MASK_RGB (256, render/common/lighting.glsl): ShadowMask is RGBA8 with
// coloured sun visibility in rgb; ray traced local lights use GpuShadow kind 2 (screen-space visibility channel).

// --- random numbers (PCG) ---
uint oxPcgHash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
uint oxRtSeed(uvec2 pixel, uint frame, uint salt) {
    return oxPcgHash(pixel.x + oxPcgHash(pixel.y + oxPcgHash(frame + oxPcgHash(salt))));
}
float oxRtRandom(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    word = (word >> 22u) ^ word;
    return float(word >> 8) * (1.0 / 16777216.0);
}
vec2 oxRtRandom2(inout uint state) { return vec2(oxRtRandom(state), oxRtRandom(state)); }

// --- sampling ---
vec3 oxToWorld(vec3 local, vec3 n) {
    vec3 t, b;
    oxBasis(n, t, b);
    return t * local.x + b * local.y + n * local.z;
}
vec3 oxToLocal(vec3 v, vec3 n) {
    vec3 t, b;
    oxBasis(n, t, b);
    return vec3(dot(v, t), dot(v, b), dot(v, n));
}

// Cosine-weighted hemisphere around n (pdf = cosθ / π).
vec3 oxSampleCosineHemisphere(vec2 u, vec3 n) {
    float r = sqrt(u.x);
    float phi = OX_TWO_PI * u.y;
    return oxToWorld(vec3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - u.x))), n);
}

// Uniform direction inside a cone of half-angle acos(cosMax) around axis (pdf = 1 / (2π (1 - cosMax))).
vec3 oxSampleCone(vec2 u, vec3 axis, float cosMax) {
    float cosT = mix(1.0, cosMax, u.x);
    float sinT = sqrt(max(0.0, 1.0 - cosT * cosT));
    float phi = OX_TWO_PI * u.y;
    return oxToWorld(vec3(cos(phi) * sinT, sin(phi) * sinT, cosT), axis);
}

// GGX visible normal sampling (Heitz 2018). Ve: view direction in the local frame (z = normal), alpha = roughness².
vec3 oxSampleGGXVNDF(vec3 Ve, float alpha, vec2 u) {
    vec3 Vh = normalize(vec3(alpha * Ve.x, alpha * Ve.y, Ve.z));
    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    vec3 T1 = lensq > 0.0 ? vec3(-Vh.y, Vh.x, 0.0) * inversesqrt(lensq) : vec3(1.0, 0.0, 0.0);
    vec3 T2 = cross(Vh, T1);
    float r = sqrt(u.x);
    float phi = OX_TWO_PI * u.y;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s) * sqrt(max(0.0, 1.0 - t1 * t1)) + s * t2;
    vec3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;
    return normalize(vec3(alpha * Nh.x, alpha * Nh.y, max(0.0, Nh.z)));
}

// Smith G1 for GGX (separable form used by the VNDF pdf).
float oxSmithG1(float NdotV, float alpha) {
    float a2 = alpha * alpha;
    return 2.0 * NdotV / (NdotV + sqrt(a2 + (1.0 - a2) * NdotV * NdotV));
}

// pdf of a reflected direction sampled with oxSampleGGXVNDF: D_v(H) / (4 VdotH) = G1(V) D(H) / (4 NdotV).
float oxGGXVNDFPdf(float NdotV, float NdotH, float alpha) {
    return oxSmithG1(NdotV, alpha) * oxDGGX(NdotH, alpha) / max(4.0 * NdotV, 1e-6);
}

float oxPowerHeuristic(float a, float b) {
    float a2 = a * a;
    return a2 / max(a2 + b * b, 1e-12);
}

// Result of a closest-hit query (ray query or RT pipeline payload).
struct RtHit {
    bool hit;
    float t;
    uint instance; // RtInstance index
    uint primitive;
    vec2 bary;
};

// --- hit surface ---
struct RtSurface {
    vec3 position;
    vec3 geometricNormal; // facing the incoming ray
    vec3 normal;          // shading normal (normal mapped), facing the incoming ray side
    vec2 uv;
    bool frontFace;       // ray hit the CCW side (outside of a closed mesh)
    uint material;
    uint blend;
    vec4 baseColor;
    float metallic;
    float perceptualRoughness;
    vec3 emissive;
};

vec2 oxRtMaterialUv(Material m, vec2 uv) { return uv * m.uvTiling + m.uvOffset; }

vec4 oxRtSampleTex(uint tex, uint smp, vec2 uv, float lod) {
    return textureLod(sampler2D(OX_TEX2D(tex), OX_SAMPLER(smp)), uv, lod);
}

// Alpha of a material at a uv (explicit LOD: compute / ray stages have no derivatives).
float oxRtMaterialAlpha(Material m, vec2 uv0, float lod) {
    float a = m.baseColor.a;
    if (m.albedoTexture != OX_INVALID_INDEX) a *= oxRtSampleTex(m.albedoTexture, m.samplerIndex, oxRtMaterialUv(m, uv0), lod).a;
    return a;
}

// Interpolated attributes of triangle `primitive` of RtInstance `rtIndex`.
void oxRtTriangle(SceneBuffer sb, RtSceneBuffer rb, uint rtIndex, uint primitive, out RtInstance ri, out uvec3 vid) {
    ri = rb.h.instances.i[rtIndex];
    uint base = ri.firstIndex + primitive * 3u;
    RtIndexBuffer idx = rb.h.indices;
    vid = uvec3(idx.i[base], idx.i[base + 1u], idx.i[base + 2u]) + uint(ri.vertexOffset);
}

vec2 oxRtHitUv(SceneBuffer sb, RtSceneBuffer rb, uint rtIndex, uint primitive, vec2 bary) {
    RtInstance ri;
    uvec3 vid;
    oxRtTriangle(sb, rb, rtIndex, primitive, ri, vid);
    AttributeBuffer attr = sb.s.attributes;
    vec3 w = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
    return attr.a[vid.x].uv0 * w.x + attr.a[vid.y].uv0 * w.y + attr.a[vid.z].uv0 * w.z;
}

// Alpha test of a candidate hit (any-hit emulation for ray queries, and the RT pipeline's any-hit shader).
bool oxRtAlphaTestPasses(SceneBuffer sb, RtSceneBuffer rb, uint rtIndex, uint primitive, vec2 bary) {
    RtInstance ri = rb.h.instances.i[rtIndex];
    Material m = sb.s.materials.m[ri.material];
    if ((m.flags & 7u) != OX_BLEND_ALPHA_TEST) return true;
    return oxRtMaterialAlpha(m, oxRtHitUv(sb, rb, rtIndex, primitive, bary), 0.0) >= m.alphaCutoff;
}

// Full hit surface. `lod`: texture LOD (ray cone estimate, 0 = finest).
RtSurface oxRtFetchSurface(SceneBuffer sb, RtSceneBuffer rb, uint rtIndex, uint primitive, vec2 bary, vec3 rayDir,
                           float lod) {
    RtInstance ri;
    uvec3 vid;
    oxRtTriangle(sb, rb, rtIndex, primitive, ri, vid);
    mat4 world = sb.s.instances.i[ri.gpuInstance].world;
    PositionBuffer pos = sb.s.positions;
    AttributeBuffer attr = sb.s.attributes;
    vec3 p0 = pos.p[vid.x], p1 = pos.p[vid.y], p2 = pos.p[vid.z];
    VertexAttributes a0 = attr.a[vid.x], a1 = attr.a[vid.y], a2 = attr.a[vid.z];
    if ((ri.flags & 4u) != 0u) {
        // BLAS built from the compute skinning output: hit attributes from the same deformed vertices.
        uint base = sb.s.instances.i[ri.gpuInstance].paletteOffset - uint(ri.vertexOffset);
        SkinnedVertexBuffer sv = sb.s.skinnedVertices;
        SkinnedVertex s0 = sv.v[base + vid.x], s1 = sv.v[base + vid.y], s2 = sv.v[base + vid.z];
        p0 = s0.position;
        p1 = s1.position;
        p2 = s2.position;
        a0.normal = s0.normal;
        a1.normal = s1.normal;
        a2.normal = s2.normal;
        a0.tangent = s0.tangent;
        a1.tangent = s1.tangent;
        a2.tangent = s2.tangent;
    }
    vec3 w = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);

    RtSurface s;
    vec3 lp = p0 * w.x + p1 * w.y + p2 * w.z;
    s.position = (world * vec4(lp, 1.0)).xyz;
    mat3 cof = oxCofactor(world);
    vec3 gn = normalize(cof * cross(p1 - p0, p2 - p0));
    vec3 n = normalize(cof * (a0.normal * w.x + a1.normal * w.y + a2.normal * w.z));
    if (dot(gn, n) < 0.0) gn = -gn; // orient the geometric normal like the vertex normals (outside)
    s.frontFace = dot(gn, rayDir) < 0.0;
    vec4 T = a0.tangent * w.x + a1.tangent * w.y + a2.tangent * w.z;
    T.xyz = mat3(world) * T.xyz;
    s.uv = a0.uv0 * w.x + a1.uv0 * w.y + a2.uv0 * w.z;
    vec4 color = oxUnpackColor(a0.color) * w.x + oxUnpackColor(a1.color) * w.y + oxUnpackColor(a2.color) * w.z;

    Material m = sb.s.materials.m[ri.material];
    s.material = ri.material;
    s.blend = m.flags & 7u;
    vec2 uv = oxRtMaterialUv(m, s.uv);
    s.baseColor = m.baseColor * color;
    if (m.albedoTexture != OX_INVALID_INDEX) s.baseColor *= oxRtSampleTex(m.albedoTexture, m.samplerIndex, uv, lod);
    s.metallic = m.metallic;
    s.perceptualRoughness = m.roughness;
    if (m.ormTexture != OX_INVALID_INDEX) {
        vec3 orm = oxRtSampleTex(m.ormTexture, m.samplerIndex, uv, lod).rgb;
        s.perceptualRoughness *= orm.g;
        s.metallic *= orm.b;
    }
    s.emissive = m.emissive;
    if (m.emissiveTexture != OX_INVALID_INDEX) s.emissive *= oxRtSampleTex(m.emissiveTexture, m.samplerIndex, uv, lod).rgb;
    if (m.normalTexture != OX_INVALID_INDEX && dot(T.xyz, T.xyz) > 1e-8) {
        vec2 xy = oxRtSampleTex(m.normalTexture, m.samplerIndex, uv, lod).rg * 2.0 - 1.0;
        xy *= m.normalStrength;
        vec3 tn = vec3(xy, sqrt(clamp(1.0 - dot(xy, xy), 0.0, 1.0)));
        vec3 t = normalize(T.xyz - n * dot(n, T.xyz));
        vec3 b = cross(n, t) * (T.w < 0.0 ? -1.0 : 1.0);
        n = normalize(t * tn.x + b * tn.y + n * tn.z);
    }
    // Face the incoming ray (double-sided materials and back faces of thin geometry).
    if (!s.frontFace) {
        gn = -gn;
        n = -n;
    }
    s.geometricNormal = gn;
    s.normal = dot(n, gn) < 0.0 ? reflect(n, gn) : n;
    return s;
}

// Surface → OxSurface for the shared BRDF code. V: direction towards the viewer / previous vertex.
OxSurface oxRtToSurface(RtSurface h, vec3 V) {
    OxSurface s;
    s.position = h.position;
    s.normal = h.normal;
    s.geometricNormal = h.geometricNormal;
    s.view = V;
    s.baseColor = h.baseColor.rgb;
    s.alpha = h.baseColor.a;
    s.metallic = clamp(h.metallic, 0.0, 1.0);
    s.perceptualRoughness = h.perceptualRoughness;
    s.occlusion = 1.0;
    s.emissive = h.emissive;
    oxSurfaceFinalize(s);
    return s;
}

// Material emissive is display-relative (1 = white at the current exposure): radiance = emissive / exposure.
vec3 oxRtEmissiveRadiance(ViewBuffer vb, vec3 emissive) { return emissive / max(vb.v.exposure, 1e-12); }

// Diffuse indirect at a hit: DDGI when the GI volume is active, else the SH9 sky irradiance ("probe fallback").
vec3 oxRtIndirectDiffuse(ViewBuffer vb, RtSceneBuffer rb, vec3 P, vec3 N, vec3 V) {
    if (rb.h.ddgi.enabled != 0u) {
        DdgiVolume v = rb.h.ddgi;
        ivec3 c = ivec3(floor(P / v.spacing - 0.5));
        if (all(greaterThanEqual(c, v.minCoord)) && all(lessThan(c + 1, v.minCoord + v.counts))) {
            return oxDdgiIrradiance(v, P, N, V);
        }
    }
    vec4 sh[9];
    OxSHBuffer shb = vb.v.irradianceSH;
    for (int k = 0; k < 9; ++k) sh[k] = shb.c[k];
    return oxEvalSH9(sh, N) * vb.v.iblIntensity;
}

vec3 oxRtPrefilteredSpecular(ViewBuffer vb, vec3 R, float perceptualRoughness) {
    uint cube = vb.v.prefilteredCube;
    if (cube == OX_INVALID_INDEX) return vec3(0.0);
    float lod = perceptualRoughness * float(vb.v.prefilteredMips - 1u);
    return textureLod(samplerCube(OX_TEXCUBE(cube), OX_SAMPLER(OX_SAMPLER_LINEAR_CLAMP)), R, lod).rgb * vb.v.iblIntensity;
}

// Radiance of a ray that left the scene.
vec3 oxRtMissRadiance(ViewBuffer vb, SceneBuffer sb, vec3 dir, bool sunDisk) { return oxSkyRadiance(vb, sb, dir, sunDisk); }

// Beer-Lambert transmittance (absorptionColor reached after absorptionDistance metres).
vec3 oxRtBeerLambert(vec3 absorptionColor, float absorptionDistance, float distance) {
    if (absorptionDistance <= 0.0) return vec3(1.0);
    return pow(max(absorptionColor, vec3(1e-4)), vec3(max(distance, 0.0) / absorptionDistance));
}

float oxRtTransmission(Material m) { return m.transmission > 0.0 ? clamp(m.transmission, 0.0, 1.0) : 1.0; }

// Throughput of a shadow ray crossing a translucent surface (front faces only; thickness from the material).
vec3 oxRtSurfaceTransmittance(Material m, vec4 baseColor) {
    uint blend = m.flags & 7u;
    if (blend == OX_BLEND_TRANSPARENT) return mix(vec3(1.0), baseColor.rgb, baseColor.a) * (1.0 - baseColor.a);
    float thickness = m.thickness > 0.0 ? m.thickness : 0.01;
    return baseColor.rgb * oxRtTransmission(m) * oxRtBeerLambert(m.absorptionColor, m.absorptionDistance, thickness) * 0.92;
}

// Ray cone texture LOD estimate (Akenine-Möller et al. 2019, simplified: no curvature).
float oxRtConeLod(float distance, float spreadAngle, float texelsPerMetre) {
    float width = distance * spreadAngle;
    return max(log2(max(width * texelsPerMetre, 1e-6)), 0.0);
}

// Direction towards the sun sampled inside its angular radius (soft shadows), pdf uniform over the cone.
vec3 oxRtSampleSun(ViewBuffer vb, Light sun, vec2 u) {
    float radius = max(vb.v.sunAngularRadius, 0.0);
    vec3 L = -sun.direction;
    if (radius <= 0.0) return L;
    return oxSampleCone(u, L, cos(radius));
}

// Point on a spherical light (sourceRadius) as seen from P: cone sampling of the subtended solid angle.
vec3 oxRtSampleSphereLight(vec3 P, Light l, vec2 u, out float dist, out float solidAngle) {
    vec3 toL = l.position - P;
    float d = length(toL);
    vec3 axis = toL / max(d, 1e-5);
    float r = l.sourceRadius;
    if (r <= 0.0 || d <= r) {
        dist = d;
        solidAngle = 0.0;
        return axis;
    }
    float sinMax = r / d;
    float cosMax = sqrt(max(0.0, 1.0 - sinMax * sinMax));
    vec3 L = oxSampleCone(u, axis, cosMax);
    // Distance to the sphere surface along L.
    float b = dot(toL, L);
    float c = dot(toL, toL) - r * r;
    dist = b - sqrt(max(b * b - c, 0.0));
    solidAngle = OX_TWO_PI * (1.0 - cosMax);
    return L;
}

// Unshadowed radiance scale of a local light (attenuation × spot) towards P along L (surface → light).
float oxRtLocalAttenuation(Light l, vec3 P, vec3 L) {
    vec3 toL = l.position - P;
    float d2 = dot(toL, toL);
    if (d2 > l.range * l.range) return 0.0;
    float att = oxDistanceAttenuation(d2, l.range);
    if (l.type == OX_LIGHT_SPOT) att *= oxSpotAttenuation(normalize(toL), l.direction, l.spotScale, l.spotOffset);
    return att;
}

// Offset a ray origin off a surface (Wächter & Binder 2019, simplified scale-aware epsilon).
vec3 oxRtOffsetRay(vec3 p, vec3 n) {
    float scale = max(max(abs(p.x), abs(p.y)), max(abs(p.z), 1.0));
    return p + n * (1e-3 * scale);
}

#endif
