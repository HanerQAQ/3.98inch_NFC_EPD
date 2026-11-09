#!/usr/bin/env python3
"""
3.98" Passive NFC E-Paper Web Studio Server (Pure RAM Direct Mode)
Provides local web UI for image uploading, real-time dithering preview,
and direct USB-Serial RAM streaming + automated NFC burn escort (Zero Flash Wear).

Endpoints:
  GET  /              -> Serves tools/epd_studio.html
  GET  /api/status    -> Returns real-time JSON status and progress
  POST /upload_photo  -> Streams LZO1X D3 packets directly into ESP32-C3 RAM and refreshes EPD
"""

import re
import sys
import time
import json
import socket
import threading
from pathlib import Path
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

# Ensure tools directory is in sys.path
SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

import flash_and_monitor

# Paths
HTML_PATH = SCRIPT_DIR / "epd_studio.html"
UPLOADS_DIR = SCRIPT_DIR / "uploads"
UPLOADS_DIR.mkdir(parents=True, exist_ok=True)

# Global status tracker
g_lock = threading.Lock()
g_status = {
    "busy": False,
    "percent": 0,
    "stage": "idle",
    "status": "就绪：等待图片上传",
    "done": False,
    "error": None,
    "timestamp": time.time(),
    "last_log": ""
}

def set_status(percent=None, stage=None, status_msg=None, done=None, error=None, last_log=None):
    with g_lock:
        if percent is not None:
            g_status["percent"] = int(percent)
        if stage is not None:
            g_status["stage"] = stage
        if status_msg is not None:
            g_status["status"] = status_msg
        if done is not None:
            g_status["done"] = bool(done)
        if error is not None:
            g_status["error"] = error
        if last_log is not None:
            g_status["last_log"] = last_log
        g_status["timestamp"] = time.time()
        print(f"[{g_status['percent']}%] {g_status['status']}", flush=True)

