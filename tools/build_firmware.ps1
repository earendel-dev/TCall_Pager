# Compile firmware only. This script never uploads or changes eFuses.
param([string]$ArduinoCli = "arduino-cli")
$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$sketchDirectory = Join-Path $repoRoot "build\firmware-sketch\TCall_Pager"
$buildDirectory = Join-Path $repoRoot "build\firmware"
$outputDirectory = Join-Path $repoRoot "dist\firmware"
New-Item -ItemType Directory -Force -Path $sketchDirectory, $buildDirectory, $outputDirectory | Out-Null
Copy-Item -LiteralPath (Join-Path $repoRoot "TCall_Pager.ino"), (Join-Path $repoRoot "modem.h") -Destination $sketchDirectory -Force
& $ArduinoCli compile --fqbn esp32:esp32:esp32 --board-options PSRAM=enabled,PartitionScheme=default --warnings all --build-path $buildDirectory --output-dir $outputDirectory $sketchDirectory
if ($LASTEXITCODE -ne 0) { throw "Firmware compilation failed ($LASTEXITCODE)." }
