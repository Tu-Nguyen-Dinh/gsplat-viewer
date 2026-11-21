# Runs the CPU unit tests on a connected phone (USB debugging on) and installs the app.
#
#   powershell -ExecutionPolicy Bypass -File tools\test_android.ps1 [-AndroidRoot F:\tools\android] [-NoInstall]
#
# Needs tools\build_android.ps1 to have run (it builds build-android\android\gsplat_tests_cpu too).
param(
  [string]$AndroidRoot = $(if ($env:GSV_ANDROID_ROOT) { $env:GSV_ANDROID_ROOT } else { "F:\tools\android" }),
  [switch]$NoInstall
)
$root = Split-Path -Parent $PSScriptRoot
$adb = Join-Path $AndroidRoot "sdk\platform-tools\adb.exe"
$devices = & $adb devices | Select-String "\tdevice$"
if (-not $devices) { throw "No phone found: connect it with USB and allow USB debugging (see docs/ANDROID.md)" }
# The test runner of the device's ABI (arm64-v8a on phones, x86_64 on the PC emulator).
$abi = (& $adb shell getprop ro.product.cpu.abi).Trim()
$tests = Join-Path $root "build-android\$abi\android\gsplat_tests_cpu"
if (-not (Test-Path $tests)) { throw "Missing ${tests} - run tools\build_android.ps1 first" }
Write-Output ("Device: " + (& $adb shell getprop ro.product.model) + ", Android " + (& $adb shell getprop ro.build.version.release))

$remote = "/data/local/tmp/gsplat"
& $adb shell "rm -rf $remote && mkdir -p $remote/tests" | Out-Null
& $adb push $tests "$remote/" | Out-Null
& $adb push (Join-Path $root "tests\data") "$remote/tests/" | Out-Null
& $adb shell "chmod 755 $remote/gsplat_tests_cpu && cd $remote && ./gsplat_tests_cpu --cpu-only"
$code = $LASTEXITCODE

if (-not $NoInstall) {
  $apk = Get-ChildItem (Join-Path $root "dist") -Filter "*-android.apk" | Sort-Object LastWriteTime | Select-Object -Last 1
  if ($apk) {
    & $adb install -r $apk.FullName
    & $adb shell monkey -p com.gsplatviewer.app -c android.intent.category.LAUNCHER 1 | Out-Null
    Write-Output "Installed and started $($apk.Name). Logs: adb logcat -s gsplat"
  }
}
exit $code
