// Тени окон в обзоре и плавной панораме — такие же, как рисует Windows.
// Часть InfiniteDesktop.
//
// Превью окна (DWM-thumbnail) — это само окно без тени: тень Windows рисует отдельно.
// Раньше тень под превью рисовалась Direct2D — узкая, с маленьким радиусом скругления
// (её углы торчали из-под скруглённых углов окна), и только на обоях: превью всегда лежат
// поверх всего, что окно обзора рисует само, поэтому на соседние окна тень не падала.
//
// Здесь тень — тоже превью. Для каждого окна заводится скрытое вспомогательное окно с
// полупрозрачной картинкой тени; его превью регистрируется прямо перед превью самого окна
// и потому лежит под ним, но над окнами ниже — как настоящая тень.
//
// Форма тени не подбиралась на глаз: она снята с экрана (tests.exe --shadow-calib) —
// профили затемнения белого фона по четырём сторонам окна Windows 11 при 100 % масштабе.
// Профилей четыре: тень активного окна больше, чем неактивного, а у окна с тёмным
// заголовком (тёмная тема приложения) она примерно вдвое сильнее, чем у светлого.
// Модель α(x,y) = A·hx(x)·hy(y) воспроизводит снятую тень вдоль сторон точно, у углов —
// с ошибкой до 2 % яркости у светлых окон и до 5 % у тёмных.

#include "id_common.h"

