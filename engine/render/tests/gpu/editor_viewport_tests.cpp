// Editor integration: recording into an externally owned command list, Uuid picking, hidden entities.
#include "render_fixture.hpp"

#include <oxwald/render/editor_viewport.hpp>

using namespace ox;
using namespace ox::render;
using namespace ox::render::test;

using EditorViewportTest = RenderTest;

TEST_F(EditorViewportTest, RecordsIntoCallerCommandListAndPicksUuids) {
    renderer.reset(); // the editor path owns its own Renderer
    EditorViewportRenderer evr(*device);
    auto mat = [&](glm::vec4 c) {
        assets::MaterialAsset m;
        m.baseColor = c;
        const Uuid id = Uuid::generate();
        evr.renderer().resources().addMaterial(id, m);
        return id;
    };
    auto addMesh = [&](Primitive p, Uuid m, glm::vec3 pos) {
        Entity e = world->create(primitiveName(p));
        e.setPosition(pos);
        auto& mr = e.add<MeshRendererComponent>();
        mr.mesh = primitiveUuid(p);
        mr.materials = {m};
        return e;
    };
    Entity red = addMesh(Primitive::Cube, mat({0.9f, 0.1f, 0.1f, 1}), {-1.2f, 0, 0});
    Entity green = addMesh(Primitive::Sphere, mat({0.1f, 0.9f, 0.1f, 1}), {1.2f, 0, 0});
    sun({-0.3f, -0.8f, -0.5f}, 20000.0f);
    world->updateTransforms();
    world->snapshotPreviousTransforms();

    EditorViewportFrame f;
    f.world = world.get();
    f.camera = camera({0, 0, 5}, {0, 0, 0}, 12.0f, 50.0f);
    const std::vector<Uuid> selection{red.uuid()};
    f.selection = selection;

    ensureTarget(256, 256, VK_FORMAT_R8G8B8A8_UNORM);
    // Simulate the editor frame: it owns beginFrame/endFrame and submits the command list itself.
    for (int i = 0; i < 2; ++i) {
        device->beginFrame();
        rhi::CommandList& cmd = device->commandList(rhi::QueueType::Graphics, "editor.frame");
        evr.render(f, target, cmd);
        cmd.transition(target, rhi::Access::TransferRead); // editor: transition(Present)
        device->submit(cmd);
        device->endFrame();
    }
    device->waitIdle();
    Image img;
    img.width = img.height = 256;
    img.rgba = device->readTexture(target);
    EXPECT_GT(img.at(70, 128).r, img.at(70, 128).g + 40) << "red cube on the left";
    EXPECT_GT(img.at(186, 128).g, img.at(186, 128).r + 40) << "green sphere on the right";

    EXPECT_EQ(evr.pick(f, {256, 256}, {70, 128}), std::optional<Uuid>(red.uuid()));
    EXPECT_EQ(evr.pick(f, {256, 256}, {186, 128}), std::optional<Uuid>(green.uuid()));
    EXPECT_EQ(evr.pick(f, {256, 256}, {128, 10}), std::nullopt);
    // Pick in a larger viewport (coordinates scale with the viewport size).
    EXPECT_EQ(evr.pick(f, {512, 512}, {140, 256}), std::optional<Uuid>(red.uuid()));

    const std::vector<Uuid> hidden{green.uuid()};
    f.hidden = hidden;
    EXPECT_EQ(evr.pick(f, {256, 256}, {186, 128}), std::nullopt) << "hidden entities are not drawn";
    const std::vector<u8> px = evr.renderToImage(f, 64, 64);
    ASSERT_EQ(px.size(), 64u * 64u * 4u);
}

TEST_F(EditorViewportTest, RenderToImageHonoursShowFlags) {
    renderer.reset();
    EditorViewportRenderer evr(*device);
    sun({-0.3f, -0.8f, -0.5f}, 20000.0f);
    world->updateTransforms();
    world->snapshotPreviousTransforms();
    DebugDraw noLines;
    noLines.flush(0.0f);

    EditorViewportFrame f;
    f.world = world.get();
    f.camera = camera({0, 4, 6}, {0, 0, 0}, 12.0f, 50.0f);
    f.lines = &noLines;
    auto shot = [&](bool grid) {
        f.grid = grid;
        (void)evr.renderToImage(f, 96, 96); // settle temporal effects
        return evr.renderToImage(f, 96, 96);
    };
    auto differingPixels = [](const std::vector<u8>& a, const std::vector<u8>& b) {
        usize n = 0;
        for (usize i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4) {
            n += std::abs(int(a[i]) - int(b[i])) + std::abs(int(a[i + 1]) - int(b[i + 1])) +
                     std::abs(int(a[i + 2]) - int(b[i + 2])) >
                 24;
        }
        return n;
    };
    const std::vector<u8> off = shot(false);
    const std::vector<u8> on = shot(true);
    ASSERT_EQ(off.size(), 96u * 96u * 4u);
    EXPECT_GT(differingPixels(off, on), 50u) << "grid visible only when requested";
    const std::vector<u8> offAgain = shot(false);
    EXPECT_LT(differingPixels(off, offAgain), 10u) << "a previous frame's flags must not leak into offscreen frames";
}
