// Настройки: стиль Win11, горячие клавиши, окно настроек (Direct2D, по стайлгайду)
// Часть InfiniteDesktop.

#include "id_common.h"

using Microsoft::WRL::ComPtr;

// ---------- Стиль Win11, шрифт, размещение миникарты ----------
// Заголовок в цвет темы (тёмный/светлый) и скруглённые углы (Windows 11)
void ApplyWin11Style(AppState& app, HWND hwnd) {
    BOOL dark = app.theme.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    int round = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &round, sizeof(round));
}

static std::wstring UiCfgPath() { return ConfigPath(L"ui.txt"); }

static void SaveUiCfg(AppState& app) {
    std::ofstream f(UiCfgPath().c_str(), std::ios::binary | std::ios::trunc);
    if (f) {
        std::string s = "corner "  + std::to_string(app.corner) + "\n"
                      + "mmb "     + std::to_string(app.mmbAnywhere.load() ? 1 : 0) + "\n"
                      + "maxmove " + std::to_string(app.moveMaximized ? 1 : 0) + "\n"
                      + "glide "   + std::to_string(app.glideEnabled ? 1 : 0) + "\n";
        f.write(s.data(), s.size());
    }
}
void LoadUiCfg(AppState& app) {
    std::ifstream f(UiCfgPath().c_str(), std::ios::binary);
    if (!f) return;
    std::string key; int v;
    while (f >> key >> v) {
        if (key == "corner" && v >= 0 && v <= 3) app.corner = v;
        if (key == "mmb")                        app.mmbAnywhere.store(v != 0);
        if (key == "maxmove")                    app.moveMaximized = (v != 0);
        if (key == "glide")                      app.glideEnabled = (v != 0);
    }
}

// Разместить миникарту в выбранном углу рабочей области основного монитора.
void PlaceHud(AppState& app) {
    if (!app.hud) return;
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    // Размер — по масштабу основного монитора: на 200 % миникарта в физических пикселях
    // была бы вдвое мельче задуманного.
    POINT origin = { 0, 0 };
    float s = MonitorScale(app, MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY));
    if (s != app.uiScale) { app.uiScale = s; ResetMapGdi(app); }   // перья и шрифты — под новый масштаб
    int w = (int)lround(MAP_W * s), h = (int)lround(MAP_H * s), m = (int)lround(MAP_MARGIN * s);
    int x = (app.corner == 0 || app.corner == 2) ? wa.left + m : wa.right - w - m;
    int y = (app.corner == 0 || app.corner == 1) ? wa.top + m  : wa.bottom - h - m;
    SetWindowPos(app.hud, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
}

// ---------- Горячие клавиши ----------
void InitDefaultKeys(AppState& app) {
    app.keys[HK_LEFT]    = { MOD_CONTROL | MOD_ALT, VK_LEFT,  Tr(S_KEY_PAN_LEFT) };
    app.keys[HK_RIGHT]   = { MOD_CONTROL | MOD_ALT, VK_RIGHT, Tr(S_KEY_PAN_RIGHT) };
    app.keys[HK_UP]      = { MOD_CONTROL | MOD_ALT, VK_UP,    Tr(S_KEY_PAN_UP) };
    app.keys[HK_DOWN]    = { MOD_CONTROL | MOD_ALT, VK_DOWN,  Tr(S_KEY_PAN_DOWN) };
    app.keys[HK_HOME]    = { MOD_CONTROL | MOD_ALT, 'H',      Tr(S_KEY_HOME) };
    app.keys[HK_MAP]     = { MOD_CONTROL | MOD_ALT, 'M',      Tr(S_KEY_MAP) };
    app.keys[HK_REFRESH] = { MOD_CONTROL | MOD_ALT, 'R',      Tr(S_KEY_REFRESH) };
    app.keys[HK_QUIT]    = { MOD_CONTROL | MOD_ALT, 'Q',      Tr(S_KEY_QUIT) };
    app.keys[HK_PREVIEW] = { MOD_CONTROL | MOD_ALT, 'P',      Tr(S_KEY_PREVIEW) };
    app.keys[HK_PIN]     = { MOD_CONTROL | MOD_ALT, 'F',      Tr(S_KEY_PIN) };
    // Направленный фокус. Не Ctrl+Стрелки: это перемещение по словам в любом
    // текстовом поле, глобальный хоткей сломал бы редактирование везде.
    const UINT CAS = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
    app.keys[HK_FOCUS_LEFT]  = { CAS, VK_LEFT,  Tr(S_KEY_FOCUS_LEFT) };
    app.keys[HK_FOCUS_RIGHT] = { CAS, VK_RIGHT, Tr(S_KEY_FOCUS_RIGHT) };
    app.keys[HK_FOCUS_UP]    = { CAS, VK_UP,    Tr(S_KEY_FOCUS_UP) };
    app.keys[HK_FOCUS_DOWN]  = { CAS, VK_DOWN,  Tr(S_KEY_FOCUS_DOWN) };
    // Закладки камеры: запомнить — с Shift, вернуться — без. Имена по номеру — для
    // сообщений о конфликтах («…уже занято: Перейти к месту 3»).
    static wchar_t markNames[MARK_COUNT][32], gotoNames[MARK_COUNT][32];
    for (int i = 0; i < MARK_COUNT; ++i) {
        wsprintfW(markNames[i], Tr(S_MARK_FMT), i + 1);
        wsprintfW(gotoNames[i], Tr(S_GOTO_FMT), i + 1);
        app.keys[HK_MARK1 + i] = { CAS,                   (UINT)('1' + i), markNames[i] };
        app.keys[HK_GOTO1 + i] = { MOD_CONTROL | MOD_ALT, (UINT)('1' + i), gotoNames[i] };
    }
}

