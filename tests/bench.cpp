// Стенд плавности панорамы: tests.exe --bench [окон] [секунд]
//
// Измеряет то, что нельзя посчитать на бумаге, — сколько длится кадр панорамы и сколько
// кадров пропущено. Чтобы не трогать окна пользователя, стенд сам запускает отдельные
// процессы с окнами (--dummy) и двигает только их тем же RepositionAll, что и программа,
// в том же ритме (кадр → ожидание композиции DWM). Часть окон можно сделать «тяжёлыми»:
// они задерживаются в обработке перемещения, как настоящие большие приложения.

#include "../src/id_common.h"
#include <cstdio>

static const wchar_t* DUMMY_CLASS = L"InfiniteDesktopBenchDummy";
static int g_dummyDelayUs = 0;

static double NowMs() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

// ---------- Процесс-окно ----------
static LRESULT CALLBACK DummyProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_WINDOWPOSCHANGED:
        if (g_dummyDelayUs) {          // «тяжёлое» приложение: занято в обработке перемещения
            double end = NowMs() + g_dummyDelayUs / 1000.0;
            while (NowMs() < end) {}
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HBRUSH b = CreateSolidBrush(g_dummyDelayUs ? RGB(120, 60, 40) : RGB(40, 70, 110));
        FillRect(dc, &rc, b); DeleteObject(b);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
        const wchar_t* t = g_dummyDelayUs ? L"bench: slow window" : L"bench window";
        DrawTextW(dc, t, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int DummyMain(int delayUs, int x, int y, int w, int h, int flags) {
    g_dummyDelayUs = delayUs;
    EnableDpiAwareness();
    WNDCLASSW c = {};
    c.lpfnWndProc = DummyProc; c.hInstance = GetModuleHandleW(nullptr);
    c.hCursor = LoadCursorW(nullptr, IDC_ARROW); c.lpszClassName = DUMMY_CLASS;
    RegisterClassW(&c);
    HWND hw = CreateWindowExW(((flags & 1) ? WS_EX_TOPMOST : 0) | ((flags & 2) ? WS_EX_LAYERED : 0), DUMMY_CLASS, L"InfiniteDesktop bench", WS_OVERLAPPEDWINDOW,
                              x, y, w, h, nullptr, nullptr, c.hInstance, nullptr);
    if (!hw) return 1;
    if (flags & 2) SetLayeredWindowAttributes(hw, 0, 255, LWA_ALPHA);
    if (flags & 4) { BOOL dark = TRUE; DwmSetWindowAttribute(hw, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark)); }
    ShowWindow(hw, SW_SHOWNOACTIVATE);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}

// ---------- Замер ----------
struct Series {
    std::vector<double> v;
    void add(double x) { v.push_back(x); }
    double pct(double p) { if (v.empty()) return 0; std::vector<double> s = v; std::sort(s.begin(), s.end());
                           return s[(size_t)((s.size() - 1) * p)]; }
    double avg() { double a = 0; for (double x : v) a += x; return v.empty() ? 0 : a / v.size(); }
    double mx()  { double a = 0; for (double x : v) a = max(a, x); return a; }
};

static BOOL CALLBACK CollectDummies(HWND h, LPARAM lp) {
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (!lstrcmpW(cls, DUMMY_CLASS) && IsWindowVisible(h)) ((std::vector<HWND>*)lp)->push_back(h);
    return TRUE;
}

static void PumpMessages() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
}

// Один прогон: камера ходит по синусоиде, окна остаются в кадре и двигаются каждый кадр.
static void Run(AppState& app, const wchar_t* name, double seconds, double periodMs, bool withMap) {
    Series frame, move, map;
    app.camX = app.camY = 0;
    double t0 = NowMs(), last = t0, lastMap = 0;
    for (;;) {
        double now = NowMs(), t = (now - t0) / 1000.0;
        if (t >= seconds) break;
        app.camX = 260.0 * sin(t * 6.2831853 / 1.3);
        app.camY = 90.0 * sin(t * 6.2831853 / 0.9);
        double a = NowMs();
        RepositionAll(app);
        double b = NowMs();
        move.add(b - a);
        if (withMap && b - lastMap >= 16.0) {       // как RefreshHudThrottled
            lastMap = b;
            RefreshHud(app);
            PumpMessages();                         // WM_PAINT миникарты
            map.add(NowMs() - b);
        }
        DwmFlush();
        double e = NowMs();
        frame.add(e - last);
        last = e;
    }
    app.camX = app.camY = 0;
    RepositionAll(app);
    int dropped = 0;
    for (double x : frame.v) if (x > periodMs * 1.5) ++dropped;
    wprintf(L"%-34s fps %6.1f | frame ms avg %5.2f p95 %5.2f p99 %5.2f max %6.2f | dropped %4d of %5d (%4.1f%%) | move ms avg %5.2f p95 %5.2f max %6.2f",
            name, 1000.0 / frame.avg(), frame.avg(), frame.pct(0.95), frame.pct(0.99), frame.mx(),
            dropped, (int)frame.v.size(), 100.0 * dropped / max(1, (int)frame.v.size()),
            move.avg(), move.pct(0.95), move.mx());
    if (withMap) wprintf(L" | map ms avg %5.2f max %5.2f", map.avg(), map.mx());
    wprintf(L"\n");
}

// ---------- Сравнение способов перемещения ----------
// Двигает окна напрямую, в обход RepositionAll, чтобы сравнить сами вызовы системы.
// «late» — сколько окон к моменту следующей композиции ещё не стояло на новом месте
// (для асинхронных способов: окно применяет перемещение, когда до него дойдёт его поток).
enum { M_BATCH_SYNC, M_EACH_SYNC, M_EACH_ASYNC, M_BATCH_ASYNC, M_THREADS,
       M_BATCH_NOCOPY, M_BATCH_NOREDRAW, M_BATCH_BOTH, M_COUNT };
static const wchar_t* MODE_NAME[M_COUNT] = {
    L"batch, sync", L"one by one, sync", L"one by one, async", L"batch, async", L"sync on 4 threads",
    L"batch + NOCOPYBITS", L"batch + NOREDRAW", L"batch + both" };

struct MoveJob { HWND h; int x, y; };
static const UINT F_SYNC = SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING;

struct Pool {
    static const int N = 4;
    HANDLE th[N] = {}, go[N] = {}, done[N] = {};
    const std::vector<MoveJob>* jobs = nullptr;
    volatile LONG quit = 0;
    struct Arg { Pool* p; int i; } args[N];
    static DWORD WINAPI Thread(LPVOID a) {
        Arg* x = (Arg*)a;
        for (;;) {
            WaitForSingleObject(x->p->go[x->i], INFINITE);
            if (x->p->quit) return 0;
            const std::vector<MoveJob>& j = *x->p->jobs;
            for (size_t k = x->i; k < j.size(); k += N) SetWindowPos(j[k].h, nullptr, j[k].x, j[k].y, 0, 0, F_SYNC);
            SetEvent(x->p->done[x->i]);
        }
    }
    void start() { for (int i = 0; i < N; ++i) { go[i] = CreateEventW(nullptr, FALSE, FALSE, nullptr); done[i] = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                                                  args[i] = { this, i }; th[i] = CreateThread(nullptr, 0, Thread, &args[i], 0, nullptr); } }
    void run(const std::vector<MoveJob>& j) { jobs = &j; for (int i = 0; i < N; ++i) SetEvent(go[i]); WaitForMultipleObjects(N, done, TRUE, INFINITE); }
    void stop() { quit = 1; for (int i = 0; i < N; ++i) SetEvent(go[i]); WaitForMultipleObjects(N, th, TRUE, 2000); }
};

