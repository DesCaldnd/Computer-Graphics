# Builds the OxwaldPlayer APK (apps/android) and optionally installs it.
#   tools/android/build.ps1                                   # debug, x86_64 (emulator), showcase pak, install
#   tools/android/build.ps1 -Abi arm64-v8a -BuildType Release # phone release APK
#   tools/android/build.ps1 -NativeConfig Debug               # -O0 native code for step debugging
#   tools/android/build.ps1 -Sanitizer hwaddress -Abi arm64-v8a  # HWASan (Android 14+ arm64, e.g. OnePlus 12)
# Prerequisites: setup-toolchain.ps1, vcpkg-install.ps1 -Triplet <x64|arm64>-android, a host oxpack.exe (any
# desktop build) to cook the project. Outputs go to $OX_ANDROID_BUILD_ROOT (default E:\Development\...\android).
param(
    [ValidateSet("x86_64", "arm64-v8a", "all")] [string]$Abi = "x86_64",
    [ValidateSet("Debug", "Release")] [string]$BuildType = "Debug",
    [ValidateSet("Debug", "RelWithDebInfo", "Release")] [string]$NativeConfig = "RelWithDebInfo",
    [ValidateSet("", "address", "hwaddress")] [string]$Sanitizer = "",
    [string]$Project = "",   # project directory to cook (default samples/OxwaldShowcase)
    [string]$Pak = "",       # use this pak instead of cooking
    [int]$Jobs = 4,          # parallel native compile jobs
    [switch]$NoPak,
    [switch]$NoInstall,
    [string]$Oxpack = ""
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\env.ps1"

if (-not $NoPak -and -not $Pak) {
    if (-not $Project) { $Project = Join-Path $OxRepo "samples\OxwaldShowcase" }
    if (-not $Oxpack) {
        $candidates = @("E:\Development\OxwaldEngine\build\android-hostcheck\bin\oxpack.exe") +
            (Get-ChildItem (Join-Path $OxRepo "build") -Directory -ErrorAction SilentlyContinue | ForEach-Object { Join-Path $_.FullName "bin\oxpack.exe" })
        $Oxpack = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    }
    if (-not $Oxpack) { throw "oxpack.exe not found: build a desktop configuration first or pass -Oxpack / -Pak / -NoPak" }
    $name = Split-Path $Project -Leaf
    $Pak = Join-Path $OxBuildRoot "content\$name.oxpak"
    New-Item -ItemType Directory -Force (Split-Path $Pak) | Out-Null
    Write-Host "cooking $Project -> $Pak ($Oxpack)"
    & $Oxpack $Project -o $Pak --quiet
    if ($LASTEXITCODE -ne 0) { throw "oxpack failed ($LASTEXITCODE)" }
}

$abis = if ($Abi -eq "all") { "arm64-v8a,x86_64" } else { $Abi }
$task = "assemble$BuildType"
$gradleArgs = @($task, "-PoxAbis=$abis", "-PoxNativeConfig=$NativeConfig", "-PoxSanitizer=$Sanitizer", "-PoxPak=$Pak", "-PoxJobs=$Jobs",
                "-PoxBuildRoot=$($OxBuildRoot.Replace('\','/'))", "--console=plain")
# AGP packages incrementally into the previous APK and leaves the replaced entries as dead space (a 140 MB APK grew
# to 800+ MB after a few native rebuilds): start every package step from scratch.
$gradleOut = Join-Path $OxBuildRoot "gradle\app"
foreach ($d in @("outputs\apk\$($BuildType.ToLower())", "intermediates\apk\$($BuildType.ToLower())", "intermediates\incremental\package$BuildType")) {
    Remove-Item -Recurse -Force (Join-Path $gradleOut $d) -ErrorAction SilentlyContinue
}
Push-Location $OxGradleDir
try {
    & .\gradlew.bat @gradleArgs
    if ($LASTEXITCODE -ne 0) { throw "gradle $task failed" }
} finally { Pop-Location }

$apk = Get-ChildItem (Join-Path $OxBuildRoot "gradle\app\outputs\apk\$($BuildType.ToLower())") -Filter *.apk | Sort-Object LastWriteTime | Select-Object -Last 1
Write-Host "APK: $($apk.FullName) ($([math]::Round($apk.Length / 1MB, 1)) MB)"
if (-not $NoInstall) {
    if ((Get-AdbState) -eq "device") {
        & $Adb install -r -t $apk.FullName
        if ($LASTEXITCODE -ne 0) { throw "adb install failed" }
    } else {
        Write-Host "no device connected: not installing"
    }
}
