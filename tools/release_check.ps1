# Release step 1 (read-only): what would be committed, and does the Android build still work?
# Started by double-clicking tools\release_check.bat. Writes build\release_check.log (UTF-8).
$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
New-Item -ItemType Directory -Force -Path (Join-Path $root "build") | Out-Null
$log = Join-Path $root "build\release_check.log"
"=== release check $(Get-Date -Format s) ===" | Out-File $log -Encoding utf8
function Log([string]$t) { $t | Out-File $log -Append -Encoding utf8; Write-Host $t }
function Run([string]$title, [scriptblock]$cmd) {
  Log "=== $title ==="
  & $cmd 2>&1 | ForEach-Object { "$_" } | Out-File $log -Append -Encoding utf8
  Log "exit code $LASTEXITCODE"
}
Run "git version" { git --version }
Run "remote" { git remote -v }
Run "branch" { git branch --show-current }
Run "identity" { git config user.name; git config user.email }
Run "fetch" { git fetch origin --tags }
Run "status" { git status --short --branch }
Run "tags" { git tag -l --sort=-creatordate | Select-Object -First 10 }
Run "log" { git log --oneline -8 }
Run "diff stat" { git diff --stat | Select-Object -Last 5 }
Log "=== untracked files (size KB) ==="
git ls-files --others --exclude-standard | ForEach-Object {
  $kb = [math]::Round((Get-Item -LiteralPath $_).Length / 1KB, 1)
  "$kb`t$_"
} | Out-File $log -Append -Encoding utf8
Log "=== modified files larger than 1 MB ==="
git diff --name-only | ForEach-Object {
  if ((Test-Path -LiteralPath $_) -and (Get-Item -LiteralPath $_).Length -gt 1MB) { "$_" }
} | Out-File $log -Append -Encoding utf8
Run "android build (arm64-v8a)" { powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "tools\build_android.ps1") -Abis arm64-v8a }
Log "=== done ==="
Write-Host ""
Write-Host "Log: $log"
Read-Host "Press Enter to close"
