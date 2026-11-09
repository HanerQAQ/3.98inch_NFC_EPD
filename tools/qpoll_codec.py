"""
QPoll Lite Python Codec Suite (墨小七核心算法与编解码库)
Faithfully ported from https://github.com/yhl88890/qpoll-lite
- ReinkRaster.java (4 official modes: 鎏金, 质感, 色阶, 水墨; Atkinson 6/8; Redmean distance; 4x5 LOMO)
- PanelImage.java (Area Average downsampling, density measurement)
- PanelType.java (A0 transposed row mapping, A1 direct, forScan top-down transformation)
- PanelProtocol.java (packCodes, literalLzo, compressedLzo, D3/D4 packetization)
"""

import math
from typing import Tuple, List, Dict, Optional
import numpy as np
from PIL import Image

# 屏幕硬件参数
WIDTH = 552
HEIGHT = 768
IMAGE_BYTES = 105984  # WIDTH * HEIGHT / 4

# 四色定义与调色板
# 0: 黑, 1: 白, 2: 黄, 3: 红
COLOR_BLACK  = 0
COLOR_WHITE  = 1
COLOR_YELLOW = 2
COLOR_RED    = 3

PALETTE_ARGB = [
    (0, 0, 0),        # 0: 黑 #000000
    (255, 255, 255),  # 1: 白 #FFFFFF
    (245, 205, 0),    # 2: 黄 #F5CD00
    (200, 0, 0)       # 3: 红 #C80000
]

MODES = ["鎏金", "质感", "色阶", "水墨"]
MODE_GOLD    = 0
MODE_TEXTURE = 1
MODE_LEVEL   = 2
MODE_INK     = 3

# 质感模式 4x5 LOMO 调色矩阵 (墨小佳 matrixLOMO)
# 增益 1.7, 偏置 -73.1, 交点在 81: 低于 81 压黑, 高于 81 推至纯白
LOMO_MATRIX = [
    1.7, 0.1, 0.1, 0.0, -73.1,
    0.0, 1.7, 0.1, 0.0, -73.1,
    0.0, 0.1, 1.6, 0.0, -73.1,
    0.0, 0.0, 0.0, 1.0, 0.0
]

# Atkinson 误差扩散步进: 6 个邻居每个分 1/8, 合计 6/8, 故意丢弃 2/8
ATKINSON_STEPS = [(1, 0), (2, 0), (-1, 1), (0, 1), (1, 1), (0, 2)]

def clamp(val: float) -> float:
    return max(0.0, min(255.0, val))

def redmean_nearest(r: float, g: float, b: float) -> int:
    """
    Redmean 加权颜色距离 (感知更准，浅色区不偏黄)
    """
    sr = int(round(clamp(r)))
    sg = int(round(clamp(g)))
    sb = int(round(clamp(b)))
    best_idx = 0
    best_dist = float('inf')

    for idx, (cr, cg, cb) in enumerate(PALETTE_ARGB):
        dr = sr - cr
        dg = sg - cg
        db = sb - cb
        mean = (sr + cr) // 2
        dist = (((512 + mean) * dr * dr) >> 8) + (4 * dg * dg) + (((767 - mean) * db * db) >> 8)
        if dist < best_dist:
            best_dist = dist
            best_idx = idx

    return best_idx

def apply_lomo_transform(r_arr: np.ndarray, g_arr: np.ndarray, b_arr: np.ndarray):
    """原地应用 4x5 LOMO 调色矩阵"""
    m = LOMO_MATRIX
    sr = r_arr.copy()
    sg = g_arr.copy()
    sb = b_arr.copy()
    r_arr[:] = np.clip(m[0]*sr + m[1]*sg + m[2]*sb + m[4], 0.0, 255.0)
    g_arr[:] = np.clip(m[5]*sr + m[6]*sg + m[7]*sb + m[9], 0.0, 255.0)
    b_arr[:] = np.clip(m[10]*sr + m[11]*sg + m[12]*sb + m[14], 0.0, 255.0)

