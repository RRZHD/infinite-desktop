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

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#include <dwmapi.h>
#include <vector>
#include <string>
#include <fstream>
#include <cmath>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")

#ifndef SPI_GETWINARRANGING
#define SPI_GETWINARRANGING 0x0082
#endif
#ifndef SPI_SETWINARRANGING
#define SPI_SETWINARRANGING 0x0083
#endif

// ---------- Состояние ----------
struct TrackedWin {
    HWND       hwnd;
    RECT       world;            // позиция/размер в координатах мира
    HTHUMBNAIL thumb;            // живое DWM-превью на миникарте (или nullptr)
    HTHUMBNAIL thumbOv;          // живое DWM-превью в полноэкранном обзоре (зум)
};

static std::vector<TrackedWin> g_wins;
static double g_camX = 0, g_camY = 0;          // текущая камера (мировая точка в левом-верхнем углу виртуального экрана)
static double g_targetX = 0, g_targetY = 0;    // целевая камера (для плавной анимации)

// Виртуальный экран (объединение всех мониторов)
static int  g_vsX = 0, g_vsY = 0, g_vsW = 0, g_vsH = 0;

static HWND g_hud = nullptr;                    // окно миникарты
static bool g_hudVisible = true;
static bool g_previewsOn = true;                // живые DWM-превью на миникарте
static int  g_syncCounter = 0;
static DWORD g_lastHudTick = 0, g_lastSyncTick = 0;  // троттлинг миникарты/синхронизации по времени

// Хоткеи
enum {
    HK_LEFT = 1, HK_RIGHT, HK_UP, HK_DOWN,
    HK_HOME, HK_MAP, HK_REFRESH, HK_QUIT, HK_PREVIEW
};
const double PAN_STEP = 400.0;   // шаг панорамы в пикселях мира
const double EASE     = 0.22;    // коэффициент сглаживания камеры

// Текущее преобразование мир -> клиент миникарты
struct MapXform { double s; LONG ox, oy; RECT bounds; };
static MapXform g_xform;

// Фоновый холст-подложка на весь виртуальный экран
static HWND g_bg = nullptr;
static bool g_bgAttached = false;        // прикреплён ли холст на уровень обоев
static LONG g_bgLastCamX = 0, g_bgLastCamY = 0;  // камера, под которую подложка уже отрисована
const int GRID = 80;                     // шаг сетки холста, px

// Панорама средней кнопкой мыши (глобальный hook на отдельном потоке)
static HHOOK  g_mouseHook = nullptr;
static HANDLE g_hookThread = nullptr;
static DWORD  g_hookThreadId = 0;
static bool   g_dragging = false;
static POINT  g_lastMouse = {};

// Панорама/зум жестами тачпада (двухпальцевый свайп = колесо/гориз.колесо, пинч = Ctrl+колесо)
const double  TP_WHEEL = 0.8;            // px панорамы на единицу delta колеса

// Отслеживание изменения камеры для троттлинга перерисовки
static double g_lastCamX = 1e18, g_lastCamY = 1e18;

// Зум-обзор через DWM-превью (зум-аут всего рабочего стола колесом)
static HWND   g_ov = nullptr;            // полноэкранное окно обзора
static bool   g_overview = false;        // активен ли обзор
static double g_zoom = 1.0;              // текущий масштаб обзора (<=1)
static double g_zoomTarget = 1.0;        // целевой масштаб
// Якорь зума: мировая точка g_axW удерживается в клиентской точке g_axC
static double g_axWX = 0, g_axWY = 0, g_axCX = 0, g_axCY = 0;
// Перетаскивание окна/панорама внутри обзора
static HWND   g_ovDragHwnd = nullptr;    // перетаскиваемое окно (или nullptr — фон)
static POINT  g_ovDragLast = {};
static bool   g_ovDragMoved = false;
const double  ZOOM_MIN = 0.08;           // самый дальний зум-аут
const double  ZOOM_STEP = 1.15;          // множитель на тик колеса
const double  ZOOM_EASE = 0.25;
static HDC     g_ovMemDC = nullptr;       // кэш двойной буферизации обзора
static HBITMAP g_ovBmp = nullptr;
static int     g_ovCacheW = 0, g_ovCacheH = 0;

