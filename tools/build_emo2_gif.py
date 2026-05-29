#!/usr/bin/env python3
"""Render an animated GIF preview of the emo 2.0 screen.

Uses the same procedural mood shapes as build_emo_hd.py so the GIF
matches what's on the device pixel-for-shape. Renders the bloom halo
with PIL.ImageFilter.GaussianBlur (proper Gaussian, not box-blur) so the
glow looks like the canvas demos on emo2-landing.html, not stair-stepped.

Output: daemon/web/emo2-demo.gif.

Requires Pillow: `pip install --user Pillow`.
"""
from __future__ import annotations

import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_emo_hd import render_shape  # noqa: E402

from PIL import Image, ImageDraw, ImageFilter  # noqa: E402

OUT = ROOT / "daemon" / "web" / "emo2-demo.gif"

# ─── canvas / layout ───────────────────────────────────────────────────────
W, H = 480, 240
EYE_BASE = 104        # base eye on-screen size
EYE_HALO = 168        # halo canvas (extends padding for the bloom)
SPEC_W, SPEC_H = 24, 18
EYE_GAP = 32
EYE_CY = H // 2 + 6
EYE_L_CX = W // 2 - EYE_BASE // 2 - EYE_GAP // 2
EYE_R_CX = W // 2 + EYE_BASE // 2 + EYE_GAP // 2

CYAN = (61, 224, 224)
AMBER = (255, 176, 46)
RED = (255, 77, 77)
WHITE = (255, 255, 255)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def halo_color(pct: float):
    if pct < 70: return CYAN
    if pct < 90: return lerp(CYAN, AMBER, (pct - 70) / 20)
    return lerp(AMBER, RED, min(1.0, (pct - 90) / 10))


# ─── precompute one base mask + one halo bloom per mood ───────────────────
MOODS = ["HAPPY", "NEUTRAL", "SLEEP", "ANGRY", "UPSET", "SAD", "LOVE",
         "CIRCLE", "PUPIL_LEFT", "PUPIL_SLIT", "CROSS"]
BASE: dict[str, Image.Image] = {}
HALO: dict[str, Image.Image] = {}  # white "energy" mask, recolour per frame

print("rendering shape masks…", file=sys.stderr)
for m in MOODS:
    # Base: high-quality shape at base size, used as alpha mask for tint.
    src_w = EYE_BASE * 2
    buf = render_shape(src_w, src_w, m, samples=3)
    big = Image.frombytes("L", (src_w, src_w), bytes(buf))
    base_mask = big.resize((EYE_BASE, EYE_BASE), Image.LANCZOS)
    BASE[m] = base_mask

    # Halo: render the SAME shape onto a larger transparent canvas, then
    # Gaussian-blur it. Smooth falloff, no boxy edges.
    # For pupil/cross moods, halo must be solid (no cutout) — render its own
    # solid mask via is_base=False so the bloom is even.
    solid = render_shape(EYE_BASE, EYE_BASE, m, samples=3, is_base=False)
    solid_img = Image.frombytes("L", (EYE_BASE, EYE_BASE), bytes(solid))
    halo_canvas = Image.new("L", (EYE_HALO, EYE_HALO), 0)
    pad = (EYE_HALO - EYE_BASE) // 2
    halo_canvas.paste(solid_img, (pad, pad))
    halo_blur = halo_canvas.filter(ImageFilter.GaussianBlur(radius=EYE_HALO * 0.11))
    # Boost intensity so the bloom is visibly bright on black.
    halo_blur = halo_blur.point(lambda v: min(255, v * 2))
    HALO[m] = halo_blur


def soft_ellipse_image(w: int, h: int) -> Image.Image:
    cx = (w - 1) / 2; cy = (h - 1) / 2
    rx = w / 2; ry = h / 2
    out = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            nx = (x - cx) / rx; ny = (y - cy) / ry
            d = nx * nx + ny * ny
            if d < 1:
                out[y * w + x] = int(255 * (1 - d) ** 1.6 + 0.5)
    return Image.frombytes("L", (w, h), bytes(out))


SPEC = soft_ellipse_image(SPEC_W, SPEC_H)


def tint_mask(mask: Image.Image, color: tuple, alpha_scale: float = 1.0) -> Image.Image:
    """Return an RGBA image where RGB=color and A=mask*alpha_scale."""
    a = mask
    if alpha_scale != 1.0:
        a = a.point(lambda v: int(min(255, v * alpha_scale)))
    rgb = Image.new("RGB", mask.size, color)
    rgba = rgb.convert("RGBA")
    rgba.putalpha(a)
    return rgba


