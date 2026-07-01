// Оконные процедуры HUD, подъезд к окну, Aero Snap, иконки, DPI
// Часть InfiniteDesktop. Компилируется как единый модуль через main.cpp.

#include "id_common.h"

// ---------- Оконная процедура HUD ----------
LRESULT CALLBACK HudProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        ScopedDC mem(CreateCompatibleDC(hdc));
        ScopedBitmap bmp(CreateCompatibleBitmap(hdc, rc.right, rc.bottom));
        HGDIOBJ old = SelectObject(mem, bmp);
        DrawMinimap(mem, rc);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        RECT rc; GetClientRect(hwnd, &rc);
        int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
        RECT gr = GearRect(rc);
        if (mx >= gr.left && mx <= gr.right && my >= gr.top && my <= gr.bottom) {
            OpenSettings();   // клик по шестерёнке — настройки клавиш
            return 0;
        }
        SetCapture(hwnd); g_hudDrag = true;   // зажать и тащить — камера следует за курсором
        ComputeXform(rc);
        double wx, wy; MapToWorld(mx, my, wx, wy);
        CenterOn(wx, wy);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g_hudDrag || GetCapture() != hwnd) return 0;
        RECT rc; GetClientRect(hwnd, &rc);
        ComputeXform(rc);
        double wx, wy; MapToWorld((short)LOWORD(lp), (short)HIWORD(lp), wx, wy);
        CenterOn(wx, wy);   // обновляем цель — камера непрерывно едет за курсором
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_hudDrag) { g_hudDrag = false; if (GetCapture() == hwnd) ReleaseCapture(); }
        return 0;
    }
    case WM_DISPLAYCHANGE: {
        // изменилась конфигурация мониторов
        UpdateVirtualScreen();
        LoadWallpaper();                        // перерисовать обои под новое разрешение
        PlaceHud();                             // миникарта — в свой угол новой раскладки
        if (g_ov) SetWindowPos(g_ov, HWND_TOPMOST, g_vsX, g_vsY, g_vsW, g_vsH,
                               SWP_NOACTIVATE | (g_overview ? 0 : SWP_NOREDRAW));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lp && !lstrcmpW((LPCWSTR)lp, L"Control Panel\\Desktop")) LoadWallpaper();
        break;
    case WM_HOTKEY: {
        if (g_capRow >= HK_LEFT) {   // окно настроек ждёт сочетание — перехватываем
            UINT mods = LOWORD(lp), vk = HIWORD(lp);
            int conflict = FindConflict(mods, vk, g_capRow);
            if (conflict != -1) {    // уже занято другим действием — предупреждаем, не применяем
                g_status = L"⚠ " + KeyComboText(mods, vk) + L" уже занято: " + g_keys[conflict].name;
            } else {
                g_keys[g_capRow].mods = mods; g_keys[g_capRow].vk = vk;
                g_capRow = -1; g_status.clear();
                UnregisterAllHotkeys(); RegisterAllHotkeys(); SaveHotkeys();
            }
            if (g_settings) InvalidateRect(g_settings, nullptr, FALSE);
            return 0;
        }
        switch (wp) {
        case HK_LEFT:    PanBy(-PAN_STEP, 0); break;
        case HK_RIGHT:   PanBy( PAN_STEP, 0); break;
        case HK_UP:      PanBy(0, -PAN_STEP); break;
        case HK_DOWN:    PanBy(0,  PAN_STEP); break;
        case HK_HOME:    GoHome(); break;
        case HK_REFRESH: RebuildWindowList(); RepositionAll(); break;
        case HK_MAP:
            g_hudVisible = !g_hudVisible;
            if (g_hudVisible) { RegisterAllThumbs(); ShowWindow(g_hud, SW_SHOWNA); }
            else {
                // RAII: ScopedThumbnail очистит превью при reset().
                for (auto& w : g_wins) w.thumb.reset();
                ShowWindow(g_hud, SW_HIDE);
                if (g_settings) ShowWindow(g_settings, SW_HIDE);  // шестерёнка скрывается с картой
            }
            break;
        case HK_PREVIEW:
            g_previewsOn = !g_previewsOn;
            if (g_previewsOn) { if (g_hudVisible) RegisterAllThumbs(); }
            else {
                // RAII: ScopedThumbnail очистит превью при reset().
                for (auto& w : g_wins) w.thumb.reset();
            }
            InvalidateRect(g_hud, nullptr, FALSE);
            break;
        case HK_PIN: {
            // закрепить/открепить активное окно (не двигается с панорамой)
            HWND fg = GetForegroundWindow();
            for (auto& w : g_wins) if (w.hwnd == fg) { w.pinned = !w.pinned; break; }
            if (g_hudVisible) InvalidateRect(g_hud, nullptr, FALSE);
            break;
        }
        case HK_QUIT:    PostQuitMessage(0); break;
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Отдельный поток с собственным циклом сообщений для low-level mouse hook.
// Изоляция от главного потока: даже если тот подвиснет (например, на синхронном
// вызове к занятому окну), перехват СКМ/колеса продолжит работать и Windows не
// удалит hook по таймауту.
DWORD WINAPI HookThreadProc(LPVOID param) {
    HINSTANCE hInst = (HINSTANCE)param;
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, hInst, 0);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        // hook вызывается системой; цикл нужен лишь чтобы поток жил и принял WM_QUIT
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
    return 0;
}

// Подъезд камеры к активируемому окну (Win+Tab, Alt+Tab, клик по панели задач).
// Если окно стало активным, но не видно целиком в текущем вьюпорте — плавно
// центрируем стол на нём, вместо того чтобы оставить его за краем экрана.

// Центрировать мировую точку по центру монитора, где сейчас курсор (там, где
// окно «вызывается»), а не по центру всех мониторов.
void CenterOnCursorMonitor(double wx, double wy) {
    POINT pt;
    if (!GetCursorPos(&pt)) { CenterOn(wx, wy); return; }
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi)) {
        g_targetX = wx - (mi.rcMonitor.left + mi.rcMonitor.right) / 2.0;
        g_targetY = wy - (mi.rcMonitor.top  + mi.rcMonitor.bottom) / 2.0;
    } else {
        CenterOn(wx, wy);
    }
}

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                  LONG idObject, LONG, DWORD, DWORD) {
    if (event != EVENT_SYSTEM_FOREGROUND || idObject != OBJID_WINDOW || !hwnd) return;
    if (g_overview) return;   // в режиме обзора зум не трогаем
    for (auto& w : g_wins) {
        if (w.hwnd != hwnd) continue;
        LONG cx = (LONG)llround(g_camX), cy = (LONG)llround(g_camY);
        RECT s = { w.world.left - cx, w.world.top - cy,
                   w.world.right - cx, w.world.bottom - cy };   // позиция на экране
        // Видно хотя бы частично? Тогда не трогаем — фокус только для окон,
        // которые целиком за пределами всех мониторов.
        bool visible = (s.left < g_vsX + g_vsW && s.right > g_vsX &&
                        s.top  < g_vsY + g_vsH && s.bottom > g_vsY);
        if (!visible)
            CenterOnCursorMonitor((w.world.left + w.world.right) / 2.0,
                                  (w.world.top + w.world.bottom) / 2.0);   // подъехать к окну
        return;
    }
}

