// Сохранение/восстановление раскладки окон, закладки камеры, каталог конфига
// Часть InfiniteDesktop.

#include "id_common.h"

// ---------- Каталог сохраняемых файлов ----------
// Всё состояние приложения живёт в %LOCALAPPDATA%\InfiniteDesktop; каталог
// создаётся при первом обращении. Единая точка для layout/marks/hotkeys/ui/wall.log.
std::wstring ConfigPath(const wchar_t* file) {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring dir = (n && n < MAX_PATH) ? std::wstring(buf) : L".";
    dir += L"\\InfiniteDesktop";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\" + file;
}

// ---------- Сохранение/восстановление раскладки окон ----------

static std::string WToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring Utf8ToW(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static void WinKey(HWND h, WinKeyId& k) {
    wchar_t c[128] = {};
    GetClassNameW(h, c, 128);
    k.cls = c;
    int n = GetWindowTextLengthW(h);
    std::wstring t((size_t)n + 1, 0);
    int got = GetWindowTextW(h, &t[0], n + 1);
    t.resize(got < 0 ? 0 : got);
    // Заголовок попадает в строку файла целиком: перевод строки в нём разорвал бы запись.
    for (auto& ch : t) if (ch == L'\n' || ch == L'\r') ch = L' ';
    k.title = t;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    k.exe = ExeName(pid);
    for (auto& ch : k.exe) ch = (wchar_t)towlower(ch);
}

// Сохранить мировые позиции всех окон. Формат v2: первая строка «v2», далее
// L<TAB>T<TAB>R<TAB>B<TAB>exe<TAB>class<TAB>title. В старом формате (без заголовка) поля
// exe нет. Пишем во временный файл и подменяем им прежний: автосохранение идёт во время
// работы, и оборванная запись не должна оставить половину раскладки.
void SaveLayout(AppState& app) {
    std::wstring path = ConfigPath(L"layout.txt"), tmp = path + L".tmp";
    {
        std::ofstream f(tmp.c_str(), std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write("v2\n", 3);
        auto put = [&](HWND hwnd, const RECT& world) {
            if (!IsWindow(hwnd)) return;
            WinKeyId k;
            WinKey(hwnd, k);
            std::string line = std::to_string(world.left)  + "\t" + std::to_string(world.top) + "\t"
                             + std::to_string(world.right) + "\t" + std::to_string(world.bottom) + "\t"
                             + WToUtf8(k.exe) + "\t" + WToUtf8(k.cls) + "\t" + WToUtf8(k.title) + "\n";
            f.write(line.data(), (std::streamsize)line.size());
        };
        for (auto& w : app.wins) put(w.hwnd, w.world);
        // Свёрнутые и скрытые окна тоже: иначе Win+D перед выключением сохранил бы пустой стол.
        for (auto& a : app.away) put(a.hwnd, a.world);
        if (!f.good()) return;
    }
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// Автосохранение. Раньше раскладка писалась только при чистом выходе, а программа в трее
// чаще всего заканчивает работу вместе с Windows или после сбоя — и раскладка терялась.
// Сохраняем, когда позиции окон изменились и 3 секунды не менялись (перетаскивание,
// открытие пачки окон не должны писать файл на каждый шаг).
void AutoSaveLayout(AppState& app) {
    if (app.sessionEnding) return;   // Windows закрывает окна — раскладка уже сохранена
    unsigned long long h = 1469598103934665603ULL;   // FNV-1a по окнам и их мировым позициям
    auto mix = [&](unsigned long long v) { h = (h ^ v) * 1099511628211ULL; };
    auto mixWin = [&](HWND hwnd, const RECT& r) {
        mix((unsigned long long)(UINT_PTR)hwnd);
        mix((unsigned long long)(unsigned)r.left);  mix((unsigned long long)(unsigned)r.top);
        mix((unsigned long long)(unsigned)r.right); mix((unsigned long long)(unsigned)r.bottom);
    };
    for (auto& w : app.wins) mixWin(w.hwnd, w.world);
    for (auto& a : app.away) mixWin(a.hwnd, a.world);
    DWORD now = GetTickCount();
    if (h != app.layoutHash) {
        bool first = (app.layoutHash == 0);   // стартовая раскладка только что загружена — не пишем
        app.layoutHash = h;
        if (!first) { app.layoutDirty = true; app.layoutChangedTick = now; }
        return;
    }
    if (app.layoutDirty && now - app.layoutChangedTick >= 3000) {
        app.layoutDirty = false;
        SaveLayout(app);
    }
}

void LoadLayout(AppState& app) {
    app.saved.clear();
    std::ifstream f(ConfigPath(L"layout.txt").c_str(), std::ios::binary);
    if (!f) return;
    std::string line;
    bool v2 = false;
    bool firstLine = true;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (firstLine) { firstLine = false; if (line == "v2") { v2 = true; continue; } }
        if (line.empty()) continue;
        long v[4]; size_t p = 0; bool ok = true;
        for (int i = 0; i < 4 && ok; ++i) {
            size_t t = line.find('\t', p);
            if (t == std::string::npos) { ok = false; break; }
            v[i] = atol(line.substr(p, t - p).c_str());
            p = t + 1;
        }
        if (!ok) continue;
        SavedWin sw;
        if (v2) {                                   // exe — отдельным полем перед классом
            size_t t = line.find('\t', p);
            if (t == std::string::npos) continue;
            sw.exe = Utf8ToW(line.substr(p, t - p));
            p = t + 1;
        }
        size_t t = line.find('\t', p);          // граница class|title
        if (t == std::string::npos) continue;
        sw.cls   = Utf8ToW(line.substr(p, t - p));
        sw.title = Utf8ToW(line.substr(t + 1));
        sw.world.left = v[0]; sw.world.top = v[1]; sw.world.right = v[2]; sw.world.bottom = v[3];
        sw.used = false;
        app.saved.push_back(sw);
    }
}

// Разложить окна «полками» без наложения. Старт — ниже уже размещённых окон.
static void SpreadWindows(AppState& app, const std::vector<int>& idx) {
    if (idx.empty()) return;
    std::vector<bool> spread(app.wins.size(), false);
    for (int i : idx) spread[i] = true;

    bool any = false; RECT bb = {};
    for (size_t i = 0; i < app.wins.size(); ++i) {
        if (spread[i]) continue;
        RECT& r = app.wins[i].world;
        if (!any) { bb = r; any = true; }
        else { bb.left = min(bb.left, r.left); bb.top = min(bb.top, r.top);
               bb.right = max(bb.right, r.right); bb.bottom = max(bb.bottom, r.bottom); }
    }
    const LONG gap = 40;
    LONG rowStartX = any ? bb.left : (LONG)app.vsX + gap;
    LONG x = rowStartX;
    LONG y = any ? bb.bottom + gap : (LONG)app.vsY + gap;
    LONG rowH = 0;
    LONG maxRowW = app.vsW > 0 ? (LONG)app.vsW : 3000;   // ширина полки ~ весь виртуальный экран
    for (int i : idx) {
        LONG w = app.wins[i].world.right - app.wins[i].world.left;
        LONG h = app.wins[i].world.bottom - app.wins[i].world.top;
        if (x - rowStartX + w > maxRowW && x > rowStartX) { x = rowStartX; y += rowH + gap; rowH = 0; }
        app.wins[i].world.left = x;       app.wins[i].world.top = y;
        app.wins[i].world.right = x + w;  app.wins[i].world.bottom = y + h;
        x += w + gap; rowH = max(rowH, h);
    }
}

// Сопоставить окна с сохранённой раскладкой: для каждого ключа — индекс записи или -1.
// Два прохода: точное совпадение (программа + класс + заголовок), затем программа + класс
// (заголовок мог измениться). Программа сверяется обязательно: класс окна у Chrome, Edge,
// VS Code и прочих приложений на Chromium/Electron один и тот же, и по одному классу окно
// одной программы вставало на место другой. У записей старого формата программы нет —
// для них остаётся прежнее сопоставление по классу.
std::vector<int> MatchSavedLayout(std::vector<SavedWin>& saved, const std::vector<WinKeyId>& keys) {
    std::vector<int> match(keys.size(), -1);
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < keys.size(); ++i) {
            if (match[i] >= 0) continue;
            for (size_t j = 0; j < saved.size(); ++j) {
                SavedWin& s = saved[j];
                if (s.used || s.cls != keys[i].cls) continue;
                if (!s.exe.empty() && s.exe != keys[i].exe) continue;
                if (pass == 0 && s.title != keys[i].title) continue;
                s.used = true; match[i] = (int)j;
                break;
            }
        }
    }
    return match;
}

