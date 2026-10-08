// OxwaldEngine render: material sampling (bindless textures, ORM, tangent-space normal maps).
#ifndef OX_RENDER_MATERIAL_GLSL
#define OX_RENDER_MATERIAL_GLSL

#include "scene.glsl"

struct OxMaterialSample {
    vec4 baseColor; // linear rgb + alpha
    vec3 normal;    // world, normalised, facing the viewer for double-sided materials
    float metallic;
    float perceptualRoughness;
    float occlusion;
    vec3 emissive;
};

vec4 oxSampleTex(uint tex, uint smp, vec2 uv, float mipBias) {
    return texture(sampler2D(OX_TEX2D(tex), OX_SAMPLER(smp)), uv, mipBias);
}

vec2 oxMaterialUv(Material m, vec2 uv) { return uv * m.uvTiling + m.uvOffset; }

float oxMaterialAlpha(Material m, vec2 uv0, vec4 vertexColor, float mipBias) {
    float a = m.baseColor.a * vertexColor.a;
    if (m.albedoTexture != OX_INVALID_INDEX) a *= oxSampleTex(m.albedoTexture, m.samplerIndex, oxMaterialUv(m, uv0), mipBias).a;
    return a;
}

// N, T: interpolated world normal / tangent (xyz + sign). frontFacing flips for double-sided materials.
OxMaterialSample oxSampleMaterial(Material m, vec2 uv0, vec4 vertexColor, vec3 N, vec4 T, bool frontFacing,
                                  float mipBias) {
    OxMaterialSample o;
    vec2 uv = oxMaterialUv(m, uv0);
    o.baseColor = m.baseColor * vertexColor;
    if (m.albedoTexture != OX_INVALID_INDEX) o.baseColor *= oxSampleTex(m.albedoTexture, m.samplerIndex, uv, mipBias);
    o.metallic = m.metallic;
    o.perceptualRoughness = m.roughness;
    o.occlusion = 1.0;
    if (m.ormTexture != OX_INVALID_INDEX) {
        vec3 orm = oxSampleTex(m.ormTexture, m.samplerIndex, uv, mipBias).rgb;
        o.occlusion = mix(1.0, orm.r, m.occlusionStrength);
        o.perceptualRoughness *= orm.g;
        o.metallic *= orm.b;
    }
    o.emissive = m.emissive;
    if (m.emissiveTexture != OX_INVALID_INDEX) o.emissive *= oxSampleTex(m.emissiveTexture, m.samplerIndex, uv, mipBias).rgb;

    vec3 n = normalize(N);
    if ((m.flags & OX_MATERIAL_DOUBLE_SIDED) != 0u && !frontFacing) n = -n;
    if (m.normalTexture != OX_INVALID_INDEX && dot(T.xyz, T.xyz) > 1e-8) {
        vec2 xy = oxSampleTex(m.normalTexture, m.samplerIndex, uv, mipBias).rg * 2.0 - 1.0;
        xy *= m.normalStrength;
        vec3 tn = vec3(xy, sqrt(clamp(1.0 - dot(xy, xy), 0.0, 1.0)));
        vec3 t = normalize(T.xyz - n * dot(n, T.xyz));
        vec3 b = cross(n, t) * (T.w < 0.0 ? -1.0 : 1.0);
        n = normalize(t * tn.x + b * tn.y + n * tn.z);
    }
    o.normal = n;
    return o;
}

#endif
