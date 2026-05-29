#pragma once

// Runtime UI language. Default LANG_EN; daemon flips it via a BLE CTRL byte
// (0x40 = EN, 0x41 = RU) handled in main.cpp.
enum lang_t { LANG_EN, LANG_RU };

enum str_id {
    STR_USAGE_TITLE,
    STR_SESSION,        // "Session" / "Сессия"
    STR_WEEK,           // "Week" / "Неделя"
    STR_SESSION_SHORT,  // "S" / "С"
    STR_WEEK_SHORT,     // "W" / "Н"
    STR_WEEK_PCT_PREFIX,// "W " / "Н "
    STR_RESET_IN,       // "Reset in" / "Сброс через"
    STR_DASH,           // "--"
    STR_U_MIN,          // "m" / "м"
    STR_U_HOUR,         // "h" / "ч"
    STR_U_DAY,          // "d" / "д"
    STR_BT_LOADING,
    STR_BT_CONNECTED,
    STR_BT_WAITING,
    STR_BT_DISCONNECTED,
    STR_BT_DEVICE,      // "Device:" / "Устройство:"
    STR_BT_ADDR,        // "Address:" / "Адрес:"
    STR_BT_RESET,       // "Reset Bluetooth" / "Сброс Bluetooth"
    STR_RECONNECT,      // "Reconnecting" / "Переподключение"
    STR_REAUTH,         // "Re-auth needed" / "Нужен повторный вход"
    STR_COUNT
};

extern lang_t g_lang;

void        i18n_set(lang_t l);          // set current language (no-op if same)
const char* TR(str_id id);               // localized string for g_lang
const char* const* i18n_spinner(int* count);  // spinner word list for g_lang
