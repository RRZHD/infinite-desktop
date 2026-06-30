// Миникарта: преобразование мир<->карта, отрисовка, шестерёнка
// Часть InfiniteDesktop. Компилируется как единый модуль через main.cpp.

#include "id_common.h"

// ---------- Преобразование мир <-> миникарта ----------
// Границы мира берём по окнам (стабильны при панораме) + поле.
void ComputeWorldBounds(RECT& out) {
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

void ComputeXform(const RECT& client) {
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

RECT WorldToClient(const RECT& w) {
    RECT r;
    r.left   = g_xform.ox + (LONG)((w.left   - g_xform.bounds.left) * g_xform.s);
    r.top    = g_xform.oy + (LONG)((w.top    - g_xform.bounds.top)  * g_xform.s);
    r.right  = g_xform.ox + (LONG)((w.right  - g_xform.bounds.left) * g_xform.s);
    r.bottom = g_xform.oy + (LONG)((w.bottom - g_xform.bounds.top)  * g_xform.s);
    return r;
}

void MapToWorld(int mx, int my, double& wx, double& wy) {
    wx = g_xform.bounds.left + (mx - g_xform.ox) / g_xform.s;
    wy = g_xform.bounds.top  + (my - g_xform.oy) / g_xform.s;
}

// Обновить позиции живых превью под текущую раскладку/камеру.
void UpdateThumbs(const RECT& client) {
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

HFONT UiFont();   // системный шрифт Win11 (определён ниже)

// Кнопка-шестерёнка в правом-верхнем углу миникарты
RECT GearRect(const RECT& client) {
    return { client.right - 28, 4, client.right - 6, 26 };
}

// ---------- Отрисовка миникарты (фон, бордюры, вьюпорт) ----------
// DWM рисует превью ПОВЕРХ этого GDI-содержимого, поэтому бордюры окон
// делаем чуть шире рамки превью, а рамку вьюпорта — поверх пустых зон.
static HPEN g_framePen = nullptr, g_pinPen = nullptr, g_vpPen = nullptr;
static HBRUSH g_fillBr = nullptr;
static void EnsureMapGdi() {
    if (!g_framePen) g_framePen = CreatePen(PS_SOLID, 1, RGB(90, 110, 140));
    if (!g_fillBr)   g_fillBr   = CreateSolidBrush(RGB(70, 130, 200));
    if (!g_pinPen)   g_pinPen   = CreatePen(PS_SOLID, 2, RGB(255, 170, 60));
    if (!g_vpPen)    g_vpPen    = CreatePen(PS_SOLID, 2, RGB(255, 200, 60));
}
static void FreeMapGdiInternal() {
    if (g_framePen) { DeleteObject(g_framePen); g_framePen = nullptr; }
    if (g_fillBr)   { DeleteObject(g_fillBr);   g_fillBr   = nullptr; }
    if (g_pinPen)   { DeleteObject(g_pinPen);   g_pinPen   = nullptr; }
    if (g_vpPen)    { DeleteObject(g_vpPen);    g_vpPen    = nullptr; }
}
void FreeMapGdi() { FreeMapGdiInternal(); }

void DrawMinimap(HDC hdc, const RECT& client) {
    HBRUSH bg = CreateSolidBrush(RGB(20, 22, 28));
    FillRect(hdc, &client, bg);
    DeleteObject(bg);

    ComputeXform(client);

    EnsureMapGdi();

    HGDIOBJ oldPen = SelectObject(hdc, g_framePen);
    HGDIOBJ oldBr  = SelectObject(hdc, g_fillBr);
    for (auto& w : g_wins) {
        RECT m = WorldToClient(w.world);
        InflateRect(&m, 1, 1);
        Rectangle(hdc, m.left, m.top, m.right, m.bottom);
    }

    // закреплённые окна — янтарной рамкой
    SelectObject(hdc, g_pinPen);
    SelectObject(hdc, GetStockObject(NULL_BRUSH));
    for (auto& w : g_wins) if (w.pinned) {
        RECT m = WorldToClient(w.world);
        InflateRect(&m, 1, 1);
        Rectangle(hdc, m.left, m.top, m.right, m.bottom);
    }

    // рамка вьюпорта (видимая область всех мониторов) — пустой кистью
    SelectObject(hdc, GetStockObject(NULL_BRUSH));
    LONG cx = (LONG)llround(g_camX), cy = (LONG)llround(g_camY);
    RECT vpWorld = { g_vsX + cx, g_vsY + cy, g_vsX + g_vsW + cx, g_vsY + g_vsH + cy };
    RECT vp = WorldToClient(vpWorld);
    SelectObject(hdc, g_vpPen);
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
    static HFONT gearFont = CreateFontW(20, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI Symbol");
    RECT gr = GearRect(client);
    HGDIOBJ of = SelectObject(hdc, gearFont);
    SetTextColor(hdc, RGB(180, 190, 205));
    DrawTextW(hdc, L"⚙", -1, &gr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, of);
}


