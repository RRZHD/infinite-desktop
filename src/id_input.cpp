// Ввод: зум колесом, hook СКМ/колеса/тачпада
// Часть InfiniteDesktop. Компилируется как единый модуль через main.cpp.

#include "id_common.h"

// ---------- Зум колесом (вход в обзор) ----------
bool IsDesktopClass(AppState& app, HWND h) {
    if (!h) return true;
    if (h == app.ov) return true;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    return !lstrcmpW(cls, L"WorkerW") || !lstrcmpW(cls, L"Progman") ||
           !lstrcmpW(cls, L"SHELLDLL_DefView") || !lstrcmpW(cls, L"SysListView32");
}

// Меняет масштаб в `factor` раз с привязкой к точке pt (экранные координаты).
void DoZoomFactor(AppState& app, double factor, POINT pt) {
    double cx = pt.x - app.vsX;       // клиентские координаты окна обзора
    double cy = pt.y - app.vsY;
    if (!app.overview && app.zoom >= 0.999) {
        // вход в обзор: якорь так, чтобы при z=1 ТОЧНО совпасть с реальными окнами.
        // Реальные окна ставятся по llround(камеры) — берём её же, иначе на входе/
        // выходе превью прыгают на ~1px.
        app.axCX = cx; app.axCY = cy;
        app.axWX = cx + app.vsX + (double)llround(app.camX);
        app.axWY = cy + app.vsY + (double)llround(app.camY);
    }
    double wx = app.axWX + (cx - app.axCX) / app.zoom;   // мир под точкой
    double wy = app.axWY + (cy - app.axCY) / app.zoom;
    app.zoomTarget *= factor;
    if (app.zoomTarget > 1.0)      app.zoomTarget = 1.0;
    if (app.zoomTarget < ZOOM_MIN) app.zoomTarget = ZOOM_MIN;
    app.axWX = wx; app.axCX = cx;                        // переякорить на точку
    app.axWY = wy; app.axCY = cy;
}

// Тик колеса: один шаг зума к/от курсора.
void DoZoom(AppState& app, int delta, POINT pt) {
    double ticks = (double)delta / (double)WHEEL_DELTA;
    DoZoomFactor(app, pow(ZOOM_STEP, ticks), pt);
}

// Панорама холста двухпальцевым скроллом (delta колеса/гориз.колеса).
// Холст следует за пальцами; вертикаль и горизонталь — независимо.
void PanWheel(AppState& app, double dxDelta, double dyDelta) {
    if (app.overview) {
        app.axCX -= dxDelta * TP_WHEEL;
        app.axCY += dyDelta * TP_WHEEL;
    } else {
        app.targetX += dxDelta * TP_WHEEL;  // плавно через easing
        app.targetY -= dyDelta * TP_WHEEL;
    }
}

// ---------- Глобальный hook СКМ/колеса ----------
LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp) {
    AppState& app = *g_app;
    if (code == HC_ACTION) {
        MSLLHOOKSTRUCT* m = (MSLLHOOKSTRUCT*)lp;
        switch (wp) {
        case WM_MBUTTONDOWN: {
            bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            // Панораму берём только над пустым столом / в обзоре / по Ctrl+СКМ.
            // Над окном приложения (Blender, браузер) СКМ уходит в само приложение.
            if (app.overview || ctrl || IsDesktopClass(app, WindowFromPoint(m->pt))) {
                app.dragDeltaX.store(0, std::memory_order_relaxed);
                app.dragDeltaY.store(0, std::memory_order_relaxed);
                app.dragging.store(true, std::memory_order_release);
                app.lastMouse = m->pt;
                return 1;             // поглощаем только когда сами панорамим
            }
            break;                    // иначе СКМ — в приложение
        }
        case WM_MOUSEMOVE:
            if (app.dragging.load(std::memory_order_acquire)) {
                int dx = m->pt.x - app.lastMouse.x;
                int dy = m->pt.y - app.lastMouse.y;
                app.lastMouse = m->pt;
                app.dragDeltaX.fetch_add(dx, std::memory_order_relaxed);
                app.dragDeltaY.fetch_add(dy, std::memory_order_relaxed);
                // НЕ поглощаем move — иначе системный курсор замирает на месте.
            }
            break;
        case WM_MOUSEWHEEL: {
            if (app.dragging.load(std::memory_order_acquire)) return 1;   // во время панорамы СКМ колесо не зумит
            int delta = (short)HIWORD(m->mouseData);
            bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool onDesk = app.overview || IsDesktopClass(app, WindowFromPoint(m->pt));
            if (!onDesk) break;                              // внутри приложения — обычная прокрутка
            // Колесо мыши отправляет >= WHEEL_DELTA на клик. Мелкая дельта тачпада — панорама.
            bool mouseWheel = (abs(delta) >= WHEEL_DELTA);
            if (ctrl || mouseWheel) { DoZoom(app, delta, m->pt); return 1; }   // зум
            PanWheel(app, 0.0, (double)delta); return 1;          // 2 пальца верт. => панорама
        }
        case WM_MOUSEHWHEEL: {
            if (app.dragging.load(std::memory_order_acquire)) return 1;   // во время панорамы СКМ колесо не зумит
            bool onDesk = app.overview || IsDesktopClass(app, WindowFromPoint(m->pt));
            if (onDesk) { PanWheel(app, (double)(short)HIWORD(m->mouseData), 0.0); return 1; }  // 2 пальца гориз.
            break;
        }
        case WM_MBUTTONUP:
            if (app.dragging.load(std::memory_order_acquire)) {
                app.dragging.store(false, std::memory_order_release);
                return 1;
            }
            break;
        }
    }
    return CallNextHookEx(app.mouseHook, code, wp, lp);
}



