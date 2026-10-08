#version 460
// Particles: camera-facing billboards, velocity-stretched billboards, or meshes (OX_PARTICLE_MESH, indexed draw of
// the mesh with one instance per particle). One instance per alive particle; the instance count comes from the
// simulation (vkCmdDrawIndirect / vkCmdDrawIndexedIndirect with GPU-written args, no drawIndirectCount).
// Lighting (lit emitters) is evaluated per vertex: sun with CSM shadow, clustered local lights, SH ambient.
#include "particle_push.glsl"
#include <render/common/lighting.glsl>
#include "fog.glsl"

layout(location = 0) out vec4 vColor;      // rgb radiance (not pre-exposed), a opacity
layout(location = 1) out vec4 vUv;         // xy frame A, zw frame B
layout(location = 2) out vec4 vFog;        // rgb in-scatter, a transmittance
layout(location = 3) flat out vec4 vParams; // x view depth, y soft distance, z frame blend, w blend mode

vec3 particleLight(vec3 P, vec3 N, bool useNormal, vec2 pixel, float viewDepth, vec3 V) {
    vec3 E = vec3(0.0);
    LightBuffer lights = SCENE.lights;
    uint dirCount = SCENE.directionalLightCount;
    for (uint i = 0u; i < dirCount; ++i) {
        Light l = lights.l[i];
        vec3 L = -l.direction;
        float ndl = useNormal ? max(dot(N, L), 0.0) : 0.5;
        float vis = int(i) == VIEW.sunLight ? oxSunShadow(pc.view, P, L, viewDepth, pixel) : 1.0;
        E += l.color * ndl * vis;
    }
    if (VIEW.lightClusterCount > 0u) {
        uint cluster = oxClusterIndex(pc.view, pixel * VIEW.renderSize.zw, viewDepth);
        uint count = oxClusterLightCount(pc.view, cluster);
        for (uint i = 0u; i < count; ++i) {
            Light l = lights.l[oxClusterLight(pc.view, cluster, i)];
            vec3 toLight = l.position - P;
            float d2 = dot(toLight, toLight);
            if (d2 > l.range * l.range) continue;
            vec3 L = toLight * inversesqrt(max(d2, 1e-8));
            float att = oxDistanceAttenuation(d2, l.range);
            if (l.type == OX_LIGHT_SPOT) att *= oxSpotAttenuation(L, l.direction, l.spotScale, l.spotOffset);
            float ndl = useNormal ? max(dot(N, L), 0.0) : 0.5;
            E += l.color * att * ndl;
        }
    }
    vec4 sh[9];
    OxSHBuffer shb = VIEW.irradianceSH;
    for (int k = 0; k < 9; ++k) sh[k] = shb.c[k];
    vec3 ambient = oxEvalSH9(sh, useNormal ? N : V) * VIEW.iblIntensity;
    return E * OX_INV_PI + ambient;
}

vec2 atlasUv(vec2 uv, float frame) {
    float cols = max(EM.flipbook.x, 1.0), rows = max(EM.flipbook.y, 1.0);
    float f = mod(frame, cols * rows);
    vec2 cell = vec2(mod(f, cols), floor(f / cols));
    return (cell + uv) / vec2(cols, rows);
}

