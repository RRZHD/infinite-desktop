// Оконные процедуры HUD, подъезд к окну, Aero Snap, иконки, DPI
// Часть InfiniteDesktop.

#include "id_common.h"

// ---------- Оконная процедура HUD ----------
LRESULT CALLBACK HudProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    AppState& app = *g_app;
    if (msg == WM_TRAY) { OnTrayMessage(app, wp, lp); return 0; }
    if (app.taskbarCreatedMsg && msg == app.taskbarCreatedMsg) {   // Explorer перезапустился
        app.trayAdded = false; AddTrayIcon(app); return 0;
    }
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        EnsureHudCache(app, hdc, rc.right, rc.bottom);   // буфер живёт между кадрами
        if (app.hudMemDC) {
            DrawMinimap(app, app.hudMemDC, rc);
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, app.hudMemDC, 0, 0, SRCCOPY);
        } else {
            DrawMinimap(app, hdc, rc);                   // нет памяти под буфер — рисуем напрямую
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        RECT rc; GetClientRect(hwnd, &rc);
        int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
        RECT gr = GearRect(rc);
        if (mx >= gr.left && mx <= gr.right && my >= gr.top && my <= gr.bottom) {
            OpenSettings(app);   // клик по шестерёнке — настройки клавиш
            return 0;
        }
        SetCapture(hwnd); app.hudDrag = true;   // зажать и тащить — камера следует за курсором
        ComputeXform(app, rc);
        double wx, wy; MapToWorld(app, mx, my, wx, wy);
        CenterOn(app, wx, wy);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!app.hudDrag || GetCapture() != hwnd) return 0;
        RECT rc; GetClientRect(hwnd, &rc);
        ComputeXform(app, rc);
        double wx, wy; MapToWorld(app, (short)LOWORD(lp), (short)HIWORD(lp), wx, wy);
        CenterOn(app, wx, wy);   // обновляем цель — камера непрерывно едет за курсором
        return 0;
    }
    case WM_LBUTTONUP: {
        if (app.hudDrag) { app.hudDrag = false; if (GetCapture() == hwnd) ReleaseCapture(); }
        return 0;
    }
    case WM_DISPLAYCHANGE: {
        // изменилась конфигурация мониторов
        UpdateVirtualScreen(app);
        LoadWallpaper(app);                        // перерисовать обои под новое разрешение
        PlaceHud(app);                             // миникарта — в свой угол новой раскладки
        if (app.ov) SetWindowPos(app.ov, HWND_TOPMOST, app.vsX, app.vsY, app.vsW, app.vsH,
                               SWP_NOACTIVATE | (app.overview ? 0 : SWP_NOREDRAW));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lp && !lstrcmpW((LPCWSTR)lp, L"Control Panel\\Desktop")) LoadWallpaper(app);
        if (lp && !lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet")) OnThemeChanged(app);   // светлая/тёмная, акцент
        break;
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        OnThemeChanged(app);
        break;
    case WM_HOTKEY: {
        if (app.capRow >= HK_LEFT) {   // окно настроек ждёт сочетание — перехватываем
            ApplyCapture(app, LOWORD(lp), HIWORD(lp));   // конфликт/не цифра — причина в status
            if (app.settings) InvalidateRect(app.settings, nullptr, FALSE);
            return 0;
        }
        switch (wp) {
        case HK_LEFT:    PanBy(app, -PAN_STEP, 0); break;
        case HK_RIGHT:   PanBy(app, PAN_STEP, 0); break;
        case HK_UP:      PanBy(app, 0, -PAN_STEP); break;
        case HK_DOWN:    PanBy(app, 0,  PAN_STEP); break;
        case HK_HOME:    GoHome(app); break;
        case HK_REFRESH: RebuildWindowList(app); RepositionAll(app); break;
        case HK_MAP:     ToggleMinimap(app); break;    // общие с меню трея
        case HK_PREVIEW: TogglePreviews(app); break;
        case HK_PIN: {
            // закрепить/открепить активное окно (не двигается с панорамой)
            HWND fg = GetForegroundWindow();
            for (auto& w : app.wins) if (w.hwnd == fg) { w.pinned = !w.pinned; break; }
            if (app.hudVisible) InvalidateRect(app.hud, nullptr, FALSE);
            break;
        }
        case HK_QUIT:    PostQuitMessage(0); break;
        case HK_FOCUS_LEFT:  FocusDir(app, -1,  0); break;
        case HK_FOCUS_RIGHT: FocusDir(app,  1,  0); break;
        case HK_FOCUS_UP:    FocusDir(app,  0, -1); break;
        case HK_FOCUS_DOWN:  FocusDir(app,  0,  1); break;
        default:
            if (wp >= HK_MARK1 && wp <= HK_MARK9) SetMark(app,  (int)(wp - HK_MARK1));
            if (wp >= HK_GOTO1 && wp <= HK_GOTO9) GotoMark(app, (int)(wp - HK_GOTO1));
            break;
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
    AppState& app = *g_app;
    HINSTANCE hInst = (HINSTANCE)param;
    app.mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, hInst, 0);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        // hook вызывается системой; цикл нужен лишь чтобы поток жил и принял WM_QUIT
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (app.mouseHook) { UnhookWindowsHookEx(app.mouseHook); app.mouseHook = nullptr; }
    return 0;
}

// Подъезд камеры к активируемому окну (Win+Tab, Alt+Tab, клик по панели задач).
// Если окно стало активным, но не видно целиком в текущем вьюпорте — плавно
// центрируем стол на нём, вместо того чтобы оставить его за краем экрана.

// Центрировать мировую точку по центру монитора, где сейчас курсор (там, где
// окно «вызывается»), а не по центру всех мониторов.
// Подвезти камеру так, чтобы окно оказалось в рабочей области монитора под курсором
// (там, где его «вызвали» — панель задач, Alt+Tab, фокус стрелками). Вписываем ВИДИМУЮ
// рамку окна в rcWork, а не центр окна в центр монитора: окно, подогнанное под рабочую
// область, при центрировании по монитору уезжало вниз на полвысоты панели задач — сверху
// оставалась пустая полоса. По каждой оси: меньше рабочей области — по центру, иначе —
// к её краю (верхнему/левому).
void BringWindowIntoView(AppState& app, const TrackedWin& w) {
    RECT vw = VisWorld(w);
    POINT pt;
    MONITORINFO mi = { sizeof(mi) };
    if (!GetCursorPos(&pt) || !GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi)) {
        CenterOn(app, (vw.left + vw.right) / 2.0, (vw.top + vw.bottom) / 2.0);
        return;
    }
    const RECT& wa = mi.rcWork;
    double ww = vw.right - vw.left,  wh = vw.bottom - vw.top;
    double waW = wa.right - wa.left, waH = wa.bottom - wa.top;
    double sx = (ww < waW) ? wa.left + (waW - ww) / 2.0 : (double)wa.left;   // экранный left видимой рамки
    double sy = (wh < waH) ? wa.top  + (waH - wh) / 2.0 : (double)wa.top;    // экранный top
    app.targetX = vw.left - sx;   // экран = мир − камера
    app.targetY = vw.top  - sy;
}

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                  LONG idObject, LONG, DWORD, DWORD) {
    AppState& app = *g_app;
    if (idObject != OBJID_WINDOW || !hwnd) return;

    // Пользователь начал/закончил тащить или ресайзить окно мышью (или Win+стрелки).
    // Пока идёт перетаскивание, FrameTick читает позицию этого окна каждый кадр —
    // миникарта следует за ним плавно, а не рывками фоновой синхронизации раз в 300 мс.
    if (event == EVENT_SYSTEM_MOVESIZESTART) {
        if (IsTracked(app, hwnd)) app.moveSizeHwnd = hwnd;
        return;
    }
    if (event == EVENT_SYSTEM_MOVESIZEEND) {
        if (app.moveSizeHwnd == hwnd) {
            app.moveSizeHwnd = nullptr;
            SyncWorldFromScreen(app);   // финальная позиция + перерисовка при смене размера
            if (app.hudVisible) RefreshHud(app);
        }
        return;
    }

    if (event != EVENT_SYSTEM_FOREGROUND) return;
    if (app.overview) return;   // в режиме обзора зум не трогаем
    for (auto& w : app.wins) {
        if (w.hwnd != hwnd) continue;
        LONG cx = (LONG)llround(app.camX), cy = (LONG)llround(app.camY);
        RECT s = { w.world.left - cx, w.world.top - cy,
                   w.world.right - cx, w.world.bottom - cy };   // позиция на экране
        // Видно хотя бы частично? Тогда не трогаем — фокус только для окон,
        // которые целиком за пределами всех мониторов.
        bool visible = (s.left < app.vsX + app.vsW && s.right > app.vsX &&
                        s.top  < app.vsY + app.vsH && s.bottom > app.vsY);
        if (!visible) BringWindowIntoView(app, w);   // подъехать к окну
        return;
    }
}

