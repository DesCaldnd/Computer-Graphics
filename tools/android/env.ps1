# Dot-source from the other tools/android scripts:  . "$PSScriptRoot\env.ps1"
# Loads the Android toolchain environment (user-level variables written by setup-toolchain.ps1) into this process
# even when the shell was started before they were set, and defines common paths.
# The user-level values win over inherited ones: a machine-wide JAVA_HOME (e.g. another JDK 17 build) makes AGP's
# JdkImageTransform (jlink over android-35 core-for-system-modules.jar) fail.
foreach ($k in "JAVA_HOME", "ANDROID_HOME", "ANDROID_SDK_ROOT", "ANDROID_NDK_HOME", "ANDROID_USER_HOME", "ANDROID_AVD_HOME", "GRADLE_USER_HOME") {
    $v = [Environment]::GetEnvironmentVariable($k, "User")
    if ($v) { Set-Item "env:$k" $v }
}
if (-not $env:ANDROID_HOME) { throw "ANDROID_HOME is not set: run tools/android/setup-toolchain.ps1" }
$env:Path = "$env:JAVA_HOME\bin;$env:ANDROID_HOME\platform-tools;$env:ANDROID_HOME\emulator;$env:Path"

$OxRepo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$OxGradleDir = Join-Path $OxRepo "apps\android"
$OxBuildRoot = if ($env:OX_ANDROID_BUILD_ROOT) { $env:OX_ANDROID_BUILD_ROOT } else { "E:\Development\OxwaldEngine\build\android" }
$OxPackage = "com.oxwald.player"
$OxActivity = "$OxPackage/.OxwaldActivity"
$Adb = Join-Path $env:ANDROID_HOME "platform-tools\adb.exe"
$OxNdk = $env:ANDROID_NDK_HOME
if (-not $OxNdk) { $OxNdk = Get-ChildItem (Join-Path $env:ANDROID_HOME "ndk") | Sort-Object Name | Select-Object -Last 1 -ExpandProperty FullName }

function Invoke-Adb {
    & $Adb @args
}

# Runs adb and returns its stdout as one trimmed string, discarding stderr. Windows PowerShell 5.1 turns native
# stderr into terminating NativeCommandError records under $ErrorActionPreference = "Stop" (even with 2>$null), so
# "error: no devices/emulators found" would abort the calling script; this relaxes the preference for the call only.
function Invoke-AdbQuiet {
    $ErrorActionPreference = "Continue"
    try { ((& $Adb @args 2>$null) -join "`n").Trim() } catch { "" }
}

# "device" when exactly one device / emulator is ready, otherwise "" (none, offline, unauthorized).
function Get-AdbState {
    Invoke-AdbQuiet get-state
}

# Binary-safe adb exec-out to a file (PowerShell's > re-encodes native output as text and corrupts PNGs).
function Save-AdbExecOut([string]$Command, [string]$Path) {
    cmd /c "`"$Adb`" exec-out $Command > `"$Path`" 2>nul"
}

# Device ABI (arm64-v8a on phones, x86_64 on the emulator).
function Get-DeviceAbi {
    Invoke-AdbQuiet shell getprop ro.product.cpu.abi
}

function Get-AppPid {
    Invoke-AdbQuiet shell pidof $OxPackage
}
