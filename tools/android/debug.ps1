# Native debugging of OxwaldPlayer with the NDK's LLDB from the command line (what Android Studio does underneath):
#   1. pushes the NDK lldb-server for the device ABI into the app sandbox (run-as, debuggable APK required),
#   2. starts the app with --ez wait_debugger true (android_main spins until a tracer attaches),
#   3. runs `lldb-server platform` inside the app sandbox on an abstract unix socket,
#   4. starts lldb.exe: platform remote-android -> attach -> breakpoints -> continue.
# Symbols: the unstripped liboxwald_player.so from the CMake build tree is added to the search path, so this also
# works with stripped APKs.
#
#   tools/android/debug.ps1                                     # interactive lldb, break in ox::Engine::tick
#   tools/android/debug.ps1 -Break "ox::render::RuntimeRenderer::render" -Batch   # break once, print bt, detach
#   tools/android/debug.ps1 -NoLaunch                           # attach to the running app
#   tools/android/debug.ps1 -PlayerArgs "--frames 2000"
param(
    [string[]]$Break = @("ox::Engine::tick"),
    [string]$PlayerArgs = "",
    [switch]$NoLaunch,
    [switch]$Batch,            # non-interactive: stop at the first breakpoint, print backtrace + locals, detach
    [string[]]$ExtraCommands = @(),
    [string]$SymbolDir = ""
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\env.ps1"
$abi = Get-DeviceAbi
$arch = @{ "arm64-v8a" = "aarch64"; "x86_64" = "x86_64"; "armeabi-v7a" = "arm"; "x86" = "i386" }[$abi]
$llvm = Join-Path $OxNdk "toolchains\llvm\prebuilt\windows-x86_64"
$lldbServer = Get-ChildItem (Join-Path $llvm "lib\clang") -Recurse -Filter lldb-server | Where-Object { $_.Directory.Name -eq $arch } | Select-Object -First 1
# lldb.cmd sets PYTHONHOME/PATH for the bundled python311.dll; lldb.exe alone fails to load ("cannot open shared object").
$lldb = Join-Path $llvm "bin\lldb.cmd"
if (-not $lldbServer) { throw "lldb-server for $arch not found in $llvm" }

# 1. lldb-server into the sandbox (it must run as the app's uid to ptrace it).
# (adb push reports progress on stderr: with the script's output redirected, e.g. `debug.ps1 -Batch *> log`, Windows
# PowerShell 5.1 would turn that into a terminating error under $ErrorActionPreference = "Stop")
& { $ErrorActionPreference = "Continue"; & $Adb push $lldbServer.FullName /data/local/tmp/oxwald-lldb-server 2>&1 | Out-Null }
if ($LASTEXITCODE -ne 0) { throw "adb push of lldb-server failed" }
# (a server left from an earlier session keeps the binary busy: "cp: Text file busy")
& $Adb shell "run-as $OxPackage sh -c 'pkill -x lldb-server; cp /data/local/tmp/oxwald-lldb-server ./lldb-server && chmod 700 ./lldb-server'"
if ($LASTEXITCODE -ne 0) { throw "run-as failed: is the installed APK debuggable (debug build type)?" }

# 2. launch and wait for the process
if (-not $NoLaunch) {
    $runArgs = @{ WaitDebugger = $true }
    if ($PlayerArgs) { $runArgs.PlayerArgs = $PlayerArgs }
    & "$PSScriptRoot\run.ps1" @runArgs | Out-Null
}
$appPid = ""
for ($i = 0; $i -lt 50 -and -not $appPid; $i++) { $appPid = Get-AppPid; if (-not $appPid) { Start-Sleep -Milliseconds 200 } }
if (-not $appPid) { throw "$OxPackage is not running" }
Write-Host "app pid $appPid ($abi)"

# 3. lldb-server platform on an abstract socket inside the sandbox
$socket = "/$OxPackage/platform-oxwald.sock"
& $Adb shell "run-as $OxPackage sh -c 'pkill -x lldb-server; true'" | Out-Null
$server = Start-Process -FilePath $Adb -ArgumentList @("shell", "run-as $OxPackage ./lldb-server platform --server --listen unix-abstract://$socket") -PassThru -WindowStyle Hidden
# (no --log-channels "": Start-Process joins the arguments unquoted and lldb-server then drops the first connection)
Start-Sleep -Seconds 2

# 4. symbols + lldb script
if (-not $SymbolDir) {
    $lib = Get-ChildItem (Join-Path $OxBuildRoot "cxx") -Recurse -Filter liboxwald_player.so -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "\\obj\\$abi\\" -or $_.FullName -match "\\$abi\\lib\\" } | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($lib) { $SymbolDir = $lib.DirectoryName }
}
$cmds = @(
    # ART's JIT registers code with the GDB JIT interface; stopping on every registration only gets in the way.
    "settings set plugin.jit-loader.gdb.enable off",
    "platform select remote-android",
    "platform connect unix-abstract-connect://$socket"
)
if ($SymbolDir) { $cmds += "settings append target.exec-search-paths `"$($SymbolDir.Replace('\','/'))`"" }
$cmds += "process attach --pid $appPid"
foreach ($b in $Break) { if ($b -match "^(.+):(\d+)$") { $cmds += "breakpoint set --file `"$($Matches[1])`" --line $($Matches[2])" } else { $cmds += "breakpoint set --name `"$b`"" } }
# Android sends SIGSEGV/SIGBUS-like signals for its own purposes (ART, GC) - do not stop on them while debugging native code.
$cmds += "process handle SIGSEGV SIGBUS SIGXCPU SIGPWR -n true -p true -s false"
$cmds += $ExtraCommands
$cmds += "process continue"
if ($Batch) {
    # The stop can report several threads (signals, other breakpoints): select the one at our breakpoint.
    $cmds += "script p = lldb.debugger.GetSelectedTarget().GetProcess(); t = [p.GetThreadAtIndex(i) for i in range(p.GetNumThreads()) if p.GetThreadAtIndex(i).GetStopReason() == lldb.eStopReasonBreakpoint]; print('breakpoint threads: %s' % [x.GetIndexID() for x in t]); t and p.SetSelectedThreadByIndexID(t[0].GetIndexID())"
    $cmds += @("thread backtrace 20", "frame variable", "image list liboxwald_player.so", "breakpoint delete --force", "process detach", "quit")
}
$script = Join-Path $env:TEMP "oxwald-lldb.txt"
Set-Content -Encoding ascii $script $cmds
Write-Host "lldb commands ($script):"; $cmds | ForEach-Object { Write-Host "  $_" }
$env:ANDROID_SERIAL = ((& $Adb get-serialno) -join "").Trim()
$lldbExit = 0
try {
    if ($Batch) { & $lldb --batch -s $script } else { & $lldb -s $script }
    $lldbExit = $LASTEXITCODE
} finally {
    & $Adb shell "run-as $OxPackage sh -c 'pkill -x lldb-server; true'" | Out-Null
    if ($server -and -not $server.HasExited) { $server.Kill() }
}
if ($lldbExit -ne 0) { Write-Host "lldb exited with $lldbExit"; exit $lldbExit }
