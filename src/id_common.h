// Общее: типы, состояние, константы, межмодульные прототипы.
// Часть InfiniteDesktop.

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

// Атрибуты DWM для стиля Win11 (заголовок в цвет темы, скруглённые углы).
// Значения фиксированы системой; имена есть не во всех SDK — защищаем #ifndef.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

// ---------- RAII-обёртки для GDI/WinAPI ресурсов ----------
// Устраняют утечки при раннем выходе, исключениях и ошибках создания.
// Один шаблон вместо восьми почти одинаковых классов:Close-функцию передаём
// параметром шаблона (C++17 `auto` NTTP), семантика — прежняя один в один.

template <typename H, auto Close>
struct ScopedRes {
    H h = nullptr;
    explicit ScopedRes(H hh = nullptr) : h(hh) {}
    ~ScopedRes() { if (h) Close(h); }
    ScopedRes(const ScopedRes&) = delete;
    ScopedRes& operator=(const ScopedRes&) = delete;
    ScopedRes(ScopedRes&& o) noexcept : h(o.h) { o.h = nullptr; }
    ScopedRes& operator=(ScopedRes&& o) noexcept {
        if (this != &o) { if (h) Close(h); h = o.h; o.h = nullptr; }
        return *this;
    }
    operator H() const { return h; }
    H* put() { reset(); return &h; }
    void reset(H nh = nullptr) { if (h) Close(h); h = nh; }
    H release() { H r = h; h = nullptr; return r; }
};

using ScopedDC        = ScopedRes<HDC,        DeleteDC>;
using ScopedBitmap    = ScopedRes<HBITMAP,    DeleteObject>;
using ScopedPen       = ScopedRes<HPEN,       DeleteObject>;
using ScopedBrush     = ScopedRes<HBRUSH,     DeleteObject>;
using ScopedFont      = ScopedRes<HFONT,      DeleteObject>;
using ScopedRgn       = ScopedRes<HRGN,       DeleteObject>;
using ScopedHandle    = ScopedRes<HANDLE,     CloseHandle>;
using ScopedThumbnail = ScopedRes<HTHUMBNAIL, DwmUnregisterThumbnail>;

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
const double TP_WHEEL  = 0.8;    // px панорамы на единицу delta колеса
const double ZOOM_MIN  = 0.08;   // самый дальний зум-аут
const double ZOOM_STEP = 1.07;   // мягкий множитель на один шаг колеса
const double ZOOM_EASE = 0.18;   // сглаживание масштаба обзора

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
    // Живые обои в зуме: под окном обзора лежит подложка с живым DWM-превью окна-хоста
    // обоев; само окно обзора при этом прозрачно там, где нет теней и превью окон.
    HWND            ovBack = nullptr;      // подложка (прозрачна для мыши)
    ScopedThumbnail wallThumb;             // живое превью хоста обоев на подложке
    RECT            wallDst = {};          // куда превью ложится в клиенте подложки
    bool            ovLiveBg = false;      // текущий сеанс обзора идёт с живым фоном
    bool          overview = false;        // активен ли обзор
    double        zoom = 1.0;              // текущий масштаб обзора (<=1)
    double        zoomTarget = 1.0;        // целевой масштаб
    // Якорь зума: мировая точка axW удерживается в клиентской точке axC
    double        axWX = 0, axWY = 0, axCX = 0, axCY = 0;
    // Перетаскивание окна/панорама внутри обзора
    HWND          ovDragHwnd = nullptr;    // перетаскиваемое окно (или nullptr — фон)
    POINT         ovDragLast = {};
    bool          ovDragMoved = false;
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
    bool          wallLive = false;        // на столе живые обои (окно чужого процесса внутри WorkerW/Progman)
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

// Якорь зума: мировая точка (axWX,axWY) закреплена в клиентской (axCX,aCY) на масштабе
// zoom. Прямое и обратное преобразования нужны и отрисовке обзора, и перепривязке
// зума колесом (id_overview.cpp, id_input.cpp).
inline double OvClientX(const AppState& app, double wx) { return app.axCX + (wx - app.axWX) * app.zoom; }
inline double OvClientY(const AppState& app, double wy) { return app.axCY + (wy - app.axWY) * app.zoom; }
inline double OvWorldX(const AppState& app, double cx)  { return app.axWX + (cx - app.axCX) / app.zoom; }
inline double OvWorldY(const AppState& app, double cy)  { return app.axWY + (cy - app.axCY) / app.zoom; }

