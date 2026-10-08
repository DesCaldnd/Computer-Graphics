#include "test_helpers.hpp"

using namespace oxtest;

namespace {

void expectTangentsOrthogonal(const MeshData& mesh) {
    for (const auto& a : mesh.attributes) {
        EXPECT_NEAR(glm::length(a.normal), 1.0f, 1e-3f);
        EXPECT_NEAR(glm::length(glm::vec3(a.tangent)), 1.0f, 1e-3f);
        EXPECT_NEAR(glm::dot(a.normal, glm::vec3(a.tangent)), 0.0f, 1e-3f);
        EXPECT_TRUE(a.tangent.w == 1.0f || a.tangent.w == -1.0f);
    }
}

void expectMeshletsValid(const MeshData& mesh, u32 maxV = 64, u32 maxT = 124) {
    for (const auto& m : mesh.meshlets) {
        EXPECT_LE(m.vertexCount, maxV);
        EXPECT_LE(m.triangleCount, maxT);
        EXPECT_GT(m.triangleCount, 0u);
        EXPECT_EQ(m.triangleOffset % 4, 0u);
        for (u32 t = 0; t < m.triangleCount * 3; ++t) {
            const u8 local = mesh.meshletTriangles[m.triangleOffset + t];
            ASSERT_LT(local, m.vertexCount);
            const u32 v = mesh.meshletVertices[m.vertexOffset + local];
            ASSERT_LT(v, mesh.vertexCount());
            // The bounding sphere contains every vertex of the meshlet.
            EXPECT_LE(glm::distance(mesh.positions[v], m.center), m.radius * 1.001f + 1e-4f);
        }
    }
}

} // namespace

TEST(MeshImport, GltfBoxHierarchyAndMaterials) {
    TempDir dir;
    const auto path = writeTestGltf(dir.path(), "box");
    auto model = importModel(path);
    ASSERT_TRUE(model) << model.error().message;
    EXPECT_EQ(model->importer, "fastgltf");
    ASSERT_EQ(model->meshes.size(), 1u);
    const MeshData& mesh = model->meshes[0];
    EXPECT_EQ(mesh.submeshes.size(), 2u);
    EXPECT_EQ(mesh.materials.size(), 2u);
    EXPECT_EQ(mesh.triangleCount(), 12u);
    // Both primitives share the 24 source vertices; each submesh keeps only what it references.
    EXPECT_EQ(mesh.submeshes[0].vertexCount, 4u);
    EXPECT_EQ(mesh.submeshes[1].vertexCount, 20u);
    expectTangentsOrthogonal(mesh);
    ASSERT_EQ(model->nodes.size(), 2u);
    EXPECT_EQ(model->nodes[0].name, "Root");
    EXPECT_EQ(model->nodes[1].parent, 0);
    EXPECT_EQ(model->nodes[1].meshes, std::vector<u32>{0});
    EXPECT_FLOAT_EQ(model->nodes[1].local.scale.x, 2.0f);
    ASSERT_EQ(model->materials.size(), 2u);
    const auto& m0 = model->materials[0];
    EXPECT_EQ(m0.name, "Painted");
    EXPECT_FLOAT_EQ(m0.material.baseColor.g, 0.5f);
    EXPECT_FLOAT_EQ(m0.material.normalStrength, 0.8f);
    EXPECT_EQ(m0.albedo.path, "box_albedo.png");
    EXPECT_EQ(m0.normal.path, "box_n.png");
    EXPECT_EQ(model->materials[1].material.blendMode, BlendMode::AlphaTest);
    EXPECT_TRUE(model->materials[1].material.doubleSided);

    // Merged: node transforms baked => box of size 2 centred at y = 1.
    MeshData merged = mergeModel(*model);
    computeBounds(merged);
    EXPECT_NEAR(merged.bounds.min.y, 0.0f, 1e-5f);
    EXPECT_NEAR(merged.bounds.max.y, 2.0f, 1e-5f);
    EXPECT_NEAR(merged.bounds.max.x, 1.0f, 1e-5f);
}

