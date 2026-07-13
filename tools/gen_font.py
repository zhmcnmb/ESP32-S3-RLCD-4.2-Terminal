#!/usr/bin/env python3
"""生成 16x16 中文点阵字库 C 源码。

用法:
    conda activate esp-idf-tools
    python3 tools/gen_font.py

字符来源(自动合并去重):
  1. 自动扫描 firmware/ 下所有 .c/.h 的**字符串字面量**里的中文(注释里的不算)
  2. tools/charset.txt 手工补充——用于运行时才出现、源码里没有字面量的文案
     (如天气API返回的"多云转晴")

自动扫描是为了杜绝"新增文案忘了加字导致屏幕显示方框"这类人为疏漏。

输出: firmware/components/display_st7305/font_cn16.c
"""

import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
FIRMWARE = ROOT / "firmware"
CHARSET = ROOT / "tools" / "charset.txt"
OUTPUT = ROOT / "firmware" / "components" / "display_st7305" / "font_cn16.c"

FONT_PATH = "/System/Library/Fonts/STHeiti Medium.ttc"
SIZE = 16

# 先剥离注释，再提取字符串字面量，避免注释中的中文混入
COMMENT_RE = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)
STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
CJK_RE = re.compile(r"[一-鿿　-〿＀-￯]")


def scan_source_chars() -> set[str]:
    """扫描固件源码字符串字面量里的中文。"""
    chars: set[str] = set()
    for path in FIRMWARE.rglob("*"):
        if path.suffix not in (".c", ".h"):
            continue
        # 生成物本身不扫描，否则字库会自我固化
        if path.name.startswith("font_cn16"):
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue

        stripped = COMMENT_RE.sub(" ", text)
        for literal in STRING_RE.findall(stripped):
            chars.update(CJK_RE.findall(literal))
    return chars


def load_manual_chars() -> set[str]:
    """charset.txt 手工补充的字符。"""
    if not CHARSET.exists():
        return set()
    chars: set[str] = set()
    for line in CHARSET.read_text(encoding="utf-8").splitlines():
        if line.startswith("#"):
            continue
        chars.update(CJK_RE.findall(line))
    return chars


def render(ch: str, font: ImageFont.FreeTypeFont) -> list[int]:
    """渲染单字为 32 字节: 16行 × 每行2字节(bit7为最左像素)。"""
    img = Image.new("1", (SIZE, SIZE), 0)
    draw = ImageDraw.Draw(img)

    bbox = draw.textbbox((0, 0), ch, font=font)
    x = (SIZE - (bbox[2] - bbox[0])) // 2 - bbox[0]
    y = (SIZE - (bbox[3] - bbox[1])) // 2 - bbox[1]
    draw.text((x, y), ch, font=font, fill=1)

    data = []
    px = img.load()
    for row in range(SIZE):
        for byte_idx in range(2):
            byte = 0
            for bit in range(8):
                col = byte_idx * 8 + bit
                if px[col, row]:
                    byte |= 1 << (7 - bit)
            data.append(byte)
    return data


def main() -> int:
    from_source = scan_source_chars()
    from_manual = load_manual_chars()
    chars = sorted(from_source | from_manual, key=ord)

    if not chars:
        print("没有找到任何中文字符", file=sys.stderr)
        return 1

    try:
        font = ImageFont.truetype(FONT_PATH, SIZE)
    except OSError:
        print(f"找不到字体: {FONT_PATH}", file=sys.stderr)
        return 1

    lines = [
        "/* 本文件由 tools/gen_font.py 自动生成，请勿手工编辑。",
        " * 字符集 = 源码字符串字面量中的中文 + tools/charset.txt 手工补充。",
        " * 新增中文文案后重跑: python3 tools/gen_font.py",
        f" * 共 {len(chars)} 字，16x16 点阵，每字 32 字节。 */",
        "",
        '#include "font_cn16.h"',
        "",
        f"const int FONT_CN16_COUNT = {len(chars)};",
        "",
        "/* 按 Unicode 码点升序，固件侧二分查找 */",
        "const font_cn16_glyph_t FONT_CN16[] = {",
    ]

    for ch in chars:
        data = render(ch, font)
        hexes = ", ".join(f"0x{b:02X}" for b in data)
        lines.append(f"    {{ 0x{ord(ch):04X}, {{ {hexes} }} }},  /* {ch} */")

    lines.append("};")
    lines.append("")

    OUTPUT.write_text("\n".join(lines), encoding="utf-8")

    size_bytes = len(chars) * 34
    print(f"生成 {OUTPUT.relative_to(ROOT)}")
    print(f"  源码扫描 {len(from_source)} 字，手工补充 {len(from_manual - from_source)} 字")
    print(f"  合计 {len(chars)} 字，约 {size_bytes / 1024:.1f} KB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
