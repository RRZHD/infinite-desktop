// Окна: виртуальный экран, фильтр, перечисление, превью, перемещение, камера
// Часть InfiniteDesktop. Компилируется как единый модуль через main.cpp.

#include "id_common.h"

// ---------- Виртуальный экран ----------
void UpdateVirtualScreen(AppState& app) {
    app.vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    app.vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    app.vsW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    app.vsH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

// ---------- Фильтр окон (эвристика Alt+Tab) ----------
bool IsCloaked(HWND h) {
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
        return cloaked != 0;
    return false;
}

bool IsManageable(AppState& app, HWND h) {
    if (h == app.hud || h == app.ov) return false;
    if (!IsWindowVisible(h)) return false;
    if (IsIconic(h)) return false;                  // свёрнутые не трогаем
    if (GetWindowTextLengthW(h) == 0) return false;
    if (IsCloaked(h)) return false;

    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return false;
    if (GetWindow(h, GW_OWNER) != nullptr) return false; // только корневые окна

    wchar_t cls[128] = {};
    GetClassNameW(h, cls, 128);
    if (!lstrcmpW(cls, L"Progman") || !lstrcmpW(cls, L"WorkerW") ||
        !lstrcmpW(cls, L"Shell_TrayWnd") || !lstrcmpW(cls, L"Shell_SecondaryTrayWnd") ||
        !lstrcmpW(cls, L"Windows.UI.Core.CoreWindow"))
        return false;

    return true;
}

// Отступы от GetWindowRect до видимых границ окна (невидимые рамки изменения размера).
RECT ComputeVM(HWND h, const RECT& r) {
    RECT ext;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &ext, sizeof(ext)))) {
        RECT m = { ext.left - r.left, ext.top - r.top, r.right - ext.right, r.bottom - ext.bottom };
        if (m.left < 0) m.left = 0;   if (m.top < 0) m.top = 0;
        if (m.right < 0) m.right = 0; if (m.bottom < 0) m.bottom = 0;
        return m;
    }
    return { 0, 0, 0, 0 };
}

BOOL CALLBACK EnumProc(HWND h, LPARAM) {
    AppState& app = *g_app;
    if (!IsManageable(app, h)) return TRUE;
    RECT r;
    if (!GetWindowRect(h, &r)) return TRUE;
    LONG cx = (LONG)llround(app.camX), cy = (LONG)llround(app.camY);
    TrackedWin w;
    w.hwnd    = h;
    w.vm      = ComputeVM(h, r);
    w.pinned  = false;
    w.world.left   = r.left   + cx;
    w.world.top    = r.top    + cy;
    w.world.right  = r.right  + cx;
    w.world.bottom = r.bottom + cy;
    app.wins.push_back(std::move(w));
    return TRUE;
}

// ---------- DWM-превью ----------
void RegisterThumb(AppState& app, TrackedWin& w) {
    if (w.thumb || !app.hud || !app.previewsOn) return;
    if (SUCCEEDED(DwmRegisterThumbnail(app.hud, w.hwnd, w.thumb.put()))) {
        // RAII: ScopedThumbnail автоматически вызовет DwmUnregisterThumbnail при уничтожении
    }
}

void RegisterAllThumbs(AppState& app) {
    for (auto& w : app.wins) RegisterThumb(app, w);
}

// Превью для полноэкранного обзора (зум)

void RegisterOvThumb(AppState& app, TrackedWin& w) {
    if (w.thumbOv || !app.ov) return;
    if (SUCCEEDED(DwmRegisterThumbnail(app.ov, w.hwnd, w.thumbOv.put()))) {
        // RAII: ScopedThumbnail автоматически вызовет DwmUnregisterThumbnail при уничтожении
    }
}

BOOL CALLBACK OvZProc(HWND h, LPARAM) {     // собрать отслеживаемые в z-порядке
    AppState& app = *g_app;
    for (auto& w : app.wins) if (w.hwnd == h) { app.ovZ.push_back(h); break; }
    return TRUE;
}

// Превью регистрируем СНИЗУ ВВЕРХ по текущему z-порядку: DWM компонует их в порядке
// регистрации, поэтому верхнее окно (зарегистрированное последним) ложится поверх —
// как на реальном рабочем столе.
void RegisterAllOvThumbs(AppState& app) {
    app.ovZ.clear();
    EnumWindows(OvZProc, 0);                        // сверху вниз
    for (auto it = app.ovZ.rbegin(); it != app.ovZ.rend(); ++it)
        for (auto& w : app.wins) if (w.hwnd == *it) { RegisterOvThumb(app, w); break; }
    for (auto& w : app.wins) RegisterOvThumb(app, w);      // добрать не попавшие в EnumWindows
}

void RebuildWindowList(AppState& app) {
    // Очистка app.wins автоматически вызывает деструкторы ScopedThumbnail,
    // которые unregister-ят старые превью.
    app.wins.clear();
    EnumWindows(EnumProc, 0);
    if (app.hudVisible) RegisterAllThumbs(app);
}