// Сочетание может быть занято другой программой — тогда RegisterHotKey откажет. Раньше
// результат не проверялся: в настройках сочетание выглядело назначенным, но не работало.
void RegisterAllHotkeys(AppState& app) {
    for (int id = HK_LEFT; id <= HK_LAST; ++id)
        app.keyFailed[id] = !RegisterHotKey(app.hud, id, app.keys[id].mods, app.keys[id].vk);
}
void UnregisterAllHotkeys(AppState& app) {
    for (int id = HK_LEFT; id <= HK_LAST; ++id) UnregisterHotKey(app.hud, id);
}

static std::wstring HotkeysPath() { return ConfigPath(L"hotkeys.txt"); }

static void SaveHotkeys(AppState& app) {
    std::ofstream f(HotkeysPath().c_str(), std::ios::binary | std::ios::trunc);
    if (!f) return;
    f.write("v2\n", 3);   // версия формата (v2: 9 мест); далее строки «id mods vk»
    for (int id = HK_LEFT; id <= HK_LAST; ++id) {
        std::string line = std::to_string(id) + " " + std::to_string(app.keys[id].mods)
                         + " " + std::to_string(app.keys[id].vk) + "\n";
        f.write(line.data(), (std::streamsize)line.size());
    }
}

static int DigitIndex(UINT vk, UINT& base);   // определена ниже

void LoadHotkeys(AppState& app) {
    std::ifstream f(HotkeysPath().c_str(), std::ios::binary);
    if (!f) return;
    std::string line;
    // v1 (без заголовка, 4 места): ids 15…18 — места 1…4, 19…22 — переходы 1…4. В v2
    // (9 мест) эти id сдвинулись, поэтому без миграции старые переходы легли бы в слоты
    // «Запомнить место 5…8» и задублировали бы хоткеи.
    bool v2 = false;
    if (std::getline(f, line) && line == "v2") v2 = true; else { f.clear(); f.seekg(0); }
    const int oldMark1 = HK_FOCUS_DOWN + 1, oldGoto1 = oldMark1 + 4;
    while (std::getline(f, line)) {
        std::istringstream iss(line);
        int id, mods, vk;
        if (!(iss >> id >> mods >> vk)) continue;
        if (!v2) {
            if      (id >= oldMark1 && id < oldMark1 + 4) id = HK_MARK1 + (id - oldMark1);
            else if (id >= oldGoto1 && id < oldGoto1 + 4) id = HK_GOTO1 + (id - oldGoto1);
            else if (id > HK_FOCUS_DOWN) continue;
        }
        if (id >= HK_LEFT && id <= HK_LAST) {
            app.keys[id].mods = (UINT)mods;
            app.keys[id].vk   = (UINT)vk;
        }
    }
    // Группы мест держим согласованными: модификаторы и база цифр (ряд/Num) — от первого
    // члена, цифры по порядку. Защищает и от повреждённого/частичного файла.
    for (int g : { (int)HK_MARK1, (int)HK_GOTO1 }) {
        UINT base = '1';
        if (DigitIndex(app.keys[g].vk, base) != 0) base = '1';   // первый член должен быть «1»
        for (int i = 0; i < MARK_COUNT; ++i) { app.keys[g + i].mods = app.keys[g].mods; app.keys[g + i].vk = base + i; }
    }
}

