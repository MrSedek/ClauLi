#!/usr/bin/env python3
"""Build firmware/src/emo_eyes_hd.h — procedural A8 eye sprites for the
emo 2.0 screen, for THREE switchable characters (ClauLi / Pixl / Old-TV).

For each of the 21 moods we render a 64×64 anti-aliased base silhouette
(4× supersampled), then derive the per-character variants:

* **ClauLi** — the sharp squircle silhouette (default).
* **Pixl**   — the silhouette quantised to a coarse LCD-style cell grid.
* **Old-TV** — the silhouette with horizontal scanline falloff (CRT look).

Plus a small procedural specular highlight (white soft ellipse).

PIL-free — pure-python rasterisation (no Pillow). The Q_EYE / EXCLAIM
glyphs are procedural bitmaps cut out of the squircle. The old 112×112
GaussianBlur halo table was unused by emo2.cpp and has been dropped.

Re-run after touching this script.
"""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "firmware" / "src" / "emo_eyes_hd.h"

# Mood names in emo_mood_t order (mirror emo2.cpp). Append-only — the
# enum in emo2.cpp indexes into emo2_base_frames[] by ordinal.
MOODS = ["HAPPY", "NEUTRAL", "SLEEP", "ANGRY", "UPSET", "SAD", "LOVE",
         "CIRCLE", "PUPIL_LEFT", "PUPIL_SLIT", "CROSS",
         "OVAL_TALL", "DIAMOND",
         "SQUIRCLE_THICK", "RECT_TV", "CAPSULE_H", "CRESCENT",
         "BRACKETS", "PIXEL_CLUSTER", "Q_EYE", "EXCLAIM"]

# Sprite resolutions used by emo2.cpp.
BASE_SIZE = 64        # base eye sprite
SPEC_W, SPEC_H = 14, 10

# Pixl: cell grid (MUST divide 64 evenly → uniform dots) + fill threshold.
PIX_GRID = 8             # 64/8 = 8px even cells → no rounding drift / uneven dots
PIX_THRESH = 100


# ─── procedural glyph bitmaps for Q_EYE / EXCLAIM (PIL-free) ────────────────
_GLYPH_Q = [
    "0111110",
    "1100011",
    "0000011",
    "0000110",
    "0001100",
    "0001000",
    "0000000",
    "0001000",
    "0001000",
]
_GLYPH_EXCL = [
    "010",
    "010",
    "010",
    "010",
    "010",
    "010",
    "000",
    "010",
    "010",
]


def _glyph_ink(bm, nx: float, ny: float) -> bool:
    """True if (nx,ny)∈[-1,1] falls on an 'ink' cell of the centred glyph."""
    rows = len(bm); cols = len(bm[0])
    gx = (nx + 0.42) / 0.84
    gy = (ny + 0.60) / 1.20
    if gx < 0 or gx >= 1 or gy < 0 or gy >= 1:
        return False
    return bm[int(gy * rows)][int(gx * cols)] == "1"


# ─── mood shape predicates (nx, ny ∈ [-1, 1], y down) ──────────────────────

