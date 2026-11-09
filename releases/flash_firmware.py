#!/usr/bin/env python3
"""
一键烧录出厂固件到 ESP32-C3
支持自动探测串口并烧录完整 Bootloader + 分区表 + 固件应用
"""

import os
import sys
import subprocess
from pathlib import Path

try:
    import serial.tools.list_ports
except ImportError:
    subprocess.check_call([sys.executable, "-m", "pip", "install", "pyserial", "esptool"])
    import serial.tools.list_ports

SCRIPT_DIR = Path(__file__).resolve().parent

BOOTLOADER_BIN = SCRIPT_DIR / "bootloader.bin"
PARTITION_BIN = SCRIPT_DIR / "partition-table.bin"
APP_BIN = SCRIPT_DIR / "pn5180_epd_driver.bin"

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
    print("=" * 65)
    print("  3.98\" 无源 NFC 墨水屏 ESP32-C3 固件一键烧录工具")
    print("=" * 65)

    if not APP_BIN.exists():
        print(f"[错误] 固件文件不存在: {APP_BIN}")
        sys.exit(1)

    port = None
    if len(sys.argv) > 1:
        port = sys.argv[1]
    else:
        port = find_esp32_port()

    if not port:
        print("[错误] 未检测到任何串口设备，请使用 USB 数据线连接 ESP32-C3 并重试！")
        sys.exit(1)

    print(f"目标串口: {port}")
    print(f"固件文件: {APP_BIN.name} ({APP_BIN.stat().st_size} 字节)")
    print("正在启动烧录 (Baud: 921600)...")

    cmd = [
        sys.executable, "-m", "esptool",
        "--chip", "esp32c3",
        "-p", port,
        "-b", "921600",
        "--before=default_reset",
        "--after=hard_reset",
        "write_flash",
        "0x0", str(BOOTLOADER_BIN),
        "0x8000", str(PARTITION_BIN),
        "0x10000", str(APP_BIN)
    ]

    try:
        ret = subprocess.run(cmd)
        if ret.returncode == 0:
            print("\n" + "=" * 65)
            print("  [完成] 烧录成功！ESP32-C3 已自动硬复位！")
            print("  现在可以连接 Wi-Fi: EPD-3.98-NFC (无密码)")
            print("  或用手机/电脑浏览器打开: http://192.168.4.1/ 开始刷屏！")
            print("=" * 65 + "\n")
        else:
            print(f"\n[失败] esptool 返回退出码: {ret.returncode}")
    except Exception as e:
        print(f"\n[错误] 烧录进程异常: {e}")

if __name__ == "__main__":
    main()
