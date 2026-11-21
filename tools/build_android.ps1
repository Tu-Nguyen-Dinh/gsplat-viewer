# Builds the Android app (arm64-v8a) and packages a signed APK without Gradle:
#   NDK CMake toolchain -> libgsplat_android.so
#   aapt2 link (manifest + assets/shaders) -> aapt add (native library) -> zipalign -> apksigner
#
#   powershell -ExecutionPolicy Bypass -File tools\build_android.ps1 [-AndroidRoot F:\tools\android] [-Install]
#
# Output: dist\gsplat-viewer-<version>-android.apk (debug-signed: install with
# `adb install -r <apk>` or copy it to the phone and open it).
param(
  [string]$AndroidRoot = $(if ($env:GSV_ANDROID_ROOT) { $env:GSV_ANDROID_ROOT } else { "F:\tools\android" }),
  [string]$NdkVersion = "27.3.13750724",
  [string]$BuildTools = "35.0.1",
  [int]$MinSdk = 29,
  [int]$TargetSdk = 35,
  # arm64-v8a: phones; x86_64: the Android emulator on a PC (runs natively, no ARM translation).
  [string[]]$Abis = @("arm64-v8a", "x86_64"),
  [switch]$Install
)
# Native tools write progress to stderr; exit codes are checked explicitly instead.
function Assert-Ok([string]$what) { if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)" } }

$root = Split-Path -Parent $PSScriptRoot
$sdk = Join-Path $AndroidRoot "sdk"
$ndk = Join-Path $sdk "ndk\$NdkVersion"
$bt = Join-Path $sdk "build-tools\$BuildTools"
$jdk = Get-ChildItem $AndroidRoot -Directory -Filter "jdk-*" | Select-Object -First 1
foreach ($p in $ndk, $bt, $jdk.FullName) { if (-not (Test-Path $p)) { throw "Missing $p (see docs/ANDROID.md)" } }
$env:JAVA_HOME = $jdk.FullName
$env:Path = "$($jdk.FullName)\bin;C:\msys64\mingw64\bin;$env:Path"

$version = (Select-String -Path (Join-Path $root "CMakeLists.txt") -Pattern 'project\(gsplat_viewer VERSION ([0-9.]+)').Matches[0].Groups[1].Value
$parts = $version.Split('.') | ForEach-Object { [int]$_ }
$versionCode = $parts[0] * 10000 + $parts[1] * 100 + $parts[2]
$build = Join-Path $root "build-android"
$stage = Join-Path $build "apk"

# 1. Native library. Reuse the desktop build's dependency sources when present (no network).
$deps = Join-Path $root "build\_deps"
$fetch = @()
foreach ($d in "glm", "imgui") {
  $src = Join-Path $deps "$d-src"
  if (Test-Path $src) { $fetch += "-DFETCHCONTENT_SOURCE_DIR_$($d.ToUpper())=$($src -replace '\\','/')" }
}
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$stage\assets" | Out-Null
foreach ($abi in $Abis) {
  $abiBuild = Join-Path $build $abi  # one CMake build tree per ABI (also holds gsplat_tests_cpu)
  & cmake -S $root -B $abiBuild -G "MinGW Makefiles" `
    "-DCMAKE_TOOLCHAIN_FILE=$($ndk -replace '\\','/')/build/cmake/android.toolchain.cmake" `
    "-DANDROID_ABI=$abi" "-DANDROID_PLATFORM=android-$MinSdk" -DCMAKE_BUILD_TYPE=Release `
    -DFETCHCONTENT_UPDATES_DISCONNECTED=ON @fetch | Out-Null
  Assert-Ok "cmake configure ($abi)"
  & cmake --build $abiBuild -j 8 --target gsplat_android gsplat_tests_cpu
  Assert-Ok "cmake build ($abi)"
  # 2. Stage the APK content: the native library of every ABI.
  New-Item -ItemType Directory -Force "$stage\lib\$abi" | Out-Null
  Copy-Item (Join-Path $abiBuild "android\libgsplat_android.so") "$stage\lib\$abi\"
  & "$ndk\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe" --strip-unneeded "$stage\lib\$abi\libgsplat_android.so"
  Assert-Ok "strip ($abi)"
}
Copy-Item -Recurse (Join-Path $root "shaders") "$stage\assets\shaders"  # shaders as assets

# 3. Package: manifest + assets (aapt2), add the library (aapt), align, sign.
$unsigned = Join-Path $build "unsigned.apk"
$aligned = Join-Path $build "aligned.apk"
& "$bt\aapt2.exe" link -o $unsigned --manifest (Join-Path $root "android\AndroidManifest.xml") `
  -I (Join-Path $sdk "platforms\android-$TargetSdk\android.jar") `
  --min-sdk-version $MinSdk --target-sdk-version $TargetSdk --version-code $versionCode --version-name $version --debug-mode
Assert-Ok "aapt2 link"
# Library and assets are added with aapt, entry names written with '/': aapt2 -A on Windows stores
# "assets/shaders\include\x.glsl", which AAssetManager on the phone cannot find.
Push-Location $stage
$entries = @($Abis | ForEach-Object { "lib/$_/libgsplat_android.so" }) + (Get-ChildItem -Recurse -File "assets" |
  ForEach-Object { (Resolve-Path -Relative $_.FullName).TrimStart('.', '\').Replace('\', '/') })
& "$bt\aapt.exe" add $unsigned @entries | Out-Null
$code = $LASTEXITCODE
Pop-Location
if ($code -ne 0) { throw "aapt add failed (exit $code)" }
& "$bt\zipalign.exe" -p -f 4 $unsigned $aligned
Assert-Ok "zipalign"

$keystore = Join-Path $AndroidRoot "debug.keystore"
if (-not (Test-Path $keystore)) {
  & keytool -genkeypair -keystore $keystore -storepass android -keypass android -alias androiddebugkey `
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" 2>$null | Out-Null
  Assert-Ok "keytool"
}
New-Item -ItemType Directory -Force (Join-Path $root "dist") | Out-Null
$apk = Join-Path $root "dist\gsplat-viewer-$version-android.apk"
& cmd /c "`"$bt\apksigner.bat`" sign --ks `"$keystore`" --ks-pass pass:android --key-pass pass:android --out `"$apk`" `"$aligned`""
Assert-Ok "apksigner sign"
& cmd /c "`"$bt\apksigner.bat`" verify `"$apk`""
Assert-Ok "apksigner verify"
Write-Output ("{0}  {1:N2} MB" -f $apk, ((Get-Item $apk).Length / 1MB))

if ($Install) {
  & (Join-Path $sdk "platform-tools\adb.exe") install -r $apk
  Assert-Ok "adb install"
}
