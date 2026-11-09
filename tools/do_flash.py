#!/usr/bin/env python3
"""
ESP32-C3 Firmware Flash Tool
Flashes pn5180_epd_driver.bin to 0x10000 on ESP32-C3
"""

import os
import sys
import subprocess
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent.parent

try:
    import serial.tools.list_ports
except ImportError:
    serial = None

def find_esp32_port():
    if not serial:
        return None
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
    bin_path = ROOT_DIR / "firmware" / "build" / "pn5180_epd_driver.bin"
    if not bin_path.exists():
        bin_path = ROOT_DIR / "releases" / "pn5180_epd_driver.bin"

    if not bin_path.exists():
        print(f"[ERROR] Firmware binary not found: {bin_path}")
        sys.exit(1)

    port = sys.argv[1] if len(sys.argv) > 1 else find_esp32_port()
    if not port:
        print("[ERROR] No serial device detected. Specify port as argument: python do_flash.py COMx")
        sys.exit(1)

    python_idf = os.environ.get("PYTHON_IDF", r"C:\Espressif\tools\python\v5.5.3\venv\Scripts\python.exe")
    esptool_py = os.environ.get("ESPTOOL_PY", r"E:\espidf\.espressif\v5.5.3\esp-idf\components\esptool_py\esptool\esptool.py")

    if os.path.exists(python_idf) and os.path.exists(esptool_py):
        cmd = [python_idf, esptool_py]
    else:
        cmd = [sys.executable, "-m", "esptool"]

    cmd += [
        "--chip", "esp32c3",
        "-p", port,
        "-b", "921600",
        "--before=default_reset",
        "--after=hard_reset",
        "write_flash", "0x10000", str(bin_path)
    ]

    print(f"[Flash] Port: {port}, Binary: {bin_path.name}")
    print("Running esptool...")
    res = subprocess.run(cmd)
    sys.exit(res.returncode)

if __name__ == "__main__":
    main()
