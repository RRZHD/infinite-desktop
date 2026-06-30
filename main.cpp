// InfiniteDesktop - бесконечный зум-канвас рабочего стола для Windows 10/11.
//
// Идея: виртуальный "мир" больше экрана. Камера смотрит в участок мира.
//  - Панорама (1:1): все реальные окна физически переезжают через SetWindowPos,
//    оставаясь полноразмерными и кликабельными => ощущение бесконечного стола.
//  - Обзор (зум-аут): миникарта с ЖИВЫМИ превью окон (DWM-thumbnails) и рамкой
//    текущего вьюпорта; клик по карте = перелёт камеры.
//
// Поддержка нескольких мониторов: сцена строится в координатах виртуального
// экрана (SM_*VIRTUALSCREEN), окна перемещаются по всему рабочему пространству.
//
// Ограничение ОС: живое окно чужого процесса нельзя интерактивно масштабировать,
// поэтому "зум" реализован как навигационный обзор, а взаимодействие — в 1:1.
//
// Горячие клавиши (глобальные):
//   Ctrl+Alt+Стрелки  — панорама камеры
//   Ctrl+Alt+H        — домой (камера в 0,0)
//   Ctrl+Alt+M        — показать/скрыть миникарту
//   Ctrl+Alt+R        — пересобрать список окон
//   Ctrl+Alt+Q        — выход


#include "src/id_common.h"   // типы, inline-глобалы, прототипы (модули — отдельные .cpp)

// ---------- Кадр анимации и точка входа ----------
static void FrameTick(double dt) {
    double kz = 1.0 - pow(1.0 - ZOOM_EASE, dt * 60.0);   // эквивалент при 60 Гц
    if (fabs(g_zoomTarget - g_zoom) > 0.001) g_zoom += (g_zoomTarget - g_zoom) * kz;
    else g_zoom = g_zoomTarget;
    bool wantOv = (g_zoom < 1.0) || (g_zoomTarget < 1.0);
    if (wantOv && !g_overview) ShowOverview();
    if (g_overview) {
        // Скрываем обзор ТОЛЬКО после кадра на масштабе ровно 1.0 — тогда превью
        // точно совпадает с реальным окном (размер и позиция), и переход бесшовный.
        if (!wantOv && g_ovLastZoom >= 1.0) {
            CommitZoomCam();
            RepositionAll();   // окна на местах ДО скрытия обзора
            g_lastCamX = g_camX; g_lastCamY = g_camY;
            HideOverview();
            // дальше — обычная логика (окна уже расставлены)
        } else {
            // периодически перечитываем обои (слайдшоу/смена в реальном времени),
            // когда зум устаканился — чтобы не дёргать кадры во время анимации
            if (fabs(g_zoomTarget - g_zoom) < 0.001 && GetTickCount() - g_lastWallCap >= 1500) {
                g_lastWallCap = GetTickCount();
                LoadWallpaper();
                InvalidateRect(g_ov, nullptr, FALSE);
            }
            // перерисовываем обзор только при изменениях (зум/панорама/перетаскивание)
            bool ovChanged = (g_zoom != g_ovLastZoom || g_axCX != g_ovLastAxCX ||
                              g_axCY != g_ovLastAxCY || g_axWX != g_ovLastAxWX ||
                              g_axWY != g_ovLastAxWY || g_ovDragHwnd != nullptr);
            if (ovChanged) {
                g_ovLastZoom = g_zoom; g_ovLastAxCX = g_axCX; g_ovLastAxCY = g_axCY;
                g_ovLastAxWX = g_axWX; g_ovLastAxWY = g_axWY;
                UpdateOverview();
            }
            return;
        }
    }

    if (!g_dragging) {
        double k = 1.0 - pow(1.0 - EASE, dt * 60.0);
        double dx = g_targetX - g_camX, dy = g_targetY - g_camY;
        if (fabs(dx) > 0.5 || fabs(dy) > 0.5) {
            g_camX += dx * k; g_camY += dy * k;
            if (fabs(g_targetX - g_camX) < 0.5) g_camX = g_targetX;
            if (fabs(g_targetY - g_camY) < 0.5) g_camY = g_targetY;
        }
    }

    bool camChanged = (g_camX != g_lastCamX || g_camY != g_lastCamY);
    if (camChanged) {
        g_lastCamX = g_camX; g_lastCamY = g_camY;
        RepositionAll();        // реальные окна — каждый кадр (вплоть до частоты монитора)
        DWORD now = GetTickCount();
        if (g_hudVisible && now - g_lastHudTick >= 16) {   // миникарта ~60 Гц достаточно
            g_lastHudTick = now;
            RECT rc; GetClientRect(g_hud, &rc);
            ComputeXform(rc); UpdateThumbs(rc);
            InvalidateRect(g_hud, nullptr, FALSE);
        }
    } else if (!g_dragging) {
        DWORD now = GetTickCount();
        if (now - g_lastSyncTick >= 300) {                 // периодическая синхронизация
            g_lastSyncTick = now;
            SyncWorldFromScreen();
            AddNewWindows();
            // Перепроверка припаркованных за кадром окон: приложение могло само вернуть
            // окно в видимую зону (типично сразу после открытия). Тогда снимаем парковку,
            // и RepositionAll снова вытолкнет его на истинную мир-позицию за кадром.
            {
                const LONG vL = g_vsX, vT = g_vsY, vR = g_vsX + g_vsW, vB = g_vsY + g_vsH;
                bool repark = false;
                for (auto& w : g_wins) {
                    if (!w.parkedOff || !IsWindow(w.hwnd)) continue;
                    RECT r;
                    if (GetWindowRect(w.hwnd, &r) &&
                        r.right > vL && r.left < vR && r.bottom > vT && r.top < vB) {
                        w.parkedOff = false; repark = true;
                    }
                }
                if (repark) RepositionAll();
            }
            if (g_hudVisible) {
                RECT rc; GetClientRect(g_hud, &rc);
                ComputeXform(rc); UpdateThumbs(rc);
                InvalidateRect(g_hud, nullptr, FALSE);
            }
        }
    }
}

