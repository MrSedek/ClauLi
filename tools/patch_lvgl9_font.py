#!/usr/bin/env python3
# Patch lv_font_conv (LVGL-8-style) output into the clean LVGL 9 form used by
# firmware/src/fonts_cyr/*.c. See CLAUDE.md gotcha #4: LVGL 9 needs the version
# #if guards removed, the glyph cache dropped, and the public font struct to
# carry .fallback/.user_data/.release_glyph/.kerning/.static_bitmap.
#
# Usage: patch_lvgl9_font.py <raw_lv_font_conv.c> <symbol_name> <out.c>

import re
import sys

raw_path, symbol, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
src = open(raw_path, encoding="utf-8").read()

# 1. Header: strip the include shim + `#if MXX` guard down to the BITMAPS banner.
m = re.search(r"/\*-+\n \*    BITMAPS", src)
src = '#include "lvgl.h"\n\n' + src[m.start():]

# 2. Trailing `#endif /*#if MXX*/`
src = re.sub(r"\n#endif\s*/\*#if \w+\*/\s*$", "\n", src)

# 3. Drop the LVGL 8 glyph cache block.
src = re.sub(
    r"#if LVGL_VERSION_MAJOR == 8\n"
    r"/\*Store all the custom data of the font\*/\n"
    r"static\s+lv_font_fmt_txt_glyph_cache_t cache;\n"
    r"#endif\n", "", src)

# 4. font_dsc: collapse the version guard, drop `.cache`.
src = src.replace(
    "#if LVGL_VERSION_MAJOR >= 8\n"
    "static const lv_font_fmt_txt_dsc_t font_dsc = {\n"
    "#else\n"
    "static lv_font_fmt_txt_dsc_t font_dsc = {\n"
    "#endif\n",
    "static const lv_font_fmt_txt_dsc_t font_dsc = {\n")
src = re.sub(r",?\n#if LVGL_VERSION_MAJOR == 8\n\s*\.cache = &cache\n#endif\n",
             "\n", src)

# 5. Rebuild the public font struct in canonical LVGL 9 form.
pub = re.search(
    r"/\*Initialize a public general font descriptor\*/.*?\n};",
    src, re.S).group(0)
line_height = re.search(r"\.line_height = (\d+)", pub).group(1)
base_line = re.search(r"\.base_line = (-?\d+)", pub).group(1)
up = re.search(r"\.underline_position = (-?\d+)", pub).group(1)
ut = re.search(r"\.underline_thickness = (-?\d+)", pub).group(1)

canonical = f"""/*-----------------
 *  PUBLIC FONT
 *----------------*/

const lv_font_t {symbol} = {{
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = {line_height},
    .base_line = {base_line},
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = {up},
    .underline_thickness = {ut},
    .dsc = &font_dsc,
    .fallback = NULL,
    .user_data = NULL,
    .release_glyph = NULL,
    .kerning = LV_FONT_KERNING_NONE,
    .static_bitmap = 0,
}};"""

# Replace from the PUBLIC FONT banner through the struct.
src = re.sub(
    r"/\*-+\n \*  PUBLIC FONT\n \*-+\*/\n\n"
    r"/\*Initialize a public general font descriptor\*/.*?\n};",
    canonical, src, flags=re.S)

src = src.rstrip() + "\n"
open(out_path, "w", encoding="utf-8").write(src)
print(f"patched -> {out_path} (symbol={symbol}, line_height={line_height})")
