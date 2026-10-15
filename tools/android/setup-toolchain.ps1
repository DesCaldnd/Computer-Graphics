# Installs the Android toolchain for OxwaldEngine without admin rights:
#   JDK 17 (Temurin), Android SDK cmdline-tools, platform-tools, build-tools, platforms, NDK, CMake, emulator,
#   an x86_64 Google APIs system image and an AVD. Everything lands under $Root (default E:\Development\Android),
#   user-level environment variables point there so nothing large is written to C:.
param(
    [string]$Root = "E:\Development\Android",
    [string]$NdkVersion = "28.2.13676358",
    [string]$BuildTools = "35.0.0",
    [string]$CMakeVersion = "3.31.6",
    [string]$SystemImage = "system-images;android-35;google_apis;x86_64",
    [string]$AvdName = "Oxwald_API35_x86_64",
    [switch]$SkipAvd
)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"   # Invoke-WebRequest is 10x slower with the progress bar

$jdk  = Join-Path $Root "jdk"
$sdk  = Join-Path $Root "sdk"
$dl   = Join-Path $Root "downloads"
$user = Join-Path $Root "user"            # ANDROID_USER_HOME (.android: adb keys, emulator config)
$avd  = Join-Path $Root "avd"             # ANDROID_AVD_HOME
$gradle = Join-Path $Root "gradle"        # GRADLE_USER_HOME (wrapper distributions, dependency cache)
New-Item -ItemType Directory -Force $jdk, $sdk, $dl, $user, $avd, $gradle | Out-Null

function Get-File($url, $out) {
    if (-not (Test-Path $out)) {
        Write-Host "Downloading $url"
        Invoke-WebRequest -Uri $url -OutFile "$out.part" -UseBasicParsing
        Move-Item "$out.part" $out
    }
}

# --- JDK 17 ---
if (-not (Test-Path (Join-Path $jdk "bin\java.exe"))) {
    $zip = Join-Path $dl "temurin17.zip"
    Get-File "https://api.adoptium.net/v3/binary/latest/17/ga/windows/x64/jdk/hotspot/normal/eclipse?project=jdk" $zip
    $tmp = Join-Path $dl "jdk_extract"
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    Expand-Archive $zip $tmp
    $inner = Get-ChildItem $tmp | Select-Object -First 1
    Get-ChildItem $inner.FullName | Move-Item -Destination $jdk
    Remove-Item -Recurse -Force $tmp
}

# --- cmdline-tools ---
$cmdline = Join-Path $sdk "cmdline-tools\latest"
if (-not (Test-Path (Join-Path $cmdline "bin\sdkmanager.bat"))) {
    $zip = Join-Path $dl "cmdline-tools.zip"
    Get-File "https://dl.google.com/android/repository/commandlinetools-win-11076708_latest.zip" $zip
    $tmp = Join-Path $dl "cmdline_extract"
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    Expand-Archive $zip $tmp
    New-Item -ItemType Directory -Force (Split-Path $cmdline) | Out-Null
    Move-Item (Join-Path $tmp "cmdline-tools") $cmdline
    Remove-Item -Recurse -Force $tmp
}

# --- environment (user level only) ---
$vars = @{
    JAVA_HOME = $jdk; ANDROID_HOME = $sdk; ANDROID_SDK_ROOT = $sdk
    ANDROID_NDK_HOME = (Join-Path $sdk "ndk\$NdkVersion"); ANDROID_NDK_ROOT = (Join-Path $sdk "ndk\$NdkVersion")
    ANDROID_USER_HOME = $user; ANDROID_EMULATOR_HOME = $user; ANDROID_AVD_HOME = $avd; GRADLE_USER_HOME = $gradle
}
foreach ($k in $vars.Keys) {
    [Environment]::SetEnvironmentVariable($k, $vars[$k], "User")
    Set-Item "env:$k" $vars[$k]
}
$env:Path = "$jdk\bin;$sdk\platform-tools;$sdk\emulator;$cmdline\bin;$env:Path"
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
foreach ($p in @("$sdk\platform-tools", "$sdk\emulator")) {
    if ($userPath -notlike "*$p*") { $userPath = "$userPath;$p" }
}
[Environment]::SetEnvironmentVariable("Path", $userPath, "User")

# --- SDK packages ---
$sdkm = Join-Path $cmdline "bin\sdkmanager.bat"
# PowerShell pipes do not reach the batch file's java process reliably: feed the "y" answers through cmd.
cmd /c "(for /L %i in (1,1,40) do @echo y) | `"$sdkm`" --sdk_root=`"$sdk`" --licenses" | Out-Null
$pkgs = @("platform-tools", "build-tools;$BuildTools", "platforms;android-34", "platforms;android-35",
          "ndk;$NdkVersion", "cmake;$CMakeVersion", "emulator", $SystemImage)
& $sdkm --sdk_root=$sdk @pkgs
if ($LASTEXITCODE -ne 0) { throw "sdkmanager failed ($LASTEXITCODE)" }

# --- AVD ---
if (-not $SkipAvd) {
    $avdm = Join-Path $cmdline "bin\avdmanager.bat"
    $existing = & $avdm list avd -c
    if ($existing -notcontains $AvdName) {
        "no" | & $avdm create avd -n $AvdName -k $SystemImage -d "pixel_7" --force
    }
    $cfg = Join-Path $avd "$AvdName.avd\config.ini"
    if (Test-Path $cfg) {
        $lines = Get-Content $cfg | Where-Object { $_ -notmatch '^(hw\.gpu\.(enabled|mode)|hw\.ramSize|disk\.dataPartition\.size|hw\.keyboard|vm\.heapSize)=' }
        $lines += "hw.gpu.enabled=yes", "hw.gpu.mode=host", "hw.ramSize=4096", "disk.dataPartition.size=8G", "hw.keyboard=yes", "vm.heapSize=512"
        Set-Content -Encoding ascii $cfg $lines
    }
}
# The emulator UI must not ask for adb in a modal dialog (blocks automation).
& "$PSScriptRoot\configure-emulator-adb.ps1" -Sdk $sdk
Write-Host "Android toolchain ready in $Root"
