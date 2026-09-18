// Общее: типы, состояние, константы, includes
// Часть InfiniteDesktop. Компилируется как единый модуль через main.cpp.

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <utility>
#include <atomic>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "msimg32.lib")

#ifndef SPI_GETWINARRANGING
#define SPI_GETWINARRANGING 0x0082
#endif
#ifndef SPI_SETWINARRANGING
#define SPI_SETWINARRANGING 0x0083
#endif

// ---------- RAII-обёртки для GDI/WinAPI ресурсов ----------
// Устраняют утечки при раннем выходе, исключениях, и ошибках создания.

struct ScopedDC {
    HDC h = nullptr;
    explicit ScopedDC(HDC h = nullptr) : h(h) {}
    ~ScopedDC() { if (h) DeleteDC(h); }
    ScopedDC(const ScopedDC&) = delete;
    ScopedDC& operator=(const ScopedDC&) = delete;
    ScopedDC(ScopedDC&& o) noexcept : h(o.h) { o.h = nullptr; }
    ScopedDC& operator=(ScopedDC&& o) noexcept { if (this != &o) { if (h) DeleteDC(h); h = o.h; o.h = nullptr; } return *this; }
    operator HDC() const { return h; }
    HDC* put() { reset(); return &h; }
    void reset(HDC nh = nullptr) { if (h) DeleteDC(h); h = nh; }
    HDC release() { HDC r = h; h = nullptr; return r; }
};

struct ScopedBitmap {
    HBITMAP h = nullptr;
    explicit ScopedBitmap(HBITMAP h = nullptr) : h(h) {}
    ~ScopedBitmap() { if (h) DeleteObject(h); }
    ScopedBitmap(const ScopedBitmap&) = delete;
    ScopedBitmap& operator=(const ScopedBitmap&) = delete;
    ScopedBitmap(ScopedBitmap&& o) noexcept : h(o.h) { o.h = nullptr; }
    ScopedBitmap& operator=(ScopedBitmap&& o) noexcept { if (this != &o) { if (h) DeleteObject(h); h = o.h; o.h = nullptr; } return *this; }
    operator HBITMAP() const { return h; }
    HBITMAP* put() { reset(); return &h; }
    void reset(HBITMAP nh = nullptr) { if (h) DeleteObject(h); h = nh; }
    HBITMAP release() { HBITMAP r = h; h = nullptr; return r; }
};

struct ScopedPen {
    HPEN h = nullptr;
    explicit ScopedPen(HPEN h = nullptr) : h(h) {}
    ~ScopedPen() { if (h) DeleteObject(h); }
    ScopedPen(const ScopedPen&) = delete;
    ScopedPen& operator=(const ScopedPen&) = delete;
    ScopedPen(ScopedPen&& o) noexcept : h(o.h) { o.h = nullptr; }
    ScopedPen& operator=(ScopedPen&& o) noexcept { if (this != &o) { if (h) DeleteObject(h); h = o.h; o.h = nullptr; } return *this; }
    operator HPEN() const { return h; }
    HPEN* put() { reset(); return &h; }
    void reset(HPEN nh = nullptr) { if (h) DeleteObject(h); h = nh; }
    HPEN release() { HPEN r = h; h = nullptr; return r; }
};

struct ScopedBrush {
    HBRUSH h = nullptr;
    explicit ScopedBrush(HBRUSH h = nullptr) : h(h) {}
    ~ScopedBrush() { if (h) DeleteObject(h); }
    ScopedBrush(const ScopedBrush&) = delete;
    ScopedBrush& operator=(const ScopedBrush&) = delete;
    ScopedBrush(ScopedBrush&& o) noexcept : h(o.h) { o.h = nullptr; }
    ScopedBrush& operator=(ScopedBrush&& o) noexcept { if (this != &o) { if (h) DeleteObject(h); h = o.h; o.h = nullptr; } return *this; }
    operator HBRUSH() const { return h; }
    HBRUSH* put() { reset(); return &h; }
    void reset(HBRUSH nh = nullptr) { if (h) DeleteObject(h); h = nh; }
    HBRUSH release() { HBRUSH r = h; h = nullptr; return r; }
};

