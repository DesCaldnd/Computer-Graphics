#pragma once

// Reflection + ComponentRegistry registration of every render-owned ECS component (reflection probes, particle
// emitters, post-process volumes, ...) and their extract hooks. Idempotent; call it from module init (runtime,
// editor, tools) before loading scenes. Renderer::create() also registers the components of the features it adds.

namespace ox::render {

void registerRenderTypes();

} // namespace ox::render
