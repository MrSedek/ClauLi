#!/usr/bin/env python3
"""Build firmware/src/emo_eyes_hd.h — procedural squircle eye sprites
for the emo 2.0 screen, matching the Living.ai EMO aesthetic and the
look of daemon/web/emo2-demo.gif.

For each of the 7 moods we render two anti-aliased shapes:

* **base 64×64** — sharp squircle (rounded square) shape with 4× super-
  sampling. Used as the alpha mask for the main eye sprite.
* **halo 128×128** — the same shape rendered at ~80×80 *with 24 px
  padding on every side*, then run through PIL.ImageFilter.GaussianBlur
  with σ≈12 so the bloom fades smoothly to 0 well inside the sprite
  edges (no more boxy/rectangular cutoff against the canvas border).

Plus a small procedural specular highlight (white soft ellipse) for the
lens-shine layer.

Requires Pillow:  `pip install --user Pillow`.

Re-run after touching this script.
"""
from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

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
HALO_SIZE = 112       # halo canvas — larger than base for clean bloom fade
HALO_SHAPE = 64       # shape size *inside* the halo canvas (centered)
HALO_SIGMA = 3        # Gaussian blur σ — small so the rim glow keeps each
                       # form's silhouette (was 10 → smudged everything into
                       # the same generic blob regardless of shape).
HALO_BOOST = 1.0      # no amplification — barely-visible rim glow
SPEC_W, SPEC_H = 14, 10


# ─── mood shape predicates (nx, ny ∈ [-1, 1], y down) ──────────────────────

def mood_inside(mood: str, nx: float, ny: float, *, is_base: bool = True) -> bool:
    """`is_base` controls whether pupil/cross cutouts apply. For halo rendering
    pass is_base=False so the bloom uses the solid outer silhouette."""
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
        # Two perpendicular diagonal bands, half-thickness 0.18.  √2 ≈ 1.41421
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
        # round corners via squircle near edges:
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
        # Two C-shaped brackets `[  ]`. Vertical bar + top/bottom stubs on
        # each side. Mirror-symmetric.
        t = 0.20            # bar/stub thickness
        bar_x = 0.78        # outer edge of bar
        stub_w = 0.45       # stub horizontal reach (inward)
        stub_y = 0.78       # top/bottom stub vertical position
        # vertical bars
        if bar_x - t <= abs(nx) <= bar_x and abs(ny) <= stub_y + t / 2:
            return True
        # top/bottom stubs
        if (stub_y - t / 2 <= abs(ny) <= stub_y + t / 2 and
            bar_x - stub_w <= abs(nx) <= bar_x):
            return True
        return False

    if mood == "PIXEL_CLUSTER":
        # 4×4 grid of square dots.
        grid = 4
        cell = 1.7 / grid       # cell pitch in [-0.85..0.85]
        fill_ratio = 0.65       # fraction of cell that's filled
        if abs(nx) > 0.85 or abs(ny) > 0.85:
            return False
        # quantise to grid
        col = int((nx + 0.85) / cell)
        row = int((ny + 0.85) / cell)
        if col < 0 or col >= grid or row < 0 or row >= grid:
            return False
        cx = -0.85 + (col + 0.5) * cell
        cy = -0.85 + (row + 0.5) * cell
        half = cell * fill_ratio / 2
        return abs(nx - cx) <= half and abs(ny - cy) <= half

    return False


def render_shape(w: int, h: int, mood: str, samples: int = 4,
                 *, is_base: bool = True) -> list[int]:
    """Multi-sampled rasterization of mood_inside into an A8 buffer.
    `is_base=False` strips pupil/cross cutouts so the halo silhouette stays solid."""
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


# ─── high-quality renderers via Pillow ─────────────────────────────────────

def _font_for(size: int) -> ImageFont.ImageFont:
    """Best-effort sans-serif font for glyph cutouts."""
    for path in [
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/SFNS.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    ]:
        try:
            return ImageFont.truetype(path, size)
        except Exception:
            continue
    return ImageFont.load_default()


def render_glyph_base(glyph: str) -> bytes:
    """Squircle base with `glyph` cut out — used for Q_EYE / EXCLAIM."""
    sq = render_shape(BASE_SIZE, BASE_SIZE, "NEUTRAL", samples=4, is_base=True)
    img = Image.frombytes("L", (BASE_SIZE, BASE_SIZE), bytes(sq))
    d = ImageDraw.Draw(img)
    font = _font_for(int(BASE_SIZE * 0.78))
    bbox = d.textbbox((0, 0), glyph, font=font)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    x = (BASE_SIZE - tw) // 2 - bbox[0]
    y = (BASE_SIZE - th) // 2 - bbox[1]
    d.text((x, y), glyph, fill=0, font=font)
    return img.tobytes()


def render_base(mood: str) -> bytes:
    """64×64 base sprite — supersampled shape, or Pillow-glyph cutout."""
    if mood == "Q_EYE":   return render_glyph_base("?")
    if mood == "EXCLAIM": return render_glyph_base("!")
    return bytes(render_shape(BASE_SIZE, BASE_SIZE, mood, samples=4, is_base=True))