struct ScopedFont {
    HFONT h = nullptr;
    explicit ScopedFont(HFONT h = nullptr) : h(h) {}
    ~ScopedFont() { if (h) DeleteObject(h); }
    ScopedFont(const ScopedFont&) = delete;
    ScopedFont& operator=(const ScopedFont&) = delete;
    ScopedFont(ScopedFont&& o) noexcept : h(o.h) { o.h = nullptr; }
    ScopedFont& operator=(ScopedFont&& o) noexcept { if (this != &o) { if (h) DeleteObject(h); h = o.h; o.h = nullptr; } return *this; }
    operator HFONT() const { return h; }
    HFONT* put() { reset(); return &h; }
    void reset(HFONT nh = nullptr) { if (h) DeleteObject(h); h = nh; }
    HFONT release() { HFONT r = h; h = nullptr; return r; }
};

struct ScopedRgn {
    HRGN h = nullptr;
    explicit ScopedRgn(HRGN h = nullptr) : h(h) {}
    ~ScopedRgn() { if (h) DeleteObject(h); }
    ScopedRgn(const ScopedRgn&) = delete;
    ScopedRgn& operator=(const ScopedRgn&) = delete;
    ScopedRgn(ScopedRgn&& o) noexcept : h(o.h) { o.h = nullptr; }
    ScopedRgn& operator=(ScopedRgn&& o) noexcept { if (this != &o) { if (h) DeleteObject(h); h = o.h; o.h = nullptr; } return *this; }
    operator HRGN() const { return h; }
    HRGN* put() { reset(); return &h; }
    void reset(HRGN nh = nullptr) { if (h) DeleteObject(h); h = nh; }
    HRGN release() { HRGN r = h; h = nullptr; return r; }
};

struct ScopedHandle {
    HANDLE h = nullptr;
    explicit ScopedHandle(HANDLE h = nullptr) : h(h) {}
    ~ScopedHandle() { if (h) CloseHandle(h); }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ScopedHandle(ScopedHandle&& o) noexcept : h(o.h) { o.h = nullptr; }
    ScopedHandle& operator=(ScopedHandle&& o) noexcept { if (this != &o) { if (h) CloseHandle(h); h = o.h; o.h = nullptr; } return *this; }
    operator HANDLE() const { return h; }
    HANDLE* put() { reset(); return &h; }
    void reset(HANDLE nh = nullptr) { if (h) CloseHandle(h); h = nh; }
    HANDLE release() { HANDLE r = h; h = nullptr; return r; }
};

// RAII для DWM-превью: привязывает owner-окно для корректной очистки.
struct ScopedThumbnail {
    HTHUMBNAIL h = nullptr;
    explicit ScopedThumbnail(HTHUMBNAIL h = nullptr) : h(h) {}
    ~ScopedThumbnail() { if (h) DwmUnregisterThumbnail(h); }
    ScopedThumbnail(const ScopedThumbnail&) = delete;
    ScopedThumbnail& operator=(const ScopedThumbnail&) = delete;
    ScopedThumbnail(ScopedThumbnail&& o) noexcept : h(o.h) { o.h = nullptr; }
    ScopedThumbnail& operator=(ScopedThumbnail&& o) noexcept { if (this != &o) { if (h) DwmUnregisterThumbnail(h); h = o.h; o.h = nullptr; } return *this; }
    operator HTHUMBNAIL() const { return h; }
    HTHUMBNAIL* put() { reset(); return &h; }
    void reset(HTHUMBNAIL nh = nullptr) { if (h) DwmUnregisterThumbnail(h); h = nh; }
    HTHUMBNAIL release() { HTHUMBNAIL r = h; h = nullptr; return r; }
};