def composite_eye(img: Image.Image, cx: int, mood: str, color: tuple,
                  scale_y: float, halo_scale: float):
    if scale_y < 0.04:
        return

    # --- halo bloom (smooth Gaussian) ---
    hw = max(8, int(EYE_HALO * halo_scale))
    hh = max(8, int(EYE_HALO * halo_scale * scale_y))
    halo_resized = HALO[mood].resize((hw, hh), Image.LANCZOS)
    halo_tinted = tint_mask(halo_resized, color, alpha_scale=0.55)
    img.alpha_composite(halo_tinted,
                        (cx - hw // 2, EYE_CY - hh // 2))

    # --- base sharp eye ---
    bw = EYE_BASE
    bh = max(4, int(EYE_BASE * scale_y))
    base_resized = BASE[mood].resize((bw, bh), Image.LANCZOS)
    base_tinted = tint_mask(base_resized, color, alpha_scale=1.0)
    img.alpha_composite(base_tinted, (cx - bw // 2, EYE_CY - bh // 2))

    # --- inner gradient highlight (top-left): paints a subtle bright
    # sphere on top of the base, clipped by the base shape ---
    if scale_y > 0.3 and mood not in ("SLEEP",):
        inner = Image.new("RGBA", (bw, bh), (0, 0, 0, 0))
        idraw = ImageDraw.Draw(inner)
        # Soft white blob in upper-left
        for r in range(int(bw * 0.55), 0, -2):
            a = max(0, 80 - r * 2)
            idraw.ellipse((bw * 0.18 - r, bh * 0.12 - r,
                           bw * 0.18 + r, bh * 0.12 + r),
                          fill=(255, 255, 255, a))
        # Clip by the base mask
        inner.putalpha(Image.eval(
            Image.merge("L", (Image.eval(inner.split()[3], lambda v: v),)).split()[0],
            lambda v: v))
        # multiply with base alpha to clip
        clip = base_resized
        a = inner.split()[3]
        a = ImageChops_multiply(a, clip)
        inner.putalpha(a)
        img.alpha_composite(inner, (cx - bw // 2, EYE_CY - bh // 2))

    # --- crisp specular dot ---
    # Mirror the firmware's mood_has_spec(): only full-bodied shapes get the
    # lens-shine. Thin arcs / dots would render spec outside the silhouette.
    bool_has_spec = mood in ("NEUTRAL", "LOVE", "CIRCLE",
                             "PUPIL_LEFT", "PUPIL_SLIT")
    if bool_has_spec and scale_y > 0.45:
        spec_tinted = tint_mask(SPEC, WHITE, alpha_scale=0.85)
        sx = cx - EYE_BASE // 4 - 4
        sy = EYE_CY - int(bh * 0.35)
        img.alpha_composite(spec_tinted, (sx, sy))


def ImageChops_multiply(a: Image.Image, b: Image.Image) -> Image.Image:
    # Simple multiply for L channels
    from PIL import ImageChops
    return ImageChops.multiply(a, b)


# ─── demo timeline ─────────────────────────────────────────────────────────
DURATION = 8.0
FPS = 20
N_FRAMES = int(FPS * DURATION)

BLINK_TIMES = [0.7, 1.6, 3.1, 5.5]


def frame_state(t: float):
    # Showcase: NEUTRAL → CIRCLE → PUPIL_LEFT → PUPIL_SLIT → HAPPY ⌒ →
    # ANGRY → CROSS → SLEEP, ~1.0 s each (8 moods × 1 s = 8 s loop).
    if t < 1.0:    return "NEUTRAL",    22.0 + t * 8
    if t < 2.0:    return "CIRCLE",     38.0
    if t < 3.0:    return "PUPIL_LEFT", 50.0
    if t < 4.0:    return "PUPIL_SLIT", 60.0
    if t < 5.0:    return "HAPPY",      70.0
    if t < 6.0:    return "ANGRY",      85.0
    if t < 7.0:    return "CROSS",      95.0
    return                "SLEEP",      100.0


def render_frame(t: float) -> Image.Image:
    img = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    mood, pct = frame_state(t)
    color = halo_color(pct)

    scale_y = 1.0
    for bt in BLINK_TIMES:
        d = t - bt
        if 0 <= d <= 0.22:
            if d < 0.11: scale_y = 1.0 - (d / 0.11) * 0.9
            else:        scale_y = 0.1 + ((d - 0.11) / 0.11) * 0.9
            break

    halo_scale = 1.0 + 0.05 * math.sin(t * 2 * math.pi / 1.6)
    if mood == "SLEEP":
        halo_scale = 1.0 + 0.10 * math.sin(t * 2 * math.pi / 2.4)

    composite_eye(img, EYE_L_CX, mood, color, scale_y, halo_scale)
    composite_eye(img, EYE_R_CX, mood, color, scale_y, halo_scale)

    # z's during sleep
    if mood == "SLEEP":
        d = ImageDraw.Draw(img)
        try:
            from PIL import ImageFont
            font = ImageFont.load_default(size=28)
        except Exception:
            font = None
        for i in range(3):
            phase = (((t - 6.2) / 2) + i * 0.33) % 1.0
            zx = EYE_R_CX + EYE_BASE // 2 + 12 + int(20 * phase)
            zy = EYE_CY - 22 - int(70 * phase)
            alpha = int(255 * (1 - phase))
            d.text((zx, zy), "z", fill=(*color, alpha), font=font)

    return img.convert("RGB")


def main() -> None:
    print(f"rendering {N_FRAMES} frames at {FPS} fps ({DURATION:.1f} s)…",
          file=sys.stderr)
    frames: list[Image.Image] = [render_frame(i / FPS) for i in range(N_FRAMES)]

    # Use a single adaptive palette derived from the union of frames so the
    # smooth gradients stay smooth across the loop.
    pal_master = frames[0].convert("P", palette=Image.ADAPTIVE, colors=256)
    pal = [f.quantize(palette=pal_master, dither=Image.NONE) for f in frames]
    pal[0].save(
        OUT,
        save_all=True,
        append_images=pal[1:],
        duration=int(1000 / FPS),
        loop=0,
        optimize=False,
        disposal=2,
    )
    print(f"wrote {OUT}  ({OUT.stat().st_size // 1024} KB, {N_FRAMES} frames)")


if __name__ == "__main__":
    main()
