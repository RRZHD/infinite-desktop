// Обзор (зум): превью, тени, обои, окно обзора. Отрисовка — Direct2D (GPU).
// Часть InfiniteDesktop.

#include "id_common.h"

using Microsoft::WRL::ComPtr;

static HWND FindLiveWallpaperHost();   // определена ниже, рядом со снимком стола
static void LogWindowChain(const wchar_t* what, HWND h);

// ---------- Полноэкранный обзор (зум через DWM-превью) ----------
// Прямые/обратные преобразования якоря зума (OvClientX/Y, OvWorldX/Y) — inline в id_common.h.

// Видимый мировой прямоугольник окна (без невидимых рамок) — для превью/теней/клика,
// чтобы зазоры в обзоре совпадали с холстом.
RECT VisWorld(const TrackedWin& w) {
    return { w.world.left + w.vm.left,  w.world.top + w.vm.top,
             w.world.right - w.vm.right, w.world.bottom - w.vm.bottom };
}

void ShowOverview(AppState& app) {
    if (app.overview) return;
    // Над каким окном колесо запустило зум — в лог, по разу на окно: если зум сработает
    // там, где должна быть прокрутка, по записи видно, что система отдала под курсором.
    HWND src = app.zoomSrc.exchange(nullptr, std::memory_order_relaxed);
    if (src && app.zoomSrcLogged.size() < 16 &&
        std::find(app.zoomSrcLogged.begin(), app.zoomSrcLogged.end(), src) == app.zoomSrcLogged.end()) {
        app.zoomSrcLogged.push_back(src);
        LogWindowChain(L"zoom started over", src);
    }
    PrepareOvBackground(app);
    app.ovLastZoom = -1;   // форсировать первую перерисовку
    RegisterAllOvThumbs(app);
    app.barP = 0; app.ovLastBarP = -1;
    RegisterOvBars(app);   // панель задач — превью поверх окон, на своём месте
    app.overview = true;   // с этого момента UpdateOverview работает
    UpdateOverview(app);   // места превью — до показа окна: панель не должна мигнуть
    // Окно во весь экран оболочка считает полноэкранным приложением: убирает панель
    // задач и включает «Не беспокоить» — в трее появляется значок, значки сдвигаются.
    // Под обзором этого не было видно, а в живом превью панели стало бы рывком в самом
    // начале. NonRudeHWND говорит оболочке, что окно не полноэкранное приложение; поверх
    // панели оно остаётся и так — оно «поверх всех» и поднято последним.
    SetPropW(app.ov, L"NonRudeHWND", (HANDLE)(INT_PTR)1);
    if (app.ovBack) SetPropW(app.ovBack, L"NonRudeHWND", (HANDLE)(INT_PTR)1);
    SetWindowPos(app.ov, HWND_TOPMOST, app.vsX, app.vsY, app.vsW, app.vsH, SWP_NOACTIVATE);
    ShowWindow(app.ov, SW_SHOWNA);
    // Подложка — строго ПОД окном обзора и после него: появись она раньше, на кадр
    // закрыла бы реальные окна обоями без превью. Сначала возвращаем ей «поверх всех»:
    // плавная панорама держит оба окна в обычном слое.
    if (app.ovLiveBg) {
        SetWindowPos(app.ovBack, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowPos(app.ovBack, app.ov, app.vsX, app.vsY, app.vsW, app.vsH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    app.overview = true;
}

// ---------- Панели у края экрана ----------
// Что считать панелью. Панель задач Windows — по классу окна. Но её вид и место меняют
// моды (Windhawk: высота, положение сверху или сбоку, «плавающая» панель), а вместо неё
// может стоять другая программа (YASB и подобные). Поэтому вторым признаком служит само
// поведение: окно «поверх всех», лежащее в полосе у края монитора, которую рабочая
// область не занимает, — так выглядит любая панель, зарезервировавшая себе место. Вид
// панели знать не нужно вовсе: показывается живое превью настоящего окна.
static bool IsEdgeBar(AppState& app, HWND h) {
    if (h == app.ov || h == app.ovBack || h == app.hud || h == app.settings) return false;
    if (!IsWindowVisible(h) || IsIconic(h)) return false;
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (!lstrcmpW(cls, L"Shell_TrayWnd") || !lstrcmpW(cls, L"Shell_SecondaryTrayWnd")) return true;
    if (!(GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOPMOST) || IsTracked(app, h)) return false;
    RECT r;
    if (!GetWindowRect(h, &r) || r.right - r.left < 8 || r.bottom - r.top < 8) return false;
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONULL), &mi)) return false;
    if (EqualRect(&mi.rcMonitor, &mi.rcWork)) return false;          // на этом мониторе ничего не зарезервировано
    RECT inMon, inWork;
    if (!IntersectRect(&inMon, &r, &mi.rcMonitor)) return false;
    long long area = (long long)(inMon.right - inMon.left) * (inMon.bottom - inMon.top);
    long long work = IntersectRect(&inWork, &r, &mi.rcWork)
                   ? (long long)(inWork.right - inWork.left) * (inWork.bottom - inWork.top) : 0;
    return work * 10 <= area;                                        // в рабочую область заходит не больше десятой части
}

static BOOL CALLBACK CollectBars(HWND h, LPARAM lp) {
    if (IsEdgeBar(*g_app, h)) ((std::vector<HWND>*)lp)->push_back(h);
    return TRUE;
}

// Превью панелей регистрируются ПОСЛЕ превью окон — значит, лежат поверх них, как и
// настоящие панели. Размер приёмника равен размеру источника: превью пиксель в пиксель
// совпадает с панелью, и на масштабе 1:1 подмена незаметна.
void RegisterOvBars(AppState& app) {
    app.ovBars.clear();
    if (!app.ov) return;
    std::vector<HWND> bars;
    EnumWindows(CollectBars, (LPARAM)&bars);                         // сверху вниз
    for (auto it = bars.rbegin(); it != bars.rend(); ++it) {
        HWND h = *it;
        OvBar b;
        if (FAILED(DwmRegisterThumbnail(app.ov, h, b.thumb.put()))) continue;
        RECT wr, vr;
        SIZE src = {};
        if (!GetWindowRect(h, &wr) || FAILED(DwmQueryThumbnailSourceSize(b.thumb, &src)) || src.cx < 1 || src.cy < 1) continue;
        // Источник превью — либо окно целиком, либо его видимая рамка (без невидимых
        // границ): привязываемся к тому прямоугольнику, чей размер совпал.
        RECT r = wr;
        if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &vr, sizeof(vr))) &&
            vr.right - vr.left == src.cx && vr.bottom - vr.top == src.cy) r = vr;
        r.right = r.left + src.cx; r.bottom = r.top + src.cy;
        MONITORINFO mi = { sizeof(mi) };
        if (!GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi)) continue;
        const RECT& m = mi.rcMonitor;
        // Уходит к ближайшему краю своего монитора: горизонтальная панель — вверх или
        // вниз, вертикальная — влево или вправо.
        b.dx = b.dy = 0;
        if (src.cx >= src.cy) b.dy = ((r.top + r.bottom) / 2 < (m.top + m.bottom) / 2) ? -(r.bottom - m.top) : (m.bottom - r.top);
        else                  b.dx = ((r.left + r.right) / 2 < (m.left + m.right) / 2) ? -(r.right - m.left) : (m.right - r.left);
        b.dst  = { r.left - app.vsX, r.top - app.vsY, r.right - app.vsX, r.bottom - app.vsY };
        b.clip = { m.left - app.vsX, m.top - app.vsY, m.right - app.vsX, m.bottom - app.vsY };
        app.ovBars.push_back(std::move(b));
    }
}

