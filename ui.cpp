// FastGithubCpp.UI.exe - Win32 desktop launcher with live traffic graph.
// Unicode build (-municode). Reads engine stats from fastgithubcpp.stats.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static HWND g_status = nullptr;
static HWND g_speed = nullptr;
static HWND g_button = nullptr;
static HWND g_log = nullptr;
static HANDLE g_child = nullptr;
static DWORD g_pid = 0;

static std::vector<double> g_rx_hist(60, 0.0);
static std::vector<double> g_tx_hist(60, 0.0);
static double g_rx_speed = 0.0;
static double g_tx_speed = 0.0;
static uint64_t g_total_rx = 0;
static uint64_t g_total_tx = 0;
static uint64_t g_last_rx = 0;
static uint64_t g_last_tx = 0;
static long long g_last_ms = 0;

static NOTIFYICONDATAW g_nid{};
static const UINT WM_TRAY = WM_USER + 1;
static const int ID_BTN = 1;
static const int ID_SHOW = 2;
static const int ID_EXIT = 3;

struct StatsShared {
    volatile LONG64 timestamp_ms;
    volatile LONG64 rx;
    volatile LONG64 tx;
};
static const char* STATS_MAP_NAME = "Local\\FastGithubCppStats";
static HANDLE g_stats_map = nullptr;
static StatsShared* g_stats_view = nullptr;
static HANDLE g_single_mutex = nullptr;
static const char* SINGLE_MUTEX_NAME = "Local\\FastGithubCppUI_SingleInstance";

static std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

static std::wstring exe_dir_w() {
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s(buf);
    size_t p = s.find_last_of(L"\\/");
    return p == std::wstring::npos ? L"." : s.substr(0, p);
}

// Narrow path for Win32/MinGW file APIs. Must be the system ANSI codepage,
// not UTF-8, otherwise Chinese directory names cannot be opened.
static std::string exe_dir_ansi() {
    std::wstring w = exe_dir_w();
    int n = WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return ".";
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

static void set_status(const std::wstring& s) {
    if (g_status) SetWindowTextW(g_status, s.c_str());
}

static bool is_admin() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION elev{};
    DWORD len = sizeof(elev);
    BOOL ok = GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &len);
    CloseHandle(tok);
    return ok && elev.TokenIsElevated;
}

static void ensure_admin_or_exit() {
    if (is_admin()) return;
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    HINSTANCE r = ShellExecuteW(nullptr, L"runas", path, nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) {
        wchar_t msg[128] = {0};
        swprintf(msg, 128, L"请求管理员权限失败，错误码 %d。", static_cast<int>(reinterpret_cast<INT_PTR>(r)));
        MessageBoxW(nullptr, msg, L"FastGithubCpp", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    }
}

static std::wstring format_rate(double bps) {
    wchar_t buf[64] = {0};
    if (bps >= 1024.0 * 1024.0) swprintf(buf, 64, L"%.2f MB/s", bps / (1024.0 * 1024.0));
    else if (bps >= 1024.0) swprintf(buf, 64, L"%.1f KB/s", bps / 1024.0);
    else swprintf(buf, 64, L"%.0f B/s", bps);
    return buf;
}

static std::wstring format_total(uint64_t bytes) {
    wchar_t buf[64] = {0};
    if (bytes >= 1024ULL * 1024ULL * 1024ULL)
        swprintf(buf, 64, L"%.2f GB", bytes / double(1024ULL * 1024ULL * 1024ULL));
    else if (bytes >= 1024ULL * 1024ULL)
        swprintf(buf, 64, L"%.1f MB", bytes / double(1024ULL * 1024ULL));
    else if (bytes >= 1024ULL)
        swprintf(buf, 64, L"%.1f KB", bytes / 1024.0);
    else
        swprintf(buf, 64, L"%llu B", static_cast<unsigned long long>(bytes));
    return buf;
}

static void start_engine() {
    if (g_child) return;
    std::wstring exe = exe_dir_w() + L"\\FastGithubCpp.exe";
    std::wstring cmd = L"\"" + exe + L"\" --engine";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        set_status(L"启动失败 (CreateProcess 错误 " + std::to_wstring(GetLastError()) + L")");
        return;
    }
    g_child = pi.hProcess;
    g_pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    set_status(L"运行中 (PID " + std::to_wstring(g_pid) + L") - " +
               (is_admin() ? L"管理员" : L"非管理员") + L" - 无需设置代理");
    if (g_button) SetWindowTextW(g_button, L"停止");
}

