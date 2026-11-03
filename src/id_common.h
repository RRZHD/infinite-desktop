// Общее: типы, состояние, константы, includes
// Часть InfiniteDesktop. Компилируется как единый модуль через main.cpp.

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#include <dwmapi.h>
#include <objbase.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <shellapi.h>
#include <wrl/client.h>
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
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

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

// ---------- Типы и константы ----------
struct TrackedWin {
    HWND            hwnd;
    RECT            world;            // позиция/размер в координатах мира (по GetWindowRect)
    RECT            vm;               // отступы до видимых границ окна (невидимые рамки DWM)
    bool            pinned;           // закреплено: не двигается с панорамой
    ScopedThumbnail thumb;            // живое DWM-превью на миникарте
    ScopedThumbnail thumbOv;          // живое DWM-превью в полноэкранном обзоре (зум)
    bool            parkedOff = false; // окно физически выставлено за кадром (отсечка RepositionAll)
    POINT           parkedPos = {};    // куда именно поставили при парковке (детекция чужих сдвигов)
};
const int   MAP_W = 420, MAP_H = 260, MAP_MARGIN = 16;
const BYTE  HUD_ALPHA = 224;   // полупрозрачность миникарты (окно настроек непрозрачно — Direct2D)

// Хоткеи
enum {
    HK_LEFT = 1, HK_RIGHT, HK_UP, HK_DOWN,
    HK_HOME, HK_MAP, HK_REFRESH, HK_QUIT, HK_PREVIEW, HK_PIN,
    // Направленный фокус: активировать ближайшее окно в сторону от активного
    HK_FOCUS_LEFT, HK_FOCUS_RIGHT, HK_FOCUS_UP, HK_FOCUS_DOWN,
    // Закладки камеры («рабочие места» на холсте): запомнить / вернуться. В настройках —
    // две групповые строки (модификаторы + цифра 1…9), регистрируются все 18 хоткеев.
    HK_MARK1, HK_MARK2, HK_MARK3, HK_MARK4, HK_MARK5, HK_MARK6, HK_MARK7, HK_MARK8, HK_MARK9,
    HK_GOTO1, HK_GOTO2, HK_GOTO3, HK_GOTO4, HK_GOTO5, HK_GOTO6, HK_GOTO7, HK_GOTO8, HK_GOTO9,
    HK_LAST = HK_GOTO9
};
const int MARK_COUNT = 9;
struct CamMark { double x = 0, y = 0; bool set = false; };
const double PAN_STEP = 400.0;   // шаг панорамы в пикселях мира
const double EASE     = 0.22;    // коэффициент сглаживания камеры

// Настраиваемые горячие клавиши (сохраняются между сеансами)
struct KeyBind { UINT mods; UINT vk; const wchar_t* name; };
// Геометрия окна настроек
const int SET_W = 420;   // ширина окна настроек; высота — по содержимому (раскладка в id_settings.cpp)

