# The emulator UI looks for adb on its own and shows a modal "could not automatically detect adb binary" dialog
# when it fails (it then blocks automation until closed). Point it at the SDK's adb explicitly: the emulator keeps
# its UI settings (Extended controls -> Settings -> "Use detected ADB location") in
# <emulator user dir>/Android Open Source Project/Emulator.conf (QSettings) with the keys set/autoFindAdb and
# set/adbPath. The user dir is ANDROID_EMULATOR_HOME (set to the same place as ANDROID_USER_HOME), falling back
# to %USERPROFILE%\.android - all candidates are written.
param([string]$Sdk = $(if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { [Environment]::GetEnvironmentVariable("ANDROID_HOME", "User") }))
$ErrorActionPreference = "Stop"
$adb = (Join-Path $Sdk "platform-tools\adb.exe")
if (-not (Test-Path $adb)) { throw "adb not found at $adb" }
$userHome = [Environment]::GetEnvironmentVariable("ANDROID_USER_HOME", "User")
if ($userHome -and -not [Environment]::GetEnvironmentVariable("ANDROID_EMULATOR_HOME", "User")) {
    [Environment]::SetEnvironmentVariable("ANDROID_EMULATOR_HOME", $userHome, "User")
}
$dirs = @($userHome, [Environment]::GetEnvironmentVariable("ANDROID_EMULATOR_HOME", "User"), (Join-Path $env:USERPROFILE ".android")) |
    Where-Object { $_ } | Select-Object -Unique
$adbIni = $adb.Replace('\', '/')
foreach ($d in $dirs) {
    $confDir = Join-Path $d "Android Open Source Project"
    New-Item -ItemType Directory -Force $confDir | Out-Null
    $conf = Join-Path $confDir "Emulator.conf"
    $lines = @()
    if (Test-Path $conf) { $lines = Get-Content $conf | Where-Object { $_ -notmatch '^(autoFindAdb|adbPath)=' } }
    if ($lines -notcontains "[set]") { $lines += "[set]" }
    $out = @()
    foreach ($l in $lines) {
        $out += $l
        if ($l -eq "[set]") { $out += "autoFindAdb=false"; $out += "adbPath=$adbIni" }
    }
    Set-Content -Encoding ascii $conf $out
    Write-Host "emulator adb path -> $adbIni ($conf)"
}
