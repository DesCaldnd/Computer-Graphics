#include "test_helpers.hpp"

#include <ktx.h>

using namespace oxtest;

TEST(TextureMips, CountAndSizes) {
    Image img(100, 40, glm::vec4(0.5f));
    auto chain = generateMipChain(img, MipSettings{});
    ASSERT_EQ(chain.size(), mipLevelCount(100, 40));
    EXPECT_EQ(chain.size(), 7u); // 100,50,25,12,6,3,1
    EXPECT_EQ(chain[1].width, 50u);
    EXPECT_EQ(chain[1].height, 20u);
    EXPECT_EQ(chain[6].width, 1u);
    EXPECT_EQ(chain[6].height, 1u);
    for (const auto& l : chain) EXPECT_NEAR(l.pixels[0].r, 0.5f, 1e-4f); // constant stays constant (Kaiser)
}

TEST(TextureMips, SrgbCorrectDownsampling) {
    // Black/white checker stored in sRGB: the correct average is linear 0.5 -> sRGB ~0.7354, not 0.5.
    Image img(8, 8);
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) img.at(x, y) = glm::vec4(glm::vec3(f32((x + y) & 1)), 1.0f);
    }
    MipSettings s;
    s.filter = MipFilter::Box;
    s.srgb = true;
    auto chain = generateMipChain(img, s);
    EXPECT_NEAR(chain[1].at(0, 0).r, linearToSrgb(0.5f), 1e-3f);
    EXPECT_NEAR(chain[1].at(0, 0).r, 0.7354f, 1e-3f);
    s.srgb = false;
    auto naive = generateMipChain(img, s);
    EXPECT_NEAR(naive[1].at(0, 0).r, 0.5f, 1e-4f);
}

TEST(TextureMips, NormalMapRenormalised) {
    Image img = normalMapImage(64, 64);
    MipSettings s;
    s.normalMap = true;
    auto chain = generateMipChain(img, s);
    for (usize l = 1; l < chain.size(); ++l) {
        for (const auto& p : chain[l].pixels) EXPECT_NEAR(glm::length(glm::vec3(p) * 2.0f - 1.0f), 1.0f, 1e-3f);
    }
}

TEST(TextureMips, AlphaCoveragePreserved) {
    // Foliage-like alpha: filtering pulls values towards the mean, so plain mips lose coverage at a high cutoff.
    Image img(64, 64, glm::vec4(0.2f, 0.6f, 0.1f, 0.0f));
    for (u32 y = 0; y < 64; ++y) {
        for (u32 x = 0; x < 64; ++x) {
            img.at(x, y).a = glm::clamp(0.5f + 0.5f * std::sin(f32(x) * 0.7f) * std::sin(f32(y) * 0.9f), 0.0f, 1.0f);
        }
    }
    const f32 cutoff = 0.7f;
    const f32 target = alphaCoverage(img, cutoff);
    MipSettings s;
    s.preserveAlphaCoverage = true;
    s.alphaCutoff = cutoff;
    auto chain = generateMipChain(img, s);
    MipSettings plain = s;
    plain.preserveAlphaCoverage = false;
    auto naive = generateMipChain(img, plain);
    for (usize l = 1; l < 3; ++l) {
        EXPECT_NEAR(alphaCoverage(chain[l], cutoff), target, 0.05f) << "level " << l;
    }
    EXPECT_LT(alphaCoverage(naive[2], cutoff), target * 0.6f);
}

TEST(TextureCompression, BC7RoundTripPsnr) {
    Image img = gradientImage(64, 64);
    auto bc = compressBC7(img, 1);
    ASSERT_TRUE(bc) << bc.error().message;
    EXPECT_EQ(bc->size(), 16u * 16 * 16);
    Image decoded = decodeBC7(*bc, 64, 64);
    const f64 psnr = computePsnr(img, decoded, 15);
    EXPECT_GT(psnr, 38.0) << "BC7 PSNR " << psnr;
}

TEST(TextureCompression, BC5RoundTripPsnr) {
    Image img = normalMapImage(64, 64);
    auto bc = compressBC5(img);
    EXPECT_EQ(bc.size(), 16u * 16 * 16);
    Image decoded = decodeBC5(bc, 64, 64);
    EXPECT_GT(computePsnr(img, decoded, 3), 40.0);
    // Reconstructed Z matches the source normal.
    f64 maxErr = 0;
    for (usize i = 0; i < img.pixels.size(); ++i) maxErr = std::max<f64>(maxErr, std::abs(img.pixels[i].b - decoded.pixels[i].b));
    EXPECT_LT(maxErr, 0.03);
}

