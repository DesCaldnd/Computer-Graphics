#pragma once

// Profiling macros. With OX_TRACY (set by the core target when OX_ENABLE_TRACY=ON and the Tracy
// package is found) they map to Tracy; otherwise they compile to nothing. Arguments are referenced
// only in unevaluated contexts in the no-op variant, so they produce no unused-variable warnings
// and have no side effects.
//
//   OX_PROFILE_ZONE();                       // zone named after the function
//   OX_PROFILE_ZONE_N("Cull");               // name must be a string literal
//   OX_PROFILE_ZONE_C("Upload", 0xff8800);   // with RGB color
//   OX_PROFILE_FRAME();                      // end of the main frame
//   OX_PROFILE_FRAME_N("Render");            // secondary frame set (string literal)
//   OX_PROFILE_ALLOC(ptr, size); OX_PROFILE_FREE(ptr);
//   OX_PROFILE_THREAD_NAME("Worker 3");      // const char*, must outlive the thread
//   OX_PROFILE_PLOT("Draw calls", value);    // name must be a string literal
//   OX_PROFILE_MESSAGE(text, size);

#if defined(OX_TRACY) && OX_TRACY

#include <tracy/Tracy.hpp>

#define OX_PROFILE_ENABLED 1
#define OX_PROFILE_ZONE() ZoneScoped
#define OX_PROFILE_ZONE_N(name) ZoneScopedN(name)
#define OX_PROFILE_ZONE_C(name, color) ZoneScopedNC(name, color)
#define OX_PROFILE_FRAME() FrameMark
#define OX_PROFILE_FRAME_N(name) FrameMarkNamed(name)
#define OX_PROFILE_ALLOC(ptr, size) TracyAlloc(ptr, size)
#define OX_PROFILE_FREE(ptr) TracyFree(ptr)
#define OX_PROFILE_THREAD_NAME(name) ::tracy::SetThreadName(name)
#define OX_PROFILE_PLOT(name, value) TracyPlot(name, value)
#define OX_PROFILE_MESSAGE(text, size) TracyMessage(text, size)

#else

#define OX_PROFILE_ENABLED 0
#define OX_PROFILE_ZONE() static_cast<void>(0)
#define OX_PROFILE_ZONE_N(name) static_cast<void>(sizeof(name))
#define OX_PROFILE_ZONE_C(name, color) static_cast<void>(sizeof(name) + sizeof(color))
#define OX_PROFILE_FRAME() static_cast<void>(0)
#define OX_PROFILE_FRAME_N(name) static_cast<void>(sizeof(name))
#define OX_PROFILE_ALLOC(ptr, size) static_cast<void>(sizeof(ptr) + sizeof(size))
#define OX_PROFILE_FREE(ptr) static_cast<void>(sizeof(ptr))
#define OX_PROFILE_THREAD_NAME(name) static_cast<void>(sizeof(name))
#define OX_PROFILE_PLOT(name, value) static_cast<void>(sizeof(name) + sizeof(value))
#define OX_PROFILE_MESSAGE(text, size) static_cast<void>(sizeof(text) + sizeof(size))

#endif