// ---------- main ----------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    EnableDpiAwareness();
    UpdateVirtualScreen();
    DisableSnap();          // на бесконечном столе краёв нет — выключаем Aero Snap
    HideDesktopIcons();     // убираем ярлыки рабочего стола на время работы

    Gdiplus::GdiplusStartupInput gsi;
    Gdiplus::GdiplusStartup(&g_gdiToken, &gsi, nullptr);
    LoadWallpaper();   // обои пользователя для фона обзора

    // Класс и окно полноэкранного обзора (зум), пока скрыто
    WNDCLASSW oc = {};
    oc.lpfnWndProc = OvProc;
    oc.hInstance = hInst;
    oc.hCursor = LoadCursor(nullptr, IDC_HAND);
    oc.lpszClassName = L"InfiniteDesktopOverview";
    RegisterClassW(&oc);
    g_ov = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        oc.lpszClassName, L"", WS_POPUP,
        g_vsX, g_vsY, g_vsW, g_vsH,
        nullptr, nullptr, hInst, nullptr);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = HudProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_HAND);
    wc.lpszClassName = L"InfiniteDesktopHUD";
    RegisterClassW(&wc);

    g_hud = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        wc.lpszClassName, L"InfiniteDesktop",
        WS_POPUP | WS_VISIBLE,
        0, 0, MAP_W, MAP_H,
        nullptr, nullptr, hInst, nullptr);
    SetLayeredWindowAttributes(g_hud, 0, HUD_ALPHA, LWA_ALPHA);  // полупрозрачность
    ApplyWin11Style(g_hud);
    LoadUiCfg();   // сохранённый угол миникарты
    PlaceHud();    // разместить в выбранном углу

    // Окно настроек горячих клавиш (скрыто; открывается по шестерёнке)
    WNDCLASSW stc = {};
    stc.lpfnWndProc = SettingsProc;
    stc.hInstance = hInst;
    stc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    stc.lpszClassName = L"InfiniteDesktopSettings";
    RegisterClassW(&stc);
    g_settings = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        stc.lpszClassName, L"Горячие клавиши",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        120, 120, SET_W, 480, nullptr, nullptr, hInst, nullptr);
    SetLayeredWindowAttributes(g_settings, 0, SETTINGS_ALPHA, LWA_ALPHA);  // полупрозрачность
    ApplyWin11Style(g_settings);

    InitDefaultKeys();      // значения по умолчанию
    LoadHotkeys();          // переопределение из файла (если есть)
    RegisterAllHotkeys();   // зарегистрировать все

    RebuildWindowList();
    LoadLayout();          // прошлая раскладка (если есть)
    ArrangeOnStartup();    // вернуть сохранённые / разложить новые без наложения
    RepositionAll();       // применить расстановку
    {
        RECT rc; GetClientRect(g_hud, &rc);
        ComputeXform(rc);
        UpdateThumbs(rc);
    }
    // Глобальный hook СКМ/колеса — на отдельном потоке (устойчив к подвисаниям)
    g_hookThread = CreateThread(nullptr, 0, HookThreadProc, hInst, 0, &g_hookThreadId);

    // Подъезд камеры к активируемому окну (Win+Tab / Alt+Tab / панель задач)
    g_winEventHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                     nullptr, WinEventProc, 0, 0,
                                     WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    // Высокочастотный цикл кадров: панорама плавная на мониторах >60 Гц.
    // Когда есть движение — тикаем ~200 Гц; в покое спим и тикаем ~33 Гц (для синхронизации).
    timeBeginPeriod(1);
    // Частота кадров = частота монитора: двигать окна чаще бессмысленно (дисплей
    // не покажет) и при синхронном SetWindowPos лишь грузит CPU.
    DEVMODEW dm = {}; dm.dmSize = sizeof(dm);
    double hz = 120.0;
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        hz = (double)dm.dmDisplayFrequency;
    if (hz < 60.0) hz = 60.0;
    if (hz > 240.0) hz = 240.0;
    LARGE_INTEGER qf, prev;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&prev);
    const double TARGET = 1.0 / hz;
    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - prev.QuadPart) / (double)qf.QuadPart;
        bool active = g_dragging || g_overview ||
                      fabs(g_targetX - g_camX) > 0.5 || fabs(g_targetY - g_camY) > 0.5 ||
                      fabs(g_zoomTarget - g_zoom) > 0.001;
        double step = active ? TARGET : 0.030;
        DWORD waitms;
        if (dt >= step) { prev = now; FrameTick(dt); waitms = (DWORD)(step * 1000.0); }
        else            { waitms = (DWORD)((step - dt) * 1000.0 + 0.5); }
        MsgWaitForMultipleObjectsEx(0, nullptr, waitms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    timeEndPeriod(1);

    if (g_winEventHook) UnhookWinEvent(g_winEventHook);
    if (g_hookThreadId) PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
    if (g_hookThread) {
        WaitForSingleObject(g_hookThread, 1000);
        CloseHandle(g_hookThread);
    }
    RestoreSnap();          // вернуть прежнее состояние Aero Snap (и привязки)
    RestoreDesktopIcons();  // вернуть ярлыки рабочего стола

    // сохранить раскладку и собрать окна обратно на видимый рабочий стол
    HideOverview();
    SyncWorldFromScreen();   // подхватить актуальные позиции
    SaveLayout();            // запомнить мировые позиции на следующий запуск
    GatherToDesktop();       // окна возвращаются на видимый стол

    FreeMapGdi();
    UnregisterAllThumbs();
    UnregisterAllOvThumbs();
    if (g_ovMemDC) DeleteDC(g_ovMemDC);
    if (g_ovBmp)   DeleteObject(g_ovBmp);
    if (g_wallDC)  DeleteDC(g_wallDC);
    if (g_wallBmp) DeleteObject(g_wallBmp);
    if (g_blackDC) DeleteDC(g_blackDC);
    if (g_blackBmp) DeleteObject(g_blackBmp);
    if (g_gdiToken) Gdiplus::GdiplusShutdown(g_gdiToken);
    if (g_settings) DestroyWindow(g_settings);
    if (g_ov)      DestroyWindow(g_ov);
    return 0;
}
