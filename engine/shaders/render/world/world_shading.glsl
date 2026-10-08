// OxwaldEngine render / world-skinning area: forward shading wrapper of the world passes (mirrors
// passes/forward.frag incl. debug views and height fog). Fragment stage only.
#ifndef OX_RENDER_WORLD_SHADING_GLSL
#define OX_RENDER_WORLD_SHADING_GLSL

#include <render/common/color.glsl>
#include <render/common/lighting.glsl>
#include <render/common/material.glsl>
#include "world_common.glsl"


struct OxWorldInputs {
    uint shadowMask;
    uint ao;
    uint reflections;
    uint indirectDiffuse;
};

// translucency: rgb transmission tint × amount for foliage back-lighting from the sun (0 = opaque surface).
vec4 oxWorldShade(ViewBuffer vb, SceneBuffer sb, OxSurface s, vec2 fragCoord, OxWorldInputs inp, vec3 translucency,
                  bool receiveShadows) {
    vec3 camPos = vb.v.cameraPosition.xyz;
    bool ortho = (vb.v.flags & OX_VIEW_ORTHOGRAPHIC) != 0u;
    float viewDepth = ortho ? dot(s.position - camPos, -vb.v.invView[2].xyz) : -(vb.v.view * vec4(s.position, 1.0)).z;
    vec2 uv = fragCoord * vb.v.renderSize.zw;
    uint debugView = vb.v.debugView;
    if (debugView != OX_DEBUG_NONE && debugView != OX_DEBUG_LIGHT_COMPLEXITY && debugView != OX_DEBUG_SHADOW_CASCADES &&
        debugView <= OX_DEBUG_EMISSIVE) {
        vec3 d = vec3(0.0);
        if (debugView == OX_DEBUG_ALBEDO) d = s.baseColor;
        else if (debugView == OX_DEBUG_NORMALS) d = s.normal * 0.5 + 0.5;
        else if (debugView == OX_DEBUG_ROUGHNESS) d = vec3(s.perceptualRoughness);
        else if (debugView == OX_DEBUG_METALLIC) d = vec3(s.metallic);
        else if (debugView == OX_DEBUG_AO) {
            float ao = s.occlusion;
            if (inp.ao != OX_INVALID_INDEX) ao *= OX_SAMPLE_2D_LOD(inp.ao, OX_SAMPLER_LINEAR_CLAMP, uv, 0.0).r;
            d = vec3(ao);
        } else if (debugView == OX_DEBUG_EMISSIVE) d = s.emissive;
        if (debugView == OX_DEBUG_ALBEDO || debugView == OX_DEBUG_EMISSIVE) return vec4(d * vb.v.preExposure, 1.0);
        return vec4(oxSrgbToLinear(d) * vb.v.preExposure, 1.0);
    }
    OxLightingInputs inputs;
    inputs.screenSpace = true;
    inputs.uv = uv;
    inputs.shadowMask = inp.shadowMask;
    inputs.ao = inp.ao;
    inputs.reflections = inp.reflections;
    inputs.indirectDiffuse = inp.indirectDiffuse;
    inputs.receiveShadows = receiveShadows;
    inputs.clearcoat = 0.0;
    inputs.clearcoatRoughness = 0.0;
    OxLightingResult lit = oxEvaluateLighting(vb, sb, s, fragCoord, viewDepth, inputs);
    vec3 color = lit.direct + lit.indirect + s.emissive / max(vb.v.exposure, 1e-12);
    int sun = vb.v.sunLight;
    if (sun >= 0 && dot(translucency, translucency) > 0.0) {
        Light l = sb.s.lights.l[sun];
        vec3 L = -l.direction;
        // Thin-leaf transmission: forward scattering towards the viewer + diffuse wrap through the back face.
        float forward = pow(clamp(dot(s.view, -L), 0.0, 1.0), 4.0);
        float back = clamp(dot(-s.normal, L), 0.0, 1.0);
        color += translucency * s.baseColor * l.color * lit.sunShadow * (0.6 * forward + 0.4 * back) * OX_INV_PI;
    }
    if (debugView == OX_DEBUG_LIGHT_COMPLEXITY) {
        return vec4(oxHeatmap(float(lit.localLightCount) / 32.0) * vb.v.preExposure, 1.0);
    }
    if (debugView == OX_DEBUG_SHADOW_CASCADES) {
        const vec3 kColors[5] = vec3[](vec3(1.0, 0.25, 0.25), vec3(0.25, 1.0, 0.25), vec3(0.3, 0.45, 1.0),
                                       vec3(1.0, 1.0, 0.25), vec3(0.6, 0.6, 0.6));
        uint c = min(oxSelectCascade(vb, viewDepth), 4u);
        float lum = clamp(oxLuminance(color * vb.v.exposure), 0.0, 1.0);
        return vec4(kColors[c] * (0.25 + 0.75 * lum) * vb.v.preExposure, 1.0);
    }
    if (vb.v.fogColor.w > 0.5) {
        vec4 sh[9];
        OxSHBuffer shb = vb.v.irradianceSH;
        for (int k = 0; k < 9; ++k) sh[k] = shb.c[k];
        vec3 ambient = oxEvalSH9(sh, vec3(0.0, 1.0, 0.0)) * vb.v.iblIntensity;
        color = oxApplyHeightFog(vb, color, s.position, vb.v.fogColor.rgb * ambient);
    }
    return vec4(color * vb.v.preExposure, 1.0);
}

#endif
