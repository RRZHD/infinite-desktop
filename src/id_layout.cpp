// Сохранение/восстановление раскладки окон
// Часть InfiniteDesktop. Компилируется как единый модуль через main.cpp.

#include "id_common.h"

// ---------- Сохранение/восстановление раскладки окон ----------

std::string WToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring Utf8ToW(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

void WinKey(HWND h, std::wstring& cls, std::wstring& title) {
    wchar_t c[128] = {};
    GetClassNameW(h, c, 128);
    cls = c;
    int n = GetWindowTextLengthW(h);
    std::wstring t((size_t)n + 1, 0);
    int got = GetWindowTextW(h, &t[0], n + 1);
    t.resize(got < 0 ? 0 : got);
    title = t;
}

std::wstring LayoutPath() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring dir = (n && n < MAX_PATH) ? std::wstring(buf) : L".";
    dir += L"\\InfiniteDesktop";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\layout.txt";
}

// Сохранить мировые позиции всех окон (формат: L<TAB>T<TAB>R<TAB>B<TAB>class<TAB>title).
void SaveLayout(AppState& app) {
    std::ofstream f(LayoutPath().c_str(), std::ios::binary | std::ios::trunc);
    if (!f) return;
    for (auto& w : app.wins) {
        if (!IsWindow(w.hwnd)) continue;
        std::wstring cls, title;
        WinKey(w.hwnd, cls, title);
        std::string line = std::to_string(w.world.left)  + "\t" + std::to_string(w.world.top) + "\t"
                         + std::to_string(w.world.right) + "\t" + std::to_string(w.world.bottom) + "\t"
                         + WToUtf8(cls) + "\t" + WToUtf8(title) + "\n";
        f.write(line.data(), (std::streamsize)line.size());
    }
}

void LoadLayout(AppState& app) {
    app.saved.clear();
    std::ifstream f(LayoutPath().c_str(), std::ios::binary);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        long v[4]; size_t p = 0; bool ok = true;
        for (int i = 0; i < 4 && ok; ++i) {
            size_t t = line.find('\t', p);
            if (t == std::string::npos) { ok = false; break; }
            v[i] = atol(line.substr(p, t - p).c_str());
            p = t + 1;
        }
        if (!ok) continue;
        size_t t = line.find('\t', p);          // граница class|title
        if (t == std::string::npos) continue;
        SavedWin sw;
        sw.cls   = Utf8ToW(line.substr(p, t - p));
        sw.title = Utf8ToW(line.substr(t + 1));
        sw.world.left = v[0]; sw.world.top = v[1]; sw.world.right = v[2]; sw.world.bottom = v[3];
        sw.used = false;
        app.saved.push_back(sw);
    }
}

// Разложить окна «полками» без наложения. Старт — ниже уже размещённых окон.
void SpreadWindows(AppState& app, const std::vector<int>& idx) {
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

// При запуске: вернуть окна на сохранённые места; новые/несопоставленные — разложить.
void ArrangeOnStartup(AppState& app) {
    std::vector<bool> assigned(app.wins.size(), false);
    auto tryAssign = [&](bool exact) {
        for (size_t i = 0; i < app.wins.size(); ++i) {
            if (assigned[i]) continue;
            std::wstring cls, title;
            WinKey(app.wins[i].hwnd, cls, title);
            for (auto& s : app.saved) {
                if (s.used) continue;
                if (s.cls != cls) continue;
                if (exact && s.title != title) continue;
                LONG w = app.wins[i].world.right - app.wins[i].world.left;
                LONG h = app.wins[i].world.bottom - app.wins[i].world.top;
                app.wins[i].world.left = s.world.left;  app.wins[i].world.top = s.world.top;
                app.wins[i].world.right = s.world.left + w;
                app.wins[i].world.bottom = s.world.top + h;   // позиция из файла, размер текущий
                s.used = true; assigned[i] = true;
                break;
            }
        }
    };
    tryAssign(true);    // точное совпадение class+title
    tryAssign(false);   // запасное — по одному классу (заголовок мог измениться)

    std::vector<int> toSpread;
    for (size_t i = 0; i < app.wins.size(); ++i)
        if (!assigned[i]) toSpread.push_back((int)i);
    SpreadWindows(app, toSpread);
}

// ---------- Закладки камеры («рабочие места» на бесконечном холсте) ----------
// Бесконечный стол делит на «рабочие места» сама камера: закладка — это точка
// холста, куда можно вернуться одним хоткеем. Хранятся рядом с раскладкой.
std::wstring MarksPath() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring dir = (n && n < MAX_PATH) ? std::wstring(buf) : L".";
    dir += L"\\InfiniteDesktop";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\marks.txt";
}

void SaveMarks(AppState& app) {
    std::ofstream f(MarksPath().c_str(), std::ios::binary | std::ios::trunc);
    if (!f) return;
    for (int i = 0; i < MARK_COUNT; ++i) {
        if (!app.marks[i].set) continue;
        std::string line = std::to_string(i) + " " + std::to_string((long long)llround(app.marks[i].x))
                         + " " + std::to_string((long long)llround(app.marks[i].y)) + "\n";
        f.write(line.data(), (std::streamsize)line.size());
    }
}

void LoadMarks(AppState& app) {
    std::ifstream f(MarksPath().c_str(), std::ios::binary);
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

// При выходе: собрать окна на видимый рабочий стол (в рабочую область монитора).
void GatherToDesktop(AppState& app) {
    for (auto& w : app.wins) {
        if (!IsWindow(w.hwnd) || IsZoomed(w.hwnd)) continue;
        RECT r;
        if (!GetWindowRect(w.hwnd, &r)) continue;
        MONITORINFO mi = { sizeof(mi) };
        if (!GetMonitorInfoW(MonitorFromWindow(w.hwnd, MONITOR_DEFAULTTONEAREST), &mi)) continue;
        LONG ww = r.right - r.left, hh = r.bottom - r.top;
        LONG x = r.left, y = r.top;
        if (x + ww > mi.rcWork.right)  x = mi.rcWork.right - ww;
        if (y + hh > mi.rcWork.bottom) y = mi.rcWork.bottom - hh;
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y < mi.rcWork.top)  y = mi.rcWork.top;
        SetWindowPos(w.hwnd, nullptr, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

// Один кадр анимации/панорамы. dt — реальное время кадра (сглаживание не зависит
// от частоты кадров). Вызывается из высокочастотного цикла, а не по WM_TIMER.