// Фон окна обзора — общий для зума и плавной панорамы.
void PrepareOvBackground(AppState& app) {
    // Живые обои (Wallpaper Engine и т.п.): фон обзора — не снимок, а живое DWM-превью
    // окна-хоста, чтобы обои продолжали идти и в зуме. DWM-превью всегда рисуются поверх
    // содержимого своего окна, поэтому в одном окне порядок «обои → тени → окна» не
    // получить; слоёв два: подложка с превью хоста, над ней — окно обзора, прозрачное
    // везде, кроме теней и превью окон (см. CreateOverviewBackdrop).
    app.ovLiveBg = false;
    HWND host = FindLiveWallpaperHost();
    RECT hr;
    if (host && app.ovBack && GetWindowRect(host, &hr) &&
        SUCCEEDED(DwmRegisterThumbnail(app.ovBack, host, app.wallThumb.put()))) {
        app.wallDst = { hr.left - app.vsX, hr.top - app.vsY, hr.right - app.vsX, hr.bottom - app.vsY };
        app.ovLiveBg = true;
    }
    // Обычные обои — снимок/файл; перекодируем (десятки мс) только при реальной смене,
    // иначе вход в зум каждый раз начинался бы с задержки. Живой фон не удался — берём
    // снимок хоста; wallLive в условии — чтобы после закрытия программы обоев один раз
    // вернуться к обычным.
    if (!app.ovLiveBg && (host || app.wallLive || WallpaperFileChanged(app))) LoadWallpaper(app);
    app.lastWallCap = GetTickCount();
}

void HideOverview(AppState& app) {
    if (!app.overview) return;
    // Сначала подложка (сквозь прозрачный обзор сразу видны настоящие стол и окна — в 1:1
    // они совпадают с превью), потом обзор, и лишь затем снимаем превью: в обратном
    // порядке на кадр мелькнули бы обои без окон.
    if (app.ovBack) ShowWindow(app.ovBack, SW_HIDE);
    ShowWindow(app.ov, SW_HIDE);
    for (auto& w : app.wins) { w.thumbOv.reset(); w.thumbShadow.reset(); }   // ScopedThumbnail снимает регистрацию
    app.ovBars.clear();
    app.wallThumb.reset();
    app.ovLiveBg = false;
    app.overview = false;
}

// Яркость обоев в обзоре: чем дальше отдалили, тем темнее (не темнее ~70%).
static float OvWallOpacity(const AppState& app) {
    double dim = (1.0 - app.zoom) / (1.0 - ZOOM_MIN);
    if (dim < 0) dim = 0; if (dim > 1) dim = 1;
    return (float)(1.0 - dim * 0.7);
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
    if (!app.overview && !app.glide) return;
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
        if (w.thumbShadow) {
            // Тень — та же рамка, расширенная на поля тени; масштабируется вместе с окном.
            RECT sd = { (LONG)llround(OvClientX(app, vw.left - w.shadowPad.left)),
                        (LONG)llround(OvClientY(app, vw.top - w.shadowPad.top)),
                        (LONG)llround(OvClientX(app, vw.right + w.shadowPad.right)),
                        (LONG)llround(OvClientY(app, vw.bottom + w.shadowPad.bottom)) };
            bool soff = (sd.right <= sd.left || sd.bottom <= sd.top ||
                         sd.right < 0 || sd.left > app.vsW || sd.bottom < 0 || sd.top > app.vsH);
            DWM_THUMBNAIL_PROPERTIES sp = {};
            sp.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
            sp.opacity = 255;
            sp.fSourceClientAreaOnly = FALSE;
            sp.fVisible = soff ? FALSE : TRUE;
            sp.rcDestination = sd;
            DwmUpdateThumbnailProperties(w.thumbShadow, &sp);
        }
    }
    // Панели у края экрана: уезжают за край и растворяются по мере barP.
    for (auto& b : app.ovBars) {
        const double p = app.barP;
        LONG ox = (LONG)lround(b.dx * p), oy = (LONG)lround(b.dy * p);
        RECT moved = { b.dst.left + ox, b.dst.top + oy, b.dst.right + ox, b.dst.bottom + oy }, vis;
        DWM_THUMBNAIL_PROPERTIES tp = {};
        if (p >= 0.999 || !IntersectRect(&vis, &moved, &b.clip)) {
            tp.dwFlags = DWM_TNP_VISIBLE;
            tp.fVisible = FALSE;
        } else {
            // Часть, выехавшую за монитор, отрезаем и у приёмника, и у источника — превью
            // «уходит под край», а не въезжает на соседний монитор.
            tp.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE |
                         DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
            tp.rcDestination = vis;
            tp.rcSource = { vis.left - moved.left, vis.top - moved.top, vis.right - moved.left, vis.bottom - moved.top };
            tp.opacity = (BYTE)(255.0 * (1.0 - p) + 0.5);
            tp.fVisible = TRUE;
            tp.fSourceClientAreaOnly = FALSE;
        }
        DwmUpdateThumbnailProperties(b.thumb, &tp);
    }
    // Живой фон: затемнение — непрозрачностью превью над чёрной подложкой.
    if (app.ovLiveBg && app.wallThumb) {
        DWM_THUMBNAIL_PROPERTIES p = {};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE |
                    DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        p.rcDestination = app.wallDst;
        p.opacity = (BYTE)(OvWallOpacity(app) * 255.0f + 0.5f);
        p.fVisible = TRUE;
        p.fSourceClientAreaOnly = FALSE;
        DwmUpdateThumbnailProperties(app.wallThumb, &p);
    }
    // Фон/тени НЕ через InvalidateRect: WM_PAINT пришёл бы на следующей итерации цикла,
    // и тени отставали бы от превью на кадр. FrameTick вызывает RenderOverview сразу
    // за этим обновлением — превью и тени попадают в одну композицию.
}

