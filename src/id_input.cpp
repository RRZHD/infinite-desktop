// Ввод: зум колесом, hook СКМ/колеса/тачпада
// Часть InfiniteDesktop. Компилируется как единый модуль через main.cpp.

#pragma once
#include "id_common.h"

// ---------- Зум колесом (вход в обзор) ----------
bool IsDesktopClass(HWND h) {
    if (!h) return true;
    if (h == g_ov) return true;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    return !lstrcmpW(cls, L"WorkerW") || !lstrcmpW(cls, L"Progman") ||
           !lstrcmpW(cls, L"SHELLDLL_DefView") || !lstrcmpW(cls, L"SysListView32");
}

// Меняет масштаб в `factor` раз с привязкой к точке pt (экранные координаты).
void DoZoomFactor(double factor, POINT pt) {
    double cx = pt.x - g_vsX;       // клиентские координаты окна обзора
    double cy = pt.y - g_vsY;
    if (!g_overview && g_zoom >= 0.999) {
        // вход в обзор: якорь так, чтобы при z=1 ТОЧНО совпасть с реальными окнами.
        // Реальные окна ставятся по llround(камеры) — берём её же, иначе на входе/
        // выходе превью прыгают на ~1px.
        g_axCX = cx; g_axCY = cy;
        g_axWX = cx + g_vsX + (double)llround(g_camX);
        g_axWY = cy + g_vsY + (double)llround(g_camY);
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
void DoZoom(int delta, POINT pt) {
    DoZoomFactor(delta > 0 ? ZOOM_STEP : 1.0 / ZOOM_STEP, pt);
}

// Панорама холста двухпальцевым скроллом (delta колеса/гориз.колеса).
// Холст следует за пальцами; вертикаль и горизонталь — независимо.
void PanWheel(double dxDelta, double dyDelta) {
    if (g_overview) {
        g_axCX -= dxDelta * TP_WHEEL;
        g_axCY += dyDelta * TP_WHEEL;
    } else {
        g_targetX += dxDelta * TP_WHEEL;  // плавно через easing
        g_targetY -= dyDelta * TP_WHEEL;
    }
}

// ---------- Глобальный hook СКМ/колеса ----------
LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp) {
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
            if (g_dragging) return 1;   // во время панорамы СКМ колесо не зумит
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
            if (g_dragging) return 1;   // во время панорамы СКМ колесо не зумит
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


