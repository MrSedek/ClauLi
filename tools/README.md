# Asset tools

Generators that turn vendored source assets into firmware headers. Run from
the repo root; rebuild the firmware afterwards.

## Splash animations — `convert_to_c.js`

```bash
node tools/convert_to_c.js
```

Reads the vendored 20×20 pixel-art frames in `tools/claudepix_data/*.json`
and emits `firmware/src/splash_animations.h`:

- `splash_<ident>_frames[N][400]` — per-frame cell codes (0=empty, 1=body, 2=eye)
- `splash_<ident>_holds[N]` — per-frame hold time (ms)
- `splash_anims[]` + `SPLASH_ANIM_COUNT` — master table

`splash.cpp` consumes this header. Override input/output with `--in` / `--out`.

## Emo robot eyes — `convert_ani_emo.js`

```bash
node tools/convert_ani_emo.js
```

Reads the vendored bitmaps in `tools/ani_emo_eyes_src.h` and emits
`firmware/src/emo_eyes.h` (LVGL 9 A8, tintable). `emo.cpp` consumes it.

## Cyrillic fonts — `patch_lvgl9_font.py`

Patches `lv_font_conv` output into the LVGL 9 format used in
`firmware/src/fonts_cyr/`. See the font regeneration snippet in `CLAUDE.md`.

## Icons — `png_to_lvgl.js`

Converts an alpha PNG into an LVGL image array (see file header for usage).

All generated headers are committed — do not hand-edit them; re-run the
relevant generator and rebuild.