static std::wstring KeyName(UINT vk) {
    switch (vk) {
        case VK_LEFT:  return L"←"; case VK_RIGHT: return L"→";
        case VK_UP:    return L"↑"; case VK_DOWN:  return L"↓";
        case VK_SPACE: return L"Space"; case VK_RETURN: return L"Enter";
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        wchar_t c[2] = { (wchar_t)vk, 0 }; return c;
    }
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    bool ext = (vk == VK_PRIOR || vk == VK_NEXT || vk == VK_HOME || vk == VK_END ||
                vk == VK_INSERT || vk == VK_DELETE);
    LONG lp = (LONG)(sc << 16);
    if (ext) lp |= (1 << 24);
    wchar_t buf[64] = {};
    if (sc && GetKeyNameTextW(lp, buf, 64) > 0) return buf;
    wchar_t b[16]; wsprintfW(b, L"VK_%02X", vk); return b;
}
static std::wstring KeyComboText(UINT mods, UINT vk) {
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl+";
    if (mods & MOD_ALT)     s += L"Alt+";
    if (mods & MOD_SHIFT)   s += L"Shift+";
    if (mods & MOD_WIN)     s += L"Win+";
    s += KeyName(vk);
    return s;
}

static bool IsModifierVk(UINT vk) {
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
           vk == VK_LWIN || vk == VK_RWIN ||
           vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL ||
           vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU || vk == VK_CAPITAL;
}
static UINT CurrentMods() {
    UINT m = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= MOD_CONTROL;
    if (GetKeyState(VK_MENU)    & 0x8000) m |= MOD_ALT;
    if (GetKeyState(VK_SHIFT)   & 0x8000) m |= MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) m |= MOD_WIN;
    return m;
}

// ---------- Строки хоткеев ----------
// Обычные действия — по строке на хоткей. Закладки камеры — две ГРУППОВЫЕ строки
// («Запомнить место 1…9», «Перейти к месту 1…9»): настраиваются модификаторы, цифра
// подразумевается (ряд или Num-блок). Иначе 9 мест заняли бы 18 рядов.
// Представитель группы — её первый id (HK_MARK1 / HK_GOTO1).
static int  RowCount() { return (HK_FOCUS_DOWN - HK_LEFT + 1) + 2; }
static int  RowId(int row) {
    int n = HK_FOCUS_DOWN - HK_LEFT + 1;
    if (row < n) return HK_LEFT + row;
    return row == n ? HK_MARK1 : HK_GOTO1;
}
static bool IsGroup(int id) { return id == HK_MARK1 || id == HK_GOTO1; }
static const wchar_t* RowName(AppState& app, int id) {
    if (id == HK_MARK1) return Tr(S_MARK_GROUP);
    if (id == HK_GOTO1) return Tr(S_GOTO_GROUP);
    return app.keys[id].name;
}
// Строка настроек не работает: сочетание (для группы — любое из девяти) занято в системе.
static bool RowFailed(AppState& app, int id) {
    if (!IsGroup(id)) return app.keyFailed[id];
    for (int i = 0; i < MARK_COUNT; ++i) if (app.keyFailed[id + i]) return true;
    return false;
}
// Первое неработающее сочетание — для сообщения в окне настроек («» — все работают).
static std::wstring FirstFailedText(AppState& app) {
    for (int id = HK_LEFT; id <= HK_LAST; ++id)
        if (app.keyFailed[id]) return L"⚠ " + KeyComboText(app.keys[id].mods, app.keys[id].vk) + Tr(S_ERR_SYSTEM_TAKEN);
    return L"";
}
static std::wstring RowComboText(AppState& app, int id) {
    std::wstring s = KeyComboText(app.keys[id].mods, app.keys[id].vk);
    if (IsGroup(id)) s += L"…9";   // «Ctrl+Alt+1» → «Ctrl+Alt+1…9», «…Num 1» → «…Num 1…9»
    return s;
}
// Цифра 1…9 (ряд или Num-блок): индекс 0…8 и базовый vk группы, иначе -1.
static int DigitIndex(UINT vk, UINT& base) {
    if (vk >= '1' && vk <= '9')                 { base = '1';        return (int)(vk - '1'); }
    if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9)   { base = VK_NUMPAD1; return (int)(vk - VK_NUMPAD1); }
    return -1;
}

