#pragma once

// Destruction → render bridge (gameplay DestructibleComponent / DestructionChunkComponent). An extract hook collects
// the prepared fractures of the world (shared chunk mesh data) into a snapshot extension; the DestructionMeshes
// feature converts each chunk to assets::MeshData once and registers it in the GpuResourceCache under the runtime
// mesh id the chunk entities' MeshRenderers use. Compiles to a no-op when render does not link gameplay.

namespace ox::render {

class FeatureRegistry;

void registerDestructionFeatures(FeatureRegistry& features);

} // namespace ox::render
