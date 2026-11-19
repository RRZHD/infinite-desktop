// Плавная панорама: слой живых превью на время движения камеры и рабочий поток,
// который тем временем двигает настоящие окна. Часть InfiniteDesktop.
//
// Зачем. Перемещение настоящего окна — системный вызов ценой 0,25–0,5 мс на окно, под
// общей блокировкой оконного менеджера (замеры: tests.exe --bench и --bench-modes). На
// мониторе 180 Гц кадр длится 5,6 мс: дюжина окон в кадре — и панорама идёт на половине
// частоты, две дюжины — на трети, причём неровно. Потоками это не лечится: вызовы стоят в
// очереди к одной блокировке.
//
// Как. Пока камера движется, поверх обычных окон лежит окно обзора на масштабе 1:1 с
// живыми DWM-превью всех окон на их местах — выглядит как сам рабочий стол. Двигаются
// превью: это одна дешёвая операция композитора на окно, кадр идёт на частоте монитора
// при любом числе окон. Настоящие окна стоят под слоем и догоняют камеру в рабочем потоке
// — чтобы их содержимое оставалось живым и чтобы в конце им оставалось пройти немного.
// Когда движение закончилось, окна встают точно на места и слой убирается.
//
// Слой лежит в обычном z-слое, под окнами «поверх всех»: панель задач, миникарта и
// такие окна остаются видны как есть (отслеживаемые окна «поверх всех» главный поток
// двигает сам, каждый кадр). Неотслеживаемые окна (диалоги с владельцем, палитры) холст
// и так не двигает — их превью стоят на месте.

#include "id_common.h"

// ---------- Рабочий поток ----------
// Берёт последние цели и по кругу ставит окна на них, пока все не окажутся на местах.
// Пришли новые цели посреди круга — продолжает с того же окна уже по новым: иначе при
// непрерывном движении до последних окон очередь не доходила бы никогда.
static DWORD WINAPI MoverThread(LPVOID param) {
    GlideMover& mv = *(GlideMover*)param;
    const UINT F = SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING;
    std::vector<GlideMove> t;
    unsigned seen = 0;
    size_t cursor = 0, clean = 0;
    LARGE_INTEGER qf, burst = {}, now;
    QueryPerformanceFrequency(&qf);
    for (;;) {
        EnterCriticalSection(&mv.cs);
        bool quit = mv.quit;
        double budgetMs = mv.budgetMs;
        if (mv.version != seen) { t = mv.targets; seen = mv.version; clean = 0; }
        bool done = t.empty() || clean >= t.size();
        // Порция исчерпана — отдаём блокировку оконного менеджера главному потоку: он
        // разбудит на следующем кадре, когда сам уйдёт ждать показа.
        QueryPerformanceCounter(&now);
        bool spent = burst.QuadPart && (double)(now.QuadPart - burst.QuadPart) * 1000.0 / (double)qf.QuadPart >= budgetMs;
        if (done || spent) SetEvent(mv.idle);   // под блокировкой: главный поток не увидит «свободен» раньше времени
        LeaveCriticalSection(&mv.cs);
        if (quit) return 0;
        if (done || spent) {
            WaitForSingleObject(mv.wake, INFINITE);
            QueryPerformanceCounter(&burst);    // новая порция
            continue;
        }
        if (!burst.QuadPart) QueryPerformanceCounter(&burst);
        if (cursor >= t.size()) cursor = 0;
        const GlideMove& m = t[cursor++];
        RECT r;
        if (IsWindow(m.hwnd) && GetWindowRect(m.hwnd, &r) && (r.left != m.x || r.top != m.y) &&
            !IsHungAppWindow(m.hwnd))
            SetWindowPos(m.hwnd, nullptr, m.x, m.y, 0, 0, F);
        ++clean;                            // это окно обработано по текущим целям
    }
}

static bool EnsureMover(AppState& app) {
    GlideMover& mv = app.mover;
    if (mv.thread) return true;
    mv.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    mv.idle = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (!mv.wake || !mv.idle) return false;
    mv.thread = CreateThread(nullptr, 0, MoverThread, &mv, 0, nullptr);
    return mv.thread != nullptr;
}

// Передать цели и разбудить поток на одну порцию. Порция — около 40 % периода
// монитора: поток работает, пока главный ждёт показа кадра, и замолкает к началу
// следующего, не мешая ему (иначе их вызовы стояли бы в очереди к одной блокировке).
static void PostTargets(AppState& app, std::vector<GlideMove>& targets) {
    GlideMover& mv = app.mover;
    EnterCriticalSection(&mv.cs);
    mv.targets.swap(targets);
    mv.budgetMs = app.framePeriod * 1000.0 * app.glideBudget;
    ++mv.version;
    ResetEvent(mv.idle);
    LeaveCriticalSection(&mv.cs);
    SetEvent(mv.wake);
}

