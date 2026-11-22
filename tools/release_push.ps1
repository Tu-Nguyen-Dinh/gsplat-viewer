# Release step 2: two commits (code, then the release notes), annotated tag v2.0.0, push.
# Started by double-clicking tools\release_push.bat. Writes build\release_push.log (UTF-8).
$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$log = Join-Path $root "build\release_push.log"
"=== release push $(Get-Date -Format s) ===" | Out-File $log -Encoding utf8
function Log([string]$t) { $t | Out-File $log -Append -Encoding utf8; Write-Host $t }
function Run([string]$title, [scriptblock]$cmd) {
  Log "=== $title ==="
  & $cmd 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $log -Append | Out-Host
  $code = $LASTEXITCODE
  Log "exit code $code"
  return $code
}
function Stop([string]$why) { Log "STOPPED: $why"; Read-Host "Press Enter to close"; exit 1 }

# Safety checks: right branch, not behind GitHub, tag not used yet.
if ((git branch --show-current) -ne "main") { Stop "not on main" }
git fetch origin --tags 2>&1 | Out-Null
$behind = git rev-list --count HEAD..origin/main
if ([int]$behind -gt 0) { Stop "main is $behind commit(s) behind origin/main: pull first" }
if (git tag -l v2.0.0) { Stop "tag v2.0.0 already exists" }

# The messages live in build\ (ignored by git) so they are not committed themselves.
foreach ($m in "release_commit_msg.txt", "release_tag_msg.txt") {
  $src = Join-Path $root "tools\$m"
  if (Test-Path $src) { Move-Item -Force $src (Join-Path $root "build\$m") }
}
$release = @("README.md", "CHANGELOG.md", "docs/images/v20_editor.jpg")
if ((Run "stage code" { git add -A -- . ":!README.md" ":!CHANGELOG.md" ":!docs/images/v20_editor.jpg" }) -ne 0) { Stop "git add" }
if ((Run "commit code" { git commit -F build/release_commit_msg.txt }) -ne 0) { Stop "commit 1" }
if ((Run "stage release notes" { git add -- $release }) -ne 0) { Stop "git add release" }
if ((Run "commit release" { git commit -m "release: v2.0.0" -m "README (editor and usability sections, screenshot), CHANGELOG, version 2.0.0." }) -ne 0) { Stop "commit 2" }
if ((Run "tag" { git tag -a v2.0.0 -F build/release_tag_msg.txt }) -ne 0) { Stop "tag" }
Run "log" { git log --oneline -4 } | Out-Null
if ((Run "push main" { git push origin main }) -ne 0) { Stop "push main (commits are local; run again after fixing)" }
if ((Run "push tag" { git push origin v2.0.0 }) -ne 0) { Stop "push tag" }
Log "=== done: https://github.com/Tu-Nguyen-Dinh/gsplat-viewer ==="
Read-Host "Press Enter to close"