namespace {

// Затемнение ×255 на расстоянии d = 1, 2, 3… px от видимой рамки окна наружу.
const BYTE SIDE_INACTIVE[] = { 28,26,24,22,20,18,16,13,13,11,10,9,7,7,7,5,5,5,4,4,4,3,3,3,3,3,2,2,2,2,2,1,1,1,1,1,1,1,1,1 };
const BYTE TOP_INACTIVE[]  = { 16,14,12,11,9,6,6,5,4,3,2,2,1 };
const BYTE BOT_INACTIVE[]  = { 56,54,52,51,48,46,45,44,42,40,39,37,36,36,34,34,33,31,31,30,30,29,28,28,27,26,25,24,24,23,22,21,21,20,
                               19,18,17,17,16,15,14,14,13,12,12,11,10,10,9,8,8,7,7,6,6,5,5,5,4,4,4,3,3,3,2,2,2,2,2,1,1,1,1,1,1,1,1 };
const BYTE SIDE_ACTIVE[]   = { 41,39,38,36,33,32,30,27,26,24,22,21,19,18,16,15,14,13,11,11,9,8,8,6,6,6,6,4,4,4,3,3,2,2,1,1,1,1,1,1,1,1 };
const BYTE TOP_ACTIVE[]    = { 27,25,24,22,20,18,17,16,14,12,11,10,9,8,7,6,5,5,4,3,3,3,2,2,2,1,1,1,1,1 };
const BYTE BOT_ACTIVE[]    = { 78,76,75,74,73,70,69,68,66,65,63,62,60,58,57,55,54,52,51,50,48,46,45,44,42,41,39,38,37,35,34,32,31,30,
                               28,27,26,25,23,22,21,20,19,18,17,16,15,14,13,12,12,11,10,9,9,8,7,7,6,6,5,5,4,4,4,3,3,3,2,2,2,2,1,1,1,1,1,1,1 };

// Окно с тёмным заголовком (DWMWA_USE_IMMERSIVE_DARK_MODE): та же форма, но сильнее.
const BYTE SIDE_DARK_INACTIVE[] = { 63,58,53,48,42,38,33,29,25,23,20,18,16,14,13,11,11,9,8,8,7,7,6,6,5,5,5,4,4,4,3,3,3,2,2,2,2,1,1,1,1,1,1,1 };
const BYTE TOP_DARK_INACTIVE[]  = { 39,34,29,24,20,17,13,11,8,7,5,3,3,2,2 };
const BYTE BOT_DARK_INACTIVE[]  = { 111,108,104,99,96,92,88,85,81,78,76,74,70,69,68,66,64,62,61,59,58,57,55,54,52,51,49,48,46,45,43,42,40,38,
                                    37,35,34,32,31,29,28,26,25,24,22,21,20,19,17,16,15,14,13,12,11,10,10,9,8,8,7,6,6,5,5,4,4,4,3,3,2,2,2,2,1,1,1,1,1,1 };
const BYTE SIDE_DARK_ACTIVE[]   = { 91,86,82,79,73,70,66,62,57,53,49,46,42,38,36,33,30,27,25,22,21,18,16,15,13,13,11,10,9,8,7,6,5,5,4,3,2,2,2,2,1,1,1,1,1 };
const BYTE TOP_DARK_ACTIVE[]    = { 64,60,56,52,48,44,41,38,35,30,28,25,23,20,17,15,14,12,10,9,7,7,6,5,4,3,3,2,2,1,1,1 };
const BYTE BOT_DARK_ACTIVE[]    = { 151,149,146,144,141,139,136,134,131,128,125,122,119,117,114,110,108,105,102,99,96,93,90,87,85,81,78,76,73,70,
                                    67,65,63,60,57,55,52,49,47,45,42,40,38,36,34,32,30,28,27,25,23,22,20,19,17,16,15,14,13,12,11,10,9,8,7,7,6,5,5,
                                    4,4,3,3,3,2,2,2,1,1,1,1 };

struct Profile { const BYTE* side; int nSide; const BYTE* top; int nTop; const BYTE* bot; int nBot; };
#define PROFILE(s, t, b) { s, (int)sizeof(s), t, (int)sizeof(t), b, (int)sizeof(b) }
const Profile PROFILES[4] = {       // индекс: (тёмное ? 2 : 0) + (активное ? 1 : 0)
    PROFILE(SIDE_INACTIVE, TOP_INACTIVE, BOT_INACTIVE),
    PROFILE(SIDE_ACTIVE, TOP_ACTIVE, BOT_ACTIVE),
    PROFILE(SIDE_DARK_INACTIVE, TOP_DARK_INACTIVE, BOT_DARK_INACTIVE),
    PROFILE(SIDE_DARK_ACTIVE, TOP_DARK_ACTIVE, BOT_DARK_ACTIVE),
};
#undef PROFILE
const Profile& ProfileFor(bool active, bool dark) { return PROFILES[(dark ? 2 : 0) + (active ? 1 : 0)]; }

// Значение профиля на расстоянии d (в пикселях профиля, d = 1 — первый пиксель снаружи),
// с линейной интерполяцией; за концом таблицы — ноль.
double At(const BYTE* t, int n, double d) {
    double i = d - 1.0;
    if (i < 0) i = 0;
    int i0 = (int)i;
    if (i0 >= n) return 0.0;
    double a = t[i0], b = (i0 + 1 < n) ? t[i0 + 1] : 0.0;
    return a + (b - a) * (i - i0);
}

// Система показывает превью только достаточно большого окна-источника: у́же ~200 px или
// ниже ~80 px оно обрезается или не рисуется вовсе (проверено пробой). Меньшие тени
// дополняются прозрачными полями.
const int MIN_SRC_W = 220, MIN_SRC_H = 110;
const int SRC_X = -16000, SRC_Y = -16000;      // где лежат окна-источники: за пределами любого экрана

const wchar_t* ShadowClass() {
    static bool registered = false;
    static const wchar_t* name = L"InfiniteDesktopShadow";
    if (!registered) {
        WNDCLASSW c = {};
        c.lpfnWndProc = DefWindowProcW;
        c.hInstance = GetModuleHandleW(nullptr);
        c.lpszClassName = name;
        RegisterClassW(&c);
        registered = true;
    }
    return name;
}

// Есть ли у окна системная тень: её рисует DWM вместе с рамкой, и у развёрнутого окна её нет.
bool HasNativeShadow(HWND h) {
    if (IsZoomed(h)) return false;
    BOOL nc = FALSE;
    return SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_NCRENDERING_ENABLED, &nc, sizeof(nc))) && nc;
}

// У окна тёмный заголовок? От этого зависит, какую тень ему рисует Windows. Атрибут
// читается и у чужих окон; на Windows 10, где его нет, тень считаем светлой.
bool IsDarkFrame(HWND h) {
    BOOL dark = FALSE;
    return SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark))) && dark;
}

// Рамка окна в состоянии «активна»? Большую тень Windows рисует именно таким окнам, и
// это не то же самое, что «окно на переднем плане»: приложения со своей рамкой (Electron,
// Chromium) перехватывают активацию рамки, система считает её неактивной и рисует таким
// окнам тень неактивного окна, даже когда в них работают.
bool IsFrameActive(HWND h) {
    WINDOWINFO wi = { sizeof(wi) };
    return GetWindowInfo(h, &wi) && (wi.dwWindowStatus & WS_ACTIVECAPTION) != 0;
}

}   // namespace

