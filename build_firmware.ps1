$paths = @(
    "E:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64",
    "E:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin",
    "E:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin",
    "E:\Espressif\tools\cmake\4.0.3\bin",
    "E:\Espressif\tools\ninja\1.12.1",
    "E:\Espressif\tools\python\v6.1\venv\Scripts",
    "E:\esp\v6.1\esp-idf\tools"
)
$env:PATH = ($paths -join ";") + ";" + $env:PATH
$env:IDF_PATH = "E:\esp\v6.1\esp-idf"

if (!(Test-Path "build\build.ninja")) {
    Write-Host "Configuring CMake project with Ninja for ESP32-C5 (Dual-Band 2.4/5GHz)..."
    cmake -G Ninja -DPYTHON="E:\Espressif\tools\python\v6.1\venv\Scripts\python.exe" -DPYTHON_DEPS_CHECKED=1 -DIDF_TARGET=esp32c5 -S . -B build
    if ($LASTEXITCODE -ne 0) {
        Write-Host "`n[ERROR] Project configuration failed."
        exit $LASTEXITCODE
    }
}

Write-Host "Compiling project with Ninja..."
ninja -C build
if ($LASTEXITCODE -eq 0) {
    Write-Host "`n[SUCCESS] ESP32-C5 Build completed successfully!"
    exit 0
} else {
    Write-Host "`n[ERROR] Build failed."
    exit $LASTEXITCODE
}
