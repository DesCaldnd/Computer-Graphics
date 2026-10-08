// OxwaldEngine render / world-skinning area: helpers shared by the terrain, vegetation and sky shaders
// (noise, dithering, motion vectors). Usable from every stage; fragment shading lives in world_shading.glsl.
#ifndef OX_RENDER_WORLD_COMMON_GLSL
#define OX_RENDER_WORLD_COMMON_GLSL

#include <render/common/math.glsl>
#include <render/common/scene.glsl>

OX_READONLY_BUFFER(OxWorldMatrixBuffer, { mat4 m[]; });

// --- noise -----------------------------------------------------------------------------------------------------

float oxWorldHash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float oxWorldHash13(vec3 p3) {
    p3 = fract(p3 * 0.1031);
    p3 += dot(p3, p3.zyx + 31.32);
    return fract((p3.x + p3.y) * p3.z);
}

vec3 oxWorldHash33(vec3 p3) {
    p3 = fract(p3 * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yxz + 33.33);
    return fract((p3.xxy + p3.yxx) * p3.zyx);
}

float oxWorldValueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = oxWorldHash12(i), b = oxWorldHash12(i + vec2(1.0, 0.0));
    float c = oxWorldHash12(i + vec2(0.0, 1.0)), d = oxWorldHash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float oxWorldFbm(vec2 p) {
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 4; ++i) {
        s += a * oxWorldValueNoise(p);
        p = p * 2.03 + vec2(17.1, 9.7);
        a *= 0.5;
    }
    return s / 0.9375;
}

// --- dithered LOD cross-fade --------------------------------------------------------------------------------------

// Fade code from the vegetation culling pass: low 16 bits = fade * 65535, bit 16 = complement (the incoming LOD
// takes the pixels the outgoing one drops, so both together cover each pixel exactly once).
bool oxWorldDitherKeep(vec2 pixel, uint fadeCode) {
    float fade = float(fadeCode & 0xffffu) / 65535.0;
    if (fade >= 1.0) return true;
    float n = oxInterleavedGradientNoise(floor(pixel));
    bool keep = n < fade;
    return (fadeCode & 0x10000u) != 0u ? !keep : keep;
}

// --- motion vectors -------------------------------------------------------------------------------------------------

vec2 oxWorldVelocity(vec4 curClip, vec4 prevClip) {
    return (curClip.xy / curClip.w - prevClip.xy / prevClip.w) * 0.5;
}

#endif