// Остановить перемещения и дождаться, пока поток закончит начатое: после этого окна
// двигает только главный поток. Ждём недолго — поток мог встать на зависшем окне.
static void QuiesceMover(AppState& app) {
    if (!app.mover.thread) return;
    std::vector<GlideMove> none;
    PostTargets(app, none);                   // пустые цели: поток закончит начатый вызов и встанет
    WaitForSingleObject(app.mover.idle, 250);
}

void StopGlideMover(AppState& app) {
    GlideMover& mv = app.mover;
    if (!mv.thread) return;
    EnterCriticalSection(&mv.cs);
    mv.quit = true;
    LeaveCriticalSection(&mv.cs);
    SetEvent(mv.wake);
    WaitForSingleObject(mv.thread, 1000);
    CloseHandle(mv.thread); mv.thread = nullptr;
    CloseHandle(mv.wake);   mv.wake = nullptr;
    CloseHandle(mv.idle);   mv.idle = nullptr;
}

// ---------- Превью ----------
static bool IsCloakedWindow(HWND h) {
    int cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

// Окна обычного слоя сверху вниз: всё, что слой превью закроет собой.
static BOOL CALLBACK GlideEnumProc(HWND h, LPARAM lp) {
    AppState& app = *g_app;
    if (h == app.ov || h == app.ovBack || h == app.hud || h == app.settings) return TRUE;
    if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOPMOST) return TRUE;   // остаётся поверх слоя как есть
    if (IsShellDesktopWindow(h)) {
        // Дошли до рабочего стола: всё, что ниже него, на экране не видно. Обычно ниже
        // ничего и нет, но «Показать рабочий стол» в Windows 11 окна не сворачивает, а
        // поднимает стол поверх них — их превью в слое быть не должно. Служебные окна
        // оболочки того же класса бывают мелкими; столом считаем окно во весь экран.
        RECT r;
        if (GetWindowRect(h, &r) && (r.right - r.left) * 2 >= app.vsW && (r.bottom - r.top) * 2 >= app.vsH) return FALSE;
        return TRUE;                                                   // сам стол рисуем мы (обои)
    }
    if (IsCloakedWindow(h)) return TRUE;                               // другой виртуальный стол и т.п.
    ((std::vector<HWND>*)lp)->push_back(h);
    return TRUE;
}

// Зарегистрировать превью снизу вверх по z-порядку (DWM кладёт их в порядке регистрации).
// Отслеживаемые окна — в свои поля, их двигает UpdateOverview; остальные — неподвижные.
static void RegisterGlideThumbs(AppState& app) {
    for (auto& w : app.wins) {
        w.thumbOv.reset(); w.thumbShadow.reset();
        w.glideReal  = IsWindow(w.hwnd) && (GetWindowLongW(w.hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        // Холст не двигает закреплённые окна и (в обычном режиме) развёрнутые. Запоминаем
        // один раз: во время движения это не меняется, а спрашивать систему каждый кадр дорого.
        w.glideFixed = w.pinned || !IsWindow(w.hwnd) || !IsTopLevelWindow(w.hwnd) ||
                       (!app.moveMaximized && IsZoomed(w.hwnd));
    }
    app.glideStatic.clear();
    std::vector<HWND> order;
    EnumWindows(GlideEnumProc, (LPARAM)&order);
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        HWND h = *it;
        TrackedWin* tw = nullptr;
        for (auto& w : app.wins) if (w.hwnd == h) { tw = &w; break; }
        if (tw) { RegisterOvThumb(app, *tw); continue; }
        // Неотслеживаемое окно: превью на том же месте, где стоит само окно (видимая рамка).
        RECT r;
        if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) && !GetWindowRect(h, &r)) continue;
        if (r.right <= r.left || r.bottom <= r.top) continue;
        if (r.right <= app.vsX || r.left >= app.vsX + app.vsW || r.bottom <= app.vsY || r.top >= app.vsY + app.vsH) continue;
        ScopedThumbnail t;
        if (FAILED(DwmRegisterThumbnail(app.ov, h, t.put()))) continue;
        DWM_THUMBNAIL_PROPERTIES p = {};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        p.rcDestination = { r.left - app.vsX, r.top - app.vsY, r.right - app.vsX, r.bottom - app.vsY };
        p.opacity = 255; p.fVisible = TRUE; p.fSourceClientAreaOnly = FALSE;
        DwmUpdateThumbnailProperties(t, &p);
        app.glideStatic.push_back(std::move(t));
    }
}

