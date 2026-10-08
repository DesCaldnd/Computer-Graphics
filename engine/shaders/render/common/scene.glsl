// OxwaldEngine render: GPU scene buffers (mirrors ox::render::GpuSceneHeader / GpuInstance / GpuMeshInfo /
// GpuMaterial / GpuLight / GpuShadow, scalar layout) and vertex pulling helpers.
#ifndef OX_RENDER_SCENE_GLSL
#define OX_RENDER_SCENE_GLSL

#include "view.glsl"

struct Instance {
    mat4 world;
    mat4 prevWorld;
    vec4 boundingSphere;  // world xyz + radius
    uint meshIndex;
    uint materialIndex;
    uint flags;
    uint entityId;
    uint paletteOffset;
    uint prevPaletteOffset;
    uint userData;
    uint pad;
};

const uint OX_INSTANCE_CAST_SHADOWS = 1u;
const uint OX_INSTANCE_RECEIVE_SHADOWS = 2u;
const uint OX_INSTANCE_SKINNED = 4u;
const uint OX_INSTANCE_MOVED = 8u;
const uint OX_INSTANCE_SELECTED = 16u;
const uint OX_INSTANCE_SKINNED_OUTPUT = 64u; // compute-skinned: palette offsets index SceneHeader.skinnedVertices

struct MeshInfo {
    uint firstIndex;
    uint indexCount;
    int vertexOffset;
    uint vertexCount;
    vec4 boundingSphere;
    vec3 aabbMin;
    uint meshletOffset;
    vec3 aabbMax;
    uint meshletCount;
    uint skinOffset;
    uint lodCount;
    uint pad0;
    uint pad1;
};

struct Material {
    vec4 baseColor;
    vec3 emissive;
    float metallic;
    float roughness;
    float normalStrength;
    float occlusionStrength;
    float alphaCutoff;
    uint albedoTexture;
    uint normalTexture;
    uint ormTexture;
    uint emissiveTexture;
    uint flags;           // bits 0-2 blend mode (0 opaque, 1 alpha test, 2 transparent, 3 refractive), 3 double sided, 4 unlit
    uint samplerIndex;
    float ior;
    float transmission;
    vec3 absorptionColor;
    float absorptionDistance;
    float thickness;
    float clearcoat;
    float clearcoatRoughness;
    float subsurface;
    vec2 uvTiling;
    vec2 uvOffset;
};

const uint OX_BLEND_OPAQUE = 0u;
const uint OX_BLEND_ALPHA_TEST = 1u;
const uint OX_BLEND_TRANSPARENT = 2u;
const uint OX_BLEND_REFRACTIVE = 3u;
const uint OX_MATERIAL_DOUBLE_SIDED = 8u;
const uint OX_MATERIAL_UNLIT = 16u;

const uint OX_LIGHT_DIRECTIONAL = 0u;
const uint OX_LIGHT_POINT = 1u;
const uint OX_LIGHT_SPOT = 2u;

struct Light {
    vec3 position;
    float range;
    vec3 color;        // colour × intensity: candela (point/spot), lux (directional)
    uint type;
    vec3 direction;    // direction the light travels
    float sourceRadius;
    float spotScale;
    float spotOffset;
    int shadowIndex;
    uint entityId;
};

struct Shadow {
    mat4 viewProj;
    vec4 atlasRect;    // uv offset xy, scale zw
    float bias;
    float normalBias;
    float nearPlane;
    float farPlane;
    uint kind;         // 0 spot (atlas), 1 point (cube faces in PointShadows array)
    uint cubeLayer;
    float texelSize;
    float lightSize;   // PCSS: sourceRadius * resolution / (2 tan(fov/2))
};

struct VertexAttributes {
    vec3 normal;
    vec4 tangent; // xyz, w = bitangent sign
    vec2 uv0;
    vec2 uv1;
    uint color;   // RGBA8, R in the low byte
};

struct SkinVertex {
    uint joints01;  // 2 × u16
    uint joints23;
    uint weights01; // 2 × unorm16
    uint weights23;
};

OX_READONLY_BUFFER(InstanceBuffer, { Instance i[]; });
OX_READONLY_BUFFER(MeshInfoBuffer, { MeshInfo m[]; });
OX_READONLY_BUFFER(MaterialBuffer, { Material m[]; });
OX_READONLY_BUFFER(PositionBuffer, { vec3 p[]; });
OX_READONLY_BUFFER(AttributeBuffer, { VertexAttributes a[]; });
OX_READONLY_BUFFER(SkinBuffer, { SkinVertex s[]; });
OX_READONLY_BUFFER(PaletteBuffer, { mat4 m[]; });
OX_READONLY_BUFFER(LightBuffer, { Light l[]; });
OX_READONLY_BUFFER(ShadowBuffer, { Shadow s[]; });
OX_READONLY_BUFFER(MeshletBuffer, { uint data[]; });
OX_READONLY_BUFFER(InstanceIdBuffer, { uint id[]; });

// Compute skinning output (mirrors ox::render::GpuSkinnedVertex; model space).
struct SkinnedVertex {
    vec3 position;
    vec3 normal;
    vec4 tangent;
};
OX_READONLY_BUFFER(SkinnedVertexBuffer, { SkinnedVertex v[]; });

