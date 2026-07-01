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

// ---------- Состояние ----------
struct TrackedWin {
    HWND            hwnd;
    RECT            world;            // позиция/размер в координатах мира (по GetWindowRect)
    RECT            vm;               // отступы до видимых границ окна (невидимые рамки DWM)
    bool            pinned;           // закреплено: не двигается с панорамой
    ScopedThumbnail thumb;            // живое DWM-превью на миникарте
    ScopedThumbnail thumbOv;          // живое DWM-превью в полноэкранном обзоре (зум)
    bool            parkedOff = false; // окно физически выставлено за кадром (отсечка RepositionAll)
};

inline std::vector<TrackedWin> g_wins;
inline double g_camX = 0, g_camY = 0;          // текущая камера (мировая точка в левом-верхнем углу виртуального экрана)
inline double g_targetX = 0, g_targetY = 0;    // целевая камера (для плавной анимации)

// Виртуальный экран (объединение всех мониторов)
inline int  g_vsX = 0, g_vsY = 0, g_vsW = 0, g_vsH = 0;

inline HWND g_hud = nullptr;                    // окно миникарты
inline bool g_hudVisible = true;
inline bool g_previewsOn = true;                // живые DWM-превью на миникарте
inline int  g_corner = 1;                       // угол миникарты: 0=ЛВ,1=ПВ,2=ЛН,3=ПН
inline bool g_hudDrag = false;                  // зажата ЛКМ на миникарте (перетаскивание)
const int   MAP_W = 420, MAP_H = 260, MAP_MARGIN = 16;
const BYTE  HUD_ALPHA = 224, SETTINGS_ALPHA = 238;  // полупрозрачность
inline int  g_syncCounter = 0;
inline DWORD g_lastHudTick = 0, g_lastSyncTick = 0;  // троттлинг миникарты/синхронизации по времени

// Хоткеи
enum {
    HK_LEFT = 1, HK_RIGHT, HK_UP, HK_DOWN,
    HK_HOME, HK_MAP, HK_REFRESH, HK_QUIT, HK_PREVIEW, HK_PIN
};
const double PAN_STEP = 400.0;   // шаг панорамы в пикселях мира
const double EASE     = 0.22;    // коэффициент сглаживания камеры

// Настраиваемые горячие клавиши (сохраняются между сеансами)
struct KeyBind { UINT mods; UINT vk; const wchar_t* name; };
inline KeyBind g_keys[HK_PIN + 1];   // индекс = HK_*
inline HWND g_settings = nullptr;    // окно настроек горячих клавиш
inline int  g_capRow = -1;           // строка в режиме перехвата клавиш (или -1)
inline std::wstring g_status;        // сообщение в окне настроек (конфликт и т.п.)
// Геометрия окна настроек
const int SET_W = 400, SET_HEADER = 52, SET_ROW = 30, SET_FOOTER = 90;
const int SET_BOX_X = 210, SET_BOX_W = 170, SET_BOX_H = 24;

// Текущее преобразование мир -> клиент миникарты
struct MapXform { double s; LONG ox, oy; RECT bounds; };
inline MapXform g_xform;

// Панорама средней кнопкой мыши (глобальный hook на отдельном потоке)
inline HHOOK      g_mouseHook = nullptr;
inline ScopedHandle g_hookThread;
inline DWORD      g_hookThreadId = 0;
inline std::atomic_bool g_dragging = false;
inline POINT      g_lastMouse = {};
inline std::atomic<long> g_dragDeltaX = 0;
inline std::atomic<long> g_dragDeltaY = 0;

// Панорама/зум жестами тачпада (двухпальцевый свайп = колесо/гориз.колесо, пинч = Ctrl+колесо)
const double  TP_WHEEL = 0.8;            // px панорамы на единицу delta колеса

// Отслеживание изменения камеры для троттлинга перерисовки
inline double g_lastCamX = 1e18, g_lastCamY = 1e18;

// Зум-обзор через DWM-превью (зум-аут всего рабочего стола колесом)
inline HWND          g_ov = nullptr;            // полноэкранное окно обзора
inline bool          g_overview = false;        // активен ли обзор
inline double        g_zoom = 1.0;              // текущий масштаб обзора (<=1)
inline double        g_zoomTarget = 1.0;        // целевой масштаб
// Якорь зума: мировая точка g_axW удерживается в клиентской точке g_axC
inline double        g_axWX = 0, g_axWY = 0, g_axCX = 0, g_axCY = 0;
// Перетаскивание окна/панорама внутри обзора
inline HWND          g_ovDragHwnd = nullptr;    // перетаскиваемое окно (или nullptr — фон)
inline POINT         g_ovDragLast = {};
inline bool          g_ovDragMoved = false;
const double         ZOOM_MIN = 0.08;           // самый дальний зум-аут
const double         ZOOM_STEP = 1.07;          // мягкий множитель на один шаг колеса
const double         ZOOM_EASE = 0.18;
inline ScopedBitmap  g_ovBmp;
inline ScopedDC      g_ovMemDC;
inline int           g_ovCacheW = 0, g_ovCacheH = 0;
// Обои пользователя для фона обзора (затемняются по мере отдаления)
inline ScopedGdiplus g_gdi;                     // RAII для Gdiplus
inline ScopedBitmap  g_wallBmp;
inline ScopedDC      g_wallDC;
inline int           g_wallW = 0, g_wallH = 0;
inline std::wstring  g_wallPath;                // путь загруженных обоев (для отслеживания смены)
inline DWORD         g_lastWallCap = 0;         // время последнего перечитывания обоев в обзоре
inline ScopedBitmap  g_blackBmp;
inline ScopedDC      g_blackDC;                // 1x1 чёрный для мягких теней (AlphaBlend)
// Троттлинг перерисовки обзора (не каждый кадр, только при изменениях)
inline double  g_ovLastZoom = -1, g_ovLastAxCX = 0, g_ovLastAxCY = 0, g_ovLastAxWX = 0, g_ovLastAxWY = 0;

