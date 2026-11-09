#!/usr/bin/env python3
"""
ESP32-C3 Flash & Real-Time EPD Refresh Monitor
Refactored per QPoll Lite Architecture
"""

import os
import sys
import time
import subprocess
import serial
import serial.tools.list_ports

from pathlib import Path

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
if hasattr(sys.stderr, 'reconfigure'):
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')

ROOT_DIR = Path(__file__).resolve().parent.parent
PYTHON_IDF = os.environ.get('PYTHON_IDF', r'C:\Espressif\tools\python\v5.5.3\venv\Scripts\python.exe')
ESPTOOL_PY = os.environ.get('ESPTOOL_PY', r'E:\espidf\.espressif\v5.5.3\esp-idf\components\esptool_py\esptool\esptool.py')
BIN_PATH = str(ROOT_DIR / 'firmware' / 'build' / 'pn5180_epd_driver.bin')
if not os.path.exists(BIN_PATH) and os.path.exists(str(ROOT_DIR / 'releases' / 'pn5180_epd_driver.bin')):
    BIN_PATH = str(ROOT_DIR / 'releases' / 'pn5180_epd_driver.bin')

def find_esp32_port():
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        desc = (p.description or "").lower()
        hwid = (p.hwid or "").lower()
        if "303a" in hwid or "esp32" in desc or "usb-serial" in desc or "jtag" in desc or "ch340" in desc or "cp210" in desc:
            return p.device
    if ports:
        return ports[0].device
    return None

def main():
    port = None
    if len(sys.argv) > 1:
        port = sys.argv[1]
    else:
        port = find_esp32_port()

    print(f"============================================================")
    print(f"[NFC EPD Flash Tool] Target Serial Port: {port}")
    print(f"[NFC EPD Flash Tool] Binary: {BIN_PATH}")
    print(f"============================================================")

    if not port:
        print("[ERROR] No serial device detected. Please connect ESP32-C3 via USB!")
        sys.exit(1)

    if not os.path.exists(BIN_PATH):
        print(f"[ERROR] Firmware binary not found: {BIN_PATH}")
        sys.exit(1)

    print(f"[Step 1/2] Flashing firmware to ESP32-C3 ({port})...", flush=True)
    if os.path.exists(PYTHON_IDF) and os.path.exists(ESPTOOL_PY):
        cmd = [PYTHON_IDF, ESPTOOL_PY]
    else:
        cmd = [sys.executable, '-m', 'esptool']

    cmd += [
        '--chip', 'esp32c3',
        '-p', port,
        '-b', '921600',
        '--before=default_reset',
        '--after=hard_reset',
        'write_flash', '0x10000', BIN_PATH
    ]

    res = subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if res.returncode != 0:
        print(f"[FAIL] esptool returned code {res.returncode}:\n{res.stderr or res.stdout}", flush=True)
        sys.exit(1)

    print("[Step 1/2] Flash Succeeded! ESP32-C3 hard reset completed.\n", flush=True)
    time.sleep(0.3)

    print(f"[Step 2/2] Opening {port} at 115200 baud for real-time monitoring...", flush=True)
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = False
    s.rts = False

    try:
        s.open()
    except Exception as e:
        print(f"[ERROR] Failed to open port {port}: {e}")
        sys.exit(1)

    start_time = time.time()
    max_duration = 600 # 10 minutes max

    try:
        err_count = 0
        while time.time() - start_time < max_duration:
            try:
                n = s.in_waiting
                err_count = 0
            except Exception as e:
                err_count += 1
                if err_count > 20:
                    print(f"\n[ERROR] Serial read error: {e}\n")
                    break
                time.sleep(0.1)
                continue

            data = s.read(n or 1)
            if data:
                text = data.decode('utf-8', errors='replace')
                sys.stdout.write(text)
                sys.stdout.flush()

                if "[SUCCESS]" in text or "Safe Power-Down Sequence Finished" in text:
                    print("\n\n[MONITOR] Refresh confirmed successful! Monitoring concluding in 3 seconds...", flush=True)
                    time.sleep(3)
                    break
            else:
                time.sleep(0.01)
    except KeyboardInterrupt:
        print("\n\n[MONITOR] Interrupted by user.")
    finally:
        s.close()
        print("[MONITOR] Serial port closed.")

if __name__ == '__main__':
    main()
