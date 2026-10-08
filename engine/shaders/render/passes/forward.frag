#version 460
// Clustered forward+ opaque shading into SceneColorHDR (pre-exposed). Depth test EQUAL against the prepass.
#include <render/common/color.glsl>
#include <render/common/lighting.glsl>
#include <render/common/material.glsl>

// inputs: shadowMask, ao, reflections, indirectDiffuse, flags, unused
OX_RENDER_DRAW_PUSH(uint shadowMask; uint ao; uint reflections; uint indirectDiffuse; uint flags; uint unused;);

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vTangent;
layout(location = 3) in vec2 vUv0;
layout(location = 4) in vec4 vColor;
layout(location = 5) in vec4 vCurClip;
layout(location = 6) in vec4 vPrevClip;
layout(location = 7) flat in uint vInstance;

layout(location = 0) out vec4 outColor;

const vec3 kCascadeColors[5] = vec3[](vec3(1.0, 0.25, 0.25), vec3(0.25, 1.0, 0.25), vec3(0.3, 0.45, 1.0),
                                      vec3(1.0, 1.0, 0.25), vec3(0.6, 0.6, 0.6));

void main() {
    InstanceBuffer instances = SCENE.instances;
    uint materialIndex = instances.i[vInstance].materialIndex;
    uint instanceFlags = instances.i[vInstance].flags;
    Material m = SCENE.materials.m[materialIndex];
    OxMaterialSample ms = oxSampleMaterial(m, vUv0, vColor, vNormal, vTangent, gl_FrontFacing, VIEW.mipBias);

    vec3 camPos = VIEW.cameraPosition.xyz;
    OxSurface s;
    s.position = vWorldPos;
    s.normal = ms.normal;
    vec3 gn = normalize(vNormal);
    if ((m.flags & OX_MATERIAL_DOUBLE_SIDED) != 0u && !gl_FrontFacing) gn = -gn;
    s.geometricNormal = gn;
    // Orthographic: every view ray is parallel to the camera's -Z, so V = camera +Z.
    s.view = (VIEW.flags & OX_VIEW_ORTHOGRAPHIC) != 0u ? normalize(VIEW.invView[2].xyz) : normalize(camPos - vWorldPos);
    s.baseColor = ms.baseColor.rgb;
    s.alpha = ms.baseColor.a;
    s.metallic = clamp(ms.metallic, 0.0, 1.0);
    s.perceptualRoughness = ms.perceptualRoughness;
    s.occlusion = ms.occlusion;
    s.emissive = ms.emissive;
    oxSurfaceFinalize(s);

    float viewDepth = (VIEW.flags & OX_VIEW_ORTHOGRAPHIC) != 0u ? dot(vWorldPos - camPos, -VIEW.invView[2].xyz)
                                                                : -(VIEW.view * vec4(vWorldPos, 1.0)).z;
    vec2 uv = gl_FragCoord.xy * VIEW.renderSize.zw;

    uint debugView = VIEW.debugView;
    if (debugView != OX_DEBUG_NONE && debugView != OX_DEBUG_LIGHT_COMPLEXITY && debugView != OX_DEBUG_SHADOW_CASCADES &&
        debugView <= OX_DEBUG_EMISSIVE) {
        vec3 d = vec3(0.0);
        if (debugView == OX_DEBUG_ALBEDO) d = s.baseColor;
        else if (debugView == OX_DEBUG_NORMALS) d = s.normal * 0.5 + 0.5;
        else if (debugView == OX_DEBUG_ROUGHNESS) d = vec3(s.perceptualRoughness);
        else if (debugView == OX_DEBUG_METALLIC) d = vec3(s.metallic);
        else if (debugView == OX_DEBUG_AO) {
            float ao = s.occlusion;
            if (pc.ao != OX_INVALID_INDEX) ao *= OX_SAMPLE_2D_LOD(pc.ao, OX_SAMPLER_LINEAR_CLAMP, uv, 0.0).r;
            d = vec3(ao);
        } else if (debugView == OX_DEBUG_EMISSIVE) d = s.emissive;
        // Debug values bypass exposure/tonemapping (see tonemap.frag): store as display-linear × preExposure.
        outColor = vec4(oxSrgbToLinear(d) * VIEW.preExposure, 1.0);
        if (debugView == OX_DEBUG_ALBEDO || debugView == OX_DEBUG_EMISSIVE) outColor = vec4(d * VIEW.preExposure, 1.0);
        return;
    }

    vec3 color;
    if ((m.flags & OX_MATERIAL_UNLIT) != 0u) {
        color = s.baseColor / max(VIEW.exposure, 1e-12);
    } else {
        OxLightingInputs inputs;
        inputs.screenSpace = true;
        inputs.uv = uv;
        inputs.shadowMask = pc.shadowMask;
        inputs.ao = pc.ao;
        inputs.reflections = pc.reflections;
        inputs.indirectDiffuse = pc.indirectDiffuse;
        inputs.receiveShadows = (instanceFlags & OX_INSTANCE_RECEIVE_SHADOWS) != 0u;
        OxLightingResult lit = oxEvaluateLighting(pc.view, pc.scene, s, gl_FragCoord.xy, viewDepth, inputs);
        // Emissive is display-relative: 1.0 = white at the current exposure.
        color = lit.direct + lit.indirect + s.emissive / max(VIEW.exposure, 1e-12);
        if (debugView == OX_DEBUG_LIGHT_COMPLEXITY) {
            outColor = vec4(oxHeatmap(float(lit.localLightCount) / 32.0) * VIEW.preExposure, 1.0);
            return;
        }
        if (debugView == OX_DEBUG_SHADOW_CASCADES) {
            uint c = min(oxSelectCascade(pc.view, viewDepth), 4u);
            float l = clamp(oxLuminance(color * VIEW.exposure), 0.0, 1.0);
            outColor = vec4(kCascadeColors[c] * (0.25 + 0.75 * l) * VIEW.preExposure, 1.0);
            return;
        }
        if (VIEW.fogColor.w > 0.5) {
            vec4 sh[9];
            OxSHBuffer shb = VIEW.irradianceSH;
            for (int k = 0; k < 9; ++k) sh[k] = shb.c[k];
            vec3 ambient = oxEvalSH9(sh, vec3(0.0, 1.0, 0.0)) * VIEW.iblIntensity;
            color = oxApplyHeightFog(pc.view, color, vWorldPos, VIEW.fogColor.rgb * ambient);
        }
    }
    outColor = vec4(color * VIEW.preExposure, 1.0);
}
