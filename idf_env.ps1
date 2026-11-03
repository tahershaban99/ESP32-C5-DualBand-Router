# PowerShell Environment Activator for ESP-IDF v6.1 & ESP32-C5 (Dual-Band 2.4/5GHz)
$env:IDF_PATH = "E:\esp\v6.1\esp-idf"
$env:IDF_TOOLS_PATH = "E:\Espressif"
$env:PYTHONUTF8 = "1"

if (Test-Path "$env:IDF_PATH\export.ps1") {
    . "$env:IDF_PATH\export.ps1"
} else {
    Write-Host "[ERROR] ESP-IDF not found at $env:IDF_PATH" -ForegroundColor Red
}

