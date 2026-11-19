// Оконные процедуры HUD, подъезд к окну, Aero Snap, иконки, DPI
// Часть InfiniteDesktop.

#include "id_common.h"

// ---------- Оконная процедура HUD ----------
LRESULT CALLBACK HudProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    AppState& app = *g_app;
    if (msg == WM_TRAY) { OnTrayMessage(app, wp, lp); return 0; }
    if (msg == WM_SHOWSETTINGS) {   // программу запустили второй раз — показываем, что она уже работает
        if (app.settings && !IsWindowVisible(app.settings)) OpenSettings(app);
        else if (app.settings) SetForegroundWindow(app.settings);
        return 0;
    }
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
        RECT gr = GearRect(app, rc);
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
        // Места парковки считались от прежних границ экрана: окно, стоявшее за старым
        // краем, могло оказаться на новом мониторе или вне всех. Пересчитываем все.
        for (auto& w : app.wins) w.parkedOff = false;
        if (!app.overview) RepositionAll(app);   // в обзоре окна встанут на места при выходе
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_DPICHANGED:
        PlaceHud(app);                             // новый масштаб — новый размер миникарты
        return 0;
    // Windows завершает сеанс (выключение, перезагрузка, выход из системы). Процесс после
    // этого просто снимут, до кода после цикла сообщений дело не дойдёт — а для программы
    // в трее это и есть обычный конец работы. Раскладку сохраняем уже на запросе: дальше
    // система закрывает окна по одному, и автосохранение записало бы пустеющий стол.
    case WM_QUERYENDSESSION:
        if (!app.sessionEnding) {
            app.sessionEnding = true;
            if (!app.overview) SyncWorldFromScreen(app);
            SaveLayout(app);
        }
        return TRUE;
    case WM_ENDSESSION:
        if (wp) {                                  // сеанс действительно завершается
            RestoreSnap(app);
            RestoreDesktopIcons(app);
            EndSession();
        } else {
            app.sessionEnding = false;             // завершение отменили — работаем дальше
        }
        return 0;
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
    GlideRestack(app);          // плавная панорама: активированное окно система подняла над слоем превью
    app.pendingFocus = nullptr;
    // Окна ещё нет в списке (только что открыто или развёрнуто из свёрнутого) — берём
    // сразу, не дожидаясь фоновой синхронизации. Не взялось (ещё разворачивается) —
    // запоминаем: подъедем, когда синхронизация его добавит.
    if (!IsTracked(app, hwnd)) {
        app.pendingFocus = hwnd;
        AddNewWindows(app);     // добавив это окно, сам подъедет и сбросит pendingFocus
        return;
    }
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

// Ищем только внутри окон рабочего стола оболочки. SHELLDLL_DefView есть и в окнах
// Проводника, и в диалогах выбора файла старого типа (там он — прямой потомок диалога):
// поиск по одному классу мог спрятать список файлов в таком диалоге вместо иконок стола.
static BOOL CALLBACK FindDefViewProc(HWND top, LPARAM lp) {
    if (!IsShellDesktopWindow(top)) return TRUE;
    HWND def = FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr);
    if (def) { *(HWND*)lp = def; return FALSE; }
    return TRUE;
}

static HWND FindDesktopIcons() {
    HWND def = nullptr;
    EnumWindows(FindDefViewProc, (LPARAM)&def);   // Progman либо отдельный слой WorkerW
    return def;
}

// Где живёт DefView, зависит от системы:
//  - прямо в Progman — Windows 11 24H2+ и Windows 10, пока стол не разнесён на слои;
//  - в отдельном окне верхнего уровня WorkerW, лежащем ПОВЕРХ слоя обоев, — так Windows 10
//    и ранние сборки 11 раскладывают стол после сообщения 0x052C (его шлют программы
//    живых обоев и сама оболочка при плавной смене обоев).
// В первом случае хватает скрыть DefView. Во втором этого мало: окно-слой остаётся в
// композиции уже без представления иконок, которое обычно закрывает его целиком, и его
// поверхность накладывается на обои под ним. По issue #4 (Windows 10 + Wallpaper Engine)
// стол при этом заметно светлеет, а до v1.0.15 живые обои могли не показаться вовсе.
// Поэтому слой, в котором нет ничего, кроме иконок, скрываем целиком.
struct LayerCtx { HWND def; bool other; };
static BOOL CALLBACK LayerChildProc(HWND c, LPARAM lp) {
    LayerCtx* x = (LayerCtx*)lp;
    if (c == x->def || IsChild(x->def, c)) return TRUE;                 // сами иконки
    if (GetWindowLongW(c, GWL_STYLE) & WS_VISIBLE) { x->other = true; return FALSE; }
    return TRUE;
}
static HWND IconsLayerOf(HWND def) {
    HWND top = GetAncestor(def, GA_ROOT);
    wchar_t cls[32] = {};
    if (!top || top == def || !GetClassNameW(top, cls, 32) || lstrcmpW(cls, L"WorkerW")) return nullptr;
    if (!IsShellDesktopWindow(top)) return nullptr;      // чужой WorkerW — не слой рабочего стола
    LayerCtx x = { def, false };
    EnumChildWindows(top, LayerChildProc, (LPARAM)&x);   // в слое есть что-то ещё (обои?) — не трогаем
    return x.other ? nullptr : top;
}

