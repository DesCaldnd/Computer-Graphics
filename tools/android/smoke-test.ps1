# On-device smoke test: runs the installed OxwaldPlayer for N frames (windowed, Vulkan validation on), waits for
# it to exit, then checks the log: exit code 0, the frame count, no Vulkan validation errors, no crash; pulls the
# player's own screenshot of the last frame and an `adb screencap` of the screen.
# Exit codes: 0 pass, 1 fail, 77 skipped (no device / app not installed) - ctest label "android" (SKIP_RETURN_CODE).
#   tools/android/smoke-test.ps1 [-Frames 300] [-PlayerArgs "--scene ..."] [-OutDir <dir>]
param(
    [int]$Frames = 300,
    [string]$PlayerArgs = "",
    [string]$OutDir = "E:\Development\OxwaldEngine\build\android\smoke",
    [int]$TimeoutSec = 600,
    [switch]$NoValidation
)
$ErrorActionPreference = "Stop"
try { . "$PSScriptRoot\env.ps1" } catch { Write-Host "SKIP: $_"; exit 77 }
$state = Get-AdbState
if ($state -ne "device") { Write-Host "SKIP: no Android device / emulator connected"; exit 77 }
$installed = Invoke-AdbQuiet shell pm list packages $OxPackage
if ($installed -notmatch [regex]::Escape($OxPackage)) { Write-Host "SKIP: $OxPackage is not installed (tools/android/build.ps1)"; exit 77 }

New-Item -ItemType Directory -Force $OutDir | Out-Null
$log = Join-Path $OutDir "logcat.txt"
& $Adb logcat -c
# Android shows a "Viewing full screen" confirmation the first time an app goes immersive (every cold-booted AVD):
# it would steal focus (the player pauses audio and input) and cover the screencap. Mark it as already seen.
Invoke-AdbQuiet shell settings put secure immersive_mode_confirmations confirmed | Out-Null
$playerLine = "--frames $Frames --stats --screenshot smoke.png $PlayerArgs".Trim()
$validation = if ($NoValidation) { "0" } else { "1" }
Write-Host "starting: $playerLine (validation $validation)"
& $Adb shell "am start -S -W -n $OxActivity --es validation $validation --es args '$playerLine'" | Out-Null

$deadline = (Get-Date).AddSeconds($TimeoutSec)
$done = $false
$screenFile = Join-Path $OutDir "screen.png"
$screenTaken = $false
Remove-Item $screenFile -ErrorAction SilentlyContinue
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 3
    $tail = Invoke-AdbQuiet logcat -d -s OxwaldStdout:V OxwaldCrash:V DEBUG:V
    # Screen grab while the game is on screen (the first stats line comes after the first rendered frames).
    if (-not $screenTaken -and $tail -match "frame stats") { Save-AdbExecOut "screencap -p" $screenFile; $screenTaken = $true }
    if ($tail -match "OxwaldPlayer: exit code" -or $tail -match "OxwaldCrash" -or $tail -match "\*\*\* \*\*\* \*\*\*") { $done = $true; break }
    if (-not (Get-AppPid)) { Start-Sleep -Seconds 2; $done = $true; break } # died without a word
}
if (-not $screenTaken) { Save-AdbExecOut "screencap -p" $screenFile }
cmd /c "`"$Adb`" logcat -d -v threadtime > `"$log`" 2>nul" # raw bytes (UTF-8), not PowerShell's UTF-16
$text = Get-Content $log -Raw -Encoding UTF8
$userShot = Join-Path $OutDir "player-screenshot.png"
Save-AdbExecOut "run-as $OxPackage cat files/user/smoke.png" $userShot

$fail = @()
if (-not $done) { $fail += "timed out after $TimeoutSec s" }
if ($text -notmatch "OxwaldPlayer: exit code 0") { $fail += "no 'exit code 0' in the log" }
if ($text -notmatch "OxwaldPlayer: frames=(\d+)") { $fail += "no frame summary" }
elseif ([int]$Matches[1] -lt $Frames) { $fail += "only $($Matches[1]) of $Frames frames" }
$vkErrors = ([regex]::Matches($text, "E Oxwald\s*: \[vulkan\]")).Count
if ($vkErrors -gt 0) { $fail += "$vkErrors Vulkan validation error(s)" }
if ($text -match "OxwaldCrash|Fatal signal|FATAL EXCEPTION") { $fail += "crash" }
# Case-sensitive and anchored on the engine's tag: "Device: " alone also matches system lines (e.g. "...Device: -1").
if ($text -cmatch "[[]rhi[]] Device: .*validation on") { Write-Host "validation layers: active" } elseif (-not $NoValidation) { Write-Host "validation layers: NOT active (layer missing from the APK?)" }
$stats = [regex]::Matches($text, "OxwaldPlayer: frame stats: (.*)")
if ($stats.Count -gt 0) { Write-Host "last stats: $($stats[$stats.Count - 1].Groups[1].Value.Trim())" }
if ($text -cmatch "[[]rhi[]] Device: (.*)") { Write-Host "device: $($Matches[1].Trim())" }
Write-Host "log: $log, screenshots: $OutDir"
if ($fail.Count -gt 0) { Write-Host "FAIL: $($fail -join '; ')"; exit 1 }
Write-Host "PASS: $Frames frames, exit code 0, 0 validation errors"
exit 0
