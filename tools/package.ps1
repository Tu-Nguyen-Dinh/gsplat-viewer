# Builds a Release and packages a self-contained Windows zip:
#   dist/gsplat-viewer-<version>-win64.zip
#     gsplat_viewer.exe  shaders/  README.md  LICENSE  THIRD_PARTY.md
# Then smoke-tests the unpacked copy from a clean folder (no MSYS2 on PATH, no source tree shaders).
#
# Usage (from the repo root):  powershell -ExecutionPolicy Bypass -File tools\package.ps1
param([string]$MinGW = "C:\msys64\mingw64\bin")

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$env:PATH = "$MinGW;$env:PATH"

function Invoke-Step([string]$what, [scriptblock]$cmd) {
  & $cmd
  if ($LASTEXITCODE -ne 0) { Write-Error "$what failed (exit $LASTEXITCODE)"; exit 1 }
}

# 1. Build + tests (CPU and GPU) so a broken build never gets packaged.
Invoke-Step "configure" { cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_UPDATES_DISCONNECTED=ON | Out-Null }
Invoke-Step "build" { cmake --build build -j 8 | Out-Null }
Invoke-Step "tests" { .\build\gsplat_tests.exe | Select-Object -Last 1 }

$version = (Select-String -Path CMakeLists.txt -Pattern 'project\(gsplat_viewer VERSION ([0-9.]+)').Matches[0].Groups[1].Value
$name = "gsplat-viewer-$version-win64"
$stage = Join-Path $root "dist\$name"
$zip = Join-Path $root "dist\$name.zip"

# 2. Stage the files.
Remove-Item -Recurse -Force $stage, $zip -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item build\gsplat_viewer.exe, README.md, LICENSE, THIRD_PARTY.md $stage
Copy-Item -Recurse shaders (Join-Path $stage "shaders")
Compress-Archive -Path "$stage\*" -DestinationPath $zip

# 3. Smoke test: unzip into a fresh folder and run with a minimal PATH.
$test = Join-Path $env:TEMP "gsv-package-test"
Remove-Item -Recurse -Force $test -ErrorAction SilentlyContinue
Expand-Archive $zip $test
$savedPath = $env:PATH
$env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
Push-Location $test
& .\gsplat_viewer.exe --gen smoke.ply 20000 | Out-Null
$gen = $LASTEXITCODE
& .\gsplat_viewer.exe smoke.ply --size 640x360 --screenshot smoke.png | Out-Null
$shot = $LASTEXITCODE
Pop-Location
$env:PATH = $savedPath

$ok = ($gen -eq 0) -and ($shot -eq 0) -and (Test-Path (Join-Path $test "smoke.png")) -and ((Get-Item (Join-Path $test "smoke.png")).Length -gt 10000)
$sizeMB = (Get-Item $zip).Length / 1MB
"{0}  {1:N2} MB  smoke test: {2}" -f $zip, $sizeMB, ($(if ($ok) { "PASS" } else { "FAIL" }))
Remove-Item -Recurse -Force $test
if (-not $ok) { exit 1 }
if ($sizeMB -ge 10) { Write-Error "zip is larger than 10 MB"; exit 1 }
exit 0