def mood_inside(mood: str, nx: float, ny: float, *, is_base: bool = True) -> bool:
    """`is_base` controls whether pupil/cross cutouts apply."""
    if mood == "NEUTRAL":
        return abs(nx) ** 4 + abs(ny) ** 4 <= 1.0

    if mood == "HAPPY":
        # Thin TOP arc ⌒ — closed-eye joy smile, clearly distinct from NEUTRAL.
        if ny > 0.05:
            return False
        return abs(nx) ** 4 + abs(ny * 2.2) ** 4 <= 1.0

    if mood == "SLEEP":
        # Thin horizontal slit (closed eyes).
        return abs(nx * 1.02) ** 4 + abs(ny * 5.0) ** 4 <= 1.0

    if mood == "ANGRY":
        # Lower 60 % of squircle, hard top cut — furrowed-brow look.
        if abs(nx) ** 4 + abs(ny) ** 4 > 1.0:
            return False
        return ny > -0.10

    if mood == "UPSET":
        # Small concerned squircle — shrunken to ~50 %, dot-like worry.
        return abs(nx * 2.0) ** 4 + abs(ny * 2.0) ** 4 <= 1.0

    if mood == "SAD":
        # Thin BOTTOM arc ⌣ — mirror of HAPPY, classic frown eye.
        if ny < -0.05:
            return False
        return abs(nx) ** 4 + abs(ny * 2.2) ** 4 <= 1.0

    if mood == "LOVE":
        px = nx * 1.3
        py = -ny * 1.3 - 0.10
        f = (px * px + py * py - 1.0) ** 3 - px * px * (py ** 3)
        return f <= 0.0

    if mood == "CIRCLE":
        # Perfect circle — soft, rounder than the squircle baseline.
        return nx * nx + ny * ny <= 1.0

    if mood == "PUPIL_LEFT":
        # Squircle with a circular pupil hole shifted left — "looking aside".
        if abs(nx) ** 4 + abs(ny) ** 4 > 1.0:
            return False
        if is_base:
            px2, py2 = nx + 0.30, ny
            if px2 * px2 + py2 * py2 <= 0.30 * 0.30:
                return False
        return True

    if mood == "PUPIL_SLIT":
        # Squircle with a vertical-slit pupil — cat-like / focused.
        if abs(nx) ** 4 + abs(ny) ** 4 > 1.0:
            return False
        if is_base:
            if (nx / 0.10) ** 2 + (ny / 0.78) ** 2 <= 1.0:
                return False
        return True

    if mood == "CROSS":
        # X-shape within the unit disc — shocked / dizzy / KO'd.
        if nx * nx + ny * ny > 1.0:
            return False
        d1 = abs(nx - ny) / 1.41421
        d2 = abs(nx + ny) / 1.41421
        return d1 < 0.18 or d2 < 0.18

    if mood == "OVAL_TALL":
        # Vertically-stretched ellipse — alert / wide-eyed.
        return (nx / 0.78) ** 2 + (ny / 1.10) ** 2 <= 1.0

    if mood == "DIAMOND":
        # L1-norm unit disc with mild horizontal squash — sharp rhombus.
        return abs(nx) / 0.85 + abs(ny) <= 1.0

    if mood == "SQUIRCLE_THICK":
        # n=8 squircle — closer to a rounded rectangle.
        return abs(nx) ** 8 + abs(ny) ** 8 <= 1.0

    if mood == "RECT_TV":
        # Rounded rectangle, wider than tall (TV-era).
        if abs(nx) > 1.0 or abs(ny) > 0.72:
            return False
        ax = max(0.0, abs(nx) - 0.78)
        ay = max(0.0, abs(ny) - 0.50)
        if ax == 0 or ay == 0:
            return True
        return (ax / 0.22) ** 2 + (ay / 0.22) ** 2 <= 1.0

    if mood == "CAPSULE_H":
        # Horizontal pill — long ellipse stretched horizontally.
        if abs(ny) > 0.40:
            return False
        ax = max(0.0, abs(nx) - 0.55)
        return (ax / 0.45) ** 2 + (ny / 0.40) ** 2 <= 1.0

    if mood == "CRESCENT":
        # Outer disc minus an offset inner disc on the right side.
        if nx * nx + ny * ny > 1.0:
            return False
        ix, iy = nx - 0.35, ny
        if ix * ix + iy * iy < 0.85 * 0.85:
            return False
        return True

    if mood == "BRACKETS":
        # Two C-shaped brackets `[  ]`. Vertical bar + top/bottom stubs.
        t = 0.20
        bar_x = 0.78
        stub_w = 0.45
        stub_y = 0.78
        if bar_x - t <= abs(nx) <= bar_x and abs(ny) <= stub_y + t / 2:
            return True
        if (stub_y - t / 2 <= abs(ny) <= stub_y + t / 2 and
            bar_x - stub_w <= abs(nx) <= bar_x):
            return True
        return False

    if mood == "PIXEL_CLUSTER":
        # 4×4 grid of square dots.
        grid = 4
        cell = 1.7 / grid
        fill_ratio = 0.65
        if abs(nx) > 0.85 or abs(ny) > 0.85:
            return False
        col = int((nx + 0.85) / cell)
        row = int((ny + 0.85) / cell)
        if col < 0 or col >= grid or row < 0 or row >= grid:
            return False
        cx = -0.85 + (col + 0.5) * cell
        cy = -0.85 + (row + 0.5) * cell
        half = cell * fill_ratio / 2
        return abs(nx - cx) <= half and abs(ny - cy) <= half

    if mood == "Q_EYE":
        # Squircle with a procedural "?" cut out.
        if abs(nx) ** 4 + abs(ny) ** 4 > 1.0:
            return False
        return not (is_base and _glyph_ink(_GLYPH_Q, nx, ny))

    if mood == "EXCLAIM":
        # Squircle with a procedural "!" cut out.
        if abs(nx) ** 4 + abs(ny) ** 4 > 1.0:
            return False
        return not (is_base and _glyph_ink(_GLYPH_EXCL, nx, ny))

    return False


