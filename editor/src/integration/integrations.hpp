#pragma once

namespace ox::editor {

class EditorContext;

// Reflection/component registration of every linked engine module (gameplay, world, assets, runtime). Idempotent.
void registerModuleTypes();

// Wires the linked engine modules into the editor services:
//   gameplay  IPlayRuntime (Simulate = physics/animation only; systems added for the fallback session)
//   assets    RegistryAssetBackend on the engine's AssetRegistry (swapped whenever the engine restarts)
//   render    GPU viewport renderer factory, mesh/material thumbnails, render::autoDetectQuality benchmark,
//             AssetManager -> GpuResourceCache provider + hot reload
void installIntegrations(EditorContext& ctx);
void uninstallIntegrations(EditorContext& ctx);

} // namespace ox::editor
