#version 460
// Transparent and refractive surfaces, lit by the same clusters / shadows / IBL as the opaque pass.
//   OX_MODE_OIT        Weighted Blended OIT: accumulation (RGBA16F, ONE/ONE) + revealage (R8, ZERO/ONE_MINUS_SRC_COLOR)
//   OX_MODE_SORTED     premultiplied alpha over SceneColorHDR, drawn back to front
//   OX_MODE_REFRACTIVE screen-space refraction of SceneColorRefraction, Beer-Lambert absorption, Schlick Fresnel,
//                      total internal reflection; writes the final colour (blend off)
#include "common.glsl"
#include "translucent_push.glsl"

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vTangent;
layout(location = 3) in vec2 vUv0;
layout(location = 4) in vec4 vColor;
layout(location = 5) flat in uint vInstance;

#ifdef OX_MODE_OIT
layout(location = 0) out vec4 outAccum;
layout(location = 1) out vec4 outReveal;
#else
layout(location = 0) out vec4 outColor;
#endif

void main() {
    InstanceBuffer instances = SCENE.instances;
    Material m = SCENE.materials.m[instances.i[vInstance].materialIndex];
    uint instanceFlags = instances.i[vInstance].flags;
    OxMaterialSample ms = oxSampleMaterial(m, vUv0, vColor, vNormal, vTangent, gl_FrontFacing, VIEW.mipBias);
    bool doubleSided = (m.flags & OX_MATERIAL_DOUBLE_SIDED) != 0u;

    OxSurface s;
    s.position = vWorldPos;
    s.normal = ms.normal;
    vec3 gn = normalize(vNormal);
    if (doubleSided && !gl_FrontFacing) gn = -gn;
    s.geometricNormal = gn;
    s.view = oxTranslucencyViewVector(pc.view, vWorldPos);
    s.baseColor = ms.baseColor.rgb;
    s.alpha = ms.baseColor.a;
    s.metallic = clamp(ms.metallic, 0.0, 1.0);
    s.perceptualRoughness = ms.perceptualRoughness;
    s.occlusion = ms.occlusion;
    s.emissive = ms.emissive;
    float viewDepth = oxTranslucencyViewDepth(pc.view, vWorldPos);
    vec2 uv = gl_FragCoord.xy * VIEW.renderSize.zw;
    float invExposure = 1.0 / max(VIEW.exposure, 1e-12);
    bool unlit = (m.flags & OX_MATERIAL_UNLIT) != 0u;

    OxLightingInputs inputs = oxDefaultLightingInputs();
    inputs.uv = uv;
    inputs.receiveShadows = (instanceFlags & OX_INSTANCE_RECEIVE_SHADOWS) != 0u;

#ifdef OX_MODE_REFRACTIVE
    float ior = max(m.ior, 1.0001);
    bool entering = gl_FrontFacing || !doubleSided;
    float transmission = m.transmission > 0.0 ? clamp(m.transmission, 0.0, 1.0) : 1.0;
    s.metallic = 0.0;
    oxSurfaceFinalize(s);
    float f0 = oxF0FromIor(ior);
    s.f0 = vec3(f0);
    s.diffuseColor *= 1.0 - transmission;

    vec3 I = -s.view;
    vec3 N = s.normal;
    vec3 T = refract(I, N, entering ? 1.0 / ior : ior);
    bool tir = dot(T, T) < 1e-6;
    float cosI = clamp(dot(N, s.view), 0.0, 1.0);
    // Exiting a denser medium: Schlick with the transmitted angle (symmetric form).
    float cosF = entering ? cosI : sqrt(max(1.0 - (1.0 - cosI * cosI) * ior * ior, 0.0));
    float F = tir ? 1.0 : oxSchlick(cosF, f0);

    // Thickness: back-face depth difference (closed meshes), else the material thickness.
    float sceneLin = oxSceneLinearDepth(pc.view, pc.sceneDepth, uv);
    float thickness = m.thickness;
    if ((pc.flags & 1u) != 0u && entering) {
        float bd = OX_SAMPLE_2D_LOD(pc.backDepth, OX_SAMPLER_NEAREST_CLAMP, uv, 0.0).r;
        if (bd > 0.0) {
            float backLin = oxLinearDepth(pc.view, bd);
            float cosView = max(dot(-I, normalize(VIEW.invView[2].xyz)), 0.2); // view ray vs camera axis
            thickness = max(min(backLin, sceneLin) - viewDepth, 0.0) / cosView;
        }
    }

    vec3 refracted = vec3(0.0);
    if (!tir) {
        // Follow the refracted ray through the object and to the background behind it.
        float behind = clamp(sceneLin - viewDepth, 0.0, pc.maxRefractionDistance);
        float travel = (thickness + behind) * pc.refractionStrength;
        float d;
        vec2 ruv = oxProjectToUv(VIEW.viewProj, vWorldPos + T * travel, d);
        ruv = clamp(ruv, vec2(0.5) * VIEW.renderSize.zw, vec2(1.0) - 0.5 * VIEW.renderSize.zw);
        // Never refract foreground geometry that is in front of the surface.
        if (oxSceneLinearDepth(pc.view, pc.sceneDepth, ruv) < viewDepth) ruv = uv;
        refracted = oxSampleRefraction(pc.refraction, pc.refractionMips, ruv, s.perceptualRoughness) / max(VIEW.preExposure, 1e-12);
        refracted *= oxBeerLambert(m.absorptionColor, m.absorptionDistance, thickness) * ms.baseColor.rgb;
    }

    vec3 color = s.emissive * invExposure;
    if (!unlit) {
        OxLightingResult lit = oxEvaluateLighting(pc.view, pc.scene, s, gl_FragCoord.xy, viewDepth, inputs);
        color += lit.direct + lit.indirect;
        // Split sum already weights the IBL reflection by F; with TIR everything is reflected.
        if (tir) color += oxSamplePrefiltered(pc.view, reflect(I, N), s.perceptualRoughness) * VIEW.iblIntensity * (1.0 - f0);
    }
    color += (1.0 - F) * transmission * refracted;
    color = oxTranslucencyFog(pc.view, pc.scene, color, vWorldPos, uv, pc.fog);
    outColor = vec4(color * VIEW.preExposure, 1.0);
#else
    oxSurfaceFinalize(s);
    vec3 color;
    if (unlit) {
        color = s.baseColor * invExposure;
    } else {
        OxLightingResult lit = oxEvaluateLighting(pc.view, pc.scene, s, gl_FragCoord.xy, viewDepth, inputs);
        color = lit.direct + lit.indirect + s.emissive * invExposure;
    }
    color = oxTranslucencyFog(pc.view, pc.scene, color, vWorldPos, uv, pc.fog);
    float a = clamp(s.alpha, 0.0, 1.0);
    vec3 premul = color * VIEW.preExposure * a;
#ifdef OX_MODE_OIT
    float w = oxOitWeight(a, viewDepth);
    outAccum = vec4(premul, a) * w;
    outReveal = vec4(a);
#else
    outColor = vec4(premul, a);
#endif
#endif
}