// Найти другое действие с тем же сочетанием (или -1).
static int FindConflict(AppState& app, UINT mods, UINT vk, int exceptId) {
    for (int id = HK_LEFT; id <= HK_LAST; ++id)
        if (id != exceptId && app.keys[id].mods == mods && app.keys[id].vk == vk) return id;
    return -1;
}

// Применить перехваченное сочетание к строке capRow (общий код для WM_KEYDOWN окна
// настроек и WM_HOTKEY HUD). false — не применено: конфликт или неподходящая клавиша,
// причина — в app.status; остаёмся в режиме перехвата.
bool ApplyCapture(AppState& app, UINT mods, UINT vk) {
    int row = app.capRow;
    if (row < HK_LEFT) return false;
    if (IsGroup(row)) {
        UINT base = 0; int d = DigitIndex(vk, base);
        if (d < 0) { app.status = Tr(S_ERR_NEED_DIGIT); return false; }
        for (int i = 0; i < MARK_COUNT; ++i) {
            int c = FindConflict(app, mods, base + i, row + i);
            if (c != -1 && !(c >= row && c < row + MARK_COUNT)) {
                app.status = L"⚠ " + KeyComboText(mods, base + i) + Tr(S_ERR_TAKEN) + app.keys[c].name;
                return false;
            }
        }
        for (int i = 0; i < MARK_COUNT; ++i) { app.keys[row + i].mods = mods; app.keys[row + i].vk = base + i; }
    } else {
        int c = FindConflict(app, mods, vk, row);
        if (c != -1) { app.status = L"⚠ " + KeyComboText(mods, vk) + Tr(S_ERR_TAKEN) + app.keys[c].name; return false; }
        app.keys[row].mods = mods; app.keys[row].vk = vk;
    }
    app.capRow = -1;
    UnregisterAllHotkeys(app); RegisterAllHotkeys(app); SaveHotkeys(app);
    app.status = FirstFailedText(app);   // назначили — но сочетание может быть занято в системе
    return true;
}

// ---------- Раскладка окна настроек ----------
// Размеры по стайлгайду: поле 28 px (sm), радиус 8, рамка 1 px, переключатель 38×22,
// отступ 16. Одна функция строит раскладку и для отрисовки, и для кликов — они не
// разойдутся. Содержимое выше окна прокручивается колесом (setScroll).
namespace {
const float PAD = 16.f, HDR_H = 62.f, SEC_H = 48.f, ROW_H = 32.f, SW_H = 38.f;
const float CTRL_W = 176.f, CTRL_H = 28.f, RAD = 8.f;
enum { RK_SECTION, RK_HOTKEY, RK_CORNER, RK_SWITCH, RK_RESET, RK_STATUS };
struct SetRow { int kind; int id; const wchar_t* title; float y, h; };
struct SettingsLayout { std::vector<SetRow> rows; float contentH = 0; };

SettingsLayout BuildLayout(AppState& app) {
    SettingsLayout L; float y = HDR_H;
    auto add = [&](int kind, int id, const wchar_t* t, float h) { L.rows.push_back({ kind, id, t, y, h }); y += h; };
    add(RK_SECTION, 0, Tr(S_SEC_HOTKEYS), SEC_H);
    for (int row = 0; row < RowCount(); ++row) {
        int id = RowId(row);
        if (id == HK_MARK1) add(RK_SECTION, 0, Tr(S_SEC_MARKS), SEC_H);
        add(RK_HOTKEY, id, RowName(app, id), ROW_H);
    }
    add(RK_SECTION, 0, Tr(S_SEC_BEHAVIOR), SEC_H);
    add(RK_CORNER, 0, Tr(S_OPT_CORNER), SW_H);
    add(RK_SWITCH, 0, Tr(S_OPT_MMB), SW_H);
    add(RK_SWITCH, 1, Tr(S_OPT_MAXMOVE), SW_H);
    add(RK_SWITCH, 2, Tr(S_OPT_GLIDE), SW_H);
    y += 8;
    add(RK_RESET, 0, Tr(S_RESET), 40.f);
    if (!app.status.empty()) add(RK_STATUS, 0, nullptr, 24.f);
    L.contentH = y + PAD;
    return L;
}
D2D1_RECT_F CtrlRect(float W, const SetRow& r) {
    float x = W - PAD - CTRL_W, cy = r.y + (r.h - CTRL_H) / 2;
    return { x, cy, x + CTRL_W, cy + CTRL_H };
}
D2D1_RECT_F SegRect(float W, const SetRow& r, int i) {
    const float sw = 34.f, gap = 6.f;
    float x = W - PAD - (4 * sw + 3 * gap) + i * (sw + gap), cy = r.y + (r.h - CTRL_H) / 2;
    return { x, cy, x + sw, cy + CTRL_H };
}
D2D1_RECT_F SwitchRect(float W, const SetRow& r) {
    float x = W - PAD - 38.f, cy = r.y + (r.h - 22.f) / 2;
    return { x, cy, x + 38.f, cy + 22.f };
}
D2D1_RECT_F ResetRect(const SetRow& r) { return { PAD, r.y + 4, PAD + 196.f, r.y + 4 + 32.f }; }
bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; }
int MaxScroll(AppState& app, HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    return (int)max(0.f, BuildLayout(app).contentH - (float)rc.bottom / app.setScale);
}
}

