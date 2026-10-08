#version 460
// Impostor billboard (4 vertices, index buffer {0,1,2, 0,2,3}): faces the camera (or the light with OX_VEG_SHADOW,
// light travel direction packed with oxOctEncode in pc.lightDirOct) in the instance's object space so the frame selection is
// rotation aware.
#include "impostor_common.glsl"

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vObjDir;
layout(location = 2) out vec2 vUv;
layout(location = 3) out vec4 vCurClip;
layout(location = 4) out vec4 vPrevClip;
layout(location = 5) flat out uint vFade;
layout(location = 6) flat out uint vInstance;

invariant gl_Position;

void main() {
    uvec2 rec = pc.records.r[gl_InstanceIndex];
    VegInstance inst = pc.instances.i[rec.x];
    VegProto proto = pc.protos.p[pc.proto];
    mat4 W = oxVegMatrix(inst);
    float radius = proto.impostor.y;
    vec3 centerObj = vec3(0.0, proto.impostor.z, 0.0);
    vec3 centerW = (W * vec4(centerObj, 1.0)).xyz;
#ifdef OX_VEG_SHADOW
    vec3 toEye = -oxOctDecode(unpackUnorm2x16(pc.lightDirOct));
#else
    vec3 toEye = (VIEW.flags & OX_VIEW_ORTHOGRAPHIC) != 0u ? VIEW.invView[2].xyz : VIEW.cameraPosition.xyz - centerW;
#endif
    mat3 R = mat3(W);
    vec3 objDir = normalize(transpose(R) * toEye);
    vec3 basisDir = normalize(vec3(objDir.x, max(objDir.y, 0.0), objDir.z) + vec3(0.0, 1e-4, 0.0));
    vec3 right, up;
    oxImpostorBasis(basisDir, right, up);
    vec2 uv = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    if (gl_VertexIndex == 3) uv = vec2(0.0, 1.0);
    if (gl_VertexIndex == 2) uv = vec2(1.0, 1.0);
    vec2 o = uv * 2.0 - 1.0;
    vec3 posObj = centerObj + (right * o.x + up * o.y) * radius;
    vec3 world = (W * vec4(posObj, 1.0)).xyz;
    float heightWorld = proto.wind.w * length(W[1].xyz);
    vec3 n = normalize(R * basisDir);
    vec3 cur = oxVegAnimate(pc.frame, proto, world, posObj, W[3].xyz, heightWorld, vec4(0.0), n, inst.random, false);
    vWorldPos = cur;
    vObjDir = objDir;
    vUv = vec2(uv.x, 1.0 - uv.y);
    vFade = rec.y;
    vInstance = rec.x;
#ifdef OX_VEG_SHADOW
    vCurClip = vec4(0.0);
    vPrevClip = vec4(0.0);
    gl_Position = pc.matrices.m[0] * vec4(cur, 1.0);
#else
    vec3 prev = oxVegAnimate(pc.frame, proto, world, posObj, W[3].xyz, heightWorld, vec4(0.0), n, inst.random, true);
    vCurClip = VIEW.unjitteredViewProj * vec4(cur, 1.0);
    vPrevClip = VIEW.prevUnjitteredViewProj * vec4(prev, 1.0);
    gl_Position = VIEW.viewProj * vec4(cur, 1.0);
#endif
}