static void stop_engine() {
    if (!g_child) return;
    TerminateProcess(g_child, 0);
    WaitForSingleObject(g_child, 3000);
    CloseHandle(g_child);
    g_child = nullptr;
    g_pid = 0;
    set_status(L"已停止");
    if (g_button) SetWindowTextW(g_button, L"启动");
}

static void refresh_log() {
    if (!g_log) return;
    std::ifstream f(exe_dir_ansi() + "\\fastgithubcpp.log");
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line)) {
        lines.push_back(line);
        if (lines.size() > 12) lines.erase(lines.begin());
    }
    std::wstring s;
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        s += utf8_to_wide(*it);
        s += L"\r\n";
    }
    SetWindowTextW(g_log, s.c_str());
    SendMessageW(g_log, EM_SETSEL, 0, 0);
    SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
}

static void refresh_stats(HWND hwnd) {
    if (!g_stats_view) {
        g_stats_map = OpenFileMappingA(FILE_MAP_READ, FALSE, STATS_MAP_NAME);
        if (g_stats_map)
            g_stats_view = static_cast<StatsShared*>(
                MapViewOfFile(g_stats_map, FILE_MAP_READ, 0, 0, sizeof(StatsShared)));
    }
    long long ms = 0;
    uint64_t rx = 0, tx = 0;
    bool have = false;
    if (g_stats_view) {
        // Read-only view: plain aligned 64-bit reads (no interlocked write).
        ms = static_cast<long long>(g_stats_view->timestamp_ms);
        rx = static_cast<uint64_t>(g_stats_view->rx);
        tx = static_cast<uint64_t>(g_stats_view->tx);
        have = (ms > 0);
    }
    if (!have) {
        g_rx_speed = g_tx_speed = 0.0;
    } else {
        g_total_rx = rx;
        g_total_tx = tx;
        long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (now - ms > 3000) {
            g_rx_speed = g_tx_speed = 0.0;
        } else if (g_last_ms > 0 && ms > g_last_ms) {
            double sec = (ms - g_last_ms) / 1000.0;
            double rs = (rx >= g_last_rx) ? static_cast<double>(rx - g_last_rx) / sec : 0.0;
            double ts = (tx >= g_last_tx) ? static_cast<double>(tx - g_last_tx) / sec : 0.0;
            g_rx_speed = rs;
            g_tx_speed = ts;
            g_rx_hist.erase(g_rx_hist.begin());
            g_tx_hist.erase(g_tx_hist.begin());
            g_rx_hist.push_back(rs);
            g_tx_hist.push_back(ts);
        }
        g_last_ms = ms;
        g_last_rx = rx;
        g_last_tx = tx;
    }
    std::wstring text = L"下载 " + format_rate(g_rx_speed) +
                        L"    上传 " + format_rate(g_tx_speed) +
                        L"    累计 " + format_total(g_total_rx + g_total_tx);
    if (g_speed) SetWindowTextW(g_speed, text.c_str());
    RECT gr{16, 130, 454, 250};
    InvalidateRect(hwnd, &gr, FALSE);
}