TEST(TextureCompression, BuildTextureFormats) {
    Image img = gradientImage(32, 32);
    TextureImportSettings s;
    auto color = importTexture(img, s);
    ASSERT_TRUE(color) << color.error().message;
    EXPECT_EQ(color->format, TextureFormat::BC7Srgb);
    EXPECT_EQ(color->mipCount, 6u);
    EXPECT_EQ(color->mips[5].data.size(), 16u); // 1x1 level = one block
    s.type = TextureType::Normal;
    auto normal = importTexture(normalMapImage(32, 32), s);
    ASSERT_TRUE(normal);
    EXPECT_EQ(normal->format, TextureFormat::BC5Unorm);
    EXPECT_TRUE(normal->normalMap);
    s.type = TextureType::HDR;
    Image hdr(16, 8, glm::vec4(4.0f, 2.0f, 1.0f, 1.0f));
    hdr.hdr = true;
    auto h = importTexture(hdr, s);
    ASSERT_TRUE(h);
    EXPECT_EQ(h->format, TextureFormat::RGBA16Float);
    Image back = decodeTextureLevel(h->format, h->mips[0].data, 16, 8);
    EXPECT_NEAR(back.pixels[0].r, 4.0f, 1e-3f);
    s = {};
    s.compression = TextureCompression::Uncompressed;
    s.maxSize = 16;
    auto small = importTexture(img, s);
    ASSERT_TRUE(small);
    EXPECT_EQ(small->width, 16u);
    EXPECT_EQ(small->format, TextureFormat::RGBA8Srgb);
    auto rgba = decompressToRGBA8(*color);
    EXPECT_EQ(rgba.format, TextureFormat::RGBA8Srgb);
    EXPECT_EQ(rgba.mips[0].data.size(), 32u * 32 * 4);
}

TEST(TextureFile, RoundTripAndMipRange) {
    TempDir dir;
    TextureImportSettings s;
    s.compression = TextureCompression::Uncompressed;
    auto tex = importTexture(gradientImage(64, 32), s);
    ASSERT_TRUE(tex);
    auto bytes = serializeTexture(*tex);
    auto back = deserializeTexture(bytes);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->mipCount, tex->mipCount);
    for (u32 i = 0; i < tex->mipCount; ++i) EXPECT_EQ(back->mips[i].data, tex->mips[i].data);
    auto partial = deserializeTexture(bytes, 2);
    ASSERT_TRUE(partial);
    EXPECT_EQ(partial->firstMip, 2u);
    EXPECT_EQ(partial->mips[0].width, 16u);

    const fs::path file = dir / "t.oxtex";
    writeBytes(file, bytes);
    auto info = readTextureInfo(std::span(bytes).first(1024));
    ASSERT_TRUE(info);
    // Smallest mips are stored first, right after the header.
    EXPECT_LT(info->levels.back().offset, info->levels.front().offset);
    auto tail = readMipRange(file, 3, 100);
    ASSERT_TRUE(tail) << tail.error().message;
    ASSERT_EQ(tail->size(), tex->mipCount - 3);
    EXPECT_EQ((*tail)[0].data, tex->mips[3].data);
    bytes[bytes.size() - 1] ^= std::byte{1};
    EXPECT_FALSE(deserializeTexture(bytes));
}

