#version 460
// Underwater view (camera below the surface): refraction wobble, Beer-Lambert absorption + in-scattering fog along
// the view ray up to the scene or the water plane. Copies SceneColorHDR into a new target (republished).
#include "water_push.glsl"
#include "common.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 suv = gl_FragCoord.xy * VIEW.renderSize.zw;
    vec3 cam = VIEW.cameraPosition.xyz;
    float time = oxWaterTime(pc.water);
    float camSurface = WATER.foam.w; // CPU: surface height above the camera
    vec3 src = OX_SAMPLE_2D_LOD(pc.hdr, OX_SAMPLER_LINEAR_CLAMP, suv, 0.0).rgb;
    if (cam.y >= camSurface || !oxWaterInside(pc.water, cam.xz)) {
        outColor = vec4(src, 1.0);
        return;
    }
    vec2 wobble = vec2(sin(suv.y * 31.0 + time * 2.1), cos(suv.x * 27.0 + time * 1.7)) * 0.0025;
    vec2 wuv = clamp(suv + wobble, vec2(0.0), vec2(1.0));
    vec3 color = OX_SAMPLE_2D_LOD(pc.hdr, OX_SAMPLER_LINEAR_CLAMP, wuv, 0.0).rgb / max(VIEW.preExposure, 1e-12);

    float d = OX_SAMPLE_2D_LOD(pc.sceneDepth, OX_SAMPLER_NEAREST_CLAMP, wuv, 0.0).r;
    vec3 dir = oxViewRay(pc.view, wuv);
    float dist = d > 0.0 ? length(oxWorldPositionFromDepth(pc.view, wuv, d) - cam) : 1e4;
    // Rays leaving through the (flat approximated) surface only travel underwater up to it.
    if (dir.y > 1e-4) dist = min(dist, max(oxWaterBase(pc.water) - cam.y, 0.0) / dir.y + 0.0);
    dist = max(dist, 0.0);

    vec4 sh[9];
    OxSHBuffer shb = VIEW.irradianceSH;
    for (int k = 0; k < 9; ++k) sh[k] = shb.c[k];
    vec3 ambient = oxEvalSH9(sh, vec3(0.0, 1.0, 0.0)) * VIEW.iblIntensity;
    vec3 sunLight = vec3(0.0);
    if (VIEW.sunLight >= 0) {
        Light l = SCENE.lights.l[VIEW.sunLight];
        sunLight = l.color * max(-l.direction.y, 0.0) * OX_INV_PI;
    }
    // Light fades with the camera depth below the surface.
    float camDepth = camSurface - cam.y;
    vec3 fogRadiance = WATER.underwater.rgb * (ambient + sunLight) * 0.5 * exp(-WATER.absorption.rgb * camDepth);
    vec3 T = exp(-(WATER.absorption.rgb + WATER.underwater.w) * dist);
    float fogAmount = 1.0 - exp(-WATER.underwater.w * dist);
    color = color * T + fogRadiance * fogAmount;
    outColor = vec4(color * VIEW.preExposure, 1.0);
}
