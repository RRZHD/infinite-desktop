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
// Возвращает true, если кадр обзора уже показан с ожиданием vblank (EndDraw) —
// тогда цикл не должен ждать композицию ещё раз через DwmFlush.
static bool FrameTick(AppState& app, double dt) {
    bool dragging = app.dragging.load(std::memory_order_acquire);
    long dragDx = app.dragDeltaX.exchange(0, std::memory_order_acq_rel);
    long dragDy = app.dragDeltaY.exchange(0, std::memory_order_acq_rel);
    if (dragDx || dragDy) {
        if (app.overview) {
            // В обзоре СКМ двигает сам зум-вид (якорь).
            app.axCX += dragDx;
            app.axCY += dragDy;
        } else {
            // «Рука»: окна следуют за курсором, значит камера идёт в обратную сторону.
            app.camX -= dragDx;
            app.camY -= dragDy;
            app.targetX = app.camX;
            app.targetY = app.camY;
        }
    }

    double kz = 1.0 - pow(1.0 - ZOOM_EASE, dt * 60.0);   // эквивалент при 60 Гц
    if (fabs(app.zoomTarget - app.zoom) > 0.001) app.zoom += (app.zoomTarget - app.zoom) * kz;
    else app.zoom = app.zoomTarget;
    bool wantOv = (app.zoom < 1.0) || (app.zoomTarget < 1.0);
    if (wantOv && !app.overview) ShowOverview(app);
    if (app.overview) {
        // Скрываем обзор ТОЛЬКО после кадра на масштабе ровно 1.0 — тогда превью
        // точно совпадает с реальным окном (размер и позиция), и переход бесшовный.
        if (!wantOv && app.ovLastZoom >= 1.0) {
            CommitZoomCam(app);
            RepositionAll(app);   // окна на местах ДО скрытия обзора
            app.lastCamX = app.camX; app.lastCamY = app.camY;
            HideOverview(app);
            // дальше — обычная логика (окна уже расставлены)
        } else {
            // периодически проверяем обои (слайдшоу/смена в реальном времени), когда зум
            // устаканился. Проверка — дешёвый stat файла; полная перекодировка (GDI+ decode
            // + бикубический ресемплинг под все мониторы, десятки мс) — только при смене.
            // Раньше перекодировка шла безусловно каждые 1.5 с — заметная заминка в обзоре.
            if (fabs(app.zoomTarget - app.zoom) < 0.001 && GetTickCount() - app.lastWallCap >= 1500) {
                app.lastWallCap = GetTickCount();
                if (WallpaperFileChanged(app)) {
                    LoadWallpaper(app);
                    app.ovLastZoom = -1;          // форсировать перерисовку в этом же кадре
                }
            }
            // Окна, открытые кликами из обзора (диалоги, новые окна), подхватываем и здесь —
            // обычная фоновая синхронизация в обзоре не выполняется.
            if (GetTickCount() - app.lastSyncTick >= 300) { app.lastSyncTick = GetTickCount(); AddNewWindows(app); }
            // перерисовываем обзор только при изменениях (зум/панорама/перетаскивание)
            bool ovChanged = (app.zoom != app.ovLastZoom || app.axCX != app.ovLastAxCX ||
                              app.axCY != app.ovLastAxCY || app.axWX != app.ovLastAxWX ||
                              app.axWY != app.ovLastAxWY || app.ovDragHwnd != nullptr);
            if (ovChanged) {
                app.ovLastZoom = app.zoom; app.ovLastAxCX = app.axCX; app.ovLastAxCY = app.axCY;
                app.ovLastAxWX = app.axWX; app.ovLastAxWY = app.axWY;
                UpdateOverview(app);            // превью (DWM) — на новые места
                RenderOverview(app, app.ov);    // фон и тени — сразу, в тот же кадр; ждёт vblank
                return true;
            }
            return false;
        }
    }

    if (!dragging) {
        double k = 1.0 - pow(1.0 - EASE, dt * 60.0);
        double dx = app.targetX - app.camX, dy = app.targetY - app.camY;
        if (fabs(dx) > 0.5 || fabs(dy) > 0.5) {
            app.camX += dx * k; app.camY += dy * k;
            if (fabs(app.targetX - app.camX) < 0.5) app.camX = app.targetX;
            if (fabs(app.targetY - app.camY) < 0.5) app.camY = app.targetY;
        }
    }

    // Пользователь тащит/ресайзит окно мышью (EVENT_SYSTEM_MOVESIZESTART..END). Камера
    // при этом не меняется, и раньше миникарта подхватывала новую позицию только фоновой
    // синхронизацией раз в 300 мс — отсюда рывки превью. Теперь во время перетаскивания
    // мир этого одного окна читаем каждый кадр, и карта следует за ним плавно.
    if (app.moveSizeHwnd) {
        if (!IsWindow(app.moveSizeHwnd)) {
            app.moveSizeHwnd = nullptr;          // окно закрыли посреди перетаскивания — END не придёт
        } else {
            LONG cx = (LONG)llround(app.camX), cy = (LONG)llround(app.camY);
            RECT r;
            for (auto& w : app.wins) if (w.hwnd == app.moveSizeHwnd && GetWindowRect(w.hwnd, &r)) {
                w.world = { r.left + cx, r.top + cy, r.right + cx, r.bottom + cy };
                break;
            }
            DWORD now = GetTickCount();
            if (app.hudVisible && now - app.lastHudTick >= 16) {   // миникарта ~60 Гц достаточно
                app.lastHudTick = now;
                RECT rc; GetClientRect(app.hud, &rc);
                ComputeXform(app, rc); UpdateThumbs(app, rc);
                InvalidateRect(app.hud, nullptr, FALSE);
            }
        }
    }

    bool camChanged = (app.camX != app.lastCamX || app.camY != app.lastCamY);
    if (camChanged) {
        app.lastCamX = app.camX; app.lastCamY = app.camY;
        RepositionAll(app);        // реальные окна — каждый кадр (вплоть до частоты монитора)
        DWORD now = GetTickCount();
        if (app.hudVisible && now - app.lastHudTick >= 16) {   // миникарта ~60 Гц достаточно
            app.lastHudTick = now;
            RECT rc; GetClientRect(app.hud, &rc);
            ComputeXform(app, rc); UpdateThumbs(app, rc);
            InvalidateRect(app.hud, nullptr, FALSE);
        }
    } else if (!dragging) {
        DWORD now = GetTickCount();
        if (now - app.lastSyncTick >= 300) {                 // периодическая синхронизация
            app.lastSyncTick = now;
            SyncWorldFromScreen(app);
            AddNewWindows(app);
            // Перепроверка припаркованных за кадром окон: приложение могло само сдвинуть
            // окно с места парковки (типично сразу после открытия). Сравниваем с точкой,
            // куда его поставили мы: припаркованное окно перекрывает край экрана на 1 px,
            // поэтому критерий «пересекает вьюпорт» здесь не годится — срабатывал бы всегда.
            // Сдвинуто — снимаем парковку, и RepositionAll вернёт его к краю.
            {
                bool repark = false;
                for (auto& w : app.wins) {
                    if (!w.parkedOff || !IsWindow(w.hwnd)) continue;
                    RECT r;
                    if (GetWindowRect(w.hwnd, &r) &&
                        (r.left != w.parkedPos.x || r.top != w.parkedPos.y)) {
                        w.parkedOff = false; repark = true;
                    }
                }
                if (repark) RepositionAll(app);
            }
            if (app.hudVisible) {
                RECT rc; GetClientRect(app.hud, &rc);
                ComputeXform(app, rc); UpdateThumbs(app, rc);
                InvalidateRect(app.hud, nullptr, FALSE);
            }
        }
    }
    return false;
}

