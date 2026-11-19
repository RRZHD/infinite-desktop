// InfiniteDesktop - бесконечный зум-канвас рабочего стола для Windows 10/11.
//
// Идея: виртуальный "мир" больше экрана. Камера смотрит в участок мира.
//  - Панорама (1:1): все реальные окна физически переезжают через SetWindowPos,
//    оставаясь полноразмерными и кликабельными => ощущение бесконечного стола.
//  - Обзор (зум-аут): полноэкранный вид из ЖИВЫХ превью окон (DWM-thumbnails); окна
//    можно переставлять и нажимать прямо в нём.
//  - Миникарта: все окна и рамка текущего вьюпорта; клик по карте = перелёт камеры.
//
// Поддержка нескольких мониторов: сцена строится в координатах виртуального
// экрана (SM_*VIRTUALSCREEN), окна перемещаются по всему рабочему пространству.
//
// Ограничение ОС: живое окно чужого процесса нельзя интерактивно масштабировать,
// поэтому зум — это обзор из превью; клик в нём доставляется в настоящее окно обходным
// путём (см. ClickIntoWindow). Горячие клавиши и управление описаны в README.


#include "src/id_common.h"   // типы, inline-глобалы, прототипы (модули — отдельные .cpp)

// ---------- Кадр анимации и точка входа ----------

// Троттлинг обновления миникарты: ~60 Гц достаточно, чаще — только гонять DWM.
static void RefreshHudThrottled(AppState& app) {
    if (!app.hudVisible) return;
    DWORD now = GetTickCount();
    if (now - app.lastHudTick < 16) return;
    app.lastHudTick = now;
    RefreshHud(app);
}

// Кадр в открытом обзоре: проверка обоев, подхват новых окон, перерисовка только при
// изменениях зума/панорамы/перетаскивания. Возвращает true, если кадр уже показан с
// ожиданием vblank (EndDraw) — цикл тогда не должен ждать композицию ещё раз через DwmFlush.
static bool OverviewFrame(AppState& app) {
    // периодически проверяем обои (слайдшоу/смена в реальном времени), когда зум
    // устаканился. Проверка — дешёвый stat файла; полная перекодировка (декод WIC и
    // ресемплинг, десятки мс) — только при смене.
    if (fabs(app.zoomTarget - app.zoom) < 0.001 && GetTickCount() - app.lastWallCap >= 1500) {
        app.lastWallCap = GetTickCount();
        if (WallpaperFileChanged(app)) {
            LoadWallpaper(app);
            app.ovLastZoom = -1;          // форсировать перерисовку в этом же кадре
        }
    }
    // Окна, открытые кликами из обзора (диалоги, новые окна), подхватываем и здесь —
    // обычная фоновая синхронизация в обзоре не выполняется. Закрытые — убираем, иначе
    // от них до выхода из зума оставалась бы тень.
    if (GetTickCount() - app.lastSyncTick >= 300) {
        app.lastSyncTick = GetTickCount();
        AddNewWindows(app);
        if (PruneDeadWindows(app)) app.ovLastZoom = -1;   // перерисовать без ушедших
        RefreshShadowStates(app);   // активным стало другое окно (клик внутрь) — тени под стать
    }
    // перерисовываем обзор только при изменениях (зум/панорама/перетаскивание)
    bool ovChanged = (app.zoom != app.ovLastZoom || app.axCX != app.ovLastAxCX ||
                      app.axCY != app.ovLastAxCY || app.axWX != app.ovLastAxWX ||
                      app.axWY != app.ovLastAxWY || app.ovDragHwnd != nullptr ||
                      app.barP != app.ovLastBarP);
    if (ovChanged) {
        app.ovLastZoom = app.zoom; app.ovLastAxCX = app.axCX; app.ovLastAxCY = app.axCY;
        app.ovLastAxWX = app.axWX; app.ovLastAxWY = app.axWY; app.ovLastBarP = app.barP;
        UpdateOverview(app);            // превью (DWM) — на новые места
        RenderOverview(app, app.ov);    // фон и тени — сразу, в тот же кадр; ждёт vblank
        return true;
    }
    return false;
}

// Перепроверка припаркованных за кадром окон: приложение могло само сдвинуть окно
// с места парковки (типично сразу после открытия). Сравниваем с точкой, куда его
// поставили мы: припаркованное окно перекрывает край экрана на 1 px, поэтому
// критерий «пересекает вьюпорт» здесь не годится — срабатывал бы всегда.
static bool RecheckParkedWindows(AppState& app) {
    bool repark = false;
    for (auto& w : app.wins) {
        if (!w.parkedOff || !IsWindow(w.hwnd)) continue;
        RECT r;
        if (GetWindowRect(w.hwnd, &r) && (r.left != w.parkedPos.x || r.top != w.parkedPos.y)) {
            w.parkedOff = false; repark = true;
        }
    }
    return repark;
}