// RAII для GDI+: безопасно завершает сессию при выходе.
struct ScopedGdiplus {
    ULONG_PTR token = 0;
    ScopedGdiplus() {
        Gdiplus::GdiplusStartupInput gsi;
        Gdiplus::GdiplusStartup(&token, &gsi, nullptr);
    }
    ~ScopedGdiplus() {
        if (token) { Gdiplus::GdiplusShutdown(token); token = 0; }
    }
    ScopedGdiplus(const ScopedGdiplus&) = delete;
    ScopedGdiplus& operator=(const ScopedGdiplus&) = delete;
    operator ULONG_PTR() const { return token; }
};

// ---------- Типы и константы ----------
struct TrackedWin {
    HWND            hwnd;
    RECT            world;            // позиция/размер в координатах мира (по GetWindowRect)
    RECT            vm;               // отступы до видимых границ окна (невидимые рамки DWM)
    bool            pinned;           // закреплено: не двигается с панорамой
    ScopedThumbnail thumb;            // живое DWM-превью на миникарте
    ScopedThumbnail thumbOv;          // живое DWM-превью в полноэкранном обзоре (зум)
    bool            parkedOff = false; // окно физически выставлено за кадром (отсечка RepositionAll)
};
const int   MAP_W = 420, MAP_H = 260, MAP_MARGIN = 16;
const BYTE  HUD_ALPHA = 224, SETTINGS_ALPHA = 238;  // полупрозрачность

// Хоткеи
enum {
    HK_LEFT = 1, HK_RIGHT, HK_UP, HK_DOWN,
    HK_HOME, HK_MAP, HK_REFRESH, HK_QUIT, HK_PREVIEW, HK_PIN
};
const double PAN_STEP = 400.0;   // шаг панорамы в пикселях мира
const double EASE     = 0.22;    // коэффициент сглаживания камеры

// Настраиваемые горячие клавиши (сохраняются между сеансами)
struct KeyBind { UINT mods; UINT vk; const wchar_t* name; };
// Геометрия окна настроек
const int SET_W = 400, SET_HEADER = 52, SET_ROW = 30, SET_FOOTER = 90;
const int SET_BOX_X = 210, SET_BOX_W = 170, SET_BOX_H = 24;

// Текущее преобразование мир -> клиент миникарты
struct MapXform { double s; LONG ox, oy; RECT bounds; };

// Панорама/зум жестами тачпада (двухпальцевый свайп = колесо/гориз.колесо, пинч = Ctrl+колесо)
const double  TP_WHEEL = 0.8;            // px панорамы на единицу delta колеса
const double         ZOOM_MIN = 0.08;           // самый дальний зум-аут
const double         ZOOM_STEP = 1.07;          // мягкий множитель на один шаг колеса
const double         ZOOM_EASE = 0.18;

// Сохранённая раскладка окон (layout.txt)
struct SavedWin { std::wstring cls, title; RECT world; bool used; };

// ---------- Состояние приложения ----------
// Всё изменяемое состояние собрано в один объект и передаётся в функции по ссылке.
// Единственный экземпляр создаётся в wWinMain. Некопируем/неперемещаем
// (содержит std::atomic и RAII-обёртки) — это намеренно.
struct AppState {

    std::vector<TrackedWin> wins;
    double camX = 0, camY = 0;          // текущая камера (мировая точка в левом-верхнем углу виртуального экрана)
    double targetX = 0, targetY = 0;    // целевая камера (для плавной анимации)

    // Виртуальный экран (объединение всех мониторов)
    int  vsX = 0, vsY = 0, vsW = 0, vsH = 0;

    HWND hud = nullptr;                    // окно миникарты
    bool hudVisible = true;
    bool previewsOn = true;                // живые DWM-превью на миникарте
    int  corner = 1;                       // угол миникарты: 0=ЛВ,1=ПВ,2=ЛН,3=ПН
    bool hudDrag = false;                  // зажата ЛКМ на миникарте (перетаскивание)
    int  syncCounter = 0;
    DWORD lastHudTick = 0, lastSyncTick = 0;  // троттлинг миникарты/синхронизации по времени
    KeyBind keys[HK_PIN + 1];   // индекс = HK_*
    HWND settings = nullptr;    // окно настроек горячих клавиш
    int  capRow = -1;           // строка в режиме перехвата клавиш (или -1)
    std::wstring status;        // сообщение в окне настроек (конфликт и т.п.)
    MapXform xform;

