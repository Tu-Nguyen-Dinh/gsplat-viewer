# Shared helpers for the capture pipeline scripts (dot-source: . "$PSScriptRoot\common.ps1").

# Default locations of the external tools; override with environment variables.
$script:Colmap = if ($env:GSV_COLMAP) { $env:GSV_COLMAP } else { "F:\tools\colmap\bin\colmap.exe" }
$script:Brush = if ($env:GSV_BRUSH) { $env:GSV_BRUSH } else { "F:\tools\brush\brush_app.exe" }

# Quotes one argument for a Windows command line (CommandLineToArgvW rules).
function ConvertTo-Arg([string]$a) {
  if ($a -notmatch '[\s"]' -and $a -ne '') { return $a }
  return '"' + ($a -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}

# Runs an external program, appending its stdout + stderr to $Log. Returns the exit code.
# (PowerShell 5.1 turns native stderr into error records, so the output goes to a file instead.)
function Invoke-Tool([string]$Exe, [string[]]$Arguments, [string]$Log) {
  $argLine = ((@($Exe) + $Arguments) | ForEach-Object { ConvertTo-Arg $_ }) -join ' '
  Add-Content -Encoding utf8 $Log "`n> $argLine"
  # cmd.exe does the redirection, so nothing is buffered in PowerShell and no pipe can fill up.
  # cmd /c strips one outer pair of quotes, hence the extra pair around the whole line.
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = "$env:SystemRoot\System32\cmd.exe"
  $psi.Arguments = '/d /c "' + $argLine + ' >> ' + (ConvertTo-Arg $Log) + ' 2>&1"'
  $psi.UseShellExecute = $false
  $psi.CreateNoWindow = $true
  $p = [System.Diagnostics.Process]::Start($psi)
  $p.WaitForExit()
  return $p.ExitCode
}

function Write-Step([string]$Text) { Write-Host "`n== $Text" -ForegroundColor Cyan }

function Format-Seconds([double]$s) {
  if ($s -lt 90) { return "{0:N1} s" -f $s }
  return "{0:N1} min" -f ($s / 60)
}
