/* Single translation unit holding the miniaudio + stb_vorbis implementations. */
#if defined(_WIN32)
/* The one translation unit that wants the full Win32 audio API (WASAPI, DirectSound, WinMM); the project-wide
   WIN32_LEAN_AND_MEAN strips mmsystem.h / ole2.h from <windows.h>. */
#undef WIN32_LEAN_AND_MEAN
#endif
#if defined(_MSC_VER)
/* Third-party implementations: /external:W0 covers the front-end warnings only; this one is reported by the code
   generator (stb_vorbis.c, "potentially uninitialized local variable"). */
#pragma warning(disable : 4701)
#endif

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#undef STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
