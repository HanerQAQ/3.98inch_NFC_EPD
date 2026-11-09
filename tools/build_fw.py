import os
import sys
import subprocess

from pathlib import Path

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
if hasattr(sys.stderr, 'reconfigure'):
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')

import shutil

ROOT_DIR = Path(__file__).resolve().parent.parent
BUILD_DIR = str(ROOT_DIR / 'firmware' / 'build')
NINJA_PATH = os.environ.get('NINJA_PATH') or shutil.which('ninja') or r'C:\Espressif\tools\ninja\1.12.1\ninja.exe'

env = dict(os.environ)
env['IDF_PATH'] = r'E:\espidf\.espressif\v5.5.3\esp-idf'
env['ESP_ROM_ELF_DIR'] = r'E:\espidf\.espressif\tools\esp-rom-elfs\20241011'
env['PATH'] = (
    r'C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;'
    r'C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20251107\riscv32-esp-elf\bin;'
    r'C:\Espressif\tools\cmake\3.30.2\bin;' + env.get('PATH', '')
)

res = subprocess.run([NINJA_PATH], cwd=BUILD_DIR, env=env, capture_output=True, text=True, encoding='utf-8', errors='replace')
if res.returncode != 0:
    print(f"Ninja build failed:\n{res.stderr or res.stdout}")
    sys.exit(1)
else:
    print("Ninja build succeeded!")
    print(res.stdout[-300:] if len(res.stdout) > 300 else res.stdout)
