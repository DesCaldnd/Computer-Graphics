// OxwaldEngine render / world: impostor atlas sampling (fragment stage: uses screen-space derivatives).
#ifndef OX_RENDER_WORLD_IMPOSTOR_SAMPLE_GLSL
#define OX_RENDER_WORLD_IMPOSTOR_SAMPLE_GLSL

#include "impostor_common.glsl"

struct OxImpostorSample {
    vec4 albedo; // rgb linear, a coverage
    vec3 normal; // object space
};

// Blends the 4 frames around the object-space view direction `objDir` at quad uv (frame-local [0,1]²).
OxImpostorSample oxSampleImpostor(VegProto proto, vec3 objDir, vec2 uv) {
    float frames = proto.impostor.x;
    vec2 oct = oxHemiOctEncode(normalize(vec3(objDir.x, max(objDir.y, 0.0), objDir.z))) * 0.5 + 0.5;
    vec2 g = clamp(oct * frames - 0.5, vec2(0.0), vec2(frames - 1.0));
    vec2 f0 = floor(g);
    vec2 t = g - f0;
    vec2 gx = dFdx(uv) / frames, gy = dFdy(uv) / frames;
    OxImpostorSample o;
    o.albedo = vec4(0.0);
    o.normal = vec3(0.0);
    for (int k = 0; k < 4; ++k) {
        vec2 off = vec2(float(k & 1), float(k >> 1));
        vec2 f = min(f0 + off, vec2(frames - 1.0));
        float w = (off.x > 0.5 ? t.x : 1.0 - t.x) * (off.y > 0.5 ? t.y : 1.0 - t.y);
        vec2 auv = (f + clamp(uv, vec2(0.002), vec2(0.998))) / frames;
        vec4 a = textureGrad(sampler2D(OX_TEX2D(proto.albedoAtlas), OX_SAMPLER(OX_SAMPLER_LINEAR_CLAMP)), auv, gx, gy);
        vec4 n = textureGrad(sampler2D(OX_TEX2D(proto.normalAtlas), OX_SAMPLER(OX_SAMPLER_LINEAR_CLAMP)), auv, gx, gy);
        o.albedo += a * w;
        o.normal += (n.xyz * 2.0 - 1.0) * a.a * w;
    }
    o.albedo.rgb /= max(o.albedo.a, 1e-4);
    o.normal = length(o.normal) > 1e-4 ? normalize(o.normal) : vec3(0.0, 1.0, 0.0);
    return o;
}

#endif
