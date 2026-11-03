@echo off
rem Batch Environment Activator for ESP-IDF v6.1 & ESP32-C5 (Dual-Band 2.4/5GHz)
set "IDF_PATH=E:\esp\v6.1\esp-idf"
set "IDF_TOOLS_PATH=E:\Espressif"
set "PYTHONUTF8=1"

if exist "%IDF_PATH%\export.bat" (
    call "%IDF_PATH%\export.bat"
) else (
    echo [ERROR] ESP-IDF not found at %IDF_PATH%
)