static void MoveAll(int mode, const std::vector<MoveJob>& jobs, Pool& pool) {
    switch (mode) {
    case M_BATCH_SYNC: case M_BATCH_ASYNC: case M_BATCH_NOCOPY: case M_BATCH_NOREDRAW: case M_BATCH_BOTH: {
        // Без SWP_NOSENDCHANGING: с ним DeferWindowPos возвращает ошибку 87 и пакет не собирается.
        UINT f = SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | (mode == M_BATCH_ASYNC ? SWP_ASYNCWINDOWPOS : 0);
        if (mode == M_BATCH_NOCOPY   || mode == M_BATCH_BOTH) f |= SWP_NOCOPYBITS;
        if (mode == M_BATCH_NOREDRAW || mode == M_BATCH_BOTH) f |= SWP_NOREDRAW;
        HDWP d = BeginDeferWindowPos((int)jobs.size());
        for (auto& j : jobs) { if (!d) break; d = DeferWindowPos(d, j.h, nullptr, j.x, j.y, 0, 0, f); }
        if (d) EndDeferWindowPos(d);
        break;
    }
    case M_EACH_SYNC:  for (auto& j : jobs) SetWindowPos(j.h, nullptr, j.x, j.y, 0, 0, F_SYNC); break;
    case M_EACH_ASYNC: for (auto& j : jobs) SetWindowPos(j.h, nullptr, j.x, j.y, 0, 0, F_SYNC | SWP_ASYNCWINDOWPOS); break;
    case M_THREADS:    pool.run(jobs); break;
    }
}

int BenchModes(int count, double seconds, int slowCount, int slowUs);

static void RunMode(int mode, std::vector<HWND>& hs, std::vector<RECT>& home, double seconds, double periodMs, Pool& pool) {
    Series frame, move;
    std::vector<MoveJob> jobs(hs.size());
    long late = 0, checks = 0;
    double t0 = NowMs(), last = t0;
    for (;;) {
        double now = NowMs(), t = (now - t0) / 1000.0;
        if (t >= seconds) break;
        int dx = (int)lround(260.0 * sin(t * 6.2831853 / 1.3)), dy = (int)lround(90.0 * sin(t * 6.2831853 / 0.9));
        for (size_t i = 0; i < hs.size(); ++i) jobs[i] = { hs[i], home[i].left - dx, home[i].top - dy };
        double a = NowMs();
        MoveAll(mode, jobs, pool);
        move.add(NowMs() - a);
        DwmFlush();
        double e = NowMs();
        frame.add(e - last);
        last = e;
        for (auto& j : jobs) {                       // успело ли окно встать к композиции
            RECT r;
            if (GetWindowRect(j.h, &r)) { ++checks; if (r.left != j.x || r.top != j.y) ++late; }
        }
    }
    Sleep(60);
    for (size_t i = 0; i < hs.size(); ++i) SetWindowPos(hs[i], nullptr, home[i].left, home[i].top, 0, 0, F_SYNC);
    int dropped = 0;
    for (double x : frame.v) if (x > periodMs * 1.5) ++dropped;
    wprintf(L"%-24s fps %6.1f | frame ms avg %5.2f p95 %5.2f p99 %5.2f max %6.2f | dropped %5.1f%% | move ms avg %5.2f p95 %5.2f max %6.2f | late %5.2f%%\n",
            MODE_NAME[mode], 1000.0 / frame.avg(), frame.avg(), frame.pct(0.95), frame.pct(0.99), frame.mx(),
            100.0 * dropped / max(1, (int)frame.v.size()), move.avg(), move.pct(0.95), move.mx(),
            100.0 * late / max(1L, checks));
}

// Запустить процессы-окна и дождаться их. Возвращает задание, закрытие которого их снимает.
static HANDLE SpawnDummies(int count, int slowCount, int slowUs, std::vector<HWND>& hs) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int cols = 4, ww = 460, wh = 320;
    for (int i = 0; i < count; ++i) {
        int x = wa.left + 320 + (i % cols) * (ww + 30) + (i / (cols * 3)) * 40;
        int y = wa.top + 140 + ((i / cols) % 3) * (wh + 30) + (i / (cols * 3)) * 40;
        wchar_t cmd[512];
        swprintf_s(cmd, L"\"%s\" --dummy %d %d %d %d %d", exe, i < slowCount ? slowUs : 0, x, y, ww, wh);
        STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = {};
        if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
            AssignProcessToJobObject(job, pi.hProcess);
            ResumeThread(pi.hThread);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        }
    }
    for (int tries = 0; tries < 100 && (int)hs.size() < count; ++tries) { Sleep(50); hs.clear(); EnumWindows(CollectDummies, (LPARAM)&hs); }
    Sleep(400);
    return job;
}

int BenchModes(int count, double seconds, int slowCount, int slowUs) {
    EnableDpiAwareness();
    DEVMODEW dm = {}; dm.dmSize = sizeof(dm);
    double hz = 60.0;
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) hz = dm.dmDisplayFrequency;
    std::vector<HWND> hs;
    HANDLE job = SpawnDummies(count, slowCount, slowUs, hs);
    if ((int)hs.size() < count) { wprintf(L"only %d of %d bench windows appeared\n", (int)hs.size(), count); CloseHandle(job); return 1; }
    std::vector<RECT> home(hs.size());
    for (size_t i = 0; i < hs.size(); ++i) GetWindowRect(hs[i], &home[i]);
    wprintf(L"display %.0f Hz (frame budget %.2f ms), %d windows, %d of them slow (%.1f ms each)\n",
            hz, 1000.0 / hz, count, slowCount, slowUs / 1000.0);
    Pool pool; pool.start();
    for (int m = 0; m < M_COUNT; ++m) RunMode(m, hs, home, seconds, 1000.0 / hz, pool);
    pool.stop();
    CloseHandle(job);
    return 0;
}

// ---------- Сравнение картинки: рабочий стол и слой превью ----------
// Плавная панорама годится, только если слой превью неотличим от стола под ним: иначе при
// каждом начале и конце движения картинка «подпрыгивала» бы. Снимаем экран до и во время.
static void GrabScreen(const RECT& r, std::vector<BYTE>& out) {
    int w = r.right - r.left, h = r.bottom - r.top;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    HDC sdc = GetDC(nullptr);
    HDC mdc = CreateCompatibleDC(sdc);
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mdc, dib);
    BitBlt(mdc, 0, 0, w, h, sdc, r.left, r.top, SRCCOPY | CAPTUREBLT);
    GdiFlush();
    out.assign((BYTE*)bits, (BYTE*)bits + (size_t)w * h * 4);
    SelectObject(mdc, old); DeleteObject(dib); DeleteDC(mdc); ReleaseDC(nullptr, sdc);
}

