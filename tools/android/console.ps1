# Remote engine console over `adb forward` (debug APKs listen on device port 27015; release: --remote-console N).
#   tools/android/console.ps1                       # interactive: type commands / cvars, Ctrl+C quits
#   tools/android/console.ps1 -Command "stats","r.ScreenPercentage 70"   # run commands, print the replies, exit
# Log lines at Info+ are streamed as "# [level] [category] message".
param([int]$Port = 27015, [string[]]$Command = @(), [int]$WaitMs = 1500)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\env.ps1"
& $Adb forward "tcp:$Port" "tcp:$Port" | Out-Null
$client = New-Object System.Net.Sockets.TcpClient
$client.Connect("127.0.0.1", $Port)
$stream = $client.GetStream()
$reader = New-Object System.IO.StreamReader($stream, [Text.Encoding]::UTF8)
$writer = New-Object System.IO.StreamWriter($stream, [Text.Encoding]::UTF8)
$writer.AutoFlush = $true
$buffer = New-Object char[] 8192

function Read-Available([int]$ms) {
    $deadline = (Get-Date).AddMilliseconds($ms)
    $text = ""
    while ((Get-Date) -lt $deadline) {
        if ($stream.DataAvailable) {
            $n = $reader.Read($buffer, 0, $buffer.Length)
            if ($n -le 0) { break }
            $text += -join $buffer[0..($n - 1)]
        } else { Start-Sleep -Milliseconds 30 }
    }
    return $text
}

if ($Command.Count -gt 0) {
    Read-Available 500 | Out-Null # banner
    foreach ($c in $Command) {
        $writer.WriteLine($c)
        Write-Host "> $c"
        Write-Host (Read-Available $WaitMs).TrimEnd("> ").TrimEnd()
    }
    $client.Close()
    exit 0
}
Write-Host (Read-Available 500) -NoNewline
while ($client.Connected) {
    $line = Read-Host
    $writer.WriteLine($line)
    Write-Host (Read-Available $WaitMs) -NoNewline
}
