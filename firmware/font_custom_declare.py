# PlatformIO extra script to set LV_FONT_CUSTOM_DECLARE build flag.
# Cannot use parentheses in platformio.ini build_flags due to shell parsing.

Import("env", "projenv")

custom_declare = ('LV_FONT_CUSTOM_DECLARE',
    'LV_FONT_DECLARE(lv_font_montserrat_12) '
    'LV_FONT_DECLARE(lv_font_montserrat_14) '
    'LV_FONT_DECLARE(lv_font_montserrat_16) '
    'LV_FONT_DECLARE(lv_font_montserrat_20)')

# Apply to project env and all library build envs
for e in [env, projenv]:
    e.Append(CPPDEFINES=[custom_declare])
