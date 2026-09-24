// Обзор (зум): превью, тени, обои, окно обзора. Отрисовка — Direct2D (GPU).
// Часть InfiniteDesktop.

#include "id_common.h"

using Microsoft::WRL::ComPtr;

// ---------- Полноэкранный обзор (зум через DWM-превью) ----------
double OvClientX(AppState& app, double wx) { return app.axCX + (wx - app.axWX) * app.zoom; }
double OvClientY(AppState& app, double wy) { return app.axCY + (wy - app.axWY) * app.zoom; }

// Видимый мировой прямоугольник окна (без невидимых рамок) — для превью/теней/клика,
// чтобы зазоры в обзоре совпадали с холстом.
RECT VisWorld(const TrackedWin& w) {
    return { w.world.left + w.vm.left,  w.world.top + w.vm.top,
             w.world.right - w.vm.right, w.world.bottom - w.vm.bottom };
}

void ShowOverview(AppState& app) {
    if (app.overview) return;
    // Актуальные обои на входе — но перекодируем файл (декод + ресемплинг, десятки мс)
    // только если он реально сменился, иначе вход в зум каждый раз начинался бы с задержки.
    if (WallpaperFileChanged(app)) LoadWallpaper(app);
    app.lastWallCap = GetTickCount();
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
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    // PRESENT_OPTIONS_NONE: EndDraw ждёт vblank — кадр показывается целиком в одной
    // композиции DWM. С IMMEDIATELY часть экрана успевала обновиться, часть нет — на
    // обоях, меняющих яркость каждый кадр, это выглядело как дёрганье. Чтобы не ждать
    // vblank дважды, цикл кадров пропускает DwmFlush после такого кадра (см. FrameTick).
    D2D1_HWND_RENDER_TARGET_PROPERTIES hp = D2D1::HwndRenderTargetProperties(
        hwnd, D2D1::SizeU((UINT32)rc.right, (UINT32)rc.bottom), D2D1_PRESENT_OPTIONS_NONE);
    if (FAILED(app.d2dFactory->CreateHwndRenderTarget(rp, hp, app.ovRT.GetAddressOf()))) return false;
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
static void WallLog(const wchar_t* fmt, ...) {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring dir = (n && n < MAX_PATH) ? std::wstring(buf) : L".";
    dir += L"\\InfiniteDesktop";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + L"\\wall.log";
    // Ограничиваем рост: при > 64 КБ начинаем файл заново (слайдшоу пишет строки на каждую смену).
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    bool big = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) &&
               (fa.nFileSizeHigh || fa.nFileSizeLow > 65536);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), big ? L"w, ccs=UTF-8" : L"a, ccs=UTF-8") || !f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fwprintf(f, L"%02d:%02d:%02d  ", st.wHour, st.wMinute, st.wSecond);
    va_list ap; va_start(ap, fmt); vfwprintf(f, fmt, ap); va_end(ap);
    fwprintf(f, L"\n");
    fclose(f);
}

static BOOL CALLBACK CollectWorkerW(HWND h, LPARAM lp) {
    wchar_t cls[32] = {};
    GetClassNameW(h, cls, 32);
    if (!lstrcmpW(cls, L"WorkerW") && IsWindowVisible(h)) ((std::vector<HWND>*)lp)->push_back(h);
    return TRUE;
}

static bool TryCaptureWindow(AppState& app, HWND hw);

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
BOOL CALLBACK WallMonProc(HMONITOR hm, HDC, LPRECT, LPARAM lp) {
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

// Мягкая тень под окно: несколько расширяющихся скруглённых слоёв чёрного с малой
// альфой, накапливающихся в градиент (как тень окна Windows). Превью ложится сверху.
// Геометрия та же, что была в GDI-версии (CreateRoundRectRgn задаёт ДИАМЕТР скругления,
// отсюда /2 для радиуса D2D).
void DrawShadow(AppState& app, const RECT& r, int radius) {
    for (int i = 7; i >= 1; --i) {
        float dy = (float)(i / 2 + 1);
        float rad = (radius + i) / 2.0f;
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
            D2D1::RectF((float)(r.left - i), (float)(r.top - i) + dy,
                        (float)(r.right + i), (float)(r.bottom + i) + dy), rad, rad);
        app.ovRT->FillRoundedRectangle(rr, app.ovShadowBrush.Get());
    }
}

