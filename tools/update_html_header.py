import gzip
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent.parent
html_path = ROOT_DIR / "tools" / "epd_studio.html"
header_path = ROOT_DIR / "firmware" / "main" / "epd_studio_html.h"

content = html_path.read_bytes()
compressed = gzip.compress(content, compresslevel=9)

with open(header_path, "w", encoding="utf-8") as f:
    f.write("// Auto-generated from tools/epd_studio.html\n")
    f.write("#ifndef EPD_STUDIO_HTML_H\n")
    f.write("#define EPD_STUDIO_HTML_H\n")
    f.write("#include <stddef.h>\n")
    f.write("#include <stdint.h>\n\n")
    f.write(f"static const size_t EPD_STUDIO_HTML_GZ_LEN = {len(compressed)};\n")
    f.write(f"static const uint8_t EPD_STUDIO_HTML_GZ[] = {{\n")
    for i in range(0, len(compressed), 16):
        chunk = compressed[i:i+16]
        hex_str = ", ".join(f"0x{b:02x}" for b in chunk)
        f.write(f"    {hex_str},\n")
    f.write("};\n\n")
    f.write("#endif // EPD_STUDIO_HTML_H\n")

print(f"Generated {header_path}: {len(compressed)} bytes gzipped from {len(content)} bytes raw.")