def render_shape(w: int, h: int, mood: str, samples: int = 4,
                 *, is_base: bool = True) -> list[int]:
    """Multi-sampled rasterization of mood_inside into an A8 buffer."""
    out = [0] * (w * h)
    inv = 1.0 / samples
    ss2 = samples * samples
    half = ss2 // 2
    for y in range(h):
        for x in range(w):
            n = 0
            for sy in range(samples):
                fy = (y + (sy + 0.5) * inv) / h * 2.0 - 1.0
                for sx in range(samples):
                    fx = (x + (sx + 0.5) * inv) / w * 2.0 - 1.0
                    if mood_inside(mood, fx, fy, is_base=is_base):
                        n += 1
            out[y * w + x] = (n * 255 + half) // ss2
    return out


def render_base(mood: str) -> bytes:
    """64×64 ClauLi base sprite — supersampled silhouette."""
    return bytes(render_shape(BASE_SIZE, BASE_SIZE, mood, samples=4, is_base=True))


# ─── per-character derivations (pure python) ────────────────────────────────

def quantize(buf: bytes, w: int = BASE_SIZE, h: int = BASE_SIZE,
             grid: int = PIX_GRID, thresh: int = PIX_THRESH) -> bytes:
    """Pixl: snap the silhouette to a coarse LCD cell grid. Uses INTEGER even
    cells (grid divides 64) so every dot is the same size with the same gap —
    no rounding drift that made the old float-grid look uneven on device."""
    cell = w // grid                 # 8 px for grid=8 (even)
    pad = 1 if cell <= 8 else 2      # uniform gap → uniform dot
    out = [0] * (w * h)
    for gy in range(grid):
        y0 = gy * cell
        for gx in range(grid):
            x0 = gx * cell
            s = 0
            for y in range(y0, y0 + cell):
                base = y * w
                for x in range(x0, x0 + cell):
                    s += buf[base + x]
            if s // (cell * cell) > thresh:
                for y in range(y0 + pad, y0 + cell - pad):
                    base = y * w
                    for x in range(x0 + pad, x0 + cell - pad):
                        out[base + x] = 255
    return bytes(out)


def dilate(buf: bytes, w: int = BASE_SIZE, h: int = BASE_SIZE,
           iters: int = 4) -> bytes:
    """3×3 max-filter → rounder / gummier silhouette (kept for future use)."""
    cur = list(buf)
    for _ in range(iters):
        nxt = list(cur)
        for y in range(h):
            for x in range(w):
                m = cur[y * w + x]
                for dy in (-1, 0, 1):
                    yy = y + dy
                    if yy < 0 or yy >= h:
                        continue
                    rb = yy * w
                    for dx in (-1, 0, 1):
                        xx = x + dx
                        if 0 <= xx < w:
                            v = cur[rb + xx]
                            if v > m:
                                m = v
                nxt[y * w + x] = m
        cur = nxt
    return bytes(cur)


# ─── specular ──────────────────────────────────────────────────────────────

def soft_ellipse(w: int, h: int) -> bytes:
    cx = (w - 1) / 2; cy = (h - 1) / 2
    rx = w / 2; ry = h / 2
    out = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            nx = (x - cx) / rx; ny = (y - cy) / ry
            d = nx * nx + ny * ny
            if d < 1:
                out[y * w + x] = int(255 * (1 - d) ** 1.6 + 0.5)
    return bytes(out)


