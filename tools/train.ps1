# Trains a 3D Gaussian Splatting model with Brush on <Scene>/dataset (written by reconstruct.ps1).
#
#   powershell -ExecutionPolicy Bypass -File tools\train.ps1 -Scene scenes\mug [-Steps 30000]
#
# Every 8th image is held out (never trained on) and used to measure PSNR / SSIM at the end, so
# the score says how well the model predicts views it has not seen.
# Output:
#   <Scene>/<name>.ply              the trained splats (standard 3DGS PLY, opens in gsplat_viewer)
#   <Scene>/brush/eval_<steps>/     Brush's renders of the held-out views
#   <Scene>/training.json           steps, time, PSNR, SSIM, splat count
#   <Scene>/logs/brush.log
param(
  [Parameter(Mandatory = $true)][string]$Scene,
  [int]$Steps = 30000,
  [int]$EvalSplitEvery = 8,
  [string]$Name = ""
)
. "$PSScriptRoot\common.ps1"

$Scene = (Resolve-Path $Scene).Path
if (-not $Name) { $Name = Split-Path -Leaf $Scene }
$dataset = Join-Path $Scene "dataset"
$out = Join-Path $Scene "brush"
$logDir = Join-Path $Scene "logs"
$log = Join-Path $logDir "brush.log"
if (-not (Test-Path $Brush)) { Write-Error "Brush not found at $Brush (set GSV_BRUSH)"; exit 1 }
if (-not (Test-Path (Join-Path $dataset "sparse\0"))) { Write-Error "No COLMAP dataset in $dataset (run reconstruct.ps1)"; exit 1 }

Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out, $logDir | Out-Null
Set-Content -Encoding utf8 $log "Brush log $(Get-Date -Format s)"

Write-Step "Brush: $Steps steps (eval on every ${EvalSplitEvery}th image)"
$env:RUST_LOG = "brush_cli=info"  # progress + eval lines only
$t = [Diagnostics.Stopwatch]::StartNew()
$code = Invoke-Tool $Brush @($dataset, "--total-steps", "$Steps", "--eval-split-every", "$EvalSplitEvery",
  "--eval-every", "$Steps", "--eval-save-to-disk", "--export-every", "$Steps",
  "--export-path", $out, "--export-name", "$Name.ply") $log
$seconds = $t.Elapsed.TotalSeconds
Remove-Item Env:\RUST_LOG
if ($code -ne 0) { Write-Error "Brush failed (exit $code); see $log"; exit 1 }

$ply = Join-Path $out "$Name.ply"
if (-not (Test-Path $ply)) { Write-Error "Brush did not write $ply; see $log"; exit 1 }
Copy-Item $ply (Join-Path $Scene "$Name.ply") -Force

# "Eval iter 30000: PSNR 27.1, ssim 0.87"
$evalLine = Select-String -Path $log -Pattern 'Eval iter (\d+): PSNR ([\d.]+), ssim ([\d.]+)' | Select-Object -Last 1
$psnr = if ($evalLine) { [double]$evalLine.Matches[0].Groups[2].Value } else { $null }
$ssim = if ($evalLine) { [double]$evalLine.Matches[0].Groups[3].Value } else { $null }

# Splat count from the PLY header ("element vertex N").
$splats = 0
$reader = [IO.StreamReader]::new($ply)
while (($line = $reader.ReadLine()) -ne $null -and $line -ne "end_header") {
  if ($line -match '^element vertex (\d+)') { $splats = [int]$Matches[1] }
}
$reader.Close()

$summary = [ordered]@{
  steps = $Steps
  seconds = [math]::Round($seconds, 1)
  eval_split_every = $EvalSplitEvery
  eval_images = @(Get-ChildItem (Join-Path $out "eval_$Steps") -File -ErrorAction SilentlyContinue).Count
  brush_psnr = $psnr
  brush_ssim = $ssim
  splats = $splats
  ply_mb = [math]::Round((Get-Item $ply).Length / 1MB, 1)
}
$summary | ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $Scene "training.json")

Write-Step "Summary"
"   {0:N0} splats, {1}, held-out PSNR {2} dB, SSIM {3}" -f $splats, (Format-Seconds $seconds), $psnr, $ssim
exit 0