// ---------- Прототипы функций модулей ----------
// Только межмодульный API: используемое в одном файле объявлено static в нём же
// и сюда не вынесено.
// id_input.cpp — колёсный зум и глобальный хук мыши ( MouseProc нужен id_procs.cpp )
LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp);
// id_layout.cpp — раскладка, закладки и общий каталог сохраняемых файлов
std::wstring ConfigPath(const wchar_t* file);
void SaveLayout(AppState& app);
void LoadLayout(AppState& app);
void ArrangeOnStartup(AppState& app);
void GatherToDesktop(AppState& app);
void LoadMarks(AppState& app);
void SetMark(AppState& app, int i);
void GotoMark(AppState& app, int i);
// id_minimap.cpp — преобразования и отрисовка миникарты
void ComputeXform(AppState& app, const RECT& client);
void MapToWorld(AppState& app, int mx, int my, double& wx, double& wy);
void UpdateThumbs(AppState& app, const RECT& client);
void RefreshHud(AppState& app);   // пересчёт превью + WM_PAINT миникарты
HFONT UiFont();
RECT GearRect(const RECT& client);
void DrawMinimap(AppState& app, HDC hdc, const RECT& client);
void EnsureHudCache(AppState& app, HDC ref, int w, int h);
void ResetMapGdi(AppState& app);
// id_overview.cpp — полноэкранный зум-обзор
void LoadWallpaper(AppState& app);
bool WallpaperFileChanged(AppState& app);
void ShowOverview(AppState& app);
void HideOverview(AppState& app);
void CommitZoomCam(AppState& app);
void UpdateOverview(AppState& app);
void RenderOverview(AppState& app, HWND hwnd);
void InitGraphics(AppState& app);
void CreateOverviewBackdrop(AppState& app, HINSTANCE hi);
RECT VisWorld(const TrackedWin& w);
LRESULT CALLBACK OvProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
// id_procs.cpp — HUD-окно, WinEvent, Snap, иконки стола, DPI
LRESULT CALLBACK HudProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
DWORD WINAPI HookThreadProc(LPVOID param);
void BringWindowIntoView(AppState& app, const TrackedWin& w);
void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG, DWORD, DWORD);
void DisableSnap(AppState& app);
void RestoreSnap(AppState& app);
void HideDesktopIcons(AppState& app);
void RecheckDesktopIcons(AppState& app);
void RestoreDesktopIcons(AppState& app);
void EnableDpiAwareness();
// id_settings.cpp — стиль Win11, размещение миникарты, горячие клавиши, окно настроек
void ApplyWin11Style(AppState& app, HWND hwnd);
void LoadUiCfg(AppState& app);
void PlaceHud(AppState& app);
void InitDefaultKeys(AppState& app);
void RegisterAllHotkeys(AppState& app);
void UnregisterAllHotkeys(AppState& app);
void LoadHotkeys(AppState& app);
bool ApplyCapture(AppState& app, UINT mods, UINT vk);
LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
void OpenSettings(AppState& app);
// id_ui.cpp — тема и примитивы Direct2D
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
// id_tray.cpp — иконка в трее и общие переключатели
void AddTrayIcon(AppState& app);
void RefreshTrayIcon(AppState& app);
void RemoveTrayIcon(AppState& app);
void OnTrayMessage(AppState& app, WPARAM wp, LPARAM lp);
void ToggleMinimap(AppState& app);
void TogglePreviews(AppState& app);
// id_windows.cpp — фильтр/перечисление окон, превью, камера
void UpdateVirtualScreen(AppState& app);
void RegisterAllThumbs(AppState& app);
void RegisterAllOvThumbs(AppState& app);
void RebuildWindowList(AppState& app);
void SyncWorldFromScreen(AppState& app);
bool IsTracked(AppState& app, HWND h);
bool IsTopLevelWindow(HWND h);
void AddNewWindows(AppState& app);
void RepositionAll(AppState& app);
void PanBy(AppState& app, double dx, double dy);
void GoHome(AppState& app);
void FocusDir(AppState& app, int dx, int dy);
void CenterOn(AppState& app, double wx, double wy);