// Тема интерфейса: нейтрали Fluent (светлая/тёмная — по настройке приложений Windows),
// акцент — системный. Геометрия и состояния элементов — по стайлгайду (см. id_ui.cpp).
struct Theme {
    bool dark = true;
    COLORREF bg = 0, surface = 0, control = 0, controlHover = 0, border = 0, borderStrong = 0,
             text = 0, muted = 0, accent = 0, accentInk = 0, accentSoft = 0, onAccent = 0,
             danger = 0, warning = 0, success = 0;
};
#define WM_TRAY (WM_APP + 1)   // сообщения иконки в трее (окно-владелец — миникарта)

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
    // СКМ двигает холст поверх любого окна (а не только над пустым столом / с Ctrl).
    // Читается из hook-потока мыши — поэтому atomic. Цена выбора: СКМ в приложениях
    // (орбита в Blender, автопрокрутка в браузере) достаётся холсту.
    std::atomic_bool mmbAnywhere = false;
    // Развёрнутые окна двигаются с холстом (иначе — приклеены к монитору и панорама
    // под ними визуально стоит). При выходе такие окна пересобираются на ближайшем мониторе.
    bool moveMaximized = false;
    bool hudDrag = false;                  // зажата ЛКМ на миникарте (перетаскивание)
    int  syncCounter = 0;
    DWORD lastHudTick = 0, lastSyncTick = 0;  // троттлинг миникарты/синхронизации по времени
    KeyBind keys[HK_LAST + 1];  // индекс = HK_*
    CamMark marks[MARK_COUNT];  // закладки камеры (marks.txt)
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
    bool          ovPassThrough = false;   // оверлей обзора прозрачен для hit-теста (пока инжектируем клик в окно)
    // ---- Direct2D: обзор рисуется на GPU ----
    // Device-independent (живут весь процесс):
    Microsoft::WRL::ComPtr<ID2D1Factory>          d2dFactory;
    Microsoft::WRL::ComPtr<IDWriteFactory>        dwFactory;
    Microsoft::WRL::ComPtr<IWICImagingFactory>    wicFactory;
    Microsoft::WRL::ComPtr<IDWriteTextFormat>     ovTextFormat;
    // ---- Интерфейс: тема, шрифты DirectWrite, окно настроек на Direct2D, трей ----
    Theme theme;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmtTitle, fmtBody, fmtCtrl, fmtSmall, fmtSec;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> setRT;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  setBrush;
    int   setScroll = 0;                   // прокрутка окна настроек, px
    HICON trayIcon = nullptr;              // рисуется кодом (в сборке нет ресурсов)
    bool  trayAdded = false;
    UINT  taskbarCreatedMsg = 0;           // «TaskbarCreated»: Explorer перезапустился — иконку добавить заново
    // Device-dependent (пересоздаются при потере устройства — D2DERR_RECREATE_TARGET):
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> ovRT;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>  ovShadowBrush, ovTextBrush;
    Microsoft::WRL::ComPtr<ID2D1Bitmap>           wallD2D;       // обои на GPU (создаются из wallWic)
    // Обои пользователя для фона обзора (затемняются по мере отдаления)
    Microsoft::WRL::ComPtr<IWICBitmap>            wallWic;       // декодированные обои в памяти (PBGRA)
    bool          wallIsScreen = false;    // wallWic — снимок реального рендера стола (1:1), а не файл
    int           wallOffX = 0, wallOffY = 0;   // смещение снимка относительно виртуального экрана
    std::wstring  wallPath;                // путь загруженных обоев (для отслеживания смены)
    ULONGLONG     wallMtime = 0;           // время изменения файла обоев (перечитываем только при смене)
    DWORD         lastWallCap = 0;         // время последней проверки обоев в обзоре
    // Троттлинг перерисовки обзора (не каждый кадр, только при изменениях)
    double  ovLastZoom = -1, ovLastAxCX = 0, ovLastAxCY = 0, ovLastAxWX = 0, ovLastAxWY = 0;
    std::vector<HWND>     ovZ;             // z-порядок окон в обзоре
    std::vector<SavedWin> saved;           // загруженная раскладка
    HWINEVENTHOOK winEventHook = nullptr;  // hook активации/перетаскивания окна
    HWND moveSizeHwnd = nullptr;           // окно, которое пользователь сейчас тащит/ресайзит мышью
                                           // (между EVENT_SYSTEM_MOVESIZESTART и ...END); nullptr — никакое
    BOOL prevWinArranging = TRUE;          // прежнее состояние Aero Snap
    HWND iconsWin = nullptr;               // окно иконок рабочего стола
    bool iconsWereVisible = false;

    // Кэш GDI-объектов миникарты (перья/кисти создаются один раз)
    ScopedPen   framePen, pinPen, vpPen;
    ScopedBrush fillBr, mapBg;
    // Задний буфер миникарты (пересоздаётся только при смене размера, а не каждый WM_PAINT)
    ScopedDC      hudMemDC;
    ScopedBitmap  hudBmp;
    int           hudCacheW = 0, hudCacheH = 0;

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
bool WallpaperFileChanged(AppState& app);
void EnsureHudCache(AppState& app, HDC ref, int w, int h);
void ShowOverview(AppState& app);
void HideOverview(AppState& app);
void CommitZoomCam(AppState& app);
void UpdateOverview(AppState& app);
void InitGraphics(AppState& app);
BOOL CALLBACK WallMonProc(HMONITOR hm, HDC, LPRECT, LPARAM lp);
void DrawShadow(AppState& app, const RECT& r, int radius);
void RenderOverview(AppState& app, HWND hwnd);
LRESULT CALLBACK OvProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK HudProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
DWORD WINAPI HookThreadProc(LPVOID param);
void BringWindowIntoView(AppState& app, const TrackedWin& w);
void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG, DWORD, DWORD);
void DisableSnap(AppState& app);
void RestoreSnap(AppState& app);
BOOL CALLBACK FindDefViewProc(HWND top, LPARAM lp);
HWND FindDesktopIcons(AppState& app);
void HideDesktopIcons(AppState& app);
void RestoreDesktopIcons(AppState& app);
void EnableDpiAwareness();
void ApplyWin11Style(AppState& app, HWND hwnd);
std::wstring UiCfgPath();
void SaveUiCfg(AppState& app);
void LoadUiCfg(AppState& app);
void PlaceHud(AppState& app);
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
int FindConflict(AppState& app, UINT mods, UINT vk, int exceptId);
bool ApplyCapture(AppState& app, UINT mods, UINT vk);
LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
void OpenSettings(AppState& app);
// id_ui.cpp — тема и примитивы Direct2D
void LoadTheme(AppState& app);
void InitUi(AppState& app);
void OnThemeChanged(AppState& app);
COLORREF UiMix(COLORREF a, COLORREF b, float t);
D2D1_COLOR_F ToD2D(COLORREF c, float a);
bool EnsureUiTarget(AppState& app, HWND hwnd, Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget>& rt,
                    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>& br);
void UiFillRR(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const D2D1_RECT_F& r, float rad, COLORREF c, float a);
void UiStrokeRR(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const D2D1_RECT_F& r, float rad, COLORREF c, float a, float w);
void UiText(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, IDWriteTextFormat* f, const std::wstring& s,
            const D2D1_RECT_F& r, COLORREF c, DWRITE_TEXT_ALIGNMENT al);
void ResetMapGdi(AppState& app);
// id_tray.cpp — иконка в трее и общие переключатели
void AddTrayIcon(AppState& app);
void RefreshTrayIcon(AppState& app);
void RemoveTrayIcon(AppState& app);
void OnTrayMessage(AppState& app, WPARAM wp, LPARAM lp);
void ToggleMinimap(AppState& app);
void TogglePreviews(AppState& app);
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
void FocusDir(AppState& app, int dx, int dy);
std::wstring MarksPath();
void SaveMarks(AppState& app);
void LoadMarks(AppState& app);
void SetMark(AppState& app, int i);
void GotoMark(AppState& app, int i);
void CenterOn(AppState& app, double wx, double wy);


