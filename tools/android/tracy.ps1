# Connects the Tracy profiler (desktop Tracy GUI, protocol matching the vcpkg tracy port) to OxwaldPlayer on the
# device: Tracy's client listens on port 8086 inside the app (debug/RelWithDebInfo builds, OX_ENABLE_TRACY=ON).
# After the forward, connect the Tracy GUI to 127.0.0.1:8086. Discovery broadcasts do not cross adb, so use the
# address field instead of the client list.
param([int]$Port = 8086)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\env.ps1"
& $Adb forward "tcp:$Port" "tcp:$Port"
Write-Host "Tracy: connect the profiler to 127.0.0.1:$Port (adb forward tcp:$Port -> device tcp:$Port)"
& $Adb forward --list
