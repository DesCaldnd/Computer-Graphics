#include <oxwald/animation/serialization.hpp>

#include <oxwald/core/log.hpp>

namespace ox::anim {

namespace {

constexpr u32 kSkeletonMagic = 0x4C4B534F; // "OSKL"
constexpr u32 kClipMagic = 0x50494C43;     // "CLIP"
constexpr u32 kCompactMagic = 0x504D4343;  // "CCMP"
constexpr u32 kMeshMagic = 0x4853454D;     // "MESH"
constexpr u32 kVersion = 1;

bool readHeader(ByteReader& r, u32 magic, const char* what) {
    const u32 m = r.read<u32>();
    const u32 v = r.read<u32>();
    if (r.failed() || m != magic) {
        OX_LOG_ERROR("animation", "{}: bad magic", what);
        return false;
    }
    if (v > kVersion) {
        OX_LOG_ERROR("animation", "{}: unsupported version {}", what, v);
        return false;
    }
    return true;
}

template <class T>
void writeTrack(ByteWriter& w, const Track<T>& t) {
    w.write(static_cast<u8>(t.interpolation));
    w.writeVector(t.times);
    w.writeVector(t.values);
    w.writeVector(t.inTangents);
    w.writeVector(t.outTangents);
}

template <class T>
void readTrack(ByteReader& r, Track<T>& t) {
    t.interpolation = static_cast<Interpolation>(r.read<u8>());
    t.times = r.readVector<f32>();
    t.values = r.readVector<T>();
    t.inTangents = r.readVector<T>();
    t.outTangents = r.readVector<T>();
}

void writeEvents(ByteWriter& w, const std::vector<AnimEvent>& events) {
    w.write(static_cast<u32>(events.size()));
    for (const auto& e : events) {
        w.write(e.time);
        w.write(e.payload);
        w.writeString(e.name);
    }
}

void readEvents(ByteReader& r, std::vector<AnimEvent>& events) {
    const u32 n = r.read<u32>();
    events.clear();
    for (u32 i = 0; i < n && !r.failed(); ++i) {
        AnimEvent e;
        e.time = r.read<f32>();
        e.payload = r.read<f32>();
        e.name = r.readString();
        events.push_back(std::move(e));
    }
}

void writeRootMotion(ByteWriter& w, const RootMotionTrack& rm) {
    w.writeVector(rm.times);
    w.writeVector(rm.positions);
    w.writeVector(rm.rotations);
}

void readRootMotion(ByteReader& r, RootMotionTrack& rm) {
    rm.times = r.readVector<f32>();
    rm.positions = r.readVector<glm::vec3>();
    rm.rotations = r.readVector<glm::quat>();
}

} // namespace

void ByteWriter::writeBytes(const void* data, usize size) {
    const auto* p = static_cast<const u8*>(data);
    m_data.insert(m_data.end(), p, p + size);
}

void ByteWriter::writeString(const std::string& s) {
    write(static_cast<u32>(s.size()));
    writeBytes(s.data(), s.size());
}

bool ByteReader::readBytes(void* out, usize size) {
    if (m_failed || size > remaining()) {
        m_failed = true;
        std::memset(out, 0, size);
        return false;
    }
    std::memcpy(out, m_data.data() + m_pos, size);
    m_pos += size;
    return true;
}

std::string ByteReader::readString() {
    const u32 n = read<u32>();
    if (m_failed || n > remaining()) {
        m_failed = true;
        return {};
    }
    std::string s(reinterpret_cast<const char*>(m_data.data() + m_pos), n);
    m_pos += n;
    return s;
}

void serialize(ByteWriter& w, const Skeleton& skeleton) {
    w.write(kSkeletonMagic);
    w.write(kVersion);
    w.write(static_cast<u32>(skeleton.jointCount()));
    for (usize i = 0; i < skeleton.jointCount(); ++i) {
        w.writeString(skeleton.names()[i]);
        w.write(skeleton.parents()[i]);
        w.write(skeleton.bindPose()[i]);
        w.write(skeleton.inverseBindMatrices()[i]);
    }
}

bool deserialize(ByteReader& r, Skeleton& skeleton) {
    if (!readHeader(r, kSkeletonMagic, "skeleton")) {
        return false;
    }
    skeleton = Skeleton{};
    const u32 n = r.read<u32>();
    for (u32 i = 0; i < n && !r.failed(); ++i) {
        std::string name = r.readString();
        const i32 parent = r.read<i32>();
        const Transform bind = r.read<Transform>();
        const glm::mat4 inverseBind = r.read<glm::mat4>();
        if (r.failed() || parent >= static_cast<i32>(i) || parent < kNoJoint) {
            OX_LOG_ERROR("animation", "skeleton: corrupt joint {}", i);
            return false;
        }
        const i32 j = skeleton.addJoint(std::move(name), parent, bind);
        skeleton.setInverseBind(j, inverseBind);
    }
    return !r.failed();
}

void serialize(ByteWriter& w, const AnimationClip& clip) {
    w.write(kClipMagic);
    w.write(kVersion);
    w.writeString(clip.name);
    w.write(clip.duration);
    w.write(static_cast<u8>(clip.rotationBlend));
    w.write(static_cast<u8>(clip.additive));
    w.write(static_cast<u32>(clip.tracks.size()));
    for (const auto& jt : clip.tracks) {
        writeTrack(w, jt.translation);
        writeTrack(w, jt.rotation);
        writeTrack(w, jt.scale);
    }
    writeEvents(w, clip.events);
    writeRootMotion(w, clip.rootMotion);
}

bool deserialize(ByteReader& r, AnimationClip& clip) {
    if (!readHeader(r, kClipMagic, "clip")) {
        return false;
    }
    clip = AnimationClip{};
    clip.name = r.readString();
    clip.duration = r.read<f32>();
    clip.rotationBlend = static_cast<RotationBlend>(r.read<u8>());
    clip.additive = r.read<u8>() != 0;
    const u32 n = r.read<u32>();
    if (r.failed() || n > r.remaining()) {
        return false;
    }
    clip.tracks.resize(n);
    for (auto& jt : clip.tracks) {
        readTrack(r, jt.translation);
        readTrack(r, jt.rotation);
        readTrack(r, jt.scale);
    }
    readEvents(r, clip.events);
    readRootMotion(r, clip.rootMotion);
    return !r.failed();
}

void serialize(ByteWriter& w, const CompactClip& clip) {
    w.write(kCompactMagic);
    w.write(kVersion);
    w.writeString(clip.name);
    w.write(clip.duration);
    w.write(static_cast<u8>(clip.rotationBlend));
    w.write(static_cast<u8>(clip.additive));
    w.write(static_cast<u8>(clip.quantized));
    w.write(clip.jointCount);
    w.writeVector(clip.rangeOffset);
    w.writeVector(clip.rangeCount);
    w.writeVector(clip.rangeStep);
    w.writeVector(clip.translationTimes);
    w.writeVector(clip.translations);
    w.writeVector(clip.rotationTimes);
    w.writeVector(clip.rotations);
    w.writeVector(clip.packedRotations);
    w.writeVector(clip.scaleTimes);
    w.writeVector(clip.scales);
    writeEvents(w, clip.events);
    writeRootMotion(w, clip.rootMotion);
}

bool deserialize(ByteReader& r, CompactClip& clip) {
    if (!readHeader(r, kCompactMagic, "compact clip")) {
        return false;
    }
    clip = CompactClip{};
    clip.name = r.readString();
    clip.duration = r.read<f32>();
    clip.rotationBlend = static_cast<RotationBlend>(r.read<u8>());
    clip.additive = r.read<u8>() != 0;
    clip.quantized = r.read<u8>() != 0;
    clip.jointCount = r.read<u32>();
    clip.rangeOffset = r.readVector<u32>();
    clip.rangeCount = r.readVector<u32>();
    clip.rangeStep = r.readVector<u8>();
    clip.translationTimes = r.readVector<f32>();
    clip.translations = r.readVector<glm::vec3>();
    clip.rotationTimes = r.readVector<f32>();
    clip.rotations = r.readVector<glm::quat>();
    clip.packedRotations = r.readVector<PackedQuat>();
    clip.scaleTimes = r.readVector<f32>();
    clip.scales = r.readVector<glm::vec3>();
    readEvents(r, clip.events);
    readRootMotion(r, clip.rootMotion);
    if (r.failed()) {
        return false;
    }
    // Validate ranges so a corrupt file can't index out of bounds at sample time.
    const usize ranges = static_cast<usize>(clip.jointCount) * 3;
    if (clip.rangeOffset.size() != ranges || clip.rangeCount.size() != ranges || clip.rangeStep.size() != ranges) {
        return false;
    }
    const usize rotCount = clip.quantized ? clip.packedRotations.size() : clip.rotations.size();
    for (usize i = 0; i < ranges; ++i) {
        const usize end = static_cast<usize>(clip.rangeOffset[i]) + clip.rangeCount[i];
        const usize limit = (i % 3 == 0) ? std::min(clip.translationTimes.size(), clip.translations.size())
                          : (i % 3 == 1) ? std::min(clip.rotationTimes.size(), rotCount)
                                         : std::min(clip.scaleTimes.size(), clip.scales.size());
        if (end > limit) {
            return false;
        }
    }
    return true;
}

void serialize(ByteWriter& w, const SkinnedMeshData& mesh) {
    w.write(kMeshMagic);
    w.write(kVersion);
    w.writeString(mesh.name);
    w.write(mesh.influencesPerVertex);
    w.write(mesh.materialIndex);
    w.writeVector(mesh.positions);
    w.writeVector(mesh.normals);
    w.writeVector(mesh.tangents);
    w.writeVector(mesh.uvs);
    w.writeVector(mesh.indices);
    w.writeVector(mesh.joints);
    w.writeVector(mesh.weights);
}

bool deserialize(ByteReader& r, SkinnedMeshData& mesh) {
    if (!readHeader(r, kMeshMagic, "skinned mesh")) {
        return false;
    }
    mesh = SkinnedMeshData{};
    mesh.name = r.readString();
    mesh.influencesPerVertex = r.read<u32>();
    mesh.materialIndex = r.read<i32>();
    mesh.positions = r.readVector<glm::vec3>();
    mesh.normals = r.readVector<glm::vec3>();
    mesh.tangents = r.readVector<glm::vec4>();
    mesh.uvs = r.readVector<glm::vec2>();
    mesh.indices = r.readVector<u32>();
    mesh.joints = r.readVector<u16>();
    mesh.weights = r.readVector<f32>();
    return !r.failed() && mesh.joints.size() == mesh.positions.size() * mesh.influencesPerVertex &&
           mesh.weights.size() == mesh.joints.size();
}

} // namespace ox::anim
