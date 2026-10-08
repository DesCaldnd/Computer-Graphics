#pragma once

// All engine code talks to Vulkan through volk: global function pointers loaded at runtime,
// no link-time dependency on the loader. Never include <vulkan/vulkan.h> with prototypes.
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <volk.h>
