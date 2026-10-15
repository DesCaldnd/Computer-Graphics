# Boots the OxwaldEngine AVD with host GPU (gfxstream: guest Vulkan 1.3 forwarded to the host driver) and waits
# until Android is up.
#   tools/android/start-emulator.ps1                 # headless (-no-window): automation, smoke tests, screenshots
#   tools/android/start-emulator.ps1 -Window         # with the emulator window (touch with the mouse)
#   tools/android/start-emulator.ps1 -Gpu swiftshader_indirect   # software Vulkan fallback
param(
    [string]$Avd = "Oxwald_API35_x86_64",
    [string]$Gpu = "host",
    [switch]$Window,
    [switch]$NoWait,          # launch and return (poll with: adb shell getprop sys.boot_completed)
    [int]$TimeoutSec = 600
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\env.ps1"
# The emulator must find everything under E: (no ~/.android on C:) and must not ask for adb in a modal dialog.
if (-not $env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT = $env:ANDROID_HOME }
if (-not $env:ANDROID_EMULATOR_HOME) { $env:ANDROID_EMULATOR_HOME = $env:ANDROID_USER_HOME }
& "$PSScriptRoot\configure-emulator-adb.ps1" -Sdk $env:ANDROID_HOME | Out-Null

$emu = Join-Path $env:ANDROID_HOME "emulator\emulator.exe"
$running = & $Adb devices | Select-String "^emulator-\d+\s+device"
if ($running) { Write-Host "emulator already running: $($running.Line)"; exit 0 }
$emuArgs = @("-avd", $Avd, "-gpu", $Gpu, "-no-snapshot-save", "-no-boot-anim", "-no-audio", "-feature", "Vulkan")
if (-not $Window) { $emuArgs += "-no-window" }
$log = Join-Path $env:ANDROID_USER_HOME "emulator-$Avd.log"
Start-Process -FilePath $emu -ArgumentList $emuArgs -RedirectStandardOutput $log -RedirectStandardError "$log.err" -WindowStyle Minimized
if ($NoWait) { Write-Host "emulator starting (log: $log)"; exit 0 }
& $Adb wait-for-device
$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline) {
    $booted = Invoke-AdbQuiet shell getprop sys.boot_completed
    if ($booted -eq "1") { Write-Host "emulator booted"; exit 0 }
    Start-Sleep -Seconds 3
}
throw "emulator did not boot within $TimeoutSec s (log: $log)"