TEST(MeshImport, UnitAndAxisConversion) {
    TempDir dir;
    const auto path = writeTestGltf(dir.path(), "box");
    ModelImportSettings s;
    s.upAxis = UpAxis::Z; // pretend the source was Z-up: +Z becomes +Y
    s.scale = 0.5f;
    auto model = importModel(path, s);
    ASSERT_TRUE(model);
    MeshData merged = mergeModel(*model);
    // Root translation (0,1,0) in Z-up space maps to (0,0,-1) in Y-up, scaled by 0.5.
    EXPECT_NEAR(merged.bounds.center().z, -0.5f, 1e-4f);
    EXPECT_NEAR(merged.bounds.size().x, 1.0f, 1e-4f);
}

TEST(MeshImport, LegacyObjViaAssimp) {
    auto model = importModel(legacyDir() / "sphere.obj");
    ASSERT_TRUE(model) << model.error().message;
    EXPECT_EQ(model->importer, "assimp");
    MeshData mesh = mergeModel(*model);
    EXPECT_GT(mesh.vertexCount(), 100u);
    EXPECT_GT(mesh.triangleCount(), 100u);
    computeBounds(mesh);
    EXPECT_NEAR(mesh.bounds.size().y, mesh.bounds.size().x, mesh.bounds.size().x * 0.05f); // a sphere
    expectTangentsOrthogonal(mesh);
}

TEST(MeshImport, ObjWithTexturedMaterials) {
    auto model = importModel(legacyDir() / "Soldier/soldier.obj");
    ASSERT_TRUE(model) << model.error().message;
    ASSERT_FALSE(model->materials.empty());
    usize textured = 0;
    for (const auto& m : model->materials) textured += m.albedo.valid();
    EXPECT_GT(textured, 0u);
}

TEST(MeshProcessing, LodsReduceTriangles) {
    auto mesh = importMeshFile(legacyDir() / "monkey.obj");
    ASSERT_TRUE(mesh) << mesh.error().message;
    ASSERT_GE(mesh->lodCount(), 2u);
    u64 prev = mesh->triangleCount(0);
    for (u32 l = 1; l < mesh->lodCount(); ++l) {
        const u64 tris = mesh->triangleCount(l);
        EXPECT_LT(tris, prev) << "LOD " << l;
        prev = tris;
    }
    EXPECT_LT(mesh->triangleCount(1), mesh->triangleCount(0) * 3 / 4);
    for (const auto& sm : mesh->submeshes) {
        for (usize l = 1; l < sm.lods.size(); ++l) EXPECT_GE(sm.lods[l].error, sm.lods[l - 1].error);
    }
}

TEST(MeshProcessing, MeshletLimitsAndBounds) {
    auto mesh = importMeshFile(legacyDir() / "sphere.obj");
    ASSERT_TRUE(mesh);
    ASSERT_FALSE(mesh->meshlets.empty());
    expectMeshletsValid(*mesh);
    // Every LOD's meshlets cover exactly its triangles.
    for (const auto& sm : mesh->submeshes) {
        for (const auto& lod : sm.lods) {
            u64 tris = 0;
            for (u32 i = 0; i < lod.meshletCount; ++i) tris += mesh->meshlets[lod.meshletOffset + i].triangleCount;
            EXPECT_EQ(tris, lod.indexCount / 3);
        }
    }
    MeshProcessSettings s;
    s.meshletMaxVertices = 32;
    s.meshletMaxTriangles = 64;
    buildMeshlets(*mesh, s);
    expectMeshletsValid(*mesh, 32, 64);
}

