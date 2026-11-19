// Окна: виртуальный экран, фильтр, перечисление, превью, перемещение, камера
// Часть InfiniteDesktop.

#include "id_common.h"

// ---------- Виртуальный экран ----------
void UpdateVirtualScreen(AppState& app) {
    app.vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    app.vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    app.vsW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    app.vsH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

// Окно всё ещё верхнего уровня? Программы живых обоев (Wallpaper Engine, Lively и т.п.)
// создают обычное окно, а потом переносят его SetParent-ом внутрь окна рабочего стола
// (WorkerW/Progman). Если мы успели взять такое окно в список, пока оно было верхнего
// уровня, дальше двигать его нельзя: координаты SetWindowPos для дочернего окна — в
// системе родителя, и панорама увозила бы сами обои со стола (а парковка — за край).
// У окна верхнего уровня (в т.ч. с владельцем) родитель — окно рабочего стола.
bool IsTopLevelWindow(HWND h) {
    return GetAncestor(h, GA_PARENT) == GetDesktopWindow();
}

// Окно верхнего уровня — рабочий стол оболочки: Progman либо WorkerW того же процесса
// (слои стола на Windows 10 и ранних сборках 11). WorkerW — общее имя класса, такие окна
// создают и другие программы, поэтому сверяем процесс с окном оболочки. Пока Explorer
// перезапускается, GetShellWindow() пуст — тогда опорой служит Progman, если он есть;
// нет и его — рабочего стола сейчас нет вовсе.
bool IsShellDesktopWindow(HWND root) {
    wchar_t cls[32] = {};
    if (!root || !GetClassNameW(root, cls, 32)) return false;
    if (lstrcmpW(cls, L"WorkerW") && lstrcmpW(cls, L"Progman")) return false;
    HWND shell = GetShellWindow();
    if (!shell) shell = FindWindowW(L"Progman", nullptr);
    if (!shell) return false;
    DWORD a = 0, b = 0;
    GetWindowThreadProcessId(root, &a);
    GetWindowThreadProcessId(shell, &b);
    return a == b;
}

// Имя программы, которой принадлежит процесс (без пути).
std::wstring ExeName(DWORD pid) {
    std::wstring name = L"?";
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return name;
    wchar_t buf[MAX_PATH]; DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(p, 0, buf, &n)) {
        const wchar_t* slash = wcsrchr(buf, L'\\');
        name = slash ? slash + 1 : buf;
    }
    CloseHandle(p);
    return name;
}

// ---------- Фильтр окон (эвристика Alt+Tab) ----------
static bool IsCloaked(HWND h) {
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
        return cloaked != 0;
    return false;
}

static bool IsManageable(AppState& app, HWND h) {
    if (h == app.hud || h == app.ov || h == app.ovBack || h == app.settings) return false;
    if (!IsWindowVisible(h)) return false;
    if (IsIconic(h)) return false;                  // свёрнутые не трогаем
    if (GetWindowTextLengthW(h) == 0) return false;
    if (IsCloaked(h)) return false;

    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return false;
    if (GetWindow(h, GW_OWNER) != nullptr) return false; // только корневые окна

    wchar_t cls[128] = {};
    GetClassNameW(h, cls, 128);
    if (!lstrcmpW(cls, L"Progman") || !lstrcmpW(cls, L"WorkerW") ||
        !lstrcmpW(cls, L"Shell_TrayWnd") || !lstrcmpW(cls, L"Shell_SecondaryTrayWnd") ||
        !lstrcmpW(cls, L"Windows.UI.Core.CoreWindow"))
        return false;

    return true;
}

// Отступы от GetWindowRect до видимых границ окна (невидимые рамки изменения размера).
static RECT ComputeVM(HWND h, const RECT& r) {
    RECT ext;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &ext, sizeof(ext)))) {
        RECT m = { ext.left - r.left, ext.top - r.top, r.right - ext.right, r.bottom - ext.bottom };
        if (m.left < 0) m.left = 0;   if (m.top < 0) m.top = 0;
        if (m.right < 0) m.right = 0; if (m.bottom < 0) m.bottom = 0;
        return m;
    }
    return { 0, 0, 0, 0 };
}