// Aero Snap (прилипание окон к краям/верху экрана). На бесконечном столе краёв
// нет, поэтому на время работы отключаем системное «упорядочивание окон» и
// восстанавливаем прежнее значение при выходе.

void DisableSnap(AppState& app) {
    app.prevWinArranging = TRUE;
    if (!SystemParametersInfoW(SPI_GETWINARRANGING, 0, &app.prevWinArranging, 0)) return;
    if (app.prevWinArranging)
        SystemParametersInfoW(SPI_SETWINARRANGING, FALSE, nullptr, SPIF_SENDCHANGE);
}

void RestoreSnap(AppState& app) {
    if (!app.prevWinArranging) return;
    SystemParametersInfoW(SPI_SETWINARRANGING, TRUE, nullptr, SPIF_SENDCHANGE);
}

// Иконки рабочего стола рисует окно SHELLDLL_DefView (дочернее у Progman/WorkerW).
// Скрываем их на время работы и возвращаем при выходе.

static BOOL CALLBACK FindDefViewProc(HWND top, LPARAM lp) {
    HWND def = FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr);
    if (def) { *(HWND*)lp = def; return FALSE; }
    return TRUE;
}

static HWND FindDesktopIcons() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    HWND def = progman ? FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr) : nullptr;
    if (!def) EnumWindows(FindDefViewProc, (LPARAM)&def);  // иногда внутри WorkerW
    return def;
}

void HideDesktopIcons(AppState& app) {
    app.iconsWin = FindDesktopIcons();
    if (app.iconsWin) {
        app.iconsWereVisible = IsWindowVisible(app.iconsWin) != 0;
        if (app.iconsWereVisible) ShowWindow(app.iconsWin, SW_HIDE);
    }
}

// Explorer перезапустился (сбой, обновление, перезапуск из-за модов оболочки) — окно
// иконок пересоздано, прежний хэндл мёртв, а новые иконки снова видны. Прячем заново.
// Вызывается из фоновой синхронизации; в обычной ситуации это один IsWindow.
void RecheckDesktopIcons(AppState& app) {
    if (app.iconsWin && IsWindow(app.iconsWin)) {
        // окно то же, но иконки снова показаны (гонка при старте Explorer) — прячем
        if (IsWindowVisible(app.iconsWin)) { app.iconsWereVisible = true; ShowWindow(app.iconsWin, SW_HIDE); }
        return;
    }
    HideDesktopIcons(app);
}

void RestoreDesktopIcons(AppState& app) {
    if (app.iconsWin && app.iconsWereVisible && IsWindow(app.iconsWin))
        ShowWindow(app.iconsWin, SW_SHOW);
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



