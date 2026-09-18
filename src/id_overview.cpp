// Обзор (зум): превью, тени, обои, окно обзора
// Часть InfiniteDesktop. Компилируется как единый модуль через main.cpp.

#include "id_common.h"

// ---------- Полноэкранный обзор (зум через DWM-превью) ----------
double OvClientX(AppState& app, double wx) { return app.axCX + (wx - app.axWX) * app.zoom; }
double OvClientY(AppState& app, double wy) { return app.axCY + (wy - app.axWY) * app.zoom; }

// Видимый мировой прямоугольник окна (без невидимых рамок) — для превью/теней/клика,
// чтобы зазоры в обзоре совпадали с холстом.
RECT VisWorld(const TrackedWin& w) {
    return { w.world.left + w.vm.left,  w.world.top + w.vm.top,
             w.world.right - w.vm.right, w.world.bottom - w.vm.bottom };
}

void LoadWallpaper(AppState& app);   // определена ниже

void ShowOverview(AppState& app) {
    if (app.overview) return;
    LoadWallpaper(app); app.lastWallCap = GetTickCount();  // актуальные обои на входе
    app.ovLastZoom = -1;   // форсировать первую перерисовку
    RegisterAllOvThumbs(app);
    SetWindowPos(app.ov, HWND_TOPMOST, app.vsX, app.vsY, app.vsW, app.vsH, SWP_NOACTIVATE);
    ShowWindow(app.ov, SW_SHOWNA);
    app.overview = true;
}

void HideOverview(AppState& app) {
    if (!app.overview) return;
    // Очистка превью обзора: сбрасываем в app.wins, ScopedThumbnail деструктор очистит.
    for (auto& w : app.wins) w.thumbOv.reset();
    ShowWindow(app.ov, SW_HIDE);
    app.overview = false;
}

// При выходе из обзора подобрать камеру так, чтобы реальные окна оказались там,
// где показывал зум-вид при масштабе 1.
void CommitZoomCam(AppState& app) {
    app.camX = app.axWX - app.axCX - app.vsX;
    app.camY = app.axWY - app.axCY - app.vsY;
    app.targetX = app.camX; app.targetY = app.camY;
}

// Разложить превью окон по обзору согласно текущему масштабу/якорю.
void UpdateOverview(AppState& app) {
    if (!app.overview) return;
    for (auto& w : app.wins) {
        if (!w.thumbOv) continue;
        RECT vw = VisWorld(w);
        RECT d = { (LONG)llround(OvClientX(app, vw.left)),  (LONG)llround(OvClientY(app, vw.top)),
                   (LONG)llround(OvClientX(app, vw.right)), (LONG)llround(OvClientY(app, vw.bottom)) };
        DWM_THUMBNAIL_PROPERTIES p = {};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE |
                    DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        p.opacity = 255;
        p.fSourceClientAreaOnly = FALSE;
        bool off = (d.right <= d.left || d.bottom <= d.top ||
                    d.right < 0 || d.left > app.vsW || d.bottom < 0 || d.top > app.vsH);
        p.fVisible = off ? FALSE : TRUE;
        p.rcDestination = d;
        DwmUpdateThumbnailProperties(w.thumbOv, &p);
    }
    InvalidateRect(app.ov, nullptr, FALSE);
}

void EnsureOvCache(AppState& app, HDC ref, int w, int h) {
    if (app.ovMemDC && app.ovCacheW == w && app.ovCacheH == h) return;
    // RAII: старые ресурсы очистятся автоматически при назначении новых
    app.ovMemDC.reset(CreateCompatibleDC(ref));
    if (!app.ovMemDC) return;
    app.ovBmp.reset(CreateCompatibleBitmap(ref, w, h));
    if (!app.ovBmp) { app.ovMemDC.reset(); return; }
    SelectObject(app.ovMemDC, app.ovBmp);
    app.ovCacheW = w; app.ovCacheH = h;
}