// Трекуемое окно из_rect_ ОС: мир = экран + смещение камеры (общий код EnumProc/AddNewProc).
static TrackedWin MakeTracked(AppState& app, HWND h, const RECT& r) {
    LONG cx = (LONG)llround(app.camX), cy = (LONG)llround(app.camY);
    TrackedWin w;
    w.hwnd    = h;
    w.vm      = ComputeVM(h, r);
    w.pinned  = false;
    w.world   = { r.left + cx, r.top + cy, r.right + cx, r.bottom + cy };
    return w;
}

// ---------- Временно выпавшие окна ----------
// Запомнить окно, уходящее из списка, но продолжающее существовать.
static void RememberAway(AppState& app, const TrackedWin& w) {
    AwayWin a = { w.hwnd, w.world, w.pinned, w.parkedOff, w.parkedPos };
    for (auto& e : app.away) if (e.hwnd == w.hwnd) { e = a; return; }
    app.away.push_back(a);
}

// Окно вернулось в список: вернуть ему закрепление, а если оно стояло на парковке и
// появилось ровно там же — и место на холсте. Иначе его экранная позиция настоящая
// (например, свернули видимое окно, развернули — оно встало туда же на экране), и мир
// уже посчитан от неё.
static void RestoreAway(AppState& app, TrackedWin& w, const RECT& r) {
    for (size_t i = 0; i < app.away.size(); ++i) {
        const AwayWin& a = app.away[i];
        if (a.hwnd != w.hwnd) continue;
        w.pinned = a.pinned;
        if (a.parked && r.left == a.parkedPos.x && r.top == a.parkedPos.y) {
            w.world = { a.world.left, a.world.top,
                        a.world.left + (r.right - r.left), a.world.top + (r.bottom - r.top) };
            w.parkedOff = true;
            w.parkedPos = a.parkedPos;
        }
        app.away.erase(app.away.begin() + i);
        return;
    }
}

// Взять окно в список (общий код первичного перечисления и подхвата новых окон).
static TrackedWin* TrackWindow(AppState& app, HWND h) {
    if (!IsManageable(app, h) || IsTracked(app, h)) return nullptr;
    RECT r;
    if (!GetWindowRect(h, &r)) return nullptr;
    app.wins.push_back(MakeTracked(app, h, r));
    RestoreAway(app, app.wins.back(), r);
    return &app.wins.back();
}

static BOOL CALLBACK EnumProc(HWND h, LPARAM) {
    TrackWindow(*g_app, h);
    return TRUE;
}

// ---------- DWM-превью ----------
// RAII: ScopedThumbnail сам снимет превью при уничтожении/перезаписи, поэтому
// неуспех регистрации (nullptr) — просто пустое поле, чинить нечего.
static void RegisterThumb(AppState& app, TrackedWin& w) {
    if (w.thumb || !app.hud || !app.previewsOn) return;
    DwmRegisterThumbnail(app.hud, w.hwnd, w.thumb.put());
}

void RegisterAllThumbs(AppState& app) {
    for (auto& w : app.wins) RegisterThumb(app, w);
}

// Превью для полноэкранного обзора (зум)

void RegisterOvThumb(AppState& app, TrackedWin& w) {
    if (w.thumbOv || !app.ov) return;
    // Сначала тень, потом само окно: превью ложатся в порядке регистрации, так что тень
    // окажется под своим окном, но над окнами ниже него.
    w.thumbShadow.reset();
    EnsureShadow(app, w);
    if (w.shadowSrc) DwmRegisterThumbnail(app.ov, w.shadowSrc, w.thumbShadow.put());
    DwmRegisterThumbnail(app.ov, w.hwnd, w.thumbOv.put());
}

static BOOL CALLBACK OvZProc(HWND h, LPARAM) {     // собрать отслеживаемые в z-порядке
    AppState& app = *g_app;
    for (auto& w : app.wins) if (w.hwnd == h) { app.ovZ.push_back(h); break; }
    return TRUE;
}