def get_lan_ip():
    try:
        hostname = socket.gethostname()
        for ip in socket.gethostbyname_ex(hostname)[2]:
            if ip.startswith("192.168.") or ip.startswith("10.") or (ip.startswith("172.") and int(ip.split('.')[1]) in range(16, 32)):
                if not ip.startswith("192.168.137.") and not ip.startswith("192.168.165.") and not ip.startswith("192.168.198."):
                    return ip
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("1.1.1.1", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"

def ram_serial_d3_worker(payload: bytes, pkt_count: int):
    import serial

    with g_lock:
        g_status["busy"] = True
        g_status["done"] = False
        g_status["error"] = None

    s = None
    try:
        port = flash_and_monitor.find_esp32_port()
        set_status(percent=15, stage="ram_upload", status_msg=f"正在通过 USB 串口向 ESP32-C3 RAM 直传 {pkt_count} 个 D3 分包 ({len(payload)} 字节，0 Flash 磨损)...")

        s = serial.Serial()
        s.port = port
        s.baudrate = 115200
        s.timeout = 0.2
        s.dtr = False
        s.rts = False
        s.open()

        # Clear any pending serial input and send 'u' command to start RAM direct load
        s.reset_input_buffer()
        s.write(b'u')
        s.flush()

        # Wait for READY_RAM_HEX
        ready = False
        t0 = time.time()
        while time.time() - t0 < 3.0:
            line = s.readline().decode('utf-8', errors='replace').strip()
            if not line:
                continue
            print(f"[ESP32 Serial] {line}", flush=True)
            if "READY_RAM_HEX" in line:
                ready = True
                break
            elif "ERR_BUSY" in line:
                set_status(error="ESP32-C3 当前正在执行刷屏任务，请稍候再试！", done=True)
                return

        if not ready:
            set_status(error="ESP32-C3 未响应 RAM 直传握手指令 (请确认已烧录最新纯 RAM 固件)", done=True)
            return

        # Send length line and hex payload in chunks
        s.write(f"{len(payload)}\n".encode('ascii'))
        s.flush()

        hex_data = payload.hex().encode('ascii')
        chunk_sz = 4096
        for off in range(0, len(hex_data), chunk_sz):
            s.write(hex_data[off:off + chunk_sz])
            s.flush()

        # Wait for RAM_LOAD_OK
        loaded = False
        t1 = time.time()
        while time.time() - t1 < 5.0:
            line = s.readline().decode('utf-8', errors='replace').strip()
            if not line:
                continue
            print(f"[ESP32 Serial] {line}", flush=True)
            if "RAM_LOAD_OK" in line:
                loaded = True
                break
            elif "ERR_" in line:
                set_status(error=f"RAM 内存直传校验失败: {line}", done=True)
                return

        if not loaded:
            set_status(error="ESP32-C3 RAM 数据接收超时", done=True)
            return

        set_status(percent=25, stage="nfc_poll", status_msg="RAM 内存直传完成！正在 13.56MHz 寻卡 (请将墨水屏放于天线中心)...")

        start_t = time.time()
        while time.time() - start_t < 300:
            if s.in_waiting:
                line = s.readline().decode('utf-8', errors='replace')
                if not line:
                    continue
                line_str = line.strip()
                if line_str:
                    print(f"[ESP32 Serial] {line_str}", flush=True)
                if "Card Detected" in line or "成功识别被动" in line:
                    set_status(percent=35, stage="detected", status_msg="成功识别 3.98 寸墨水屏！正在对储能电容预充电...")
                elif "预充电" in line:
                    set_status(percent=40, stage="precharge", status_msg="3.3V 储能电容母线预充电中...")
                elif "RATS" in line or "校验安全" in line:
                    set_status(percent=45, stage="auth", status_msg="ISO-DEP 传输通道握手与安全 PIN 认证通过...")
                elif "Image Transfer:" in line:
                    m = re.search(r'Image Transfer:\s*(\d+)/(\d+)', line)
                    if m:
                        cur = int(m.group(1))
                        tot = int(m.group(2))
                        pct = 45 + int((cur / tot) * 37)
                        set_status(percent=pct, stage="transmitting", status_msg=f"正在从 RAM 传输点阵分包: {cur}/{tot} ({int(cur*100/tot)}%)...")
                elif "正在传输分包" in line:
                    m = re.search(r'\((\d+)%\)', line)
                    if m:
                        pct = 45 + int(int(m.group(1)) * 0.35)
                        set_status(percent=pct, stage="transmitting", status_msg=line_str)
                elif "高压升压泵蓄能" in line or "Stage 4.5" in line:
                    set_status(percent=82, stage="pump", status_msg="点阵数据传输完成，为高压升压泵蓄能中...")
                elif "高压电泳刷新" in line or "软启动" in line:
                    set_status(percent=85, stage="trigger", status_msg="正在触发高压电泳软启动 (D4 指令)...")
                elif "High-Voltage Pure Sine Wave:" in line:
                    m = re.search(r'sec\s*(\d+)/(\d+)', line)
                    if m:
                        sec = int(m.group(1))
                        tot = int(m.group(2))
                        pct = 85 + int((sec / float(tot)) * 12)
                        set_status(percent=pct, stage="escort", status_msg=f"纯正弦波高压电泳无扰护航: {sec}/{tot} 秒...")
                elif "Electrophoretic Settling:" in line:
                    set_status(percent=97, stage="settling", status_msg="电泳定型收尾确认中...")
                elif "[SUCCESS]" in line or "大功告成" in line or "Refresh cycle finished successfully" in line:
                    set_status(percent=100, stage="done", status_msg="[完成] 墨水屏刷新大功告成！画面已固化，射频天线场已安全断电。", done=True)
                    return
                elif "超时" in line or "失败" in line or "取消" in line:
                    if "寻卡等待" not in line and "retry" not in line:
                        set_status(error=line_str, done=True)
                        return
            else:
                time.sleep(0.05)
    except Exception as e:
        print(f"[ram_serial_d3_worker Error] {e}", flush=True)
        set_status(error=str(e), done=True)
    finally:
        if s is not None and s.is_open:
            try:
                s.close()
            except Exception:
                pass
        with g_lock:
            g_status["busy"] = False

class EPDStudioHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        try:
            msg = format % args
            if "/api/status" in msg:
                return
            sys.stdout.write("%s - - [%s] %s\n" % (self.address_string(), self.log_date_time_string(), msg))
        except Exception:
            pass

    def do_GET(self):
        if self.path == "/favicon.ico":
            self.send_response(204)
            self.end_headers()
            return
        if self.path == "/" or self.path.startswith("/index"):
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            with open(HTML_PATH, "rb") as f:
                self.wfile.write(f.read())
        elif self.path.startswith("/api/status"):
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            with g_lock:
                data = json.dumps(g_status, ensure_ascii=False).encode('utf-8')
            self.wfile.write(data)
        elif self.path.startswith("/uploads/"):
            raw_name = self.path.split("/uploads/")[1].split("?")[0]
            file_name = Path(raw_name).name
            file_path = UPLOADS_DIR / file_name
            if file_path.is_file():
                ext = file_path.suffix.lower()
                mime_map = {
                    ".png": "image/png",
                    ".jpg": "image/jpeg",
                    ".jpeg": "image/jpeg",
                    ".webp": "image/webp",
                    ".gif": "image/gif",
                    ".bin": "application/octet-stream",
                }
                self.send_response(200)
                self.send_header("Content-Type", mime_map.get(ext, "application/octet-stream"))
                self.end_headers()
                with open(file_path, "rb") as f:
                    self.wfile.write(f.read())
            else:
                self.send_error(404, "File Not Found")
        else:
            self.send_error(404, "Not Found")

    def do_POST(self):
        if self.path.startswith("/upload_photo"):
            content_length = int(self.headers.get("Content-Length", 0))
            payload = self.rfile.read(content_length)
            print(f"[Upload] Received raw binary stream (RAM direct): {len(payload)} bytes")

            pkts = []
            off = 0
            while off + 7 <= len(payload):
                hdr = payload[off:off+7]
                if hdr[0] != 0xF0 or hdr[1] != 0xD3:
                    break
                pkt_len = 5 + hdr[4]
                if off + pkt_len > len(payload):
                    break
                pkts.append(payload[off:off+pkt_len])
                off += pkt_len

            print(f"[Upload] Parsed {len(pkts)} D3 packets from binary stream", flush=True)

            if len(pkts) == 0:
                self.send_response(400)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.end_headers()
                self.wfile.write(json.dumps({"ok": False, "msg": "未提取到有效 D3 点阵分包！"}).encode('utf-8'))
                return

            t = threading.Thread(
                target=ram_serial_d3_worker,
                args=(payload[:off], len(pkts)),
                daemon=True
            )
            t.start()

            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.end_headers()
            resp = json.dumps({"ok": True, "packets": len(pkts), "msg": f"数据已接收 ({len(pkts)} 个 D3 分包)，正在通过 RAM 内存直传刷屏！"}, ensure_ascii=False)
            self.wfile.write(resp.encode('utf-8'))
        else:
            self.send_error(404, "Not Found")

def run_server(port=8080):
    lan_ip = get_lan_ip()
    server_address = ("0.0.0.0", port)
    httpd = ThreadingHTTPServer(server_address, EPDStudioHandler)
    print("=" * 65)
    print("  3.98\" 无源 NFC 墨水屏 Web 刷图工作台 (纯 RAM 直传版) 已启动")
    print("=" * 65)
    print(f"  > 本地电脑访问: http://localhost:{port}")
    print(f"  > 手机局域网访问: http://{lan_ip}:{port}")
    print("=" * 65)
    print("  支持 4 种精选渲染算法: 鎏金 Atkinson / 质感 LOMO / 纯色阶 / BT.601水墨")
    print("  纯 RAM 零磨损直传 + 100% 纯正弦波高压电泳无扰护航")
    print("=" * 65 + "\n")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[Server] 正在停止 Web 服务...")
        httpd.server_close()
        print("[Server] 服务已停止。")

if __name__ == "__main__":
    port = 8080
    if len(sys.argv) > 1 and sys.argv[1].isdigit():
        port = int(sys.argv[1])
    run_server(port)