// ---------- Виртуальный экран ----------
static void UpdateVirtualScreen() {
    g_vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_vsW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_vsH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

// ---------- Фильтр окон (эвристика Alt+Tab) ----------
static bool IsCloaked(HWND h) {
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
        return cloaked != 0;
    return false;
}

static bool IsManageable(HWND h) {
    if (h == g_hud || h == g_bg || h == g_ov) return false;
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

static BOOL CALLBACK EnumProc(HWND h, LPARAM) {
    if (!IsManageable(h)) return TRUE;
    RECT r;
    if (!GetWindowRect(h, &r)) return TRUE;
    LONG cx = (LONG)llround(g_camX), cy = (LONG)llround(g_camY);
    TrackedWin w;
    w.hwnd    = h;
    w.thumb   = nullptr;
    w.thumbOv = nullptr;
    w.world.left   = r.left   + cx;
    w.world.top    = r.top    + cy;
    w.world.right  = r.right  + cx;
    w.world.bottom = r.bottom + cy;
    g_wins.push_back(w);
    return TRUE;
}

// ---------- DWM-превью ----------
static void RegisterThumb(TrackedWin& w) {
    if (w.thumb || !g_hud || !g_previewsOn) return;
    DwmRegisterThumbnail(g_hud, w.hwnd, &w.thumb);
}

static void UnregisterThumb(TrackedWin& w) {
    if (w.thumb) {
        DwmUnregisterThumbnail(w.thumb);
        w.thumb = nullptr;
    }
}

static void RegisterAllThumbs() {
    for (auto& w : g_wins) RegisterThumb(w);
}

static void UnregisterAllThumbs() {
    for (auto& w : g_wins) UnregisterThumb(w);
}

// Превью для полноэкранного обзора (зум)
static void RegisterOvThumb(TrackedWin& w) {
    if (w.thumbOv || !g_ov) return;
    DwmRegisterThumbnail(g_ov, w.hwnd, &w.thumbOv);
}
static void UnregisterOvThumb(TrackedWin& w) {
    if (w.thumbOv) { DwmUnregisterThumbnail(w.thumbOv); w.thumbOv = nullptr; }
}
static void RegisterAllOvThumbs()   { for (auto& w : g_wins) RegisterOvThumb(w); }
static void UnregisterAllOvThumbs() { for (auto& w : g_wins) UnregisterOvThumb(w); }

static void RebuildWindowList() {
    UnregisterAllThumbs();
    UnregisterAllOvThumbs();
    g_wins.clear();
    EnumWindows(EnumProc, 0);
    if (g_hudVisible) RegisterAllThumbs();
    if (g_bg && !g_bgAttached)
        SetWindowPos(g_bg, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Синхронизировать мировые позиции с фактическим положением окон
// (учитывает ручное перетаскивание и закрытие окон).
static void SyncWorldFromScreen() {
    LONG cx = (LONG)llround(g_camX), cy = (LONG)llround(g_camY);
    for (size_t i = 0; i < g_wins.size(); ) {
        HWND h = g_wins[i].hwnd;
        if (!IsWindow(h) || !IsWindowVisible(h) || IsIconic(h)) {
            UnregisterThumb(g_wins[i]);
            UnregisterOvThumb(g_wins[i]);
            g_wins.erase(g_wins.begin() + i);
            continue;
        }
        RECT r;
        if (GetWindowRect(h, &r)) {
            g_wins[i].world.left   = r.left   + cx;
            g_wins[i].world.top    = r.top    + cy;
            g_wins[i].world.right  = r.right  + cx;
            g_wins[i].world.bottom = r.bottom + cy;
        }
        ++i;
    }
}

static bool IsTracked(HWND h) {
    for (auto& w : g_wins) if (w.hwnd == h) return true;
    return false;
}

// Добавить вновь появившиеся окна (открытые уже после запуска).
static BOOL CALLBACK AddNewProc(HWND h, LPARAM) {
    if (!IsManageable(h) || IsTracked(h)) return TRUE;
    RECT r;
    if (!GetWindowRect(h, &r)) return TRUE;
    LONG cx = (LONG)llround(g_camX), cy = (LONG)llround(g_camY);
    TrackedWin w;
    w.hwnd    = h;
    w.thumb   = nullptr;
    w.thumbOv = nullptr;
    w.world.left   = r.left   + cx;
    w.world.top    = r.top    + cy;
    w.world.right  = r.right  + cx;
    w.world.bottom = r.bottom + cy;
    g_wins.push_back(w);
    if (g_hudVisible && g_previewsOn) RegisterThumb(g_wins.back());
    if (g_overview)                   RegisterOvThumb(g_wins.back());
    return TRUE;
}

static void AddNewWindows() { EnumWindows(AddNewProc, 0); }

// Расставить все окна по экрану согласно текущей камере.
// Индивидуальные SetWindowPos (а не DeferWindowPos): батч-перемещение чужих
// окон через HDWP ненадёжно — один сбой обнуляет весь батч, и не двигается
// ничего. SWP_ASYNCWINDOWPOS не блокирует нас на чужих потоках.
static void RepositionAll() {
    LONG cx = (LONG)llround(g_camX), cy = (LONG)llround(g_camY);
    for (auto& w : g_wins) {
        if (!IsWindow(w.hwnd)) continue;
        if (IsZoomed(w.hwnd)) continue;          // развёрнутые приклеены к монитору
        if (IsHungAppWindow(w.hwnd)) continue;   // не блокировать кадр на зависшем окне
        int x = w.world.left - cx;
        int y = w.world.top  - cy;
        // Синхронно (без SWP_ASYNCWINDOWPOS): применяется сразу => окна точно следуют
        // за курсором, без «желейного» отставания от очереди асинхронных запросов.
        SetWindowPos(w.hwnd, nullptr, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static void PanBy(double dx, double dy) { g_targetX += dx; g_targetY += dy; }
static void GoHome() { g_targetX = 0; g_targetY = 0; }

// Центрировать камеру на мировой точке (для клика по миникарте).
static void CenterOn(double wx, double wy) {
    g_targetX = wx - g_vsX - g_vsW / 2.0;
    g_targetY = wy - g_vsY - g_vsH / 2.0;
}

// ---------- Преобразование мир <-> миникарта ----------
// Границы мира берём по окнам (стабильны при панораме) + поле.
static void ComputeWorldBounds(RECT& out) {
    if (g_wins.empty()) {
        LONG cx = (LONG)llround(g_camX), cy = (LONG)llround(g_camY);
        out.left = g_vsX + cx; out.top = g_vsY + cy;
        out.right = out.left + g_vsW; out.bottom = out.top + g_vsH;
    } else {
        out = g_wins[0].world;
        for (auto& w : g_wins) {
            out.left   = min(out.left,   w.world.left);
            out.top    = min(out.top,    w.world.top);
            out.right  = max(out.right,  w.world.right);
            out.bottom = max(out.bottom, w.world.bottom);
        }
    }
    LONG mx = (out.right - out.left) / 10 + 100;
    LONG my = (out.bottom - out.top) / 10 + 100;
    out.left -= mx; out.top -= my; out.right += mx; out.bottom += my;
}

static void ComputeXform(const RECT& client) {
    RECT b; ComputeWorldBounds(b);
    double wW = (double)(b.right - b.left);
    double wH = (double)(b.bottom - b.top);
    if (wW < 1) wW = 1; if (wH < 1) wH = 1;

    int pad = 10;
    double cw = client.right - 2 * pad;
    double ch = client.bottom - 2 * pad;
    if (cw < 1) cw = 1; if (ch < 1) ch = 1;
    double s = min(cw / wW, ch / wH);

    g_xform.s  = s;
    g_xform.ox = pad + (LONG)((cw - wW * s) / 2);
    g_xform.oy = pad + (LONG)((ch - wH * s) / 2);
    g_xform.bounds = b;
}

static RECT WorldToClient(const RECT& w) {
    RECT r;
    r.left   = g_xform.ox + (LONG)((w.left   - g_xform.bounds.left) * g_xform.s);
    r.top    = g_xform.oy + (LONG)((w.top    - g_xform.bounds.top)  * g_xform.s);
    r.right  = g_xform.ox + (LONG)((w.right  - g_xform.bounds.left) * g_xform.s);
    r.bottom = g_xform.oy + (LONG)((w.bottom - g_xform.bounds.top)  * g_xform.s);
    return r;
}

static void MapToWorld(int mx, int my, double& wx, double& wy) {
    wx = g_xform.bounds.left + (mx - g_xform.ox) / g_xform.s;
    wy = g_xform.bounds.top  + (my - g_xform.oy) / g_xform.s;
}

// Обновить позиции живых превью под текущую раскладку/камеру.
static void UpdateThumbs(const RECT& client) {
    if (!g_hudVisible || !g_previewsOn) return;
    for (auto& w : g_wins) {
        if (!w.thumb) continue;
        RECT d = WorldToClient(w.world);
        DWM_THUMBNAIL_PROPERTIES p = {};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE |
                    DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        p.opacity = 255;
        p.fSourceClientAreaOnly = FALSE;
        bool offscreen = (d.right <= d.left || d.bottom <= d.top ||
                          d.right < client.left || d.left > client.right ||
                          d.bottom < client.top || d.top > client.bottom);
        p.fVisible = offscreen ? FALSE : TRUE;
        p.rcDestination = d;
        DwmUpdateThumbnailProperties(w.thumb, &p);
    }
}

// ---------- Отрисовка миникарты (фон, бордюры, вьюпорт) ----------
// DWM рисует превью ПОВЕРХ этого GDI-содержимого, поэтому бордюры окон
// делаем чуть шире рамки превью, а рамку вьюпорта — поверх пустых зон.
static void DrawMinimap(HDC hdc, const RECT& client) {
    HBRUSH bg = CreateSolidBrush(RGB(20, 22, 28));
    FillRect(hdc, &client, bg);
    DeleteObject(bg);

    ComputeXform(client);

    // С превью: тонкая рамка вокруг живой картинки.
    // Без превью: закрашенные прямоугольники, чтобы окна были видны на карте.
    HPEN framePen = CreatePen(PS_SOLID, 1, RGB(90, 110, 140));
    HBRUSH fillBr = CreateSolidBrush(RGB(70, 130, 200));
    HGDIOBJ oldPen = SelectObject(hdc, framePen);
    HGDIOBJ oldBr  = SelectObject(hdc, g_previewsOn ? GetStockObject(NULL_BRUSH)
                                                    : (HGDIOBJ)fillBr);
    for (auto& w : g_wins) {
        RECT m = WorldToClient(w.world);
        InflateRect(&m, 1, 1);
        Rectangle(hdc, m.left, m.top, m.right, m.bottom);
    }

    // рамка вьюпорта (видимая область всех мониторов) — пустой кистью
    SelectObject(hdc, GetStockObject(NULL_BRUSH));
    LONG cx = (LONG)llround(g_camX), cy = (LONG)llround(g_camY);
    RECT vpWorld = { g_vsX + cx, g_vsY + cy, g_vsX + g_vsW + cx, g_vsY + g_vsH + cy };
    RECT vp = WorldToClient(vpWorld);
    HPEN vpPen = CreatePen(PS_SOLID, 2, RGB(255, 200, 60));
    SelectObject(hdc, vpPen);
    Rectangle(hdc, vp.left, vp.top, vp.right, vp.bottom);

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBr);
    DeleteObject(framePen);
    DeleteObject(fillBr);
    DeleteObject(vpPen);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(150, 160, 175));
    const wchar_t* tip = L"ЛКМ — перелёт   Ctrl+Alt+стрелки — панорама";
    TextOutW(hdc, 10, client.bottom - 18, tip, lstrlenW(tip));
}

// ---------- Привязка холста на уровень обоев ----------
// Найти окно-хост обоев. На Win10/11 шелл создаёт WorkerW позади иконок
// рабочего стола после сообщения 0x052C в Progman.
static BOOL CALLBACK FindWorkerProc(HWND top, LPARAM lp) {
    if (FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr)) {
        HWND worker = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
        if (worker) *(HWND*)lp = worker;
    }
    return TRUE;
}

static HWND FindWallpaperHost() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) {
        DWORD_PTR res = 0;
        SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &res);
    }
    HWND found = nullptr;
    EnumWindows(FindWorkerProc, (LPARAM)&found);       // WorkerW за иконками
    if (found) return found;
    if (progman && FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr))
        return progman;                                // иконки прямо под Progman
    HWND any = FindWindowExW(nullptr, nullptr, L"WorkerW", nullptr);
    return any ? any : progman;
}

