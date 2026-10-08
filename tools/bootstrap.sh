#!/usr/bin/env bash
# Installs all OxwaldEngine dependencies through vcpkg (manifest mode) into ./vcpkg_installed.
#   tools/bootstrap.sh            — everything (engine + Qt editor)
#   tools/bootstrap.sh --no-editor — skip Qt (engine, player, tests only)
# Requires VCPKG_ROOT. Triplets: arm64-osx (overlay, AppleClang) on macOS, x64-windows on Windows, vcpkg's default
# elsewhere. On macOS: Xcode command line tools, plus `brew install autoconf autoconf-archive
# automake libtool pkg-config` (host build tools for some ports, e.g. libb2 used by Qt).
set -euo pipefail
cd "$(dirname "$0")/.."
: "${VCPKG_ROOT:?set VCPKG_ROOT to your vcpkg checkout}"

args=(install --x-install-root=vcpkg_installed)
[[ "${1:-}" == "--no-editor" ]] && args+=(--x-no-default-features)

if [[ "$(uname)" == "Darwin" ]]; then
    args+=(--triplet arm64-osx)
    # Our overlay triplet builds with AppleClang. Make sure a Homebrew LLVM in CC/CXX/LDFLAGS
    # doesn't leak into autotools ports, and expose GNU libtoolize without shadowing Apple's libtool.
    hosttools="$PWD/build/hosttools"
    mkdir -p "$hosttools"
    if ! command -v libtoolize >/dev/null && command -v glibtoolize >/dev/null; then
        ln -sf "$(command -v glibtoolize)" "$hosttools/libtoolize"
    fi
    exec env -u CC -u CXX -u CPPFLAGS -u LDFLAGS -u CFLAGS -u CXXFLAGS \
        PATH="$hosttools:/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin" \
        "$VCPKG_ROOT/vcpkg" "${args[@]}"
fi
# Windows (Git Bash / MSYS2): same triplet the CMake toolchain picks for an x64 MSVC build; older vcpkg clients
# default to x86-windows on the command line. Run from a shell where MSVC is discoverable (vcpkg finds Visual
# Studio through vswhere); git-lfs must be on PATH for the nvidia-dlss overlay port.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) args+=(--triplet x64-windows) ;;
esac
exec "$VCPKG_ROOT/vcpkg" "${args[@]}"