TEST(MeshProcessing, SerializationRoundTrip) {
    ModelImportSettings s;
    s.generateCollision = true;
    auto mesh = importMeshFile(legacyDir() / "monkey.obj", s);
    ASSERT_TRUE(mesh);
    mesh->materials[0].material = Uuid::fromName("mat");
    auto bytes = serializeMesh(*mesh);
    auto back = deserializeMesh(bytes);
    ASSERT_TRUE(back) << back.error().message;
    EXPECT_EQ(back->positions, mesh->positions);
    EXPECT_EQ(back->indices, mesh->indices);
    EXPECT_EQ(back->meshletTriangles, mesh->meshletTriangles);
    EXPECT_EQ(back->meshletVertices, mesh->meshletVertices);
    ASSERT_EQ(back->submeshes.size(), mesh->submeshes.size());
    EXPECT_EQ(back->submeshes[0].lods.size(), mesh->submeshes[0].lods.size());
    EXPECT_EQ(back->materials[0].material, Uuid::fromName("mat"));
    EXPECT_EQ(back->collision.hullVertices, mesh->collision.hullVertices);
    EXPECT_EQ(back->bounds.min, mesh->bounds.min);
    // Corruption is detected.
    bytes[bytes.size() / 2] ^= std::byte{0x55};
    EXPECT_FALSE(deserializeMesh(bytes));
}

TEST(MeshProcessing, ConvexHullContainsPoints) {
    std::vector<glm::vec3> pts;
    u64 seed = 1;
    auto rnd = [&] {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        return f32((seed >> 33) & 0xffffff) / f32(0xffffff) * 2.0f - 1.0f;
    };
    for (int i = 0; i < 2000; ++i) pts.push_back(glm::vec3(rnd(), rnd() * 0.5f, rnd() * 2.0f));
    ConvexHull hull = computeConvexHull(pts, 0);
    ASSERT_FALSE(hull.indices.empty());
    auto inside = [&](const ConvexHull& h, glm::vec3 p, f32 eps) {
        for (usize t = 0; t < h.indices.size(); t += 3) {
            const glm::vec3 a = h.vertices[h.indices[t]], b = h.vertices[h.indices[t + 1]], c = h.vertices[h.indices[t + 2]];
            const glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
            if (glm::dot(n, p - a) > eps) return false;
        }
        return true;
    };
    for (const auto& p : pts) ASSERT_TRUE(inside(hull, p, 1e-4f));
    // Euler characteristic of a closed triangulated sphere: V - E + F = 2, E = 3F/2.
    const usize F = hull.indices.size() / 3;
    EXPECT_EQ(i64(hull.vertices.size()) - i64(F * 3 / 2) + i64(F), 2);
    ConvexHull reduced = computeConvexHull(pts, 32);
    EXPECT_LE(reduced.vertices.size(), 32u);
    EXPECT_GE(reduced.vertices.size(), 8u);
}

TEST(MeshProcessing, CollisionData) {
    ModelImportSettings s;
    s.generateCollision = true;
    s.maxHullVertices = 48;
    auto mesh = importMeshFile(legacyDir() / "monkey.obj", s);
    ASSERT_TRUE(mesh);
    EXPECT_FALSE(mesh->collision.hullIndices.empty());
    EXPECT_LE(mesh->collision.hullVertices.size(), 48u);
    EXPECT_FALSE(mesh->collision.indices.empty());
    EXPECT_LT(mesh->collision.indices.size(), mesh->triangleCount(0) * 3);
}

TEST(MeshProcessing, CubeMesh) {
    MeshData cube = makeCubeMesh();
    EXPECT_EQ(cube.vertexCount(), 24u);
    EXPECT_EQ(cube.triangleCount(), 12u);
    EXPECT_FLOAT_EQ(cube.bounds.max.x, 0.5f);
    expectTangentsOrthogonal(cube);
    expectMeshletsValid(cube);
    // Outward winding: geometric normal of every triangle matches the vertex normal.
    for (usize t = 0; t < cube.indices.size(); t += 3) {
        const auto a = cube.positions[cube.indices[t]], b = cube.positions[cube.indices[t + 1]],
                   c = cube.positions[cube.indices[t + 2]];
        EXPECT_GT(glm::dot(glm::cross(b - a, c - a), cube.attributes[cube.indices[t]].normal), 0.0f);
    }
}
