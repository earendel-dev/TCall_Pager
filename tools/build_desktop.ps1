# Build both desktop executables and run the offline packaging smoke check.
param([string]$Python = "python")
$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Push-Location (Join-Path $repoRoot "GUI")
try {
    & $Python -m PyInstaller --clean --noconfirm gui.spec
    if ($LASTEXITCODE -ne 0) { throw "Desktop build failed ($LASTEXITCODE)." }
    & ".\dist\TCallPager-1.1.0-rc.2-windows\TCallPager-1.1.0-rc.2-selfcheck.exe" --self-check
    if ($LASTEXITCODE -ne 0) { throw "Frozen smoke check failed ($LASTEXITCODE)." }
} finally { Pop-Location }