struct SceneHeader {
    InstanceBuffer instances;
    MaterialBuffer materials;
    MeshInfoBuffer meshes;
    PositionBuffer positions;
    AttributeBuffer attributes;
    SkinBuffer skin;
    PaletteBuffer palettes;
    LightBuffer lights;
    ShadowBuffer shadows;
    MeshletBuffer meshlets;
    uint instanceCount;
    uint materialCount;
    uint lightCount;            // directional lights first
    uint directionalLightCount;
    uint shadowCount;
    uint meshCount;
    SkinnedVertexBuffer skinnedVertices; // compute skinning output (0 when unused)
};

OX_READONLY_BUFFER(SceneBuffer, { SceneHeader s; });

// Push constants of scene geometry passes: view, scene, per-draw instance ids (gl_InstanceIndex → instance),
// then up to 104 bytes of pass data (fields must not contain commas, declare one per statement).
#define OX_RENDER_PUSH(Fields) \
    layout(push_constant, scalar) uniform OxPushConstants { ViewBuffer view; SceneBuffer scene; Fields } pc
#define OX_RENDER_DRAW_PUSH(Fields) \
    layout(push_constant, scalar) uniform OxPushConstants { ViewBuffer view; SceneBuffer scene; InstanceIdBuffer drawIds; Fields } pc

#define VIEW pc.view.v
#define SCENE pc.scene.s

// --- vertex pulling ---

struct OxVertex {
    vec3 position;     // world
    vec3 prevPosition; // world, previous frame (motion vectors)
    vec3 normal;       // world (not normalised)
    vec4 tangent;      // world xyz + sign
    vec2 uv0;
    vec2 uv1;
    vec4 color;
};

vec4 oxUnpackColor(uint c) {
    return vec4(float(c & 0xffu), float((c >> 8) & 0xffu), float((c >> 16) & 0xffu), float(c >> 24)) / 255.0;
}

// Cofactor matrix = inverse transpose × determinant: transforms normals correctly under non-uniform scale.
mat3 oxCofactor(mat4 m) {
    vec3 a = m[0].xyz, b = m[1].xyz, c = m[2].xyz;
    return mat3(cross(b, c), cross(c, a), cross(a, b));
}

mat4 oxSkinMatrix(SceneBuffer sb, uint paletteOffset, SkinVertex sv) {
    PaletteBuffer pal = sb.s.palettes;
    uvec4 j = uvec4(sv.joints01 & 0xffffu, sv.joints01 >> 16, sv.joints23 & 0xffffu, sv.joints23 >> 16);
    vec4 w = vec4(float(sv.weights01 & 0xffffu), float(sv.weights01 >> 16), float(sv.weights23 & 0xffffu),
                  float(sv.weights23 >> 16)) / 65535.0;
    return pal.m[paletteOffset + j.x] * w.x + pal.m[paletteOffset + j.y] * w.y + pal.m[paletteOffset + j.z] * w.z +
           pal.m[paletteOffset + j.w] * w.w;
}

// vertexIndex = gl_VertexIndex (already includes the mesh's vertexOffset from drawIndexed).
OxVertex oxFetchVertex(SceneBuffer sb, uint instanceId, uint vertexIndex) {
    InstanceBuffer instances = sb.s.instances;
    vec3 p = sb.s.positions.p[vertexIndex];
    VertexAttributes a = sb.s.attributes.a[vertexIndex];
    mat4 world = instances.i[instanceId].world;
    mat4 prevWorld = instances.i[instanceId].prevWorld;
    uint flags = instances.i[instanceId].flags;
    vec3 prevP = p;
    if ((flags & OX_INSTANCE_SKINNED_OUTPUT) != 0u) {
        // Pre-skinned by the compute pass (model space, current + previous frame).
        uint meshIndex = instances.i[instanceId].meshIndex;
        uint local = vertexIndex - uint(sb.s.meshes.m[meshIndex].vertexOffset);
        SkinnedVertexBuffer sv = sb.s.skinnedVertices;
        SkinnedVertex cur = sv.v[instances.i[instanceId].paletteOffset + local];
        p = cur.position;
        prevP = sv.v[instances.i[instanceId].prevPaletteOffset + local].position;
        a.normal = cur.normal;
        a.tangent = cur.tangent;
    } else if ((flags & OX_INSTANCE_SKINNED) != 0u) {
        uint meshIndex = instances.i[instanceId].meshIndex;
        uint paletteOffset = instances.i[instanceId].paletteOffset;
        uint prevPaletteOffset = instances.i[instanceId].prevPaletteOffset;
        MeshInfoBuffer meshes = sb.s.meshes;
        uint skinIndex = meshes.m[meshIndex].skinOffset + (vertexIndex - uint(meshes.m[meshIndex].vertexOffset));
        SkinVertex sv = sb.s.skin.s[skinIndex];
        mat4 skin = oxSkinMatrix(sb, paletteOffset, sv);
        mat4 prevSkin = prevPaletteOffset != OX_INVALID_INDEX ? oxSkinMatrix(sb, prevPaletteOffset, sv) : skin;
        world = world * skin;
        prevWorld = prevWorld * prevSkin;
    }
    OxVertex v;
    v.position = (world * vec4(p, 1.0)).xyz;
    v.prevPosition = (prevWorld * vec4(prevP, 1.0)).xyz;
    v.normal = oxCofactor(world) * a.normal;
    v.tangent = vec4(mat3(world) * a.tangent.xyz, a.tangent.w);
    v.uv0 = a.uv0;
    v.uv1 = a.uv1;
    v.color = oxUnpackColor(a.color);
    return v;
}

uint oxMaterialBlend(Material m) { return m.flags & 7u; }

#endif
