#version 460
// Vegetation forward shading: material sampling, two-sided foliage with sun translucency. Depth EQUAL against the
// vegetation prepass (the dither / alpha test already ran there).
#include "vegetation_common.glsl"
#include "world_shading.glsl"

OX_RENDER_PUSH(VegInstanceBuffer instances; VegRecordBuffer records; VegFrameBuffer frame; VegProtoBuffer protos; OxWorldMatrixBuffer matrices; uint proto; uint material; uint entityId; uint unused; uint inputs[4];);

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vTangent;
layout(location = 3) in vec2 vUv0;
layout(location = 4) in vec4 vColor;
layout(location = 5) in vec4 vCurClip;
layout(location = 6) in vec4 vPrevClip;
layout(location = 7) flat in uint vFade;

layout(location = 0) out vec4 outColor;

void main() {
    Material m = SCENE.materials.m[pc.material];
    VegProto proto = pc.protos.p[pc.proto];
    vec3 camPos = VIEW.cameraPosition.xyz;
    vec3 V = (VIEW.flags & OX_VIEW_ORTHOGRAPHIC) != 0u ? normalize(VIEW.invView[2].xyz) : normalize(camPos - vWorldPos);
    // Two-sided foliage: both faces use the (viewer-bent) canopy normal, see oxFoliageNormal.
    vec3 faceN = normalize(cross(dFdx(vWorldPos), dFdy(vWorldPos)));
    vec3 Ls = oxSunDirection(pc.view, pc.scene);
    vec3 n = oxFoliageNormal(vNormal, faceN, V, Ls);
    Material mm = m;
    mm.flags &= ~OX_MATERIAL_DOUBLE_SIDED;
    OxMaterialSample ms = oxSampleMaterial(mm, vUv0, vec4(vColor.rgb, 1.0), n, vTangent, true, VIEW.mipBias);
    OxSurface s;
    s.position = vWorldPos;
    s.normal = oxFoliageNormal(ms.normal, faceN, V, Ls);
    s.geometricNormal = n;
    s.view = V;
    s.baseColor = ms.baseColor.rgb;
    s.alpha = 1.0;
    s.metallic = clamp(ms.metallic, 0.0, 1.0);
    s.perceptualRoughness = ms.perceptualRoughness;
    s.occlusion = ms.occlusion;
    s.emissive = ms.emissive;
    oxSurfaceFinalize(s);
    // Thin alpha-tested foliage transmits light; solid parts (bark) only when the material has a subsurface amount.
    bool leaves = oxMaterialBlend(m) == OX_BLEND_ALPHA_TEST;
    vec3 translucency = vec3(proto.wind.z * (m.subsurface > 0.0 ? m.subsurface : (leaves ? 1.0 : 0.0)));
    OxWorldInputs inp = OxWorldInputs(pc.inputs[0], pc.inputs[1], pc.inputs[2], pc.inputs[3]);
    outColor = oxWorldShade(pc.view, pc.scene, s, gl_FragCoord.xy, inp, translucency, true);
}
