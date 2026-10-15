# Windows regression check for the Android port: configures + builds the desktop dev configuration in a separate
# build directory and runs the CPU tests (ctest preset "cpu" equivalent: label gpu excluded) plus the Android-port
# unit tests. Uses the existing x64-windows vcpkg install (no manifest install, so it never contends for the vcpkg
# lock with an Android dependency build).
#   tools/android/hostcheck.ps1 [-BuildDir E:\Development\OxwaldEngine\build\android-hostcheck] [-Jobs 8] [-SkipTests]
param(
    [string]$BuildDir = "E:\Development\OxwaldEngine\build\android-hostcheck",
    [int]$Jobs = 8,
    [string]$Targets = "",
    [switch]$SkipTests,
    [switch]$KeepGoing,       # build everything that compiles (other teams' work in progress may not)
    [string]$TestRegex = ""
)
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$vcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { "C:\Programming\vcpkg" }

# Import the MSVC developer environment into this PowerShell process.
cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
    if ($_ -match "^([^=]+)=(.*)$") { Set-Item "env:$($Matches[1])" $Matches[2] }
}

if (-not (Test-Path (Join-Path $BuildDir "CMakeCache.txt"))) {
    cmake -S $repo -B $BuildDir -G Ninja `
        "-DCMAKE_TOOLCHAIN_FILE=$vcpkgRoot\scripts\buildsystems\vcpkg.cmake" `
        "-DVCPKG_INSTALLED_DIR=$repo\vcpkg_installed" -DVCPKG_MANIFEST_INSTALL=OFF `
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOX_WARNINGS_AS_ERRORS=ON
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }
}
$buildArgs = @("--build", $BuildDir, "-j", "$Jobs")
if ($Targets) { $buildArgs += @("--target") + $Targets.Split(",") }
if ($KeepGoing) { $buildArgs += @("--", "-k", "0") }
cmake @buildArgs
if ($LASTEXITCODE -ne 0 -and -not $KeepGoing) { throw "build failed" }
if ($SkipTests) { exit 0 }
$ctestArgs = @("--test-dir", $BuildDir, "-LE", "gpu|android", "--output-on-failure", "-j", "$Jobs", "--timeout", "300")
if ($TestRegex) { $ctestArgs += @("-R", $TestRegex) }
ctest @ctestArgs
exit $LASTEXITCODE
