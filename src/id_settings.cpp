// Настройки: стиль Win11, горячие клавиши, окно настроек
// Часть InfiniteDesktop. Компилируется как единый модуль через main.cpp.

#include "id_common.h"

// ---------- Стиль Win11, шрифт, размещение миникарты ----------
HFONT UiFont() {
    static HFONT f = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
    return f;
}

// Тёмный заголовок и скруглённые углы (Windows 11)
void ApplyWin11Style(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    int round = 2 /*DWMWCP_ROUND*/;
    DwmSetWindowAttribute(hwnd, 33 /*WINDOW_CORNER_PREFERENCE*/, &round, sizeof(round));
}

std::wstring UiCfgPath() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring dir = (n && n < MAX_PATH) ? std::wstring(buf) : L".";
    dir += L"\\InfiniteDesktop";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\ui.txt";
}
void SaveUiCfg() {
    std::ofstream f(UiCfgPath().c_str(), std::ios::binary | std::ios::trunc);
    if (f) { std::string s = "corner " + std::to_string(g_corner) + "\n"; f.write(s.data(), s.size()); }
}
void LoadUiCfg() {
    std::ifstream f(UiCfgPath().c_str(), std::ios::binary);
    if (!f) return;
    std::string key; int v;
    while (f >> key >> v) if (key == "corner" && v >= 0 && v <= 3) g_corner = v;
}

// Разместить миникарту в выбранном углу рабочей области основного монитора.
void PlaceHud() {
    if (!g_hud) return;
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = (g_corner == 0 || g_corner == 2) ? wa.left + MAP_MARGIN : wa.right - MAP_W - MAP_MARGIN;
    int y = (g_corner == 0 || g_corner == 1) ? wa.top + MAP_MARGIN  : wa.bottom - MAP_H - MAP_MARGIN;
    SetWindowPos(g_hud, HWND_TOPMOST, x, y, MAP_W, MAP_H, SWP_NOACTIVATE);
}
const wchar_t* CornerGlyph() {
    switch (g_corner) { case 0: return L"↖"; case 1: return L"↗"; case 2: return L"↙"; default: return L"↘"; }
}

// ---------- Настраиваемые горячие клавиши ----------
void InitDefaultKeys() {
    g_keys[HK_LEFT]    = { MOD_CONTROL | MOD_ALT, VK_LEFT,  L"Панорама влево" };
    g_keys[HK_RIGHT]   = { MOD_CONTROL | MOD_ALT, VK_RIGHT, L"Панорама вправо" };
    g_keys[HK_UP]      = { MOD_CONTROL | MOD_ALT, VK_UP,    L"Панорама вверх" };
    g_keys[HK_DOWN]    = { MOD_CONTROL | MOD_ALT, VK_DOWN,  L"Панорама вниз" };
    g_keys[HK_HOME]    = { MOD_CONTROL | MOD_ALT, 'H',      L"Домой (камера 0,0)" };
    g_keys[HK_MAP]     = { MOD_CONTROL | MOD_ALT, 'M',      L"Миникарта вкл/выкл" };
    g_keys[HK_REFRESH] = { MOD_CONTROL | MOD_ALT, 'R',      L"Пересобрать окна" };
    g_keys[HK_QUIT]    = { MOD_CONTROL | MOD_ALT, 'Q',      L"Выход" };
    g_keys[HK_PREVIEW] = { MOD_CONTROL | MOD_ALT, 'P',      L"Превью вкл/выкл" };
    g_keys[HK_PIN]     = { MOD_CONTROL | MOD_ALT, 'F',      L"Закрепить окно" };
}

void RegisterAllHotkeys() {
    for (int id = HK_LEFT; id <= HK_PIN; ++id)
        RegisterHotKey(g_hud, id, g_keys[id].mods, g_keys[id].vk);
}
void UnregisterAllHotkeys() {
    for (int id = HK_LEFT; id <= HK_PIN; ++id) UnregisterHotKey(g_hud, id);
}

