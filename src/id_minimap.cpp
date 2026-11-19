// Миникарта: преобразование мир<->карта, отрисовка, шестерёнка
// Часть InfiniteDesktop.

#include "id_common.h"

// ---------- Преобразование мир <-> миникарта ----------
// Границы мира берём по окнам (стабильны при панораме) + поле.
static void ComputeWorldBounds(AppState& app, RECT& out) {
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

    int pad = (int)lround(10 * app.uiScale);
    double cw = client.right - 2 * pad;
    double ch = client.bottom - 2 * pad;
    if (cw < 1) cw = 1; if (ch < 1) ch = 1;
    double s = min(cw / wW, ch / wH);

    app.xform.s  = s;
    app.xform.ox = pad + (LONG)((cw - wW * s) / 2);
    app.xform.oy = pad + (LONG)((ch - wH * s) / 2);
    app.xform.bounds = b;
}

static RECT WorldToClient(AppState& app, const RECT& w) {
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

// Обновить миникарту под текущую камеру: пересчёт отображения, живые превью,
// запрос WM_PAINT. app.hudVisible проверяет вызывающий (часть путей обновляет
// карту и в фоне, без троттлинга).
void RefreshHud(AppState& app) {
    if (!app.hud) return;
    RECT rc; GetClientRect(app.hud, &rc);
    ComputeXform(app, rc);
    UpdateThumbs(app, rc);
    InvalidateRect(app.hud, nullptr, FALSE);
}

// Кнопка-шестерёнка в правом-верхнем углу миникарты
RECT GearRect(const AppState& app, const RECT& client) {
    const float s = app.uiScale;
    return { client.right - (LONG)lround(28 * s), (LONG)lround(4 * s),
             client.right - (LONG)lround(6 * s),  (LONG)lround(26 * s) };
}

// ---------- Отрисовка миникарты (фон, бордюры, вьюпорт) ----------
// DWM рисует превью ПОВЕРХ этого GDI-содержимого, поэтому бордюры окон
// делаем чуть шире рамки превью, а рамку вьюпорта — поверх пустых зон.
// Цвета — из темы (акцент и нейтрали Windows). При смене темы ResetMapGdi сбрасывает
// кэш, и объекты пересоздаются уже в новых цветах.
static void EnsureMapGdi(AppState& app) {
    const Theme& T = app.theme;
    const float s = app.uiScale;                       // толщины и шрифты — по DPI монитора
    const int thin = max(1, (int)lround(1 * s)), thick = max(1, (int)lround(2 * s));
    if (!app.framePen) app.framePen.reset(CreatePen(PS_SOLID, thin, UiMix(T.accent, T.bg, 0.30f)));
    if (!app.fillBr)   app.fillBr.reset(CreateSolidBrush(UiMix(T.accent, T.bg, 0.55f)));
    if (!app.mapBg)    app.mapBg.reset(CreateSolidBrush(T.bg));
    if (!app.pinPen)   app.pinPen.reset(CreatePen(PS_SOLID, thick, T.warning));
    if (!app.vpPen)    app.vpPen.reset(CreatePen(PS_SOLID, thick, T.accent));
    if (!app.hudFont || app.hudFontScale != s) {
        app.hudFontScale = s;
        app.hudFont.reset(CreateFontW(-(int)lround(15 * s), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text"));
        app.hudGearFont.reset(CreateFontW((int)lround(20 * s), 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI Symbol"));
    }
}
void ResetMapGdi(AppState& app) {
    app.framePen.reset(); app.fillBr.reset(); app.mapBg.reset(); app.pinPen.reset(); app.vpPen.reset();
    app.hudFont.reset(); app.hudGearFont.reset();
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
    HGDIOBJ oldFont = SelectObject(hdc, app.hudFont);
    SetTextColor(hdc, app.theme.muted);
    const wchar_t* tip = Tr(S_MAP_TIP);
    TextOutW(hdc, (int)lround(12 * app.uiScale), client.bottom - (int)lround(22 * app.uiScale), tip, lstrlenW(tip));
    SelectObject(hdc, oldFont);

    // шестерёнка (настройки горячих клавиш)
    RECT gr = GearRect(app, client);
    HGDIOBJ of = SelectObject(hdc, app.hudGearFont);
    SetTextColor(hdc, app.theme.muted);
    DrawTextW(hdc, L"⚙", -1, &gr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, of);
}



