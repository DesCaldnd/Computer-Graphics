#version 460
// Impostor forward shading (depth EQUAL against the impostor prepass): baked albedo/normal frames, foliage lighting.
#include "impostor_sample.glsl"
#include "world_shading.glsl"

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vObjDir;
layout(location = 2) in vec2 vUv;
layout(location = 3) in vec4 vCurClip;
layout(location = 4) in vec4 vPrevClip;
layout(location = 5) flat in uint vFade;
layout(location = 6) flat in uint vInstance;

layout(location = 0) out vec4 outColor;

void main() {
    VegProto proto = pc.protos.p[pc.proto];
    VegInstance inst = pc.instances.i[vInstance];
    OxImpostorSample is = oxSampleImpostor(proto, vObjDir, vUv);
    mat3 R = mat3(oxVegMatrix(inst));
    vec3 camPos = VIEW.cameraPosition.xyz;
    OxSurface s;
    s.position = vWorldPos;
    s.view = (VIEW.flags & OX_VIEW_ORTHOGRAPHIC) != 0u ? normalize(VIEW.invView[2].xyz) : normalize(camPos - vWorldPos);
    vec3 n = oxFoliageNormal(R * is.normal, s.view, s.view, oxSunDirection(pc.view, pc.scene));
    s.normal = n;
    s.geometricNormal = n;
    s.baseColor = is.albedo.rgb * oxUnpackColor(inst.tint).rgb;
    s.alpha = 1.0;
    s.metallic = 0.0;
    s.perceptualRoughness = 0.8;
    s.occlusion = 1.0;
    s.emissive = vec3(0.0);
    oxSurfaceFinalize(s);
    OxWorldInputs inp = OxWorldInputs(pc.inputs[0], pc.inputs[1], pc.inputs[2], pc.inputs[3]);
    outColor = oxWorldShade(pc.view, pc.scene, s, gl_FragCoord.xy, inp, vec3(proto.wind.z * 0.7), true);
}