static bool FrameTick(AppState& app, double dt) {
    ApplyWheelInput(app);   // колесо и тачпад, накопленные hook-потоком
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
    // Панель задач в обзоре: уходит за край, когда зум начат, и возвращается, когда он
    // идёт обратно к 1:1. Движение — по времени, а не по масштабу: один щелчок колеса не
    // должен оставлять панель наполовину съехавшей.
    if (app.ovBars.empty()) {
        app.barP = 0;             // панелей нет (или обзор ещё не показан) — ждать нечего
    } else {
        double goal = (app.zoomTarget < 1.0) ? 1.0 : 0.0;
        double kb = 1.0 - pow(1.0 - BAR_EASE, dt * 60.0);
        if (fabs(goal - app.barP) > 0.003) app.barP += (goal - app.barP) * kb;
        else app.barP = goal;
    }
    if (wantOv && !app.overview) {
        EndGlide(app);            // зум начался посреди панорамы: сначала окна на места
        ShowOverview(app);
    }
    if (app.overview) {
        // Скрываем обзор ТОЛЬКО после кадра на масштабе ровно 1.0 — тогда превью
        // точно совпадает с реальным окном (размер и позиция), и переход бесшовный.
        // …и после того, как панель задач вернулась на место: под её превью — она сама.
        if (!wantOv && app.ovLastZoom >= 1.0 && app.barP <= 0.0 && app.ovLastBarP <= 0.0) {
            CommitZoomCam(app);
            RepositionAll(app);   // окна на местах ДО скрытия обзора
            app.lastCamX = app.camX; app.lastCamY = app.camY;
            HideOverview(app);
            // дальше — обычная логика (окна уже расставлены)
        } else {
            return OverviewFrame(app);
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

    // Плавная панорама: пока камера движется (тащат СКМ или идёт перелёт), кадры рисуются
    // из живых превью окон, а настоящие окна догоняет рабочий поток (см. id_glide.cpp).
    // Движение закончилось — окна встают на места, слой превью убирается.
    bool moving = dragging || fabs(app.targetX - app.camX) > 0.5 || fabs(app.targetY - app.camY) > 0.5;
    if (!app.glide && moving && app.glideEnabled && !app.glideBroken && !BeginGlide(app))
        app.glideBroken = true;   // слой не создаётся — дальше двигаем окна напрямую, как раньше
    if (app.glide) {
        app.lastCamX = app.camX; app.lastCamY = app.camY;
        if (!moving) {
            EndGlide(app);
            if (app.hudVisible) RefreshHud(app);
            return false;
        }
        GlideFrame(app);
        RefreshHudThrottled(app);
        return true;              // кадр показан с ожиданием vblank
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
            RefreshHudThrottled(app);
        }
    }

    bool camChanged = (app.camX != app.lastCamX || app.camY != app.lastCamY);
    if (camChanged) {
        app.lastCamX = app.camX; app.lastCamY = app.camY;
        RepositionAll(app);        // реальные окна — каждый кадр (вплоть до частоты монитора)
        RefreshHudThrottled(app);
    } else if (!dragging) {
        DWORD now = GetTickCount();
        if (now - app.lastSyncTick >= 300) {                 // периодическая синхронизация
            app.lastSyncTick = now;
            SyncWorldFromScreen(app);
            AddNewWindows(app);
            if (RecheckParkedWindows(app)) RepositionAll(app);
            HideDesktopIcons(app);      // иконки стола — заново после перезапуска Explorer
            if (app.sessionDirty) SaveSession(app);
            AutoSaveLayout(app);        // раскладка — на диск, когда изменилась и устоялась
            if (app.hudVisible) RefreshHud(app);
        }
    }
    return false;
}

// Регистрация класса окна приложения (окна отличаются только процедурой, именем и
// курсором) — общий код для обзора, миникарты и настроек.
static void RegClass(HINSTANCE hi, const wchar_t* name, WNDPROC proc, LPCWSTR cursorId) {
    WNDCLASSW c = {};
    c.lpfnWndProc = proc;
    c.hInstance = hi;
    c.hCursor = LoadCursorW(nullptr, cursorId);
    c.lpszClassName = name;
    RegisterClassW(&c);
}

// Аварийное завершение: вернуть системе то, что меняли (Snap, иконки стола), и собрать
// окна на видимый стол — иначе они остались бы у края экрана, а чинить пришлось бы руками.
// Состояние процесса после сбоя ненадёжно, поэтому только простые вызовы и один раз; если
// не выйдет и это, следующий запуск разберётся по файлу сеанса (BeginSession).
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS*) {
    static LONG entered = 0;
    if (InterlockedExchange(&entered, 1) == 0 && g_app) {
        AppState& app = *g_app;
        RestoreSnap(app);
        RestoreDesktopIcons(app);
        GatherToDesktop(app);
        EndSession();
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------- main ----------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // Один экземпляр: два процесса двигали бы одни и те же окна каждый своей камерой и
    // по очереди «возвращали» бы Snap и иконки не в то состояние. Повторный запуск просит
    // работающий экземпляр показать настройки — так видно, что программа уже запущена.
    ScopedHandle instance(CreateMutexW(nullptr, FALSE, L"Local\\InfiniteDesktop.SingleInstance"));
    if (instance && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND hud = FindWindowW(L"InfiniteDesktopHUD", nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(hud, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(hud, WM_SHOWSETTINGS, 0, 0);
        }
        return 0;
    }
    // COM нужен WIC (декод обоев для Direct2D). Объявлен ДО AppState: локальные объекты
    // уничтожаются в обратном порядке, так что COM-объекты состояния освободятся
    // раньше CoUninitialize.
    struct ScopedCom {
        ScopedCom()  { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
        ~ScopedCom() { CoUninitialize(); }
    } com;
    AppState app;          // единственный экземпляр состояния
    g_app = &app;          // для Win32-колбэков
    InitLanguage();        // язык интерфейса — как в Windows (до создания окон и меню)
    EnableDpiAwareness();
    UpdateVirtualScreen(app);
    DisableSnap(app);          // на бесконечном столе краёв нет — выключаем Aero Snap
    HideDesktopIcons(app);     // убираем ярлыки рабочего стола на время работы
    BeginSession(app);         // прошлый сеанс оборвался? — помним, что вернуть при выходе
    SetUnhandledExceptionFilter(CrashFilter);

    InitGraphics(app);    // фабрики Direct2D / DirectWrite / WIC (один раз на процесс)
    InitUi(app);          // шрифты DirectWrite и тема (цвета Windows)
    LoadWallpaper(app);   // обои пользователя для фона обзора

    // Класс и окно полноэкранного обзора (зум), пока скрыто
    RegClass(hInst, L"InfiniteDesktopOverview", OvProc, IDC_HAND);
    app.ov = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        L"InfiniteDesktopOverview", L"", WS_POPUP,
        app.vsX, app.vsY, app.vsW, app.vsH,
        nullptr, nullptr, hInst, nullptr);
    CreateOverviewBackdrop(app, hInst);   // подложка живых обоев + прозрачность окна обзора

    RegClass(hInst, L"InfiniteDesktopHUD", HudProc, IDC_HAND);
    app.hud = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        L"InfiniteDesktopHUD", L"InfiniteDesktop",
        WS_POPUP | WS_VISIBLE,
        0, 0, MAP_W, MAP_H,
        nullptr, nullptr, hInst, nullptr);
    SetLayeredWindowAttributes(app.hud, 0, HUD_ALPHA, LWA_ALPHA);  // полупрозрачность
    ApplyWin11Style(app, app.hud);
    LoadUiCfg(app);   // сохранённый угол миникарты
    PlaceHud(app);    // разместить в выбранном углу
    // Иконка в трее: настройки, переключатели, выход. Переживает перезапуск Explorer.
    app.taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    AddTrayIcon(app);

    // Окно настроек горячих клавиш (скрыто; открывается по шестерёнке)
    RegClass(hInst, L"InfiniteDesktopSettings", SettingsProc, IDC_ARROW);
    // Непрозрачное: рисуется Direct2D (сглаженные скругления, кольцо фокуса).
    app.settings = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"InfiniteDesktopSettings", Tr(S_SETTINGS_TITLE),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        120, 120, SET_W, 480, nullptr, nullptr, hInst, nullptr);
    ApplyWin11Style(app, app.settings);

    InitDefaultKeys(app);      // значения по умолчанию
    LoadHotkeys(app);          // переопределение из файла (если есть)
    RegisterAllHotkeys(app);   // зарегистрировать все

    RebuildWindowList(app);
    LoadLayout(app);          // прошлая раскладка (если есть)
    LoadMarks(app);           // закладки камеры (если есть)
    ArrangeOnStartup(app);    // вернуть сохранённые / разложить новые без наложения
    RepositionAll(app);       // применить расстановку
    RefreshHud(app);          // миникарта: пересчёт отображения и превью до первого кадра
    // Прогрев: устройство Direct2D и обои на видеокарте создаются при первом кадре окна
    // обзора и стоят ~0,2 с. Рисуем один кадр в скрытое окно сейчас, чтобы эта заминка не
    // пришлась на первое же движение холста.
    if (app.ov) RenderOverview(app, app.ov);
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
    // vblank впустую) — спим до следующего сообщения, но не дольше IDLE, и тикаем для
    // фоновой синхронизации. Всё, что требует кадра немедленно, приходит сообщением:
    // хоткеи, события окон, а hook-поток мыши будит цикл сам (WakeMain).
    BOOL dwmOk = FALSE;
    DwmIsCompositionEnabled(&dwmOk);             // на Win8+ всегда TRUE; fallback на всякий случай
    // Точный системный таймер нужен только запасному пути без DWM: держать его всё время
    // работы — лишний расход энергии для программы, которая живёт в трее постоянно.
    if (!dwmOk) timeBeginPeriod(1);
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
    app.framePeriod = TARGET;
    const double IDLE   = 0.100;
    double workAvg = 0;                 // сглаженное время работы кадра (для ровного ритма)
    int pace = 1;                       // показывать кадр раз в pace периодов монитора
    LARGE_INTEGER lastShown = prev;
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
        bool active = app.dragging.load(std::memory_order_acquire) || app.overview || app.glide ||
                      app.moveSizeHwnd != nullptr ||          // тащат окно — карта следит покадрово
                      fabs(app.targetX - app.camX) > 0.5 || fabs(app.targetY - app.camY) > 0.5 ||
                      fabs(app.zoomTarget - app.zoom) > 0.001 || WheelInputPending(app);
        if (active) {
            prev = now;
            // Первый кадр после покоя: dt — это время сна, а не кадра. Без ограничения
            // сглаживание камеры и зума проскочило бы почти весь путь за один шаг.
            if (dt > 0.05) dt = 0.05;
            // Кадр обзора уже показан через EndDraw с ожиданием vblank — второй раз
            // (DwmFlush) не ждём, иначе получали бы каждый второй кадр.
            bool presented = FrameTick(app, dt);
            if (presented) {
                // пейсинг задал EndDraw
            } else if (dwmOk) {
                // Ровный ритм. Если работа кадра (перемещение настоящих окон) не укладывается
                // в период монитора, кадры выходили то через два, то через три vblank — это
                // и видно как дёрганье. Держим постоянный шаг в pace периодов: ровные 90
                // кадров/с выглядят плавнее рваных 100. Шаг растёт сразу, а уменьшается с
                // запасом, чтобы не скакать туда-сюда на границе.
                LARGE_INTEGER t; QueryPerformanceCounter(&t);
                double work = (double)(t.QuadPart - now.QuadPart) / (double)qf.QuadPart;
                workAvg = workAvg > 0 ? workAvg * 0.8 + work * 0.2 : work;
                int need = 1;
                while (need < 4 && workAvg * 1.15 > need * TARGET) ++need;
                if (need > pace) pace = need;
                else if (need < pace && workAvg * 1.5 < (pace - 1) * TARGET) pace = need;
                DwmFlush();                      // до следующей композиции DWM (vblank)
                for (int extra = 1; extra < pace; ++extra) {
                    QueryPerformanceCounter(&t);
                    if ((double)(t.QuadPart - lastShown.QuadPart) / (double)qf.QuadPart >= (pace - 0.5) * TARGET) break;
                    DwmFlush();
                }
                QueryPerformanceCounter(&lastShown);
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
    if (!dwmOk) timeEndPeriod(1);

    if (app.winEventHook) UnhookWinEvent(app.winEventHook);
    if (app.hookThreadId) PostThreadMessageW(app.hookThreadId, WM_QUIT, 0, 0);
    if (app.hookThread) {
        ScopedHandle ht(app.hookThread.release());
        WaitForSingleObject(ht, 1000);
    }
    RestoreSnap(app);          // вернуть прежнее состояние Aero Snap (и привязки)
    RestoreDesktopIcons(app);  // вернуть ярлыки рабочего стола

    // сохранить раскладку и собрать окна обратно на видимый рабочий стол
    EndGlide(app);              // вышли посреди панорамы — окна на места, слой убрать
    StopGlideMover(app);
    HideOverview(app);
    SyncWorldFromScreen(app);   // подхватить актуальные позиции
    SaveLayout(app);            // запомнить мировые позиции на следующий запуск
    GatherToDesktop(app);       // окна возвращаются на видимый стол
    EndSession();               // завершились чисто — файл сеанса не нужен

    RemoveTrayIcon(app);
    if (app.settings) DestroyWindow(app.settings);
    if (app.ovBack)  DestroyWindow(app.ovBack);
    if (app.ov)      DestroyWindow(app.ov);
    return 0;
}