// ---------- Перенесённые модульные глобалы ----------
struct SavedWin { std::wstring cls, title; RECT world; bool used; };
inline std::vector<HWND>     g_ovZ;             // z-порядок окон в обзоре
inline std::vector<SavedWin> g_saved;           // загруженная раскладка
inline HWINEVENTHOOK g_winEventHook = nullptr;  // hook активации окна
inline BOOL g_prevWinArranging = TRUE;          // прежнее состояние Aero Snap
inline HWND g_iconsWin = nullptr;               // окно иконок рабочего стола
inline bool g_iconsWereVisible = false;

// ---------- Прототипы функций модулей ----------
bool IsDesktopClass(HWND h);
void DoZoomFactor(double factor, POINT pt);
void DoZoom(int delta, POINT pt);
void PanWheel(double dxDelta, double dyDelta);
LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp);
std::string WToUtf8(const std::wstring& w);
std::wstring Utf8ToW(const std::string& s);
void WinKey(HWND h, std::wstring& cls, std::wstring& title);
std::wstring LayoutPath();
void SaveLayout();
void LoadLayout();
void SpreadWindows(const std::vector<int>& idx);
void ArrangeOnStartup();
void GatherToDesktop();
void ComputeWorldBounds(RECT& out);
void ComputeXform(const RECT& client);
RECT WorldToClient(const RECT& w);
void MapToWorld(int mx, int my, double& wx, double& wy);
void UpdateThumbs(const RECT& client);
HFONT UiFont();
RECT GearRect(const RECT& client);
void DrawMinimap(HDC hdc, const RECT& client);
double OvClientX(double wx);
double OvClientY(double wy);
RECT VisWorld(const TrackedWin& w);
void LoadWallpaper();
void ShowOverview();
void HideOverview();
void CommitZoomCam();
void UpdateOverview();
void EnsureOvCache(HDC ref, int w, int h);
BOOL CALLBACK WallMonProc(HMONITOR hm, HDC, LPRECT, LPARAM lp);
void EnsureBlack();
void DrawShadow(HDC hdc, const RECT& r, int radius);
void DrawOverview(HDC hdc, const RECT& client);
LRESULT CALLBACK OvProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK HudProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
DWORD WINAPI HookThreadProc(LPVOID param);
void CenterOnCursorMonitor(double wx, double wy);
void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG, DWORD, DWORD);
void DisableSnap();
void RestoreSnap();
BOOL CALLBACK FindDefViewProc(HWND top, LPARAM lp);
HWND FindDesktopIcons();
void HideDesktopIcons();
void RestoreDesktopIcons();
void EnableDpiAwareness();
void ApplyWin11Style(HWND hwnd);
std::wstring UiCfgPath();
void SaveUiCfg();
void LoadUiCfg();
void PlaceHud();
const wchar_t* CornerGlyph();
void InitDefaultKeys();
void RegisterAllHotkeys();
void UnregisterAllHotkeys();
std::wstring HotkeysPath();
void SaveHotkeys();
void LoadHotkeys();
std::wstring KeyName(UINT vk);
std::wstring KeyComboText(UINT mods, UINT vk);
bool IsModifierVk(UINT vk);
UINT CurrentMods();
int SetFooterY();
int FindConflict(UINT mods, UINT vk, int exceptId);
LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
void OpenSettings();
void UpdateVirtualScreen();
bool IsCloaked(HWND h);
bool IsManageable(HWND h);
RECT ComputeVM(HWND h, const RECT& r);
BOOL CALLBACK EnumProc(HWND h, LPARAM);
void RegisterThumb(TrackedWin& w);
void RegisterAllThumbs();
void RegisterOvThumb(TrackedWin& w);
BOOL CALLBACK OvZProc(HWND h, LPARAM);
void RegisterAllOvThumbs();
void RebuildWindowList();
void SyncWorldFromScreen();
bool IsTracked(HWND h);
BOOL CALLBACK AddNewProc(HWND h, LPARAM);
void AddNewWindows();
void RepositionAll();
void PanBy(double dx, double dy);
void GoHome();
void CenterOn(double wx, double wy);