// Построить картинку тени для окна с видимой рамкой W×H (active — окно активно, dark — у
// него тёмный заголовок). Для больших окон картинка
// хранится уменьшенной в k раз, превью её растягивает; pad — на сколько тень выступает за
// рамку окна с каждой стороны (в пикселях экрана).
// px — BGRA с предумноженной альфой, чёрный цвет: pixel = alpha << 24.
void BuildShadowBitmap(int W, int H, bool active, bool dark, double scale, std::vector<DWORD>& px,
                       int& srcW, int& srcH, RECT& pad) {
    const Profile& P = ProfileFor(active, dark);
    if (scale < 1.0) scale = 1.0;
    const int padL = (int)ceil(P.nSide * scale), padT = (int)ceil(P.nTop * scale), padB = (int)ceil(P.nBot * scale);
    const int fullW = W + 2 * padL, fullH = H + padT + padB;
    // Превью растягивается без сглаживания, так что уменьшение в k раз — это ступеньки по
    // k пикселей на градиенте. Небольшие окна храним 1:1, уменьшаем только большие, чтобы
    // тень каждого окна не занимала мегабайты.
    int k = 1;
    while (k < 4 && (long long)(fullW / k) * (fullH / k) > 400000) ++k;
    k = min(k, min(fullW / MIN_SRC_W, fullH / MIN_SRC_H));
    if (k < 1) k = 1;
    srcW = max(MIN_SRC_W, (fullW + k - 1) / k);
    srcH = max(MIN_SRC_H, (fullH + k - 1) / k);
    pad = { padL, padT, srcW * k - W - padL, srcH * k - H - padT };

    const double A255 = 2.0 * P.side[0];        // у размытого прямоугольника на кромке ровно половина плато
    // Множители по столбцам и строкам (центры пикселей уменьшенной картинки → экран).
    std::vector<double> hx(srcW), hy(srcH);
    for (int i = 0; i < srcW; ++i) {
        double x = (i + 0.5) * k - padL;         // относительно левой кромки рамки
        double d;
        if (x < 0)       d = At(P.side, P.nSide, (-x + 0.5) / scale) / A255;                 // слева снаружи
        else if (x > W)  d = At(P.side, P.nSide, (x - W + 0.5) / scale) / A255;              // справа снаружи
        else             d = 1.0 - At(P.side, P.nSide, (min(x, W - x) + 0.5) / scale) / A255;   // внутри: зеркально
        hx[i] = d;
    }
    const double topEdge = P.top[0] / A255;
    for (int j = 0; j < srcH; ++j) {
        double y = (j + 0.5) * k - padT;
        double d;
        if (y < 0)       d = At(P.top, P.nTop, (-y + 0.5) / scale) / A255;                   // сверху снаружи
        else if (y > H)  d = At(P.bot, P.nBot, (y - H + 0.5) / scale) / A255;                // снизу снаружи
        else             d = min(1.0, topEdge + (1.0 - topEdge) * (y / (40.0 * scale)));     // внутри: тень смещена вниз
        hy[j] = min(1.0, d);
    }
    // Под самим окном тень не нужна — её закрывает превью окна; оставляем только полосу у
    // кромок (её видно в скруглённых углах). Так тень не темнит полупрозрачные окна.
    const double band = 14.0 * scale;
    px.assign((size_t)srcW * srcH, 0);
    for (int j = 0; j < srcH; ++j) {
        double y = (j + 0.5) * k - padT;
        bool rowInside = (y > band && y < H - band);
        for (int i = 0; i < srcW; ++i) {
            if (rowInside) {
                double x = (i + 0.5) * k - padL;
                if (x > band && x < W - band) continue;
            }
            int a = (int)(A255 * hx[i] * hy[j] + 0.5);
            if (a > 0) px[(size_t)j * srcW + i] = (DWORD)min(255, a) << 24;
        }
    }
}

// Та же тень как набор вложенных контуров равного затемнения. Каждый слой — скруглённый
// прямоугольник, выступающий за рамку окна настолько, насколько тень на этом уровне
// тянется в каждую сторону; слои, наложенные друг на друга, дают те же профили, что сняты
// с экрана. Нужна там, где превью-тень не работает: окно обзора с живыми обоями прозрачно,
// и на прозрачном месте система полупрозрачное превью не показывает (см. RenderOverview).
static std::vector<ShadowLayer> MakeLayers(const Profile& P) {
    std::vector<ShadowLayer> layers;
    const int step = 2;                          // шаг уровня затемнения, в 1/255
    double prev = 0.0;
    for (int level = step; level <= P.bot[0]; level += step) {     // самое тёмное место — вплотную под окном
        auto reach = [&](const BYTE* t, int n) { int d = 0; while (d < n && t[d] >= level) ++d; return (float)d; };
        double c = level / 255.0;
        ShadowLayer l;
        l.alpha = (float)((c - prev) / (1.0 - prev));   // чтобы после наложения суммарное затемнение стало ровно c
        prev = c;
        l.dl = l.dr = reach(P.side, P.nSide);
        l.dt = reach(P.top, P.nTop);
        l.db = reach(P.bot, P.nBot);
        layers.push_back(l);
    }
    return layers;
}

