# Downloads the Khronos Vulkan validation layer for Android (libVkLayer_khronos_validation.so per ABI) from the
# Vulkan-ValidationLayers GitHub release and lays it out as <Dest>/<abi>/libVkLayer_khronos_validation.so.
# apps/android packages that directory into debug APKs (gradle property oxValidationDir); the NDK no longer ships it.
#   tools/android/fetch-validation-layers.ps1 [-Dest E:\Development\Android\vvl] [-Tag vulkan-sdk-1.4.321.0]
param(
    [string]$Dest = "E:\Development\Android\vvl",
    [string]$Tag = ""   # empty = latest release
)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$api = if ($Tag) { "https://api.github.com/repos/KhronosGroup/Vulkan-ValidationLayers/releases/tags/$Tag" }
       else { "https://api.github.com/repos/KhronosGroup/Vulkan-ValidationLayers/releases/latest" }
$release = Invoke-RestMethod -Uri $api -Headers @{ "User-Agent" = "OxwaldEngine" }
$asset = $release.assets | Where-Object { $_.name -like "android-binaries*.zip" } | Select-Object -First 1
if (-not $asset) { throw "release $($release.tag_name) has no android-binaries*.zip asset" }
$work = Join-Path ([IO.Path]::GetDirectoryName($Dest)) "vvl-download"
New-Item -ItemType Directory -Force $work, $Dest | Out-Null
$zip = Join-Path $work $asset.name
if (-not (Test-Path $zip)) {
    Write-Host "Downloading $($asset.browser_download_url)"
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zip -UseBasicParsing
}
$extract = Join-Path $work "extract"
if (Test-Path $extract) { Remove-Item -Recurse -Force $extract }
Expand-Archive $zip $extract
foreach ($abi in "arm64-v8a", "x86_64", "armeabi-v7a", "x86") {
    $so = Get-ChildItem $extract -Recurse -Filter libVkLayer_khronos_validation.so | Where-Object { $_.Directory.Name -eq $abi } | Select-Object -First 1
    if ($so) {
        New-Item -ItemType Directory -Force (Join-Path $Dest $abi) | Out-Null
        Copy-Item $so.FullName (Join-Path $Dest "$abi\libVkLayer_khronos_validation.so") -Force
        Write-Host "  $abi <- $($release.tag_name)"
    }
}
Set-Content -Encoding ascii (Join-Path $Dest "VERSION.txt") $release.tag_name
Write-Host "validation layers in $Dest ($($release.tag_name))"