def apply_grayscale_bt601(r_arr: np.ndarray, g_arr: np.ndarray, b_arr: np.ndarray):
    """ITU-R BT.601 灰阶转换"""
    lum = 0.299 * r_arr + 0.587 * g_arr + 0.114 * b_arr
    r_arr[:] = lum
    g_arr[:] = lum
    b_arr[:] = lum

def area_average_fit(img: Image.Image, target_w: int = WIDTH, target_h: int = HEIGHT, cover: bool = True) -> np.ndarray:
    """
    面积平均加权降采样 (PanelImage.fit):
    防止手机高清照片隔点抽样产生传感器噪点放大
    返回: (target_h, target_w, 3) float64 数组 (0~255)
    """
    rgba = img.convert('RGBA')
    src_w, src_h = rgba.size
    
    scale = max(target_w / src_w, target_h / src_h) if cover else min(target_w / src_w, target_h / src_h)
    
    # 转换为 numpy
    src_np = np.array(rgba, dtype=np.float64)
    # 透明通道按纯白底合成
    alpha = src_np[:, :, 3] / 255.0
    r_plane = src_np[:, :, 0] * alpha + 255.0 * (1.0 - alpha)
    g_plane = src_np[:, :, 1] * alpha + 255.0 * (1.0 - alpha)
    b_plane = src_np[:, :, 2] * alpha + 255.0 * (1.0 - alpha)

    out = np.full((target_h, target_w, 3), 255.0, dtype=np.float64)
    drawn_w = src_w * scale
    drawn_h = src_h * scale
    origin_x = (target_w - drawn_w) / 2.0
    origin_y = (target_h - drawn_h) / 2.0

    for y in range(target_h):
        top = (y - origin_y) / scale
        bottom = (y + 1 - origin_y) / scale
        if bottom <= 0 or top >= src_h:
            continue
        top = max(0.0, top)
        bottom = min(float(src_h), bottom)
        first_row = int(math.floor(top))
        last_row = int(math.ceil(bottom))

        for x in range(target_w):
            left = (x - origin_x) / scale
            right = (x + 1 - origin_x) / scale
            if right <= 0 or left >= src_w:
                continue
            left = max(0.0, left)
            right = min(float(src_w), right)
            first_col = int(math.floor(left))
            last_col = int(math.ceil(right))

            sum_r = sum_g = sum_b = total_area = 0.0

            for sy in range(first_row, last_row):
                overlap_y = min(bottom, sy + 1.0) - max(top, float(sy))
                if overlap_y <= 0:
                    continue
                for sx in range(first_col, last_col):
                    overlap_x = min(right, sx + 1.0) - max(left, float(sx))
                    if overlap_x <= 0:
                        continue
                    area = overlap_x * overlap_y
                    sum_r += r_plane[sy, sx] * area
                    sum_g += g_plane[sy, sx] * area
                    sum_b += b_plane[sy, sx] * area
                    total_area += area

            if total_area > 0:
                out[y, x, 0] = sum_r / total_area
                out[y, x, 1] = sum_g / total_area
                out[y, x, 2] = sum_b / total_area

    return out