void main() {
    uint slot = pc.lists.v[pc.listOffset + uint(gl_InstanceIndex) * pc.listStride + pc.listSlot];
    Particle p = pc.particles.p[slot];
    float t = clamp(p.age / max(p.lifetime, 1e-4), 0.0, 1.0);
    vec4 grad = vec4(1.0);
    float sizeMul = 1.0;
    if (EM.gradient != OX_INVALID_INDEX) {
        grad = OX_SAMPLE_2D_LOD(EM.gradient, OX_SAMPLER_LINEAR_CLAMP, vec2(t, 0.25), 0.0);
        sizeMul = OX_SAMPLE_2D_LOD(EM.gradient, OX_SAMPLER_LINEAR_CLAMP, vec2(t, 0.75), 0.0).r;
    }
    float size = max(p.size * sizeMul, 0.0);
    if (p.lifetime <= 0.0) size = 0.0;
    vec3 cam = VIEW.cameraPosition.xyz;
    vec3 V = normalize(cam - p.position);
    vec3 worldPos;
    vec3 N = V;
    bool useNormal = false;
    vec2 cornerUv = vec2(0.5);

#ifdef OX_PARTICLE_MESH
    vec3 lp = SCENE.positions.p[uint(gl_VertexIndex)];
    vec3 ln = SCENE.attributes.a[uint(gl_VertexIndex)].normal;
    float c1 = cos(p.rotation), s1 = sin(p.rotation);
    float c2 = cos(p.rotation * 0.7 + 1.3), s2 = sin(p.rotation * 0.7 + 1.3);
    mat3 ry = mat3(c1, 0.0, -s1, 0.0, 1.0, 0.0, s1, 0.0, c1);
    mat3 rx = mat3(1.0, 0.0, 0.0, 0.0, c2, s2, 0.0, -s2, c2);
    mat3 rot = ry * rx;
    worldPos = p.position + rot * (lp * size);
    N = normalize(rot * ln);
    useNormal = true;
    cornerUv = SCENE.attributes.a[uint(gl_VertexIndex)].uv0;
#else
    const vec2 kCorners[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
    vec2 c = kCorners[gl_VertexIndex % 6];
    cornerUv = c * vec2(0.5, -0.5) + 0.5;
    vec3 right, up;
    if (oxParticleRenderMode(EM) == 1u && dot(p.velocity, p.velocity) > 1e-6) {
        vec3 dir = p.velocity - dot(p.velocity, V) * V;
        float len = length(dir);
        up = len > 1e-5 ? dir / len : vec3(0.0, 1.0, 0.0);
        right = normalize(cross(up, V));
        worldPos = p.position + right * c.x * size * 0.5 + up * c.y * (size * 0.5 + length(p.velocity) * EM.rotation.z);
    } else {
        vec3 camRight = VIEW.invView[0].xyz, camUp = VIEW.invView[1].xyz;
        float cr = cos(p.rotation), sr = sin(p.rotation);
        right = camRight * cr + camUp * sr;
        up = -camRight * sr + camUp * cr;
        worldPos = p.position + (right * c.x + up * c.y) * size * 0.5;
    }
#endif

    vec4 clip = VIEW.viewProj * vec4(worldPos, 1.0);
    gl_Position = clip;
    float viewDepth = max(-(VIEW.view * vec4(worldPos, 1.0)).z, 1e-4);
    vec2 ndc = clip.xy / max(clip.w, 1e-6);
    vec2 pixel = clamp(ndc * 0.5 + 0.5, vec2(0.0), vec2(1.0)) * VIEW.renderSize.xy;

    vec4 base = EM.color * grad;
    float invExposure = 1.0 / max(VIEW.exposure, 1e-12);
    vec3 radiance = base.rgb * EM.collision.w * invExposure;
    if ((EM.flags & OX_PARTICLE_LIT) != 0u) radiance += base.rgb * particleLight(p.position, N, useNormal, pixel, viewDepth, V);
    vColor = vec4(radiance, base.a);

    float frames = max(EM.flipbook.x * EM.flipbook.y, 1.0);
    float frame = (EM.flipbook.z > 0.0 ? p.age * EM.flipbook.z : t * frames) + p.frameOffset;
    if (EM.flipbook.z <= 0.0 && EM.flipbook.w < 0.5) frame = min(frame, frames - 1.0);
    vUv = vec4(atlasUv(cornerUv, floor(frame)), atlasUv(cornerUv, floor(frame) + 1.0));
    vFog = oxTranslucencyFogFactors(pc.view, pc.scene, p.position, pixel * VIEW.renderSize.zw, pc.fog);
    vParams = vec4(viewDepth, EM.rotation.w, fract(frame), float(oxParticleBlend(EM)));
}
