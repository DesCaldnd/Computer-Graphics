// OxwaldEngine ray tracing: ray query tracing helpers (compute shaders) and simple hit lighting.
//   oxRtTraceClosest   closest hit with alpha-tested any-hit emulation (candidate loop)
//   oxRtVisibility     any-hit occlusion test (shadow / AO rays), alpha tested
//   oxRtTransmittance  coloured transmission through translucent shadow casters
//   oxRtShadeHit       direct (sun + one stochastic local light, both shadowed) + emissive + DDGI/SH indirect +
//                      prefiltered specular fallback: the "simple hit lighting" of reflections / GI / translucency
#ifndef OX_RT_QUERY_GLSL
#define OX_RT_QUERY_GLSL

#extension GL_EXT_ray_query : require

#include "rt_common.glsl"

accelerationStructureEXT oxRtTlas(RtSceneBuffer rb) { return accelerationStructureEXT(rb.h.tlas); }

RtHit oxRtTraceClosest(SceneBuffer sb, RtSceneBuffer rb, vec3 origin, vec3 dir, float tmin, float tmax, uint mask) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, oxRtTlas(rb), gl_RayFlagsNoneEXT, mask, origin, tmin, dir, tmax);
    while (rayQueryProceedEXT(rq)) {
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
            uint inst = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false);
            uint prim = rayQueryGetIntersectionPrimitiveIndexEXT(rq, false);
            vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rq, false);
            if (oxRtAlphaTestPasses(sb, rb, inst, prim, bary)) rayQueryConfirmIntersectionEXT(rq);
        }
    }
    RtHit h;
    h.hit = rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT;
    h.t = tmax;
    h.instance = 0u;
    h.primitive = 0u;
    h.bary = vec2(0.0);
    if (h.hit) {
        h.t = rayQueryGetIntersectionTEXT(rq, true);
        h.instance = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
        h.primitive = rayQueryGetIntersectionPrimitiveIndexEXT(rq, true);
        h.bary = rayQueryGetIntersectionBarycentricsEXT(rq, true);
    }
    return h;
}

// 1 = unoccluded. Terminates on the first confirmed hit.
float oxRtVisibility(SceneBuffer sb, RtSceneBuffer rb, vec3 origin, vec3 dir, float tmin, float tmax, uint mask) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, oxRtTlas(rb), gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT, mask,
                          origin, tmin, dir, tmax);
    while (rayQueryProceedEXT(rq)) {
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
            uint inst = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false);
            uint prim = rayQueryGetIntersectionPrimitiveIndexEXT(rq, false);
            vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rq, false);
            if (oxRtAlphaTestPasses(sb, rb, inst, prim, bary)) rayQueryConfirmIntersectionEXT(rq);
        }
    }
    return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}

// Product of the transmittances of every translucent caster crossed (front faces), unordered.
vec3 oxRtTransmittance(SceneBuffer sb, RtSceneBuffer rb, vec3 origin, vec3 dir, float tmin, float tmax) {
    vec3 T = vec3(1.0);
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, oxRtTlas(rb), gl_RayFlagsNoneEXT, OX_RT_MASK_SHADOW_TRANSLUCENT, origin, tmin, dir, tmax);
    while (rayQueryProceedEXT(rq)) {
        if (rayQueryGetIntersectionTypeEXT(rq, false) != gl_RayQueryCandidateIntersectionTriangleEXT) continue;
        if (!rayQueryGetIntersectionFrontFaceEXT(rq, false)) continue;
        uint inst = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false);
        uint prim = rayQueryGetIntersectionPrimitiveIndexEXT(rq, false);
        vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rq, false);
        RtInstance ri = rb.h.instances.i[inst];
        Material m = sb.s.materials.m[ri.material];
        vec4 base = m.baseColor;
        if (m.albedoTexture != OX_INVALID_INDEX) {
            base *= oxRtSampleTex(m.albedoTexture, m.samplerIndex, oxRtMaterialUv(m, oxRtHitUv(sb, rb, inst, prim, bary)), 0.0);
        }
        T *= oxRtSurfaceTransmittance(m, base);
        if (max(T.r, max(T.g, T.b)) < 1e-3) break;
    }
    return T;
}

// Visibility towards a light: opaque casters (binary) × translucent casters (coloured) when `colored`.
vec3 oxRtShadow(SceneBuffer sb, RtSceneBuffer rb, vec3 origin, vec3 dir, float tmax, bool colored) {
    float v = oxRtVisibility(sb, rb, origin, dir, 0.0, tmax, OX_RT_MASK_SHADOW_OPAQUE);
    if (v <= 0.0 || !colored) return vec3(v);
    return oxRtTransmittance(sb, rb, origin, dir, 0.0, tmax);
}

// Direct lighting at a hit: sun (shadow ray) + one uniformly picked local light (shadow ray, weight = count).
vec3 oxRtDirectLighting(ViewBuffer vb, SceneBuffer sb, RtSceneBuffer rb, OxSurface s, inout uint rng, bool colored) {
    vec3 result = vec3(0.0);
    vec3 origin = oxRtOffsetRay(s.position, s.geometricNormal);
    int sun = vb.v.sunLight;
    LightBuffer lights = sb.s.lights;
    if (sun >= 0) {
        Light l = lights.l[sun];
        vec3 L = oxRtSampleSun(vb, l, oxRtRandom2(rng));
        if (dot(L, s.geometricNormal) > 0.0) {
            vec3 brdf = oxBrdfDirect(s, L, vec3(1.0));
            if (dot(brdf, brdf) > 0.0) result += brdf * l.color * oxRtShadow(sb, rb, origin, L, 1e4, colored);
        }
    }
    uint first = sb.s.directionalLightCount;
    uint count = sb.s.lightCount > first ? sb.s.lightCount - first : 0u;
    if (count > 0u) {
        uint li = first + min(uint(oxRtRandom(rng) * float(count)), count - 1u);
        Light l = lights.l[li];
        float dist, solidAngle;
        vec3 L = oxRtSampleSphereLight(s.position, l, oxRtRandom2(rng), dist, solidAngle);
        float att = oxRtLocalAttenuation(l, s.position, L);
        if (att > 0.0 && dot(L, s.geometricNormal) > 0.0) {
            vec3 brdf = oxBrdfDirect(s, L, vec3(1.0));
            if (dot(brdf, brdf) > 0.0) {
                result += brdf * l.color * att * float(count) *
                          oxRtShadow(sb, rb, origin, L, max(dist - 1e-3, 0.0), colored);
            }
        }
    }
    return result;
}

// "Simple hit lighting" for secondary rays. V: towards the ray origin.
vec3 oxRtShadeHit(ViewBuffer vb, SceneBuffer sb, RtSceneBuffer rb, RtSurface h, vec3 V, inout uint rng, bool colored) {
    OxSurface s = oxRtToSurface(h, V);
    vec3 c = oxRtDirectLighting(vb, sb, rb, s, rng, colored);
    c += oxRtEmissiveRadiance(vb, s.emissive);
    vec3 irr = oxRtIndirectDiffuse(vb, rb, s.position, s.normal, V);
    vec3 F = oxFSchlickRoughness(s.NdotV, s.f0, s.perceptualRoughness);
    c += irr * s.diffuseColor * (1.0 - F);
    c += oxRtPrefilteredSpecular(vb, reflect(-V, s.normal), s.perceptualRoughness) * F;
    return c;
}

#endif
