# A test run of the standalone build (mkw-recomp's work/run-vk.ps1, for hookshot.exe): the Vulkan back end
# (RX_BACKEND=vulkan), headless unless -Window (then it never takes focus), High priority (-Priority), muted unless
# -Sound, screenshots every -ShotEvery presents into work\runs\<name>\shots. Extra environment: -Env "A=1;B=2".
#
# Guards (the PC is the user's, and one game runs at a time): no run while Rainbow Six Siege is open, nor while
# another session's mkw.exe or hookshot.exe runs (-Force overrides both); the game is stopped when its working set passes
# -MaxGB. Delete work\runs\<name>\shots once the shots are looked at: they reach gigabytes.
param([int]$Seconds = 120, [string]$Name = "vk", [int]$ShotEvery = 300, [string]$Env = "", [switch]$Window,
      [int]$MaxGB = 10, [switch]$Force, [switch]$Sound, [string]$Romfs = "", [string]$Priority = "High")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Force) {
    if (Get-Process RainbowSix, RainbowSix_BE, RainbowSix_Vulkan -ErrorAction SilentlyContinue) {
        "a game is running (Rainbow Six): no test run now"; exit 2
    }
    $other = Get-Process mkw, hookshot -ErrorAction SilentlyContinue
    if ($other) { "another game run is going ($(($other | ForEach-Object { "$($_.Name).exe $($_.Id)" }) -join ', ')): one at a time"; exit 2 }
}
if (-not $Romfs) {   # (the dump's romfs, from work\dump.txt: nx2/dump.py --set)
    $cfg = Join-Path $root "work\dump.txt"
    if (-not (Test-Path $cfg)) { "no -Romfs and no work\dump.txt (python -I nx2/dump.py --dump <folder> --set)"; exit 2 }
    $dump = (Get-Content $cfg | Where-Object { $_ -and -not $_.StartsWith("#") } | Select-Object -First 1).Trim()
    $Romfs = Join-Path $dump "romfs"
}
$exe = Join-Path $root "work\sa\build\hookshot.exe"
$image = Join-Path $root "work\sa\image\image.bin"
$user = Join-Path $root "work\sa\user"
$out = Join-Path $root "work\runs\$Name"
$shots = Join-Path $out "shots"
New-Item -ItemType Directory -Force $out, $user, $shots | Out-Null
$env:RX_BACKEND = "vulkan"
if ($Window) { Remove-Item Env:HOS_HEADLESS -ErrorAction SilentlyContinue; $env:HOS_WINDOW_NOACTIVATE = "1" } else { $env:HOS_HEADLESS = "1" }
$env:RX_VK_SHOTS = $shots
$env:RX_VK_SHOT_EVERY = "$ShotEvery"
# (test runs are silent: the user is often in a call; -Sound plays it)
if ($Sound) { Remove-Item Env:HOS_AUDIO -ErrorAction SilentlyContinue } else { $env:HOS_AUDIO = "0" }
foreach ($kv in ($Env -split ";")) { if ($kv -match "^([^=]+)=(.*)$") { Set-Item -Path ("Env:" + $matches[1]) -Value $matches[2] } }
$t0 = Get-Date
$p = Start-Process -FilePath $exe -ArgumentList @("`"$image`"", "--romfs", "`"$Romfs`"", "--user", "`"$user`"") `
    -WorkingDirectory $out -PassThru -NoNewWindow `
    -RedirectStandardOutput (Join-Path $out "stdout.txt") -RedirectStandardError (Join-Path $out "log.txt")
try { $p.PriorityClass = $Priority } catch {}   # (High by default: the user, 2026-11-04; -Priority BelowNormal to yield)
$peak = 0
$exited = $false
for ($t = 0; $t -lt $Seconds; $t += 2) {
    if ($p.WaitForExit(2000)) { $exited = $true; break }
    try { $p.Refresh(); $mb = [int]($p.WorkingSet64 / 1MB) } catch { $mb = 0 }
    if ($mb -gt $peak) { $peak = $mb }
    if ($mb -gt $MaxGB * 1024) { "memory guard: $mb MB, stopped"; break }
}
if (-not $exited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
"peak working set: $peak MB"
$secs = [int]((Get-Date) - $t0).TotalSeconds
"run '$Name': $secs s, " + $(if ($exited) { "exited with $($p.ExitCode)" } else { "stopped after $Seconds s" })