// Рендер обоев пользователя в кэш app.wallBmp (по каждому монитору, режим «Заполнение»).
struct WallCtx { Gdiplus::Graphics* g; Gdiplus::Bitmap* img; };
BOOL CALLBACK WallMonProc(HMONITOR hm, HDC, LPRECT, LPARAM lp) {
    AppState& app = *g_app;
    WallCtx* c = (WallCtx*)lp;
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(hm, &mi)) return TRUE;
    double mx = mi.rcMonitor.left - app.vsX, my = mi.rcMonitor.top - app.vsY;
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

void LoadWallpaper(AppState& app) {
    // RAII: старые ресурсы очистятся автоматически
    app.wallDC.reset();
    app.wallBmp.reset();
    app.wallW = app.wallH = 0;

    wchar_t path[MAX_PATH] = {};
    if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0) || !path[0]) { app.wallPath.clear(); return; }
    Gdiplus::Bitmap img(path);
    if (img.GetLastStatus() != Gdiplus::Ok) { app.wallPath.clear(); return; }

    HDC screen = GetDC(nullptr);
    app.wallDC.reset(CreateCompatibleDC(screen));
    app.wallBmp.reset(CreateCompatibleBitmap(screen, app.vsW, app.vsH));
    ReleaseDC(nullptr, screen);
    if (!app.wallDC || !app.wallBmp) return;
    SelectObject(app.wallDC, app.wallBmp);

    RECT full = { 0, 0, app.vsW, app.vsH };
    ScopedBrush bk(CreateSolidBrush(RGB(16, 18, 23)));   // на случай зазоров между мониторами
    FillRect(app.wallDC, &full, bk);
    {
        Gdiplus::Graphics g(app.wallDC);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        WallCtx ctx = { &g, &img };
        EnumDisplayMonitors(nullptr, nullptr, WallMonProc, (LPARAM)&ctx);
    }
    app.wallW = app.vsW; app.wallH = app.vsH;
    app.wallPath = path;
}

void EnsureBlack(AppState& app) {
    if (app.blackDC) return;
    HDC s = GetDC(nullptr);
    app.blackDC.reset(CreateCompatibleDC(s));
    app.blackBmp.reset(CreateCompatibleBitmap(s, 1, 1));
    ReleaseDC(nullptr, s);
    if (!app.blackDC || !app.blackBmp) return;
    SelectObject(app.blackDC, app.blackBmp);
    SetPixelV(app.blackDC, 0, 0, RGB(0, 0, 0));
}

// Мягкая тень под окно: несколько расширяющихся скруглённых слоёв чёрного с малой
// альфой, накапливающихся в градиент (как тень окна Windows). Превью ложится сверху.
void DrawShadow(AppState& app, HDC hdc, const RECT& r, int radius) {
    EnsureBlack(app);
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 16, 0 };   // ~16 альфы на слой
    for (int i = 7; i >= 1; --i) {
        RECT s = r;
        InflateRect(&s, i, i);
        OffsetRect(&s, 0, i / 2 + 1);
        ScopedRgn rgn(CreateRoundRectRgn(s.left, s.top, s.right, s.bottom,
                                          radius + i, radius + i));
        SelectClipRgn(hdc, rgn);
        AlphaBlend(hdc, s.left, s.top, s.right - s.left, s.bottom - s.top,
                    app.blackDC, 0, 0, 1, 1, bf);
    }
    SelectClipRgn(hdc, nullptr);
}

