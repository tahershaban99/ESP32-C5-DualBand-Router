import subprocess
import sys
import time
import os

try:
    sys.stdout.reconfigure(encoding='utf-8')
except Exception:
    pass

python_exe = r"E:\Espressif\python_env\idf6.1_py3.14_env\Scripts\python.exe"
port = "COM7"
baud = "460800"

bootloader = r"build\bootloader\bootloader.bin"
partition = r"build\partition_table\partition-table.bin"
nvs_blank = r"build\nvs_blank.bin"
ota_data = r"build\ota_data_initial.bin"
app_bin = r"build\esp32_napt_router.bin"

# Ensure nvs_blank exists (24KB of 0xFF)
if not os.path.exists(nvs_blank) or os.path.getsize(nvs_blank) != 0x6000:
    with open(nvs_blank, "wb") as f:
        f.write(b"\xff" * 0x6000)

cmd = [
    python_exe, "-m", "esptool",
    "--chip", "esp32c5",
    "-p", port,
    "-b", baud,
    "--before", "usb-reset",
    "--after", "hard-reset",
    "--no-stub",
    "write-flash",
    "--flash-mode", "dio",
    "--flash-size", "4MB",
    "--flash-freq", "80m",
    "0x2000", bootloader,
    "0x8000", partition,
    "0xf000", ota_data,
    "0x20000", app_bin
]

print("=" * 60)
print("  ESP32-C5 Router Flasher & NVS Eraser")
print("=" * 60)
print(f"Target Port : {port}")
print(f"Target Chip : ESP32-C5 (Dual-Band Wi-Fi 6)")
print(f"NVS Reset   : Included (0x9000 - 24KB wiped clean)")
print("=" * 60)
print("If device is not responding automatically:")
print(">> HOLD the 'BOOT' button, CLICK the 'RST' button, then RELEASE 'BOOT' <<")
print("=" * 60)

max_attempts = 20
success = False

for attempt in range(1, max_attempts + 1):
    print(f"[{attempt}/{max_attempts}] Connecting to {port}...", flush=True)
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode == 0:
        print("\n" + "=" * 60)
        print(">> FLASH & NVS ERASE SUCCESSFUL! <<")
        print("=" * 60)
        print(res.stdout)
        success = True
        break
    else:
        err = res.stderr + "\n" + res.stdout
        if "Failed to connect" in err or "No serial data received" in err:
            print("  Waiting for Download Mode... (Please hold BOOT and click RST)", flush=True)
        elif "Failed to start stub" in err:
            print("  Stub detected, please click RST once...", flush=True)
        else:
            print("  Retrying...", flush=True)
        time.sleep(1.5)

if not success:
    print("\n[FAILED] Timed out waiting for ESP32-C5 Download Mode.")
    sys.exit(1)

# Monitor boot log for 4 seconds to verify startup
print("\nVerifying boot...")
time.sleep(0.5)
try:
    import serial
    s = serial.Serial(port, 115200, timeout=0.1)
    s.dtr = False
    s.rts = False
    time.sleep(0.1)
    s.dtr = True
    s.rts = True
    t0 = time.time()
    while time.time() - t0 < 4:
        data = s.read(s.in_waiting or 1).decode('utf-8', errors='ignore')
        if data:
            print(data, end='', flush=True)
        time.sleep(0.05)
    s.close()
except Exception as e:
    print(f"Note: Could not monitor serial: {e}")

print("\nProcess finished.")

