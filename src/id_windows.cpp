// Окна: виртуальный экран, фильтр, перечисление, превью, перемещение, камера
// Часть InfiniteDesktop. Компилируется как единый модуль через main.cpp.

#include "id_common.h"

// ---------- Виртуальный экран ----------
void UpdateVirtualScreen() {
    g_vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_vsW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_vsH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

// ---------- Фильтр окон (эвристика Alt+Tab) ----------
bool IsCloaked(HWND h) {
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
        return cloaked != 0;
    return false;
}

bool IsManageable(HWND h) {
    if (h == g_hud || h == g_ov) return false;
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
    if (!IsManageable(h)) return TRUE;
    RECT r;
    if (!GetWindowRect(h, &r)) return TRUE;
    LONG cx = (LONG)llround(g_camX), cy = (LONG)llround(g_camY);
    TrackedWin w;
    w.hwnd    = h;
    w.vm      = ComputeVM(h, r);
    w.pinned  = false;
    w.world.left   = r.left   + cx;
    w.world.top    = r.top    + cy;
    w.world.right  = r.right  + cx;
    w.world.bottom = r.bottom + cy;
    g_wins.push_back(std::move(w));
    return TRUE;
}

// ---------- DWM-превью ----------
void RegisterThumb(TrackedWin& w) {
    if (w.thumb || !g_hud || !g_previewsOn) return;
    if (SUCCEEDED(DwmRegisterThumbnail(g_hud, w.hwnd, w.thumb.put()))) {
        // RAII: ScopedThumbnail автоматически вызовет DwmUnregisterThumbnail при уничтожении
    }
}

void RegisterAllThumbs() {
    for (auto& w : g_wins) RegisterThumb(w);
}

// Превью для полноэкранного обзора (зум)

void RegisterOvThumb(TrackedWin& w) {
    if (w.thumbOv || !g_ov) return;
    if (SUCCEEDED(DwmRegisterThumbnail(g_ov, w.hwnd, w.thumbOv.put()))) {
        // RAII: ScopedThumbnail автоматически вызовет DwmUnregisterThumbnail при уничтожении
    }
}

BOOL CALLBACK OvZProc(HWND h, LPARAM) {     // собрать отслеживаемые в z-порядке
    for (auto& w : g_wins) if (w.hwnd == h) { g_ovZ.push_back(h); break; }
    return TRUE;
}

// Превью регистрируем СНИЗУ ВВЕРХ по текущему z-порядку: DWM компонует их в порядке
// регистрации, поэтому верхнее окно (зарегистрированное последним) ложится поверх —
// как на реальном рабочем столе.
void RegisterAllOvThumbs() {
    g_ovZ.clear();
    EnumWindows(OvZProc, 0);                        // сверху вниз
    for (auto it = g_ovZ.rbegin(); it != g_ovZ.rend(); ++it)
        for (auto& w : g_wins) if (w.hwnd == *it) { RegisterOvThumb(w); break; }
    for (auto& w : g_wins) RegisterOvThumb(w);      // добрать не попавшие в EnumWindows
}

void RebuildWindowList() {
    // Очистка g_wins автоматически вызывает деструкторы ScopedThumbnail,
    // которые unregister-ят старые превью.
    g_wins.clear();
    EnumWindows(EnumProc, 0);
    if (g_hudVisible) RegisterAllThumbs();
}

// Синхронизировать мировые позиции с фактическим положением окон
// (учитывает ручное перетаскивание и закрытие окон).
void SyncWorldFromScreen() {
    LONG cx = (LONG)llround(g_camX), cy = (LONG)llround(g_camY);
    const LONG vL = g_vsX, vT = g_vsY, vR = g_vsX + g_vsW, vB = g_vsY + g_vsH;
    size_t write = 0;
    for (size_t i = 0; i < g_wins.size(); ++i) {
        HWND h = g_wins[i].hwnd;
        if (!IsWindow(h) || !IsWindowVisible(h) || IsIconic(h)) {
            // ScopedThumbnail автоматически unregister-нет превью при уничтожении
            continue;
        }
        // Актуальную позицию ОС имеют только окна в кадре (RepositionAll двигает лишь их).
        // У окон за пределами вьюпорта позиция устаревшая — их мир НЕ трогаем, иначе
        // отсечка в RepositionAll испортила бы сохранённые мировые координаты.
        int sx = g_wins[i].world.left  - cx, sy = g_wins[i].world.top    - cy;
        int sr = g_wins[i].world.right - cx, sb = g_wins[i].world.bottom - cy;
        bool onScreen = !(sr <= vL || sx >= vR || sb <= vT || sy >= vB);
        RECT r;
        if (onScreen && GetWindowRect(h, &r)) {
            g_wins[i].world.left   = r.left   + cx;
            g_wins[i].world.top    = r.top    + cy;
            g_wins[i].world.right  = r.right  + cx;
            g_wins[i].world.bottom = r.bottom + cy;
        }
        if (write != i) g_wins[write] = std::move(g_wins[i]);
        ++write;
    }
    g_wins.erase(g_wins.begin() + write, g_wins.end());
}

bool IsTracked(HWND h) {
    for (auto& w : g_wins) if (w.hwnd == h) return true;
    return false;
}

// Добавить вновь появившиеся окна (открытые уже после запуска).
BOOL CALLBACK AddNewProc(HWND h, LPARAM) {
    if (!IsManageable(h) || IsTracked(h)) return TRUE;
    RECT r;
    if (!GetWindowRect(h, &r)) return TRUE;
    LONG cx = (LONG)llround(g_camX), cy = (LONG)llround(g_camY);
    TrackedWin w;
    w.hwnd    = h;
    w.vm      = ComputeVM(h, r);
    w.pinned  = false;
    w.world.left   = r.left   + cx;
    w.world.top    = r.top    + cy;
    w.world.right  = r.right  + cx;
    w.world.bottom = r.bottom + cy;
    g_wins.push_back(std::move(w));
    if (g_hudVisible && g_previewsOn) RegisterThumb(g_wins.back());
    if (g_overview)                   RegisterOvThumb(g_wins.back());
    return TRUE;
}

void AddNewWindows() { EnumWindows(AddNewProc, 0); }

// Расставить все окна по экрану согласно текущей камере.
// Индивидуальные SetWindowPos (а не DeferWindowPos): батч-перемещение чужих
// окон через HDWP ненадёжно — один сбой обнуляет весь батч, и не двигается
// ничего. SWP_ASYNCWINDOWPOS не блокирует нас на чужих потоках.
void RepositionAll() {
    LONG cx = (LONG)llround(g_camX), cy = (LONG)llround(g_camY);
    // Вьюпорт (виртуальный экран). Окна полностью вне него невидимы — двигать их
    // каждый кадр незачем; это главный источник рывков при панораме/перелёте, т.к.
    // синхронный SetWindowPos для чужого окна дорог. Ставим окно на место, лишь когда
    // его новый прямоугольник попадает в кадр (координаты абсолютные — без «прыжка»).
    const LONG vL = g_vsX, vT = g_vsY, vR = g_vsX + g_vsW, vB = g_vsY + g_vsH;
    for (auto& w : g_wins) {
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

void PanBy(double dx, double dy) { g_targetX += dx; g_targetY += dy; }
void GoHome() { g_targetX = 0; g_targetY = 0; }

// Центрировать камеру на мировой точке (для клика по миникарте).
void CenterOn(double wx, double wy) {
    g_targetX = wx - g_vsX - g_vsW / 2.0;
    g_targetY = wy - g_vsY - g_vsH / 2.0;
}


