# PlatformIO extra script to set LV_FONT_CUSTOM_DECLARE build flag.
# Cannot use parentheses in platformio.ini build_flags due to shell parsing.

Import("env", "projenv")

custom_declare = ('LV_FONT_CUSTOM_DECLARE',
    'LV_FONT_DECLARE(lv_font_montserrat_12) '
    'LV_FONT_DECLARE(lv_font_montserrat_14) '
    'LV_FONT_DECLARE(lv_font_montserrat_16) '
    'LV_FONT_DECLARE(lv_font_montserrat_20) '
    # Clock-style fonts (fonts_clock/*.c) — bitmap-rasterised from the
    # matching web TTF, digit-only glyphs (space + 0-9 + : + [ ]) so
    # they fit ~14-25 KB each instead of the full-charset 80+ KB.
    'LV_FONT_DECLARE(lv_font_clock_sharetech_56) '
    'LV_FONT_DECLARE(lv_font_clock_sharetech_36) '
    'LV_FONT_DECLARE(lv_font_clock_major_44) '
    'LV_FONT_DECLARE(lv_font_clock_orbitron_44) '
    'LV_FONT_DECLARE(lv_font_clock_onest_56) '
    'LV_FONT_DECLARE(lv_font_clock_azeret_40)')

# Apply to project env and all library build envs
for e in [env, projenv]:
    e.Append(CPPDEFINES=[custom_declare])