static void SaveBgra(AppState& app, const std::vector<BYTE>& px, int w, int h, const std::wstring& path) {
    if (!app.wicFactory) return;
    Microsoft::WRL::ComPtr<IWICBitmap> src;
    Microsoft::WRL::ComPtr<IWICStream> stream;
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> enc;
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(app.wicFactory->CreateBitmapFromMemory((UINT)w, (UINT)h, GUID_WICPixelFormat32bppBGR, (UINT)w * 4,
                                                      (UINT)px.size(), (BYTE*)px.data(), &src)) ||
        FAILED(app.wicFactory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(app.wicFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) return;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    frame->SetSize((UINT)w, (UINT)h);
    frame->SetPixelFormat(&fmt);
    if (SUCCEEDED(frame->WriteSource(src.Get(), nullptr))) { frame->Commit(); enc->Commit(); }
}

static std::wstring shotDir;   // куда сохранять снимки сравнения (пусто — не сохранять)

static void ReportDiff(AppState& app, const RECT& area, const std::vector<BYTE>& a, const std::vector<BYTE>& b, const std::wstring& dir) {
    int w = area.right - area.left, h = area.bottom - area.top;
    if (a.size() != b.size() || a.empty()) { wprintf(L"picture check: capture failed\n"); return; }
    size_t differ = 0, strong = 0; double sum = 0; int worst = 0;
    std::vector<BYTE> diff(a.size(), 0);
    for (size_t i = 0; i < a.size(); i += 4) {
        int d = max(abs(a[i] - b[i]), max(abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2])));
        if (d > 2) ++differ;
        if (d > 24) ++strong;
        sum += d; worst = max(worst, d);
        BYTE v = (BYTE)min(255, d * 4);            // усиленная разница — для просмотра глазами
        diff[i] = diff[i + 1] = diff[i + 2] = v;
    }
    size_t n = a.size() / 4;
    wprintf(L"picture check (desktop vs preview layer, camera not moved): %.3f%% pixels differ, %.3f%% strongly, mean %.3f, max %d\n",
            100.0 * differ / n, 100.0 * strong / n, sum / n, worst);
    if (!dir.empty()) {
        SaveBgra(app, a, w, h, dir + L"\\before.png");
        SaveBgra(app, b, w, h, dir + L"\\during.png");
        SaveBgra(app, diff, w, h, dir + L"\\diff.png");
    }
}

// ---------- Калибровка тени: tests.exe --shadow-calib <каталог> ----------
// Снимает настоящую тень окна Windows на белом фоне — для активного и неактивного окна —
// и печатает её профили по четырём сторонам. По этим числам строится тень превью
// (src/id_shadow.cpp): подгонять её на глаз бессмысленно, а так она совпадает с системной.
static LRESULT CALLBACK WhiteProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) { RECT r; GetClientRect(h, &r); FillRect((HDC)w, &r, (HBRUSH)GetStockObject(WHITE_BRUSH)); return 1; }
    return DefWindowProcW(h, m, w, l);
}
static void Activate(HWND h) {
    HWND cur = GetForegroundWindow();
    DWORD me = GetCurrentThreadId(), other = cur ? GetWindowThreadProcessId(cur, nullptr) : 0;
    if (other && other != me) AttachThreadInput(me, other, TRUE);
    BringWindowToTop(h); SetForegroundWindow(h);
    if (other && other != me) AttachThreadInput(me, other, FALSE);
}
int ShadowCalib(const std::wstring& outDir) {
    EnableDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSW c = {}; c.lpfnWndProc = WhiteProc; c.hInstance = hi; c.lpszClassName = L"InfiniteDesktopCalibWhite"; RegisterClassW(&c);
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    wchar_t szEnv[32] = {};               // ID_CALIB_SIZE=ШxВ — размер калибровочного окна
    int WW = 640, WH = 420;
    if (GetEnvironmentVariableW(L"ID_CALIB_SIZE", szEnv, 32) > 0) swscanf_s(szEnv, L"%dx%d", &WW, &WH);
    const int M = 130, BW = WW + 2 * M + 60, BH = WH + 2 * M + 60;
    int bx = wa.left + (wa.right - wa.left - BW) / 2, by = wa.top + (wa.bottom - wa.top - BH) / 2;
    HWND prev = GetForegroundWindow();
    HWND white = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, c.lpszClassName, L"", WS_POPUP | WS_VISIBLE,
                                 bx, by, BW, BH, nullptr, nullptr, hi, nullptr);
    PumpMessages();
    wchar_t exe[MAX_PATH] = {}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    wchar_t cmd[512];
    swprintf_s(cmd, L"\"%s\" --dummy 0 %d %d %d %d %d", exe, bx + (BW - WW) / 2, by + (BH - WH) / 2, WW, WH,
               1 | (GetEnvironmentVariableW(L"ID_CALIB_LAYERED", szEnv, 32) > 0 ? 2 : 0)
                 | (GetEnvironmentVariableW(L"ID_CALIB_DARK", szEnv, 32) > 0 ? 4 : 0));   // тёмный заголовок — другая тень
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) return 1;
    AssignProcessToJobObject(job, pi.hProcess); ResumeThread(pi.hThread);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    std::vector<HWND> hs;
    for (int t = 0; t < 100 && hs.empty(); ++t) { Sleep(50); PumpMessages(); hs.clear(); EnumWindows(CollectDummies, (LPARAM)&hs); }
    if (hs.empty()) { wprintf(L"calibration window did not appear\n"); CloseHandle(job); return 1; }
    HWND win = hs[0];
    AppState app; InitGraphics(app);
    FILE* out = nullptr;
    _wfopen_s(&out, (outDir + L"\\shadow.txt").c_str(), L"w");
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) Activate(win);
        for (int i = 0; i < 40; ++i) { Sleep(20); PumpMessages(); }
        bool active = GetForegroundWindow() == win;
        RECT fr;
        DwmGetWindowAttribute(win, DWMWA_EXTENDED_FRAME_BOUNDS, &fr, sizeof(fr));
        RECT area = { fr.left - M, fr.top - M, fr.right + M, fr.bottom + M };
        std::vector<BYTE> px;
        GrabScreen(area, px);
        int aw = area.right - area.left, ah = area.bottom - area.top;
        auto alpha = [&](int x, int y) {     // доля затемнения белого фона в точке (координаты снимка)
            if (x < 0 || y < 0 || x >= aw || y >= ah) return 0.0;
            return 1.0 - px[((size_t)y * aw + x) * 4 + 1] / 255.0;
        };
        int L = M, T = M, Rr = aw - M, B = ah - M;     // видимая рамка окна в координатах снимка
        int mx = (L + Rr) / 2, my = (T + B) / 2;
        const wchar_t* st = active ? L"active" : L"inactive";
        wprintf(L"%s window %dx%d (pass %d)\n", st, Rr - L, B - T, pass);
        const wchar_t* names[4] = { L"left", L"right", L"top", L"bottom" };
        for (int side = 0; side < 4; ++side) {
            if (out) fwprintf(out, L"%s %s", st, names[side]);
            wprintf(L"  %-6s", names[side]);
            for (int d = 1; d <= 110; ++d) {
                double a = side == 0 ? alpha(L - d, my) : side == 1 ? alpha(Rr - 1 + d, my)
                         : side == 2 ? alpha(mx, T - d) : alpha(mx, B - 1 + d);
                if (out) fwprintf(out, L" %.4f", a);
                if (d <= 40 && (d % 3 == 1)) wprintf(L" %.3f", a);
            }
            if (out) fwprintf(out, L"\n");
            wprintf(L"\n");
        }
        // Угол: сетка 12x12 с шагом 8 px от угла рамки наружу и немного внутрь — проверка модели.
        for (int corner = 0; corner < 2; ++corner) {         // 0 — левый нижний, 1 — левый верхний
            if (out) fwprintf(out, L"%s corner%d\n", st, corner);
            for (int j = -2; j < 12; ++j) {
                for (int i = -2; i < 12; ++i) {
                    int x = L - 1 - i * 8, y = corner == 0 ? B + j * 8 : T - 1 - j * 8;
                    if (out) fwprintf(out, L" %.4f", alpha(x, y));
                }
                if (out) fwprintf(out, L"\n");
            }
        }
        SaveBgra(app, px, aw, ah, outDir + (pass ? L"\\shadow_active.png" : L"\\shadow_inactive.png"));
    }
    if (out) fclose(out);
    if (prev) Activate(prev);            // вернуть фокус туда, где он был
    CloseHandle(job);
    DestroyWindow(white);
    app.wicFactory.Reset(); app.d2dFactory.Reset(); app.dwFactory.Reset(); app.ovTextFormat.Reset();   // до CoUninitialize
    CoUninitialize();
    return 0;
}

