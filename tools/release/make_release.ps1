# HeyListen release: a wrapper for make_release.py (the real steps and their notes are there).
#   powershell -File tools/release/make_release.ps1 -Tag v0.1.0 [-Out build\release] [-Dotnet C:\path\dotnet.exe] [-AllowDirty]
param(
    [string]$Tag = "",
    [string]$Out = "",
    [string]$Dotnet = "",
    [switch]$AllowDirty
)
$ErrorActionPreference = "Stop"
$py = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $py) { throw "Python 3 is needed (python.org)" }
$args_ = @("-I", "$PSScriptRoot\make_release.py")
if ($Tag) { $args_ += @("--tag", $Tag) }
if ($Out) { $args_ += @("--out", $Out) }
if ($Dotnet) { $args_ += @("--dotnet", $Dotnet) }
if ($AllowDirty) { $args_ += "--allow-dirty" }
& $py @args_
exit $LASTEXITCODE
