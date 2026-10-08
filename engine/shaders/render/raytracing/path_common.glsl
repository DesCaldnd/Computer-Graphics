// OxwaldEngine ray tracing: reference path tracer integrator, shared by the ray query megakernel (path_trace.comp)
// and the RT pipeline ray generation shader (path_trace.rgen). The including shader defines, before including:
//   RtHit ptTraceClosest(vec3 origin, vec3 dir, float tmax, uint mask);
//   vec3  ptShadow(vec3 origin, vec3 dir, float tmax);        // opaque visibility × coloured transmittance
// and the push block fields used here: view, scene, rt, width, height, frame, maxBounces.
//
// Model: GGX (VNDF sampling) + Lambert metallic-roughness BRDF; smooth dielectric for refractive / transparent
// materials (Fresnel-weighted reflect / refract choice, Beer-Lambert absorption inside); emissive surfaces and the sky
// via BSDF sampling; the sun (cone of its angular radius) with next-event estimation + BSDF sampling combined by the
// power heuristic (MIS); point / spot lights (spheres of sourceRadius) by next-event estimation only (they are not
// geometry); Russian roulette from the 4th bounce.
#ifndef OX_RT_PATH_COMMON_GLSL
#define OX_RT_PATH_COMMON_GLSL

float ptSpecProbability(OxSurface s) {
    vec3 F = oxFSchlick(s.NdotV, s.f0);
    float spec = oxLuminance(F);
    float diff = oxLuminance(s.diffuseColor * (1.0 - F));
    return clamp(spec / max(spec + diff, 1e-4), 0.1, 0.9);
}

// Mixture pdf of the BSDF sampling strategy for direction L.
float ptBsdfPdf(OxSurface s, vec3 L, float pSpec) {
    float NdotL = dot(s.normal, L);
    if (NdotL <= 0.0) return 0.0;
    vec3 H = normalize(s.view + L);
    float pdfSpec = oxGGXVNDFPdf(s.NdotV, max(dot(s.normal, H), 0.0), s.roughness);
    float pdfDiff = NdotL * OX_INV_PI;
    return pSpec * pdfSpec + (1.0 - pSpec) * pdfDiff;
}

// Sun as a spherical cap: radiance inside the cone and its solid angle.
bool ptSun(out vec3 dir, out float cosMax, out vec3 radiance, out float solidAngle) {
    int sun = VIEW.sunLight;
    if (sun < 0) return false;
    Light l = SCENE.lights.l[sun];
    float r = max(VIEW.sunAngularRadius, 0.0045);
    dir = -l.direction;
    cosMax = cos(r);
    solidAngle = OX_TWO_PI * (1.0 - cosMax);
    radiance = l.color / solidAngle;
    return true;
}

vec3 ptNextEvent(OxSurface s, vec3 origin, float pSpec, inout uint rng) {
    vec3 result = vec3(0.0);
    vec3 sunDir, sunL;
    float cosMax, omega;
    if (ptSun(sunDir, cosMax, sunL, omega)) {
        vec3 L = oxSampleCone(oxRtRandom2(rng), sunDir, cosMax);
        if (dot(L, s.geometricNormal) > 0.0) {
            vec3 f = oxBrdfDirect(s, L, vec3(1.0)); // BRDF × NdotL
            if (dot(f, f) > 0.0) {
                float pdfLight = 1.0 / omega;
                float w = oxPowerHeuristic(pdfLight, ptBsdfPdf(s, L, pSpec));
                result += f * sunL / pdfLight * w * ptShadow(origin, L, 1e4);
            }
        }
    }
    uint first = SCENE.directionalLightCount;
    uint count = SCENE.lightCount > first ? SCENE.lightCount - first : 0u;
    if (count > 0u) {
        uint li = first + min(uint(oxRtRandom(rng) * float(count)), count - 1u);
        Light l = SCENE.lights.l[li];
        float dist, solidAngle;
        vec3 L = oxRtSampleSphereLight(s.position, l, oxRtRandom2(rng), dist, solidAngle);
        float att = oxRtLocalAttenuation(l, s.position, L);
        if (att > 0.0 && dot(L, s.geometricNormal) > 0.0) {
            vec3 f = oxBrdfDirect(s, L, vec3(1.0));
            if (dot(f, f) > 0.0) result += f * l.color * att * float(count) * ptShadow(origin, L, max(dist - 1e-3, 0.0));
        }
    }
    return result;
}

float ptFresnelDielectric(float cosI, float eta) {
    float sin2T = eta * eta * (1.0 - cosI * cosI);
    if (sin2T >= 1.0) return 1.0;
    float f0 = (1.0 - eta) / (1.0 + eta);
    f0 *= f0;
    return f0 + (1.0 - f0) * oxPow5(1.0 - cosI);
}

