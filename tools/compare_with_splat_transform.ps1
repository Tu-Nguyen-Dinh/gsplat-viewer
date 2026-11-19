# Renders the same view with gsplat-viewer and with PlayCanvas splat-transform
# (an independent WebGPU renderer) and prints the PSNR between the two images.
#
# Requirements: Node.js >= 22 (npx), ffmpeg on PATH, a Release build in ..\build.
# Usage:  .\tools\compare_with_splat_transform.ps1 [-Scene demo_small.ply] [-OutDir $env:TEMP\gsv-compare]
#
# Coordinate conventions: splat-transform treats PLY data as COLMAP-style (Y down)
# and rotates it 180 degrees about Z. To render the same view we pass it the camera
# rotated the same way: (x, y, z) -> (-x, -y, z), with up = (0, -1, 0).
param(
  [string]$Scene = "",
  [string]$OutDir = (Join-Path $env:TEMP "gsv-compare"),
  [string]$Eye = "0,1.5,6",
  [string]$Target = "0,0.6,0",
  [string]$Size = "640x360"
)
# Native tools (npx, ffmpeg) write progress to stderr; Windows PowerShell 5.1 would turn that
# into errors with $ErrorActionPreference = "Stop", so exit codes are checked explicitly instead.
function Assert-Ok([string]$what) { if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)" } }
$viewer = Join-Path $PSScriptRoot "..\build\gsplat_viewer.exe"
New-Item -ItemType Directory -Force $OutDir | Out-Null
if (-not $Scene) {
  $Scene = Join-Path $OutDir "demo_small.ply"
  & $viewer --gen $Scene 30000 | Out-Null
}

function Rotate-Z180([string]$v) {
  $p = $v.Split(",") | ForEach-Object { [double]$_ }
  return "{0},{1},{2}" -f (-$p[0]), (-$p[1]), $p[2]
}

$ours = Join-Path $OutDir "gsplat_viewer.png"
$refWebp = Join-Path $OutDir "splat_transform.webp"
$refPng = Join-Path $OutDir "splat_transform.png"

& $viewer $Scene --size $Size --camera-pos $Eye --camera-target $Target --background 0,0,0 --screenshot $ours
Assert-Ok "gsplat_viewer"
cmd /c "npx --yes @playcanvas/splat-transform@3.11.2 -w `"$Scene`" `"$refWebp`" --camera-pos $(Rotate-Z180 $Eye) --camera-target $(Rotate-Z180 $Target) --camera-up 0,-1,0 --resolution $Size --camera-fov 55 2>&1"
Assert-Ok "splat-transform"
ffmpeg -loglevel error -y -i $refWebp $refPng
Assert-Ok "ffmpeg (decode)"
$line = cmd /c "ffmpeg -hide_banner -i `"$ours`" -i `"$refPng`" -lavfi psnr -f null - 2>&1" |
  Select-String -CaseSensitive "PSNR r:" | Select-Object -First 1
if ($line -match "average:([0-9.]+)") { Write-Output ("gsplat-viewer vs splat-transform: PSNR {0:N2} dB" -f [double]$Matches[1]) }
Write-Output "Images: $OutDir"
exit 0
