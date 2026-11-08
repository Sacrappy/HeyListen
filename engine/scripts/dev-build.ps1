# Incremental developer build: recompiles only what changed since the last build and copies the result into a
# game folder, in minutes instead of the Builder's ~40. It reuses the lifted code and object cache of an earlier
# full build (the Hookshot Builder's work folder, %LOCALAPPDATA%\HookshotBuilder, or your own --work folder).
#
#   powershell -File scripts/dev-build.ps1 -Game E:\OOT\hookshot-fork-game
#
# -Lift lifts the game again first (a change to nx2/ or a merge that brings one); host/ and gpu/ edits need no lift.
param(
    [string]$Game = "$PSScriptRoot\..\work\game",
    [string]$Work = "$env:LOCALAPPDATA\HookshotBuilder",
    [string]$Tools = "",      # the HookshotBuilder "tools" folder (python, llvm-mingw, cpm, wgpu-native); found beside Work when empty
    [int]$Jobs = 8,
    [switch]$Lift             # lift the game again first (after a change to nx2/'s lifter or the game-code patches, or a merge that brings one):
                              # a few minutes, then only the chunks whose lifted code changed are compiled
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..").Path
if (-not $Tools) {
    $cur = Join-Path $Work "versions\current.txt"
    if (Test-Path $cur) {
        $tag = (Get-Content $cur -Raw).Trim()
        $Tools = Join-Path $Work "versions\$tag\HookshotBuilder\tools"
    }
}
if (-not (Test-Path "$Tools\python\python.exe")) { throw "Tools folder not found: pass -Tools <HookshotBuilder\tools>" }
$env:RX_CPM = "$Tools\cpm"
$env:WGPU_NATIVE = "$Tools\wgpu-native"
$env:HOOKSHOT_MINGW = "$Tools\llvm-mingw"
$env:OOT_WORK = $Work
$env:OOT_EXEFS = "$Work\exefs"
$env:OOT_ROMFS = "$Game\romfs"
$env:RX_RELEASE = "1"
$env:A64_HOT_CHUNK = "1"
if (Test-Path "$Work\hot_funcs.txt") { $env:A64_HOT_FUNCS = "$Work\hot_funcs.txt" }
if (Test-Path "$Work\hookshot_pieces.prof") { $env:RX_PGO = "$Work\hookshot_pieces.prof" }
$env:PYTHONIOENCODING = "utf-8"
Push-Location $root
try {
    if ($Lift) {
        & "$Tools\python\python.exe" -I nx2/lift.py --standalone --write --exefs $env:OOT_EXEFS --work $Work
        if ($LASTEXITCODE) { throw "lift failed" }
        if (Test-Path "$Work\sa\gen\code_hot.c") { & "$Tools\python\python.exe" -I nx2/split_hot.py "$Work\sa\gen" }
    }
    & "$Tools\python\python.exe" -I nx2/build.py --standalone --jobs $Jobs
    if ($LASTEXITCODE) { throw "build failed" }
    $built = Join-Path $Work "sa\build"
    foreach ($n in "hookshot.exe", "hookshot_vk.dll") {
        Copy-Item "$built\$n" "$Game\$n" -Force
        Write-Host "copied $n"
    }
} finally { Pop-Location }
