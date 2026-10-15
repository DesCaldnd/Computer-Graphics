#!/usr/bin/env bash
# Native LLDB session for OxwaldPlayer from Linux/macOS/Git Bash (same flow as debug.ps1).
#   tools/android/debug.sh [breakpoint ...]          e.g. tools/android/debug.sh ox::Engine::tick
#   BATCH=1 tools/android/debug.sh ox::Engine::tick  # stop once, print backtrace, detach
#   NO_LAUNCH=1 tools/android/debug.sh               # attach to the running app
set -euo pipefail
PKG=com.oxwald.player
ACTIVITY=$PKG/.OxwaldActivity
SDK=${ANDROID_HOME:?ANDROID_HOME not set}
NDK=${ANDROID_NDK_HOME:-$(ls -d "$SDK"/ndk/* | sort | tail -1)}
ADB="$SDK/platform-tools/adb"
BUILD_ROOT=${OX_ANDROID_BUILD_ROOT:-/e/Development/OxwaldEngine/build/android}
case "$(uname -s)" in
  Linux*) HOST=linux-x86_64 ;; Darwin*) HOST=darwin-x86_64 ;; *) HOST=windows-x86_64 ;;
esac
LLVM="$NDK/toolchains/llvm/prebuilt/$HOST"
LLDB="$LLVM/bin/lldb"; [ -x "$LLDB" ] || LLDB="$LLVM/bin/lldb.exe"
ABI=$("$ADB" shell getprop ro.product.cpu.abi | tr -d '\r')
case "$ABI" in arm64-v8a) ARCH=aarch64 ;; x86_64) ARCH=x86_64 ;; armeabi-v7a) ARCH=arm ;; *) ARCH=i386 ;; esac
SERVER=$(ls "$LLVM"/lib/clang/*/lib/linux/$ARCH/lldb-server | head -1)
BREAKS=("$@"); [ ${#BREAKS[@]} -eq 0 ] && BREAKS=("ox::Engine::tick")

"$ADB" push "$SERVER" /data/local/tmp/oxwald-lldb-server >/dev/null
"$ADB" shell "run-as $PKG sh -c 'cp /data/local/tmp/oxwald-lldb-server ./lldb-server && chmod 700 ./lldb-server'"
if [ -z "${NO_LAUNCH:-}" ]; then
  "$ADB" shell "am start -S -W -n $ACTIVITY --ez wait_debugger true ${PLAYER_ARGS:+--es args '$PLAYER_ARGS'}" >/dev/null
fi
PID=""
for _ in $(seq 50); do PID=$("$ADB" shell pidof $PKG | tr -d '\r'); [ -n "$PID" ] && break; sleep 0.2; done
[ -n "$PID" ] || { echo "$PKG is not running" >&2; exit 1; }
SOCK="/$PKG/platform-oxwald.sock"
"$ADB" shell "run-as $PKG sh -c 'pkill -f lldb-server; true'" || true
"$ADB" shell "run-as $PKG ./lldb-server platform --server --listen unix-abstract://$SOCK" &
SERVER_JOB=$!
trap '"$ADB" shell "run-as $PKG sh -c \"pkill -f lldb-server; true\"" >/dev/null 2>&1 || true; kill $SERVER_JOB 2>/dev/null || true' EXIT
sleep 1
SYMDIR=$(find "$BUILD_ROOT/cxx" -path "*obj/$ABI/liboxwald_player.so" -printf '%T@ %h\n' 2>/dev/null | sort -n | tail -1 | cut -d' ' -f2- || true)
CMDS=$(mktemp)
{
  echo "platform select remote-android"
  echo "platform connect unix-abstract-connect://$SOCK"
  [ -n "$SYMDIR" ] && echo "settings append target.exec-search-paths \"$SYMDIR\""
  echo "process attach --pid $PID"
  for b in "${BREAKS[@]}"; do echo "breakpoint set --name \"$b\""; done
  echo "process handle SIGSEGV SIGBUS SIGXCPU SIGPWR -n true -p true -s false"
  echo "process continue"
  if [ -n "${BATCH:-}" ]; then
    printf '%s\n' "thread backtrace 20" "frame variable" "breakpoint delete --force" "process detach" "quit"
  fi
} > "$CMDS"
export ANDROID_SERIAL=$("$ADB" get-serialno | tr -d '\r')
if [ -n "${BATCH:-}" ]; then "$LLDB" --batch -s "$CMDS"; else "$LLDB" -s "$CMDS"; fi