// ---------- Direct2D ----------
// Фон обзора (обои с затемнением), тени под окнами и подпись рисуются Direct2D.
// DWM-превью окон композитор кладёт ПОВЕРХ содержимого окна независимо от того, чем оно
// нарисовано, поэтому GDI-путь заменён без изменения картинки. Что даёт GPU:
//  - затемнение обоев — DrawBitmap с opacity (раньше полноэкранный AlphaBlend на CPU,
//    который приходилось кэшировать по ступеням);
//  - тени — заливки скруглённых прямоугольников (раньше 7 регионов + 7 AlphaBlend на окно).

// Фабрики (device-independent) — один раз на процесс. Требует инициализированного COM (WIC).
void InitGraphics(AppState& app) {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, app.d2dFactory.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(app.dwFactory.GetAddressOf()));
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(app.wicFactory.GetAddressOf()));
    if (app.dwFactory)
        app.dwFactory->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            15.0f, L"", app.ovTextFormat.GetAddressOf());
}

// Подложка живого фона и прозрачность окна обзора (вызывается после создания app.ov).
// Подложка: чёрное окно под обзором, на которое кладётся живое DWM-превью хоста обоев.
// Layered + transparent — чтобы мышь её не замечала (клик «внутрь окна» идёт сквозь неё).
// Окно обзора получает попиксельную альфу: «размытие позади» с пустым регионом — на
// Windows 8+ размытия нет, а альфа кадра Direct2D начинает учитываться композитором.
static LRESULT CALLBACK BackProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}
void CreateOverviewBackdrop(AppState& app, HINSTANCE hi) {
    WNDCLASSW c = {};
    c.lpfnWndProc = BackProc;
    c.hInstance = hi;
    c.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    c.lpszClassName = L"InfiniteDesktopBackdrop";
    RegisterClassW(&c);
    app.ovBack = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        c.lpszClassName, L"", WS_POPUP,
        app.vsX, app.vsY, app.vsW, app.vsH, nullptr, nullptr, hi, nullptr);
    if (app.ovBack) SetLayeredWindowAttributes(app.ovBack, 0, 255, LWA_ALPHA);
    if (app.ov) {
        HRGN rgn = CreateRectRgn(0, 0, -1, -1);
        DWM_BLURBEHIND bb = {};
        bb.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
        bb.fEnable = TRUE;
        bb.hRgnBlur = rgn;
        DwmEnableBlurBehindWindow(app.ov, &bb);
        DeleteObject(rgn);
    }
}

// Всё device-dependent сбрасываем разом: после потери устройства (смена драйвера,
// сон/пробуждение, переключение GPU) пересоздадим на следующем кадре.
static void ReleaseOvTarget(AppState& app) {
    app.ovShadowBrush.Reset();
    app.ovTextBrush.Reset();
    app.wallD2D.Reset();
    app.ovRT.Reset();
}

// Render target окна обзора. Создаётся лениво при первом кадре.
static bool EnsureOvTarget(AppState& app, HWND hwnd) {
    if (app.ovRT) return true;
    if (!app.d2dFactory) return false;
    RECT rc; GetClientRect(hwnd, &rc);
    // DPI 96: приложение per-monitor-DPI-aware и считает в физических пикселях
    // виртуального экрана; иначе D2D масштабировал бы координаты под системный DPI.
    // TYPE_DEFAULT — аппаратный рендер, программный fallback D2D выбирает сам.
    D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        // PREMULTIPLIED: с живым фоном окно прозрачно там, где ничего не нарисовано
        // (см. CreateOverviewBackdrop); с обычным фоном кадр заливается непрозрачно.
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    // PRESENT_OPTIONS_NONE: EndDraw ждёт vblank — кадр показывается целиком в одной
    // композиции DWM. С IMMEDIATELY часть экрана успевала обновиться, часть нет — на
    // обоях, меняющих яркость каждый кадр, это выглядело как дёрганье. Чтобы не ждать
    // vblank дважды, цикл кадров пропускает DwmFlush после такого кадра (см. FrameTick).
    D2D1_HWND_RENDER_TARGET_PROPERTIES hp = D2D1::HwndRenderTargetProperties(
        hwnd, D2D1::SizeU((UINT32)rc.right, (UINT32)rc.bottom), D2D1_PRESENT_OPTIONS_NONE);
    if (FAILED(app.d2dFactory->CreateHwndRenderTarget(rp, hp, app.ovRT.GetAddressOf()))) return false;
    // ClearType на поверхности с альфой даёт цветную кайму поверх прозрачного — серое сглаживание.
    app.ovRT->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    app.ovRT->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 16.0f / 255.0f),   // ~16 альфы на слой тени
                                    app.ovShadowBrush.GetAddressOf());
    app.ovRT->CreateSolidColorBrush(D2D1::ColorF(150 / 255.0f, 160 / 255.0f, 175 / 255.0f),
                                    app.ovTextBrush.GetAddressOf());
    return true;
}

// Сменился ли файл обоев с момента последней загрузки (путь или время изменения).
// Дёшево: один SPI-вызов и один stat файла — в отличие от полной перекодировки.
// Слайдшоу/«обои в реальном времени» пишут новый файл, так что mtime это ловит.
static ULONGLONG FileMtime(const wchar_t* p) {
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &fa)) return 0;
    return ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
}