// ---------- Слой превью на настоящем рабочем столе: tests.exe --bench-real [каталог] ----------
// Берёт окна пользователя так же, как программа, и показывает слой превью, НЕ сдвигая
// камеру: ни одно окно не перемещается. Сравнивает экран до и во время — то, что
// пользователь видит в момент начала панорамы, — и печатает по каждому окну, есть ли у
// него тень и насколько картинка вокруг него отличается.
int BenchReal(const std::wstring& outDir) {
    EnableDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    {
        AppState app;
        g_app = &app;
        UpdateVirtualScreen(app);
        app.hudVisible = false;
        InitGraphics(app); InitUi(app);
        HINSTANCE hi = GetModuleHandleW(nullptr);
        WNDCLASSW oc = {};
        oc.hInstance = hi; oc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        oc.lpfnWndProc = OvProc; oc.lpszClassName = L"InfiniteDesktopBenchOverview"; RegisterClassW(&oc);
        app.ov = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            L"InfiniteDesktopBenchOverview", L"", WS_POPUP, app.vsX, app.vsY, app.vsW, app.vsH, nullptr, nullptr, hi, nullptr);
        CreateOverviewBackdrop(app, hi);
        wchar_t st[8] = {};                       // ID_BENCH_STATIC=1 — как с обычными обоями: без подложки живых обоев
        if (GetEnvironmentVariableW(L"ID_BENCH_STATIC", st, 8) > 0 && app.ovBack) { DestroyWindow(app.ovBack); app.ovBack = nullptr; }
        LoadWallpaper(app);
        RenderOverview(app, app.ov);
        RebuildWindowList(app);                   // окна пользователя — только чтение
        RECT scr = { app.vsX, app.vsY, app.vsX + app.vsW, app.vsY + app.vsH };
        std::vector<BYTE> before, during;
        GrabScreen(scr, before);
        if (!BeginGlide(app)) { wprintf(L"BeginGlide failed\n"); }
        else {
            GlideFrame(app); GlideFrame(app); GlideFrame(app); DwmFlush(); DwmFlush();
            GrabScreen(scr, during);
            wprintf(L"%d tracked windows, wallpaper %s\n", (int)app.wins.size(), app.ovLiveBg ? L"LIVE" : L"static");
            ReportDiff(app, scr, before, during, outDir);
            int aw = scr.right - scr.left, ah = scr.bottom - scr.top;
            HWND fg = GetForegroundWindow();
            for (auto& w : app.wins) {
                RECT fr;
                if (FAILED(DwmGetWindowAttribute(w.hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &fr, sizeof(fr)))) continue;
                // Полоса 40 px вокруг рамки: среднее отличие «до» и «во время».
                double sum = 0; long n = 0;
                for (int y = fr.top - 40; y < fr.bottom + 40; y += 2)
                    for (int x = fr.left - 40; x < fr.right + 40; x += 2) {
                        if (x >= fr.left && x < fr.right && y >= fr.top && y < fr.bottom) continue;
                        if (x < scr.left || y < scr.top || x >= scr.right || y >= scr.bottom) continue;
                        size_t i = ((size_t)(y - scr.top) * aw + (x - scr.left)) * 4;
                        sum += abs(before[i] - during[i]) + abs(before[i + 1] - during[i + 1]) + abs(before[i + 2] - during[i + 2]);
                        ++n;
                    }
                // Тень по сторонам: насколько слой светлее стола вплотную к рамке (3 px) и
                // вдали от неё (60 px). Нет тени в слое — у рамки слой заметно светлее.
                auto edge = [&](int side, int d) {      // 0 слева, 1 справа, 2 сверху, 3 снизу
                    double sb = 0, sd = 0; long cnt = 0;
                    for (int t = 0; t < 200; ++t) {
                        int x, y;
                        if (side < 2) { y = fr.top + (fr.bottom - fr.top) * (t + 1) / 201; x = side == 0 ? fr.left - d : fr.right - 1 + d; }
                        else          { x = fr.left + (fr.right - fr.left) * (t + 1) / 201; y = side == 2 ? fr.top - d : fr.bottom - 1 + d; }
                        if (x < scr.left || y < scr.top || x >= scr.right || y >= scr.bottom) continue;
                        size_t i = ((size_t)(y - scr.top) * aw + (x - scr.left)) * 4;
                        sb += before[i] + before[i + 1] + before[i + 2];
                        sd += during[i] + during[i + 1] + during[i + 2];
                        ++cnt;
                    }
                    return cnt ? (sd - sb) / (3.0 * cnt) : 0.0;
                };
                wchar_t title[40] = {}; GetWindowTextW(w.hwnd, title, 30);
                wprintf(L"    layer minus desktop at 3 px / 60 px:  left %+.1f / %+.1f   right %+.1f / %+.1f   top %+.1f / %+.1f   bottom %+.1f / %+.1f\n",
                        edge(0, 3), edge(0, 60), edge(1, 3), edge(1, 60), edge(2, 3), edge(2, 60), edge(3, 3), edge(3, 60));
                wprintf(L"  %-30s %4dx%-4d %-8s shadow: source %s, preview %s, built %s, pads %d,%d,%d,%d | around the frame differs by %.2f\n",
                        title, fr.right - fr.left, fr.bottom - fr.top, w.hwnd == fg ? L"ACTIVE" : L"",
                        w.shadowSrc ? L"yes" : L"NO", w.thumbShadow ? L"yes" : L"NO", w.shadowActive ? L"active" : L"inactive",
                        (int)w.shadowPad.left, (int)w.shadowPad.top, (int)w.shadowPad.right, (int)w.shadowPad.bottom,
                        n ? sum / (3.0 * n) : 0.0);
            }
            // Зависит ли тень от времени: тот же слой спустя 0,7 с и при повторном входе.
            auto around = [&](const std::vector<BYTE>& shot) {
                double total = 0; long cnt = 0;
                for (auto& w : app.wins) {
                    RECT fr;
                    if (FAILED(DwmGetWindowAttribute(w.hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &fr, sizeof(fr)))) continue;
                    for (int y = fr.top - 40; y < fr.bottom + 40; y += 3)
                        for (int x = fr.left - 40; x < fr.right + 40; x += 3) {
                            if (x >= fr.left && x < fr.right && y >= fr.top && y < fr.bottom) continue;
                            if (x < scr.left || y < scr.top || x >= scr.right || y >= scr.bottom) continue;
                            size_t i = ((size_t)(y - scr.top) * aw + (x - scr.left)) * 4;
                            total += abs(before[i] - shot[i]) + abs(before[i + 1] - shot[i + 1]) + abs(before[i + 2] - shot[i + 2]);
                            ++cnt;
                        }
                }
                return cnt ? total / (3.0 * cnt) : 0.0;
            };
            wprintf(L"difference around all frames: right after the layer appeared %.2f", around(during));
            double t0 = NowMs();
            while (NowMs() - t0 < 700) GlideFrame(app);
            GrabScreen(scr, during);
            wprintf(L", 0.7 s later %.2f", around(during));
            EndGlide(app);                        // камера не двигалась — окна остаются где были
            Sleep(1500);
            BeginGlide(app);
            GlideFrame(app); GlideFrame(app); DwmFlush(); DwmFlush();
            GrabScreen(scr, during);
            wprintf(L", second time %.2f\n", around(during));
            if (!outDir.empty()) SaveBgra(app, during, aw, ah, outDir + L"\\during2.png");
            EndGlide(app);
            // То же через обзор (зум на масштабе 1:1) и снова слой панорамы после него.
            app.zoom = app.zoomTarget = 1.0;
            app.axCX = 0; app.axCY = 0; app.axWX = app.vsX; app.axWY = app.vsY;
            ShowOverview(app);
            UpdateOverview(app); RenderOverview(app, app.ov);
            for (int i = 0; i < 30; ++i) { RenderOverview(app, app.ov); }
            GrabScreen(scr, during);
            wprintf(L"zoom overview at 1:1: %.2f", around(during));
            if (!outDir.empty()) SaveBgra(app, during, aw, ah, outDir + L"\\overview.png");
            HideOverview(app);
            Sleep(300);
            BeginGlide(app);
            GlideFrame(app); GlideFrame(app); DwmFlush(); DwmFlush();
            GrabScreen(scr, during);
            wprintf(L", panning layer after it %.2f\n", around(during));
            if (!outDir.empty()) SaveBgra(app, during, aw, ah, outDir + L"\\during3.png");
            EndGlide(app);
        }
        StopGlideMover(app);
        app.wins.clear(); app.away.clear();
        if (app.ovBack) DestroyWindow(app.ovBack);
        DestroyWindow(app.ov);
        g_app = nullptr;
    }
    CoUninitialize();
    return 0;
}

