# Handoff: intermittent heap corruption in `editor.SettingsTests` (2026-10-14)

## Symptom
`ox_editor_tests SettingsTests` in `build/dev` (RelWithDebInfo) crashed intermittently in
`screenshotGenerationRuns` with 0xc0000374 (STATUS_HEAP_CORRUPTION) or 0xc0000005 at random places (Lua usertype
registration, ntdll heap). The crash rate was about 2 in 8 plain runs, and 4 in 4 runs under lldb, where ntdll's heap
check breaks at the corruption.

## Root cause: stale object files, not a code bug
- Under lldb, the corruption is detected in `RtlFreeHeap` when `ViewportPanel`'s scalar deleting destructor frees the
  panel. The block size is 0x2a8 (`rdi = 0x2a8`).
- `build/dev` `main_window.cpp.obj` (built 20:11) runs `new ViewportPanel` with **0x2a8** bytes
  (`movl $0x2a8, %ecx` before `operator new`, main_window.cpp:58). That size comes from the old
  `viewport_panel.hpp`, before the FramePacer members (`m_paceClock`, `m_pacer`, `m_gpuMs`, `m_ticks`,
  `m_continuousRedraw`) were added at 20:47.
- `viewport_panel.cpp.obj` (21:08) was compiled with the new header. Its constructor writes members at offsets
  0x2a8, 0x2b0 and 0x2b8, which are **past the end of the allocation**. That is heap corruption. It shows up only
  sometimes because LFH randomizes what lies after the block.
- `screenshots.cpp.obj`, `vulkan_viewport.cpp.obj`, `stats_panel.cpp.obj`, `app/main.cpp.obj` and
  `test_gizmo.cpp.obj` are also older than the header that changed.

### Why ninja did not rebuild them
`ninja -C build/dev -t deps` reports `#deps 0` for **623** objects: ninja has no header dependencies for them. The
cause is the localized MSVC (only the 1049/Russian resources are installed). `/showIncludes` prints
"Примечание: включение файла:", and the console code page decides how that string is encoded:
- Git Bash → `cmd /c vs.bat` (how the configure ran): the Cyrillic comes out as `?`. CMake stored
  `msvc_deps_prefix = ??????????: ????????? ?????:`.
- PowerShell (console code page 65001): the same prefix comes out as UTF-8 Cyrillic (`d0 9f d1 80 ...`). It does not
  match the stored prefix, so ninja records zero deps. Header edits then never trigger recompiles.
`build/debug`, configured and built only from Git Bash, has deps for 609 objects (3 without, all autogen/rcc).
`VSLANG=1033` does not help, because no English resources are installed.

## What to do
1. `build/dev`: force a full recompile once, for example `cmake --build E:/Development/OxwaldEngine/build/dev --clean-first`,
   or delete its `*.obj` files. Then check with `ninja -C <dir> -t deps | grep -c "#deps 0"`, which should be about 3.
   Every build dir that was built from PowerShell is suspect in the same way.
2. Always build a dir from the same kind of shell it was configured from, or the deps are lost silently.
3. Permanent fix: install the English language pack for VS 2022 (VS Installer → Language packs) and set
   `VSLANG=1033` in the environment (or in the CMakePresets `environment`). Then reconfigure. The prefix becomes the
   ASCII "Note: including file:", which works from every shell.

## Verification
- `build/debug` (consistent deps, Debug CRT heap): `SettingsTests screenshotGenerationRuns` 6/6 passed, full
  `SettingsTests` 8/8 passed, and 10/10 passed in the final loop after a rebuild against the current tree (exit code 0 every run, which means no failed test functions).
- `build/dev` (stale objects): crashed in 2/8 plain runs and 4/4 runs under lldb.
- The FramePacer / single-shot timer code is not involved. The software viewport in offscreen tests does not touch
  the GPU, and the Vulkan device in the log is only the caps probe.