// Aero Snap (прилипание окон к краям/верху экрана). На бесконечном столе краёв
// нет, поэтому на время работы отключаем системное «упорядочивание окон» и
// восстанавливаем прежнее значение при выходе.

void DisableSnap() {
    g_prevWinArranging = TRUE;
    if (!SystemParametersInfoW(SPI_GETWINARRANGING, 0, &g_prevWinArranging, 0)) return;
    if (g_prevWinArranging)
        SystemParametersInfoW(SPI_SETWINARRANGING, FALSE, nullptr, SPIF_SENDCHANGE);
}

void RestoreSnap() {
    if (!g_prevWinArranging) return;
    SystemParametersInfoW(SPI_SETWINARRANGING, TRUE, nullptr, SPIF_SENDCHANGE);
}

// Иконки рабочего стола рисует окно SHELLDLL_DefView (дочернее у Progman/WorkerW).
// Скрываем их на время работы и возвращаем при выходе.

BOOL CALLBACK FindDefViewProc(HWND top, LPARAM lp) {
    HWND def = FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr);
    if (def) { *(HWND*)lp = def; return FALSE; }
    return TRUE;
}

HWND FindDesktopIcons() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    HWND def = progman ? FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr) : nullptr;
    if (!def) EnumWindows(FindDefViewProc, (LPARAM)&def);  // иногда внутри WorkerW
    return def;
}

void HideDesktopIcons() {
    g_iconsWin = FindDesktopIcons();
    if (g_iconsWin) {
        g_iconsWereVisible = IsWindowVisible(g_iconsWin) != 0;
        if (g_iconsWereVisible) ShowWindow(g_iconsWin, SW_HIDE);
    }
}

void RestoreDesktopIcons() {
    if (g_iconsWin && g_iconsWereVisible && IsWindow(g_iconsWin))
        ShowWindow(g_iconsWin, SW_SHOW);
}

// Включить осведомлённость о DPI на уровне монитора (для точных координат
// при разных масштабах на разных мониторах), с откатом на системный режим.
void EnableDpiAwareness() {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    typedef BOOL (WINAPI *PFN)(HANDLE);
    if (u32) {
        auto fn = (PFN)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (fn && fn((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */)) return;
    }
    SetProcessDPIAware();
}