// Какой файл рисовать как обои. Windows хранит УЖЕ отрендеренные обои — с учётом режима
// («Заполнение», «По размеру», «Растянуть»…) и разрешения монитора — в
// %APPDATA%\Microsoft\Windows\Themes\CachedFiles\CachedImage_<W>_<H>_POS<режим>.jpg.
// Берём его: файл размером с монитор, наш «cover» даёт ровно 1:1, и фон обзора
// совпадает с рабочим столом попиксельно. Собственный «cover» исходного файла
// расходился с системным кадрированием (картинка уезжала вверх). Кэш годится, только
// если он не старее исходника — иначе он от прошлых обоев. Запасной путь — исходник.
static std::wstring WallpaperSource(AppState&) {
    wchar_t path[MAX_PATH] = {};
    if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0) || !path[0]) return L"";
    ULONGLONG srcM = FileMtime(path);
    wchar_t ad[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", ad, MAX_PATH);
    if (n && n < MAX_PATH) {
        std::wstring dir = std::wstring(ad) + L"\\Microsoft\\Windows\\Themes\\CachedFiles\\";
        wchar_t pat[64];   // разрешение основного монитора в физических пикселях (процесс DPI-aware)
        wsprintfW(pat, L"CachedImage_%d_%d_POS*", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
        WIN32_FIND_DATAW fd;
        HANDLE hf = FindFirstFileW((dir + pat).c_str(), &fd);
        if (hf != INVALID_HANDLE_VALUE) {
            std::wstring best; ULONGLONG bestM = 0;
            do {   // несколько (разные режимы) — берём самый свежий
                ULONGLONG m = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
                if (m > bestM) { bestM = m; best = dir + fd.cFileName; }
            } while (FindNextFileW(hf, &fd));
            FindClose(hf);
            if (!best.empty() && bestM >= srcM) return best;
        }
    }
    return path;
}

bool WallpaperFileChanged(AppState& app) {
    // Живые обои: файл обоев Windows к картинке на столе отношения не имеет; снимок
    // обновляется при входе в зум (ShowOverview), а не по этой периодической проверке —
    // иначе фон обзора дёргался бы каждые 1.5 с.
    if (app.wallLive && app.wallWic) return false;
    std::wstring src = WallpaperSource(app);
    if (src.empty()) return !app.wallPath.empty();   // обоев не стало — сбросить кэш
    return app.wallPath != src || app.wallMtime != FileMtime(src.c_str()) || !app.wallWic;
}

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

// Снимок обоев в том виде, как их отрисовала сама Windows. PrintWindow(Progman,
// PW_RENDERFULLCONTENT) отдаёт содержимое окна рабочего стола БЕЗ лежащих сверху окон;
// режим («Заполнение»…), кадрирование, DPI, разные обои на мониторах — всё уже учтено
// системой, совпадение с рабочим столом попиксельное по построению. Наш собственный
// «cover» исходного файла с системным кадрированием расходился (картинка уезжала вверх).
// Иконки стола на время работы скрыты, так что в снимок не попадают. На некоторых
// сборках PrintWindow возвращает чёрное — тогда false, и остаётся декод файла.
// Диагностика источника обоев: %LOCALAPPDATA%\InfiniteDesktop\wall.log (дописывается).
// Нужен, чтобы по логу с чужой машины понять, какой путь сработал — снимок или файл.
void WallLog(const wchar_t* fmt, ...) {
    va_list ap;
    // Ограничиваем рост: при > 64 КБ начинаем файл заново (слайдшоу пишет строки на каждую смену).
    std::wstring path = ConfigPath(L"wall.log");
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    bool big = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) &&
               (fa.nFileSizeHigh || fa.nFileSizeLow > 65536);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), big ? L"w, ccs=UTF-8" : L"a, ccs=UTF-8") || !f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fwprintf(f, L"%02d:%02d:%02d  ", st.wHour, st.wMinute, st.wSecond);
    va_start(ap, fmt); vfwprintf(f, fmt, ap); va_end(ap);
    fwprintf(f, L"\n");
    fclose(f);
}

// ---------- Диагностика устройства рабочего стола ----------
// Раскладка окон стола различается между сборками Windows и меняется программами обоев
// и модами оболочки, а воспроизвести чужую систему нельзя — поэтому пишем её в wall.log.
struct TreeCtx { std::wstring text; DWORD hostPid; int count; };
static BOOL CALLBACK TreeChildProc(HWND c, LPARAM lp) {
    TreeCtx* x = (TreeCtx*)lp;
    if (++x->count > 12) { x->text += L" ..."; return FALSE; }
    wchar_t cls[64] = {};
    GetClassNameW(c, cls, 64);
    DWORD pid = 0;
    GetWindowThreadProcessId(c, &pid);
    int vis = (GetWindowLongW(c, GWL_STYLE) & WS_VISIBLE) ? 1 : 0;   // свой флаг, без учёта родителя
    x->text += L" ";
    if (pid != x->hostPid) { x->text += ExeName(pid); x->text += L":"; }
    x->text += cls;
    x->text += vis ? L"(1)" : L"(0)";
    return TRUE;
}
static BOOL CALLBACK TreeTopProc(HWND h, LPARAM lp) {
    wchar_t cls[32] = {};
    GetClassNameW(h, cls, 32);
    if (lstrcmpW(cls, L"WorkerW") && lstrcmpW(cls, L"Progman")) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != *(DWORD*)lp) return TRUE;                 // только окна процесса оболочки
    RECT r = {};
    GetWindowRect(h, &r);
    if (r.right - r.left < 200) return TRUE;             // служебные окна-пустышки
    TreeCtx x = { L"", pid, 0 };
    EnumChildWindows(h, TreeChildProc, (LPARAM)&x);
    WallLog(L"  %s %p vis=%d ex=%08X %d,%d %dx%d:%s", cls, h, IsWindowVisible(h) ? 1 : 0,
            (unsigned)GetWindowLongW(h, GWL_EXSTYLE), r.left, r.top, r.right - r.left, r.bottom - r.top,
            x.text.empty() ? L" -" : x.text.c_str());
    return TRUE;
}
// Окна рабочего стола сверху вниз (порядок EnumWindows — z-порядок); у дочерних в скобках
// их собственный флаг видимости, у окон чужих процессов — имя программы.
void LogDesktopTree(const wchar_t* why) {
    DWORD pid = 0;
    HWND shell = GetShellWindow();
    if (!shell) shell = FindWindowW(L"Progman", nullptr);   // Explorer перезапускается
    if (shell) GetWindowThreadProcessId(shell, &pid);
    WallLog(L"desktop tree (%s), shell %p:", why, shell);
    EnumWindows(TreeTopProc, (LPARAM)&pid);
}

