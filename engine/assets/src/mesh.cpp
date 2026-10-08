#include <oxwald/assets/mesh.hpp>
#include <oxwald/assets/mesh_processing.hpp>

#include "internal.hpp"

namespace ox::assets {

namespace {
constexpr u32 kMeshVersion = 1;

void writeAabb(ByteWriter& w, const AABB& b) {
    w.write(b.min);
    w.write(b.max);
}
AABB readAabb(ByteReader& r) {
    AABB b;
    b.min = r.read<glm::vec3>();
    b.max = r.read<glm::vec3>();
    return b;
}
} // namespace

u32 MeshData::lodCount() const {
    u32 n = 0;
    for (const auto& s : submeshes) n = std::max(n, static_cast<u32>(s.lods.size()));
    return n;
}

u64 MeshData::triangleCount(u32 lod) const {
    u64 n = 0;
    for (const auto& s : submeshes) {
        if (s.lods.empty()) continue;
        n += s.lods[std::min<usize>(lod, s.lods.size() - 1)].indexCount / 3;
    }
    return n;
}

usize MeshData::memoryUsage() const {
    return positions.size() * sizeof(glm::vec3) + attributes.size() * sizeof(VertexAttributes) +
           skin.size() * sizeof(SkinVertex) + indices.size() * sizeof(u32) + meshlets.size() * sizeof(Meshlet) +
           meshletVertices.size() * sizeof(u32) + meshletTriangles.size() +
           (collision.hullVertices.size() + collision.vertices.size()) * sizeof(glm::vec3) +
           (collision.hullIndices.size() + collision.indices.size()) * sizeof(u32) + sizeof(MeshData);
}

std::vector<std::byte> serializeMesh(const MeshData& mesh) {
    detail::ChunkWriter cw;
    {
        ByteWriter w;
        w.writeString(mesh.name);
        writeAabb(w, mesh.bounds);
        w.write(mesh.boundingSphere.center);
        w.write(mesh.boundingSphere.radius);
        w.writeUuid(mesh.skeleton);
        w.write(mesh.vertexCount());
        cw.add("INFO", w.take());
    }
    {
        ByteWriter w;
        w.write(static_cast<u32>(mesh.submeshes.size()));
        for (const auto& s : mesh.submeshes) {
            w.writeString(s.name);
            w.write(s.materialSlot);
            w.write(s.vertexOffset);
            w.write(s.vertexCount);
            writeAabb(w, s.bounds);
            w.write(static_cast<u32>(s.lods.size()));
            for (const auto& l : s.lods) w.write(l);
        }
        cw.add("SUBM", w.take());
    }
    {
        ByteWriter w;
        w.write(static_cast<u32>(mesh.materials.size()));
        for (const auto& m : mesh.materials) {
            w.writeString(m.name);
            w.writeUuid(m.material);
        }
        cw.add("MATS", w.take());
    }
    cw.addSpan("POSN", std::span(mesh.positions));
    cw.addSpan("ATTR", std::span(mesh.attributes));
    if (!mesh.skin.empty()) cw.addSpan("SKIN", std::span(mesh.skin));
    cw.addSpan("INDX", std::span(mesh.indices));
    cw.addSpan("MSHL", std::span(mesh.meshlets));
    cw.addSpan("MVTX", std::span(mesh.meshletVertices));
    cw.addSpan("MTRI", std::span(mesh.meshletTriangles));
    if (!mesh.collision.empty()) {
        cw.addSpan("COLH", std::span(mesh.collision.hullVertices));
        cw.addSpan("COLI", std::span(mesh.collision.hullIndices));
        cw.addSpan("COLV", std::span(mesh.collision.vertices));
        cw.addSpan("COLX", std::span(mesh.collision.indices));
    }
    return cw.finish("OXMS", kMeshVersion);
}

Result<MeshData> deserializeMesh(std::span<const std::byte> data) {
    auto cr = detail::ChunkReader::parse(data, "OXMS");
    if (!cr) return cr.error();
    if (cr->version() > kMeshVersion) return makeError("mesh version {} is newer than supported", cr->version());
    MeshData mesh;
    {
        ByteReader r(cr->get("INFO"));
        mesh.name = r.readString();
        mesh.bounds = readAabb(r);
        mesh.boundingSphere.center = r.read<glm::vec3>();
        mesh.boundingSphere.radius = r.read<f32>();
        mesh.skeleton = r.readUuid();
        r.read<u32>();
        if (r.failed()) return makeError("corrupt mesh INFO chunk");
    }
    {
        ByteReader r(cr->get("SUBM"));
        const u32 n = r.read<u32>();
        if (n > 1u << 20) return makeError("corrupt submesh table");
        for (u32 i = 0; i < n && !r.failed(); ++i) {
            Submesh s;
            s.name = r.readString();
            s.materialSlot = r.read<u32>();
            s.vertexOffset = r.read<u32>();
            s.vertexCount = r.read<u32>();
            s.bounds = readAabb(r);
            const u32 lods = r.read<u32>();
            if (lods > 64) return makeError("corrupt LOD table");
            for (u32 l = 0; l < lods; ++l) s.lods.push_back(r.read<MeshLod>());
            mesh.submeshes.push_back(std::move(s));
        }
        if (r.failed()) return makeError("corrupt mesh SUBM chunk");
    }
    {
        ByteReader r(cr->get("MATS"));
        const u32 n = r.read<u32>();
        for (u32 i = 0; i < n && !r.failed() && i < (1u << 16); ++i) {
            MaterialSlot m;
            m.name = r.readString();
            m.material = r.readUuid();
            mesh.materials.push_back(std::move(m));
        }
        if (r.failed()) return makeError("corrupt mesh MATS chunk");
    }
    bool ok = cr->read("POSN", mesh.positions) && cr->read("ATTR", mesh.attributes) && cr->read("SKIN", mesh.skin) &&
              cr->read("INDX", mesh.indices) && cr->read("MSHL", mesh.meshlets) &&
              cr->read("MVTX", mesh.meshletVertices) && cr->read("MTRI", mesh.meshletTriangles) &&
              cr->read("COLH", mesh.collision.hullVertices) && cr->read("COLI", mesh.collision.hullIndices) &&
              cr->read("COLV", mesh.collision.vertices) && cr->read("COLX", mesh.collision.indices);
    if (!ok) return makeError("corrupt mesh stream chunk");
    if (mesh.attributes.size() != mesh.positions.size() || (!mesh.skin.empty() && mesh.skin.size() != mesh.positions.size())) {
        return makeError("mesh vertex streams have different lengths");
    }
    // Validate ranges so a corrupt file cannot make the renderer read out of bounds.
    const u32 vcount = mesh.vertexCount();
    for (u32 idx : mesh.indices) {
        if (idx >= vcount) return makeError("mesh index out of range");
    }
    for (const auto& s : mesh.submeshes) {
        for (const auto& l : s.lods) {
            if (u64(l.indexOffset) + l.indexCount > mesh.indices.size() ||
                u64(l.meshletOffset) + l.meshletCount > mesh.meshlets.size()) {
                return makeError("submesh LOD range out of bounds");
            }
        }
    }
    for (const auto& m : mesh.meshlets) {
        if (u64(m.vertexOffset) + m.vertexCount > mesh.meshletVertices.size() ||
            u64(m.triangleOffset) + u64(m.triangleCount) * 3 > mesh.meshletTriangles.size()) {
            return makeError("meshlet range out of bounds");
        }
    }
    return mesh;
}

MeshData makeCubeMesh(f32 h) {
    MeshData mesh;
    mesh.name = "Cube";
    struct Face {
        glm::vec3 n, u, v;
    };
    // u x v = n so the triangles are CCW seen from outside.
    const Face faces[6] = {
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},  {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},  {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},   {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    };
    for (const auto& f : faces) {
        const u32 base = mesh.vertexCount();
        const glm::vec2 corners[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (const auto& c : corners) {
            mesh.positions.push_back((f.n + f.u * c.x + f.v * c.y) * h);
            VertexAttributes a;
            a.normal = f.n;
            a.tangent = glm::vec4(f.u, 1.0f);
            a.uv0 = glm::vec2(c.x * 0.5f + 0.5f, 0.5f - c.y * 0.5f);
            a.uv1 = a.uv0;
            mesh.attributes.push_back(a);
        }
        for (u32 i : {0u, 1u, 2u, 0u, 2u, 3u}) mesh.indices.push_back(base + i);
    }
    Submesh s;
    s.name = "Cube";
    s.vertexCount = mesh.vertexCount();
    s.lods.push_back(MeshLod{0, static_cast<u32>(mesh.indices.size()), 0, 0, 0.0f});
    mesh.submeshes.push_back(std::move(s));
    mesh.materials.push_back({"Default", builtin::defaultMaterial()});
    MeshProcessSettings settings;
    settings.generateLods = false;
    settings.optimizeVertexCache = false;
    processMesh(mesh, settings);
    // Fix the tangent sign for the generated UV orientation.
    generateTangents(mesh.positions, mesh.attributes, mesh.indices);
    return mesh;
}

} // namespace ox::assets