// Превью регистрируем СНИЗУ ВВЕРХ по текущему z-порядку: DWM компонует их в порядке
// регистрации, поэтому верхнее окно (зарегистрированное последним) ложится поверх —
// как на реальном рабочем столе.
void RegisterAllOvThumbs(AppState& app) {
    app.ovZ.clear();
    EnumWindows(OvZProc, 0);                        // сверху вниз
    for (auto it = app.ovZ.rbegin(); it != app.ovZ.rend(); ++it)
        for (auto& w : app.wins) if (w.hwnd == *it) { RegisterOvThumb(app, w); break; }
    for (auto& w : app.wins) RegisterOvThumb(app, w);      // добрать не попавшие в EnumWindows
}

// Пересобрать список окон заново. Известные окна сначала уходят во «временно выпавшие»:
// у окна за кадром экранная позиция — место парковки, и собери мы мир заново с экрана,
// вся раскладка за пределами экрана съехалась бы к его краю, а закрепления пропали бы.
void RebuildWindowList(AppState& app) {
    for (auto& w : app.wins) if (IsWindow(w.hwnd)) RememberAway(app, w);
    // Очистка app.wins автоматически вызывает деструкторы ScopedThumbnail,
    // которые unregister-ят старые превью.
    app.wins.clear();
    EnumWindows(EnumProc, 0);
    if (app.hudVisible) RegisterAllThumbs(app);
    if (app.overview)   RegisterAllOvThumbs(app);   // пересборка из обзора — превью вернуть и туда
}

// Синхронизировать мировые позиции с фактическим положением окон
// (учитывает ручное перетаскивание и закрытие окон).
void SyncWorldFromScreen(AppState& app) {
    // В плавной панораме настоящие окна под слоем превью отстают от камеры — их экранные
    // позиции сейчас ничего не значат, брать с них мир нельзя.
    if (app.glide) return;
    LONG cx = (LONG)llround(app.camX), cy = (LONG)llround(app.camY);
    const LONG vL = app.vsX, vT = app.vsY, vR = app.vsX + app.vsW, vB = app.vsY + app.vsH;
    size_t write = 0;
    for (size_t i = 0; i < app.wins.size(); ++i) {
        HWND h = app.wins[i].hwnd;
        if (!IsWindow(h) || !IsTopLevelWindow(h)) continue;   // закрыто или стало дочерним — забываем
        if (!IsWindowVisible(h) || IsIconic(h)) {
            // Свёрнуто или скрыто (в т.ч. Win+D): из списка уходит, но место на холсте и
            // закрепление помним до возвращения. ScopedThumbnail снимет превью сам.
            RememberAway(app, app.wins[i]);
            continue;
        }
        // Актуальную позицию ОС имеют только окна в кадре (RepositionAll двигает лишь их).
        // У окон за пределами вьюпорта позиция устаревшая — их мир НЕ трогаем, иначе
        // отсечка в RepositionAll испортила бы сохранённые мировые координаты.
        int sx = app.wins[i].world.left  - cx, sy = app.wins[i].world.top    - cy;
        int sr = app.wins[i].world.right - cx, sb = app.wins[i].world.bottom - cy;
        bool onScreen = !(sr <= vL || sx >= vR || sb <= vT || sy >= vB);
        RECT r;
        if (onScreen && GetWindowRect(h, &r)) {
            RECT& wr = app.wins[i].world;
            // Размер окна изменился (пользователь тянул край). Новую область, оказавшуюся
            // за монитором, приложение могло не отрисовать — в DWM-превью там чёрное.
            // Просим перерисоваться целиком. Без RDW_UPDATENOW: не блокируемся на чужом
            // окне, оно отрисуется на своём следующем цикле сообщений.
            if ((r.right - r.left) != (wr.right - wr.left) || (r.bottom - r.top) != (wr.bottom - wr.top))
                RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            wr.left   = r.left   + cx;
            wr.top    = r.top    + cy;
            wr.right  = r.right  + cx;
            wr.bottom = r.bottom + cy;
        }
        if (write != i) app.wins[write] = std::move(app.wins[i]);
        ++write;
    }
    app.wins.erase(app.wins.begin() + write, app.wins.end());
    // Запомненные окна, которых больше нет, — забыть.
    app.away.erase(std::remove_if(app.away.begin(), app.away.end(),
                                  [](const AwayWin& a) { return !IsWindow(a.hwnd); }), app.away.end());
}

