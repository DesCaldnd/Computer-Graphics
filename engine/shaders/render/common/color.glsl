// OxwaldEngine render: colour helpers — sRGB transfer, tonemappers (ACES fitted, AgX, neutral), exposure.
#ifndef OX_RENDER_COLOR_GLSL
#define OX_RENDER_COLOR_GLSL

vec3 oxLinearToSrgb(vec3 c) {
    c = max(c, vec3(0.0));
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}
vec3 oxSrgbToLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

// Exposure from EV100 (saturation based sensitivity): max non-clipping luminance = 1.2 · 2^EV100.
float oxExposureFromEV100(float ev100) { return 1.0 / (1.2 * exp2(ev100)); }

// ACES fitted (Stephen Hill): sRGB → AP1 RRT+ODT fit → sRGB, input is linear sRGB scene-referred.
vec3 oxRRTAndODTFit(vec3 v) {
    vec3 a = v * (v + 0.0245786) - 0.000090537;
    vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}
vec3 oxTonemapACES(vec3 color) {
    const mat3 inputMat = mat3(0.59719, 0.07600, 0.02840, 0.35458, 0.90834, 0.13383, 0.04823, 0.01566, 0.83777);
    const mat3 outputMat = mat3(1.60475, -0.10208, -0.00327, -0.53108, 1.10813, -0.07276, -0.07367, -0.00605, 1.07602);
    color = inputMat * color;
    color = oxRRTAndODTFit(color);
    return clamp(outputMat * color, 0.0, 1.0);
}

// AgX (Troy Sobotka) with the polynomial sigmoid fit by Benjamin Wrensch; output is display-linear sRGB.
vec3 oxAgxContrast(vec3 x) {
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}
vec3 oxTonemapAgX(vec3 color) {
    const mat3 agxIn = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051, 0.0784335999999992,
                            0.878468636469772, 0.0784336, 0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 agxOut = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438, -0.0980208811401368,
                             1.15190312990417, -0.0980434501171241, -0.0990297440797205, -0.0989611768448433,
                             1.15107367264116);
    const float minEv = -12.47393;
    const float maxEv = 4.026069;
    color = agxIn * max(color, vec3(1e-10));
    color = clamp(log2(color), minEv, maxEv);
    color = (color - minEv) / (maxEv - minEv);
    color = oxAgxContrast(color);
    color = agxOut * color;
    // The AgX base look is defined in display (sRGB-encoded) space: return linear for a uniform pipeline.
    return oxSrgbToLinear(clamp(color, 0.0, 1.0));
}

// Khronos PBR Neutral tonemapper.
vec3 oxTonemapNeutral(vec3 color) {
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;
    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, vec3(newPeak), g);
}

const uint OX_TONEMAP_ACES = 0u;
const uint OX_TONEMAP_AGX = 1u;
const uint OX_TONEMAP_NEUTRAL = 2u;
const uint OX_TONEMAP_LINEAR = 3u;

// Exposed scene-linear → display-linear.
vec3 oxTonemap(vec3 color, uint op) {
    if (op == OX_TONEMAP_ACES) return oxTonemapACES(color);
    if (op == OX_TONEMAP_AGX) return oxTonemapAgX(color);
    if (op == OX_TONEMAP_NEUTRAL) return clamp(oxTonemapNeutral(color), 0.0, 1.0);
    return clamp(color, 0.0, 1.0);
}

vec3 oxHeatmap(float t) {
    t = clamp(t, 0.0, 1.0);
    vec3 c = vec3(0.0);
    c.r = smoothstep(0.35, 0.75, t) + smoothstep(0.9, 1.0, t);
    c.g = smoothstep(0.0, 0.35, t) - smoothstep(0.75, 1.0, t) * 0.8;
    c.b = 1.0 - smoothstep(0.1, 0.5, t);
    return clamp(c, 0.0, 1.0);
}

#endif