// При запуске: вернуть окна на сохранённые места; новые/несопоставленные — разложить.
void ArrangeOnStartup(AppState& app) {
    std::vector<WinKeyId> keys(app.wins.size());
    for (size_t i = 0; i < app.wins.size(); ++i) WinKey(app.wins[i].hwnd, keys[i]);
    std::vector<int> match = MatchSavedLayout(app.saved, keys);

    std::vector<int> toSpread;
    for (size_t i = 0; i < app.wins.size(); ++i) {
        if (match[i] < 0) { toSpread.push_back((int)i); continue; }
        const RECT& s = app.saved[match[i]].world;
        LONG w = app.wins[i].world.right - app.wins[i].world.left;
        LONG h = app.wins[i].world.bottom - app.wins[i].world.top;
        app.wins[i].world = { s.left, s.top, s.left + w, s.top + h };   // позиция из файла, размер текущий
    }
    SpreadWindows(app, toSpread);
}

// ---------- Закладки камеры («рабочие места» на бесконечном холсте) ----------
// Бесконечный стол делит на «рабочие места» сама камера: закладка — это точка
// холста, куда можно вернуться одним хоткеем. Хранятся рядом с раскладкой.
static void SaveMarks(AppState& app) {
    std::ofstream f(ConfigPath(L"marks.txt").c_str(), std::ios::binary | std::ios::trunc);
    if (!f) return;
    for (int i = 0; i < MARK_COUNT; ++i) {
        if (!app.marks[i].set) continue;
        std::string line = std::to_string(i) + " " + std::to_string((long long)llround(app.marks[i].x))
                         + " " + std::to_string((long long)llround(app.marks[i].y)) + "\n";
        f.write(line.data(), (std::streamsize)line.size());
    }
}