bool IsTracked(AppState& app, HWND h) {
    for (auto& w : app.wins) if (w.hwnd == h) return true;
    return false;
}

// Убрать из списка закрытые окна. Нужна обзору: полная синхронизация там не идёт, а
// окно, закрытое кликом из обзора, иначе оставляло бы тень до выхода из зума.
bool PruneDeadWindows(AppState& app) {
    size_t before = app.wins.size();
    app.wins.erase(std::remove_if(app.wins.begin(), app.wins.end(),
                                  [](const TrackedWin& w) { return !IsWindow(w.hwnd); }), app.wins.end());
    return app.wins.size() != before;
}

// Окно целиком за пределами экрана при текущей камере?
static bool IsOffScreen(const AppState& app, const TrackedWin& w) {
    LONG cx = (LONG)llround(app.camX), cy = (LONG)llround(app.camY);
    return w.world.right - cx <= app.vsX || w.world.left - cx >= app.vsX + app.vsW ||
           w.world.bottom - cy <= app.vsY || w.world.top - cy >= app.vsY + app.vsH;
}

// Добавить вновь появившиеся окна (открытые уже после запуска).
static BOOL CALLBACK AddNewProc(HWND h, LPARAM) {
    AppState& app = *g_app;
    TrackedWin* w = TrackWindow(app, h);
    if (!w) return TRUE;
    if (app.hudVisible && app.previewsOn) RegisterThumb(app, *w);
    if (app.overview) {
        RegisterOvThumb(app, *w);
        RegisterOvBars(app);            // новое превью легло поверх панелей — вернуть их наверх
        app.ovLastBarP = -1;
    }
    // Окно активировали раньше, чем оно попало в список (развернули из свёрнутого с панели
    // задач), а его место на холсте — за кадром: подъезжаем, как к любому активированному.
    if (h == app.pendingFocus) {
        app.pendingFocus = nullptr;
        if (!app.overview && GetForegroundWindow() == h && IsOffScreen(app, *w)) BringWindowIntoView(app, *w);
    }
    return TRUE;
}

void AddNewWindows(AppState& app) { EnumWindows(AddNewProc, 0); }

// Позиция «парковки» окна за кадром. Ставим его не на истинное (возможно далёкое)
// место, а вплотную за ближайшим краем экрана так, чтобы прямоугольник окна перекрывал
// монитор ровно на 1 px. Тогда MonitorFromWindow(...) по-прежнему != NULL — иначе ряд
// приложений (Photoshop и другие Adobe) в WM_WINDOWPOSCHANGED считают окно «потерянным»
// вне всех мониторов и сами его сворачивают. Этот 1 px приходится на невидимую DWM-рамку
// (тень/зона изменения размера), так что визуально ничего не торчит. Физическая позиция
// припаркованного окна не важна: при возврате в кадр RepositionAll ставит его на истинную.
void ParkPosition(const RECT& screen, LONG wd, LONG ht, int& x, int& y) {
    const LONG vL = screen.left, vT = screen.top, vR = screen.right, vB = screen.bottom;
    if      (x >= vR)       x = vR - 1;        // справа: левый край на 1 px внутри экрана
    else if (x + wd <= vL)  x = vL - wd + 1;   // слева:  правый край на 1 px внутри
    if      (y >= vB)       y = vB - 1;        // снизу
    else if (y + ht <= vT)  y = vT - ht + 1;   // сверху
}
static void ParkPos(const AppState& app, const TrackedWin& w, int& x, int& y) {
    RECT screen = { app.vsX, app.vsY, app.vsX + app.vsW, app.vsY + app.vsH };
    ParkPosition(screen, w.world.right - w.world.left, w.world.bottom - w.world.top, x, y);
}