// Цепочка классов от окна до корня и программа-владелец — чтобы по логу было видно,
// какое именно окно приложение приняло за рабочий стол.
static void LogWindowChain(const wchar_t* what, HWND h) {
    std::wstring chain;
    int depth = 0;
    for (HWND c = h; c && depth < 10; c = GetAncestor(c, GA_PARENT), depth++) {
        wchar_t cls[64] = {};
        GetClassNameW(c, cls, 64);
        if (!lstrcmpW(cls, L"#32769")) break;            // дошли до окна рабочего стола системы
        if (depth) chain += L" < ";
        chain += cls;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    WallLog(L"%s: %p %s [%s]", what, h, chain.c_str(), ExeName(pid).c_str());
}

static BOOL CALLBACK CollectWorkerW(HWND h, LPARAM lp) {
    wchar_t cls[32] = {};
    GetClassNameW(h, cls, 32);
    if (!lstrcmpW(cls, L"WorkerW") && IsWindowVisible(h)) ((std::vector<HWND>*)lp)->push_back(h);
    return TRUE;
}

static bool TryCaptureWindow(AppState& app, HWND hw);

// ---------- Живые обои (Wallpaper Engine, Lively и т.п.) ----------
// Такие программы не меняют файл обоев Windows: они встраивают СВОЁ окно внутрь окна
// рабочего стола — в WorkerW за иконками (Windows 10, старые сборки 11) или прямо в
// Progman (Windows 11 24H2+). Признак хоста, не зависящий от конкретной программы:
// у окна рабочего стола есть видимое дочернее окно ЧУЖОГО процесса (свои дочерние —
// иконки, служебный WorkerW — принадлежат explorer.exe).
struct LiveCtx { DWORD hostPid; bool found; };
static BOOL CALLBACK LiveChildProc(HWND c, LPARAM lp) {
    LiveCtx* x = (LiveCtx*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(c, &pid);
    if (pid != x->hostPid && IsWindowVisible(c)) { x->found = true; return FALSE; }
    return TRUE;
}
static bool HostsForeignChild(HWND host) {
    LiveCtx x = { 0, false };
    GetWindowThreadProcessId(host, &x.hostPid);
    EnumChildWindows(host, LiveChildProc, (LPARAM)&x);
    return x.found;
}
static HWND FindLiveWallpaperHost() {
    std::vector<HWND> ws;
    EnumWindows(CollectWorkerW, (LPARAM)&ws);            // видимые WorkerW верхнего уровня
    for (HWND h : ws) if (HostsForeignChild(h)) return h;
    HWND pm = FindWindowW(L"Progman", nullptr);
    return (pm && HostsForeignChild(pm)) ? pm : nullptr;
}

static bool CaptureDesktopWallpaper(AppState& app) {
    if (!app.wicFactory) return false;
    if (HWND pm = FindWindowW(L"Progman", nullptr)) {
        if (TryCaptureWindow(app, pm)) { WallLog(L"capture: Progman ok"); return true; }
        WallLog(L"capture: Progman failed/black");
    } else WallLog(L"capture: Progman not found");
    // Обои может рисовать WorkerW (стол «раздвоен» live-wallpaper-программой) — перебираем
    // видимые WorkerW размером хотя бы с полэкрана.
    std::vector<HWND> ws;
    EnumWindows(CollectWorkerW, (LPARAM)&ws);
    for (HWND h : ws) {
        RECT r; if (!GetWindowRect(h, &r)) continue;
        if (r.right - r.left < app.vsW / 2 || r.bottom - r.top < app.vsH / 2) continue;
        if (TryCaptureWindow(app, h)) { WallLog(L"capture: WorkerW %p ok", h); return true; }
    }
    WallLog(L"capture: no WorkerW worked (%u candidates)", (unsigned)ws.size());
    return false;
}

static bool TryCaptureWindow(AppState& app, HWND hw) {
    RECT pr;
    if (!GetWindowRect(hw, &pr)) return false;
    int w = pr.right - pr.left, h = pr.bottom - pr.top;
    if (w < 1 || h < 1) return false;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;   // top-down, как ждёт WIC
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    HDC sdc = GetDC(nullptr);
    ScopedDC mdc(CreateCompatibleDC(sdc));
    ReleaseDC(nullptr, sdc);
    void* bits = nullptr;
    ScopedBitmap dib(CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0));
    if (!mdc || !dib || !bits) return false;
    HGDIOBJ old = SelectObject(mdc, dib);
    BOOL ok = PrintWindow(hw, mdc, PW_RENDERFULLCONTENT);
    SelectObject(mdc, old);
    if (!ok) return false;
    // Сплошь чёрный снимок — признак неудачи (у пользователя картинка): сэмплируем сетку 8×8.
    const BYTE* p = (const BYTE*)bits;
    bool any = false;
    for (int i = 0; i < 64 && !any; ++i) {
        int x = (w * (i % 8)) / 8 + w / 16, y = (h * (i / 8)) / 8 + h / 16;
        const BYTE* px = p + ((size_t)y * w + x) * 4;
        any = (px[0] | px[1] | px[2]) != 0;
    }
    if (!any) return false;
    ComPtr<IWICBitmap> raw;
    if (FAILED(app.wicFactory->CreateBitmapFromMemory((UINT)w, (UINT)h, GUID_WICPixelFormat32bppBGR,
                                                      (UINT)w * 4, (UINT)((size_t)w * h * 4), (BYTE*)bits, &raw))) return false;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(app.wicFactory->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(raw.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                nullptr, 0.0, WICBitmapPaletteTypeMedianCut))) return false;
    if (FAILED(app.wicFactory->CreateBitmapFromSource(conv.Get(), WICBitmapCacheOnLoad, &app.wallWic))) return false;
    app.wallOffX = pr.left - app.vsX;   // Progman обычно = весь виртуальный экран; на всякий случай — со смещением
    app.wallOffY = pr.top  - app.vsY;
    return true;
}

// Декодировать обои через WIC в память (PBGRA). Если картинка заметно больше экрана —
// один раз уменьшить качественным фильтром (Fant): тогда покадровое линейное
// масштабирование на GPU идёт ~1:1, без алиасинга на огромных обоях.
// GPU-битмап из этого создаётся лениво (EnsureWallD2D) — render target может ещё не
// существовать, а после потери устройства его нужно создать заново из тех же данных.
void LoadWallpaper(AppState& app) {
    app.wallWic.Reset();
    app.wallD2D.Reset();

    // Живые обои: фон обзора — снимок окна-хоста (то, что реально на столе), а не файл
    // обоев Windows. Снимок не удался (чёрный/отказ) — идём обычным путём ниже.
    HWND live = FindLiveWallpaperHost();
    app.wallLive = (live != nullptr);
    if (live && app.wicFactory) {
        if (TryCaptureWindow(app, live)) {
            app.wallIsScreen = true; app.wallPath = L"<live>";
            UINT cw = 0, ch = 0; app.wallWic->GetSize(&cw, &ch);
            WallLog(L"wallpaper: LIVE HOST %p capture %ux%u off=%d,%d", live, cw, ch, app.wallOffX, app.wallOffY);
            return;
        }
        WallLog(L"wallpaper: LIVE HOST %p capture failed/black - fallback", live);
    }

    std::wstring srcPath = WallpaperSource(app);
    if (srcPath.empty()) { app.wallPath.clear(); return; }
    app.wallMtime = FileMtime(srcPath.c_str());
    if (!app.wicFactory) { app.wallPath.clear(); return; }

    // Файл обоев сменился только что (слайдшоу): Windows ещё анимирует переход, снимок
    // поймал бы полукадр. Откладываем — wallPath пуст, следующая проверка повторит.
    {
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        ULONGLONG now = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        if (now > app.wallMtime && now - app.wallMtime < 20000000ULL) { app.wallPath.clear(); return; }   // < 2 с
    }
    if (CaptureDesktopWallpaper(app)) {
        app.wallIsScreen = true; app.wallPath = srcPath;
        UINT cw = 0, ch = 0; app.wallWic->GetSize(&cw, &ch);
        WallLog(L"wallpaper: SCREEN CAPTURE %ux%u off=%d,%d  vs=%dx%d", cw, ch, app.wallOffX, app.wallOffY, app.vsW, app.vsH);
        return;
    }
    app.wallIsScreen = false;   // снимок не удался — декодируем файл сами
    WallLog(L"wallpaper: FILE %s", srcPath.c_str());

    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(app.wicFactory->CreateDecoderFromFilename(srcPath.c_str(), nullptr, GENERIC_READ,
                                                         WICDecodeMetadataCacheOnDemand, &dec))) { app.wallPath.clear(); return; }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) { app.wallPath.clear(); return; }
    UINT iw = 0, ih = 0;
    frame->GetSize(&iw, &ih);
    if (iw < 1 || ih < 1) { app.wallPath.clear(); return; }

    ComPtr<IWICBitmapSource> src(frame);
    UINT capW = (UINT)(app.vsW * 3 / 2), capH = (UINT)(app.vsH * 3 / 2);
    if (capW > 0 && capH > 0 && iw > capW && ih > capH) {
        double s = max((double)capW / iw, (double)capH / ih);   // не меньше «cover» экрана
        ComPtr<IWICBitmapScaler> sc;
        if (SUCCEEDED(app.wicFactory->CreateBitmapScaler(&sc)) &&
            SUCCEEDED(sc->Initialize(frame.Get(), (UINT)(iw * s + 0.5), (UINT)(ih * s + 0.5),
                                     WICBitmapInterpolationModeFant)))
            src = sc;
    }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(app.wicFactory->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(src.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                nullptr, 0.0, WICBitmapPaletteTypeMedianCut))) { app.wallPath.clear(); return; }
    // CacheOnLoad: декодируем сейчас, в память; файл после этого закрыт.
    if (FAILED(app.wicFactory->CreateBitmapFromSource(conv.Get(), WICBitmapCacheOnLoad, &app.wallWic))) {
        app.wallPath.clear(); return;
    }
    app.wallPath = srcPath;
}