void DrawOverview(AppState& app, HDC hdc, const RECT& client) {
    ScopedBrush bg(CreateSolidBrush(RGB(0, 0, 0)));
    FillRect(hdc, &client, bg);
    if (app.wallBmp) {
        // Обои пользователя, затемнённые тем сильнее, чем дальше отдалили.
        double dim = (1.0 - app.zoom) / (1.0 - ZOOM_MIN);
        if (dim < 0) dim = 0; if (dim > 1) dim = 1;
        BYTE a = (BYTE)((1.0 - dim * 0.7) * 255.0);   // не темнее ~70%
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, a, 0 };
        AlphaBlend(hdc, 0, 0, client.right, client.bottom,
                   app.wallDC, 0, 0, app.wallW, app.wallH, bf);
    }

    // Тени под окна (без рамок) — превью с закруглениями Win11 лягут поверх.
    int radius = (int)(8.0 * app.zoom + 0.5);
    if (radius < 2) radius = 2;
    for (auto& w : app.wins) {
        RECT vw = VisWorld(w);
        RECT d = { (LONG)llround(OvClientX(app, vw.left)),  (LONG)llround(OvClientY(app, vw.top)),
                   (LONG)llround(OvClientX(app, vw.right)), (LONG)llround(OvClientY(app, vw.bottom)) };
        if (d.right <= d.left || d.bottom <= d.top) continue;
        DrawShadow(app, hdc, d, radius);
    }

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(150, 160, 175));
    wchar_t info[96];
    wsprintfW(info, L"Обзор  %d%%   колесо — зум • клик — приблизить • СКМ — двигать", (int)(app.zoom * 100));
    TextOutW(hdc, 24, 20, info, lstrlenW(info));
}

LRESULT CALLBACK OvProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    AppState& app = *g_app;
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        EnsureOvCache(app, hdc, rc.right, rc.bottom);
        DrawOverview(app, app.ovMemDC, rc);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, app.ovMemDC, 0, 0, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        double cx = (short)LOWORD(lp), cy = (short)HIWORD(lp);
        // верхнее окно под курсором: идём по z-порядку сверху вниз, берём первое попавшее
        app.ovDragHwnd = nullptr;
        for (int zi = (int)app.ovZ.size() - 1; zi >= 0; --zi) {
            HWND h = app.ovZ[zi];
            for (auto& w : app.wins) {
                if (w.hwnd != h) continue;
                RECT vw = VisWorld(w);
                double l = OvClientX(app, vw.left),  t = OvClientY(app, vw.top);
                double r = OvClientX(app, vw.right), b = OvClientY(app, vw.bottom);
                if (cx >= l && cx <= r && cy >= t && cy <= b) {
                    app.ovDragHwnd = h;
                    break;
                }
            }
            if (app.ovDragHwnd) break;
        }
        app.ovDragLast.x = (LONG)cx; app.ovDragLast.y = (LONG)cy;
        app.ovDragMoved = false;
        SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (GetCapture() != hwnd) return 0;
        double cx = (short)LOWORD(lp), cy = (short)HIWORD(lp);
        double ddx = cx - app.ovDragLast.x, ddy = cy - app.ovDragLast.y;
        if (!app.ovDragMoved && (fabs(ddx) > 3 || fabs(ddy) > 3)) app.ovDragMoved = true;
        if (app.ovDragMoved) {
            if (app.ovDragHwnd) {
                // двигаем окно: мировой сдвиг = клиентский / масштаб
                LONG wdx = (LONG)llround(ddx / app.zoom), wdy = (LONG)llround(ddy / app.zoom);
                for (auto& w : app.wins) if (w.hwnd == app.ovDragHwnd) {
                    w.world.left += wdx; w.world.right += wdx;
                    w.world.top  += wdy; w.world.bottom += wdy;
                    break;
                }
            } else {
                app.axCX += ddx; app.axCY += ddy;   // тащим фон => панорама обзора
            }
        }
        app.ovDragLast.x = (LONG)cx; app.ovDragLast.y = (LONG)cy;
        return 0;
    }
    case WM_LBUTTONUP: {
        bool moved = app.ovDragMoved;
        if (GetCapture() == hwnd) ReleaseCapture();
        app.ovDragHwnd = nullptr; app.ovDragMoved = false;
        if (!moved) {
            // простой клик => приблизить точку и вернуться в 1:1
            double cx = (short)LOWORD(lp), cy = (short)HIWORD(lp);
            double wx = app.axWX + (cx - app.axCX) / app.zoom;
            double wy = app.axWY + (cy - app.axCY) / app.zoom;
            app.axWX = wx; app.axCX = app.vsW / 2.0;
            app.axWY = wy; app.axCY = app.vsH / 2.0;
            app.zoomTarget = 1.0;
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}