void RenderOverview(AppState& app, HWND hwnd) {
    if (!EnsureOvTarget(app, hwnd)) return;
    ID2D1HwndRenderTarget* rt = app.ovRT.Get();
    RECT rc; GetClientRect(hwnd, &rc);
    D2D1_SIZE_U want = D2D1::SizeU((UINT32)rc.right, (UINT32)rc.bottom), have = rt->GetPixelSize();
    if (want.width != have.width || want.height != have.height) rt->Resize(want);

    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    if (EnsureWallD2D(app)) {
        // Обои пользователя, затемнённые тем сильнее, чем дальше отдалили.
        double dim = (1.0 - app.zoom) / (1.0 - ZOOM_MIN);
        if (dim < 0) dim = 0; if (dim > 1) dim = 1;
        float opacity = (float)(1.0 - dim * 0.7);   // не темнее ~70%
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

    // Тени под окна (без рамок) — превью с закруглениями Win11 лягут поверх.
    int radius = (int)(8.0 * app.zoom + 0.5);
    if (radius < 2) radius = 2;
    for (auto& w : app.wins) {
        RECT vw = VisWorld(w);
        RECT d = { (LONG)llround(OvClientX(app, vw.left)),  (LONG)llround(OvClientY(app, vw.top)),
                   (LONG)llround(OvClientX(app, vw.right)), (LONG)llround(OvClientY(app, vw.bottom)) };
        if (d.right <= d.left || d.bottom <= d.top) continue;
        // Окно целиком за пределами обзора (с запасом на размах тени) — не рисуем.
        if (d.right < -16 || d.left > rc.right + 16 || d.bottom < -16 || d.top > rc.bottom + 16) continue;
        DrawShadow(app, d, radius);
    }

    if (app.ovTextFormat && app.ovTextBrush) {
        wchar_t info[96];
        wsprintfW(info, L"Обзор  %d%%   клик по окну — нажать • клик по фону — приблизить • колесо — зум • СКМ — двигать", (int)(app.zoom * 100));
        rt->DrawText(info, (UINT32)lstrlenW(info), app.ovTextFormat.Get(),
                     D2D1::RectF(24.0f, 20.0f, (float)rc.right, 60.0f), app.ovTextBrush.Get());
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
    // Оверлей — прозрачен для hit-теста: WS_EX_TRANSPARENT (ядро пропускает такие окна)
    // плюс ответ HTTRANSPARENT на WM_NCHITTEST (см. OvProc) — на всякий случай.
    LONG_PTR ex = GetWindowLongPtrW(app.ov, GWL_EXSTYLE);
    SetWindowLongPtrW(app.ov, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT);
    app.ovPassThrough = true;
    INPUT in[2] = {};
    in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = right ? MOUSEEVENTF_RIGHTUP   : MOUSEEVENTF_LEFTUP;
    SendInput(2, in, sizeof(INPUT));
    PumpFor(30);   // маршрутизация инжектированного ввода асинхронна — даём ей пройти
    app.ovPassThrough = false;
    SetWindowLongPtrW(app.ov, GWL_EXSTYLE, ex);
    if (zoomed) SetCursorPos(savedCur.x, savedCur.y);
    else        SetWindowPos(h, nullptr, cur.left, cur.top, 0, 0, F | SWP_NOZORDER);
}

LRESULT CALLBACK OvProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    AppState& app = *g_app;
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        // На время инжекции клика оверлей прозрачен для hit-теста: ввод идёт в реальное
        // окно, подставленное под курсор (см. ClickIntoWindow).
        if (app.ovPassThrough) return HTTRANSPARENT;
        break;
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
        HWND hit = app.ovDragHwnd;                 // окно, на котором нажали (или nullptr — фон)
        if (GetCapture() == hwnd) ReleaseCapture();
        app.ovDragHwnd = nullptr; app.ovDragMoved = false;
        if (!moved) {
            double cx = (short)LOWORD(lp), cy = (short)HIWORD(lp);
            if (hit) {
                ClickIntoWindow(app, hit, cx, cy, false);   // клик по окну — в само окно, зум остаётся
            } else {
                // клик по фону => приблизить точку (по центру) и вернуться в 1:1
                double wx = app.axWX + (cx - app.axCX) / app.zoom;
                double wy = app.axWY + (cy - app.axCY) / app.zoom;
                app.axWX = wx; app.axCX = app.vsW / 2.0;
                app.axWY = wy; app.axCY = app.vsH / 2.0;
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