static bool s_placing = false;   // OpenSettings сейчас сама ставит окно (см. WM_DPICHANGED)

static void RenderSettings(AppState& app, HWND hwnd) {
    if (!EnsureUiTarget(app, hwnd, app.setRT, app.setBrush, app.setScale)) return;
    ID2D1HwndRenderTarget* rt = app.setRT.Get();
    ID2D1SolidColorBrush*  br = app.setBrush.Get();
    const Theme& T = app.theme;
    RECT rc; GetClientRect(hwnd, &rc);
    const float W = (float)rc.right / app.setScale;   // раскладка — в логических пикселях
    SettingsLayout L = BuildLayout(app);
    static const wchar_t* GLYPH[4] = { L"↖", L"↗", L"↙", L"↘" };

    rt->BeginDraw();
    rt->Clear(ToD2D(T.bg, 1.0f));
    rt->SetTransform(D2D1::Matrix3x2F::Translation(0.f, -(float)app.setScroll));
    UiText(rt, br, app.fmtTitle.Get(), Tr(S_SETTINGS_TITLE), D2D1_RECT_F{ PAD, 12.f, W - PAD, 34.f }, T.text, DWRITE_TEXT_ALIGNMENT_LEADING);
    UiText(rt, br, app.fmtSmall.Get(), Tr(S_SETTINGS_HINT),
           { PAD, 34.f, W - PAD, 52.f }, T.muted, DWRITE_TEXT_ALIGNMENT_LEADING);
    for (auto& r : L.rows) {
        D2D1_RECT_F label = { PAD, r.y, W - PAD - CTRL_W - 12.f, r.y + r.h };
        switch (r.kind) {
        case RK_SECTION: {
            UiText(rt, br, app.fmtSec.Get(), r.title, D2D1_RECT_F{ PAD, r.y + 14.f, W - PAD, r.y + 34.f }, T.muted, DWRITE_TEXT_ALIGNMENT_LEADING);
            br->SetColor(ToD2D(T.border, 1.0f));
            rt->FillRectangle(D2D1::RectF(PAD, r.y + 36.f, W - PAD, r.y + 37.f), br);
            break;
        }
        case RK_HOTKEY: {
            UiText(rt, br, app.fmtBody.Get(), r.title, label, T.text, DWRITE_TEXT_ALIGNMENT_LEADING);
            D2D1_RECT_F c = CtrlRect(W, r);
            bool cap = (app.capRow == r.id);
            bool failed = !cap && RowFailed(app, r.id);   // занято другой программой — не работает
            UiFillRR(rt, br, c, RAD, T.control, 1.0f);
            if (cap) {   // фокус: кольцо 3 px (акцент 18 %) + рамка акцентом
                D2D1_RECT_F ring = { c.left - 3.f, c.top - 3.f, c.right + 3.f, c.bottom + 3.f };
                UiStrokeRR(rt, br, ring, RAD + 3.f, T.accent, 0.18f, 3.f);
                UiStrokeRR(rt, br, c, RAD, T.accent, 1.0f, 1.f);
            } else {
                UiStrokeRR(rt, br, c, RAD, failed ? T.danger : T.border, 1.0f, 1.f);
            }
            std::wstring t = cap ? (IsGroup(r.id) ? Tr(S_CAP_DIGIT) : Tr(S_CAP_KEYS))
                                 : RowComboText(app, r.id);
            UiText(rt, br, app.fmtCtrl.Get(), t, c, cap ? T.accentInk : failed ? T.danger : T.text, DWRITE_TEXT_ALIGNMENT_CENTER);
            break;
        }
        case RK_CORNER: {
            UiText(rt, br, app.fmtBody.Get(), r.title, label, T.text, DWRITE_TEXT_ALIGNMENT_LEADING);
            for (int i = 0; i < 4; ++i) {   // сегментный выбор угла
                D2D1_RECT_F s = SegRect(W, r, i);
                bool on = (app.corner == i);
                UiFillRR(rt, br, s, RAD, on ? T.accentSoft : T.control, 1.0f);
                UiStrokeRR(rt, br, s, RAD, on ? T.accent : T.border, 1.0f, 1.f);
                UiText(rt, br, app.fmtBody.Get(), GLYPH[i], s, on ? T.accentInk : T.text, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            break;
        }
        case RK_SWITCH: {
            bool on = (r.id == 0) ? app.mmbAnywhere.load() : (r.id == 1) ? app.moveMaximized : app.glideEnabled;
            UiText(rt, br, app.fmtBody.Get(), r.title, D2D1_RECT_F{ PAD, r.y, W - PAD - 50.f, r.y + r.h }, T.text, DWRITE_TEXT_ALIGNMENT_LEADING);
            D2D1_RECT_F s = SwitchRect(W, r);
            if (on) UiFillRR(rt, br, s, 11.f, T.accent, 1.0f);
            else  { UiFillRR(rt, br, s, 11.f, T.control, 1.0f); UiStrokeRR(rt, br, s, 11.f, T.borderStrong, 1.0f, 1.f); }
            float kx = on ? s.right - 3.f - 16.f : s.left + 3.f;   // кнопка 16 px, отступ 3
            br->SetColor(ToD2D(on ? T.onAccent : T.muted, 1.0f));
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx + 8.f, s.top + 11.f), 8.f, 8.f), br);
            break;
        }
        case RK_RESET: {
            D2D1_RECT_F b = ResetRect(r);   // вторичная кнопка
            UiFillRR(rt, br, b, RAD, T.control, 1.0f);
            UiStrokeRR(rt, br, b, RAD, T.border, 1.0f, 1.f);
            UiText(rt, br, app.fmtCtrl.Get(), r.title, b, T.text, DWRITE_TEXT_ALIGNMENT_CENTER);
            break;
        }
        case RK_STATUS:
            UiText(rt, br, app.fmtSmall.Get(), app.status, D2D1_RECT_F{ PAD, r.y, W - PAD, r.y + r.h }, T.danger, DWRITE_TEXT_ALIGNMENT_LEADING);
            break;
        }
    }
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    if (rt->EndDraw() == D2DERR_RECREATE_TARGET) { app.setBrush.Reset(); app.setRT.Reset(); }
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    AppState& app = *g_app;
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(hwnd, &ps);
        RenderSettings(app, hwnd);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        if (app.setRT && LOWORD(lp) && HIWORD(lp)) app.setRT->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        return 0;
    case WM_MOUSEWHEEL: {
        int d = (short)HIWORD(wp);
        app.setScroll = max(0, min(MaxScroll(app, hwnd), app.setScroll - d / WHEEL_DELTA * 48));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        RECT rc; GetClientRect(hwnd, &rc);
        const float W = (float)rc.right / app.setScale;
        float x = (float)(short)LOWORD(lp) / app.setScale;
        float y = (float)(short)HIWORD(lp) / app.setScale + (float)app.setScroll;
        SettingsLayout L = BuildLayout(app);
        app.capRow = -1; app.status = FirstFailedText(app);
        for (auto& r : L.rows) {
            if (y < r.y || y > r.y + r.h) continue;
            switch (r.kind) {
            case RK_HOTKEY:
                if (In(CtrlRect(W, r), x, y)) app.capRow = r.id;
                break;
            case RK_CORNER:
                for (int i = 0; i < 4; ++i)
                    if (In(SegRect(W, r, i), x, y)) { app.corner = i; PlaceHud(app); SaveUiCfg(app); }
                break;
            case RK_SWITCH:   // переключается вся строка — цель крупнее
                if (r.id == 0) app.mmbAnywhere.store(!app.mmbAnywhere.load());
                else if (r.id == 1) { app.moveMaximized = !app.moveMaximized; RepositionAll(app); }
                else { app.glideEnabled = !app.glideEnabled; app.glideBroken = false; }
                SaveUiCfg(app);
                break;
            case RK_RESET:
                if (In(ResetRect(r), x, y)) {
                    InitDefaultKeys(app);
                    UnregisterAllHotkeys(app); RegisterAllHotkeys(app); SaveHotkeys(app);
                    app.status = FirstFailedText(app);
                }
                break;
            }
            break;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        if (app.capRow < 0) break;
        UINT vk = (UINT)wp;
        if (vk == VK_ESCAPE) { app.capRow = -1; app.status = FirstFailedText(app); InvalidateRect(hwnd, nullptr, FALSE); return 0; }
        if (IsModifierVk(vk)) return 0;
        UINT mods = CurrentMods();
        if (mods == 0) return 0;             // требуем хотя бы один модификатор
        ApplyCapture(app, mods, vk);         // не применилось (конфликт / не цифра для мест) —
        InvalidateRect(hwnd, nullptr, FALSE); // причина в status, остаёмся в перехвате
        return 0;
    }
    case WM_DPICHANGED: {
        // Окно перетащили на монитор с другим масштабом: система предлагает новый
        // прямоугольник, содержимое перерисуется в новом масштабе.
        // Если окно на другой монитор ставит сама OpenSettings, размер под его масштаб уже
        // посчитан — предложенный системой прямоугольник масштабировал бы его второй раз.
        if (s_placing) return 0;
        app.setScale = app.forceScale > 0.0f ? app.forceScale : HIWORD(wp) / 96.0f;
        const RECT* to = (const RECT*)lp;
        SetWindowPos(hwnd, nullptr, to->left, to->top, to->right - to->left, to->bottom - to->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_CLOSE:
        app.capRow = -1;
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Открыть/закрыть окно настроек (шестерёнка на миникарте, иконка в трее).
// Высота — по содержимому, но не выше рабочей области; остальное — прокруткой.
void OpenSettings(AppState& app) {
    if (!app.settings) return;
    if (IsWindowVisible(app.settings)) { ShowWindow(app.settings, SW_HIDE); return; }
    app.setScroll = 0;
    POINT pt; GetCursorPos(&pt);
    bool nearHud = app.hudVisible && app.hud;
    MONITORINFO mi = { sizeof(mi) };
    HMONITOR mon = nearHud ? MonitorFromWindow(app.hud, MONITOR_DEFAULTTONEAREST)
                           : MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(mon, &mi);
    const RECT& wa = mi.rcWork;
    // Размеры заданы в логических пикселях; окно — в физических, по масштабу монитора.
    const float sc = app.setScale = MonitorScale(app, mon);
    app.status = FirstFailedText(app);   // неработающие сочетания видно сразу при открытии
    float contentH = BuildLayout(app).contentH * sc;
    RECT cr = { 0, 0, (LONG)lround(SET_W * sc), (LONG)min(contentH, (float)(wa.bottom - wa.top) - 48.f * sc) };
    DWORD style = (DWORD)GetWindowLongW(app.settings, GWL_STYLE);
    DWORD exStyle = (DWORD)GetWindowLongW(app.settings, GWL_EXSTYLE);
    AdjustWindowRectExForDpi(&cr, style, FALSE, exStyle, (UINT)lround(96.0f * sc));
    int w = cr.right - cr.left, hgt = cr.bottom - cr.top;
    int gap = (int)lround(6 * sc), edge = (int)lround(16 * sc);
    int x, yy;
    if (nearHud) {   // под миникартой; не помещается — над ней
        RECT hr; GetWindowRect(app.hud, &hr);
        x = hr.right - w; yy = hr.bottom + gap;
        if (yy + hgt > wa.bottom) yy = hr.top - gap - hgt;
    } else {         // из трея: у правого-нижнего края рабочей области
        x = wa.right - w - edge; yy = wa.bottom - hgt - edge;
    }
    if (yy < wa.top) yy = wa.top;
    if (x + w > wa.right) x = wa.right - w;
    if (x < wa.left) x = wa.left;
    s_placing = true;
    SetWindowPos(app.settings, HWND_TOPMOST, x, yy, w, hgt, SWP_SHOWWINDOW);
    s_placing = false;
    SetForegroundWindow(app.settings);
    app.capRow = -1;
    InvalidateRect(app.settings, nullptr, TRUE);
}