// Расположить холст: как дочернее окно обоев (0,0..vsW,vsH) либо, при неудаче,
// как нижнее окно во весь виртуальный экран.
static void PlaceBackdrop() {
    if (!g_bg) return;
    if (g_bgAttached) {
        SetWindowPos(g_bg, nullptr, 0, 0, g_vsW, g_vsH,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        MoveWindow(g_bg, g_vsX, g_vsY, g_vsW, g_vsH, TRUE);
        SetWindowPos(g_bg, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    InvalidateRect(g_bg, nullptr, FALSE);
}

static void AttachBackdrop() {
    HWND host = FindWallpaperHost();
    if (host) {
        LONG st = GetWindowLongW(g_bg, GWL_STYLE);
        SetWindowLongW(g_bg, GWL_STYLE, (st & ~WS_POPUP) | WS_CHILD);
        SetParent(g_bg, host);                 // холст становится частью обоев
        g_bgAttached = true;
    }
    PlaceBackdrop();
    ShowWindow(g_bg, SW_SHOWNA);
}

// ---------- Фоновый холст (подложка на все экраны) ----------
// Сетка холста, сдвинутая по камере => визуально «бесконечное» полотно.
static void DrawGrid(HDC hdc, const RECT& client) {
    HBRUSH bg = CreateSolidBrush(RGB(24, 26, 32));
    FillRect(hdc, &client, bg);
    DeleteObject(bg);

    LONG cx = (LONG)llround(g_camX), cy = (LONG)llround(g_camY);
    // фаза сетки: точка клиента (0,0) соответствует миру (vsX+camX, vsY+camY)
    LONG phaseX = ((-(g_vsX + cx)) % GRID + GRID) % GRID;
    LONG phaseY = ((-(g_vsY + cy)) % GRID + GRID) % GRID;

    HPEN minor = CreatePen(PS_SOLID, 1, RGB(38, 41, 50));
    HGDIOBJ oldPen = SelectObject(hdc, minor);
    for (LONG x = phaseX; x < client.right; x += GRID) {
        MoveToEx(hdc, x, 0, nullptr); LineTo(hdc, x, client.bottom);
    }
    for (LONG y = phaseY; y < client.bottom; y += GRID) {
        MoveToEx(hdc, 0, y, nullptr); LineTo(hdc, client.right, y);
    }
    SelectObject(hdc, oldPen);
    DeleteObject(minor);

    // оси мира (x=0, y=0) — ориентир «домой»
    HPEN axis = CreatePen(PS_SOLID, 2, RGB(60, 90, 130));
    oldPen = SelectObject(hdc, axis);
    LONG ax = -(g_vsX + cx);  // клиент-координата мировой x=0
    LONG ay = -(g_vsY + cy);
    if (ax >= 0 && ax < client.right) { MoveToEx(hdc, ax, 0, nullptr); LineTo(hdc, ax, client.bottom); }
    if (ay >= 0 && ay < client.bottom){ MoveToEx(hdc, 0, ay, nullptr); LineTo(hdc, client.right, ay); }
    SelectObject(hdc, oldPen);
    DeleteObject(axis);
}

static LRESULT CALLBACK BgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1; // фон рисуем сами => без мерцания
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        // Рисуем прямо в DC окна: BeginPaint ограничивает область вывода зоной
        // обновления, поэтому при скролле перерисовывается лишь узкая полоска.
        RECT rc; GetClientRect(hwnd, &rc);
        DrawGrid(hdc, rc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Сместить подложку под текущую камеру через скролл: перерисовывается только
// открывшаяся полоска, а не весь виртуальный экран — это убирает рывки панорамы.
static void ScrollBackdrop() {
    if (!g_bg) return;
    LONG nx = (LONG)llround(g_camX), ny = (LONG)llround(g_camY);
    LONG dx = g_bgLastCamX - nx, dy = g_bgLastCamY - ny;  // содержимое смещается на -(Δкамеры)
    if (dx == 0 && dy == 0) return;
    g_bgLastCamX = nx; g_bgLastCamY = ny;
    if (labs(dx) >= g_vsW || labs(dy) >= g_vsH)
        InvalidateRect(g_bg, nullptr, FALSE);             // скачок больше экрана — целиком
    else
        ScrollWindowEx(g_bg, dx, dy, nullptr, nullptr, nullptr, nullptr, SW_INVALIDATE);
}

// ---------- Зум колесом (вход в обзор) ----------
static bool IsDesktopClass(HWND h) {
    if (!h) return true;
    if (h == g_bg || h == g_ov) return true;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    return !lstrcmpW(cls, L"WorkerW") || !lstrcmpW(cls, L"Progman") ||
           !lstrcmpW(cls, L"SHELLDLL_DefView") || !lstrcmpW(cls, L"SysListView32");
}

// Меняет масштаб в `factor` раз с привязкой к точке pt (экранные координаты).
static void DoZoomFactor(double factor, POINT pt) {
    double cx = pt.x - g_vsX;       // клиентские координаты окна обзора
    double cy = pt.y - g_vsY;
    if (!g_overview && g_zoom >= 0.999) {
        // вход в обзор: якорь так, чтобы при z=1 совпасть с реальными позициями
        g_axCX = cx; g_axCY = cy;
        g_axWX = cx + g_vsX + g_camX;
        g_axWY = cy + g_vsY + g_camY;
    }
    double wx = g_axWX + (cx - g_axCX) / g_zoom;   // мир под точкой
    double wy = g_axWY + (cy - g_axCY) / g_zoom;
    g_zoomTarget *= factor;
    if (g_zoomTarget > 1.0)      g_zoomTarget = 1.0;
    if (g_zoomTarget < ZOOM_MIN) g_zoomTarget = ZOOM_MIN;
    g_axWX = wx; g_axCX = cx;                        // переякорить на точку
    g_axWY = wy; g_axCY = cy;
}

// Тик колеса: один шаг зума к/от курсора.
static void DoZoom(int delta, POINT pt) {
    DoZoomFactor(delta > 0 ? ZOOM_STEP : 1.0 / ZOOM_STEP, pt);
}

// Панорама холста двухпальцевым скроллом (delta колеса/гориз.колеса).
// Холст следует за пальцами; вертикаль и горизонталь — независимо.
static void PanWheel(double dxDelta, double dyDelta) {
    if (g_overview) {
        g_axCX -= dxDelta * TP_WHEEL;
        g_axCY += dyDelta * TP_WHEEL;
    } else {
        g_targetX += dxDelta * TP_WHEEL;  // плавно через easing
        g_targetY -= dyDelta * TP_WHEEL;
    }
}

// ---------- Глобальный hook СКМ/колеса ----------
static LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        MSLLHOOKSTRUCT* m = (MSLLHOOKSTRUCT*)lp;
        switch (wp) {
        case WM_MBUTTONDOWN: {
            bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            // Панораму берём только над пустым столом / в обзоре / по Ctrl+СКМ.
            // Над окном приложения (Blender, браузер) СКМ уходит в само приложение.
            if (g_overview || ctrl || IsDesktopClass(WindowFromPoint(m->pt))) {
                g_dragging = true;
                g_lastMouse = m->pt;
                return 1;             // поглощаем только когда сами панорамим
            }
            break;                    // иначе СКМ — в приложение
        }
        case WM_MOUSEMOVE:
            if (g_dragging) {
                int dx = m->pt.x - g_lastMouse.x;
                int dy = m->pt.y - g_lastMouse.y;
                g_lastMouse = m->pt;
                if (g_overview) {
                    // в обзоре СКМ двигает сам зум-вид (якорь)
                    g_axCX += dx; g_axCY += dy;
                } else {
                    // «рука»: окна следуют за курсором => камера в обратную сторону.
                    g_camX -= dx; g_camY -= dy;
                    g_targetX = g_camX; g_targetY = g_camY;
                }
                // НЕ поглощаем move — иначе системный курсор замирает на месте.
            }
            break;
        case WM_MOUSEWHEEL: {
            int delta = (short)HIWORD(m->mouseData);
            bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool onDesk = g_overview || IsDesktopClass(WindowFromPoint(m->pt));
            if (!onDesk) break;                              // внутри приложения — обычная прокрутка
            // Классический клик колеса мыши кратен 120 => зум (как раньше).
            // Мелкая дельта двухпальцевого скролла тачпада => панорама.
            bool mouseWheel = (delta % WHEEL_DELTA) == 0;
            if (ctrl || mouseWheel) { DoZoom(delta, m->pt); return 1; }   // зум
            PanWheel(0.0, (double)delta); return 1;          // 2 пальца верт. => панорама
        }
        case WM_MOUSEHWHEEL: {
            bool onDesk = g_overview || IsDesktopClass(WindowFromPoint(m->pt));
            if (onDesk) { PanWheel((double)(short)HIWORD(m->mouseData), 0.0); return 1; }  // 2 пальца гориз.
            break;
        }
        case WM_MBUTTONUP:
            if (g_dragging) { g_dragging = false; return 1; }
            break;
        }
    }
    return CallNextHookEx(g_mouseHook, code, wp, lp);
}

// ---------- Полноэкранный обзор (зум через DWM-превью) ----------
static double OvClientX(double wx) { return g_axCX + (wx - g_axWX) * g_zoom; }
static double OvClientY(double wy) { return g_axCY + (wy - g_axWY) * g_zoom; }

static void ShowOverview() {
    if (g_overview) return;
    RegisterAllOvThumbs();
    SetWindowPos(g_ov, HWND_TOPMOST, g_vsX, g_vsY, g_vsW, g_vsH, SWP_NOACTIVATE);
    ShowWindow(g_ov, SW_SHOWNA);
    g_overview = true;
}

static void HideOverview() {
    if (!g_overview) return;
    UnregisterAllOvThumbs();
    ShowWindow(g_ov, SW_HIDE);
    g_overview = false;
}

// При выходе из обзора подобрать камеру так, чтобы реальные окна оказались там,
// где показывал зум-вид при масштабе 1.
static void CommitZoomCam() {
    g_camX = g_axWX - g_axCX - g_vsX;
    g_camY = g_axWY - g_axCY - g_vsY;
    g_targetX = g_camX; g_targetY = g_camY;
}

// Разложить превью окон по обзору согласно текущему масштабу/якорю.
static void UpdateOverview() {
    if (!g_overview) return;
    for (auto& w : g_wins) {
        if (!w.thumbOv) continue;
        RECT d = { (LONG)OvClientX(w.world.left),  (LONG)OvClientY(w.world.top),
                   (LONG)OvClientX(w.world.right), (LONG)OvClientY(w.world.bottom) };
        DWM_THUMBNAIL_PROPERTIES p = {};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE |
                    DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        p.opacity = 255;
        p.fSourceClientAreaOnly = FALSE;
        bool off = (d.right <= d.left || d.bottom <= d.top ||
                    d.right < 0 || d.left > g_vsW || d.bottom < 0 || d.top > g_vsH);
        p.fVisible = off ? FALSE : TRUE;
        p.rcDestination = d;
        DwmUpdateThumbnailProperties(w.thumbOv, &p);
    }
    InvalidateRect(g_ov, nullptr, FALSE);
}

static void EnsureOvCache(HDC ref, int w, int h) {
    if (g_ovMemDC && g_ovCacheW == w && g_ovCacheH == h) return;
    if (g_ovMemDC) { DeleteDC(g_ovMemDC);    g_ovMemDC = nullptr; }
    if (g_ovBmp)   { DeleteObject(g_ovBmp);  g_ovBmp = nullptr; }
    g_ovMemDC = CreateCompatibleDC(ref);
    g_ovBmp   = CreateCompatibleBitmap(ref, w, h);
    SelectObject(g_ovMemDC, g_ovBmp);
    g_ovCacheW = w; g_ovCacheH = h;
}

static void DrawOverview(HDC hdc, const RECT& client) {
    HBRUSH bg = CreateSolidBrush(RGB(16, 18, 23));   // тёмный холст скрывает реальный стол
    FillRect(hdc, &client, bg);
    DeleteObject(bg);

    // рамки окон (видны как контур вокруг живых превью)
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(80, 100, 130));
    HGDIOBJ op = SelectObject(hdc, pen);
    HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    for (auto& w : g_wins) {
        RECT d = { (LONG)OvClientX(w.world.left),  (LONG)OvClientY(w.world.top),
                   (LONG)OvClientX(w.world.right), (LONG)OvClientY(w.world.bottom) };
        InflateRect(&d, 1, 1);
        Rectangle(hdc, d.left, d.top, d.right, d.bottom);
    }
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(pen);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(150, 160, 175));
    wchar_t info[96];
    wsprintfW(info, L"Обзор  %d%%   колесо — зум • клик — приблизить • СКМ — двигать", (int)(g_zoom * 100));
    TextOutW(hdc, 24, 20, info, lstrlenW(info));
}