// ---------- Расстановка окон по камере ----------
// Цена перемещения — в самой системе: по замерам (tests.exe --bench-modes) около
// 0,25–0,5 мс на окно, под общей блокировкой оконного менеджера, так что потоками она не
// распараллеливается. Отсюда всё устройство: двигаем только окна в кадре, одним пакетом.

struct Move { TrackedWin* w; int x, y; bool park; bool done; };

// Куда поставить окно при текущей камере. false — окно сейчас не двигаем.
static bool PlanMove(AppState& app, TrackedWin& w, LONG cx, LONG cy, Move& m) {
    // Вьюпорт (виртуальный экран). Окна полностью вне него невидимы — двигать их каждый
    // кадр незачем. Ставим окно на место, лишь когда его новый прямоугольник попадает в
    // кадр (координаты абсолютные — без «прыжка»).
    const LONG vL = app.vsX, vT = app.vsY, vR = app.vsX + app.vsW, vB = app.vsY + app.vsH;
    if (!IsWindow(w.hwnd) || !IsTopLevelWindow(w.hwnd)) return false;   // стало дочерним — не наше
    if (!app.moveMaximized && IsZoomed(w.hwnd)) return false;   // режим «приклеены к монитору»
    if (w.pinned) {                          // закреплённое окно не двигаем,
        RECT r;                              // а его мир тянем за фактической позицией
        if (GetWindowRect(w.hwnd, &r)) {
            w.world.left   = r.left   + cx; w.world.top    = r.top    + cy;
            w.world.right  = r.right  + cx; w.world.bottom = r.bottom + cy;
        }
        return false;
    }
    int x = w.world.left   - cx, y = w.world.top    - cy;
    int r = w.world.right  - cx, b = w.world.bottom - cy;
    bool onScreen = !(r <= vL || x >= vR || b <= vT || y >= vB);
    // Окно полностью за кадром И уже выставлено туда — двигать незачем (невидимо). Но если
    // оно ещё НЕ припарковано (старт, либо только что ушло за край), один раз ставим его
    // на место парковки, иначе оно осталось бы в видимой зоне, выглядывая с края.
    if (!onScreen && w.parkedOff) return false;
    if (IsHungAppWindow(w.hwnd)) return false;   // не блокировать кадр на зависшем окне
    if (!onScreen) ParkPos(app, w, x, y);        // за кадр — к краю экрана, не на далёкую позицию
    m = { &w, x, y, !onScreen, false };
    return true;
}

