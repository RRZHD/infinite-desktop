// Миникарта: преобразование мир<->карта, отрисовка, шестерёнка
// Часть InfiniteDesktop. Компилируется как единый модуль через main.cpp.

#include "id_common.h"

// ---------- Преобразование мир <-> миникарта ----------
// Границы мира берём по окнам (стабильны при панораме) + поле.
void ComputeWorldBounds(AppState& app, RECT& out) {
    if (app.wins.empty()) {
        LONG cx = (LONG)llround(app.camX), cy = (LONG)llround(app.camY);
        out.left = app.vsX + cx; out.top = app.vsY + cy;
        out.right = out.left + app.vsW; out.bottom = out.top + app.vsH;
    } else {
        out = app.wins[0].world;
        for (auto& w : app.wins) {
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

void ComputeXform(AppState& app, const RECT& client) {
    RECT b; ComputeWorldBounds(app, b);
    double wW = (double)(b.right - b.left);
    double wH = (double)(b.bottom - b.top);
    if (wW < 1) wW = 1; if (wH < 1) wH = 1;

    int pad = 10;
    double cw = client.right - 2 * pad;
    double ch = client.bottom - 2 * pad;
    if (cw < 1) cw = 1; if (ch < 1) ch = 1;
    double s = min(cw / wW, ch / wH);

    app.xform.s  = s;
    app.xform.ox = pad + (LONG)((cw - wW * s) / 2);
    app.xform.oy = pad + (LONG)((ch - wH * s) / 2);
    app.xform.bounds = b;
}

RECT WorldToClient(AppState& app, const RECT& w) {
    RECT r;
    r.left   = app.xform.ox + (LONG)((w.left   - app.xform.bounds.left) * app.xform.s);
    r.top    = app.xform.oy + (LONG)((w.top    - app.xform.bounds.top)  * app.xform.s);
    r.right  = app.xform.ox + (LONG)((w.right  - app.xform.bounds.left) * app.xform.s);
    r.bottom = app.xform.oy + (LONG)((w.bottom - app.xform.bounds.top)  * app.xform.s);
    return r;
}

void MapToWorld(AppState& app, int mx, int my, double& wx, double& wy) {
    wx = app.xform.bounds.left + (mx - app.xform.ox) / app.xform.s;
    wy = app.xform.bounds.top  + (my - app.xform.oy) / app.xform.s;
}

// Обновить позиции живых превью под текущую раскладку/камеру.
void UpdateThumbs(AppState& app, const RECT& client) {
    if (!app.hudVisible || !app.previewsOn) return;
    for (auto& w : app.wins) {
        if (!w.thumb) continue;
        RECT d = WorldToClient(app, w.world);
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

HFONT UiFont();   // системный шрифт Win11 (определён ниже)

// Кнопка-шестерёнка в правом-верхнем углу миникарты
RECT GearRect(const RECT& client) {
    return { client.right - 28, 4, client.right - 6, 26 };
}

// ---------- Отрисовка миникарты (фон, бордюры, вьюпорт) ----------
// DWM рисует превью ПОВЕРХ этого GDI-содержимого, поэтому бордюры окон
// делаем чуть шире рамки превью, а рамку вьюпорта — поверх пустых зон.
static void EnsureMapGdi(AppState& app) {
    if (!app.framePen) app.framePen.reset(CreatePen(PS_SOLID, 1, RGB(90, 110, 140)));
    if (!app.fillBr)   app.fillBr.reset(CreateSolidBrush(RGB(70, 130, 200)));
    if (!app.mapBg)    app.mapBg.reset(CreateSolidBrush(RGB(20, 22, 28)));
    if (!app.pinPen)   app.pinPen.reset(CreatePen(PS_SOLID, 2, RGB(255, 170, 60)));
    if (!app.vpPen)    app.vpPen.reset(CreatePen(PS_SOLID, 2, RGB(255, 200, 60)));
}

// Постоянный задний буфер миникарты. Раньше на каждый WM_PAINT (до 60 раз/с при
// панораме) создавались и уничтожались DC и битмап 420×260 — теперь только при
// смене размера.
void EnsureHudCache(AppState& app, HDC ref, int w, int h) {
    if (app.hudMemDC && app.hudCacheW == w && app.hudCacheH == h) return;
    app.hudMemDC.reset(CreateCompatibleDC(ref));
    if (!app.hudMemDC) return;
    app.hudBmp.reset(CreateCompatibleBitmap(ref, w, h));
    if (!app.hudBmp) { app.hudMemDC.reset(); return; }
    SelectObject(app.hudMemDC, app.hudBmp);
    app.hudCacheW = w; app.hudCacheH = h;
}

void DrawMinimap(AppState& app, HDC hdc, const RECT& client) {
    EnsureMapGdi(app);
    FillRect(hdc, &client, app.mapBg);

    ComputeXform(app, client);

    HGDIOBJ oldPen = SelectObject(hdc, app.framePen);
    HGDIOBJ oldBr  = SelectObject(hdc, app.fillBr);
    for (auto& w : app.wins) {
        RECT m = WorldToClient(app, w.world);
        InflateRect(&m, 1, 1);
        Rectangle(hdc, m.left, m.top, m.right, m.bottom);
    }

    // закреплённые окна — янтарной рамкой
    SelectObject(hdc, app.pinPen);
    SelectObject(hdc, GetStockObject(NULL_BRUSH));
    for (auto& w : app.wins) if (w.pinned) {
        RECT m = WorldToClient(app, w.world);
        InflateRect(&m, 1, 1);
        Rectangle(hdc, m.left, m.top, m.right, m.bottom);
    }

    // рамка вьюпорта (видимая область всех мониторов) — пустой кистью
    SelectObject(hdc, GetStockObject(NULL_BRUSH));
    LONG cx = (LONG)llround(app.camX), cy = (LONG)llround(app.camY);
    RECT vpWorld = { app.vsX + cx, app.vsY + cy, app.vsX + app.vsW + cx, app.vsY + app.vsH + cy };
    RECT vp = WorldToClient(app, vpWorld);
    SelectObject(hdc, app.vpPen);
    Rectangle(hdc, vp.left, vp.top, vp.right, vp.bottom);

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBr);

    SetBkMode(hdc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(hdc, UiFont());
    SetTextColor(hdc, RGB(175, 185, 200));
    const wchar_t* tip = L"ЛКМ — перелёт · ⚙ — настройки";
    TextOutW(hdc, 12, client.bottom - 22, tip, lstrlenW(tip));
    SelectObject(hdc, oldFont);

    // шестерёнка (настройки горячих клавиш)
    static ScopedFont gearFont(CreateFontW(20, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI Symbol"));
    RECT gr = GearRect(client);
    HGDIOBJ of = SelectObject(hdc, gearFont);
    SetTextColor(hdc, RGB(180, 190, 205));
    DrawTextW(hdc, L"⚙", -1, &gr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, of);
}



