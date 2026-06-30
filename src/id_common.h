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

// ---------- Состояние ----------
struct TrackedWin {
    HWND       hwnd;
    RECT       world;            // позиция/размер в координатах мира (по GetWindowRect)
    RECT       vm;               // отступы до видимых границ окна (невидимые рамки DWM)
    bool       pinned;           // закреплено: не двигается с панорамой
    HTHUMBNAIL thumb;            // живое DWM-превью на миникарте (или nullptr)
    HTHUMBNAIL thumbOv;          // живое DWM-превью в полноэкранном обзоре (зум)
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
inline HHOOK  g_mouseHook = nullptr;
inline HANDLE g_hookThread = nullptr;
inline DWORD  g_hookThreadId = 0;
inline bool   g_dragging = false;
inline POINT  g_lastMouse = {};

// Панорама/зум жестами тачпада (двухпальцевый свайп = колесо/гориз.колесо, пинч = Ctrl+колесо)
const double  TP_WHEEL = 0.8;            // px панорамы на единицу delta колеса

// Отслеживание изменения камеры для троттлинга перерисовки
inline double g_lastCamX = 1e18, g_lastCamY = 1e18;

// Зум-обзор через DWM-превью (зум-аут всего рабочего стола колесом)
inline HWND   g_ov = nullptr;            // полноэкранное окно обзора
inline bool   g_overview = false;        // активен ли обзор
inline double g_zoom = 1.0;              // текущий масштаб обзора (<=1)
inline double g_zoomTarget = 1.0;        // целевой масштаб
// Якорь зума: мировая точка g_axW удерживается в клиентской точке g_axC
inline double g_axWX = 0, g_axWY = 0, g_axCX = 0, g_axCY = 0;
// Перетаскивание окна/панорама внутри обзора
inline HWND   g_ovDragHwnd = nullptr;    // перетаскиваемое окно (или nullptr — фон)
inline POINT  g_ovDragLast = {};
inline bool   g_ovDragMoved = false;
const double  ZOOM_MIN = 0.08;           // самый дальний зум-аут
const double  ZOOM_STEP = 1.15;          // множитель на тик колеса
const double  ZOOM_EASE = 0.25;
inline HDC     g_ovMemDC = nullptr;       // кэш двойной буферизации обзора
inline HBITMAP g_ovBmp = nullptr;
inline int     g_ovCacheW = 0, g_ovCacheH = 0;
// Обои пользователя для фона обзора (затемняются по мере отдаления)
inline ULONG_PTR g_gdiToken = 0;
inline HDC     g_wallDC = nullptr;
inline HBITMAP g_wallBmp = nullptr;
inline int     g_wallW = 0, g_wallH = 0;
inline std::wstring g_wallPath;          // путь загруженных обоев (для отслеживания смены)
inline DWORD g_lastWallCap = 0;          // время последнего перечитывания обоев в обзоре
inline HDC     g_blackDC = nullptr;       // 1x1 чёрный для мягких теней (AlphaBlend)
inline HBITMAP g_blackBmp = nullptr;
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
void FreeMapGdi();
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
void UnregisterThumb(TrackedWin& w);
void RegisterAllThumbs();
void UnregisterAllThumbs();
void RegisterOvThumb(TrackedWin& w);
void UnregisterOvThumb(TrackedWin& w);
BOOL CALLBACK OvZProc(HWND h, LPARAM);
void RegisterAllOvThumbs();
void UnregisterAllOvThumbs();
void RebuildWindowList();
void SyncWorldFromScreen();
bool IsTracked(HWND h);
BOOL CALLBACK AddNewProc(HWND h, LPARAM);
void AddNewWindows();
void RepositionAll();
void PanBy(double dx, double dy);
void GoHome();
void CenterOn(double wx, double wy);
