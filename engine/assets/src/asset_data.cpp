#include <oxwald/assets/asset_data.hpp>

#include "internal.hpp"

#if OX_ASSETS_HAS_ANIMATION
#include <oxwald/animation/serialization.hpp>
#endif

namespace ox::assets {

namespace {
usize valueBytes(const serial::Value& v) {
    usize n = sizeof(serial::Value) + v.getString().size() + v.packedBytes().size();
    for (const auto& i : v.items()) n += valueBytes(i);
    for (const auto& [k, f] : v.fields()) n += k.size() + valueBytes(f);
    return n;
}
} // namespace

usize SceneAsset::memoryUsage() const { return valueBytes(document.root); }
usize PrefabAsset::memoryUsage() const { return valueBytes(document.root); }

std::vector<std::byte> serializeBlob(AssetType type, const nlohmann::ordered_json& info, std::span<const std::byte> data) {
    ByteWriter w;
    w.writeBytes("OXBL", 4);
    w.write(u32{1});
    w.write(static_cast<u16>(type));
    w.write(u16{0});
    w.writeString(info.dump());
    w.write(static_cast<u64>(data.size()));
    w.write(crc32(data));
    w.align(16);
    w.writeBytes(data.data(), data.size());
    return w.take();
}

Result<BlobAsset> deserializeBlob(std::span<const std::byte> data) {
    ByteReader r(data);
    char magic[4];
    r.readBytes(magic, 4);
    if (r.failed() || std::memcmp(magic, "OXBL", 4) != 0) return makeError("not an OXBL blob");
    r.read<u32>();
    BlobAsset blob;
    blob.type = AssetType(r.read<u16>());
    r.read<u16>();
    auto info = nlohmann::ordered_json::parse(r.readString(), nullptr, false);
    if (!info.is_discarded()) blob.info = std::move(info);
    const u64 size = r.read<u64>();
    const u32 crc = r.read<u32>();
    r.seek((r.position() + 15) & ~usize(15));
    if (r.failed() || size > r.remaining()) return makeError("truncated blob");
    blob.data.assign(data.begin() + r.position(), data.begin() + r.position() + size);
    if (crc32(blob.data) != crc) return makeError("blob CRC mismatch");
    return blob;
}

std::vector<Uuid> collectUuidRefs(const serial::Value& value) {
    std::vector<Uuid> out;
    auto visit = [&](auto&& self, const serial::Value& v) -> void {
        if (v.tag() == serial::Tag::Uuid) {
            const Uuid u = v.getUuid();
            if (u.isValid()) out.push_back(u);
        } else if (v.isPacked() && v.elemDesc() && v.elemDesc()->tag == serial::Tag::Uuid) {
            for (usize i = 0; i < v.size(); ++i) {
                const Uuid u = v.at(i).getUuid();
                if (u.isValid()) out.push_back(u);
            }
        }
        for (const auto& i : v.items()) self(self, i);
        for (const auto& [k, f] : v.fields()) self(self, f);
    };
    visit(visit, value);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

#if OX_ASSETS_HAS_ANIMATION
usize SkeletonAsset::memoryUsage() const { return sizeof(*this) + skeleton.jointCount() * 160; }
usize AnimationClipAsset::memoryUsage() const {
    anim::ByteWriter w;
    anim::serialize(w, clip);
    return sizeof(*this) + w.data().size();
}

std::vector<std::byte> serializeSkeletonAsset(const anim::Skeleton& skeleton) {
    anim::ByteWriter w;
    anim::serialize(w, skeleton);
    const auto& d = w.data();
    return serializeBlob(AssetType::Skeleton, {}, std::span(reinterpret_cast<const std::byte*>(d.data()), d.size()));
}

Result<SkeletonAsset> deserializeSkeletonAsset(std::span<const std::byte> data) {
    auto blob = deserializeBlob(data);
    if (!blob) return blob.error();
    anim::ByteReader r(std::span<const u8>(reinterpret_cast<const u8*>(blob->data.data()), blob->data.size()));
    SkeletonAsset out;
    if (!anim::deserialize(r, out.skeleton)) return makeError("corrupt skeleton");
    return out;
}

std::vector<std::byte> serializeClipAsset(const anim::AnimationClip& clip, const Uuid& skeleton) {
    anim::ByteWriter w;
    anim::serialize(w, clip);
    const auto& d = w.data();
    nlohmann::ordered_json info;
    info["skeleton"] = skeleton.toString();
    return serializeBlob(AssetType::AnimationClip, info,
                         std::span(reinterpret_cast<const std::byte*>(d.data()), d.size()));
}

Result<AnimationClipAsset> deserializeClipAsset(std::span<const std::byte> data) {
    auto blob = deserializeBlob(data);
    if (!blob) return blob.error();
    anim::ByteReader r(std::span<const u8>(reinterpret_cast<const u8*>(blob->data.data()), blob->data.size()));
    AnimationClipAsset out;
    if (!anim::deserialize(r, out.clip)) return makeError("corrupt animation clip");
    out.skeleton = Uuid::parse(blob->info.value("skeleton", std::string{})).value_or(Uuid{});
    return out;
}
#endif

} // namespace ox::assets
