# Phase 6 demo: build, run the tests, then open the viewer on the "phong" scene with the editing tools.
# Started by double-clicking tools\phase6_demo.bat. Everything is also written to build\phase6_demo.log.
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$env:PATH = "C:\msys64\mingw64\bin;" + $env:PATH
$log = Join-Path $root "build\phase6_demo.log"
New-Item -ItemType Directory -Force -Path (Join-Path $root "build") | Out-Null
"=== Phase 6 demo $(Get-Date -Format s) ===" | Out-File -FilePath $log -Encoding utf8

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
if (-not $fail -and (Step "Build" { cmake --build build -j 8 }) -ne 0) { $fail = $true }
if (-not $fail) {
  Step "CPU tests" { .\build\gsplat_tests.exe --cpu-only } | Out-Null
  Step "GPU tests (RTX 3060)" { .\build\gsplat_tests.exe --gpu-only } | Out-Null
  Step "C API test" { .\build\gsplat_capi_test.exe build } | Out-Null
  Write-Host ""
  Write-Host "=== Opening the viewer ===" -ForegroundColor Green
  Write-Host "  - Menu bar: File / Edit / View / Help. View > Language > Tieng Viet. F1 = all shortcuts"
  Write-Host "  - File > Export: picture, video along saved views (needs 2 saved views), HTML report"
  Write-Host "  - Try: delete something, then close the window -> Save / Don't save / Cancel"
  Write-Host "  - Compass top right (click an axis), Num 1/3/7 quick views, Shift+R frames the selection"
  Start-Process -FilePath ".\build\gsplat_viewer.exe" -ArgumentList @(
    "scenes\phong\phong.ply", "--las", "scenes\phong\phong_points.las", "--las-transform", "1,1,0,0,0,0,0,0")
} else {
  Write-Host "Build failed: see build\phase6_demo.log" -ForegroundColor Red
}
"=== done ===" | Out-File -FilePath $log -Append -Encoding utf8
Write-Host ""
Read-Host "Press Enter to close this window"