// Плавная панорама при разной доле работы рабочего потока: tests.exe --bench-glide [окон] [секунд]
int BenchGlide(int count, double seconds) {
    EnableDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    DEVMODEW dm = {}; dm.dmSize = sizeof(dm);
    double hz = 60.0;
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) hz = dm.dmDisplayFrequency;
    const double periodMs = 1000.0 / hz;
    std::vector<HWND> hs;
    HANDLE job = SpawnDummies(count, 0, 0, hs);
    if ((int)hs.size() < count) { wprintf(L"only %d of %d bench windows appeared\n", (int)hs.size(), count); CloseHandle(job); return 1; }
    {
        AppState app;
        g_app = &app;
        UpdateVirtualScreen(app);
        app.framePeriod = periodMs / 1000.0;
        app.hudVisible = false;
        for (HWND h : hs) {
            TrackedWin w;
            RECT r, ext; GetWindowRect(h, &r);
            w.hwnd = h; w.world = r; w.vm = { 0, 0, 0, 0 }; w.pinned = false;
            if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &ext, sizeof(ext))))
                w.vm = { max(0L, ext.left - r.left), max(0L, ext.top - r.top), max(0L, r.right - ext.right), max(0L, r.bottom - ext.bottom) };
            app.wins.push_back(std::move(w));
        }
        InitGraphics(app); InitUi(app);
        HINSTANCE hi = GetModuleHandleW(nullptr);
        WNDCLASSW oc = {};
        oc.hInstance = hi; oc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        oc.lpfnWndProc = OvProc; oc.lpszClassName = L"InfiniteDesktopBenchOverview"; RegisterClassW(&oc);
        app.ov = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            L"InfiniteDesktopBenchOverview", L"", WS_POPUP, app.vsX, app.vsY, app.vsW, app.vsH, nullptr, nullptr, hi, nullptr);
        CreateOverviewBackdrop(app, hi);
        LoadWallpaper(app);
        RenderOverview(app, app.ov);          // прогрев, как в программе
        wprintf(L"display %.0f Hz (frame budget %.2f ms), %d windows\n", hz, periodMs, count);
        const double budgets[] = { 0.0, 0.15, 0.3, 0.5, 0.8, 0.3, 0.0 };
        for (double bud : budgets) {
            app.glideBudget = bud;
            app.camX = app.camY = 0;
            for (auto& w : app.wins) w.parkedOff = false;
            RepositionAll(app);
            Sleep(200);
            if (!BeginGlide(app)) { wprintf(L"BeginGlide failed\n"); break; }
            Series frame, lag, work;
            double t0 = NowMs(), last = t0;
            for (;;) {
                double now = NowMs(), t = (now - t0) / 1000.0;
                if (t >= seconds) break;
                app.camX = 260.0 * sin(t * 6.2831853 / 1.3);
                app.camY = 90.0 * sin(t * 6.2831853 / 0.9);
                GlideFrame(app);
                double e = NowMs();
                frame.add(e - last);
                last = e;
                LONG cx = (LONG)llround(app.camX), cy = (LONG)llround(app.camY);
                double worst = 0;
                for (auto& w : app.wins) {
                    RECT r;
                    if (GetWindowRect(w.hwnd, &r))
                        worst = max(worst, max(fabs((double)(r.left - (w.world.left - cx))), fabs((double)(r.top - (w.world.top - cy)))));
                }
                lag.add(worst);
                work.add(NowMs() - e);        // сколько заняла сама проверка отставания (она тоже берёт блокировку)
            }
            double s0 = NowMs();
            EndGlide(app);
            double leave = NowMs() - s0;
            int dropped = 0;
            for (double x : frame.v) if (x > periodMs * 1.5) ++dropped;
            wprintf(L"thread share %.2f | fps %6.1f | frame ms avg %5.2f p95 %5.2f p99 %5.2f max %6.2f | dropped %5.1f%% | real windows behind avg %4.0f max %4.0f px | leave %5.1f ms\n",
                    bud, 1000.0 / frame.avg(), frame.avg(), frame.pct(0.95), frame.pct(0.99), frame.mx(),
                    100.0 * dropped / max(1, (int)frame.v.size()), lag.avg(), lag.mx(), leave);
        }
        app.camX = app.camY = 0;
        RepositionAll(app);
        StopGlideMover(app);
        app.wins.clear();
        if (app.ovBack) DestroyWindow(app.ovBack);
        DestroyWindow(app.ov);
        g_app = nullptr;
    }
    CloseHandle(job);
    CoUninitialize();
    return 0;
}

