# One command from phone video to a viewable 3D scene:
#   video -> gsplat_prep (sharp frames) -> COLMAP (poses) -> Brush (training) -> gsplat_viewer
#
#   powershell -ExecutionPolicy Bypass -File tools\capture.ps1 scenes\mug\mug.mp4 [-Steps 30000] [-NoViewer]
#
# Works in the video's folder. Each step stops the pipeline with a clear message if it fails;
# timings go to <scene>/capture.json.
param(
  [Parameter(Mandatory = $true, Position = 0)][string]$Video,
  [int]$Target = 200,
  [int]$Steps = 30000,
  [ValidateSet("sequential", "exhaustive")][string]$Matcher = "sequential",
  [switch]$NoViewer
)
. "$PSScriptRoot\common.ps1"

$root = Split-Path -Parent $PSScriptRoot
$Video = (Resolve-Path $Video).Path
$scene = Split-Path -Parent $Video
$name = [IO.Path]::GetFileNameWithoutExtension($Video)
$prep = Join-Path $root "build\gsplat_prep.exe"
$viewer = Join-Path $root "build\gsplat_viewer.exe"
foreach ($exe in $prep, $viewer) { if (-not (Test-Path $exe)) { Write-Error "Missing $exe (build the project first)"; exit 1 } }

$times = [ordered]@{}
$total = [Diagnostics.Stopwatch]::StartNew()
function Invoke-Stage([string]$Label, [scriptblock]$Body) {
  Write-Host "`n#### $Label" -ForegroundColor Yellow
  $t = [Diagnostics.Stopwatch]::StartNew()
  & $Body
  $code = $LASTEXITCODE
  $times[$Label] = [math]::Round($t.Elapsed.TotalSeconds, 1)
  if ($code -ne 0) { Write-Error "$Label failed (exit $code). Stopping."; exit 1 }
}

Invoke-Stage "1. frames (gsplat_prep)" { & $prep $Video --out $scene --target $Target }
Invoke-Stage "2. poses (COLMAP)" { & powershell -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot\reconstruct.ps1" -Scene $scene -Matcher $Matcher }
Invoke-Stage "3. training (Brush)" { & powershell -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot\train.ps1" -Scene $scene -Steps $Steps -Name $name }

$summary = [ordered]@{ video = $Video; seconds = $times; total_minutes = [math]::Round($total.Elapsed.TotalMinutes, 1) }
$summary | ConvertTo-Json -Depth 3 | Set-Content -Encoding utf8 (Join-Path $scene "capture.json")
Write-Host ("`nDone in {0:N1} min: {1}" -f $total.Elapsed.TotalMinutes, (Join-Path $scene "$name.ply")) -ForegroundColor Green

if (-not $NoViewer) { & $viewer (Join-Path $scene "$name.ply") --flip-y }
exit 0