// Применить перемещения. Синхронно (без SWP_ASYNCWINDOWPOS): окна точно следуют за
// камерой, без «желейного» отставания от очереди асинхронных запросов.
//
// Пакетом (BeginDeferWindowPos/EndDeferWindowPos): система переставляет все окна за один
// проход, и DWM видит их в одной композиции — по замерам вдвое быстрее, чем по одному, и
// без смаза между окнами. Пакет не принимает SWP_NOSENDCHANGING (DeferWindowPos возвращает
// ошибку 87) — раньше флаг передавался, пакет не собирался НИКОГДА, и код молча двигал
// окна по одному. Без этого флага окно получает WM_WINDOWPOSCHANGING и может поправить
// позицию по-своему (прилипание к краям, «не уходить за экран»); такое окно после пакета
// оказывается не там, где просили, — его помечаем и дальше двигаем отдельно, с флагом.
static void ApplyMoves(std::vector<Move>& moves, bool allowBatch) {
    if (moves.empty()) return;
    const UINT FB = SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE;
    const UINT F1 = FB | SWP_NOSENDCHANGING;
    int inBatch = 0;
    for (auto& m : moves) {
        // Окно уже стоит где нужно (типично после плавной панорамы: его довёл рабочий
        // поток) — системный вызов не нужен, а он и есть вся цена.
        RECT at;
        m.done = GetWindowRect(m.w->hwnd, &at) && at.left == m.x && at.top == m.y;
        if (!m.done && !m.w->noBatch) ++inBatch;
    }
    bool batched = false;
    if (allowBatch && inBatch > 1) {
        if (HDWP h = BeginDeferWindowPos(inBatch)) {
            for (auto& m : moves) {
                if (m.done || m.w->noBatch) continue;
                h = DeferWindowPos(h, m.w->hwnd, nullptr, m.x, m.y, 0, 0, FB);
                if (!h) break;                   // HDWP после сбоя недействителен
            }
            batched = h && EndDeferWindowPos(h);
        }
    }
    for (auto& m : moves) {
        bool single = !m.done && (!batched || m.w->noBatch);
        RECT rr = {};
        if (!m.done && !single && GetWindowRect(m.w->hwnd, &rr) && (rr.left != m.x || rr.top != m.y)) {
            m.w->noBatch = true;                 // окно поправило позицию само — впредь отдельно
            single = true;
        }
        if (single) SetWindowPos(m.w->hwnd, nullptr, m.x, m.y, 0, 0, F1);
        m.w->parkedOff = m.park;                 // выставили за кадр — пометили припаркованным
        if (m.park) {                            // запомнили ФАКТИЧЕСКОЕ место парковки:
            RECT pr;                             // окно может лечь не ровно в запрошенную точку,
            m.w->parkedPos = GetWindowRect(m.w->hwnd, &pr) ? POINT{ pr.left, pr.top } : POINT{ m.x, m.y };
        }                                        // иначе перепроверка зациклилась бы
    }
}

// Расставить все окна по экрану согласно текущей камере.
void RepositionAll(AppState& app) {
    LONG cx = (LONG)llround(app.camX), cy = (LONG)llround(app.camY);
    std::vector<Move> moves;
    moves.reserve(app.wins.size());
    Move m;
    for (auto& w : app.wins) if (PlanMove(app, w, cx, cy, m)) moves.push_back(m);
    ApplyMoves(moves, true);
}

// Плавная панорама: окна, которые остаются настоящими поверх слоя превью (поверх всех
// окон), двигаем каждый кадр сами — их обычно одно-два.
void RepositionGlideReal(AppState& app) {
    LONG cx = (LONG)llround(app.camX), cy = (LONG)llround(app.camY);
    std::vector<Move> moves;
    Move m;
    for (auto& w : app.wins) if (w.glideReal && PlanMove(app, w, cx, cy, m)) moves.push_back(m);
    ApplyMoves(moves, false);
}

// Плавная панорама: цели для рабочего потока, который двигает настоящие окна под слоем
// превью. Состояние окон здесь не меняется — его приведёт в порядок RepositionAll в конце.
void PlanGlideMoves(AppState& app, std::vector<GlideMove>& out) {
    out.clear();
    LONG cx = (LONG)llround(app.camX), cy = (LONG)llround(app.camY);
    const LONG vL = app.vsX, vT = app.vsY, vR = app.vsX + app.vsW, vB = app.vsY + app.vsH;
    // Без обращений к системе на каждое окно: рабочий поток в это время держит блокировку
    // оконного менеджера, и каждый такой вызов из цикла кадров ждал бы его. Что окно
    // закреплено или приклеено, запомнено при входе (glideFixed); живо ли окно, проверит
    // сам рабочий поток.
    for (auto& w : app.wins) {
        if (w.glideReal || w.glideFixed) continue;
        int x = w.world.left - cx, y = w.world.top - cy;
        int r = w.world.right - cx, b = w.world.bottom - cy;
        if (r <= vL || x >= vR || b <= vT || y >= vB) ParkPos(app, w, x, y);
        out.push_back({ w.hwnd, x, y });
    }
}


// Активировать чужое окно из обработчика хоткея. SetForegroundWindow разрешён не всегда
// (foreground lock); на этот случай временно присоединяем ввод к потоку текущего
// активного окна — классический приём. К зависшему потоку не присоединяемся.
static void ForceForeground(HWND h) {
    HWND cur = GetForegroundWindow();
    DWORD me = GetCurrentThreadId();
    DWORD other = cur ? GetWindowThreadProcessId(cur, nullptr) : 0;
    bool attach = other && other != me && !IsHungAppWindow(cur);
    if (attach) AttachThreadInput(me, other, TRUE);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    if (attach) AttachThreadInput(me, other, FALSE);
}