# ─── Pixl patterns (MUST match daemon/web PIX_PATTERNS + PIX_FORM_MAP) ───────
# Device Pixl sprites are rendered from the SAME 7×7 hand patterns the web uses,
# so the on-device dot-matrix matches the web preview exactly.
PIX_PATTERNS = {
    "neutral": ['0111110','1111111','1111111','1111111','1111111','1111111','0111110'],
    "happy":   ['0011100','0111110','1111111','1110111','1100011','1000001','0000000'],
    "sad":     ['0000000','1000001','1100011','1110111','1111111','0111110','0011100'],
    "ring":    ['0011100','0111110','1110111','1100011','1110111','0111110','0011100'],
    "heart":   ['0110110','1111111','1111111','1111111','0111110','0011100','0001000'],
    "closed":  ['0000000','0000000','0000000','0000000','1111111','1111111','0111110'],
    "squint":  ['0000000','0000000','0111110','1111111','1111111','0000000','0000000'],
    "spark":   ['0001000','0001000','0011100','1111111','0011100','0001000','0001000'],
    "angry":   ['0000001','0000111','0011111','0111111','1111111','1111111','0111110'],
    # extra distinct shapes so pixl forms don't all collapse to 'neutral'
    "tall":    ['0011100','0011100','0111110','0111110','0111110','0011100','0011100'],
    "diamond": ['0001000','0011100','0111110','1111111','0111110','0011100','0001000'],
    "wide":    ['0000000','1111111','1111111','1111111','1111111','1111111','0000000'],
    "slit":    ['0111110','0110110','0110110','0110110','0110110','0110110','0111110'],
    "crossx":  ['1000001','0100010','0011100','0001000','0011100','0100010','1000001'],
    "crescent":['0111110','1100000','1100000','1100000','1100000','1100000','0111110'],
    "brackets":['1110111','1000001','1000001','0000000','1000001','1000001','1110111'],
}
# Mood (emo_mood_t name) → pattern key. Mirrors web PIX_FORM_MAP for shared forms.
MOOD_PIX = {
    "HAPPY":"happy", "NEUTRAL":"neutral", "SLEEP":"closed", "ANGRY":"angry",
    "UPSET":"squint", "SAD":"sad", "LOVE":"heart", "CIRCLE":"ring",
    "PUPIL_LEFT":"neutral", "PUPIL_SLIT":"slit", "CROSS":"crossx",
    "OVAL_TALL":"tall", "DIAMOND":"diamond", "SQUIRCLE_THICK":"neutral",
    "RECT_TV":"wide", "CAPSULE_H":"wide", "CRESCENT":"crescent",
    "BRACKETS":"brackets", "PIXEL_CLUSTER":"spark", "Q_EYE":"neutral", "EXCLAIM":"spark",
}


def render_pixl(pattern, w: int = BASE_SIZE, h: int = BASE_SIZE) -> bytes:
    """Render a 7×7 dot pattern into a 64×64 A8 sprite with INTEGER pitch so
    every dot and every gap is EXACTLY the same size. (A float pitch of 64/7 ≈
    9.14 rounded each dot to integer pixels with ±1px drift → visibly uneven
    spacing on the device's hard A8 pixels.)"""
    grid = len(pattern)              # 7
    # pitch + offset are MULTIPLES OF 4 so that after the firmware's 64→80
    # (×1.25) scale every cell origin lands on an exact integer screen pixel.
    # Combined with nearest-neighbour scaling (antialias off for Pixl in
    # emo2.cpp) this yields perfectly uniform, crisp squares on the device.
    pitch = 8                        # 7×8 = 56  (×1.25 = 70, integer)
    dot = 6                          # chunky LCD dot
    inset = (pitch - dot) // 2       # 1
    span = grid * pitch              # 56
    off = (w - span) // 2            # 4  (mult of 4)
    out = bytearray(w * h)
    for gy in range(grid):
        row = pattern[gy]
        ry = off + gy * pitch + inset
        for gx in range(grid):
            if row[gx] != '1':
                continue
            rx = off + gx * pitch + inset
            for y in range(ry, ry + dot):
                base = y * w
                for x in range(rx, rx + dot):
                    out[base + x] = 255
    return bytes(out)


