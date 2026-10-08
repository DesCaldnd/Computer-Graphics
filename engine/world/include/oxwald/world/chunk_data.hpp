#pragma once

#include <oxwald/world/heightfield.hpp>
#include <oxwald/world/splat_map.hpp>
#include <oxwald/world/streaming.hpp>
#include <oxwald/world/vegetation.hpp>

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ox::world {

// Everything a streamed world chunk carries. Derives from ChunkPayload so a ChunkStreamer load
// callback can return it directly.
struct ChunkData : ChunkPayload {
    ChunkCoord coord{};
    std::optional<Heightfield> heightfield; // tile incl. the shared edge (e.g. 129x129 for 128 quads)
    std::optional<SplatMap> splat;
    std::vector<VegetationInstance> vegetation;
    std::vector<u8> userData; // game-specific blob (entities, edits, ...)
};

// Binary chunk format (little-endian):
//   "OXCH" u32 version, i32 x, i32 z, u32 sectionCount,
//   sections: { u32 fourcc, u32 byteSize, bytes[byteSize], u32 crc32(bytes) }
//   HFLD heightfield (desc + raw samples), HOLE hole mask, SPLT splat map, VEGI instances, USER blob.
// Unknown sections are skipped (forward compatible); a CRC mismatch fails the load.
inline constexpr u32 kChunkFormatVersion = 1;

std::vector<u8> serializeChunk(const ChunkData& chunk);
bool deserializeChunk(std::span<const u8> bytes, ChunkData& out, std::string* error = nullptr);

} // namespace ox::world
