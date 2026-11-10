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
# (HeyListen TICK-045) The play profile recorded on this lifter's default lift (the A64_VFPX_ELEM / A64_FPX_SLOWPATH / A64_FPX_FMA3
# switches on), kept in the repository's profile/ folder: copied into the work folder when it differs, so a dev build never pairs
# the lift with another profile (an installed builder copies its own tools\work profile there when it builds; a lift without its
# profile loses ~4-5 fps, TICK-040/041)
$profileDir = Join-Path (Split-Path $root -Parent) "profile"
if (Test-Path "$profileDir\hookshot_pieces.prof") {
    foreach ($n in "hot_funcs.txt", "hot_order.txt", "hookshot_pieces.prof") {
        $src = Join-Path $profileDir $n
        $dst = Join-Path $Work $n
        if ((Test-Path $src) -and (-not (Test-Path $dst) -or (Get-FileHash $src).Hash -ne (Get-FileHash $dst).Hash)) {
            Copy-Item $src $dst -Force
            Write-Host "profile: $n from $profileDir"
        }
    }
}
if (Test-Path "$Work\hot_funcs.txt") { $env:A64_HOT_FUNCS = "$Work\hot_funcs.txt" }
if (Test-Path "$Work\hookshot_pieces.prof") { $env:RX_PGO = "$Work\hookshot_pieces.prof" }
$env:PYTHONIOENCODING = "utf-8"
Push-Location $root
try {
    if ($Lift) {
        # (TICK-040, as make_game.py: an earlier lift's C goes first. A lift that writes fewer chunks (a longer hot list ends
        # fewer of them) left the earlier lift's last chunks beside the new ones, and the link failed on duplicate symbols.
        # The objects stay cached by their contents)
        Get-ChildItem "$Work\sa\gen" -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -like "code_*.c" -or $_.Name -like "code_*.c.whole" } | Remove-Item -Force
        & "$Tools\python\python.exe" -I nx2/lift.py --standalone --write --exefs $env:OOT_EXEFS --work $Work
        if ($LASTEXITCODE) { throw "lift failed" }
        if (Test-Path "$Work\sa\gen\code_hot.c") { & "$Tools\python\python.exe" -I nx2/split_hot.py "$Work\sa\gen" }
    }
    & "$Tools\python\python.exe" -I nx2/build.py --standalone --jobs $Jobs
    if ($LASTEXITCODE) { throw "build failed" }
    $built = Join-Path $Work "sa\build"
    # (HeyListen.exe since TICK-034. Into bin\ when the folder has the launcher layout, else beside image.bin. An older hookshot.exe
    # in the same place is removed, so the folder never has two games; it cannot be while it runs: close it first)
    $dest = if ((Test-Path "$Game\bin\HeyListen.exe") -or (Test-Path "$Game\bin\hookshot.exe")) { "$Game\bin" } else { $Game }
    if (Get-Process hookshot, HeyListen -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($Game, [StringComparison]::OrdinalIgnoreCase) }) {
        throw "the game in $Game is running: close it first"
    }
    foreach ($n in "HeyListen.exe", "hookshot_vk.dll") {
        Copy-Item "$built\$n" "$dest\$n" -Force
        Write-Host "copied $n"
    }
    foreach ($old in "hookshot.exe", "hookshot.map") {
        if (Test-Path "$dest\$old") { Remove-Item "$dest\$old" -Force; Write-Host "removed the old $old (the game is HeyListen.exe now)" }
    }
    if ($dest -ne $Game) {   # (launcher layout: the old launcher hookshot.exe beside it; the new one comes with a builder build)
        if ((Test-Path "$Game\hookshot.exe") -and -not (Test-Path "$Game\HeyListen.exe")) {
            Copy-Item "$Game\hookshot.exe" "$Game\HeyListen.exe"; Remove-Item "$Game\hookshot.exe" -Force
            Write-Host "renamed the launcher to HeyListen.exe"
        }
    }
} finally { Pop-Location }
