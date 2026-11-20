# Structure-from-Motion with COLMAP: recovers camera poses + a sparse point cloud from
# <Scene>/images (written by gsplat_prep), then undistorts the images for 3DGS training.
#
#   powershell -ExecutionPolicy Bypass -File tools\reconstruct.ps1 -Scene scenes\mug [-Matcher exhaustive]
#
# Output:
#   <Scene>/colmap/database.db        features + matches
#   <Scene>/colmap/sparse/N           raw models (COLMAP may split a sequence into several)
#   <Scene>/dataset/images, sparse/0  undistorted PINHOLE images + the largest model (Brush input)
#   <Scene>/reconstruction.json       summary: registered images, points, reprojection error, times
#   <Scene>/logs/colmap.log           full COLMAP output
param(
  [Parameter(Mandatory = $true)][string]$Scene,
  [ValidateSet("sequential", "exhaustive")][string]$Matcher = "sequential",
  [int]$MaxImageSize = 1600,  # undistorted image size used for training (longest side)
  [switch]$Keep               # reuse an existing database instead of starting over
)
. "$PSScriptRoot\common.ps1"

$Scene = (Resolve-Path $Scene).Path
$images = Join-Path $Scene "images"
$work = Join-Path $Scene "colmap"
$db = Join-Path $work "database.db"
$sparse = Join-Path $work "sparse"
$dataset = Join-Path $Scene "dataset"
$logDir = Join-Path $Scene "logs"
$log = Join-Path $logDir "colmap.log"

if (-not (Test-Path $Colmap)) { Write-Error "COLMAP not found at $Colmap (set GSV_COLMAP)"; exit 1 }
$imageCount = @(Get-ChildItem $images -File -Include *.jpg, *.jpeg, *.png -Recurse).Count
if ($imageCount -lt 3) { Write-Error "Need at least 3 images in $images (run gsplat_prep first)"; exit 1 }

if (-not $Keep) { Remove-Item -Recurse -Force $work, $dataset -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force $work, $sparse, $logDir | Out-Null
Set-Content -Encoding utf8 $log "COLMAP log $(Get-Date -Format s)"
$times = [ordered]@{}

function Invoke-Colmap([string]$Name, [string[]]$CmdArgs) {
  Write-Step $Name
  $t = [Diagnostics.Stopwatch]::StartNew()
  $code = Invoke-Tool $Colmap $CmdArgs $log
  $times[$Name] = [math]::Round($t.Elapsed.TotalSeconds, 1)
  Write-Host ("   {0} (exit {1})" -f (Format-Seconds $t.Elapsed.TotalSeconds), $code)
  if ($code -ne 0) { Write-Error "COLMAP $Name failed; see $log"; exit 1 }
}

# 1. SIFT features on the GPU. One shared camera: a phone video keeps the same lens and focal length.
#    OPENCV = fx, fy, cx, cy + radial/tangential distortion (phone lenses are not perfect pinholes).
Invoke-Colmap "feature_extractor" @("feature_extractor", "--database_path", $db, "--image_path", $images,
  "--ImageReader.single_camera", "1", "--ImageReader.camera_model", "OPENCV", "--FeatureExtraction.use_gpu", "1")

# 2. Matching. Video frames are ordered, so each frame only needs to be matched against its
#    neighbours (sequential, with quadratic overlap for some long-range pairs); exhaustive matching
#    compares every pair: slower (n^2) but more robust for short or erratic videos.
if ($Matcher -eq "sequential") {
  Invoke-Colmap "sequential_matcher" @("sequential_matcher", "--database_path", $db,
    "--SequentialMatching.overlap", "15", "--SequentialMatching.quadratic_overlap", "1", "--FeatureMatching.use_gpu", "1")
} else {
  Invoke-Colmap "exhaustive_matcher" @("exhaustive_matcher", "--database_path", $db, "--FeatureMatching.use_gpu", "1")
}

# 3. Incremental mapping: pose estimation, triangulation and bundle adjustment.
Invoke-Colmap "mapper" @("mapper", "--database_path", $db, "--image_path", $images, "--output_path", $sparse)

# COLMAP may produce several disconnected models: keep the one with the most registered images.
$best = $null; $bestStats = $null
foreach ($m in Get-ChildItem $sparse -Directory) {
  $statsLog = Join-Path $logDir "model_$($m.Name).txt"
  Set-Content -Encoding utf8 $statsLog ""
  [void](Invoke-Tool $Colmap @("model_analyzer", "--path", $m.FullName) $statsLog)
  $text = Get-Content -Raw $statsLog
  $reg = if ($text -match 'Registered images:\s*(\d+)') { [int]$Matches[1] } else { 0 }
  if ($null -eq $bestStats -or $reg -gt $bestStats.registered) {
    $best = $m
    $bestStats = [ordered]@{
      registered = $reg
      points = if ($text -match 'Points:\s*(\d+)') { [int]$Matches[1] } else { 0 }
      observations = if ($text -match 'Observations:\s*(\d+)') { [int]$Matches[1] } else { 0 }
      reprojection_error_px = if ($text -match 'Mean reprojection error:\s*([\d.]+)') { [double]$Matches[1] } else { -1 }
    }
  }
}
if ($null -eq $best -or $bestStats.registered -eq 0) { Write-Error "COLMAP could not register any image; see $log"; exit 1 }

# 4. Undistort to an ideal pinhole camera (what 3DGS rasterizers assume) and lay out the dataset
#    as <dataset>/images + <dataset>/sparse/0.
Invoke-Colmap "image_undistorter" @("image_undistorter", "--image_path", $images, "--input_path", $best.FullName,
  "--output_path", $dataset, "--output_type", "COLMAP", "--max_image_size", "$MaxImageSize")
$flat = Join-Path $dataset "sparse"
$model0 = Join-Path $flat "0"
New-Item -ItemType Directory -Force $model0 | Out-Null
Get-ChildItem $flat -File | Move-Item -Destination $model0 -Force

$ratio = $bestStats.registered / $imageCount
$summary = [ordered]@{
  images = $imageCount
  registered = $bestStats.registered
  registered_ratio = [math]::Round($ratio, 3)
  models = @(Get-ChildItem $sparse -Directory).Count
  points = $bestStats.points
  mean_track_length = if ($bestStats.points -gt 0) { [math]::Round($bestStats.observations / $bestStats.points, 2) } else { 0 }
  reprojection_error_px = $bestStats.reprojection_error_px
  matcher = $Matcher
  seconds = $times
  total_seconds = [math]::Round(($times.Values | Measure-Object -Sum).Sum, 1)
}
$summary | ConvertTo-Json -Depth 3 | Set-Content -Encoding utf8 (Join-Path $Scene "reconstruction.json")

Write-Step "Summary"
"   registered {0} / {1} images ({2:P0}), {3} models, {4:N0} points, reprojection error {5:N3} px, {6}" -f `
  $summary.registered, $imageCount, $ratio, $summary.models, $summary.points, $summary.reprojection_error_px, (Format-Seconds $summary.total_seconds)
if ($ratio -lt 0.9) { Write-Warning "Less than 90% of the images were registered. Try -Matcher exhaustive or re-shoot (see docs/CAPTURE.md)." }
if ($summary.reprojection_error_px -ge 1.0) { Write-Warning "Mean reprojection error >= 1 px: poses may be inaccurate." }
exit 0
