# SPDX-License-Identifier: GPL-3.0-or-later
# Build the ESP32 firmware and export the generated firmware.bin to the repo root.
param(
    [string]$Env = 'esp32dev'
)

$buildPath = Join-Path -Path '.pio/build' -ChildPath $Env
$firmwarePath = Join-Path -Path $buildPath -ChildPath 'firmware.bin'
$exportPath = Join-Path -Path '.' -ChildPath 'firmware.bin'

if (-not (Test-Path $firmwarePath)) {
    Write-Host "Building firmware for profile '$Env'..."
    pio run -e $Env
}

if (-not (Test-Path $firmwarePath)) {
    Write-Error "Firmware binary not found at $firmwarePath"
    exit 1
}

Copy-Item -Path $firmwarePath -Destination $exportPath -Force
Write-Host "Exported firmware to $exportPath"