std::wstring HotkeysPath() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring dir = (n && n < MAX_PATH) ? std::wstring(buf) : L".";
    dir += L"\\InfiniteDesktop";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\hotkeys.txt";
}
void SaveHotkeys() {
    std::ofstream f(HotkeysPath().c_str(), std::ios::binary | std::ios::trunc);
    if (!f) return;
    for (int id = HK_LEFT; id <= HK_PIN; ++id) {
        std::string line = std::to_string(id) + " " + std::to_string(g_keys[id].mods)
                         + " " + std::to_string(g_keys[id].vk) + "\n";
        f.write(line.data(), (std::streamsize)line.size());
    }
}
void LoadHotkeys() {
    std::ifstream f(HotkeysPath().c_str(), std::ios::binary);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream iss(line);
        int id, mods, vk;
        if ((iss >> id >> mods >> vk) && id >= HK_LEFT && id <= HK_PIN) {
            g_keys[id].mods = (UINT)mods;
            g_keys[id].vk   = (UINT)vk;
        }
    }
}

std::wstring KeyName(UINT vk) {
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
std::wstring KeyComboText(UINT mods, UINT vk) {
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl+";
    if (mods & MOD_ALT)     s += L"Alt+";
    if (mods & MOD_SHIFT)   s += L"Shift+";
    if (mods & MOD_WIN)     s += L"Win+";
    s += KeyName(vk);
    return s;
}

bool IsModifierVk(UINT vk) {
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
           vk == VK_LWIN || vk == VK_RWIN ||
           vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL ||
           vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU || vk == VK_CAPITAL;
}
UINT CurrentMods() {
    UINT m = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= MOD_CONTROL;
    if (GetKeyState(VK_MENU)    & 0x8000) m |= MOD_ALT;
    if (GetKeyState(VK_SHIFT)   & 0x8000) m |= MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) m |= MOD_WIN;
    return m;
}

int SetFooterY() { return SET_HEADER + (HK_PIN - HK_LEFT + 1) * SET_ROW + 10; }

