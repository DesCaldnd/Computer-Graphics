// Preetham analytic daylight. Coefficients are computed on the CPU by ox::world::PreethamSky::compute
// and uploaded as PreethamSky::Gpu (std140, 128 bytes). Returns linear sRGB radiance in cd/m².
#ifndef OX_WORLD_PREETHAM_GLSL
#define OX_WORLD_PREETHAM_GLSL

struct OxPreetham {
    vec4 A, B, C, D, E;   // xyz = coefficients for (Y, x, y)
    vec4 zenith;          // xyz = zenith (Y [kcd/m²], x, y), w = turbidity
    vec4 normalization;   // xyz = 1 / F(0, thetaSun)
    vec4 sunDirection;    // xyz
};

vec3 oxPerez(OxPreetham p, float cosTheta, float gamma, float cosGamma) {
    return (1.0 + p.A.xyz * exp(p.B.xyz / max(cosTheta, 0.01))) *
           (1.0 + p.C.xyz * exp(p.D.xyz * gamma) + p.E.xyz * cosGamma * cosGamma);
}

vec3 oxPreethamRadiance(OxPreetham p, vec3 viewDir) {
    vec3 v = normalize(viewDir);
    float cosTheta = max(v.y, 0.001);
    float cosGamma = clamp(dot(v, p.sunDirection.xyz), -1.0, 1.0);
    float gamma = acos(cosGamma);
    vec3 Yxy = p.zenith.xyz * oxPerez(p, cosTheta, gamma, cosGamma) * p.normalization.xyz;
    float Y = Yxy.x, x = Yxy.y, y = max(Yxy.z, 1e-6);
    vec3 XYZ = vec3(x / y * Y, Y, (1.0 - x - y) / y * Y);
    vec3 rgb = vec3(3.2406 * XYZ.x - 1.5372 * XYZ.y - 0.4986 * XYZ.z,
                    -0.9689 * XYZ.x + 1.8758 * XYZ.y + 0.0415 * XYZ.z,
                    0.0557 * XYZ.x - 0.2040 * XYZ.y + 1.0570 * XYZ.z);
    return max(rgb * 1000.0, vec3(0.0));
}

#endif