// Спрятать иконки, а если они в отдельном слое — и слой. Вызывается при старте и из
// фоновой синхронизации: окно иконок пересоздаётся при перезапуске Explorer (сбой,
// обновление, моды оболочки), а при запуске программы живых обоев оболочка переносит его
// в новый слой и показывает тот заново. В обычной ситуации это несколько дешёвых проверок.
// Окна принадлежат Explorer, поэтому ShowWindowAsync: синхронный ShowWindow ждал бы его
// поток, и если тот занят или завис (как раз при перезапуске), встал бы наш цикл кадров.
void HideDesktopIcons(AppState& app) {
    bool fresh = false;
    if (!app.iconsWin || !IsWindow(app.iconsWin)) {
        app.iconsWin = FindDesktopIcons();
        app.iconsWereVisible = false;
        if (!app.iconsWin) return;
        fresh = true;
        app.sessionDirty = true;
    }
    // Собственный флаг видимости, а не IsWindowVisible: та вернёт FALSE и для видимого
    // окна внутри скрытого слоя.
    if (GetWindowLongW(app.iconsWin, GWL_STYLE) & WS_VISIBLE) {
        if (!app.iconsWereVisible) app.sessionDirty = true;
        app.iconsWereVisible = true;
        ShowWindowAsync(app.iconsWin, SW_HIDE);
    }
    HWND layer = IconsLayerOf(app.iconsWin);
    bool layerChanged = (layer != app.iconsLayer);
    if (layerChanged) {
        // Прежний слой перестал быть «слоем одних иконок» (в нём появилось чужое окно —
        // например, обои — или иконки переехали). Скрытым его оставлять нельзя: вместе с
        // ним пропало бы и то, что в нём теперь лежит. Скрывали мы — мы и возвращаем.
        if (app.iconsLayerOurs && app.iconsLayer && IsWindow(app.iconsLayer))
            ShowWindowAsync(app.iconsLayer, SW_SHOWNA);
        app.iconsLayer = layer;
        app.iconsLayerOurs = false;
        app.sessionDirty = true;
    }
    if (layer && IsWindowVisible(layer)) {
        ShowWindowAsync(layer, SW_HIDE);
        if (!app.iconsLayerOurs) app.sessionDirty = true;
        app.iconsLayerOurs = true;    // «наш» — только если скрыли сами, а не застали скрытым
    }
    static int logged = 0;            // раскладка стола — в лог, но не бесконечно
    if ((fresh || layerChanged) && logged < 20) {
        ++logged;
        LogDesktopTree(layer ? L"icons hidden with their layer" : L"icons hidden");
    }
}

// Сначала иконки (пока слой скрыт), потом слой — чтобы пустой слой не мелькнул над обоями.
// Слой показываем, только если скрывали его сами: иначе после сбоя следующий запуск
// «возвращал» бы пустой слой без иконок.
void RestoreDesktopIcons(AppState& app) {
    if (!app.iconsWin || !IsWindow(app.iconsWin)) return;
    if (app.iconsWereVisible) ShowWindowAsync(app.iconsWin, SW_SHOWNA);
    if (app.iconsLayerOurs && app.iconsLayer && IsWindow(app.iconsLayer))
        ShowWindowAsync(app.iconsLayer, SW_SHOWNA);
}

// ---------- Файл сеанса ----------
// Пока программа работает, в session.txt лежит то, что было ДО неё: включён ли Snap,
// видны ли иконки, скрыт ли нами слой иконок. При чистом выходе файл удаляется. Если при
// запуске он есть, прошлый сеанс оборвался (сбой, снятие процесса) и ничего не вернул:
// система сейчас в «нашем» состоянии, и без файла мы приняли бы его за исходное —
// выключенный Snap и спрятанные иконки остались бы такими навсегда.
static std::wstring SessionPath() { return ConfigPath(L"session.txt"); }

void SaveSession(AppState& app) {
    app.sessionDirty = false;
    std::ofstream f(SessionPath().c_str(), std::ios::binary | std::ios::trunc);
    if (!f) return;
    std::string s = "snap "  + std::to_string(app.prevWinArranging ? 1 : 0) + "\n"
                  + "icons " + std::to_string(app.iconsWereVisible ? 1 : 0) + "\n"
                  + "layer " + std::to_string(app.iconsLayerOurs ? 1 : 0) + "\n";
    f.write(s.data(), (std::streamsize)s.size());
}

// Вызывается после DisableSnap и HideDesktopIcons: они уже записали в состояние то, что
// застали; здесь оно поправляется по файлу оборванного сеанса.
void BeginSession(AppState& app) {
    std::ifstream f(SessionPath().c_str(), std::ios::binary);
    if (f) {
        std::string key; int v = 0;
        while (f >> key >> v) {
            if (key == "snap"  && v) app.prevWinArranging = TRUE;
            if (key == "icons" && v && app.iconsWin) app.iconsWereVisible = true;
            if (key == "layer" && v && app.iconsLayer) app.iconsLayerOurs = true;
        }
        f.close();
        WallLog(L"session: previous run did not exit cleanly, restoring from its record");
    }
    SaveSession(app);
}

void EndSession() {
    DeleteFileW(SessionPath().c_str());
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

// Масштаб интерфейса для монитора: его DPI относительно 96. Свои окна (миникарта,
// настройки) программа рисует в физических пикселях, поэтому масштабирует их сама.
float MonitorScale(const AppState& app, HMONITOR hm) {
    if (app.forceScale > 0.0f) return app.forceScale;
    UINT dx = 96, dy = 96;
    if (hm && SUCCEEDED(GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy)) && dx > 96) return dx / 96.0f;
    return 1.0f;
}



