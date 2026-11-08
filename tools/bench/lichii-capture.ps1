# Records Lichii's frame times with PresentMon while you play.
# Run in an ADMINISTRATOR PowerShell, with lichii.exe already running and Link in the scene:
#   powershell -ExecutionPolicy Bypass -File E:\OOT\HeyListen\tools\bench\lichii-capture.ps1 -Scene kokiri
# Walk around for the whole capture (default 60 s). Output: build\bench\lichii-<scene>\presentmon.csv
param(
  [Parameter(Mandatory)][string]$Scene,
  [int]$Seconds = 60,
  [string]$PresentMon = "E:\OOT\tools\PresentMon\PresentMon.exe"
)
$ErrorActionPreference = "Stop"
if (-not (Get-Process lichii -ErrorAction SilentlyContinue)) { throw "lichii.exe is not running: start it and load the scene first." }
$out = Join-Path $PSScriptRoot "..\..\build\bench\lichii-$Scene"
New-Item -ItemType Directory -Force $out | Out-Null
$csv = Join-Path $out "presentmon.csv"
Write-Host "Recording lichii.exe for $Seconds s in 5 s: move Link now..."
Start-Sleep 5
& $PresentMon --process_name lichii.exe --output_file $csv --timed $Seconds --terminate_after_timed --stop_existing_session --no_console_stats
Write-Host "Done: $csv"
