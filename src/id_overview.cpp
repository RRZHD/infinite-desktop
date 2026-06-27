// Обзор (зум): превью, тени, обои, окно обзора
// Часть InfiniteDesktop. Компилируется как единый модуль через main.cpp.

#pragma once
#include "id_common.h"

// ---------- Полноэкранный обзор (зум через DWM-превью) ----------
double OvClientX(double wx) { return g_axCX + (wx - g_axWX) * g_zoom; }
double OvClientY(double wy) { return g_axCY + (wy - g_axWY) * g_zoom; }

// Видимый мировой прямоугольник окна (без невидимых рамок) — для превью/теней/клика,
// чтобы зазоры в обзоре совпадали с холстом.
RECT VisWorld(const TrackedWin& w) {
    return { w.world.left + w.vm.left,  w.world.top + w.vm.top,
             w.world.right - w.vm.right, w.world.bottom - w.vm.bottom };
}

void LoadWallpaper();   // определена ниже

void ShowOverview() {
    if (g_overview) return;
    LoadWallpaper(); g_lastWallCap = GetTickCount();  // актуальные обои на входе
    g_ovLastZoom = -1;   // форсировать первую перерисовку
    RegisterAllOvThumbs();
    SetWindowPos(g_ov, HWND_TOPMOST, g_vsX, g_vsY, g_vsW, g_vsH, SWP_NOACTIVATE);
    ShowWindow(g_ov, SW_SHOWNA);
    g_overview = true;
}

void HideOverview() {
    if (!g_overview) return;
    UnregisterAllOvThumbs();
    ShowWindow(g_ov, SW_HIDE);
    g_overview = false;
}

// При выходе из обзора подобрать камеру так, чтобы реальные окна оказались там,
// где показывал зум-вид при масштабе 1.
void CommitZoomCam() {
    g_camX = g_axWX - g_axCX - g_vsX;
    g_camY = g_axWY - g_axCY - g_vsY;
    g_targetX = g_camX; g_targetY = g_camY;
}

// Разложить превью окон по обзору согласно текущему масштабу/якорю.
void UpdateOverview() {
    if (!g_overview) return;
    for (auto& w : g_wins) {
        if (!w.thumbOv) continue;
        RECT vw = VisWorld(w);
        RECT d = { (LONG)llround(OvClientX(vw.left)),  (LONG)llround(OvClientY(vw.top)),
                   (LONG)llround(OvClientX(vw.right)), (LONG)llround(OvClientY(vw.bottom)) };
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

void EnsureOvCache(HDC ref, int w, int h) {
    if (g_ovMemDC && g_ovCacheW == w && g_ovCacheH == h) return;
    if (g_ovMemDC) { DeleteDC(g_ovMemDC);    g_ovMemDC = nullptr; }
    if (g_ovBmp)   { DeleteObject(g_ovBmp);  g_ovBmp = nullptr; }
    g_ovMemDC = CreateCompatibleDC(ref);
    g_ovBmp   = CreateCompatibleBitmap(ref, w, h);
    SelectObject(g_ovMemDC, g_ovBmp);
    g_ovCacheW = w; g_ovCacheH = h;
}

// Рендер обоев пользователя в кэш g_wallBmp (по каждому монитору, режим «Заполнение»).
struct WallCtx { Gdiplus::Graphics* g; Gdiplus::Bitmap* img; };
BOOL CALLBACK WallMonProc(HMONITOR hm, HDC, LPRECT, LPARAM lp) {
    WallCtx* c = (WallCtx*)lp;
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(hm, &mi)) return TRUE;
    double mx = mi.rcMonitor.left - g_vsX, my = mi.rcMonitor.top - g_vsY;
    double mw = mi.rcMonitor.right - mi.rcMonitor.left, mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    double iw = c->img->GetWidth(), ih = c->img->GetHeight();
    if (iw < 1 || ih < 1) return TRUE;
    double s = max(mw / iw, mh / ih);   // cover (как «Заполнение» в Windows)
    double dw = iw * s, dh = ih * s;
    double dx = mx + (mw - dw) / 2, dy = my + (mh - dh) / 2;
    Gdiplus::Region clip(Gdiplus::RectF((Gdiplus::REAL)mx, (Gdiplus::REAL)my,
                                        (Gdiplus::REAL)mw, (Gdiplus::REAL)mh));
    c->g->SetClip(&clip);
    c->g->DrawImage(c->img, (Gdiplus::REAL)dx, (Gdiplus::REAL)dy,
                    (Gdiplus::REAL)dw, (Gdiplus::REAL)dh);
    c->g->ResetClip();
    return TRUE;
}

void LoadWallpaper() {
    if (g_wallDC)  { DeleteDC(g_wallDC);   g_wallDC = nullptr; }
    if (g_wallBmp) { DeleteObject(g_wallBmp); g_wallBmp = nullptr; }
    g_wallW = g_wallH = 0;

    wchar_t path[MAX_PATH] = {};
    if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0) || !path[0]) { g_wallPath.clear(); return; }
    Gdiplus::Bitmap img(path);
    if (img.GetLastStatus() != Gdiplus::Ok) { g_wallPath.clear(); return; }

    HDC screen = GetDC(nullptr);
    g_wallDC  = CreateCompatibleDC(screen);
    g_wallBmp = CreateCompatibleBitmap(screen, g_vsW, g_vsH);
    ReleaseDC(nullptr, screen);
    if (!g_wallDC || !g_wallBmp) return;
    SelectObject(g_wallDC, g_wallBmp);

    RECT full = { 0, 0, g_vsW, g_vsH };
    HBRUSH bk = CreateSolidBrush(RGB(16, 18, 23));   // на случай зазоров между мониторами
    FillRect(g_wallDC, &full, bk);
    DeleteObject(bk);
    {
        Gdiplus::Graphics g(g_wallDC);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        WallCtx ctx = { &g, &img };
        EnumDisplayMonitors(nullptr, nullptr, WallMonProc, (LPARAM)&ctx);
    }
    g_wallW = g_vsW; g_wallH = g_vsH;
    g_wallPath = path;
}