static bool EnsureWallD2D(AppState& app) {
    if (app.wallD2D) return true;
    if (!app.wallWic || !app.ovRT) return false;
    return SUCCEEDED(app.ovRT->CreateBitmapFromWicBitmap(app.wallWic.Get(), nullptr, app.wallD2D.GetAddressOf()));
}

// Обои на каждом мониторе в режиме «Заполнение» (cover), с затемнением через opacity.
// Затемнение считается GPU на лету — кэш по ступеням, нужный GDI-пути, больше не нужен.
struct WallDrawCtx { AppState* app; float opacity; };
static BOOL CALLBACK WallMonProc(HMONITOR hm, HDC, LPRECT, LPARAM lp) {
    WallDrawCtx* c = (WallDrawCtx*)lp;
    AppState& app = *c->app;
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(hm, &mi)) return TRUE;
    float mx = (float)(mi.rcMonitor.left - app.vsX), my = (float)(mi.rcMonitor.top - app.vsY);
    float mw = (float)(mi.rcMonitor.right - mi.rcMonitor.left);
    float mh = (float)(mi.rcMonitor.bottom - mi.rcMonitor.top);
    D2D1_SIZE_U isz = app.wallD2D->GetPixelSize();
    if (isz.width < 1 || isz.height < 1) return TRUE;
    float s = max(mw / isz.width, mh / isz.height);   // cover (как «Заполнение» в Windows)
    float dw = isz.width * s, dh = isz.height * s;
    float dx = mx + (mw - dw) / 2, dy = my + (mh - dh) / 2;
    app.ovRT->PushAxisAlignedClip(D2D1::RectF(mx, my, mx + mw, my + mh), D2D1_ANTIALIAS_MODE_ALIASED);
    app.ovRT->DrawBitmap(app.wallD2D.Get(), D2D1::RectF(dx, dy, dx + dw, dy + dh),
                         c->opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    app.ovRT->PopAxisAlignedClip();
    return TRUE;
}

// Тень окна средствами Direct2D: вложенные контуры равного затемнения по профилю,
// снятому с настоящей тени Windows (id_shadow.cpp). scale — пикселей слоя на пиксель
// профиля (масштаб обзора × масштаб монитора окна).
static void DrawNativeShadow(AppState& app, const RECT& r, bool active, bool dark, float scale, float corner) {
    for (const ShadowLayer& l : ShadowLayers(active, dark)) {
        D2D1_RECT_F q = { r.left - l.dl * scale, r.top - l.dt * scale, r.right + l.dr * scale, r.bottom + l.db * scale };
        float rx = corner + l.dl * scale, ry = corner + (l.dt + l.db) * 0.5f * scale;
        app.ovShadowBrush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, l.alpha));
        app.ovRT->FillRoundedRectangle(D2D1::RoundedRect(q, rx, ry), app.ovShadowBrush.Get());
    }
}