// ---------- main ----------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // COM нужен WIC (декод обоев для Direct2D). Объявлен ДО AppState: локальные объекты
    // уничтожаются в обратном порядке, так что COM-объекты состояния освободятся
    // раньше CoUninitialize.
    struct ScopedCom {
        ScopedCom()  { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
        ~ScopedCom() { CoUninitialize(); }
    } com;
    AppState app;          // единственный экземпляр состояния
    g_app = &app;          // для Win32-колбэков
    EnableDpiAwareness();
    UpdateVirtualScreen(app);
    DisableSnap(app);          // на бесконечном столе краёв нет — выключаем Aero Snap
    HideDesktopIcons(app);     // убираем ярлыки рабочего стола на время работы

    InitGraphics(app);    // фабрики Direct2D / DirectWrite / WIC (один раз на процесс)
    LoadWallpaper(app);   // обои пользователя для фона обзора

    // Класс и окно полноэкранного обзора (зум), пока скрыто
    WNDCLASSW oc = {};
    oc.lpfnWndProc = OvProc;
    oc.hInstance = hInst;
    oc.hCursor = LoadCursor(nullptr, IDC_HAND);
    oc.lpszClassName = L"InfiniteDesktopOverview";
    RegisterClassW(&oc);
    app.ov = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        oc.lpszClassName, L"", WS_POPUP,
        app.vsX, app.vsY, app.vsW, app.vsH,
        nullptr, nullptr, hInst, nullptr);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = HudProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_HAND);
    wc.lpszClassName = L"InfiniteDesktopHUD";
    RegisterClassW(&wc);

    app.hud = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        wc.lpszClassName, L"InfiniteDesktop",
        WS_POPUP | WS_VISIBLE,
        0, 0, MAP_W, MAP_H,
        nullptr, nullptr, hInst, nullptr);
    SetLayeredWindowAttributes(app.hud, 0, HUD_ALPHA, LWA_ALPHA);  // полупрозрачность
    ApplyWin11Style(app.hud);
    LoadUiCfg(app);   // сохранённый угол миникарты
    PlaceHud(app);    // разместить в выбранном углу

    // Окно настроек горячих клавиш (скрыто; открывается по шестерёнке)
    WNDCLASSW stc = {};
    stc.lpfnWndProc = SettingsProc;
    stc.hInstance = hInst;
    stc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    stc.lpszClassName = L"InfiniteDesktopSettings";
    RegisterClassW(&stc);
    app.settings = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        stc.lpszClassName, L"Горячие клавиши",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        120, 120, SET_W, 480, nullptr, nullptr, hInst, nullptr);
    SetLayeredWindowAttributes(app.settings, 0, SETTINGS_ALPHA, LWA_ALPHA);  // полупрозрачность
    ApplyWin11Style(app.settings);

    InitDefaultKeys(app);      // значения по умолчанию
    LoadHotkeys(app);          // переопределение из файла (если есть)
    RegisterAllHotkeys(app);   // зарегистрировать все

    RebuildWindowList(app);
    LoadLayout(app);          // прошлая раскладка (если есть)
    LoadMarks(app);           // закладки камеры (если есть)
    ArrangeOnStartup(app);    // вернуть сохранённые / разложить новые без наложения
    RepositionAll(app);       // применить расстановку
    {
        RECT rc; GetClientRect(app.hud, &rc);
        ComputeXform(app, rc);
        UpdateThumbs(app, rc);
    }
    // Глобальный hook СКМ/колеса — на отдельном потоке (устойчив к подвисаниям)
    app.hookThread.reset(CreateThread(nullptr, 0, HookThreadProc, hInst, 0, &app.hookThreadId));

    // Подъезд камеры к активируемому окну (Win+Tab / Alt+Tab / панель задач) и
    // отслеживание перетаскивания окон мышью (MOVESIZESTART..END) для плавной миникарты.
    // Диапазон FOREGROUND..MOVESIZEEND захватывает и события меню/захвата — их
    // отфильтровывает WinEventProc.
    app.winEventHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MOVESIZEEND,
                                     nullptr, WinEventProc, 0, 0,
                                     WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    // Цикл кадров. Во время движения синхронизируемся с композитором DWM: DwmFlush
    // блокирует до следующей композиции (= vblank монитора), так что пакет перемещений
    // окон ложится ровно в кадр DWM, а не «когда успели». Это и убирает шлейф, и даёт
    // честную частоту монитора: прежний sleep-цикл ждал полный период ПОСЛЕ работы кадра,
    // поэтому реальная частота была ниже частоты монитора и плавала в зависимости от того,
    // сколько заняли SetWindowPos. В покое DwmFlush не зовём (просыпались бы каждый
    // vblank впустую) — спим ~30 мс и тикаем для фоновой синхронизации.
    timeBeginPeriod(1);
    BOOL dwmOk = FALSE;
    DwmIsCompositionEnabled(&dwmOk);             // на Win8+ всегда TRUE; fallback на всякий случай
    // Частота монитора — только для запасного пути без DWM.
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
    const double IDLE   = 0.030;
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
        bool active = app.dragging.load(std::memory_order_acquire) || app.overview ||
                      app.moveSizeHwnd != nullptr ||          // тащат окно — карта следит покадрово
                      fabs(app.targetX - app.camX) > 0.5 || fabs(app.targetY - app.camY) > 0.5 ||
                      fabs(app.zoomTarget - app.zoom) > 0.001;
        if (active) {
            prev = now;
            // Кадр обзора уже показан через EndDraw с ожиданием vblank — второй раз
            // (DwmFlush) не ждём, иначе получали бы каждый второй кадр.
            bool presented = FrameTick(app, dt);
            if (presented) {
                // пейсинг задал EndDraw
            } else if (dwmOk) {
                DwmFlush();                      // до следующей композиции DWM (vblank)
            } else {
                // Запасной путь: ждём до абсолютного дедлайна now + TARGET с учётом
                // уже потраченного на кадр времени, а не «TARGET после кадра».
                LARGE_INTEGER t; QueryPerformanceCounter(&t);
                double left = TARGET - (double)(t.QuadPart - now.QuadPart) / (double)qf.QuadPart;
                if (left > 0.0005)
                    MsgWaitForMultipleObjectsEx(0, nullptr, (DWORD)(left * 1000.0), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }
        } else {
            if (dt >= IDLE) { prev = now; FrameTick(app, dt); dt = 0; }
            DWORD waitms = (DWORD)((IDLE - dt) * 1000.0 + 0.5);
            MsgWaitForMultipleObjectsEx(0, nullptr, waitms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    }
    timeEndPeriod(1);

    if (app.winEventHook) UnhookWinEvent(app.winEventHook);
    if (app.hookThreadId) PostThreadMessageW(app.hookThreadId, WM_QUIT, 0, 0);
    if (app.hookThread) {
        ScopedHandle ht(app.hookThread.release());
        WaitForSingleObject(ht, 1000);
    }
    RestoreSnap(app);          // вернуть прежнее состояние Aero Snap (и привязки)
    RestoreDesktopIcons(app);  // вернуть ярлыки рабочего стола

    // сохранить раскладку и собрать окна обратно на видимый рабочий стол
    HideOverview(app);
    SyncWorldFromScreen(app);   // подхватить актуальные позиции
    SaveLayout(app);            // запомнить мировые позиции на следующий запуск
    GatherToDesktop(app);       // окна возвращаются на видимый стол

    if (app.settings) DestroyWindow(app.settings);
    if (app.ov)      DestroyWindow(app.ov);
    return 0;
}

