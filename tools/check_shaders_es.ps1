# Checks that every shader compiles as GLSL ES 3.10 (the Android build) with the reference
# compiler shipped in the NDK, without a phone. CI runs the same check (job "android").
#
#   powershell -ExecutionPolicy Bypass -File tools\check_shaders_es.ps1 [-AndroidRoot F:\tools\android]
param(
  [string]$AndroidRoot = $(if ($env:GSV_ANDROID_ROOT) { $env:GSV_ANDROID_ROOT } else { "F:\tools\android" }),
  [string]$NdkVersion = "27.3.13750724"
)
$root = Split-Path -Parent $PSScriptRoot
$glslc = Join-Path $AndroidRoot "sdk\ndk\$NdkVersion\shader-tools\windows-x86_64\glslc.exe"
$dump = Join-Path $root "build\gsplat_shaderdump.exe"
foreach ($p in $glslc, $dump) { if (-not (Test-Path $p)) { throw "Missing $p (build the desktop project / install the NDK)" } }
$out = Join-Path $env:TEMP "gsv-es-shaders"
& $dump $out --es | Out-Null
if ($LASTEXITCODE -ne 0) { throw "gsplat_shaderdump failed" }
$failed = 0
foreach ($f in Get-ChildItem $out -File) {
  $msg = & cmd /c "`"$glslc`" --target-env=opengl -std=310es -fauto-map-locations -fauto-bind-uniforms -o NUL `"$($f.FullName)`" 2>&1"
  if ($LASTEXITCODE -eq 0) { Write-Output "ok    $($f.Name)" } else { $failed++; Write-Output "FAIL  $($f.Name)"; $msg | Write-Output }
}
if ($failed) { exit 1 }
Write-Output "All shaders compile as GLSL ES 3.10."
