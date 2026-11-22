# Phase 4 demo: build, run the tests, then open the viewer on the "phong" scene with a LAS layer.
# Started by double-clicking tools\phase4_demo.bat. Everything is also written to build\phase4_demo.log.
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$env:PATH = "C:\msys64\mingw64\bin;" + $env:PATH
$log = Join-Path $root "build\phase4_demo.log"
New-Item -ItemType Directory -Force -Path (Join-Path $root "build") | Out-Null
"=== Phase 4 demo $(Get-Date -Format s) ===" | Out-File -FilePath $log -Encoding utf8

function Step($title, [scriptblock]$cmd) {
  Write-Host ""
  Write-Host "=== $title ===" -ForegroundColor Cyan
  "=== $title ===" | Out-File -FilePath $log -Append -Encoding utf8
  & $cmd 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $log -Append | Out-Host
  $code = $LASTEXITCODE
  "exit code $code" | Out-File -FilePath $log -Append -Encoding utf8
  return $code
}

$fail = $false
if ((Step "Configure" { cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_UPDATES_DISCONNECTED=ON }) -ne 0) { $fail = $true }
if (-not $fail -and (Step "Build (a few minutes: everything is rebuilt as position-independent code)" { cmake --build build -j 8 }) -ne 0) { $fail = $true }
if (-not $fail) {
  Step "CPU tests" { .\build\gsplat_tests.exe --cpu-only } | Out-Null
  Step "GPU tests (RTX 3060)" { .\build\gsplat_tests.exe --gpu-only } | Out-Null
  Step "C API test" { .\build\gsplat_capi_test.exe build } | Out-Null
  Write-Host ""
  Write-Host "=== Opening the viewer ===" -ForegroundColor Green
  Write-Host "  - phong scene + LAS layer (19 231 opaque splat centres, aligned with --las-transform identity)"
  Write-Host "  - Panel 'Layers, measure, LOD': toggle points, color mode, LOD"
  Write-Host "  - Press M, click two points to measure; drag still orbits"
  Start-Process -FilePath ".\build\gsplat_viewer.exe" -ArgumentList @(
    "scenes\phong\phong.ply", "--las", "scenes\phong\phong_points.las", "--las-transform", "1,1,0,0,0,0,0,0")
} else {
  Write-Host "Build failed: see build\phase4_demo.log" -ForegroundColor Red
}
"=== done ===" | Out-File -FilePath $log -Append -Encoding utf8
Write-Host ""
Read-Host "Press Enter to close this window"
