# Launch an opencode worker on a ticket, in its own git worktree and branch.
# Usage: powershell -File tools/run-worker.ps1 -Ticket TICK-001 -Slug bench-harness [-Model deepseek/deepseek-v4.1-flash]
param(
  [Parameter(Mandatory)][string]$Ticket,
  [Parameter(Mandatory)][string]$Slug,
  [string]$Model = "opencode-go/deepseek-v4.1-flash"   # provider/model (OpenCode Go subscription)
)
$ErrorActionPreference = "Stop"
# Workers use their own opencode data folder: the default one (~/.local/share/opencode)
# was written by a newer dev build and opencode 1.19.x refuses to open it.
if (-not $env:XDG_DATA_HOME) { $env:XDG_DATA_HOME = "E:\OOT\oc-data" }
$root = git rev-parse --show-toplevel
$branch = "feature/$Ticket-$Slug"
$wt = Join-Path (Split-Path $root) "HeyListen-wt\$Ticket"
if (-not (Test-Path $wt)) { git -C $root worktree add -b $branch $wt main }
$spec = Get-Content (Join-Path $root "docs\tickets\$Ticket.md") -Raw
Push-Location $wt
try {
  opencode run --model $Model "You work on branch $branch only. Implement this ticket, run its tests, commit with message '${Ticket}: <summary>'. Do not push. Ticket:`n$spec"
} finally { Pop-Location }
