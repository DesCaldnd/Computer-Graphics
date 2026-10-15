# Starts OxwaldPlayer on the connected device / emulator with player arguments (intent extra "args").
#   tools/android/run.ps1                                   # runs until closed
#   tools/android/run.ps1 -PlayerArgs "--frames 600 --stats"      # any desktop player option
#   tools/android/run.ps1 -Validation 1 -Logcat             # Vulkan validation on, follow the filtered log
#   tools/android/run.ps1 -WaitDebugger                     # blocks in android_main until lldb attaches
param(
    [string]$PlayerArgs = "",
    [string]$Validation = "",   # "1" / "0"; empty = build default (on in debug builds)
    [switch]$WaitDebugger,
    [switch]$Logcat,
    [switch]$NoStop
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\env.ps1"
$cmd = "am start -W -n $OxActivity"
if (-not $NoStop) { $cmd = "am start -S -W -n $OxActivity" }
if ($PlayerArgs) { $cmd += " --es args '" + $PlayerArgs.Replace("'", "") + "'" }
if ($Validation) { $cmd += " --es validation $Validation" }
if ($WaitDebugger) { $cmd += " --ez wait_debugger true" }
if ($Logcat) { & $Adb logcat -c }
& $Adb shell $cmd
if ($Logcat) { & "$PSScriptRoot\logcat.ps1" -Follow }