// One path through pixel p (sub-pixel jittered). Returns radiance (not pre-exposed).
vec3 ptTracePath(ivec2 p, inout uint rng) {
    vec2 uv = (vec2(p) + oxRtRandom2(rng)) * VIEW.renderSize.zw;
    vec3 origin = oxWorldPositionFromDepth(pc.view, uv, 1.0);
    vec3 dir = oxViewRay(pc.view, uv);
    vec3 throughput = vec3(1.0);
    vec3 radiance = vec3(0.0);
    float lastPdf = 0.0;   // BSDF pdf of the previous bounce (0 = delta / camera ray)
    bool insideMedium = false;
    Material medium;
    float maxLum = 64.0 / max(VIEW.exposure, 1e-12); // firefly clamp for indirect paths

    for (uint bounce = 0u; bounce <= pc.maxBounces; ++bounce) {
        RtHit h = ptTraceClosest(origin, dir, 1e4, OX_RT_MASK_ALL);
        if (!h.hit) {
            vec3 sky = oxRtMissRadiance(pc.view, pc.scene, dir, false);
            vec3 sunDir, sunL;
            float cosMax, omega;
            if (ptSun(sunDir, cosMax, sunL, omega) && dot(dir, sunDir) >= cosMax) {
                // BSDF sample hit the sun: MIS against next-event estimation (full weight after delta events).
                float w = lastPdf > 0.0 ? oxPowerHeuristic(lastPdf, 1.0 / omega) : 1.0;
                sky += sunL * w;
            }
            vec3 c = throughput * sky;
            if (bounce > 0u && oxLuminance(c) > maxLum) c *= maxLum / oxLuminance(c);
            radiance += c;
            break;
        }
        RtSurface hs = oxRtFetchSurface(pc.scene, pc.rt, h.instance, h.primitive, h.bary, dir, 0.0);
        if (insideMedium) throughput *= oxRtBeerLambert(medium.absorptionColor, medium.absorptionDistance, h.t);
        radiance += throughput * oxRtEmissiveRadiance(pc.view, hs.emissive);
        if (bounce == pc.maxBounces) break;

        if (hs.blend == OX_BLEND_REFRACTIVE || hs.blend == OX_BLEND_TRANSPARENT) {
            // Smooth dielectric (transparent: thin sheet with ior 1 → straight through, weighted by alpha).
            Material m = SCENE.materials.m[hs.material];
            vec3 n = hs.normal;
            if (hs.blend == OX_BLEND_TRANSPARENT) {
                if (oxRtRandom(rng) < hs.baseColor.a) {
                    // Opaque part of the blend: shade like a diffuse surface below.
                } else {
                    origin = oxRtOffsetRay(hs.position, -hs.geometricNormal);
                    lastPdf = 0.0;
                    continue;
                }
            } else {
                float ior = max(m.ior, 1.0001);
                bool entering = hs.frontFace;
                float eta = entering ? 1.0 / ior : ior;
                float cosI = clamp(dot(-dir, n), 0.0, 1.0);
                float F = ptFresnelDielectric(cosI, eta);
                vec3 T = refract(dir, n, eta);
                if (dot(T, T) < 1e-6 || oxRtRandom(rng) < F) {
                    dir = reflect(dir, n);
                    origin = oxRtOffsetRay(hs.position, hs.geometricNormal);
                } else {
                    if (entering) throughput *= hs.baseColor.rgb * oxRtTransmission(m);
                    dir = T;
                    origin = oxRtOffsetRay(hs.position, -hs.geometricNormal);
                    insideMedium = entering;
                    medium = m;
                }
                lastPdf = 0.0;
                continue;
            }
        }

        OxSurface s = oxRtToSurface(hs, -dir);
        vec3 offsetOrigin = oxRtOffsetRay(s.position, s.geometricNormal);
        float pSpec = ptSpecProbability(s);
        vec3 direct = throughput * ptNextEvent(s, offsetOrigin, pSpec, rng);
        if (bounce > 0u && oxLuminance(direct) > maxLum) direct *= maxLum / oxLuminance(direct);
        radiance += direct;

        // BSDF sampling.
        vec3 L;
        if (oxRtRandom(rng) < pSpec) {
            vec3 H = oxToWorld(oxSampleGGXVNDF(oxToLocal(s.view, s.normal), s.roughness, oxRtRandom2(rng)), s.normal);
            L = reflect(-s.view, H);
        } else {
            L = oxSampleCosineHemisphere(oxRtRandom2(rng), s.normal);
        }
        float pdf = ptBsdfPdf(s, L, pSpec);
        if (pdf <= 1e-6 || dot(L, s.geometricNormal) <= 0.0) break;
        throughput *= oxBrdfDirect(s, L, vec3(1.0)) / pdf;
        lastPdf = pdf;
        origin = offsetOrigin;
        dir = L;

        if (bounce >= 3u) {
            float q = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05, 0.95);
            if (oxRtRandom(rng) > q) break;
            throughput /= q;
        }
    }
    return radiance;
}

#endif
