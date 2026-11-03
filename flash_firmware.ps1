param (
    [string]$Port = "COM7",
    [int]$Baud = 921600
)

$python = "E:\Espressif\tools\python\v6.1\venv\Scripts\python.exe"
$esptool = "E:\esp\v6.1\esp-idf\components\esptool_py\esptool\esptool.py"

if (!(Test-Path "build\esp32_napt_router.bin")) {
    Write-Host "[ERROR] Firmware binary not found! Please run .\build_firmware.ps1 first."
    exit 1
}

# Ensure blank NVS partition file exists (24KB of 0xFF)
if (!(Test-Path "build\nvs_blank.bin")) {
    & $python -c "with open('build/nvs_blank.bin', 'wb') as f: f.write(b'\xff' * 0x6000)"
}

Write-Host "Flashing ESP32-C5 Dual-Band Wi-Fi 6 firmware & Erasing NVS to $Port at $Baud baud..."

& $python $esptool -p $Port -b $Baud --chip esp32c5 `
    --before default_reset --after hard_reset write_flash `
    --flash_mode dio --flash_size 4MB --flash_freq 80m `
    0x2000 build\bootloader\bootloader.bin `
    0x8000 build\partition_table\partition-table.bin `
    0x9000 build\nvs_blank.bin `
    0xf000 build\ota_data_initial.bin `
    0x20000 build\esp32_napt_router.bin

if ($LASTEXITCODE -eq 0) {
    Write-Host "`n[SUCCESS] ESP32-C5 Firmware flashed successfully to $Port!"
    exit 0
} else {
    Write-Host "`n[ERROR] Flashing failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}
