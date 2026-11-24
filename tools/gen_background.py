#!/usr/bin/env python3
"""生成 ASCII 鱼缸的像素底图 main/aquarium_bg.c。

布局与明暗算法移植自 `赛博水草缸 · 静态草图.html`（30×40 格、每格 16×16 画布像素 =
8×8 屏幕像素）。不追求逐像素一致，但随机序列与绘制规则照抄，所以疏密、层次、
沉木走向和草图一致。

输出：
  main/aquarium_bg.c   调色板（已换字节序，index 0 = 水）+ 8bpp 索引图

用法：
  python3 tools/gen_background.py                       # 程序化生成
  python3 tools/gen_background.py --seed 7
  python3 tools/gen_background.py --preview /tmp/bg.png # 同时导出预览图（含水渐变）
  python3 tools/gen_background.py --from-png art.png    # 用 Aseprite 导出的 8bpp 索引色 PNG
  python3 tools/gen_background.py --stats               # 只统计

约定：
  * 8bpp，禁用抖动，量化按通道取整到 4 的倍数
  * index 0 = 水：运行时不烘，按行算渐变
  * 调色板 RGB565 已做高低字节对调（面板要求高字节先发）
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

W, H = 30, 40
LAMP_PX = 10            # 顶部灯条高度（屏幕像素）
SOIL_TOP = 37           # 底床起始字符行（真机反馈 4 行偏厚，改成 3 行 = 24 px）            # 顶部灯条高度（屏幕像素）；草图是 16，真机上偏宽
ART = 16                 # 画布像素/格（2 倍超采样）
CELL = 8                 # 屏幕像素/格
ART_W, ART_H = W * ART, H * ART
OUT_W, OUT_H = W * CELL, H * CELL

ROOT = Path(__file__).resolve().parents[1]

GREENS = ["#1b5e30", "#2c8a39", "#4cb946", "#7ddc58", "#a8f072"]
WOODS = ["#3a2619", "#4f3220", "#6a4529"]
SOILS = ["#3b2f27", "#4a3b30", "#2c231d", "#5a4838"]
WATER_STOPS = ["#1b6a58", "#0b3340", "#061a22"]
LAMP_STOPS = ["#d8ffb0", "#5fe05a"]
SOIL_BASE = "#14100c"


# ------------------------------------------------------------------ 工具

def hex_rgb(h: str):
    n = int(h[1:], 16)
    return ((n >> 16) & 255, (n >> 8) & 255, n & 255)


def shade(c, k: float):
    return (max(0, min(255, round(c[0] * k))),
            max(0, min(255, round(c[1] * k))),
            max(0, min(255, round(c[2] * k))))


def imul(a: int, b: int) -> int:
    return ((a & 0xFFFFFFFF) * (b & 0xFFFFFFFF)) & 0xFFFFFFFF


def rng_mulberry32(seed: int):
    """与草图完全相同的 mulberry32 序列。"""
    state = seed & 0xFFFFFFFF

    def nxt() -> float:
        nonlocal state
        state = (state + 0x6D2B79F5) & 0xFFFFFFFF
        t = imul(state ^ (state >> 15), 1 | state)
        t = ((t + imul(t ^ (t >> 7), 61 | t)) & 0xFFFFFFFF) ^ t
        return ((t ^ (t >> 14)) & 0xFFFFFFFF) / 4294967296.0

    return nxt


def hr(x: int, y: int, k: int) -> float:
    """草图的确定性哈希，用于叶点位置与亮度。"""
    v = imul((x * 73856093) ^ (y * 19349663) ^ (k * 83492791), 2654435761)
    return (v & 0xFFFFFFFF) / 4294967296.0


def band(y_cell: int) -> float:
    if y_cell < 24:
        return 0.8
    if y_cell >= 31:
        return 1.12
    return 1.0


# ------------------------------------------------------------------ 布局（移植自草图）

class Cell:
    __slots__ = ("kind", "color")

    def __init__(self, kind: str, color):
        self.kind = kind
        self.color = color


def build_cells(seed: int):
    R = rng_mulberry32(seed)
    grid = [[None] * W for _ in range(H)]
    wood_set = {hex_rgb(c) for c in WOODS}
    soil_set = {hex_rgb(c) for c in SOILS}

    def pick(seq):
        return seq[int(R() * len(seq)) % len(seq)]

    def put(x, y, kind, color):
        x, y = round(x), round(y)
        if 0 <= x < W and 2 <= y < H:
            grid[y][x] = Cell(kind, color)

    def stroke(x0, y0, x1, y1, w):
        for i in range(41):
            x = x0 + (x1 - x0) * i / 40
            y = y0 + (y1 - y0) * i / 40
            for d in range(-w, w + 1):
                if R() < 0.92:
                    put(x + d, y, "wood", pick(WOODS))

    def blob(cx, cy, rx, ry, k):
        for y in range(int(cy - ry), int(cy + ry) + 1):
            for x in range(int(cx - rx), int(cx + rx) + 1):
                d = ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2
                if d < 1 and R() < k * (1.2 - d):
                    cur = grid[y][x] if 0 <= y < H and 0 <= x < W else None
                    if cur and hex_rgb(cur.color) in wood_set and R() < 0.55:
                        continue
                    b = (1 - d) * 2.6 + (cy - y) / ry * 0.5 + (R() - 0.5) * 1.2
                    put(x, y, "plant", GREENS[max(0, min(4, int(b)))])

    # 沉木
    stroke(25, 12, 22, 35, 2)
    stroke(21, 19, 27, 13, 1)
    stroke(19, 24, 15, 35, 1)

    # 后景团块
    blob(11, 16, 10, 9, 0.85)
    blob(6, 24, 6, 6, 0.80)
    blob(15, 10, 5, 4, 0.75)
    blob(25, 11, 4, 3.5, 0.70)
    blob(26, 19, 3.5, 4, 0.65)
    blob(22, 15, 3, 3, 0.60)

    # 中景立茎
    def grow(bx, by, hgt):
        for i in range(hgt):
            put(bx + (R() - 0.5) * 1.4, by - i, "plant", pick(GREENS[1:4]))
            if R() < 0.5:
                put(bx + (-1 if R() < 0.5 else 1), by - i, "plant", pick(GREENS[1:4]))

    for args in [(3, 34, 5), (8, 34, 4), (12, 34, 3), (17, 34, 6), (21, 34, 4), (26, 34, 6)]:
        grow(*args)

    # 小水榕
    for (x, y) in [(24, 31), (25, 30), (27, 32), (23, 33)]:
        put(x, y, "plant", pick([GREENS[2], GREENS[3]]))

    # 前景草皮：SOIL_TOP 之上两行铺满，避免收窄底床后出现一条"水沟"
    for x in range(W):
        if R() < 0.95:
            put(x, SOIL_TOP - 1, "plant", pick(["#6fb84a", "#8fd05a", "#a8f072", "#4cb946"]))
        if R() < 0.92:
            put(x, 35, "plant", pick(["#6fb84a", "#8fd05a", "#a8f072", "#4cb946"]))
        if R() < 0.30:
            put(x, 34, "plant", "#6fb84a")

    # 底床
    for y in range(SOIL_TOP, H):
        for x in range(W):
            put(x, y, "soil", pick(SOILS))

    # 空洞填充：邻格植物 ≥4 才补
    filled = [row[:] for row in grid]
    for y in range(2, SOIL_TOP):
        for x in range(W):
            if grid[y][x] is not None:
                continue
            n, nb = 0, None
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    yy, xx = y + dy, x + dx
                    if 0 <= yy < H and 0 <= xx < W:
                        c = grid[yy][xx]
                        if c and c.kind == "plant":
                            n += 1
                            nb = c
            if n >= 4 and nb is not None:
                filled[y][x] = Cell("plant", nb.color)
    return filled


# ------------------------------------------------------------------ 光栅化

def lamp_color(t: float):
    a, b = hex_rgb(LAMP_STOPS[0]), hex_rgb(LAMP_STOPS[1])
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))


def rasterize(cells) -> list:
    """返回 ART_H × ART_W 的 RGB 画布；None 表示水。"""
    canvas = [[None] * ART_W for _ in range(ART_H)]

    def rect(x, y, w, h, color):
        for yy in range(y, y + h):
            if not (0 <= yy < ART_H):
                continue
            row = canvas[yy]
            for xx in range(x, x + w):
                if 0 <= xx < ART_W:
                    row[xx] = color

    # 灯条：LAMP_PX 屏幕像素高（真机反馈 16 px 偏宽，收窄到 10）
    lamp_rows = LAMP_PX * (ART // CELL)
    for y in range(lamp_rows):
        c = lamp_color(y / (lamp_rows - 1))
        for x in range(ART_W):
            canvas[y][x] = c

    # 底床底色（草图里是最底 64 画布像素的实色）
    for y in range(SOIL_TOP * ART, ART_H):
        for x in range(ART_W):
            canvas[y][x] = hex_rgb(SOIL_BASE)

    for y in range(2, H):
        gy = band(y)
        for x in range(W):
            cell = cells[y][x]
            if cell is None:
                continue
            X, Y = x * ART, y * ART
            if cell.kind == "soil":
                rect(X, Y, ART, ART, shade(hex_rgb(cell.color), 0.7 * gy))
                for i in range(3):
                    dx = int(hr(x, y, i) * 7) * 2
                    dy = int(hr(x, y, i + 3) * 7) * 2
                    rect(X + dx, Y + dy, 2, 2, shade(hex_rgb(cell.color), 1.2 * gy))
            elif cell.kind == "wood":
                rect(X, Y, ART, ART, shade(hex_rgb(cell.color), 0.8 * gy))
                for i in range(3):
                    dx = int(hr(x, y, i) * 7) * 2
                    hgt = ART - int(hr(x, y, i + 5) * 8)
                    rect(X + dx, Y, 2, hgt, shade(hex_rgb(cell.color), 1.3 * gy))
            else:  # plant
                base = hex_rgb(cell.color)
                rect(X, Y, ART, ART, shade(base, 0.62 * gy))
                for i in range(6):
                    dx = int(hr(x, y, i) * 6) * 2
                    dy = int(hr(x, y, i + 9) * 6) * 2
                    hi = hr(x, y, i + 20) < 0.30
                    k = 1.35 if hi else (0.92 + hr(x, y, i + 40) * 0.22)
                    wpx = 4 if hr(x, y, i + 60) < 0.5 else 2
                    rect(X + dx, Y + dy, wpx, 2, shade(base, k * gy))
    return canvas


def downsample(canvas):
    """2:1 采样：所有绘制对齐 2 像素，取左上角无损。"""
    return [[canvas[y * 2][x * 2] for x in range(OUT_W)] for y in range(OUT_H)]


# ------------------------------------------------------------------ 量化 / 输出

def quantize(pixels, step: int = 4):
    def q(c):
        return (c[0] // step * step, c[1] // step * step, c[2] // step * step)

    palette = [(0, 0, 0)]          # 0 = 水
    index = {}
    grid = []
    for row in pixels:
        out = []
        for c in row:
            if c is None:
                out.append(0)
                continue
            key = q(c)
            j = index.get(key)
            if j is None:
                j = len(palette)
                if j > 255:
                    raise SystemExit(f"颜色超过 255（step={step}），加大量化步长")
                palette.append(key)
                index[key] = j
            out.append(j)
        grid.append(out)
    return palette, grid


def rgb565_be(c):
    r, g, b = c
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return ((v >> 8) | ((v & 0xFF) << 8)) & 0xFFFF


def write_c(palette, grid, path: Path, meta: str) -> int:
    lines = [
        "// main/aquarium_bg.c —— 生成物，请勿手改。",
        "//",
        f"// 由 tools/gen_background.py 生成：{meta}",
        "//",
        "// index 0 = 水（运行时按行算渐变）；其余为调色板索引。",
        "// 调色板 RGB565 已做高低字节对调（面板要求高字节先发）。",
        '#include "aquarium_bg.h"',
        "",
        f"#define BG_PAL_COUNT {len(palette)}",
        "",
        "const uint16_t s_bg_pal[BG_PAL_COUNT] = {",
    ]
    for i in range(0, len(palette), 8):
        lines.append("    " + ", ".join("0x%04X" % rgb565_be(c) for c in palette[i:i + 8]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("const uint8_t s_bg_idx[BG_H * BG_W] = {")
    flat = [str(v) for row in grid for v in row]
    for i in range(0, len(flat), 32):
        lines.append("    " + ",".join(flat[i:i + 32]) + ",")
    lines.append("};")
    lines.append("")
    text = "\n".join(lines)
    path.write_text(text, encoding="utf-8")
    return len(text)


def write_png_preview(palette, grid, path: Path, scale: int = 2) -> None:
    """预览图：水按固件的三段渐变填充，方便判断整体观感。"""
    w, h = OUT_W * scale, OUT_H * scale
    stops = [hex_rgb(c) for c in WATER_STOPS]
    raw = bytearray()
    for y in range(h):
        row = grid[y // scale]
        t = (y // scale) / (OUT_H - 1)
        seg = min(1, int(t * 2))
        local = t * 2 - seg
        a, b = stops[seg], stops[min(2, seg + 1)]
        water = tuple(round(a[i] + (b[i] - a[i]) * local) for i in range(3))
        raw.append(0)
        for x in range(w):
            idx = row[x // scale]
            raw.extend(water if idx == 0 else palette[idx])

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(
            ">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
           + chunk(b"IEND", b""))
    path.write_bytes(png)


def load_indexed_png(path: Path):
    """读 Aseprite 导出的 8bpp 索引色 PNG（PLTE，可带 tRNS）。"""
    data = path.read_bytes()
    if not data.startswith(b"\x89PNG"):
        raise SystemExit("不是 PNG 文件")
    pos, palette, idat = 8, [], bytearray()
    w = h = depth = ctype = 0
    while pos < len(data):
        (ln,) = struct.unpack(">I", data[pos:pos + 4])
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if tag == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
        elif tag == b"PLTE":
            palette = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif tag == b"IDAT":
            idat += body
        elif tag == b"IEND":
            break
    if ctype != 3 or depth != 8:
        raise SystemExit(f"需要 8bpp 索引色 PNG（depth=8, color_type=3），当前 {depth}/{ctype}")
    if (w, h) != (OUT_W, OUT_H):
        raise SystemExit(f"尺寸必须是 {OUT_W}×{OUT_H}，当前 {w}×{h}")

    raw = zlib.decompress(bytes(idat))
    rows, prev, i = [], bytearray(w), 0
    for _ in range(h):
        ft = raw[i]; i += 1
        line = bytearray(raw[i:i + w]); i += w
        if ft == 1:
            for x in range(1, w):
                line[x] = (line[x] + line[x - 1]) & 0xFF
        elif ft == 2:
            for x in range(w):
                line[x] = (line[x] + prev[x]) & 0xFF
        elif ft == 3:
            for x in range(w):
                a = line[x - 1] if x else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 0xFF
        elif ft == 4:
            for x in range(w):
                a = line[x - 1] if x else 0
                b = prev[x]
                c = prev[x - 1] if x else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 0xFF
        rows.append(line)
        prev = line
    return palette, rows


# ------------------------------------------------------------------ main

def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=11)
    ap.add_argument("--preview", metavar="PNG")
    ap.add_argument("--from-png", metavar="PNG")
    ap.add_argument("--stats", action="store_true")
    ap.add_argument("--step", type=int, default=4, help="量化步长（默认 4，关闭抖动）")
    args = ap.parse_args(argv)

    if args.from_png:
        pal, rows = load_indexed_png(Path(args.from_png))
        palette = [(0, 0, 0)] + pal[1:256]
        grid = [list(r) for r in rows]
        meta = f"source=PNG {Path(args.from_png).name}, palette={len(pal)}"
    else:
        cells = build_cells(args.seed)
        canvas = rasterize(cells)
        palette, grid = quantize(downsample(canvas), args.step)
        meta = f"source=procedural seed={args.seed}, colors={len(palette)}"

    if args.preview:
        write_png_preview(palette, grid, Path(args.preview))

    lamp = sum(1 for y in range(0, LAMP_PX) for v in grid[y] if v)
    soil = sum(1 for y in range(SOIL_TOP * CELL, OUT_H) for v in grid[y] if v)
    scene = sum(1 for y in range(LAMP_PX, SOIL_TOP * CELL) for v in grid[y] if v)
    total = lamp + soil + scene
    print(f"调色板 {len(palette)} 档；非水像素 {total} ({total * 100 // (OUT_W * OUT_H)}%)")
    print(f"  灯条 {lamp}  场景 {scene}  底床 {soil}")
    print(f"  索引图 {OUT_H * OUT_W:,} B + 调色板 {len(palette) * 2} B")

    if args.stats:
        return 0
    out = ROOT / "main/aquarium_bg.c"
    size = write_c(palette, grid, out, meta)
    print(f"已写入 {out.relative_to(ROOT)}（{size:,} 字节源码）")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