void RenderOverview(AppState& app, HWND hwnd) {
    if (!EnsureOvTarget(app, hwnd)) return;
    ID2D1HwndRenderTarget* rt = app.ovRT.Get();
    RECT rc; GetClientRect(hwnd, &rc);
    D2D1_SIZE_U want = D2D1::SizeU((UINT32)rc.right, (UINT32)rc.bottom), have = rt->GetPixelSize();
    if (want.width != have.width || want.height != have.height) rt->Resize(want);

    rt->BeginDraw();
    // Живой фон: кадр прозрачен, обои показывает подложка под окном (и затемняет она же).
    rt->Clear(app.ovLiveBg ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f) : D2D1::ColorF(D2D1::ColorF::Black));
    if (!app.ovLiveBg && EnsureWallD2D(app)) {
        // Обои пользователя, затемнённые тем сильнее, чем дальше отдалили.
        float opacity = OvWallOpacity(app);
        if (app.wallIsScreen) {
            // снимок реального рендера стола — 1:1, со смещением окна Progman
            D2D1_SIZE_U s = app.wallD2D->GetPixelSize();
            float ox = (float)app.wallOffX, oy = (float)app.wallOffY;
            rt->DrawBitmap(app.wallD2D.Get(), D2D1::RectF(ox, oy, ox + s.width, oy + s.height),
                           opacity, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        } else {
            WallDrawCtx ctx = { &app, opacity };
            EnumDisplayMonitors(nullptr, nullptr, WallMonProc, (LPARAM)&ctx);
        }
    }

    // Тени окон. Основной путь — тень-превью под превью окна (id_shadow.cpp): она падает
    // и на соседние окна. Здесь рисуется то, чего превью дать не может:
    //  - живые обои: окно обзора прозрачно, а полупрозрачное превью на прозрачном месте
    //    система не показывает — тень на обоях пропадала бы целиком. Рисуем её сами, по
    //    тому же профилю; под превью окон она скрыта, так что с тенью-превью не двоится;
    //  - зум: окно без системной тени (развёрнутое и т.п.) — чтобы не сливалось с фоном.
    HWND fgWin = GetForegroundWindow();
    for (auto& w : app.wins) {
        RECT vw = VisWorld(w);
        RECT d = { (LONG)llround(OvClientX(app, vw.left)),  (LONG)llround(OvClientY(app, vw.top)),
                   (LONG)llround(OvClientX(app, vw.right)), (LONG)llround(OvClientY(app, vw.bottom)) };
        if (d.right <= d.left || d.bottom <= d.top) continue;
        // Окно целиком за пределами обзора (с запасом на размах тени) — не рисуем.
        if (d.right < -100 || d.left > rc.right + 100 || d.bottom < -100 || d.top > rc.bottom + 100) continue;
        if (app.glide && w.glideReal) continue;   // настоящее окно поверх слоя — у него своя тень
        const bool native = (bool)w.shadowSrc;    // у окна есть системная тень (см. EnsureShadow)
        if (w.thumbShadow && !app.ovLiveBg) continue;   // тень показывает превью
        if (app.glide && !native) continue;       // 1:1: у окна без системной тени её нет и на столе
        float sc = (float)app.zoom * (w.shadowDpi ? w.shadowDpi / 96.0f : 1.0f);
        DrawNativeShadow(app, d, native ? w.shadowActive : (w.hwnd == fgWin), native && w.shadowDark, sc, 8.0f * sc);
    }
    // Живые обои: нарисованная здесь тень нужна только НАД ОБОЯМИ. Под превью окон её
    // быть не должно: на окна ниже тень кладёт тень-превью, а сквозь полупрозрачное окно
    // (акриловый фон и т.п.) нарисованная тень просвечивала бы и давала двойное затемнение.
    // Стираем её в пределах рамки каждого окна — крестом из двух прямоугольников, чтобы
    // уголки рамки остались: за скруглёнными углами окна тень видна.
    if (app.ovLiveBg) {
        for (auto& w : app.wins) {
            if (!w.thumbOv) continue;
            RECT vw = VisWorld(w);
            float l = (float)OvClientX(app, vw.left), t = (float)OvClientY(app, vw.top);
            float r = (float)OvClientX(app, vw.right), b = (float)OvClientY(app, vw.bottom);
            float c = 8.0f * (float)app.zoom * (w.shadowDpi ? w.shadowDpi / 96.0f : 1.0f);
            if (r - l <= 2 * c || b - t <= 2 * c) continue;
            const D2D1_RECT_F cross[2] = { { l + c, t, r - c, b }, { l, t + c, r, b - c } };
            for (const D2D1_RECT_F& q : cross) {
                rt->PushAxisAlignedClip(q, D2D1_ANTIALIAS_MODE_ALIASED);
                rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
                rt->PopAxisAlignedClip();
            }
        }
    }

    if (!app.glide && app.ovTextFormat && app.ovTextBrush) {   // в плавной панораме подписи нет
        wchar_t info[256];
        wsprintfW(info, Tr(S_OV_HINT_FMT), (int)(app.zoom * 100));
        // Подпись — в масштабе интерфейса (как миникарта): на 200 % иначе вдвое мельче.
        const float s = app.uiScale;
        rt->SetTransform(D2D1::Matrix3x2F::Scale(s, s));
        rt->DrawText(info, (UINT32)lstrlenW(info), app.ovTextFormat.Get(),
                     D2D1::RectF(24.0f, 20.0f, (float)rc.right / s, 60.0f), app.ovTextBrush.Get());
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    HRESULT hr = rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) ReleaseOvTarget(app);   // устройство потеряно — пересоздадим
}

// Верхнее окно под точкой обзора (по z-порядку сверху вниз) или nullptr — фон.
static HWND OvHitTest(AppState& app, double cx, double cy) {
    auto inside = [&](const TrackedWin& w) {
        RECT vw = VisWorld(w);
        double l = OvClientX(app, vw.left),  t = OvClientY(app, vw.top);
        double r = OvClientX(app, vw.right), b = OvClientY(app, vw.bottom);
        return cx >= l && cx <= r && cy >= t && cy <= b;
    };
    for (int zi = (int)app.ovZ.size() - 1; zi >= 0; --zi)
        for (auto& w : app.wins)
            if (w.hwnd == app.ovZ[zi] && inside(w)) return w.hwnd;
    // Окна, появившиеся уже в обзоре (открыты кликом изнутри): в ovZ их нет — они сверху.
    for (auto& w : app.wins)
        if (std::find(app.ovZ.begin(), app.ovZ.end(), w.hwnd) == app.ovZ.end() && inside(w)) return w.hwnd;
    return nullptr;
}

