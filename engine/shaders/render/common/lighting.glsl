// OxwaldEngine render: full surface lighting — directional + clustered local lights with shadows, IBL (SH9
// irradiance + prefiltered GGX cube + split-sum LUT, multi-scatter compensation), optional screen-space inputs.
// Used by the opaque forward pass; translucency/water/particles call the same function with
// inputs.screenSpace = false (shadows are then sampled from the shadow maps instead of the ShadowMask).
#ifndef OX_RENDER_LIGHTING_GLSL
#define OX_RENDER_LIGHTING_GLSL

#include "clusters.glsl"
#include "pbr.glsl"
#include "shadows.glsl"

// Ray tracing area (ShadowsRT): view flag = ShadowMask holds coloured sun visibility in rgb (RGBA8); GpuShadow kind 2 =
// screen-space visibility mask (bindless index in cubeLayer, channel in atlasRect.x) for ray traced local lights.
const uint OX_VIEW_RT_SHADOW_MASK_RGB = 256u;
const uint OX_SHADOW_KIND_SCREEN_MASK = 2u;

struct OxLightingInputs {
    bool screenSpace;       // inputs below are sampled at `uv` (opaque pass); false for translucency
    vec2 uv;                // render-resolution uv of the pixel
    uint shadowMask;        // R8 sun visibility (OX_INVALID_INDEX = sample CSM directly)
    uint ao;                // R8 ambient occlusion (OX_INVALID_INDEX = 1)
    uint reflections;       // RGBA16F: rgb specular radiance (not pre-exposed), a = weight
    uint indirectDiffuse;   // RGBA16F: rgb diffuse radiance of a white surface (irradiance / π)
    bool receiveShadows;
};

OxLightingInputs oxDefaultLightingInputs() {
    OxLightingInputs i;
    i.screenSpace = false;
    i.uv = vec2(0.0);
    i.shadowMask = OX_INVALID_INDEX;
    i.ao = OX_INVALID_INDEX;
    i.reflections = OX_INVALID_INDEX;
    i.indirectDiffuse = OX_INVALID_INDEX;
    i.receiveShadows = true;
    return i;
}

struct OxLightingResult {
    vec3 direct;
    vec3 indirect;
    uint localLightCount; // lights in the cluster (light complexity view)
    float sunShadow;
};

vec2 oxSampleBrdfLut(ViewBuffer vb, float NdotV, float perceptualRoughness) {
    uint lut = vb.v.brdfLut;
    if (lut == OX_INVALID_INDEX) return vec2(1.0, 0.0);
    return OX_SAMPLE_2D_LOD(lut, OX_SAMPLER_LINEAR_CLAMP, vec2(NdotV, perceptualRoughness), 0.0).xy;
}

vec3 oxSamplePrefiltered(ViewBuffer vb, vec3 R, float perceptualRoughness) {
    uint cube = vb.v.prefilteredCube;
    if (cube == OX_INVALID_INDEX) return vec3(0.0);
    float lod = perceptualRoughness * float(vb.v.prefilteredMips - 1u);
    return textureLod(samplerCube(OX_TEXCUBE(cube), OX_SAMPLER(OX_SAMPLER_LINEAR_CLAMP)), R, lod).rgb;
}