void EnsureBlack() {
    if (g_blackDC) return;
    HDC s = GetDC(nullptr);
    g_blackDC = CreateCompatibleDC(s);
    g_blackBmp = CreateCompatibleBitmap(s, 1, 1);
    ReleaseDC(nullptr, s);
    SelectObject(g_blackDC, g_blackBmp);
    SetPixelV(g_blackDC, 0, 0, RGB(0, 0, 0));
}

// Мягкая тень под окно: несколько расширяющихся скруглённых слоёв чёрного с малой
// альфой, накапливающихся в градиент (как тень окна Windows). Превью ложится сверху.
void DrawShadow(HDC hdc, const RECT& r, int radius) {
    EnsureBlack();
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 16, 0 };   // ~16 альфы на слой
    for (int i = 7; i >= 1; --i) {
        RECT s = r;
        InflateRect(&s, i, i);
        OffsetRect(&s, 0, i / 2 + 1);               // лёгкое смещение вниз
        HRGN rgn = CreateRoundRectRgn(s.left, s.top, s.right, s.bottom,
                                      radius + i, radius + i);
        SelectClipRgn(hdc, rgn);
        AlphaBlend(hdc, s.left, s.top, s.right - s.left, s.bottom - s.top,
                   g_blackDC, 0, 0, 1, 1, bf);
        DeleteObject(rgn);
    }
    SelectClipRgn(hdc, nullptr);
}

void DrawOverview(HDC hdc, const RECT& client) {
    HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hdc, &client, bg);
    DeleteObject(bg);
    if (g_wallBmp) {
        // Обои пользователя, затемнённые тем сильнее, чем дальше отдалили.
        double dim = (1.0 - g_zoom) / (1.0 - ZOOM_MIN);
        if (dim < 0) dim = 0; if (dim > 1) dim = 1;
        BYTE a = (BYTE)((1.0 - dim * 0.7) * 255.0);   // не темнее ~70%
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, a, 0 };
        AlphaBlend(hdc, 0, 0, client.right, client.bottom,
                   g_wallDC, 0, 0, g_wallW, g_wallH, bf);
    }

    // Тени под окна (без рамок) — превью с закруглениями Win11 лягут поверх.
    int radius = (int)(8.0 * g_zoom + 0.5);
    if (radius < 2) radius = 2;
    for (auto& w : g_wins) {
        RECT vw = VisWorld(w);
        RECT d = { (LONG)llround(OvClientX(vw.left)),  (LONG)llround(OvClientY(vw.top)),
                   (LONG)llround(OvClientX(vw.right)), (LONG)llround(OvClientY(vw.bottom)) };
        if (d.right <= d.left || d.bottom <= d.top) continue;
        DrawShadow(hdc, d, radius);
    }

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(150, 160, 175));
    wchar_t info[96];
    wsprintfW(info, L"Обзор  %d%%   колесо — зум • клик — приблизить • СКМ — двигать", (int)(g_zoom * 100));
    TextOutW(hdc, 24, 20, info, lstrlenW(info));
}

LRESULT CALLBACK OvProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
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
        // верхнее окно под курсором: идём по z-порядку сверху вниз, берём первое
        g_ovDragHwnd = nullptr;
        for (HWND h : g_ovZ) {
            for (auto& w : g_wins) if (w.hwnd == h) {
                RECT vw = VisWorld(w);
                double l = OvClientX(vw.left),  t = OvClientY(vw.top);
                double r = OvClientX(vw.right), b = OvClientY(vw.bottom);
                if (cx >= l && cx <= r && cy >= t && cy <= b) g_ovDragHwnd = h;
                break;
            }
            if (g_ovDragHwnd) break;
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


