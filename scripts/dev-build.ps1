# Incremental developer build: recompiles only what changed since the last build and copies the result into a
# game folder, in minutes instead of the Builder's ~40. It reuses the lifted code and object cache of an earlier
# full build (the Hookshot Builder's work folder, %LOCALAPPDATA%\HookshotBuilder, or your own --work folder).
#
#   powershell -File scripts/dev-build.ps1 -Game E:\OOT\hookshot-fork-game
#
# Changing nx2/ (the lifter) or the game-code patches needs a full build (nx2/make_game.py); host/ and gpu/ edits
# are what this is for.
param(
    [string]$Game = "$PSScriptRoot\..\work\game",
    [string]$Work = "$env:LOCALAPPDATA\HookshotBuilder",
    [string]$Tools = "",      # the HookshotBuilder "tools" folder (python, llvm-mingw, cpm, wgpu-native); found beside Work when empty
    [int]$Jobs = 8
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
$env:PYTHONIOENCODING = "utf-8"
Push-Location $root
try {
    & "$Tools\python\python.exe" -I nx2/build.py --standalone --jobs $Jobs
    if ($LASTEXITCODE) { throw "build failed" }
    $built = Join-Path $Work "sa\build"
    foreach ($n in "hookshot.exe", "hookshot_vk.dll") {
        Copy-Item "$built\$n" "$Game\$n" -Force
        Write-Host "copied $n"
    }
} finally { Pop-Location }