def render_halo(mood: str) -> bytes:
    """128×128 halo sprite — shape pasted at 80×80 centred + GaussianBlur σ=12.

    The 24 px padding on every side gives the Gaussian enough room to fade
    smoothly to 0 within the sprite, so the rendered bloom no longer cuts
    off against the rectangular sprite boundary.
    """
    # Render shape at 2× HALO_SHAPE for AA, downsample with LANCZOS.
    # is_base=False → no pupil/cross cutouts in the bloom layer.
    # Glyph-mood halos reuse the plain NEUTRAL squircle (the glyph is only
    # visible on the base layer — bloom is full).
    halo_mood = "NEUTRAL" if mood in ("Q_EYE", "EXCLAIM") else mood
    src = render_shape(HALO_SHAPE * 2, HALO_SHAPE * 2, halo_mood, samples=3, is_base=False)
    shape_img = Image.frombytes("L", (HALO_SHAPE * 2, HALO_SHAPE * 2), bytes(src))
    shape_img = shape_img.resize((HALO_SHAPE, HALO_SHAPE), Image.LANCZOS)

    # Paste centered onto an empty (alpha = 0) canvas with padding.
    canvas = Image.new("L", (HALO_SIZE, HALO_SIZE), 0)
    pad = (HALO_SIZE - HALO_SHAPE) // 2
    canvas.paste(shape_img, (pad, pad))

    # True Gaussian blur — smooth falloff, no boxy edges.
    blurred = canvas.filter(ImageFilter.GaussianBlur(radius=HALO_SIGMA))

    # Mild brightness boost to recover a bit of energy the blur spreads out.
    if HALO_BOOST != 1.0:
        blurred = blurred.point(lambda v: min(255, int(v * HALO_BOOST + 0.5)))

    return blurred.tobytes()


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


def main() -> None:
    chunks: list[str] = [
        "// Generated by tools/build_emo_hd.py — DO NOT EDIT.",
        "// Procedural squircle eye sprites for the emo 2.0 screen (emo2.cpp).",
        f"// base {BASE_SIZE}×{BASE_SIZE} + halo {HALO_SIZE}×{HALO_SIZE}",
        f"// halo: shape rendered at {HALO_SHAPE}×{HALO_SHAPE} centred,",
        f"//       then PIL GaussianBlur σ={HALO_SIGMA} (×{HALO_BOOST} boost)",
        "//       so the bloom fades smoothly to 0 *inside* the sprite,",
        "//       no rectangular cutoff against the canvas border.",
        "#pragma once",
        "#include <lvgl.h>",
    ]

    for mood in MOODS:
        base = render_base(mood)
        halo = render_halo(mood)

        m = mood.lower()
        chunks.append(emit_array(f"mood_{m}_base_map", base, BASE_SIZE, BASE_SIZE))
        chunks.append(emit_dsc(f"emo_img_mood_{m}_base", f"mood_{m}_base_map",
                               BASE_SIZE, BASE_SIZE))
        chunks.append(emit_array(f"mood_{m}_halo_map", halo, HALO_SIZE, HALO_SIZE))
        chunks.append(emit_dsc(f"emo_img_mood_{m}_halo", f"mood_{m}_halo_map",
                               HALO_SIZE, HALO_SIZE))

    spec = soft_ellipse(SPEC_W, SPEC_H)
    chunks.append(emit_array("emo_spec_map", spec, SPEC_W, SPEC_H))
    chunks.append(emit_dsc("emo_img_spec", "emo_spec_map", SPEC_W, SPEC_H))

    # Emit base/halo frame tables programmatically — keeps the C size in
    # sync with the MOODS list (now 11 entries with the picked candidates).
    base_rows = "\n".join(
        f"    {{ &emo_img_mood_{m.lower()}_base, &emo_img_mood_{m.lower()}_base }}, // {m}"
        for m in MOODS)
    halo_rows = "\n".join(
        f"    {{ &emo_img_mood_{m.lower()}_halo, &emo_img_mood_{m.lower()}_halo }},"
        for m in MOODS)
    n = len(MOODS)
    chunks.append(
        f"// Mood → {{left, right}} frame table. Order matches emo_mood_t.\n"
        f"static const lv_image_dsc_t* const emo2_base_frames[{n}][2] = {{\n"
        f"{base_rows}\n"
        f"}};\n"
        f"static const lv_image_dsc_t* const emo2_halo_frames[{n}][2] = {{\n"
        f"{halo_rows}\n"
        f"}};\n"
    )

    OUT.write_text("\n".join(chunks) + "\n")
    print(f"wrote {OUT}  ({OUT.stat().st_size} bytes, {len(MOODS)} moods, "
          f"halo {HALO_SIZE}×{HALO_SIZE})")


if __name__ == "__main__":
    main()