    // Панорама средней кнопкой мыши (глобальный hook на отдельном потоке)
    HHOOK      mouseHook = nullptr;
    ScopedHandle hookThread;
    DWORD      hookThreadId = 0;
    std::atomic_bool dragging = false;
    POINT      lastMouse = {};
    std::atomic<long> dragDeltaX = 0;
    std::atomic<long> dragDeltaY = 0;

    // Отслеживание изменения камеры для троттлинга перерисовки
    double lastCamX = 1e18, lastCamY = 1e18;

    // Зум-обзор через DWM-превью (зум-аут всего рабочего стола колесом)
    HWND          ov = nullptr;            // полноэкранное окно обзора
    bool          overview = false;        // активен ли обзор
    double        zoom = 1.0;              // текущий масштаб обзора (<=1)
    double        zoomTarget = 1.0;        // целевой масштаб
    // Якорь зума: мировая точка axW удерживается в клиентской точке axC
    double        axWX = 0, axWY = 0, axCX = 0, axCY = 0;
    // Перетаскивание окна/панорама внутри обзора
    HWND          ovDragHwnd = nullptr;    // перетаскиваемое окно (или nullptr — фон)
    POINT         ovDragLast = {};
    bool          ovDragMoved = false;
    ScopedBitmap  ovBmp;
    ScopedDC      ovMemDC;
    int           ovCacheW = 0, ovCacheH = 0;
    // Обои пользователя для фона обзора (затемняются по мере отдаления)
    ScopedGdiplus gdi;                     // RAII для Gdiplus
    ScopedBitmap  wallBmp;
    ScopedDC      wallDC;
    int           wallW = 0, wallH = 0;
    std::wstring  wallPath;                // путь загруженных обоев (для отслеживания смены)
    DWORD         lastWallCap = 0;         // время последнего перечитывания обоев в обзоре
    ScopedBitmap  blackBmp;
    ScopedDC      blackDC;                // 1x1 чёрный для мягких теней (AlphaBlend)
    // Троттлинг перерисовки обзора (не каждый кадр, только при изменениях)
    double  ovLastZoom = -1, ovLastAxCX = 0, ovLastAxCY = 0, ovLastAxWX = 0, ovLastAxWY = 0;
    std::vector<HWND>     ovZ;             // z-порядок окон в обзоре
    std::vector<SavedWin> saved;           // загруженная раскладка
    HWINEVENTHOOK winEventHook = nullptr;  // hook активации окна
    BOOL prevWinArranging = TRUE;          // прежнее состояние Aero Snap
    HWND iconsWin = nullptr;               // окно иконок рабочего стола
    bool iconsWereVisible = false;

    // Кэш GDI-объектов миникарты (перья/кисти создаются один раз)
    ScopedPen   framePen, pinPen, vpPen;
    ScopedBrush fillBr;

    AppState() = default;
    AppState(const AppState&) = delete;
    AppState& operator=(const AppState&) = delete;
};

// Единственная точка доступа к состоянию для Win32-колбэков (WndProc, WH_MOUSE_LL,
// SetWinEventHook, EnumWindows), которым нельзя передать параметр. Устанавливается
// в wWinMain; вся остальная логика получает AppState& явно.
inline AppState* g_app = nullptr;

