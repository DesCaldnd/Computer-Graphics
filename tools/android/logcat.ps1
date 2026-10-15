# Filtered logcat for OxwaldPlayer: engine log (Oxwald), stdout/stderr (OxwaldStdout), crash handler
# (OxwaldCrash), Vulkan loader/driver/validation (vulkan, VALIDATION), debuggerd tombstones (DEBUG), Java crashes.
#   tools/android/logcat.ps1               # dump what is buffered
#   tools/android/logcat.ps1 -Follow       # stream
#   tools/android/logcat.ps1 -App          # only the app process (--pid)
#   tools/android/logcat.ps1 -Level W      # warnings and up
#   tools/android/logcat.ps1 -Out log.txt  # also save
param([switch]$Follow, [switch]$App, [ValidateSet("V", "D", "I", "W", "E", "F")] [string]$Level = "V", [string]$Out = "", [switch]$Clear)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\env.ps1"
if ($Clear) { & $Adb logcat -c; exit 0 }
$filters = @("Oxwald:$Level", "OxwaldStdout:$Level", "OxwaldCrash:V", "vulkan:$Level", "VALIDATION:$Level", "DEBUG:V",
             "AndroidRuntime:E", "libc:F", "GameActivity:W", "*:S")
$logArgs = @("logcat", "-v", "threadtime")
if (-not $Follow) { $logArgs += "-d" }
if ($App) {
    $appPid = Get-AppPid
    if ($appPid) { $logArgs += "--pid=$appPid" } else { Write-Warning "$OxPackage is not running" }
}
$logArgs += $filters
if ($Out) { & $Adb @logArgs | Tee-Object -FilePath $Out } else { & $Adb @logArgs }