int Bench(int count, double seconds, int slowCount, int slowUs) {
    EnableDpiAwareness();
    wchar_t sd[MAX_PATH] = {};          // ID_BENCH_SHOTS=<каталог> — сохранить снимки сравнения
    if (GetEnvironmentVariableW(L"ID_BENCH_SHOTS", sd, MAX_PATH)) shotDir = sd;
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    // Все процессы-окна — в задании, которое закрывается вместе со стендом: даже если он
    // упадёт, окна на экране не останутся.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));

    AppState app;
    g_app = &app;
    UpdateVirtualScreen(app);
    DEVMODEW dm = {}; dm.dmSize = sizeof(dm);
    double hz = 60.0;
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) hz = dm.dmDisplayFrequency;
    const double periodMs = 1000.0 / hz;

    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int cols = 4, ww = 460, wh = 320;
    wchar_t bigEnv[8] = {};             // ID_BENCH_BIG=1 — первое окно большое и активное (как окно приложения)
    const bool bigActive = GetEnvironmentVariableW(L"ID_BENCH_BIG", bigEnv, 8) > 0;
    for (int i = 0; i < count; ++i) {
        int x = wa.left + 320 + (i % cols) * (ww + 30) + (i / (cols * 3)) * 40;
        int y = wa.top + 140 + ((i / cols) % 3) * (wh + 30) + (i / (cols * 3)) * 40;
        wchar_t cmd[512];
        int w1 = ww, h1 = wh;
        if (bigActive && i == 0) { x = wa.left + 260; y = wa.top + 40; w1 = 1424; h1 = 1259; }   // как большое окно приложения
        swprintf_s(cmd, L"\"%s\" --dummy %d %d %d %d %d", exe, i < slowCount ? slowUs : 0, x, y, w1, h1);
        STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = {};
        if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
            AssignProcessToJobObject(job, pi.hProcess);
            ResumeThread(pi.hThread);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        }
    }
    std::vector<HWND> hs;
    for (int tries = 0; tries < 100 && (int)hs.size() < count; ++tries) { Sleep(50); hs.clear(); EnumWindows(CollectDummies, (LPARAM)&hs); }
    if ((int)hs.size() < count) { wprintf(L"only %d of %d bench windows appeared\n", (int)hs.size(), count); CloseHandle(job); return 1; }
    Sleep(400);
    HWND prevFg = GetForegroundWindow(), bigWin = nullptr;
    if (bigActive) {
        for (HWND h : hs) { RECT r; GetWindowRect(h, &r); if (r.right - r.left > 1000) bigWin = h; }
        if (bigWin) { Activate(bigWin); Sleep(500); }
        wprintf(L"big window is %s\n", GetForegroundWindow() == bigWin ? L"active" : L"NOT active");
    }

    for (HWND h : hs) {
        TrackedWin w;
        RECT r; GetWindowRect(h, &r);
        w.hwnd = h; w.world = r; w.vm = { 0, 0, 0, 0 }; w.pinned = false;
        RECT ext;                               // отступы до видимой рамки — как считает программа
        if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &ext, sizeof(ext))))
            w.vm = { max(0L, ext.left - r.left), max(0L, ext.top - r.top), max(0L, r.right - ext.right), max(0L, r.bottom - ext.bottom) };
        app.wins.push_back(std::move(w));
    }
    app.framePeriod = periodMs / 1000.0;
    wprintf(L"display %.0f Hz (frame budget %.2f ms), %d windows, %d of them slow (%.1f ms each)\n",
            hz, periodMs, count, slowCount, slowUs / 1000.0);

    // 0. Только ожидание композиции — потолок: что даёт сам DwmFlush без нашей работы.
    {
        Series f; double t0 = NowMs(), last = t0;
        while (NowMs() - t0 < 1500) { DwmFlush(); double e = NowMs(); f.add(e - last); last = e; }
        int d = 0; for (double x : f.v) if (x > periodMs * 1.5) ++d;
        wprintf(L"%-34s fps %6.1f | frame ms avg %5.2f p95 %5.2f p99 %5.2f max %6.2f | dropped %4d of %5d\n",
                L"baseline: DwmFlush only", 1000.0 / f.avg(), f.avg(), f.pct(0.95), f.pct(0.99), f.mx(), d, (int)f.v.size());
    }
    app.hudVisible = false;
    Run(app, L"pan, no minimap", seconds, periodMs, false);

    // Миникарта с живыми превью — как в программе.
    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSW c = {};
    c.hInstance = hi; c.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    c.lpfnWndProc = HudProc; c.lpszClassName = L"InfiniteDesktopBenchHUD"; RegisterClassW(&c);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    InitGraphics(app); InitUi(app);
    app.hud = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        L"InfiniteDesktopBenchHUD", L"", WS_POPUP | WS_VISIBLE, 0, 0, MAP_W, MAP_H, nullptr, nullptr, hi, nullptr);
    SetLayeredWindowAttributes(app.hud, 0, HUD_ALPHA, LWA_ALPHA);
    PlaceHud(app);
    app.hudVisible = true; app.previewsOn = true;
    RegisterAllThumbs(app);
    RefreshHud(app); PumpMessages();
    Run(app, L"pan, minimap with live previews", seconds, periodMs, true);
    app.previewsOn = false;
    for (auto& w : app.wins) w.thumb.reset();
    Run(app, L"pan, minimap without previews", seconds, periodMs, true);

    // Панорама через превью: настоящие окна стоят, двигаются их живые DWM-превью в окне
    // обзора на масштабе 1:1 (то же, что делает зум), кадр показывает Direct2D.
    {
        WNDCLASSW oc = {};
        oc.hInstance = hi; oc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        oc.lpfnWndProc = OvProc; oc.lpszClassName = L"InfiniteDesktopBenchOverview"; RegisterClassW(&oc);
        app.ov = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            L"InfiniteDesktopBenchOverview", L"", WS_POPUP, app.vsX, app.vsY, app.vsW, app.vsH, nullptr, nullptr, hi, nullptr);
        CreateOverviewBackdrop(app, hi);
        LoadWallpaper(app);
        app.camX = app.camY = 0;
        app.zoom = app.zoomTarget = 1.0;
        app.axCX = 0; app.axCY = 0; app.axWX = app.vsX; app.axWY = app.vsY;
        RECT scr = { app.vsX, app.vsY, app.vsX + app.vsW, app.vsY + app.vsH };
        std::vector<BYTE> shot0, shot1;
        GrabScreen(scr, shot0);                    // стол с настоящей панелью задач
        double s0 = NowMs();
        ShowOverview(app);
        UpdateOverview(app); RenderOverview(app, app.ov);
        double enter = NowMs() - s0;
        // Панель задач в обзоре: на масштабе 1:1 её превью должно совпасть с настоящей,
        // а дальше уехать за край и раствориться.
        if (HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr)) {
            RECT tr; GetWindowRect(tray, &tr);
            auto trayDiff = [&](const std::vector<BYTE>& a, const std::vector<BYTE>& b) {
                int aw = scr.right - scr.left; double sum = 0; long n = 0;
                for (int y = max(tr.top, scr.top); y < min(tr.bottom, scr.bottom); ++y)
                    for (int x = max(tr.left, scr.left); x < min(tr.right, scr.right); x += 3) {
                        size_t i = ((size_t)(y - scr.top) * aw + (x - scr.left)) * 4;
                        sum += abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2]);
                        ++n;
                    }
                return n ? sum / (3.0 * n) : 0.0;
            };
            wprintf(L"zoom overview: %d edge bar(s) found\n", (int)app.ovBars.size());
            const double steps[] = { 0.0, 0.5, 1.0 };
            for (double p : steps) {
                app.barP = p;
                UpdateOverview(app); RenderOverview(app, app.ov);
                DwmFlush(); DwmFlush(); DwmFlush();
                GrabScreen(scr, shot1);
                wprintf(L"  taskbar preview at %.1f of the way out: difference from the real taskbar %.2f\n", p, trayDiff(shot0, shot1));
                if (!shotDir.empty()) {
                    wchar_t name[64]; swprintf_s(name, L"\\bar_%02d.png", (int)(p * 10));
                    SaveBgra(app, shot1, scr.right - scr.left, scr.bottom - scr.top, shotDir + name);
                }
            }
            // Настоящая панель должна оставаться ПОД обзором и спустя время: при превью,
            // ушедшем за край, на её месте виден обзор, а не она сама.
            app.barP = 1.0;
            UpdateOverview(app); RenderOverview(app, app.ov);
            Sleep(1500); PumpMessages(); DwmFlush();
            GrabScreen(scr, shot1);
            double later = trayDiff(shot0, shot1);
            wprintf(L"  1.5 s later the real taskbar is %s the overview (difference %.2f)\n",
                    later > 8.0 ? L"still under" : L"ABOVE", later);
            app.barP = 0;
            UpdateOverview(app);
        }
        // Вклад тени в чистом виде: тот же кадр обзора с тенями и без них. Отношение
        // яркостей — это (1 − затемнение), от фона не зависит; сверяем с профилем Windows.
        // В обзоре под окнами стенда только обои, в том числе живые (прозрачный слой).
        if (!app.wins.empty()) {
            std::vector<BYTE> withS, noS;
            UpdateOverview(app); RenderOverview(app, app.ov);
            DwmFlush(); DwmFlush(); DwmFlush();
            GrabScreen(scr, withS);
            bool had = false;
            for (auto& w : app.wins) { had = had || w.thumbShadow; w.thumbShadow.reset(); }
            UpdateOverview(app); RenderOverview(app, app.ov);
            DwmFlush(); DwmFlush(); DwmFlush();
            GrabScreen(scr, noS);
            const TrackedWin* big = &app.wins[0];        // самое большое окно: у него тень хранится уменьшенной
            for (auto& w : app.wins) if (w.world.right - w.world.left > big->world.right - big->world.left) big = &w;
            const TrackedWin& w0 = *big;
            RECT fr; DwmGetWindowAttribute(w0.hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &fr, sizeof(fr));
            int aw = scr.right - scr.left;
            auto ratio = [&](int x, int y) {
                size_t i = ((size_t)(y - scr.top) * aw + (x - scr.left)) * 4;
                double a = withS[i] + withS[i + 1] + withS[i + 2], b = noS[i] + noS[i + 1] + noS[i + 2];
                return b > 30 ? 1.0 - a / b : -1.0;     // затемнение; -1 — фон слишком тёмный, не измерить
            };
            int my = (fr.top + fr.bottom) / 2, mx = (fr.left + fr.right) / 2;
            wprintf(L"shadow alone over the %s wallpaper (%s), window %dx%d; darkening going away from the frame:\n",
                    app.ovLiveBg ? L"LIVE" : L"static", had ? L"shadow previews registered" : L"NO shadow previews",
                    fr.right - fr.left, fr.bottom - fr.top);
            wprintf(L"  left  :"); for (int d = 1; d <= 34; d += 3) wprintf(L" %5.3f", ratio(fr.left - d, my)); wprintf(L"\n");
            wprintf(L"  bottom:"); for (int d = 1; d <= 67; d += 6) wprintf(L" %5.3f", ratio(mx, fr.bottom - 1 + d)); wprintf(L"\n");
            wprintf(L"  Windows (inactive) left: 0.110 0.086 0.063 0.043 0.028 0.020 ...   bottom: 0.220 0.176 0.141 0.118 0.094 0.071 ...\n");
        }
        Series frame, work;
        double t0 = NowMs(), last = t0;
        for (;;) {
            double now = NowMs(), t = (now - t0) / 1000.0;
            if (t >= seconds) break;
            app.axCX = -260.0 * sin(t * 6.2831853 / 1.3);
            app.axCY = -90.0 * sin(t * 6.2831853 / 0.9);
            double a2 = NowMs();
            UpdateOverview(app);
            work.add(NowMs() - a2);
            RenderOverview(app, app.ov);            // EndDraw ждёт vblank
            PumpMessages();
            double e = NowMs();
            frame.add(e - last);
            last = e;
        }
        double s1 = NowMs();
        HideOverview(app);
        double leave = NowMs() - s1;
        int dropped = 0;
        for (double x : frame.v) if (x > periodMs * 1.5) ++dropped;
        wprintf(L"%-34s fps %6.1f | frame ms avg %5.2f p95 %5.2f p99 %5.2f max %6.2f | dropped %4d of %5d (%4.1f%%) | previews ms avg %5.2f max %5.2f | enter %.1f ms, leave %.1f ms\n",
                L"pan via previews (overview 1:1)", 1000.0 / frame.avg(), frame.avg(), frame.pct(0.95), frame.pct(0.99), frame.mx(),
                dropped, (int)frame.v.size(), 100.0 * dropped / max(1, (int)frame.v.size()), work.avg(), work.mx(), enter, leave);
    }

    // Плавная панорама, как в программе: превью на кадр + рабочий поток для настоящих окон.
    {
        Sleep(300);
        RECT area = { app.vsX, app.vsY, app.vsX + app.vsW, app.vsY + app.vsH };
        for (auto& w : app.wins) w.parkedOff = false;
        app.camX = app.camY = 0;
        RepositionAll(app);
        Sleep(250);
        std::vector<BYTE> before, during;
        GrabScreen(area, before);                 // рабочий стол как есть
        double s0 = NowMs();
        bool ok = BeginGlide(app);
        double enter1 = NowMs() - s0;
        if (!ok) { wprintf(L"smooth panning: BeginGlide failed\n"); }
        else {
            GlideFrame(app); GlideFrame(app); DwmFlush(); DwmFlush();
            GrabScreen(area, during);             // то же место под слоем превью, камера не сдвинута
            ReportDiff(app, area, before, during, shotDir);
            if (bigWin) {                         // тень большого активного окна: слева и сверху от рамки
                RECT fr; DwmGetWindowAttribute(bigWin, DWMWA_EXTENDED_FRAME_BOUNDS, &fr, sizeof(fr));
                int aw = area.right - area.left;
                auto at = [&](const std::vector<BYTE>& v, int x, int y) { return (int)v[((size_t)(y - area.top) * aw + (x - area.left)) * 4 + 2]; };
                int my = (fr.top + fr.bottom) / 2, mx = (fr.left + fr.right) / 2;
                wprintf(L"big active window %dx%d, red channel going away from its frame:\n", fr.right - fr.left, fr.bottom - fr.top);
                wprintf(L"  left   desktop:"); for (int d = 1; d <= 40; d += 3) wprintf(L" %3d", at(before, fr.left - d, my)); wprintf(L"\n");
                wprintf(L"  left   layer  :"); for (int d = 1; d <= 40; d += 3) wprintf(L" %3d", at(during, fr.left - d, my)); wprintf(L"\n");
                wprintf(L"  bottom desktop:"); for (int d = 1; d <= 79; d += 6) wprintf(L" %3d", at(before, mx, fr.bottom - 1 + d)); wprintf(L"\n");
                wprintf(L"  bottom layer  :"); for (int d = 1; d <= 79; d += 6) wprintf(L" %3d", at(during, mx, fr.bottom - 1 + d)); wprintf(L"\n");
                for (auto& w : app.wins) if (w.hwnd == bigWin)
                    wprintf(L"  shadow source %s, thumbnail %s, built as %s for %dx%d, pads %d,%d,%d,%d\n",
                            w.shadowSrc ? L"present" : L"MISSING", w.thumbShadow ? L"registered" : L"MISSING",
                            w.shadowActive ? L"active" : L"inactive", w.shadowW, w.shadowH,
                            (int)w.shadowPad.left, (int)w.shadowPad.top, (int)w.shadowPad.right, (int)w.shadowPad.bottom);
            }
            // Панель задач должна остаться поверх слоя: сравниваем её область до и во время.
            if (HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr)) {
                RECT tr; GetWindowRect(tray, &tr);
                int aw = area.right - area.left;
                double sum = 0; long n = 0;
                for (int y = max(tr.top, area.top); y < min(tr.bottom, area.bottom); ++y)
                    for (int x = max(tr.left, area.left); x < min(tr.right, area.right); x += 3) {
                        size_t i = ((size_t)(y - area.top) * aw + (x - area.left)) * 4;
                        sum += abs(before[i] - during[i]) + abs(before[i + 1] - during[i + 1]) + abs(before[i + 2] - during[i + 2]);
                        ++n;
                    }
                double mean = n ? sum / (3.0 * n) : 0;
                wprintf(L"taskbar under the preview layer: mean difference %.2f -> %s\n", mean,
                        mean < 4.0 ? L"stays visible" : L"HIDDEN");
            }
            EndGlide(app);
            s0 = NowMs();
            BeginGlide(app);                      // повторный вход — без разовой инициализации
            double enter2 = NowMs() - s0;
            Series frame, lag;
            double t0 = NowMs(), last = t0;
            for (;;) {
                double now = NowMs(), t = (now - t0) / 1000.0;
                if (t >= seconds) break;
                app.camX = 260.0 * sin(t * 6.2831853 / 1.3);
                app.camY = 90.0 * sin(t * 6.2831853 / 0.9);
                GlideFrame(app);
                PumpMessages();
                double e = NowMs();
                frame.add(e - last);
                last = e;
                LONG cx = (LONG)llround(app.camX), cy = (LONG)llround(app.camY);
                double worst = 0;                 // насколько настоящие окна отстали от камеры
                for (auto& w : app.wins) {
                    RECT r;
                    if (GetWindowRect(w.hwnd, &r))
                        worst = max(worst, max(fabs((double)(r.left - (w.world.left - cx))), fabs((double)(r.top - (w.world.top - cy)))));
                }
                lag.add(worst);
            }
            app.camX = 137; app.camY = -41;       // остановились не в исходной точке
            s0 = NowMs();
            EndGlide(app);
            double leave = NowMs() - s0;
            int wrong = 0;                        // после выхода каждое окно должно стоять точно на месте
            for (auto& w : app.wins) {
                RECT r;
                if (!GetWindowRect(w.hwnd, &r) || r.left != w.world.left - 137 || r.top != w.world.top + 41) ++wrong;
            }
            int dropped = 0;
            for (double x : frame.v) if (x > periodMs * 1.5) ++dropped;
            wprintf(L"%-34s fps %6.1f | frame ms avg %5.2f p95 %5.2f p99 %5.2f max %6.2f | dropped %4d of %5d (%4.1f%%) | real windows behind: avg %.0f px, max %.0f px | enter %.1f ms (first %.1f), leave %.1f ms | misplaced after: %d\n",
                    L"smooth panning (previews + thread)", 1000.0 / frame.avg(), frame.avg(), frame.pct(0.95), frame.pct(0.99), frame.mx(),
                    dropped, (int)frame.v.size(), 100.0 * dropped / max(1, (int)frame.v.size()),
                    lag.avg(), lag.mx(), enter2, enter1, leave, wrong);
            app.camX = app.camY = 0;
            RepositionAll(app);
        }
        StopGlideMover(app);
        if (app.ovBack) DestroyWindow(app.ovBack);
        DestroyWindow(app.ov);
    }

    app.wins.clear();
    DestroyWindow(app.hud);
    g_app = nullptr;
    CloseHandle(job);          // закрывает все процессы-окна
    if (bigActive && prevFg) Activate(prevFg);   // вернуть фокус туда, где он был
    return 0;
}