// Подождать, прокачивая сообщения. Просто Sleep нельзя: маршрутизация ввода может
// спросить наш оверлей WM_NCHITTEST, а спящий поток не ответит — и клик уйдёт не туда.
static void PumpFor(DWORD ms) {
    DWORD end = GetTickCount() + ms;
    for (;;) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        DWORD now = GetTickCount();
        if ((LONG)(end - now) <= 0) break;
        MsgWaitForMultipleObjectsEx(0, nullptr, end - now, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

// Клик внутрь окна ИЗ обзора — не выходя из зума. Ограничение ОС: ввод нельзя отдать в
// уменьшенное превью, а подделанные WM_LBUTTON* многие приложения игнорируют (Chromium,
// UWP, игры). Обход: реальное окно — оно за оверлеем обзора, пользователь его не видит —
// на мгновение подставляется под курсор так, чтобы нажатая на превью точка оказалась
// ровно под ним; оверлей на это время прозрачен для hit-теста; инжектируется НАСТОЯЩИЙ
// клик (SendInput); окно возвращается на место. Зум не меняется, превью обновляется
// вживую. Клик настоящий — работает всё, что на него реагирует: кнопки, меню, фокус
// (дальше клавиатура идёт в это окно). Не работает то, что требует непрерывного курсора
// внутри приложения: hover, перетаскивание, прокрутка колесом (колесо в обзоре — зум).
static void ClickIntoWindow(AppState& app, HWND h, double cx, double cy, bool right) {
    TrackedWin* w = nullptr;
    for (auto& t : app.wins) if (t.hwnd == h) { w = &t; break; }
    if (!w || !IsWindow(h) || IsHungAppWindow(h)) return;
    RECT vw = VisWorld(*w);
    double l = OvClientX(app, vw.left),  t = OvClientY(app, vw.top);
    double r = OvClientX(app, vw.right), b = OvClientY(app, vw.bottom);
    if (r <= l || b <= t) return;
    double u = (cx - l) / (r - l), v = (cy - t) / (b - t);   // доля внутри видимой рамки
    LONG offX = w->vm.left + (LONG)llround(u * (vw.right - vw.left));   // нажатая точка
    LONG offY = w->vm.top  + (LONG)llround(v * (vw.bottom - vw.top));   // относительно окна
    RECT cur;
    if (!GetWindowRect(h, &cur)) return;
    const UINT F = SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING;
    // Развёрнутое окно двигать нельзя — оно и так на экране: подводим к точке курсор.
    // Обычное — подставляем под курсор; HWND_TOP, иначе клик перехватит другое реальное
    // окно, лежащее поверх в этой точке.
    bool zoomed = IsZoomed(h) != FALSE;
    POINT savedCur = {}; GetCursorPos(&savedCur);
    if (zoomed) SetCursorPos(cur.left + offX, cur.top + offY);
    else        SetWindowPos(h, HWND_TOP, app.vsX + (LONG)llround(cx) - offX,
                                          app.vsY + (LONG)llround(cy) - offY, 0, 0, F);
    // Пропустить клик сквозь окно обзора: «прокол» 3×3 px в регионе окна под курсором —
    // в этой точке окна для системы просто нет, и ввод уходит ниже. WS_EX_TRANSPARENT
    // для не-layered окна система сквозным не считает, а HTTRANSPARENT действует только
    // в пределах одного потока — проверено пробой. Подложка под обзором прозрачна для
    // мыши постоянно (layered + transparent), так что клик доходит до реального окна.
    POINT tp = zoomed ? POINT{ cur.left + offX, cur.top + offY }
                      : POINT{ app.vsX + (LONG)llround(cx), app.vsY + (LONG)llround(cy) };
    int hx = tp.x - app.vsX, hy = tp.y - app.vsY;
    HRGN rgn = CreateRectRgn(0, 0, app.vsW, app.vsH);
    HRGN hole = CreateRectRgn(hx - 1, hy - 1, hx + 2, hy + 2);
    CombineRgn(rgn, rgn, hole, RGN_DIFF);
    DeleteObject(hole);
    SetWindowRgn(app.ov, rgn, FALSE);          // регионом теперь владеет система
    INPUT in[2] = {};
    in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = right ? MOUSEEVENTF_RIGHTUP   : MOUSEEVENTF_LEFTUP;
    SendInput(2, in, sizeof(INPUT));
    PumpFor(30);   // маршрутизация инжектированного ввода асинхронна — даём ей пройти
    SetWindowRgn(app.ov, nullptr, FALSE);      // вернуть целое окно
    if (zoomed) SetCursorPos(savedCur.x, savedCur.y);
    // Клик мог изменить само окно: «Развернуть», «Свернуть», «Закрыть». Развёрнутое или
    // свёрнутое окно на прежнее место не двигаем — сдвинули бы его с монитора.
    else if (IsWindow(h) && !IsZoomed(h) && !IsIconic(h))
        SetWindowPos(h, nullptr, cur.left, cur.top, 0, 0, F | SWP_NOZORDER);
}

LRESULT CALLBACK OvProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    AppState& app = *g_app;
    // Плавная панорама: слой живёт доли секунды и только показывает — клики обзора
    // (нажать окно, вернуться в 1:1) на нём не работают, курсор обычный.
    if (app.glide) {
        switch (msg) {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MOUSEMOVE:
            return 0;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
    }
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);        // валидирует область; рисуем не в DC, а Direct2D
        RenderOverview(app, hwnd);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        if (app.ovRT && LOWORD(lp) && HIWORD(lp))
            app.ovRT->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        return 0;
    case WM_LBUTTONDOWN: {
        double cx = (short)LOWORD(lp), cy = (short)HIWORD(lp);
        app.ovDragHwnd = OvHitTest(app, cx, cy);   // окно под курсором или nullptr — фон
        app.ovDragLast.x = (LONG)cx; app.ovDragLast.y = (LONG)cy;
        app.ovDownPt = app.ovDragLast;
        app.ovDragMoved = false;
        // Точка захвата: за какое место окна его взяли (в мировых координатах). Дальше
        // окно ставится так, чтобы это место оставалось под курсором.
        for (auto& w : app.wins) if (w.hwnd == app.ovDragHwnd) {
            app.ovGrabX = OvWorldX(app, cx) - w.world.left;
            app.ovGrabY = OvWorldY(app, cy) - w.world.top;
            break;
        }
        SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (GetCapture() != hwnd) return 0;
        double cx = (short)LOWORD(lp), cy = (short)HIWORD(lp);
        double ddx = cx - app.ovDragLast.x, ddy = cy - app.ovDragLast.y;
        // Порог — от места нажатия, а не от прошлого события: иначе медленное
        // перетаскивание (шаги по 1–2 px) не начиналось вовсе и при отпускании
        // превращалось в клик по окну.
        if (!app.ovDragMoved && (fabs(cx - app.ovDownPt.x) > 3 || fabs(cy - app.ovDownPt.y) > 3)) app.ovDragMoved = true;
        if (app.ovDragMoved) {
            if (app.ovDragHwnd) {
                // Окно ведём по абсолютному положению курсора: точка захвата остаётся под
                // ним. Раньше складывались округлённые сдвиги каждого события — остатки
                // терялись, и окно расходилось с курсором на несколько пикселей.
                for (auto& w : app.wins) if (w.hwnd == app.ovDragHwnd) {
                    LONG ww = w.world.right - w.world.left, wh = w.world.bottom - w.world.top;
                    LONG nl = (LONG)llround(OvWorldX(app, cx) - app.ovGrabX);
                    LONG nt = (LONG)llround(OvWorldY(app, cy) - app.ovGrabY);
                    w.world = { nl, nt, nl + ww, nt + wh };
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
        HWND hit = app.ovDragHwnd;                 // окно, на котором нажали (или nullptr — фон)
        if (GetCapture() == hwnd) ReleaseCapture();
        app.ovDragHwnd = nullptr; app.ovDragMoved = false;
        // Последнее движение могло прийти вместе с отпусканием, уже после кадра: без
        // перерисовки окно «доезжало» бы эти пиксели позже, при следующем изменении.
        app.ovLastZoom = -1;
        if (!moved) {
            double cx = (short)LOWORD(lp), cy = (short)HIWORD(lp);
            if (hit) {
                ClickIntoWindow(app, hit, cx, cy, false);   // клик по окну — в само окно, зум остаётся
            } else {
                // клик по фону => приблизить точку (по центру) и вернуться в 1:1
                app.axWX = OvWorldX(app, cx); app.axCX = app.vsW / 2.0;
                app.axWY = OvWorldY(app, cy); app.axCY = app.vsH / 2.0;
                app.zoomTarget = 1.0;
            }
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        double cx = (short)LOWORD(lp), cy = (short)HIWORD(lp);
        if (HWND hit = OvHitTest(app, cx, cy)) ClickIntoWindow(app, hit, cx, cy, true);   // ПКМ — тоже в окно
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
