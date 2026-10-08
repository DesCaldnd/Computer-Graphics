#pragma once

namespace ox::editor {

// Registers the inspector extensions of the gameplay components (no-op without the gameplay module).
void registerGameplayInspectorExtensions();
// Showcase project template: physics bodies, a Lua script with properties, a spline follower, AI.
void registerGameplayTemplates();

} // namespace ox::editor
