#version 460
// Impostor depth prepass (OX_VEG_SHADOW: shadow cascades): coverage test, dithered fade, world normal, velocity.
#include "impostor_sample.glsl"

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vObjDir;
layout(location = 2) in vec2 vUv;
layout(location = 3) in vec4 vCurClip;
layout(location = 4) in vec4 vPrevClip;
layout(location = 5) flat in uint vFade;
layout(location = 6) flat in uint vInstance;

#ifndef OX_VEG_SHADOW
layout(location = 0) out vec4 outNormals;
layout(location = 1) out vec2 outVelocity;
#ifdef OX_ENTITY_ID
layout(location = 2) out uint outEntity;
#endif
#endif

void main() {
    VegProto proto = pc.protos.p[pc.proto];
    OxImpostorSample is = oxSampleImpostor(proto, vObjDir, vUv);
    if (is.albedo.a < proto.impostor.w) discard;
#ifndef OX_VEG_SHADOW
    if (!oxWorldDitherKeep(gl_FragCoord.xy, vFade)) discard;
    mat3 R = mat3(oxVegMatrix(pc.instances.i[vInstance]));
    outNormals = vec4(normalize(R * is.normal), 0.8);
    outVelocity = oxWorldVelocity(vCurClip, vPrevClip);
#ifdef OX_ENTITY_ID
    outEntity = pc.entityId;
#endif
#endif
}
