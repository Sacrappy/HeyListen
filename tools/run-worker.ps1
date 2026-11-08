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
  opencode run --model $Model "You work alone and unattended in this git worktree, on branch $branch only. Nobody will answer questions. Implement the whole ticket below, then run its tests (install missing Python tools with 'python -m pip install --user <pkg>'), then 'git add' and commit with message '${Ticket}: <summary>'. Do not stop before the commit exists. Do not push. Folders under E:\OOT outside this worktree are read-only references: read them, never write to them, never copy GPL code from them. Never read or copy game data (.nup, romfs, generated files). Ticket:`n$spec"
} finally { Pop-Location }