static void draw_graph(HDC hdc) {
    const int left = 16, top = 130, right = 454, bottom = 250;
    RECT bg{left, top, right, bottom};
    HBRUSH brush = CreateSolidBrush(RGB(250, 250, 250));
    FillRect(hdc, &bg, brush);
    DeleteObject(brush);

    double maxv = 1024.0;
    for (double v : g_rx_hist) maxv = std::max(maxv, v);
    for (double v : g_tx_hist) maxv = std::max(maxv, v);
    maxv *= 1.2;

    HPEN grid = CreatePen(PS_SOLID, 1, RGB(224, 224, 224));
    HPEN old = reinterpret_cast<HPEN>(SelectObject(hdc, grid));
    for (int i = 1; i < 4; ++i) {
        int y = top + (bottom - top) * i / 4;
        MoveToEx(hdc, left, y, nullptr);
        LineTo(hdc, right, y);
    }
    SelectObject(hdc, old);
    DeleteObject(grid);

    auto draw_line = [&](const std::vector<double>& hist, COLORREF color) {
        HPEN pen = CreatePen(PS_SOLID, 2, color);
        HPEN prev = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
        size_t n = hist.size();
        for (size_t i = 0; i < n; ++i) {
            int x = left + static_cast<int>(i * (right - left - 1) / (n - 1));
            int y = bottom - static_cast<int>(hist[i] / maxv * (bottom - top - 4)) - 2;
            if (i == 0) MoveToEx(hdc, x, y, nullptr); else LineTo(hdc, x, y);
        }
        SelectObject(hdc, prev);
        DeleteObject(pen);
    };
    draw_line(g_rx_hist, RGB(30, 110, 190));
    draw_line(g_tx_hist, RGB(220, 120, 30));

    wchar_t label[64] = {0};
    swprintf(label, 64, L"峰值 %.1f KB/s", maxv / 1024.0);
    SetTextColor(hdc, RGB(90, 90, 90));
    SetBkMode(hdc, TRANSPARENT);
    TextOutW(hdc, left + 6, top + 4, label, static_cast<int>(wcslen(label)));
    TextOutW(hdc, right - 120, bottom - 18, L"蓝:下载 橙:上传", 8);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_status = CreateWindowW(L"STATIC", L"准备启动...", WS_CHILD | WS_VISIBLE,
                                 16, 14, 430, 24, hwnd, nullptr, nullptr, nullptr);
        g_speed = CreateWindowW(L"STATIC", L"下载 0 B/s    上传 0 B/s    累计 0 B", WS_CHILD | WS_VISIBLE,
                                16, 44, 430, 22, hwnd, nullptr, nullptr, nullptr);
        g_button = CreateWindowW(L"BUTTON", L"停止", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 16, 72, 80, 28, hwnd, reinterpret_cast<HMENU>(1), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"实时流量 (最近 60 秒):", WS_CHILD | WS_VISIBLE,
                      16, 108, 430, 20, hwnd, nullptr, nullptr, nullptr);
        CreateWindowW(L"STATIC", L"日志 (fastgithubcpp.log):", WS_CHILD | WS_VISIBLE,
                      16, 258, 430, 20, hwnd, nullptr, nullptr, nullptr);
        g_log = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE |
                              ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                              16, 282, 430, 180, hwnd, nullptr, nullptr, nullptr);
        SetTimer(hwnd, 1, 1000, nullptr);

        g_nid.cbSize = sizeof(g_nid);
        g_nid.hWnd = hwnd;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAY;
        g_nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCE(1));
        wcscpy(g_nid.szTip, L"FastGithubCpp - 双击显示");
        Shell_NotifyIconW(NIM_ADD, &g_nid);

        start_engine();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_BTN) {
            if (g_child) stop_engine(); else start_engine();
        } else if (LOWORD(wp) == ID_SHOW) {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
        } else if (LOWORD(wp) == ID_EXIT) {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_TRAY:
        if (wp == 1) {
            if (lp == WM_LBUTTONDBLCLK) {
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
            } else if (lp == WM_RBUTTONUP) {
                POINT pt{};
                GetCursorPos(&pt);
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, ID_SHOW, L"显示窗口");
                AppendMenuW(menu, MF_STRING, ID_EXIT, L"退出");
                SetForegroundWindow(hwnd);
                TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(menu);
            }
        }
        return 0;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        break;
    case WM_TIMER:
        if (g_child && WaitForSingleObject(g_child, 0) == WAIT_OBJECT_0) {
            CloseHandle(g_child);
            g_child = nullptr;
            g_pid = 0;
            set_status(L"引擎已退出");
            if (g_button) SetWindowTextW(g_button, L"启动");
        }
        refresh_stats(hwnd);
        refresh_log();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        draw_graph(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        stop_engine();
        if (g_stats_view) { UnmapViewOfFile(g_stats_view); g_stats_view = nullptr; }
        if (g_stats_map) { CloseHandle(g_stats_map); g_stats_map = nullptr; }
        if (g_single_mutex) { ReleaseMutex(g_single_mutex); CloseHandle(g_single_mutex); g_single_mutex = nullptr; }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nShow) {
    ensure_admin_or_exit();
    if (!is_admin()) return 0;

    g_single_mutex = CreateMutexA(nullptr, TRUE, SINGLE_MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"FastGithubCppUI", nullptr);
        if (existing) {
            ShowWindow(existing, SW_SHOW);
            SetForegroundWindow(existing);
        }
        CloseHandle(g_single_mutex);
        g_single_mutex = nullptr;
        return 0;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"FastGithubCppUI";
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCE(1));
    if (!RegisterClassW(&wc)) return 1;

    HWND hwnd = CreateWindowW(L"FastGithubCppUI", L"FastGithubCpp - GitHub 加速",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              480, 500, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