// ---------- Прототипы функций модулей ----------
bool IsDesktopClass(AppState& app, HWND h);
void DoZoomFactor(AppState& app, double factor, POINT pt);
void DoZoom(AppState& app, int delta, POINT pt);
void PanWheel(AppState& app, double dxDelta, double dyDelta);
LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp);
std::string WToUtf8(const std::wstring& w);
std::wstring Utf8ToW(const std::string& s);
void WinKey(HWND h, std::wstring& cls, std::wstring& title);
std::wstring LayoutPath();
void SaveLayout(AppState& app);
void LoadLayout(AppState& app);
void SpreadWindows(AppState& app, const std::vector<int>& idx);
void ArrangeOnStartup(AppState& app);
void GatherToDesktop(AppState& app);
void ComputeWorldBounds(AppState& app, RECT& out);
void ComputeXform(AppState& app, const RECT& client);
RECT WorldToClient(AppState& app, const RECT& w);
void MapToWorld(AppState& app, int mx, int my, double& wx, double& wy);
void UpdateThumbs(AppState& app, const RECT& client);
HFONT UiFont();
RECT GearRect(const RECT& client);
void DrawMinimap(AppState& app, HDC hdc, const RECT& client);
double OvClientX(AppState& app, double wx);
double OvClientY(AppState& app, double wy);
RECT VisWorld(const TrackedWin& w);
void LoadWallpaper(AppState& app);
void ShowOverview(AppState& app);
void HideOverview(AppState& app);
void CommitZoomCam(AppState& app);
void UpdateOverview(AppState& app);
void EnsureOvCache(AppState& app, HDC ref, int w, int h);
BOOL CALLBACK WallMonProc(HMONITOR hm, HDC, LPRECT, LPARAM lp);
void EnsureBlack(AppState& app);
void DrawShadow(AppState& app, HDC hdc, const RECT& r, int radius);
void DrawOverview(AppState& app, HDC hdc, const RECT& client);
LRESULT CALLBACK OvProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK HudProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
DWORD WINAPI HookThreadProc(LPVOID param);
void CenterOnCursorMonitor(AppState& app, double wx, double wy);
void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG, DWORD, DWORD);
void DisableSnap(AppState& app);
void RestoreSnap(AppState& app);
BOOL CALLBACK FindDefViewProc(HWND top, LPARAM lp);
HWND FindDesktopIcons(AppState& app);
void HideDesktopIcons(AppState& app);
void RestoreDesktopIcons(AppState& app);
void EnableDpiAwareness();
void ApplyWin11Style(HWND hwnd);
std::wstring UiCfgPath();
void SaveUiCfg(AppState& app);
void LoadUiCfg(AppState& app);
void PlaceHud(AppState& app);
const wchar_t* CornerGlyph(AppState& app);
void InitDefaultKeys(AppState& app);
void RegisterAllHotkeys(AppState& app);
void UnregisterAllHotkeys(AppState& app);
std::wstring HotkeysPath();
void SaveHotkeys(AppState& app);
void LoadHotkeys(AppState& app);
std::wstring KeyName(UINT vk);
std::wstring KeyComboText(UINT mods, UINT vk);
bool IsModifierVk(UINT vk);
UINT CurrentMods();
int SetFooterY();
int FindConflict(AppState& app, UINT mods, UINT vk, int exceptId);
LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
void OpenSettings(AppState& app);
void UpdateVirtualScreen(AppState& app);
bool IsCloaked(HWND h);
bool IsManageable(AppState& app, HWND h);
RECT ComputeVM(HWND h, const RECT& r);
BOOL CALLBACK EnumProc(HWND h, LPARAM);
void RegisterThumb(AppState& app, TrackedWin& w);
void RegisterAllThumbs(AppState& app);
void RegisterOvThumb(AppState& app, TrackedWin& w);
BOOL CALLBACK OvZProc(HWND h, LPARAM);
void RegisterAllOvThumbs(AppState& app);
void RebuildWindowList(AppState& app);
void SyncWorldFromScreen(AppState& app);
bool IsTracked(AppState& app, HWND h);
BOOL CALLBACK AddNewProc(HWND h, LPARAM);
void AddNewWindows(AppState& app);
void RepositionAll(AppState& app);
void PanBy(AppState& app, double dx, double dy);
void GoHome(AppState& app);
void CenterOn(AppState& app, double wx, double wy);