// Найти другое действие с тем же сочетанием (или -1).
int FindConflict(UINT mods, UINT vk, int exceptId) {
    for (int id = HK_LEFT; id <= HK_PIN; ++id)
        if (id != exceptId && g_keys[id].mods == mods && g_keys[id].vk == vk) return id;
    return -1;
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        ScopedDC h(CreateCompatibleDC(dc));
        ScopedBitmap bmp(CreateCompatibleBitmap(dc, rc.right, rc.bottom));
        HGDIOBJ ob = SelectObject(h, bmp);
        ScopedBrush bg(CreateSolidBrush(RGB(24, 26, 32))); FillRect(h, &rc, bg);
        SetBkMode(h, TRANSPARENT);
        HGDIOBJ oldF = SelectObject(h, UiFont());
        SetTextColor(h, RGB(210, 215, 225));
        RECT hr = { 14, 10, rc.right - 14, SET_HEADER };
        DrawTextW(h, L"Горячие клавиши\nКлик по сочетанию → нажмите новое (Esc — отмена)",
                  -1, &hr, DT_LEFT | DT_NOPREFIX);
        for (int id = HK_LEFT; id <= HK_PIN; ++id) {
            int y = SET_HEADER + (id - HK_LEFT) * SET_ROW;
            SetTextColor(h, RGB(190, 196, 208));
            RECT nr = { 14, y, SET_BOX_X - 6, y + SET_ROW };
            DrawTextW(h, g_keys[id].name, -1, &nr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            RECT box = { SET_BOX_X, y + 2, SET_BOX_X + SET_BOX_W, y + 2 + SET_BOX_H };
            bool cap = (g_capRow == id);
            ScopedBrush bb(CreateSolidBrush(cap ? RGB(60, 80, 120) : RGB(44, 47, 56)));
            FillRect(h, &box, bb);
            FrameRect(h, &box, (HBRUSH)GetStockObject(GRAY_BRUSH));
            SetTextColor(h, cap ? RGB(255, 230, 140) : RGB(220, 225, 235));
            std::wstring t = cap ? L"нажмите клавиши…" : KeyComboText(g_keys[id].mods, g_keys[id].vk);
            DrawTextW(h, t.c_str(), -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        RECT fr = { 14, SetFooterY(), rc.right - 14, SetFooterY() + 24 };
        SetTextColor(h, RGB(140, 170, 230));
        DrawTextW(h, L"Сбросить по умолчанию", -1, &fr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        RECT crn = { 14, SetFooterY() + 28, rc.right - 14, SetFooterY() + 52 };
        SetTextColor(h, RGB(190, 196, 208));
        std::wstring ct = std::wstring(L"Угол миникарты: ") + CornerGlyph() + L"   (клик — сменить)";
        DrawTextW(h, ct.c_str(), -1, &crn, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (!g_status.empty()) {
            RECT sr = { 14, SetFooterY() + 56, rc.right - 14, SetFooterY() + 78 };
            SetTextColor(h, RGB(255, 180, 90));
            DrawTextW(h, g_status.c_str(), -1, &sr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        SelectObject(h, oldF);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, h, 0, 0, SRCCOPY);
        SelectObject(h, ob);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        // сброс по умолчанию
        if (y >= SetFooterY() && y <= SetFooterY() + 24 && x >= 14 && x < 220) {
            InitDefaultKeys();
            UnregisterAllHotkeys(); RegisterAllHotkeys(); SaveHotkeys();
            g_capRow = -1; g_status.clear(); InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        // смена угла миникарты
        if (y >= SetFooterY() + 28 && y <= SetFooterY() + 52 && x >= 14) {
            g_corner = (g_corner + 1) % 4;
            PlaceHud(); SaveUiCfg();
            g_capRow = -1; g_status.clear(); InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        g_capRow = -1; g_status.clear();
        for (int id = HK_LEFT; id <= HK_PIN; ++id) {
            int by = SET_HEADER + (id - HK_LEFT) * SET_ROW + 2;
            if (x >= SET_BOX_X && x <= SET_BOX_X + SET_BOX_W && y >= by && y <= by + SET_BOX_H) {
                g_capRow = id; break;
            }
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        if (g_capRow < 0) break;
        UINT vk = (UINT)wp;
        if (vk == VK_ESCAPE) { g_capRow = -1; g_status.clear(); InvalidateRect(hwnd, nullptr, FALSE); return 0; }
        if (IsModifierVk(vk)) return 0;
        UINT mods = CurrentMods();
        if (mods == 0) return 0;             // требуем хотя бы один модификатор
        int conflict = FindConflict(mods, vk, g_capRow);
        if (conflict != -1) {                // сочетание уже занято — предупреждаем, не применяем
            g_status = L"⚠ " + KeyComboText(mods, vk) + L" уже занято: " + g_keys[conflict].name;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;                        // остаёмся в перехвате — можно нажать другое
        }
        g_keys[g_capRow].mods = mods;
        g_keys[g_capRow].vk   = vk;
        g_capRow = -1; g_status.clear();
        UnregisterAllHotkeys(); RegisterAllHotkeys(); SaveHotkeys();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_CLOSE:
        g_capRow = -1;
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Открыть/закрыть окно настроек (по клику на шестерёнке).
void OpenSettings() {
    if (!g_settings) return;
    if (IsWindowVisible(g_settings)) { ShowWindow(g_settings, SW_HIDE); return; }
    RECT cr = { 0, 0, SET_W, SetFooterY() + SET_FOOTER };
    DWORD style = (DWORD)GetWindowLongW(g_settings, GWL_STYLE);
    AdjustWindowRect(&cr, style, FALSE);
    RECT hr; GetWindowRect(g_hud, &hr);
    int w = cr.right - cr.left, hgt = cr.bottom - cr.top;
    int x = hr.right - w, yy = hr.bottom + 6;
    SetWindowPos(g_settings, HWND_TOPMOST, x, yy, w, hgt, SWP_SHOWWINDOW);
    SetForegroundWindow(g_settings);
    g_capRow = -1;
    InvalidateRect(g_settings, nullptr, TRUE);
}


