#!/usr/bin/env python3
"""Convert a university JPG/PNG to a native LVGL v9 RGB565 C image."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

try:
    from PIL import Image
except ImportError:
    print("ERROR: Pillow is required.", file=sys.stderr)
    print("Install it with: python3 -m pip install Pillow", file=sys.stderr)
    raise SystemExit(2)

MAX_WIDTH = 320
MAX_HEIGHT = 160


def rgb565_bytes(img: Image.Image) -> bytes:
    out = bytearray()
    raw = img.tobytes()
    for index in range(0, len(raw), 3):
        r, g, b = raw[index], raw[index + 1], raw[index + 2]
        value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        # ESP32-P4 is little-endian; LVGL RGB565 expects native 16-bit order.
        out.append(value & 0xFF)
        out.append((value >> 8) & 0xFF)
    return bytes(out)


def format_bytes(data: bytes) -> str:
    rows = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        rows.append("    " + ", ".join(f"0x{x:02X}" for x in chunk) + ",")
    return "\n".join(rows)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("output_c", type=Path)
    args = parser.parse_args()

    if not args.image.is_file():
        print(f"ERROR: Logo file not found: {args.image}", file=sys.stderr)
        return 1

    with Image.open(args.image) as source:
        img = source.convert("RGB")
        img.thumbnail((MAX_WIDTH, MAX_HEIGHT), Image.Resampling.LANCZOS)

    width, height = img.size
    if width <= 0 or height <= 0:
        print("ERROR: Invalid image dimensions", file=sys.stderr)
        return 1

    data = rgb565_bytes(img)
    args.output_c.parent.mkdir(parents=True, exist_ok=True)

    content = f'''#include "assets/university_logo.h"\n\n/*\n * AUTO-GENERATED from {args.image.name}.\n * Native RGB565: no JPG decoder and no runtime file I/O are required.\n * Size: {width}x{height}, {len(data)} bytes.\n */\nstatic const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST uint8_t university_logo_map[] = {{\n{format_bytes(data)}\n}};\n\nconst lv_image_dsc_t university_logo_image = {{\n    .header = {{\n        .magic = LV_IMAGE_HEADER_MAGIC,\n        .cf = LV_COLOR_FORMAT_RGB565,\n        .flags = 0,\n        .w = {width},\n        .h = {height},\n        .stride = {width * 2},\n        .reserved_2 = 0,\n    }},\n    .data_size = sizeof(university_logo_map),\n    .data = university_logo_map,\n    .reserved = NULL,\n}};\n\nconst bool university_logo_available = true;\n'''
    args.output_c.write_text(content, encoding="utf-8")

    print("University logo converted successfully.")
    print(f"  Input : {args.image}")
    print(f"  Output: {args.output_c}")
    print(f"  Size  : {width} x {height}")
    print(f"  RGB565: {len(data)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
