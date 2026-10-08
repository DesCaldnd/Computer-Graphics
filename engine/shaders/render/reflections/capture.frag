#version 460
// Scene capture for reflection probes, irradiance volume bakes and planar reflections (mesh.vert vertex stage,
// capture view constants). Lighting: directional lights + IBL through oxEvaluateLighting (the capture view has no
// light clusters) plus a loop over the local lights the CPU found inside the capture frustum. The sun shadow cascade is
// selected with the depth relative to the main camera (the cascades were fitted to it).
// Output: rgb = radiance × VIEW.preExposure (probes use preExposure 1), a = distance to the capture point (m).
#include <render/common/lighting.glsl>
#include <render/common/material.glsl>

OX_READONLY_BUFFER(OxCaptureLightList, { uint index[]; });
OX_RENDER_DRAW_PUSH(vec4 mainForward; vec4 mainPosition; OxCaptureLightList lightList; uint lightCount; uint flags;);

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vTangent;
layout(location = 3) in vec2 vUv0;
layout(location = 4) in vec4 vColor;
layout(location = 5) in vec4 vCurClip;
layout(location = 6) in vec4 vPrevClip;
layout(location = 7) flat in uint vInstance;

layout(location = 0) out vec4 outColor;

void main() {
    InstanceBuffer instances = SCENE.instances;
    uint materialIndex = instances.i[vInstance].materialIndex;
    uint instanceFlags = instances.i[vInstance].flags;
    Material m = SCENE.materials.m[materialIndex];
#ifdef OX_ALPHA_TEST
    if (oxMaterialAlpha(m, vUv0, vColor, 0.0) < m.alphaCutoff) discard;
#endif
    OxMaterialSample ms = oxSampleMaterial(m, vUv0, vColor, vNormal, vTangent, gl_FrontFacing, 0.0);

    vec3 camPos = VIEW.cameraPosition.xyz;
    OxSurface s;
    s.position = vWorldPos;
    s.normal = ms.normal;
    vec3 gn = normalize(vNormal);
    if ((m.flags & OX_MATERIAL_DOUBLE_SIDED) != 0u && !gl_FrontFacing) gn = -gn;
    s.geometricNormal = gn;
    s.view = normalize(camPos - vWorldPos);
    s.baseColor = ms.baseColor.rgb;
    s.alpha = ms.baseColor.a;
    s.metallic = clamp(ms.metallic, 0.0, 1.0);
    s.perceptualRoughness = ms.perceptualRoughness;
    s.occlusion = ms.occlusion;
    s.emissive = ms.emissive;
    oxSurfaceFinalize(s);

    vec3 color;
    if ((m.flags & OX_MATERIAL_UNLIT) != 0u) {
        color = (s.baseColor + s.emissive) / max(VIEW.exposure, 1e-12);
    } else {
        float mainDepth = max(dot(vWorldPos - pc.mainPosition.xyz, pc.mainForward.xyz), 0.0);
        OxLightingInputs inputs = oxDefaultLightingInputs();
        inputs.receiveShadows = (instanceFlags & OX_INSTANCE_RECEIVE_SHADOWS) != 0u;
        inputs.clearcoat = clamp(m.clearcoat, 0.0, 1.0);
        inputs.clearcoatRoughness = m.clearcoatRoughness;
        OxLightingResult lit = oxEvaluateLighting(pc.view, pc.scene, s, gl_FragCoord.xy, mainDepth, inputs);
        color = lit.direct + lit.indirect + s.emissive / max(VIEW.exposure, 1e-12);

        // Local lights without clusters.
        vec2 dfg = oxSampleBrdfLut(pc.view, s.NdotV, s.perceptualRoughness);
        vec3 energyComp = VIEW.brdfLut != OX_INVALID_INDEX ? 1.0 + s.f0 * (1.0 / max(dfg.x + dfg.y, 1e-3) - 1.0) : vec3(1.0);
        LightBuffer lights = SCENE.lights;
        ShadowBuffer shadows = SCENE.shadows;
        for (uint k = 0u; k < pc.lightCount; ++k) {
            Light l = lights.l[pc.lightList.index[k]];
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
                if (sh.kind == 1u) vis = oxPointShadow(pc.view, sh, l.position, s.position, s.geometricNormal, gl_FragCoord.xy);
                else if (sh.kind == 0u) vis = oxSpotShadow(pc.view, sh, s.position, s.geometricNormal, gl_FragCoord.xy);
            }
            color += oxBrdfDirect(s, L, energyComp) * l.color * (att * vis);
        }
    }
    outColor = vec4(min(color * VIEW.preExposure, vec3(60000.0)), distance(vWorldPos, camPos));
}