OxLightingResult oxEvaluateLighting(ViewBuffer vb, SceneBuffer sb, OxSurface s, vec2 pixel, float viewDepth,
                                    OxLightingInputs inputs) {
    OxLightingResult r;
    r.direct = vec3(0.0);
    r.indirect = vec3(0.0);
    r.localLightCount = 0u;
    r.sunShadow = 1.0;

    vec2 dfg = oxSampleBrdfLut(vb, s.NdotV, s.perceptualRoughness);
    vec3 energyComp = vec3(1.0);
    if (vb.v.brdfLut != OX_INVALID_INDEX) energyComp = 1.0 + s.f0 * (1.0 / max(dfg.x + dfg.y, 1e-3) - 1.0);

    LightBuffer lights = sb.s.lights;
    ShadowBuffer shadows = sb.s.shadows;
    uint dirCount = sb.s.directionalLightCount;
    int sun = vb.v.sunLight;
    for (uint i = 0u; i < dirCount; ++i) {
        Light l = lights.l[i];
        vec3 L = -l.direction;
        float vis = 1.0;
        vec3 visColor = vec3(-1.0); // < 0: grey visibility (vis)
        if (int(i) == sun && inputs.receiveShadows) {
            if (inputs.screenSpace && inputs.shadowMask != OX_INVALID_INDEX) {
                vec4 mask = OX_SAMPLE_2D_LOD(inputs.shadowMask, OX_SAMPLER_NEAREST_CLAMP, inputs.uv, 0.0);
                // Ray traced shadows: RGBA8 mask with coloured (transmission) sun visibility in rgb.
                if ((vb.v.flags & OX_VIEW_RT_SHADOW_MASK_RGB) != 0u) {
                    visColor = mask.rgb;
                    vis = max(mask.r, max(mask.g, mask.b));
                } else {
                    vis = mask.r;
                }
            } else {
                vis = oxSunShadow(vb, s.position, s.geometricNormal, viewDepth, pixel);
            }
            r.sunShadow = vis;
        }
        if (vis > 0.0) r.direct += oxBrdfDirect(s, L, energyComp) * l.color * (visColor.r >= 0.0 ? visColor : vec3(vis));
    }

    if (vb.v.lightClusterCount > 0u) {
        uint cluster = oxClusterIndex(vb, pixel * vb.v.renderSize.zw, viewDepth);
        uint count = oxClusterLightCount(vb, cluster);
        r.localLightCount = count;
        for (uint i = 0u; i < count; ++i) {
            uint li = oxClusterLight(vb, cluster, i);
            Light l = lights.l[li];
            vec3 toLight = l.position - s.position;
            float d2 = dot(toLight, toLight);
            if (d2 > l.range * l.range) continue;
            vec3 L = toLight * inversesqrt(max(d2, 1e-8));
            float att = oxDistanceAttenuation(d2, l.range);
            if (l.type == OX_LIGHT_SPOT) att *= oxSpotAttenuation(L, l.direction, l.spotScale, l.spotOffset);
            if (att <= 0.0 || dot(s.normal, L) <= 0.0) continue;
            float vis = 1.0;
            if (l.shadowIndex >= 0 && inputs.receiveShadows) {
                Shadow sh = shadows.s[l.shadowIndex];
                if (sh.kind == OX_SHADOW_KIND_SCREEN_MASK) {
                    // Ray traced (ShadowsRT): screen-space visibility channel atlasRect.x of texture cubeLayer.
                    vis = inputs.screenSpace ? OX_SAMPLE_2D_LOD(sh.cubeLayer, OX_SAMPLER_NEAREST_CLAMP, inputs.uv, 0.0)[
                                                   min(uint(sh.atlasRect.x), 3u)]
                                             : 1.0;
                } else {
                    vis = sh.kind == 1u ? oxPointShadow(vb, sh, l.position, s.position, s.geometricNormal, pixel)
                                        : oxSpotShadow(vb, sh, s.position, s.geometricNormal, pixel);
                }
            }
            r.direct += oxBrdfDirect(s, L, energyComp) * l.color * (att * vis);
        }
    }

    // Indirect.
    float ao = s.occlusion;
    if (inputs.screenSpace && inputs.ao != OX_INVALID_INDEX) {
        ao *= OX_SAMPLE_2D_LOD(inputs.ao, OX_SAMPLER_LINEAR_CLAMP, inputs.uv, 0.0).r;
    }
    vec3 diffuseRadiance;
    if (inputs.screenSpace && inputs.indirectDiffuse != OX_INVALID_INDEX) {
        diffuseRadiance = OX_SAMPLE_2D_LOD(inputs.indirectDiffuse, OX_SAMPLER_LINEAR_CLAMP, inputs.uv, 0.0).rgb;
    } else {
        vec4 sh[9];
        OxSHBuffer shb = vb.v.irradianceSH;
        for (int k = 0; k < 9; ++k) sh[k] = shb.c[k];
        diffuseRadiance = oxEvalSH9(sh, s.normal) * vb.v.iblIntensity;
    }
    vec3 R = reflect(-s.view, s.normal);
    vec3 specRadiance = oxSamplePrefiltered(vb, R, s.perceptualRoughness) * vb.v.iblIntensity;
    if (inputs.screenSpace && inputs.reflections != OX_INVALID_INDEX) {
        vec4 refl = OX_SAMPLE_2D_LOD(inputs.reflections, OX_SAMPLER_LINEAR_CLAMP, inputs.uv, 0.0);
        specRadiance = mix(specRadiance, refl.rgb, clamp(refl.a, 0.0, 1.0));
    }
    // Split sum with multi-scattering (Fdez-Agüera 2019).
    vec3 FssEss = s.f0 * dfg.x + dfg.y;
    float Ess = dfg.x + dfg.y;
    float Ems = 1.0 - Ess;
    vec3 Favg = s.f0 + (1.0 - s.f0) / 21.0;
    vec3 Fms = FssEss * Favg / (1.0 - Ems * Favg);
    vec3 specWeight = FssEss + Fms * Ems;
    float specOcclusion = oxSpecularOcclusion(s.NdotV, ao, s.roughness);
    vec3 kd = s.diffuseColor * (1.0 - specWeight);
    r.indirect = diffuseRadiance * kd * ao + specRadiance * specWeight * specOcclusion;
    return r;
}

// Simple exponential height fog (Environment fog fields) used until a VolumetricFog feature provides froxels.
// fogRadiance: in-scattered radiance at full density (e.g. fogColor × ambient sky radiance).
vec3 oxApplyHeightFog(ViewBuffer vb, vec3 color, vec3 worldPos, vec3 fogRadiance) {
    vec4 fc = vb.v.fogColor;
    if (fc.w < 0.5) return color;
    vec4 fp = vb.v.fogParams;
    vec3 cam = vb.v.cameraPosition.xyz;
    vec3 ray = worldPos - cam;
    float dist = max(length(ray) - fp.z, 0.0);
    float falloff = max(fp.y, 1e-4);
    float h0 = cam.y, h1 = worldPos.y;
    float heightTerm = abs(h1 - h0) > 1e-3 ? (exp(-falloff * h0) - exp(-falloff * h1)) / (falloff * (h1 - h0))
                                           : exp(-falloff * h0);
    float fog = 1.0 - exp(-fp.x * dist * heightTerm);
    return mix(color, fogRadiance, clamp(fog, 0.0, 1.0));
}

#endif