// Привести якорь обзора к камере на масштабе 1:1: клиентская точка (0,0) окна обзора —
// это мировая точка левого-верхнего угла экрана. Дальше работают обычные UpdateOverview
// и RenderOverview. Закреплённые и «приклеенные» развёрнутые окна холст не двигает —
// их мир тянем за фактическим положением, как это делает RepositionAll.
static void SyncGlideView(AppState& app) {
    LONG cx = (LONG)llround(app.camX), cy = (LONG)llround(app.camY);
    for (auto& w : app.wins) {
        if (!w.glideFixed) continue;
        RECT r;
        if (GetWindowRect(w.hwnd, &r)) w.world = { r.left + cx, r.top + cy, r.right + cx, r.bottom + cy };
    }
    app.zoom = 1.0;
    app.axCX = 0; app.axCY = 0;
    app.axWX = (double)(app.vsX + cx);
    app.axWY = (double)(app.vsY + cy);
}

// Слой — на самый верх обычного z-слоя (под окна «поверх всех»), подложка обоев — под него.
static void RaiseGlideLayer(AppState& app, bool show) {
    const UINT keep = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
    // Окно во весь монитор оболочка считает полноэкранным приложением и убирает под него
    // панель задач — вместе с кнопкой «Пуск» и треем она пропадала на время движения.
    // Свойство NonRudeHWND говорит оболочке, что окно панель задач не вытесняет.
    SetPropW(app.ov, L"NonRudeHWND", (HANDLE)(INT_PTR)1);
    if (app.ovBack) SetPropW(app.ovBack, L"NonRudeHWND", (HANDLE)(INT_PTR)1);
    SetWindowPos(app.ov, HWND_NOTOPMOST, 0, 0, 0, 0, keep);   // снять «поверх всех», если стояло после зума
    SetWindowPos(app.ov, HWND_TOP, app.vsX, app.vsY, app.vsW, app.vsH, SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : 0));
    if (app.ovLiveBg && app.ovBack) {
        SetWindowPos(app.ovBack, HWND_NOTOPMOST, 0, 0, 0, 0, keep);
        SetWindowPos(app.ovBack, app.ov, app.vsX, app.vsY, app.vsW, app.vsH, SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : 0));
    }
}

// ---------- Вход, кадр, выход ----------
bool BeginGlide(AppState& app) {
    if (app.glide) return true;
    if (!app.ov || app.overview || !EnsureMover(app)) return false;
    PrepareOvBackground(app);
    app.glide = true;                 // с этого момента UpdateOverview/RenderOverview работают на слой
    RegisterGlideThumbs(app);
    SyncGlideView(app);
    // Первый кадр — ДО показа окна: слой появляется сразу с превью на местах, без кадра
    // «обои без окон».
    UpdateOverview(app);
    RenderOverview(app, app.ov);
    if (!app.ovRT) {                  // Direct2D недоступен — слой не из чего рисовать
        for (auto& w : app.wins) { w.thumbOv.reset(); w.thumbShadow.reset(); w.glideReal = false; w.glideFixed = false; }
        app.glideStatic.clear(); app.wallThumb.reset(); app.ovLiveBg = false;
        app.glide = false;
        return false;
    }
    RaiseGlideLayer(app, true);
    return true;
}

void GlideFrame(AppState& app) {
    if (!app.glide) return;
    SyncGlideView(app);
    RepositionGlideReal(app);         // окна «поверх всех» — настоящие, двигаем в такт кадру
    std::vector<GlideMove> targets;
    PlanGlideMoves(app, targets);
    UpdateOverview(app);              // превью — на новые места
    PostTargets(app, targets);        // остальные настоящие окна догоняет рабочий поток — пока мы ждём показа
    RenderOverview(app, app.ov);      // фон и тени; показ кадра ждёт vblank
}

void EndGlide(AppState& app) {
    if (!app.glide) return;
    QuiesceMover(app);
    // Окна — точно на места, уже главным потоком. Признак парковки сбрасываем: рабочий
    // поток мог оставить окно где угодно по дороге, пусть RepositionAll поставит каждое.
    for (auto& w : app.wins) { w.glideReal = false; w.glideFixed = false; w.parkedOff = false; }
    RepositionAll(app);
    // Окна на местах и слой убран — в одной композиции: под слоем лежит ровно та же картинка.
    if (app.ovBack) ShowWindow(app.ovBack, SW_HIDE);
    ShowWindow(app.ov, SW_HIDE);
    for (auto& w : app.wins) { w.thumbOv.reset(); w.thumbShadow.reset(); }
    app.glideStatic.clear();
    app.wallThumb.reset();
    app.ovLiveBg = false;
    app.glide = false;
}

// Во время панорамы активировали другое окно: система подняла его НАД слоем, а порядок
// превью устарел. Пересобираем превью по новому порядку и возвращаем слой наверх.
void GlideRestack(AppState& app) {
    if (!app.glide) return;
    RegisterGlideThumbs(app);
    SyncGlideView(app);
    UpdateOverview(app);
    RaiseGlideLayer(app, false);
}