const std::vector<ShadowLayer>& ShadowLayers(bool active, bool dark) {
    static const std::vector<ShadowLayer> layers[4] = { MakeLayers(PROFILES[0]), MakeLayers(PROFILES[1]),
                                                        MakeLayers(PROFILES[2]), MakeLayers(PROFILES[3]) };
    return layers[(dark ? 2 : 0) + (active ? 1 : 0)];
}

// Завести или обновить окно-источник тени для окна w. Пересобирается, только если
// изменились размер окна, его активность или масштаб монитора.
void EnsureShadow(AppState& app, TrackedWin& w) {
    if (!IsWindow(w.hwnd) || !HasNativeShadow(w.hwnd)) { w.thumbShadow.reset(); w.shadowSrc.reset(); return; }
    RECT vw = VisWorld(w);
    int W = vw.right - vw.left, H = vw.bottom - vw.top;
    if (W < 1 || H < 1) { w.thumbShadow.reset(); w.shadowSrc.reset(); return; }
    bool active = IsFrameActive(w.hwnd), dark = IsDarkFrame(w.hwnd);
    UINT dpi = GetDpiForWindow(w.hwnd);
    if (dpi < 96) dpi = 96;
    if (w.shadowSrc && w.shadowW == W && w.shadowH == H && w.shadowActive == active &&
        w.shadowDark == dark && w.shadowDpi == dpi) return;

    std::vector<DWORD> px;
    int sw = 0, sh = 0;
    BuildShadowBitmap(W, H, active, dark, dpi / 96.0, px, sw, sh, w.shadowPad);

    if (!w.shadowSrc) {
        // Скрытое от пользователя окно: за экраном, вне Alt+Tab, не принимает ввод и вдобавок
        // «занавешено» (cloak) — DWM при этом его рисует, и превью с него работает.
        w.shadowSrc.reset(CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                                          ShadowClass(), L"", WS_POPUP, SRC_X, SRC_Y, sw, sh,
                                          nullptr, nullptr, GetModuleHandleW(nullptr), nullptr));
        if (!w.shadowSrc) return;
        BOOL cloak = TRUE;
        DwmSetWindowAttribute(w.shadowSrc, DWMWA_CLOAK, &cloak, sizeof(cloak));
    }
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = sw; bi.bmiHeader.biHeight = -sh;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    HDC sdc = GetDC(nullptr);
    ScopedDC mdc(CreateCompatibleDC(sdc));
    void* bits = nullptr;
    ScopedBitmap dib(CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0));
    bool ok = false;
    if (mdc && dib && bits) {
        memcpy(bits, px.data(), px.size() * sizeof(DWORD));
        HGDIOBJ old = SelectObject(mdc, dib);
        POINT zero = { 0, 0 }, pos = { SRC_X, SRC_Y };
        SIZE size = { sw, sh };
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        ok = UpdateLayeredWindow(w.shadowSrc, sdc, &pos, &size, mdc, &zero, 0, &bf, ULW_ALPHA) != FALSE;
        SelectObject(mdc, old);
    }
    ReleaseDC(nullptr, sdc);
    if (!ok) { w.thumbShadow.reset(); w.shadowSrc.reset(); return; }
    ShowWindow(w.shadowSrc, SW_SHOWNOACTIVATE);
    w.shadowW = W; w.shadowH = H; w.shadowActive = active; w.shadowDark = dark; w.shadowDpi = dpi;
}

// Активное окно сменилось (в обзоре — кликом внутрь окна): у активного окна тень Windows
// заметно больше. Перерисовываем картинки тех теней, чьё состояние изменилось; превью
// обновятся сами — они живые.
void RefreshShadowStates(AppState& app) {
    for (auto& w : app.wins)
        if (w.thumbShadow && w.shadowActive != IsFrameActive(w.hwnd)) EnsureShadow(app, w);
}