def render_tv(buf: bytes, w: int = BASE_SIZE, h: int = BASE_SIZE) -> bytes:
    """Old-TV / CRT Hero: keep the ClauLi mood silhouette but lay horizontal
    scan-lines over it (every 3rd row dimmed) plus a faint vertical brightness
    falloff, so each eye reads as a little glowing CRT screen. Stays A8 — the
    firmware tints it green→amber→red by usage %, like the other characters."""
    out = bytearray(buf)
    denom = (h - 1) if h > 1 else 1
    for y in range(h):
        gap  = (y % 4 == 3)                                   # raster gap line (4 src rows × 1.25 scale = 5 screen rows → uniform)
        fall = 1.0 - 0.18 * abs((y / denom) - 0.5) * 2.0      # screen curvature
        for x in range(w):
            i = y * w + x
            a = out[i]
            if a == 0:
                continue
            v = a * fall * (0.42 if gap else 1.0)             # dim the scan-line gaps
            out[i] = max(0, min(255, int(v)))
    return bytes(out)


# ─── C emitters ────────────────────────────────────────────────────────────

def emit_array(name: str, data: bytes, w: int, h: int) -> str:
    lines = [f"static const uint8_t {name}[] = {{"]
    for y in range(h):
        row = data[y * w : (y + 1) * w]
        lines.append("    " + ",".join(f"0x{v:02x}" for v in row) + ",")
    lines.append("};")
    return "\n".join(lines)


def emit_dsc(name: str, map_name: str, w: int, h: int) -> str:
    return (
        f"static const lv_image_dsc_t {name} = {{\n"
        f"    .header = {{ .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8,\n"
        f"                .flags = 0, .w = {w}, .h = {h}, .stride = {w} }},\n"
        f"    .data_size = {w * h},\n"
        f"    .data = {map_name},\n"
        f"}};"
    )


def _table(varname: str, suffix: str) -> str:
    rows = "\n".join(
        f"    {{ &emo_img_mood_{m.lower()}{suffix}_base, &emo_img_mood_{m.lower()}{suffix}_base }}, // {m}"
        for m in MOODS)
    return (f"static const lv_image_dsc_t* const {varname}[{len(MOODS)}][2] = {{\n"
            f"{rows}\n}};")


def main() -> None:
    chunks: list[str] = [
        "// Generated by tools/build_emo_hd.py — DO NOT EDIT.",
        "// Procedural A8 eye sprites for the emo 2.0 screen (emo2.cpp).",
        "// Three characters: ClauLi (squircle) / Pixl (LCD grid) / Old-TV (scanlines).",
        f"// base {BASE_SIZE}×{BASE_SIZE}, spec {SPEC_W}×{SPEC_H}.",
        "// No halo table (was unused). PIL-free generator.",
        "#pragma once",
        "#include <lvgl.h>",
    ]

    # Render ClauLi bases once, derive Pixl (quantize) + Old-TV (scanlines).
    clauli = {m: render_base(m) for m in MOODS}
    styles = [
        ("", clauli),
        ("_pixl", {m: render_pixl(PIX_PATTERNS[MOOD_PIX[m]]) for m in MOODS}),
        ("_tv",   {m: render_tv(clauli[m]) for m in MOODS}),
    ]

    for suffix, table in styles:
        for m in MOODS:
            ml = m.lower()
            chunks.append(emit_array(f"mood_{ml}{suffix}_base_map", table[m], BASE_SIZE, BASE_SIZE))
            chunks.append(emit_dsc(f"emo_img_mood_{ml}{suffix}_base",
                                   f"mood_{ml}{suffix}_base_map", BASE_SIZE, BASE_SIZE))

    # Specular highlight.
    chunks.append(emit_array("emo_spec_map", soft_ellipse(SPEC_W, SPEC_H), SPEC_W, SPEC_H))
    chunks.append(emit_dsc("emo_img_spec", "emo_spec_map", SPEC_W, SPEC_H))

    # Per-character frame tables (order matches emo_mood_t).
    chunks.append("// Mood → {left,right} frame tables. Order matches emo_mood_t.")
    chunks.append(_table("emo2_base_frames", ""))
    chunks.append(_table("emo2_base_frames_pixl", "_pixl"))
    chunks.append(_table("emo2_base_frames_tv",   "_tv"))

    OUT.write_text("\n".join(chunks) + "\n")
    print(f"wrote {OUT}  ({OUT.stat().st_size} bytes, {len(MOODS)} moods × 3 characters, no halo)")


if __name__ == "__main__":
    main()
