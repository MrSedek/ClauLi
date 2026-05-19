#include "i18n.h"

lang_t g_lang = LANG_EN;

void i18n_set(lang_t l) { g_lang = l; }

// [str_id][lang]  — lang index: 0 = EN, 1 = RU
static const char* const S[STR_COUNT][2] = {
    /* STR_USAGE_TITLE   */ { "Usage",           "Использование" },
    /* STR_SESSION       */ { "Session",         "Сессия" },
    /* STR_WEEK          */ { "Week",            "Неделя" },
    /* STR_SESSION_SHORT */ { "S",               "С" },
    /* STR_WEEK_SHORT    */ { "W",               "Н" },
    /* STR_WEEK_PCT_PREFIX*/{ "W ",              "Н " },
    /* STR_RESET_IN      */ { "Reset in",        "Сброс через" },
    /* STR_DASH          */ { "--",              "--" },
    /* STR_U_MIN         */ { "m",               "м" },
    /* STR_U_HOUR        */ { "h",               "ч" },
    /* STR_U_DAY         */ { "d",               "д" },
    /* STR_BT_LOADING    */ { "Loading...",      "Загрузка..." },
    /* STR_BT_CONNECTED  */ { "Connected",       "Подключено" },
    /* STR_BT_WAITING    */ { "Waiting...",      "Ожидание..." },
    /* STR_BT_DISCONNECTED*/{ "Disconnected",    "Отключено" },
    /* STR_BT_DEVICE     */ { "Device:",         "Устройство:" },
    /* STR_BT_ADDR       */ { "Address:",        "Адрес:" },
    /* STR_BT_RESET      */ { "Reset Bluetooth", "Сброс Bluetooth" },
    /* STR_RECONNECT */ { "Reconnecting", "Переподключение" },
};

const char* TR(str_id id) { return S[id][g_lang]; }

static const char* const SPIN_EN[] = {
    "Computing", "Pondering", "Studying", "Acting", "Conjuring",
    "Philosophizing", "Implementing", "Foreseeing", "Reflecting",
    "Baking", "Scheming", "Processing", "Booping", "Flibbertigibbeting",
    "Doing", "Brewing", "Forging", "Tinkering", "Counting", "Forming",
    "Divining", "Brainstorming", "Frolicking", "Reticulating",
    "Channeling", "Generating", "Chewing", "Whisking", "Germinating",
    "Finagling", "Incubating", "Spinning", "Deliberating", "Deducing",
    "Contemplating", "Vibing", "Cogitating", "Simmering", "Manifesting",
    "Synthesizing", "Crafting", "Marinating", "Musing", "Creating",
    "Wandering", "Crunching", "Transmuting", "Decoding", "Unfurling",
    "Assembling", "Untangling", "Determining", "Percolating", "Buzzing",
    "Effecting", "Wizarding", "Working", "Wrangling",
};

static const char* const SPIN_RU[] = {
    "Вычисляет", "Осмысливает", "Изучает", "Действует", "Колдует",
    "Философствует", "Реализует", "Предвидит", "Размышляет", "Выпекает",
    "Мудрит", "Обрабатывает", "Бупает", "Флибертижит", "Делает",
    "Заваривает", "Кует", "Возится", "Считает", "Формирует", "Гадает",
    "Мозгует", "Резвится", "Ретикулирует", "Канализует", "Генерирует",
    "Жует", "Взбалтывает", "Проращивает", "Схемит", "Высиживает",
    "Шлепает", "Думает", "Гудит", "Пыхтит", "Тушит", "Идеирует",
    "Сочиняет", "Воображает", "Инкубирует", "Крутит", "Обдумывает",
    "Выводит", "Созерцает", "Соображает", "Варит", "Манифестирует",
    "Синтезирует", "Крафтит", "Маринует", "Мыслит", "Создаёт",
    "Бродит", "Расшифровывает", "Разворачивает", "Собирает",
    "Распутывает", "Определяет", "Перколирует", "Жужжит", "Работает",
};

const char* const* i18n_spinner(int* count) {
    if (g_lang == LANG_RU) {
        *count = (int)(sizeof(SPIN_RU) / sizeof(SPIN_RU[0]));
        return SPIN_RU;
    }
    *count = (int)(sizeof(SPIN_EN) / sizeof(SPIN_EN[0]));
    return SPIN_EN;
}
