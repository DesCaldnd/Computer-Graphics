#version 460
// Vegetation mesh LODs: vertex pulling from the scene arenas (gl_VertexIndex includes the mesh base vertex), instance
// from the culled record list (gl_InstanceIndex = record), wind + interaction, previous position for motion vectors.
// OX_VEG_SHADOW: shadow cascades (matrix from the push block).
#include "vegetation_common.glsl"

OX_RENDER_PUSH(VegInstanceBuffer instances; VegRecordBuffer records; VegFrameBuffer frame; VegProtoBuffer protos; OxWorldMatrixBuffer matrices; uint proto; uint material; uint entityId; uint unused; uint inputs[4];);

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec4 vTangent;
layout(location = 3) out vec2 vUv0;
layout(location = 4) out vec4 vColor;
layout(location = 5) out vec4 vCurClip;
layout(location = 6) out vec4 vPrevClip;
layout(location = 7) flat out uint vFade;

invariant gl_Position;

void main() {
    uvec2 rec = pc.records.r[gl_InstanceIndex];
    VegInstance inst = pc.instances.i[rec.x];
    VegProto proto = pc.protos.p[pc.proto];
    vec3 p = SCENE.positions.p[gl_VertexIndex];
    VertexAttributes a = SCENE.attributes.a[gl_VertexIndex];
    mat4 W = oxVegMatrix(inst);
    vec3 base = W[3].xyz;
    float heightWorld = proto.wind.w * length(W[1].xyz);
    vec3 n = normalize(oxCofactor(W) * a.normal);
    vec4 color = oxUnpackColor(a.color);
    vec3 world = (W * vec4(p, 1.0)).xyz;
    vec3 cur = oxVegAnimate(pc.frame, proto, world, p, base, heightWorld, color, n, inst.random, false);
    vWorldPos = cur;
    vNormal = n;
    vTangent = vec4(normalize(mat3(W) * a.tangent.xyz), a.tangent.w);
    vUv0 = a.uv0;
    vec4 tint = oxUnpackColor(inst.tint);
    vColor = vec4(tint.rgb, color.a);
    vFade = rec.y;
#ifdef OX_VEG_SHADOW
    vCurClip = vec4(0.0);
    vPrevClip = vec4(0.0);
    gl_Position = pc.matrices.m[0] * vec4(cur, 1.0);
#else
    vec3 prev = oxVegAnimate(pc.frame, proto, world, p, base, heightWorld, color, n, inst.random, true);
    vCurClip = VIEW.unjitteredViewProj * vec4(cur, 1.0);
    vPrevClip = VIEW.prevUnjitteredViewProj * vec4(prev, 1.0);
    gl_Position = VIEW.viewProj * vec4(cur, 1.0);
#endif
}
