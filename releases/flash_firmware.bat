@echo off
chcp 65001 >nul
title 3.98寸无源NFC墨水屏一键烧录
echo ========================================================
echo   3.98寸无源四色墨水屏 ESP32-C3 固件一键烧录
echo ========================================================
echo.
python flash_firmware.py
echo.
pause