static LRESULT CALLBACK OvProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        EnsureOvCache(hdc, rc.right, rc.bottom);
        DrawOverview(g_ovMemDC, rc);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, g_ovMemDC, 0, 0, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        double cx = (short)LOWORD(lp), cy = (short)HIWORD(lp);
        // какое окно под курсором (берём верхнее — последнее в списке)
        g_ovDragHwnd = nullptr;
        for (auto& w : g_wins) {
            double l = OvClientX(w.world.left),  t = OvClientY(w.world.top);
            double r = OvClientX(w.world.right), b = OvClientY(w.world.bottom);
            if (cx >= l && cx <= r && cy >= t && cy <= b) g_ovDragHwnd = w.hwnd;
        }
        g_ovDragLast.x = (LONG)cx; g_ovDragLast.y = (LONG)cy;
        g_ovDragMoved = false;
        SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (GetCapture() != hwnd) return 0;
        double cx = (short)LOWORD(lp), cy = (short)HIWORD(lp);
        double ddx = cx - g_ovDragLast.x, ddy = cy - g_ovDragLast.y;
        if (!g_ovDragMoved && (fabs(ddx) > 3 || fabs(ddy) > 3)) g_ovDragMoved = true;
        if (g_ovDragMoved) {
            if (g_ovDragHwnd) {
                // двигаем окно: мировой сдвиг = клиентский / масштаб
                LONG wdx = (LONG)llround(ddx / g_zoom), wdy = (LONG)llround(ddy / g_zoom);
                for (auto& w : g_wins) if (w.hwnd == g_ovDragHwnd) {
                    w.world.left += wdx; w.world.right += wdx;
                    w.world.top  += wdy; w.world.bottom += wdy;
                    break;
                }
            } else {
                g_axCX += ddx; g_axCY += ddy;   // тащим фон => панорама обзора
            }
        }
        g_ovDragLast.x = (LONG)cx; g_ovDragLast.y = (LONG)cy;
        return 0;
    }
    case WM_LBUTTONUP: {
        bool moved = g_ovDragMoved;
        if (GetCapture() == hwnd) ReleaseCapture();
        g_ovDragHwnd = nullptr; g_ovDragMoved = false;
        if (!moved) {
            // простой клик => приблизить точку и вернуться в 1:1
            double cx = (short)LOWORD(lp), cy = (short)HIWORD(lp);
            double wx = g_axWX + (cx - g_axCX) / g_zoom;
            double wy = g_axWY + (cy - g_axCY) / g_zoom;
            g_axWX = wx; g_axCX = g_vsW / 2.0;
            g_axWY = wy; g_axCY = g_vsH / 2.0;
            g_zoomTarget = 1.0;
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------- Оконная процедура HUD ----------
static LRESULT CALLBACK HudProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        DrawMinimap(mem, rc);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        RECT rc; GetClientRect(hwnd, &rc);
        ComputeXform(rc);
        double wx, wy; MapToWorld(LOWORD(lp), HIWORD(lp), wx, wy);
        CenterOn(wx, wy);
        return 0;
    }
    case WM_DISPLAYCHANGE: {
        // изменилась конфигурация мониторов
        UpdateVirtualScreen();
        PlaceBackdrop();
        g_bgLastCamX = (LONG)llround(g_camX);   // подложка перерисована целиком
        g_bgLastCamY = (LONG)llround(g_camY);
        if (g_ov) SetWindowPos(g_ov, HWND_TOPMOST, g_vsX, g_vsY, g_vsW, g_vsH,
                               SWP_NOACTIVATE | (g_overview ? 0 : SWP_NOREDRAW));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_HOTKEY: {
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
            else              { UnregisterAllThumbs(); ShowWindow(g_hud, SW_HIDE); }
            break;
        case HK_PREVIEW:
            g_previewsOn = !g_previewsOn;
            if (g_previewsOn) { if (g_hudVisible) RegisterAllThumbs(); }
            else                UnregisterAllThumbs();   // снять живые превью
            InvalidateRect(g_hud, nullptr, FALSE);
            break;
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
static DWORD WINAPI HookThreadProc(LPVOID param) {
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
static HWINEVENTHOOK g_winEventHook = nullptr;

// Центрировать мировую точку по центру монитора, где сейчас курсор (там, где
// окно «вызывается»), а не по центру всех мониторов.
static void CenterOnCursorMonitor(double wx, double wy) {
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

static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
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
static BOOL g_prevWinArranging = TRUE;

static void DisableSnap() {
    g_prevWinArranging = TRUE;
    SystemParametersInfoW(SPI_GETWINARRANGING, 0, &g_prevWinArranging, 0);
    if (g_prevWinArranging)
        SystemParametersInfoW(SPI_SETWINARRANGING, 0, (PVOID)FALSE, SPIF_SENDCHANGE);
}

static void RestoreSnap() {
    if (g_prevWinArranging)   // вернуть, только если изначально был включён
        SystemParametersInfoW(SPI_SETWINARRANGING, 0,
                              (PVOID)(INT_PTR)g_prevWinArranging, SPIF_SENDCHANGE);
}

// Включить осведомлённость о DPI на уровне монитора (для точных координат
// при разных масштабах на разных мониторах), с откатом на системный режим.
static void EnableDpiAwareness() {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    typedef BOOL (WINAPI *PFN)(HANDLE);
    if (u32) {
        auto fn = (PFN)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (fn && fn((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */)) return;
    }
    SetProcessDPIAware();
}

// ---------- Сохранение/восстановление раскладки окон ----------
struct SavedWin { std::wstring cls, title; RECT world; bool used; };
static std::vector<SavedWin> g_saved;

static std::string WToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring Utf8ToW(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static void WinKey(HWND h, std::wstring& cls, std::wstring& title) {
    wchar_t c[128] = {};
    GetClassNameW(h, c, 128);
    cls = c;
    int n = GetWindowTextLengthW(h);
    std::wstring t((size_t)n + 1, 0);
    int got = GetWindowTextW(h, &t[0], n + 1);
    t.resize(got < 0 ? 0 : got);
    title = t;
}

static std::wstring LayoutPath() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring dir = (n && n < MAX_PATH) ? std::wstring(buf) : L".";
    dir += L"\\InfiniteDesktop";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\layout.txt";
}

// Сохранить мировые позиции всех окон (формат: L<TAB>T<TAB>R<TAB>B<TAB>class<TAB>title).
static void SaveLayout() {
    std::ofstream f(LayoutPath().c_str(), std::ios::binary | std::ios::trunc);
    if (!f) return;
    for (auto& w : g_wins) {
        if (!IsWindow(w.hwnd)) continue;
        std::wstring cls, title;
        WinKey(w.hwnd, cls, title);
        std::string line = std::to_string(w.world.left)  + "\t" + std::to_string(w.world.top) + "\t"
                         + std::to_string(w.world.right) + "\t" + std::to_string(w.world.bottom) + "\t"
                         + WToUtf8(cls) + "\t" + WToUtf8(title) + "\n";
        f.write(line.data(), (std::streamsize)line.size());
    }
}

static void LoadLayout() {
    g_saved.clear();
    std::ifstream f(LayoutPath().c_str(), std::ios::binary);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        long v[4]; size_t p = 0; bool ok = true;
        for (int i = 0; i < 4 && ok; ++i) {
            size_t t = line.find('\t', p);
            if (t == std::string::npos) { ok = false; break; }
            v[i] = atol(line.substr(p, t - p).c_str());
            p = t + 1;
        }
        if (!ok) continue;
        size_t t = line.find('\t', p);          // граница class|title
        if (t == std::string::npos) continue;
        SavedWin sw;
        sw.cls   = Utf8ToW(line.substr(p, t - p));
        sw.title = Utf8ToW(line.substr(t + 1));
        sw.world.left = v[0]; sw.world.top = v[1]; sw.world.right = v[2]; sw.world.bottom = v[3];
        sw.used = false;
        g_saved.push_back(sw);
    }
}

// Разложить окна «полками» без наложения. Старт — ниже уже размещённых окон.
static void SpreadWindows(const std::vector<int>& idx) {
    if (idx.empty()) return;
    std::vector<bool> spread(g_wins.size(), false);
    for (int i : idx) spread[i] = true;

    bool any = false; RECT bb = {};
    for (size_t i = 0; i < g_wins.size(); ++i) {
        if (spread[i]) continue;
        RECT& r = g_wins[i].world;
        if (!any) { bb = r; any = true; }
        else { bb.left = min(bb.left, r.left); bb.top = min(bb.top, r.top);
               bb.right = max(bb.right, r.right); bb.bottom = max(bb.bottom, r.bottom); }
    }
    const LONG gap = 40;
    LONG rowStartX = any ? bb.left : (LONG)g_vsX + gap;
    LONG x = rowStartX;
    LONG y = any ? bb.bottom + gap : (LONG)g_vsY + gap;
    LONG rowH = 0;
    LONG maxRowW = g_vsW > 0 ? (LONG)g_vsW : 3000;   // ширина полки ~ весь виртуальный экран
    for (int i : idx) {
        LONG w = g_wins[i].world.right - g_wins[i].world.left;
        LONG h = g_wins[i].world.bottom - g_wins[i].world.top;
        if (x - rowStartX + w > maxRowW && x > rowStartX) { x = rowStartX; y += rowH + gap; rowH = 0; }
        g_wins[i].world.left = x;       g_wins[i].world.top = y;
        g_wins[i].world.right = x + w;  g_wins[i].world.bottom = y + h;
        x += w + gap; rowH = max(rowH, h);
    }
}

// При запуске: вернуть окна на сохранённые места; новые/несопоставленные — разложить.
static void ArrangeOnStartup() {
    std::vector<bool> assigned(g_wins.size(), false);
    auto tryAssign = [&](bool exact) {
        for (size_t i = 0; i < g_wins.size(); ++i) {
            if (assigned[i]) continue;
            std::wstring cls, title;
            WinKey(g_wins[i].hwnd, cls, title);
            for (auto& s : g_saved) {
                if (s.used) continue;
                if (s.cls != cls) continue;
                if (exact && s.title != title) continue;
                LONG w = g_wins[i].world.right - g_wins[i].world.left;
                LONG h = g_wins[i].world.bottom - g_wins[i].world.top;
                g_wins[i].world.left = s.world.left;  g_wins[i].world.top = s.world.top;
                g_wins[i].world.right = s.world.left + w;
                g_wins[i].world.bottom = s.world.top + h;   // позиция из файла, размер текущий
                s.used = true; assigned[i] = true;
                break;
            }
        }
    };
    tryAssign(true);    // точное совпадение class+title
    tryAssign(false);   // запасное — по одному классу (заголовок мог измениться)

    std::vector<int> toSpread;
    for (size_t i = 0; i < g_wins.size(); ++i)
        if (!assigned[i]) toSpread.push_back((int)i);
    SpreadWindows(toSpread);
}

// При выходе: собрать окна на видимый рабочий стол (в рабочую область монитора).
static void GatherToDesktop() {
    for (auto& w : g_wins) {
        if (!IsWindow(w.hwnd) || IsZoomed(w.hwnd)) continue;
        RECT r;
        if (!GetWindowRect(w.hwnd, &r)) continue;
        MONITORINFO mi = { sizeof(mi) };
        if (!GetMonitorInfoW(MonitorFromWindow(w.hwnd, MONITOR_DEFAULTTONEAREST), &mi)) continue;
        LONG ww = r.right - r.left, hh = r.bottom - r.top;
        LONG x = r.left, y = r.top;
        if (x + ww > mi.rcWork.right)  x = mi.rcWork.right - ww;
        if (y + hh > mi.rcWork.bottom) y = mi.rcWork.bottom - hh;
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y < mi.rcWork.top)  y = mi.rcWork.top;
        SetWindowPos(w.hwnd, nullptr, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

// Один кадр анимации/панорамы. dt — реальное время кадра (сглаживание не зависит
// от частоты кадров). Вызывается из высокочастотного цикла, а не по WM_TIMER.
static void FrameTick(double dt) {
    double kz = 1.0 - pow(1.0 - ZOOM_EASE, dt * 60.0);   // эквивалент при 60 Гц
    if (fabs(g_zoomTarget - g_zoom) > 0.001) g_zoom += (g_zoomTarget - g_zoom) * kz;
    else g_zoom = g_zoomTarget;
    bool wantOv = (g_zoom < 0.999) || (g_zoomTarget < 0.999);
    if (wantOv && !g_overview) ShowOverview();
    if (!wantOv && g_overview) { CommitZoomCam(); HideOverview(); }
    if (g_overview) { UpdateOverview(); return; }

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
        ScrollBackdrop();       // подложка — тоже каждый кадр
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
    DisableSnap();   // на бесконечном столе краёв нет — выключаем Aero Snap

    // Класс и окно фоновой подложки (на весь виртуальный экран)
    WNDCLASSW bgc = {};
    bgc.lpfnWndProc = BgProc;
    bgc.hInstance = hInst;
    bgc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    bgc.lpszClassName = L"InfiniteDesktopBG";
    RegisterClassW(&bgc);

    g_bg = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        bgc.lpszClassName, L"", WS_POPUP,
        g_vsX, g_vsY, g_vsW, g_vsH,
        nullptr, nullptr, hInst, nullptr);
    AttachBackdrop();   // прикрепить холст на уровень обоев (виден на всех мониторах)

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

    int mapW = 420, mapH = 260, margin = 16;
    // правый-верхний угол основного монитора (виртуальные координаты)
    int px = g_vsX + g_vsW - mapW - margin;
    int py = g_vsY + margin;
    g_hud = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        wc.lpszClassName, L"InfiniteDesktop",
        WS_POPUP | WS_VISIBLE,
        px, py, mapW, mapH,
        nullptr, nullptr, hInst, nullptr);

    RegisterHotKey(g_hud, HK_LEFT,    MOD_CONTROL | MOD_ALT, VK_LEFT);
    RegisterHotKey(g_hud, HK_RIGHT,   MOD_CONTROL | MOD_ALT, VK_RIGHT);
    RegisterHotKey(g_hud, HK_UP,      MOD_CONTROL | MOD_ALT, VK_UP);
    RegisterHotKey(g_hud, HK_DOWN,    MOD_CONTROL | MOD_ALT, VK_DOWN);
    RegisterHotKey(g_hud, HK_HOME,    MOD_CONTROL | MOD_ALT, 'H');
    RegisterHotKey(g_hud, HK_MAP,     MOD_CONTROL | MOD_ALT, 'M');
    RegisterHotKey(g_hud, HK_REFRESH, MOD_CONTROL | MOD_ALT, 'R');
    RegisterHotKey(g_hud, HK_QUIT,    MOD_CONTROL | MOD_ALT, 'Q');
    RegisterHotKey(g_hud, HK_PREVIEW, MOD_CONTROL | MOD_ALT, 'P');

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
    RestoreSnap();   // вернуть прежнее состояние Aero Snap (и привязки)

    // сохранить раскладку и собрать окна обратно на видимый рабочий стол
    HideOverview();
    SyncWorldFromScreen();   // подхватить актуальные позиции
    SaveLayout();            // запомнить мировые позиции на следующий запуск
    GatherToDesktop();       // окна возвращаются на видимый стол

    UnregisterAllThumbs();
    UnregisterAllOvThumbs();
    if (g_ovMemDC) DeleteDC(g_ovMemDC);
    if (g_ovBmp)   DeleteObject(g_ovBmp);
    if (g_ov)      DestroyWindow(g_ov);
    if (g_bg)      DestroyWindow(g_bg);
    return 0;
}
