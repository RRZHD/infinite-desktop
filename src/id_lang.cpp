// Язык интерфейса: русский на русской Windows, английский — на любой другой.
// Часть InfiniteDesktop.

#include "id_common.h"

// Пары «русский / английский» в порядке StrId. Строки одной записи стоят рядом, чтобы
// переводы не разъезжались; число записей сверяется с S_COUNT при компиляции.
struct StrRow { const wchar_t* ru; const wchar_t* en; };
static const StrRow STRINGS[] = {
    /* S_LOCALE          */ { L"ru-ru", L"en-us" },
    // названия действий (окно настроек, сообщения о конфликтах)
    /* S_KEY_PAN_LEFT    */ { L"Панорама влево",       L"Pan left" },
    /* S_KEY_PAN_RIGHT   */ { L"Панорама вправо",      L"Pan right" },
    /* S_KEY_PAN_UP      */ { L"Панорама вверх",       L"Pan up" },
    /* S_KEY_PAN_DOWN    */ { L"Панорама вниз",        L"Pan down" },
    /* S_KEY_HOME        */ { L"Домой (камера 0,0)",   L"Home (camera 0,0)" },
    /* S_KEY_MAP         */ { L"Миникарта вкл/выкл",   L"Toggle minimap" },
    /* S_KEY_REFRESH     */ { L"Пересобрать окна",     L"Rebuild window list" },
    /* S_KEY_QUIT        */ { L"Выход",                L"Quit" },
    /* S_KEY_PREVIEW     */ { L"Превью вкл/выкл",      L"Toggle previews" },
    /* S_KEY_PIN         */ { L"Закрепить окно",       L"Pin window" },
    /* S_KEY_FOCUS_LEFT  */ { L"Фокус: окно слева",    L"Focus: window on the left" },
    /* S_KEY_FOCUS_RIGHT */ { L"Фокус: окно справа",   L"Focus: window on the right" },
    /* S_KEY_FOCUS_UP    */ { L"Фокус: окно сверху",   L"Focus: window above" },
    /* S_KEY_FOCUS_DOWN  */ { L"Фокус: окно снизу",    L"Focus: window below" },
    /* S_MARK_FMT        */ { L"Запомнить место %d",   L"Remember place %d" },
    /* S_GOTO_FMT        */ { L"Перейти к месту %d",   L"Go to place %d" },
    /* S_MARK_GROUP      */ { L"Запомнить место 1…9",  L"Remember place 1…9" },
    /* S_GOTO_GROUP      */ { L"Перейти к месту 1…9",  L"Go to place 1…9" },
    // сообщения при назначении сочетаний
    /* S_ERR_NEED_DIGIT  */ { L"Для мест: модификаторы + цифра (1…9 или Num 1…9)",
                              L"Places need modifiers + a digit (1…9 or Num 1…9)" },
    /* S_ERR_TAKEN       */ { L" уже занято: ",        L" is already used by: " },
    // окно настроек
    /* S_SEC_HOTKEYS     */ { L"Горячие клавиши",      L"Hotkeys" },
    /* S_SEC_MARKS       */ { L"Закладки камеры",      L"Canvas bookmarks" },
    /* S_SEC_BEHAVIOR    */ { L"Поведение",            L"Behavior" },
    /* S_OPT_CORNER      */ { L"Угол миникарты",       L"Minimap corner" },
    /* S_OPT_MMB         */ { L"СКМ двигает холст в любом окне",
                              L"Middle button moves canvas in any window" },
    /* S_OPT_MAXMOVE     */ { L"Развёрнутые окна двигаются с холстом",
                              L"Maximized windows move with the canvas" },
    /* S_RESET           */ { L"Сбросить по умолчанию", L"Reset to defaults" },
    /* S_SETTINGS_TITLE  */ { L"Настройки",            L"Settings" },
    /* S_SETTINGS_HINT   */ { L"Клик по сочетанию — нажмите новое · Esc — отмена",
                              L"Click a shortcut, then press the new one · Esc cancels" },
    /* S_CAP_KEYS        */ { L"нажмите клавиши…",     L"press keys…" },
    /* S_CAP_DIGIT       */ { L"модификаторы + цифра…", L"modifiers + digit…" },
    // трей
    /* S_TRAY_TIP        */ { L"InfiniteDesktop — бесконечный рабочий стол",
                              L"InfiniteDesktop — infinite desktop canvas" },
    /* S_MENU_SETTINGS   */ { L"Настройки…",           L"Settings…" },
    /* S_MENU_MAP        */ { L"Миникарта",            L"Minimap" },
    /* S_MENU_PREVIEW    */ { L"Живые превью на карте", L"Live previews on the minimap" },
    /* S_MENU_REFRESH    */ { L"Пересобрать список окон", L"Rebuild window list" },
    /* S_MENU_EXIT       */ { L"Выход",                L"Quit" },
    // подсказки на миникарте и в обзоре
    /* S_MAP_TIP         */ { L"ЛКМ — перелёт · ⚙ — настройки", L"Click — jump · ⚙ — settings" },
    /* S_OV_HINT_FMT     */ { L"Обзор  %d%%   клик по окну — нажать • клик по фону — приблизить • колесо — зум • СКМ — двигать",
                              L"Overview  %d%%   click a window — press • click background — zoom in • wheel — zoom • middle button — pan" },
};
static_assert(sizeof(STRINGS) / sizeof(STRINGS[0]) == S_COUNT, "STRINGS must match StrId");

// Выбирается один раз при старте: язык интерфейса Windows меняется только с повторным
// входом в систему, следить за ним на лету незачем.
static bool s_russian = false;

void InitLanguage() {
    s_russian = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
}

const wchar_t* Tr(StrId id) {
    return s_russian ? STRINGS[id].ru : STRINGS[id].en;
}
