# Installs the engine's vcpkg dependencies for Android (manifest mode, no default features = no Qt editor).
#   tools/android/vcpkg-install.ps1 -Triplet x64-android      # emulator (AVD x86_64)
#   tools/android/vcpkg-install.ps1 -Triplet arm64-android    # phones (arm64-v8a)
# Everything big goes to E:\Development (buildtrees, packages, downloads, binary cache, install root).
param(
    [ValidateSet("arm64-android", "x64-android")] [string]$Triplet = "x64-android",
    [string]$VcpkgRoot = $(if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { "C:\Programming\vcpkg" }),
    [string]$Work = "E:\Development",
    # Parent of the per-triplet install roots: <InstallRoot>\<triplet>\<triplet>. A manifest-mode install keeps only
    # the triplet it installs, so x64-android and arm64-android cannot share one root (the second removes the first).
    [string]$InstallRoot = "E:\Development\OxwaldEngine\vcpkg_installed_android",
    [int]$Jobs = 4,
    [string]$TempRoot = ""
)
$ErrorActionPreference = "Stop"
$repo = Resolve-Path (Join-Path $PSScriptRoot "..\..")
if (-not $env:ANDROID_NDK_HOME) {
    $env:ANDROID_NDK_HOME = [Environment]::GetEnvironmentVariable("ANDROID_NDK_HOME", "User")
}
if (-not $env:ANDROID_NDK_HOME -or -not (Test-Path $env:ANDROID_NDK_HOME)) {
    throw "ANDROID_NDK_HOME is not set (run tools/android/setup-toolchain.ps1)"
}
$env:VCPKG_MAX_CONCURRENCY = "$Jobs"
$env:VCPKG_DEFAULT_BINARY_CACHE = Join-Path $Work "vcpkg-cache"
$tripletRoot = Join-Path $InstallRoot $Triplet
New-Item -ItemType Directory -Force $env:VCPKG_DEFAULT_BINARY_CACHE, $tripletRoot | Out-Null
$vcpkg = Join-Path $VcpkgRoot "vcpkg.exe"
# Build temporaries can go to a faster disk (-TempRoot F:\Development); install root and caches stay in $Work.
$tempRoot = if ($TempRoot) { $TempRoot } else { $Work }
$buildtrees = Join-Path $tempRoot "vcpkg-buildtrees"
$packages = Join-Path $tempRoot "vcpkg-packages"
$downloads = Join-Path $Work "vcpkg-downloads"
$vcpkgArgs = @(
    "install", "--triplet=$Triplet", "--host-triplet=x64-windows",
    "--x-manifest-root=$repo", "--x-install-root=$tripletRoot",
    "--x-buildtrees-root=$buildtrees", "--x-packages-root=$packages", "--downloads-root=$downloads",
    "--x-no-default-features", "--clean-buildtrees-after-build", "--clean-packages-after-build", "--no-print-usage"
)
& $vcpkg @vcpkgArgs
exit $LASTEXITCODE
