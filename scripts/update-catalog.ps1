# Update data/panplay-catalog.json from PlayStation-Titles (MIT). No API key required.
# After updating, restart panplay serve or reload catalog from Settings.

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

$PythonCmd = $null
foreach ($cmd in @("python", "python3")) {
    if (Get-Command $cmd -ErrorAction SilentlyContinue) {
        $PythonCmd = $cmd
        break
    }
}
if (-not $PythonCmd -and (Get-Command py -ErrorAction SilentlyContinue)) {
    $PythonCmd = "py"
    $PyArgs = @("-3", "scripts/build-catalog.py") + $args
} elseif ($PythonCmd) {
    $PyArgs = @("scripts/build-catalog.py") + $args
}

if (-not $PythonCmd) {
    Write-Error "Python 3 not found. Install Python 3 and add it to PATH."
    exit 1
}

Write-Host "Updating game catalog from PlayStation-Titles..." -ForegroundColor Cyan
if ($PythonCmd -eq "py") {
    & py @PyArgs
} else {
    & $PythonCmd @PyArgs
}
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host ""
Write-Host "Wrote data/panplay-catalog.json." -ForegroundColor Green
Write-Host "Restart panplay serve, or use Settings > Reload catalog." -ForegroundColor Yellow