def quantize_reink(rgb_pixels: np.ndarray, mode: int = MODE_TEXTURE) -> np.ndarray:
    """
    按照墨小佳四档出图算法进行颜色量化与抖动
    输入: (HEIGHT, WIDTH, 3) 浮点色彩
    输出: (HEIGHT, WIDTH) uint8 颜色码 (0:黑, 1:白, 2:黄, 3:红)
    """
    h, w, _ = rgb_pixels.shape
    r = rgb_pixels[:, :, 0].copy()
    g = rgb_pixels[:, :, 1].copy()
    b = rgb_pixels[:, :, 2].copy()

    if mode == MODE_TEXTURE:
        apply_lomo_transform(r, g, b)
    elif mode == MODE_INK:
        apply_grayscale_bt601(r, g, b)
    elif mode == MODE_LEVEL:
        # 色阶模式: 纯量化, 零抖动 (NumPy 向量化加速)
        sr = np.round(np.clip(r, 0.0, 255.0)).astype(np.int32)
        sg = np.round(np.clip(g, 0.0, 255.0)).astype(np.int32)
        sb = np.round(np.clip(b, 0.0, 255.0)).astype(np.int32)
        dists = []
        for cr, cg, cb in PALETTE_ARGB:
            dr = sr - cr
            dg = sg - cg
            db = sb - cb
            mean = (sr + cr) // 2
            dist = (((512 + mean) * dr * dr) >> 8) + (4 * dg * dg) + (((767 - mean) * db * db) >> 8)
            dists.append(dist)
        return np.argmin(np.stack(dists, axis=-1), axis=-1).astype(np.uint8)

    # 鎏金 (MODE_GOLD), 质感 (MODE_TEXTURE), 水墨 (MODE_INK) 均使用 Atkinson 误差扩散
    err_r = np.zeros((h, w), dtype=np.float32)
    err_g = np.zeros((h, w), dtype=np.float32)
    err_b = np.zeros((h, w), dtype=np.float32)
    codes = np.zeros((h, w), dtype=np.uint8)

    for y in range(h):
        for x in range(w):
            sr = clamp(r[y, x] + err_r[y, x])
            sg = clamp(g[y, x] + err_g[y, x])
            sb = clamp(b[y, x] + err_b[y, x])

            c_idx = redmean_nearest(sr, sg, sb)
            codes[y, x] = c_idx

            pal_r, pal_g, pal_b = PALETTE_ARGB[c_idx]
            dr = (sr - pal_r) / 8.0
            dg = (sg - pal_g) / 8.0
            db = (sb - pal_b) / 8.0

            for dx, dy in ATKINSON_STEPS:
                tx = x + dx
                ty = y + dy
                if 0 <= tx < w and 0 <= ty < h:
                    err_r[ty, tx] += dr
                    err_g[ty, tx] += dg
                    err_b[ty, tx] += db

    return codes

def calculate_non_white_density(codes: np.ndarray) -> float:
    """
    计算非白占比 (Non-white Density)
    黑 (0), 黄 (2), 红 (3) 均为需要驱动翻转的色度, 白 (1) 不计
    """
    lit = np.sum((codes & 3) != 1)
    return float(lit) / codes.size * 100.0