// Синхронизировать мировые позиции с фактическим положением окон
// (учитывает ручное перетаскивание и закрытие окон).
void SyncWorldFromScreen(AppState& app) {
    LONG cx = (LONG)llround(app.camX), cy = (LONG)llround(app.camY);
    const LONG vL = app.vsX, vT = app.vsY, vR = app.vsX + app.vsW, vB = app.vsY + app.vsH;
    size_t write = 0;
    for (size_t i = 0; i < app.wins.size(); ++i) {
        HWND h = app.wins[i].hwnd;
        if (!IsWindow(h) || !IsWindowVisible(h) || IsIconic(h)) {
            // ScopedThumbnail автоматически unregister-нет превью при уничтожении
            continue;
        }
        // Актуальную позицию ОС имеют только окна в кадре (RepositionAll двигает лишь их).
        // У окон за пределами вьюпорта позиция устаревшая — их мир НЕ трогаем, иначе
        // отсечка в RepositionAll испортила бы сохранённые мировые координаты.
        int sx = app.wins[i].world.left  - cx, sy = app.wins[i].world.top    - cy;
        int sr = app.wins[i].world.right - cx, sb = app.wins[i].world.bottom - cy;
        bool onScreen = !(sr <= vL || sx >= vR || sb <= vT || sy >= vB);
        RECT r;
        if (onScreen && GetWindowRect(h, &r)) {
            app.wins[i].world.left   = r.left   + cx;
            app.wins[i].world.top    = r.top    + cy;
            app.wins[i].world.right  = r.right  + cx;
            app.wins[i].world.bottom = r.bottom + cy;
        }
        if (write != i) app.wins[write] = std::move(app.wins[i]);
        ++write;
    }
    app.wins.erase(app.wins.begin() + write, app.wins.end());
}

bool IsTracked(AppState& app, HWND h) {
    for (auto& w : app.wins) if (w.hwnd == h) return true;
    return false;
}

// Добавить вновь появившиеся окна (открытые уже после запуска).
BOOL CALLBACK AddNewProc(HWND h, LPARAM) {
    AppState& app = *g_app;
    if (!IsManageable(app, h) || IsTracked(app, h)) return TRUE;
    RECT r;
    if (!GetWindowRect(h, &r)) return TRUE;
    LONG cx = (LONG)llround(app.camX), cy = (LONG)llround(app.camY);
    TrackedWin w;
    w.hwnd    = h;
    w.vm      = ComputeVM(h, r);
    w.pinned  = false;
    w.world.left   = r.left   + cx;
    w.world.top    = r.top    + cy;
    w.world.right  = r.right  + cx;
    w.world.bottom = r.bottom + cy;
    app.wins.push_back(std::move(w));
    if (app.hudVisible && app.previewsOn) RegisterThumb(app, app.wins.back());
    if (app.overview)                   RegisterOvThumb(app, app.wins.back());
    return TRUE;
}

void AddNewWindows(AppState& app) { EnumWindows(AddNewProc, 0); }

// Расставить все окна по экрану согласно текущей камере.
// Индивидуальные SetWindowPos (а не DeferWindowPos): батч-перемещение чужих
// окон через HDWP ненадёжно — один сбой обнуляет весь батч, и не двигается
// ничего. SWP_ASYNCWINDOWPOS не блокирует нас на чужих потоках.
void RepositionAll(AppState& app) {
    LONG cx = (LONG)llround(app.camX), cy = (LONG)llround(app.camY);
    // Вьюпорт (виртуальный экран). Окна полностью вне него невидимы — двигать их
    // каждый кадр незачем; это главный источник рывков при панораме/перелёте, т.к.
    // синхронный SetWindowPos для чужого окна дорог. Ставим окно на место, лишь когда
    // его новый прямоугольник попадает в кадр (координаты абсолютные — без «прыжка»).
    const LONG vL = app.vsX, vT = app.vsY, vR = app.vsX + app.vsW, vB = app.vsY + app.vsH;
    for (auto& w : app.wins) {
        if (!IsWindow(w.hwnd)) continue;
        if (IsZoomed(w.hwnd)) continue;          // развёрнутые приклеены к монитору
        if (w.pinned) {                          // закреплённое окно не двигаем,
            RECT r;                              // а его мир тянем за фактической позицией
            if (GetWindowRect(w.hwnd, &r)) {
                w.world.left   = r.left   + cx; w.world.top    = r.top    + cy;
                w.world.right  = r.right  + cx; w.world.bottom = r.bottom + cy;
            }
            continue;
        }
        int x = w.world.left   - cx, y = w.world.top    - cy;
        int r = w.world.right  - cx, b = w.world.bottom - cy;
        bool onScreen = !(r <= vL || x >= vR || b <= vT || y >= vB);
        // Окно полностью за кадром И уже выставлено туда — двигать незачем (невидимо),
        // это и даёт экономию synchronous SetWindowPos. Но если оно ещё НЕ припарковано
        // (старт, либо только что ушло за край), один раз ставим его на мир-позицию,
        // иначе оно останется в видимой зоне / застрянет, выглядывая с края.
        if (!onScreen && w.parkedOff) continue;
        if (IsHungAppWindow(w.hwnd)) continue;   // не блокировать кадр на зависшем окне
        // Синхронно (без SWP_ASYNCWINDOWPOS): применяется сразу => окна точно следуют
        // за курсором, без «желейного» отставания от очереди асинхронных запросов.
        // SWP_NOSENDCHANGING: не дёргаем WM_WINDOWPOSCHANGING у чужого окна — быстрее.
        SetWindowPos(w.hwnd, nullptr, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
        w.parkedOff = !onScreen;                 // выставили за кадр — пометили припаркованным
    }
}

void PanBy(AppState& app, double dx, double dy) { app.targetX += dx; app.targetY += dy; }
void GoHome(AppState& app) { app.targetX = 0; app.targetY = 0; }

// Центрировать камеру на мировой точке (для клика по миникарте).
void CenterOn(AppState& app, double wx, double wy) {
    app.targetX = wx - app.vsX - app.vsW / 2.0;
    app.targetY = wy - app.vsY - app.vsH / 2.0;
}