void LoadMarks(AppState& app) {
    std::ifstream f(ConfigPath(L"marks.txt").c_str(), std::ios::binary);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream iss(line);
        int i; long long x, y;
        if ((iss >> i >> x >> y) && i >= 0 && i < MARK_COUNT)
            app.marks[i] = { (double)x, (double)y, true };
    }
}

// Запомнить текущую (целевую) позицию камеры — не промежуточную точку анимации.
void SetMark(AppState& app, int i) {
    if (i < 0 || i >= MARK_COUNT) return;
    app.marks[i] = { app.targetX, app.targetY, true };
    SaveMarks(app);
}

// Плавно перелететь к закладке (через ту же анимацию, что и клик по миникарте).
// В обзоре камера живёт в якоре зума — там закладки не применяем.
void GotoMark(AppState& app, int i) {
    if (i < 0 || i >= MARK_COUNT || !app.marks[i].set || app.overview) return;
    app.targetX = app.marks[i].x;
    app.targetY = app.marks[i].y;
}

// Вписать прямоугольник в рабочую область (сдвигом, без изменения размера).
static void ClampInto(const RECT& work, RECT& r) {
    LONG ww = r.right - r.left, hh = r.bottom - r.top;
    LONG x = r.left, y = r.top;
    if (x + ww > work.right)  x = work.right - ww;
    if (y + hh > work.bottom) y = work.bottom - hh;
    if (x < work.left) x = work.left;
    if (y < work.top)  y = work.top;
    r = { x, y, x + ww, y + hh };
}

// При выходе: собрать окна на видимый рабочий стол (в рабочую область монитора).
void GatherToDesktop(AppState& app) {
    for (auto& w : app.wins) {
        if (!IsWindow(w.hwnd)) continue;
        if (IsZoomed(w.hwnd)) {
            // В режиме «развёрнутые двигаются с холстом» такое окно могло уехать за экран —
            // пересобираем развёртывание на ближайшем мониторе. В обычном режиме не трогали.
            if (app.moveMaximized) { ShowWindow(w.hwnd, SW_RESTORE); ShowWindow(w.hwnd, SW_MAXIMIZE); }
            continue;
        }
        RECT r;
        if (!GetWindowRect(w.hwnd, &r)) continue;
        MONITORINFO mi = { sizeof(mi) };
        if (!GetMonitorInfoW(MonitorFromWindow(w.hwnd, MONITOR_DEFAULTTONEAREST), &mi)) continue;
        RECT to = r;
        ClampInto(mi.rcWork, to);
        SetWindowPos(w.hwnd, nullptr, to.left, to.top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // Свёрнутые и скрытые окна, ушедшие «с парковки»: их обычное положение — у края экрана,
    // и после выхода программы окно развернулось бы туда же, почти целиком за экраном.
    // Правим положение, не разворачивая и не показывая окно.
    for (auto& a : app.away) {
        if (!a.parked || !IsWindow(a.hwnd)) continue;
        WINDOWPLACEMENT wp = { sizeof(wp) };
        if (!GetWindowPlacement(a.hwnd, &wp)) continue;
        MONITORINFO mi = { sizeof(mi) };
        if (!GetMonitorInfoW(MonitorFromRect(&wp.rcNormalPosition, MONITOR_DEFAULTTONEAREST), &mi)) continue;
        ClampInto(mi.rcWork, wp.rcNormalPosition);
        wp.flags = 0;
        if (IsIconic(a.hwnd))               wp.showCmd = SW_SHOWMINNOACTIVE;   // остаётся свёрнутым
        else if (!IsWindowVisible(a.hwnd))  wp.showCmd = SW_HIDE;              // остаётся скрытым
        else continue;                      // уже на экране — им займётся обычная синхронизация
        SetWindowPlacement(a.hwnd, &wp);
    }
}