// Направленное переключение фокуса: ближайшее окно в сторону (dx,dy) от активного
// (если активно не отслеживаемое окно — от центра экрана). Кандидаты — только окна
// «впереди» по направлению; из них берём то, что ближе всего по курсу: отклонение
// вбок штрафуется вдвое, чтобы прямо стоящее окно выигрывало у чуть более близкого
// по диагонали. Если окно не целиком в кадре — камера подъезжает к нему.
// Сам выбор цели — отдельной функцией без окон и состояния (покрыта тестами): индекс
// лучшего кандидата из worlds или -1; self — индекс активного окна (или -1).
int PickFocusTarget(const std::vector<RECT>& worlds, int self, double ox, double oy, int dx, int dy) {
    int best = -1; double bestScore = 0;
    for (int i = 0; i < (int)worlds.size(); ++i) {
        if (i == self) continue;
        double cx = (worlds[i].left + worlds[i].right) / 2.0 - ox;
        double cy = (worlds[i].top + worlds[i].bottom) / 2.0 - oy;
        double fwd = cx * dx + cy * dy;              // вдоль направления
        if (fwd <= 0) continue;                      // только впереди
        double lat = fabs(cx * dy - cy * dx);        // поперёк
        double score = fwd + 2.0 * lat;
        if (best < 0 || score < bestScore) { best = i; bestScore = score; }
    }
    return best;
}

void FocusDir(AppState& app, int dx, int dy) {
    if (app.overview || app.wins.empty()) return;
    HWND fg = GetForegroundWindow();
    double ox = 0, oy = 0; bool fromWin = false;
    for (auto& w : app.wins) if (w.hwnd == fg) {
        ox = (w.world.left + w.world.right) / 2.0; oy = (w.world.top + w.world.bottom) / 2.0;
        fromWin = true; break;
    }
    if (!fromWin) {
        fg = nullptr;
        ox = app.camX + app.vsX + app.vsW / 2.0;
        oy = app.camY + app.vsY + app.vsH / 2.0;
    }
    // Кандидаты — живые несвёрнутые окна; активное исключаем по индексу.
    std::vector<RECT> worlds; std::vector<TrackedWin*> cand; int self = -1;
    for (auto& w : app.wins) {
        if (!IsWindow(w.hwnd) || IsIconic(w.hwnd)) continue;
        if (w.hwnd == fg) self = (int)worlds.size();
        worlds.push_back(w.world); cand.push_back(&w);
    }
    int pick = PickFocusTarget(worlds, self, ox, oy, dx, dy);
    if (pick < 0) return;
    TrackedWin* best = cand[pick];
    ForceForeground(best->hwnd);
    // Показать целиком. WinEventProc подъезжает только к окнам, которых не видно
    // вовсе; здесь же пользователь явно попросил это окно — довезём и частично видимое.
    LONG cx = (LONG)llround(app.camX), cy = (LONG)llround(app.camY);
    RECT s = { best->world.left - cx, best->world.top - cy, best->world.right - cx, best->world.bottom - cy };
    bool whole = s.left >= app.vsX && s.top >= app.vsY &&
                 s.right <= app.vsX + app.vsW && s.bottom <= app.vsY + app.vsH;
    if (!whole) BringWindowIntoView(app, *best);
}

void PanBy(AppState& app, double dx, double dy) { app.targetX += dx; app.targetY += dy; }
void GoHome(AppState& app) { app.targetX = 0; app.targetY = 0; }

// Центрировать камеру на мировой точке (для клика по миникарте).
void CenterOn(AppState& app, double wx, double wy) {
    app.targetX = wx - app.vsX - app.vsW / 2.0;
    app.targetY = wy - app.vsY - app.vsH / 2.0;
}