def pack_codes(codes: np.ndarray, width: int = WIDTH, height: int = HEIGHT) -> bytes:
    """
    将 2bit 颜色码打包为硬件扫描格式 (NumPy 向量化实现):
    每字节 4 像素, 从右向左 (x: width-1 down to 0), 从下到上 (y: height-1 down to 0 by 4), MSB 优先
    """
    rev = (np.asarray(codes, dtype=np.uint8)[::-1, ::-1] & 3).T.reshape(width, height // 4, 4)
    packed = ((rev[:, :, 0] << 6) | (rev[:, :, 1] << 4) | (rev[:, :, 2] << 2) | rev[:, :, 3]).astype(np.uint8)
    return packed.tobytes()

def map_panel_type(raw_bytes: bytes, panel_type: str = "A0") -> bytes:
    """
    A0 / A1 屏幕物理行列转置映射:
    A0: 第一半映射到偶数行, 第二半反向映射到奇数行
    A1: 直通无映射
    """
    if panel_type.upper() == "A1":
        return raw_bytes
    
    rows = WIDTH
    stride = HEIGHT // 4
    out = bytearray(len(raw_bytes))

    for row in range(rows):
        target = 2 * row if row < rows // 2 else 2 * rows - 1 - 2 * row
        src_offset = row * stride
        dst_offset = target * stride
        out[dst_offset:dst_offset + stride] = raw_bytes[src_offset:src_offset + stride]

    return bytes(out)

def for_scan_top_down(packed_bytes: bytes, top_down: bool = True) -> bytes:
    """
    工厂扫描方向适应:
    topDown == True (扫描 01): 行内字节逆序且每个字节内的 bit-pair (2bit 像素) 高低反转
    topDown == False (扫描 00): 保持从下到上扫描
    """
    if not top_down:
        return packed_bytes

    stride = HEIGHT // 4
    out = bytearray(len(packed_bytes))

    for row in range(WIDTH):
        row_offset = row * stride
        for i in range(stride):
            b = packed_bytes[row_offset + stride - 1 - i]
            # 反转 4 个 2-bit 像素位置
            rev_b = ((b & 3) << 6) | ((b & 12) << 2) | ((b & 48) >> 2) | ((b & 192) >> 6)
            out[row_offset + i] = rev_b

    return bytes(out)

# ─────────────────── LZO1X 压缩与打包 ───────────────────

def literal_lzo(raw: bytes) -> bytes:
    """标准字面量流打包 (无匹配)"""
    length = len(raw)
    out = bytearray()
    if length <= 238:
        out.append(17 + length)
    else:
        out.append(0)
        rest = length - 18
        while rest > 255:
            out.append(0)
            rest -= 255
        out.append(rest)
    out.extend(raw)
    out.extend([0x11, 0x00, 0x00])
    return bytes(out)

class _LzoWriter:
    def __init__(self, capacity: int):
        self.buf = bytearray(capacity)
        self.size = 0

    def write_byte(self, val: int):
        self.buf[self.size] = val & 0xFF
        self.size += 1

    def append(self, raw: bytes, offset: int, length: int):
        self.buf[self.size:self.size + length] = raw[offset:offset + length]
        self.size += length

    def literal(self, raw: bytes, offset: int, length: int, last_match: int):
        if length <= 0:
            return
        if length <= 3 and last_match >= 0:
            self.buf[last_match] |= (length & 0x03)
        elif self.size == 0 and length <= 238:
            self.write_byte(17 + length)
        elif length <= 18:
            self.write_byte(length - 3)
        else:
            self.write_byte(0)
            self._zero_length(length - 18)
        self.append(raw, offset, length)

    def match(self, length: int, offset: int) -> int:
        offset -= 1
        if length <= 8 and offset <= 0x0800:
            self.write_byte(((length - 1) << 5) | ((offset & 7) << 2))
            self.write_byte(offset >> 3)
        else:
            if length <= 33:
                self.write_byte(0x20 | (length - 2))
            else:
                self.write_byte(0x20)
                self._zero_length(length - 33)
            self.write_byte(offset << 2)
            self.write_byte(offset >> 6)
        return self.size - 2

    def _zero_length(self, length: int):
        while length > 255:
            self.write_byte(0)
            length -= 255
        self.write_byte(length)

    def to_bytes(self) -> bytes:
        return bytes(self.buf[:self.size])

def _lzo_hash(data: bytes, pos: int) -> int:
    val = (data[pos] << 16) | (data[pos + 1] << 8) | data[pos + 2]
    return ((val * 0x9E3779B1) >> 18) & 0x3FFF

def compressed_lzo(raw: bytes) -> bytes:
    """
    真 LZO1X 贪心+哈希链压缩器 (纯 Python 实现，100% 兼容屏端解码器)
    若高熵数据压不小，自动回退到 literal_lzo
    """
    raw_len = len(raw)
    writer = _LzoWriter(raw_len + raw_len // 8 + 80)
    head = [-1] * 16384
    prev = [-1] * raw_len

    lit_start = 0
    pos = 0
    last_match = -1

    while pos + 2 < raw_len:
        best_pos = -1
        best_len = 0
        if pos > 0:
            k = _lzo_hash(raw, pos)
            cand = head[k]
            visited = 0
            while cand >= 0 and (pos - cand) <= 0x4000 and visited < 64:
                visited += 1
                max_len = raw_len - pos
                match_l = 0
                while match_l < max_len and raw[cand + match_l] == raw[pos + match_l]:
                    match_l += 1
                if match_l > best_len or (match_l == best_len and match_l >= 3 and (pos - cand) < (pos - best_pos)):
                    best_len = match_l
                    best_pos = cand
                if best_len == max_len:
                    break
                cand = prev[cand]

        cur = pos
        k = _lzo_hash(raw, cur)
        prev[cur] = head[k]
        head[k] = cur

        if best_len >= 3:
            writer.literal(raw, lit_start, cur - lit_start, last_match)
            offset = cur - best_pos
            last_match = writer.match(best_len, offset)
            for ins in range(cur + 1, min(cur + best_len, raw_len - 2)):
                ki = _lzo_hash(raw, ins)
                prev[ins] = head[ki]
                head[ki] = ins
            pos = cur + best_len
            lit_start = pos
        else:
            pos += 1

    writer.literal(raw, lit_start, raw_len - lit_start, last_match)
    writer.write_byte(0x11)
    writer.write_byte(0)
    writer.write_byte(0)

    compressed = writer.to_bytes()
    literal = literal_lzo(raw)
    return compressed if len(compressed) < len(literal) else literal

def build_d3_packets(packed_image: bytes) -> List[bytes]:
    """
    将 105984 字节的完整点阵切片为 2000 字节块，
    逐块使用 LZO1X 压缩，再拆解为最大 250 字节的 D3 APDU 帧
    """
    packets = []
    total_blocks = (len(packed_image) + 1999) // 2000

    for block in range(total_blocks):
        offset = block * 2000
        chunk = packed_image[offset:min(offset + 2000, len(packed_image))]
        c_chunk = compressed_lzo(chunk)

        sub_count = (len(c_chunk) + 249) // 250
        for sub in range(sub_count):
            sub_off = sub * 250
            cnt = min(250, len(c_chunk) - sub_off)
            is_last = 1 if (sub_off + cnt == len(c_chunk)) else 0

            pkt = bytearray(7 + cnt)
            pkt[0] = 0xF0
            pkt[1] = 0xD3
            pkt[2] = 0x00
            pkt[3] = is_last
            pkt[4] = cnt + 2
            pkt[5] = block
            pkt[6] = sub
            pkt[7:7 + cnt] = c_chunk[sub_off:sub_off + cnt]
            packets.append(bytes(pkt))

    return packets

# ─────────────────── 解包预览 ───────────────────

def unpack_preview_rgb(packed_bytes: bytes, panel_type: str = "A0") -> Image.Image:
    """
    从打包字节回读成 552×768 RGB 图像 (所见即所发，含 A0 反转置)
    """
    raw = bytearray(packed_bytes)
    rows = WIDTH
    stride = HEIGHT // 4

    if panel_type.upper() == "A0":
        # 逆转置
        orig = bytearray(len(raw))
        for row in range(rows):
            target = 2 * row if row < rows // 2 else 2 * rows - 1 - 2 * row
            orig[row * stride:(row + 1) * stride] = raw[target * stride:(target + 1) * stride]
        raw = orig

    # 解码每像素 2bit 颜色 (NumPy 向量化实现)
    raw_arr = np.frombuffer(raw, dtype=np.uint8).reshape(WIDTH, HEIGHT // 4)
    c0 = (raw_arr >> 6) & 3
    c1 = (raw_arr >> 4) & 3
    c2 = (raw_arr >> 2) & 3
    c3 = raw_arr & 3
    codes_rev = np.stack([c0, c1, c2, c3], axis=-1).reshape(WIDTH, HEIGHT)
    codes_2d = codes_rev.T[::-1, ::-1]
    pal_np = np.array(PALETTE_ARGB, dtype=np.uint8)
    img_np = pal_np[codes_2d]

    return Image.fromarray(img_np, 'RGB')