TEST(TextureCubemap, EquirectToCube) {
    // Equirect where each direction region has a distinct colour: +Y (top rows) red, -Y (bottom) green,
    // horizon centre (-Z) blue, horizon edges (+Z) white.
    Image eq(256, 128);
    for (u32 y = 0; y < 128; ++y) {
        for (u32 x = 0; x < 256; ++x) {
            const f32 v = (f32(y) + 0.5f) / 128.0f, u = (f32(x) + 0.5f) / 256.0f;
            glm::vec4 c(0, 0, 0, 1);
            if (v < 0.25f) c = {1, 0, 0, 1};
            else if (v > 0.75f) c = {0, 1, 0, 1};
            else if (std::abs(u - 0.5f) < 0.125f) c = {0, 0, 1, 1};
            else if (u < 0.125f || u > 0.875f) c = {1, 1, 1, 1};
            eq.at(x, y) = c;
        }
    }
    auto faces = equirectToCubemap(eq, 32);
    auto centre = [&](u32 f) { return faces[f].at(16, 16); };
    EXPECT_GT(centre(2).r, 0.9f); // +Y
    EXPECT_GT(centre(3).g, 0.9f); // -Y
    EXPECT_GT(centre(5).b, 0.9f); // -Z
    EXPECT_GT(centre(4).r, 0.9f); // +Z white
    EXPECT_GT(centre(4).b, 0.9f);
    EXPECT_NEAR(glm::length(cubeFaceDirection(0, 0.5f, 0.5f) - glm::vec3(1, 0, 0)), 0.0f, 1e-6f);
    TextureImportSettings s;
    s.type = TextureType::HDR;
    s.cubemap = CubemapMode::FromEquirect;
    s.cubeFaceSize = 16;
    auto cube = importTexture(eq, s);
    ASSERT_TRUE(cube);
    EXPECT_TRUE(cube->cube);
    EXPECT_EQ(cube->layers, 6u);
    EXPECT_EQ(cube->mips[0].data.size(), 6u * 16 * 16 * 8);
}

TEST(TextureImport, Ktx2BasisTranscodedToBC7) {
    // Build a UASTC KTX2 with libktx, then import it.
    Image img = gradientImage(32, 32);
    auto rgba = encodeRGBA8(img);
    ktxTextureCreateInfo ci{};
    ci.vkFormat = 43; // R8G8B8A8_SRGB
    ci.baseWidth = 32;
    ci.baseHeight = 32;
    ci.baseDepth = 1;
    ci.numDimensions = 2;
    ci.numLevels = 1;
    ci.numLayers = 1;
    ci.numFaces = 1;
    ktxTexture2* tex = nullptr;
    ASSERT_EQ(ktxTexture2_Create(&ci, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &tex), KTX_SUCCESS);
    ktxTexture_SetImageFromMemory(ktxTexture(tex), 0, 0, 0, reinterpret_cast<const ktx_uint8_t*>(rgba.data()), rgba.size());
    ktxBasisParams p{};
    p.structSize = sizeof(p);
    p.uastc = KTX_TRUE;
    p.threadCount = 1;
    p.uastcFlags = KTX_PACK_UASTC_LEVEL_DEFAULT;
    ASSERT_EQ(ktxTexture2_CompressBasisEx(tex, &p), KTX_SUCCESS);
    ktx_uint8_t* out = nullptr;
    ktx_size_t outSize = 0;
    ASSERT_EQ(ktxTexture_WriteToMemory(ktxTexture(tex), &out, &outSize), KTX_SUCCESS);
    std::vector<std::byte> file(reinterpret_cast<std::byte*>(out), reinterpret_cast<std::byte*>(out) + outSize);
    free(out);
    ktxTexture_Destroy(ktxTexture(tex));

    auto imported = importKtx2(file, TextureImportSettings{});
    ASSERT_TRUE(imported) << imported.error().message;
    EXPECT_EQ(imported->format, TextureFormat::BC7Srgb);
    EXPECT_EQ(imported->width, 32u);
    EXPECT_GT(computePsnr(img, decodeBC7(imported->mips[0].data, 32, 32), 7), 35.0);
}

TEST(TextureImport, HdrFileLoad) {
    TempDir dir;
    // Minimal Radiance .hdr (uncompressed scanlines), 4x2.
    std::string hdr = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2 +X 4\n";
    for (int i = 0; i < 8; ++i) hdr += std::string("\x80\x80\x80\x82", 4); // (0.5*4)=2.0
    writeText(dir / "sky.hdr", hdr);
    auto img = loadImage(dir / "sky.hdr");
    ASSERT_TRUE(img) << img.error().message;
    EXPECT_TRUE(img->hdr);
    EXPECT_NEAR(img->pixels[0].r, 2.0f, 0.05f);
}

TEST(TextureImport, LegacyPngs) {
    TextureImportSettings s;
    s.maxSize = 64;
    auto tex = importTextureFile(legacyDir() / "fiit.jpg", s);
    ASSERT_TRUE(tex) << tex.error().message;
    EXPECT_LE(std::max(tex->width, tex->height), 64u);
    EXPECT_EQ(tex->format, TextureFormat::BC7Srgb);
}
