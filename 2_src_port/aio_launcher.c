/* aio_launcher.c - ROB RUN all-in-one launcher.
 *
 * One executable that performs the whole start-up sequence:
 *
 *   1. Refuse to run while VBS / Hyper-V / Memory Integrity is active.
 *      The mapper cannot load an image into the kernel in that state, so the
 *      launcher reports why instead of letting kd2.exe fail obscurely.
 *   2. Extract the mapper, driver image, overlay and WebView2 loader from
 *      embedded resources into a payload folder beside the launcher.
 *   3. Map the WmPort driver with `kd2.exe --auto` and verify that
 *      \Device\WmPort and the Key registry value exist.
 *   4. Wait until ROB RUN.exe is running, then hold the overlay back until the
 *      game owns a visible window and menu_delay seconds have passed, so the
 *      overlay starts on the main menu instead of during engine start-up.
 *   5. Start RobRunPort.exe (the overlay) from the payload folder and supervise
 *      it: if it has not reported game data within attach_timeout seconds, it
 *      is restarted (reattached) with a clean runtime cache.
 *
 * The check in step 1 is intentionally conservative: a running hypervisor or
 * an enabled VBS/HVCI policy blocks kernel image mapping, so both are refused.
 * `--ignore-vbs` bypasses the refusal for diagnostics only.
 *
 * Build with build_aio.bat. The launcher is self-contained; only the
 * WebView2 Runtime itself is a separate installation.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>      /* UNICODE_STRING, OBJECT_ATTRIBUTES, FILE_OPEN */
#include <commctrl.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>        /* _countof */
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

#include "aio_resources.h"
#include "brand_resources.h"
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "gdi32.lib")

#pragma comment(lib, "bcrypt.lib")

#define PAYLOAD_DIR_NAME L"payload"
#define GAME_EXE_NAME    L"ROB RUN.exe"
#define OVERLAY_EXE      L"RobRunPort.exe"
#define MAPPER_EXE       L"kd2.exe"
#define DRIVER_IMAGE     L"kernel-mode.sys"
#define WV2_LOADER       L"WebView2Loader.dll"
#define LOG_FILE_NAME    L"aio.log"

/* The image rejected by MAP-DRIVER.bat because of the 2026-11-07 crash dump.
 * Refusing it here keeps the launcher from re-introducing a known-bad image. */
#define KNOWN_BAD_SHA256 \
    "ec3a09682c300747dc3029c4baca5728b01403b20e2193a802bf80dbb14166aa"

#define MAP_TIMEOUT_MS   180000u
#define DRIVER_WAIT_MS    30000u
#define MENU_WINDOW_TIMEOUT_MS 300000u
#define MENU_DELAY_DEFAULT      30

/* The overlay is started only after the game has been sitting on its main menu.
 * Two knobs control that, both settable in RobRunAIO.ini or on the command line:
 *   menu_wait_window - wait until the game owns a visible top-level window
 *   menu_delay       - extra seconds to wait after that window appears
 * The window appears when the engine finishes booting; the delay covers the
 * menu's own load. Tune menu_delay to the observed boot time of the game. */
static int g_menu_delay = MENU_DELAY_DEFAULT;
static int g_menu_wait_window = 1;

/* After the overlay starts, its status.log shows whether it attached. If a
 * match is running and no snapshot appears within attach_timeout seconds, the
 * overlay is restarted (reattached) with a clean runtime cache. */
static int g_attach_timeout = 120;
static int g_reattach_attempts = 3;
/* Game pid taken from the visible game window; passed to the overlay so it
 * attaches to that exact process instead of the first same-named one. */
static DWORD g_expected_pid = 0;
/* Hiding the launcher through the driver is off: the process-list unlink is
 * cross-checked by PatchGuard on this build (PsActiveProcessHead against
 * KiProcessListHead and PspCidTable) and bugchecks 0x109. The benign-name
 * masquerade below replaces it. Set hide_launcher=1 only for older builds. */
static int g_hide_launcher = 0;

/* Each launch copies the launcher and the overlay to a random name from this
 * pool, so Task Manager never shows RobRunAIO.exe or RobRunPort.exe. */
static int g_masquerade = 1;
static wchar_t g_ini_path[MAX_PATH * 2];
static wchar_t g_masq_name[64];
/* Folder the overlay actually runs from (its own directory), which is where it
 * writes status.log, robrun_runtime.txt and CoreRR.cfg. */
static wchar_t g_overlay_dir[MAX_PATH * 2];
static wchar_t g_overlay_override[MAX_PATH * 2];

static HWND    g_wnd    = NULL;
static HWND    g_status = NULL;
static HWND    g_log    = NULL;
static wchar_t g_workdir[MAX_PATH * 2];
static wchar_t g_logpath[MAX_PATH * 2];

/* ------------------------------------------------------------------ ui ---- */

static void pump(void) {
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

static void ui_status(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (g_status) SetWindowTextW(g_status, buf);
    pump();
}

static void log_add(const wchar_t* fmt, ...) {
    wchar_t line[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(line, _countof(line), _TRUNCATE, fmt, ap);
    va_end(ap);

    if (g_log) {
        int len = GetWindowTextLengthW(g_log);
        SendMessageW(g_log, EM_SETSEL, (WPARAM)len, (LPARAM)len);
        SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)line);
        SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    }
    if (g_logpath[0]) {
        FILE* f = _wfopen(g_logpath, L"a, ccs=UTF-8");
        if (f) {
            fwprintf(f, L"%s\n", line);   /* text mode supplies CRLF */
            fclose(f);
        }
    }
    pump();
}

/* Fatal error: report through a message box, then exit non-zero. */
static void fail(const wchar_t* fmt, ...) {
    wchar_t buf[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    log_add(L"FAILED: %s", buf);
    if (g_wnd) SetWindowTextW(g_status, L"Failed. See the message.");
    pump();
    MessageBoxW(g_wnd, buf, L"Vinny TuSuce Bien", MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
    ExitProcess(1);
}

/* Brand palette (Vinny TuSuce Bien). */
#define C_BG     RGB(8, 9, 13)
#define C_SURF   RGB(16, 18, 24)
#define C_LINE   RGB(42, 47, 60)
#define C_LIME   RGB(165, 255, 54)
#define C_PURPLE RGB(155, 92, 255)
#define C_INK    RGB(245, 245, 247)
#define C_MUTE   RGB(146, 152, 165)

static HFONT   g_f_brand = NULL, g_f_brand_small = NULL, g_f_status = NULL, g_f_log = NULL;
static HBRUSH  g_log_brush = NULL;
static int     g_fonts_loaded = 0;

static void add_mem_font(int id) {
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL h;
    void* data;
    DWORD size, count = 0;
    if (!r) return;
    h = LoadResource(NULL, r);
    if (!h) return;
    data = LockResource(h);
    size = SizeofResource(NULL, r);
    if (data && size) AddFontMemResourceEx(data, size, NULL, &count);
}

static void load_brand_fonts(void) {
    if (g_fonts_loaded) return;
    g_fonts_loaded = 1;
    add_mem_font(IDR_FONT_SPACE);
    add_mem_font(IDR_FONT_INTER);
    g_f_brand = CreateFontW(-16, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Space Grotesk");
    g_f_brand_small = CreateFontW(-8, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Space Grotesk");
    g_f_status = CreateFontW(-11, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Space Grotesk");
    g_f_log = CreateFontW(-9, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Inter");
}

/* Hexagon emblem with the two F glyphs, drawn with GDI. */
static void draw_emblem(HDC dc, int x, int y, int s) {
    POINT p[6];
    HBRUSH fb, oldb;
    HPEN pen, oldp;
    HFONT f, oldf;
    p[0].x = x + s / 2;       p[0].y = y + 1;
    p[1].x = x + s - 2;       p[1].y = y + s / 4;
    p[2].x = x + s - 2;       p[2].y = y + 3 * s / 4;
    p[3].x = x + s / 2;       p[3].y = y + s - 1;
    p[4].x = x + 2;           p[4].y = y + 3 * s / 4;
    p[5].x = x + 2;           p[5].y = y + s / 4;
    fb = CreateSolidBrush(C_SURF);
    pen = CreatePen(PS_SOLID, 2, C_LIME);
    oldb = (HBRUSH)SelectObject(dc, fb);
    oldp = (HPEN)SelectObject(dc, pen);
    Polygon(dc, p, 6);
    SelectObject(dc, oldb);
    SelectObject(dc, oldp);
    DeleteObject(fb);
    DeleteObject(pen);

    f = CreateFontW(-(s * 42 / 100), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH, L"Space Grotesk");
    oldf = (HFONT)SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, C_LIME);
    TextOutW(dc, x + s * 16 / 100, y + s * 24 / 100, L"F", 1);
    SetTextColor(dc, C_PURPLE);
    TextOutW(dc, x + s * 43 / 100, y + s * 24 / 100, L"F", 1);
    SelectObject(dc, oldf);
    DeleteObject(f);
}

static void paint_window(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    TRIVERTEX tv[2];
    GRADIENT_RECT gr;
    int s = 38, tx = 24 + 38 + 14;
    SIZE word = { 0, 0 };
    HPEN pen, oldp;
    HBRUSH nullb, oldb;
    HFONT oldf;

    GetClientRect(h, &rc);
    tv[0].x = 0; tv[0].y = 0;
    tv[0].Red = 0x0c00; tv[0].Green = 0x0f00; tv[0].Blue = 0x1400; tv[0].Alpha = 0;
    tv[1].x = rc.right; tv[1].y = rc.bottom;
    tv[1].Red = 0x0800; tv[1].Green = 0x0900; tv[1].Blue = 0x0d00; tv[1].Alpha = 0;
    gr.UpperLeft = 0; gr.LowerRight = 1;
    GradientFill(dc, tv, 2, &gr, 1, GRADIENT_FILL_RECT_V);

    draw_emblem(dc, 24, 22, s);

    SetBkMode(dc, TRANSPARENT);
    oldf = (HFONT)SelectObject(dc, g_f_brand);
    SetTextCharacterExtra(dc, 2);
    GetTextExtentPoint32W(dc, L"HERITIE ", 8, &word);
    SetTextColor(dc, C_INK);
    TextOutW(dc, tx, 26, L"HERITIE", 7);
    SetTextColor(dc, C_LIME);
    TextOutW(dc, tx + word.cx, 26, L"DESCIMES", 8);
    SetTextCharacterExtra(dc, 0);
    SelectObject(dc, g_f_brand_small);
    SetTextColor(dc, C_MUTE);
    TextOutW(dc, tx, 50, L"ROB RUN  //  PRIVATE BUILD", 26);
    SelectObject(dc, oldf);

    pen = CreatePen(PS_SOLID, 1, C_LINE);
    oldp = (HPEN)SelectObject(dc, pen);

    MoveToEx(dc, 24, 78, NULL);
    LineTo(dc, rc.right - 24, 78);

    nullb = (HBRUSH)GetStockObject(NULL_BRUSH);
    oldb = (HBRUSH)SelectObject(dc, nullb);
    RoundRect(dc, 22, 112, rc.right - 22, rc.bottom - 36, 12, 12);
    SelectObject(dc, oldb);

    MoveToEx(dc, 24, rc.bottom - 38, NULL);
    LineTo(dc, rc.right - 24, rc.bottom - 38);
    SelectObject(dc, oldp);
    DeleteObject(pen);

    oldf = (HFONT)SelectObject(dc, g_f_brand_small);
    SetTextColor(dc, C_MUTE);
    SetTextCharacterExtra(dc, 2);
    TextOutW(dc, 24, rc.bottom - 26, L"Vinny TuSuce Bien", 17);
    SetTextCharacterExtra(dc, 0);
    SelectObject(dc, oldf);

    EndPaint(h, &ps);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        RECT rc;
        load_brand_fonts();
        g_log_brush = CreateSolidBrush(C_SURF);
        GetClientRect(h, &rc);
        g_status = CreateWindowExW(0, L"STATIC", L"Starting...",
                                   WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   24, 86, rc.right - 48, 22, h, NULL, NULL, NULL);
        g_log = CreateWindowExW(0, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                                ES_READONLY | ES_AUTOVSCROLL,
                                24, 116, rc.right - 48, rc.bottom - 156, h, NULL, NULL, NULL);
        if (g_f_status) SendMessageW(g_status, WM_SETFONT, (WPARAM)g_f_status, TRUE);
        if (g_f_log) SendMessageW(g_log, WM_SETFONT, (WPARAM)g_f_log, TRUE);
        return 0;
    }
    case WM_SIZE: {
        RECT rc;
        GetClientRect(h, &rc);
        if (g_status) MoveWindow(g_status, 24, 86, rc.right - 48, 22, TRUE);
        if (g_log) MoveWindow(g_log, 24, 116, rc.right - 48, rc.bottom - 156, TRUE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint_window(h);
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        if ((HWND)lp == g_log) {
            SetTextColor(dc, C_MUTE);
            SetBkColor(dc, C_SURF);
            return (LRESULT)g_log_brush;
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, C_LIME);
        return (LRESULT)GetStockObject(NULL_BRUSH);
    }
    case WM_CLOSE:
        /* Closing the window cancels the sequence and leaves the system as it
         * is: a mapped driver stays mapped, which is the documented behavior. */
        ExitProcess(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void create_window(HINSTANCE inst, const wchar_t* title) {
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon         = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"RobRunAioLauncher";
    RegisterClassW(&wc);

    int w = 640, h = 480;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    g_wnd = CreateWindowExW(WS_EX_APPWINDOW, L"RobRunAioLauncher",
                            title ? title : L"Vinny TuSuce Bien",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            x, y, w, h, NULL, NULL, inst, NULL);
    if (g_wnd) {
        ShowWindow(g_wnd, SW_SHOWNORMAL);
        UpdateWindow(g_wnd);
    }
}

/* ----------------------------------------------------------------- fs ----- */

static void exe_dir(wchar_t* out, size_t cap) {
    GetModuleFileNameW(NULL, out, (DWORD)cap);
    wchar_t* s = wcsrchr(out, L'\\');
    if (s) *s = 0;
}

static void path_join(const wchar_t* dir, const wchar_t* name, wchar_t* out, size_t cap) {
    _snwprintf_s(out, cap, _TRUNCATE, L"%s\\%s", dir, name);
}

static void ensure_dir(const wchar_t* path) {
    wchar_t tmp[MAX_PATH * 2];
    wcsncpy_s(tmp, _countof(tmp), path, _TRUNCATE);
    for (wchar_t* p = tmp + 3; *p; ++p) {
        if (*p == L'\\') {
            *p = 0;
            CreateDirectoryW(tmp, NULL);
            *p = L'\\';
        }
    }
    CreateDirectoryW(tmp, NULL);
}

static int dir_writable(const wchar_t* dir) {
    wchar_t probe[MAX_PATH * 2];
    path_join(dir, L"~aio_write_test.tmp", probe, _countof(probe));
    HANDLE h = CreateFileW(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(h);
    return 1;
}

/* Adopt a folder as the payload location and start a fresh log inside it. */
static void use_workdir(const wchar_t* dir) {
    ensure_dir(dir);
    wcsncpy_s(g_workdir, _countof(g_workdir), dir, _TRUNCATE);
    path_join(g_workdir, LOG_FILE_NAME, g_logpath, _countof(g_logpath));
    FILE* f = _wfopen(g_logpath, L"w, ccs=UTF-8");
    if (f) fclose(f);
    log_add(L"payload folder: %s", g_workdir);

    /* The overlay keeps its settings beside its own exe, which is now the
     * payload folder. Carry over a CoreRR.cfg shipped next to the launcher so
     * an existing configuration is not silently reset. */
    {
        wchar_t exedir[MAX_PATH * 2], src[MAX_PATH * 2], dst[MAX_PATH * 2];
        exe_dir(exedir, _countof(exedir));
        path_join(exedir, L"CoreRR.cfg", src, _countof(src));
        path_join(g_workdir, L"CoreRR.cfg", dst, _countof(dst));
        if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(dst) == INVALID_FILE_ATTRIBUTES &&
            CopyFileW(src, dst, TRUE))
            log_add(L"copied CoreRR.cfg into the payload folder");
    }

    /* Mapper switches travel with the payload, because kd2.exe reads its cfg
     * from its own folder. Copied every run so an edit takes effect at once. */
    {
        wchar_t exedir[MAX_PATH * 2], src[MAX_PATH * 2], dst[MAX_PATH * 2];
        exe_dir(exedir, _countof(exedir));
        path_join(exedir, L"kd2.cfg", src, _countof(src));
        path_join(g_workdir, L"kd2.cfg", dst, _countof(dst));
        if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES &&
            CopyFileW(src, dst, TRUE))
            log_add(L"copied kd2.cfg into the payload folder");
    }
}

/* Prefer a payload folder beside the launcher so the exe stays portable. Fall
 * back to %LOCALAPPDATA% when the launcher sits in a read-only directory. */
static void choose_workdir(void) {
    wchar_t dir[MAX_PATH * 2], cand[MAX_PATH * 2];

    exe_dir(dir, _countof(dir));
    _snwprintf_s(cand, _countof(cand), _TRUNCATE, L"%s\\%s", dir, PAYLOAD_DIR_NAME);
    ensure_dir(cand);
    if (dir_writable(cand)) {
        wcsncpy_s(g_workdir, _countof(g_workdir), cand, _TRUNCATE);
    } else {
        wchar_t local[MAX_PATH * 2];
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, (DWORD)_countof(local));
        if (!n || n >= _countof(local))
            n = GetTempPathW((DWORD)_countof(local), local);
        if (!n)
            fail(L"No writable folder for the payload.\n\n"
                 L"Allow writes beside the launcher, or set LOCALAPPDATA.");
        _snwprintf_s(cand, _countof(cand), _TRUNCATE, L"%s\\RRAIO\\%s", local, PAYLOAD_DIR_NAME);
        ensure_dir(cand);
        if (!dir_writable(cand))
            fail(L"No writable folder for the payload.\n\n"
                 L"Tried:\n%s", cand);
        wcsncpy_s(g_workdir, _countof(g_workdir), cand, _TRUNCATE);
    }
    use_workdir(g_workdir);
}

/* ---------------------------------------------------------- masquerade ---- */

static const wchar_t* kMasqNames[] = {
    L"dllhost", L"taskhostw", L"RuntimeBroker", L"SearchProtocolHost",
    L"backgroundTaskHost", L"WmiPrvSE", L"smartscreen", L"ctfmon", L"sihost"
};

/* Pick a different benign name for every launch. */
static void masq_pick_name(wchar_t* out, size_t cap) {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    unsigned seed = (unsigned)GetTickCount() ^ (unsigned)q.LowPart ^
                    (unsigned)GetCurrentProcessId();
    srand(seed);
    size_t n = sizeof(kMasqNames) / sizeof(kMasqNames[0]);
    wcsncpy_s(out, cap, kMasqNames[rand() % n], _TRUNCATE);
}

static void masq_write_track(const wchar_t* track, const wchar_t* path) {
    FILE* f = _wfopen(track, L"w, ccs=UTF-8");
    if (!f) return;
    fwprintf(f, L"%s\n", path);
    fclose(f);
}

/* Remove the copy created by the previous launch. A still-running copy cannot
 * be deleted and is simply left for the next attempt. */
static void masq_remove_previous(const wchar_t* track) {
    wchar_t path[MAX_PATH * 2];
    path[0] = 0;
    FILE* f = _wfopen(track, L"r, ccs=UTF-8");
    if (!f) return;
    if (!fgetws(path, _countof(path), f)) path[0] = 0;
    fclose(f);
    wchar_t* nl = wcschr(path, L'\n');
    if (nl) *nl = 0;
    if (path[0]) DeleteFileW(path);
}

static void exe_base_name(wchar_t* out, size_t cap) {
    wchar_t path[MAX_PATH * 2];
    GetModuleFileNameW(NULL, path, _countof(path));
    const wchar_t* base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    wcsncpy_s(out, cap, base, _TRUNCATE);
    wchar_t* dot = wcsrchr(out, L'.');
    if (dot) *dot = 0;
}

static void temp_dir(wchar_t* out, size_t cap) {
    DWORD n = GetTempPathW((DWORD)cap, out);
    if (!n || n >= cap) {
        wcsncpy_s(out, cap, L".", _TRUNCATE);
        return;
    }
    if (n > 0 && out[n - 1] == L'\\') out[n - 1] = 0;
}

/* Copy this launcher to %TEMP% under a random name and start it there, so
 * neither the name nor the path points at the tool folder. The payload is on
 * the command line so the copy keeps using the original extracted files.
 * Returns 1 when the child started (the caller then exits). */
static int spawn_masquerade(const wchar_t* cmdline) {
    wchar_t dir[MAX_PATH * 2], self[MAX_PATH * 2], tmp[MAX_PATH * 2], dest[MAX_PATH * 2];
    wchar_t track[MAX_PATH * 2], ini[MAX_PATH * 2], cmd[MAX_PATH * 4];

    exe_dir(dir, _countof(dir));
    GetModuleFileNameW(NULL, self, _countof(self));
    temp_dir(tmp, _countof(tmp));
    path_join(dir, L"masq.txt", track, _countof(track));
    masq_remove_previous(track);

    if (!g_masq_name[0]) masq_pick_name(g_masq_name, _countof(g_masq_name));
    _snwprintf_s(dest, _countof(dest), _TRUNCATE, L"%s\\%s.exe", tmp, g_masq_name);
    if (!CopyFileW(self, dest, FALSE)) return 0;

    if (g_ini_path[0])
        wcsncpy_s(ini, _countof(ini), g_ini_path, _TRUNCATE);
    else
        path_join(dir, L"RobRunAIO.ini", ini, _countof(ini));

    _snwprintf_s(cmd, _countof(cmd), _TRUNCATE,
                 L"\"%s\" --masq --ini \"%s\" --payload \"%s\"%s%s",
                 dest, ini, g_workdir, (cmdline && *cmdline) ? L" " : L"",
                 (cmdline && *cmdline) ? cmdline : L"");

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(dest, cmd, NULL, NULL, FALSE, 0, NULL, tmp, &si, &pi)) {
        DeleteFileW(dest);
        return 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    masq_write_track(track, dest);
    return 1;
}

/* ------------------------------------------------------------- settings ---- */

static wchar_t* trim_w(wchar_t* s) {
    while (*s == L' ' || *s == L'\t' || *s == L'\r' || *s == L'\n') ++s;
    wchar_t* e = s + wcslen(s);
    while (e > s && (e[-1] == L' ' || e[-1] == L'\t' || e[-1] == L'\r' || e[-1] == L'\n')) --e;
    *e = 0;
    return s;
}

/* Read RobRunAIO.ini, either from --ini or from beside the launcher. Missing
 * file means defaults; a malformed line is ignored rather than treated as an
 * error. */
static void load_launcher_ini(void) {
    wchar_t dir[MAX_PATH * 2], path[MAX_PATH * 2];
    if (g_ini_path[0]) {
        wcsncpy_s(path, _countof(path), g_ini_path, _TRUNCATE);
    } else {
        exe_dir(dir, _countof(dir));
        path_join(dir, L"RobRunAIO.ini", path, _countof(path));
    }
    FILE* f = _wfopen(path, L"r, ccs=UTF-8");
    if (!f) return;

    wchar_t line[512];
    while (fgetws(line, _countof(line), f)) {
        wchar_t* s = trim_w(line);
        if (*s == 0 || *s == L'#' || *s == L';' || *s == L'[') continue;
        wchar_t* eq = wcschr(s, L'=');
        if (!eq) continue;
        *eq = 0;
        wchar_t* key = trim_w(s);
        int n = _wtoi(trim_w(eq + 1));
        if (!_wcsicmp(key, L"menu_delay"))
            g_menu_delay = (n < 0) ? 0 : (n > 600 ? 600 : n);
        else if (!_wcsicmp(key, L"menu_wait_window"))
            g_menu_wait_window = (n != 0);
        else if (!_wcsicmp(key, L"attach_timeout"))
            g_attach_timeout = (n < 5) ? 5 : (n > 600 ? 600 : n);
        else if (!_wcsicmp(key, L"reattach_attempts"))
            g_reattach_attempts = (n < 0) ? 0 : (n > 10 ? 10 : n);
        else if (!_wcsicmp(key, L"hide_launcher"))
            g_hide_launcher = (n != 0);
        else if (!_wcsicmp(key, L"masquerade"))
            g_masquerade = (n != 0);
    }
    fclose(f);
}

/* Parse "--name <int>". Returns 1 when a number was present. */
static int parse_int_arg(const wchar_t* cmdline, const wchar_t* name, int* out) {
    if (!cmdline) return 0;
    const wchar_t* p = wcsstr(cmdline, name);
    if (!p) return 0;
    p += wcslen(name);
    while (*p == L' ' || *p == L'\t' || *p == L'=') ++p;
    int v = 0, any = 0;
    while (*p >= L'0' && *p <= L'9') {
        v = v * 10 + (int)(*p - L'0');
        ++p;
        any = 1;
    }
    if (!any) return 0;
    if (v > 600) v = 600;
    *out = v;
    return 1;
}

/* Write one embedded resource. An existing file of the right size is kept,
 * which keeps start-up fast and tolerates a file locked by a running overlay. */
static int extract_resource(int id, const wchar_t* name) {
    HRSRC res = FindResourceW(NULL, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res) {
        log_add(L"error: embedded resource %d is missing", id);
        return 0;
    }
    DWORD size = SizeofResource(NULL, res);
    HGLOBAL glob = LoadResource(NULL, res);
    void* data = glob ? LockResource(glob) : NULL;
    if (!data || !size) {
        log_add(L"error: embedded resource %d could not be read", id);
        return 0;
    }

    wchar_t path[MAX_PATH * 2];
    path_join(g_workdir, name, path, _countof(path));

    /* Always re-extract. A matching file size does NOT mean the bytes match:
     * a small code change often rebuilds to the exact same size, which used to
     * leave a stale overlay in the payload folder across launches. CREATE_ALWAYS
     * overwrites; a copy that is locked because it is running is handled below. */
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
            log_add(L"warning: %s is in use; keeping the existing copy", name);
            return 1;
        }
        log_add(L"error: cannot create %s (0x%08lX)", name, GetLastError());
        return 0;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, size, &written, NULL);
    CloseHandle(h);
    if (!ok || written != size) {
        log_add(L"error: could not write %s (0x%08lX)", name, GetLastError());
        return 0;
    }
    log_add(L"extracted %s (%lu bytes)", name, (unsigned long)size);
    return 1;
}

/* SHA-256 of a small file, lowercase hex. Returns 0 when hashing is unavailable
 * or the file cannot be read; callers treat that as "not the known-bad image". */
static int sha256_file(const wchar_t* path, char outhex[65]) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 4 * 1024 * 1024) {
        CloseHandle(h);
        return 0;
    }
    DWORD len = (DWORD)sz.QuadPart;
    BYTE* buf = (BYTE*)HeapAlloc(GetProcessHeap(), 0, len);
    DWORD read = 0;
    BOOL ok = buf && ReadFile(h, buf, len, &read, NULL);
    CloseHandle(h);
    if (!ok || read != len) {
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
        return 0;
    }

    BCRYPT_ALG_HANDLE alg = NULL;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) {
        HeapFree(GetProcessHeap(), 0, buf);
        return 0;
    }
    UCHAR digest[32];
    NTSTATUS st = BCryptHash(alg, NULL, 0, buf, len, digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(alg, 0);
    HeapFree(GetProcessHeap(), 0, buf);
    if (st < 0) return 0;

    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        outhex[i * 2]     = hex[digest[i] >> 4];
        outhex[i * 2 + 1] = hex[digest[i] & 15];
    }
    outhex[64] = 0;
    return 1;
}

/* ------------------------------------------------------------- vbs check --- */

static int reg_dword(const wchar_t* sub, const wchar_t* name, DWORD* out) {
    DWORD v = 0, cb = sizeof(v);
    LONG r = RegGetValueW(HKEY_LOCAL_MACHINE, sub, name, RRF_RT_REG_DWORD,
                          NULL, &v, &cb);
    if (r != ERROR_SUCCESS) return 0;
    if (out) *out = v;
    return 1;
}

static int hypervisor_present(void) {
    int regs[4] = { 0, 0, 0, 0 };
    __cpuid(regs, 1);
    return (regs[2] >> 31) & 1;   /* CPUID.1:ECX[31] - hypervisor present */
}

static void check_virtualization(int ignore, int report_only) {
    ui_status(L"Checking VBS and virtualization state...");

    const wchar_t* dg = L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard";
    DWORD vbs = 0, hvci = 0, cg = 0;
    int has_vbs  = reg_dword(dg, L"EnableVirtualizationBasedSecurity", &vbs);
    int has_hvci = reg_dword(L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard"
                             L"\\Scenarios\\HypervisorEnforcedCodeIntegrity",
                             L"Enabled", &hvci);
    int has_cg   = reg_dword(L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard"
                             L"\\Scenarios\\CredentialGuard",
                             L"Enabled", &cg);
    int hv = hypervisor_present();

    log_add(L"hypervisor present : %s", hv ? L"YES" : L"no");
    log_add(L"VBS registry       : %s", has_vbs ? (vbs ? L"ENABLED" : L"disabled") : L"not set");
    log_add(L"memory integrity   : %s", has_hvci ? (hvci ? L"ENABLED" : L"disabled") : L"not set");
    log_add(L"credential guard   : %s", has_cg ? (cg ? L"ENABLED" : L"disabled") : L"not set");

    int bad = hv || (has_vbs && vbs) || (has_hvci && hvci) || (has_cg && cg);
    if (!bad) {
        log_add(L"virtualization check passed");
        return;
    }
    if (ignore || report_only) {
        log_add(L"WARNING: VBS/hypervisor is active%s",
                ignore ? L"; continuing because --ignore-vbs was given"
                       : L" (reported only by --selftest)");
        ui_status(L"WARNING: VBS/hypervisor active; continuing because a bypass flag was given.");
        return;
    }

    fail(L"Virtualization-based security is active.\n\n"
         L"The driver mapper cannot load an image while these are enabled.\n\n"
         L"Hypervisor running : %s\n"
         L"VBS (DeviceGuard)  : %s\n"
         L"Memory Integrity   : %s\n"
         L"Credential Guard   : %s\n\n"
         L"Disable them and reboot:\n"
         L"  1. Windows Security > Device security > Core isolation\n"
         L"     > Memory integrity = Off.\n"
         L"  2. Group Policy > Computer Configuration > Administrative Templates\n"
         L"     > System > Device Guard > Turn on Virtualization Based Security\n"
         L"     = Disabled.\n"
         L"     (or set EnableVirtualizationBasedSecurity = 0 under\n"
         L"     HKLM\\SYSTEM\\CurrentControlSet\\Control\\DeviceGuard)\n"
         L"  3. Turn off Hyper-V, WSL2, Windows Sandbox and Docker Desktop,\n"
         L"     then run:  bcdedit /set hypervisorlaunchtype off\n"
         L"  4. Reboot, then run this launcher again.",
         hv ? L"YES" : L"no",
         (has_vbs && vbs) ? L"ENABLED" : L"disabled",
         (has_hvci && hvci) ? L"ENABLED" : L"disabled",
         (has_cg && cg) ? L"ENABLED" : L"disabled");
}

/* --------------------------------------------------------------- driver ---- */

typedef NTSTATUS (WINAPI* PFN_NTCREATEFILE)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
                                            PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG,
                                            ULONG, ULONG, ULONG, PVOID, ULONG);

/* Open the mapped device the same way rr_mem.c does: suffixed NT names first,
 * then the Win32 symlink. Reaching the device proves the image is initialized;
 * the Key value proves the transport key is published. */
static HANDLE open_driver_device(void) {
    HMODULE nd = GetModuleHandleW(L"ntdll.dll");
    PFN_NTCREATEFILE pCF = nd ? (PFN_NTCREATEFILE)GetProcAddress(nd, "NtCreateFile") : NULL;
    if (pCF) {
        for (int n = 7; n >= 0; --n) {
            wchar_t nt[64];
            _snwprintf_s(nt, _countof(nt), _TRUNCATE, L"\\Device\\WmPort%d", n);
            UNICODE_STRING un;
            OBJECT_ATTRIBUTES oa;
            IO_STATUS_BLOCK ios;
            un.Buffer = nt;
            un.Length = (USHORT)(wcslen(nt) * sizeof(wchar_t));
            un.MaximumLength = (USHORT)(un.Length + sizeof(wchar_t));
            InitializeObjectAttributes(&oa, &un, 0, NULL, NULL);
            HANDLE h = NULL;
            NTSTATUS st = pCF(&h, GENERIC_READ | GENERIC_WRITE, &oa, &ios, NULL, 0,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, 0, NULL, 0);
            if (st >= 0 && h) return h;
        }
    }
    HANDLE h = CreateFileA("\\\\.\\WmPort", GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    return (h == INVALID_HANDLE_VALUE) ? NULL : h;
}

static DWORD driver_token(void) {
    DWORD v = 0;
    reg_dword(L"SYSTEM\\WmPort", L"Key", &v);
    return v;
}

static DWORD driver_init_status(void) {
    DWORD v = 0;
    reg_dword(L"SYSTEM\\WmPort", L"BootState", &v);
    return v;
}

static int driver_ready(void) {
    if (!driver_token()) return 0;
    HANDLE h = open_driver_device();
    if (!h) return 0;
    CloseHandle(h);
    return 1;
}

/* The device existing means an image is already mapped: mapping again cannot
 * work and only produces a misleading "no backend" failure. The Key value is
 * published asynchronously by the driver, so on a fast relaunch it can be
 * missing for a moment even though the transport is live. */
static int driver_device_present(void) {
    HANDLE h = open_driver_device();
    if (!h) return 0;
    CloseHandle(h);
    return 1;
}

/* WM_IO_VEIL: action != 0 unlinks the process from the active-process list,
 * action == 0 restores it. Same packed request rr_mem.c sends, so the driver's
 * dispatch reads one layout. */
#define AIO_MKCTL(f) ((ULONG)((0x29u << 16) | (0 << 14) | ((f) << 2) | 0))
#define AIO_IOCTL_VEIL AIO_MKCTL(0x8A8u)

typedef NTSTATUS (WINAPI* PFN_NTDEVICEIOCONTROL)(HANDLE, HANDLE, PVOID, PVOID,
                                                 PIO_STATUS_BLOCK, ULONG, PVOID,
                                                 ULONG, PVOID, ULONG);

#pragma pack(push, 1)
typedef struct { ULONG key; ULONG pid; ULONG action; ULONG pad; } VEIL_CMD;
#pragma pack(pop)

static int driver_hide_process(DWORD pid, int hide) {
    DWORD token = driver_token();
    if (!token) return 0;
    HANDLE dev = open_driver_device();
    if (!dev) return 0;
    HMODULE nd = GetModuleHandleW(L"ntdll.dll");
    PFN_NTDEVICEIOCONTROL p = nd
        ? (PFN_NTDEVICEIOCONTROL)GetProcAddress(nd, "NtDeviceIoControlFile") : NULL;
    if (!p) { CloseHandle(dev); return 0; }

    VEIL_CMD cmd;
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.key = token;
    cmd.pid = pid;
    cmd.action = hide ? 1 : 0;
    IO_STATUS_BLOCK ios;
    ZeroMemory(&ios, sizeof(ios));
    NTSTATUS st = p(dev, NULL, NULL, NULL, &ios, AIO_IOCTL_VEIL,
                    &cmd, (ULONG)sizeof(cmd), NULL, 0);
    CloseHandle(dev);
    return st >= 0;
}

static int g_launcher_hidden = 0;

/* Deliberately no restore. The driver's hide unlinks the process entry and its
 * restore writes the remembered neighbours back; those neighbours can have
 * exited in the meantime, and the resulting LIST_ENTRY corruption bugchecks
 * (0x139 subcode 3). The overlay hides itself the same way and never restores,
 * so enabling this option must follow that pattern. Off by default after a
 * crash: `hide_launcher=1` in RobRunAIO.ini or --hide-launcher turns it on. */
static int run_mapper(void) {
    wchar_t exe[MAX_PATH * 2], cmd[MAX_PATH * 4];
    path_join(g_workdir, MAPPER_EXE, exe, _countof(exe));
    /* kd2 always maps "kernel-mode.sys" from its own directory; --auto keeps it
     * from waiting for a key press. */
    _snwprintf_s(cmd, _countof(cmd), _TRUNCATE, L"\"%s\" --auto", exe);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, g_workdir, &si, &pi)) {
        log_add(L"error: could not start kd2.exe (0x%08lX)", GetLastError());
        return 0;
    }
    ui_status(L"Mapping the kernel driver...");
    DWORD wait = WaitForSingleObject(pi.hProcess, MAP_TIMEOUT_MS);
    DWORD code = 0xFFFFFFFF;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (wait != WAIT_OBJECT_0) {
        log_add(L"error: kd2.exe did not finish within %lu s",
                (unsigned long)(MAP_TIMEOUT_MS / 1000));
        return 0;
    }
    log_add(L"kd2.exe exit code: %lu", (unsigned long)code);
    return code == 0;
}

static void wait_for_driver(void) {
    ui_status(L"Verifying the mapped driver...");
    DWORD deadline = GetTickCount() + DRIVER_WAIT_MS;
    for (;;) {
        if (driver_ready()) {
            log_add(L"driver ready (Key 0x%08lX)", (unsigned long)driver_token());
            return;
        }
        if (GetTickCount() > deadline) break;
        pump();
        Sleep(400);
    }
    fail(L"The driver did not come up.\n\n"
         L"kd2.exe reported success, but \\\\.\\WmPort could not be opened or\n"
         L"the Key value is missing.\n\n"
         L"BootState: 0x%08lX\n\n"
         L"Reboot, then run this launcher once more. If it still fails, run\n"
         L"MAP-DRIVER.bat to see the mapper's own output, and confirm that\n"
         L"Memory Integrity and VBS are off.",
         (unsigned long)driver_init_status());
}

/* ---------------------------------------------------------------- game ----- */

static DWORD find_process(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static DWORD wait_for_game(void) {
    DWORD start = GetTickCount();
    for (;;) {
        DWORD pid = find_process(GAME_EXE_NAME);
        if (pid) {
            log_add(L"%s is running (pid %lu)", GAME_EXE_NAME, (unsigned long)pid);
            return pid;
        }
        ui_status(L"Waiting for %s ...  %lu s",
                  GAME_EXE_NAME, (unsigned long)((GetTickCount() - start) / 1000));
        pump();
        Sleep(500);
    }
}

/* Same window criteria the overlay uses: a visible, non-minimized top-level
 * window owned by the game, larger than 100x100. The biggest one wins so a
 * splash or launcher window does not mask the real game window. */
typedef struct { DWORD pid; HWND best; long area; } WIN_SEARCH;

static BOOL CALLBACK find_win_cb(HWND w, LPARAM lp) {
    WIN_SEARCH* s = (WIN_SEARCH*)lp;
    DWORD pid = 0;
    RECT rc;
    GetWindowThreadProcessId(w, &pid);
    if (pid != s->pid) return TRUE;
    if (!IsWindowVisible(w) || IsIconic(w) || GetWindow(w, GW_OWNER)) return TRUE;
    if (!GetClientRect(w, &rc)) return TRUE;
    long width = rc.right - rc.left, height = rc.bottom - rc.top;
    if (width > 100 && height > 100 && width * height > s->area) {
        s->area = width * height;
        s->best = w;
    }
    return TRUE;
}

static int game_window(DWORD pid, wchar_t* title, size_t cap, int* cw, int* ch) {
    WIN_SEARCH s;
    ZeroMemory(&s, sizeof(s));
    s.pid = pid;
    EnumWindows(find_win_cb, (LPARAM)&s);
    if (!s.best) return 0;
    if (title && cap) {
        title[0] = 0;
        GetWindowTextW(s.best, title, (int)cap);
    }
    RECT rc;
    if (cw && ch && GetClientRect(s.best, &rc)) {
        *cw = (int)(rc.right - rc.left);
        *ch = (int)(rc.bottom - rc.top);
    }
    return 1;
}

static HWND game_window_hwnd(DWORD pid) {
    WIN_SEARCH s;
    ZeroMemory(&s, sizeof(s));
    s.pid = pid;
    EnumWindows(find_win_cb, (LPARAM)&s);
    return s.best;
}

/* Several processes can share the image name (a crashed copy left behind, a
 * second launcher). Selection is by the process that actually owns a window,
 * not by enumeration order. */
static int find_processes(const wchar_t* name, DWORD* pids, int max) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    int n = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0 && n < max)
                pids[n++] = pe.th32ProcessID;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return n;
}

/* Hold the overlay back until the game is at its main menu: first the game's
 * own window must exist, then menu_delay seconds must pass so the menu can
 * finish loading. Waiting for the process alone starts the overlay during the
 * engine's own start-up, which is what this delay exists to avoid.
 * Returns the pid that owns the game window, or 0 when it was not located. */
static DWORD wait_for_menu(void) {
    DWORD window_pid = 0;
    if (!g_menu_wait_window && g_menu_delay <= 0) {
        log_add(L"menu wait disabled");
        return 0;
    }

    if (g_menu_wait_window) {
        DWORD start = GetTickCount();
        wchar_t title[256];
        int cw = 0, ch = 0;
        for (;;) {
            DWORD pids[8];
            int np = find_processes(GAME_EXE_NAME, pids, (int)_countof(pids));
            if (np == 0)
                fail(L"%s closed before its main window appeared.", GAME_EXE_NAME);

            long best_area = 0;
            DWORD best_pid = 0;
            wchar_t best_title[256];
            best_title[0] = 0;
            int best_w = 0, best_h = 0;
            for (int i = 0; i < np; ++i) {
                wchar_t t[256];
                int w = 0, h = 0;
                if (!game_window(pids[i], t, _countof(t), &w, &h)) continue;
                long area = (long)w * h;
                if (area > best_area) {
                    best_area = area;
                    best_pid = pids[i];
                    best_w = w;
                    best_h = h;
                    wcsncpy_s(best_title, _countof(best_title), t, _TRUNCATE);
                }
            }
            if (best_pid) {
                wcsncpy_s(title, _countof(title), best_title, _TRUNCATE);
                cw = best_w;
                ch = best_h;
                window_pid = best_pid;
                log_add(L"game window \"%s\" %dx%d (pid %lu) after %lu s",
                        title, cw, ch, (unsigned long)best_pid,
                        (unsigned long)((GetTickCount() - start) / 1000));
                break;
            }
            ui_status(L"Waiting for the game window...  %lu s",
                      (unsigned long)((GetTickCount() - start) / 1000));
            pump();
            Sleep(250);
            if (GetTickCount() - start > MENU_WINDOW_TIMEOUT_MS) {
                log_add(L"warning: no game window after %lu s; continuing",
                        (unsigned long)(MENU_WINDOW_TIMEOUT_MS / 1000));
                break;
            }
        }
    }

    if (g_menu_delay > 0) {
        log_add(L"waiting %d s for the main menu", g_menu_delay);
        DWORD start = GetTickCount();
        DWORD total = (DWORD)g_menu_delay * 1000u;
        for (;;) {
            DWORD elapsed = GetTickCount() - start;
            if (elapsed >= total) break;
            if (!find_process(GAME_EXE_NAME))
                fail(L"%s closed while waiting for the main menu.", GAME_EXE_NAME);
            ui_status(L"Waiting for the main menu...  %lu s left",
                      (unsigned long)((total - elapsed + 999) / 1000));
            pump();
            Sleep(200);
        }
    }
    log_add(L"menu wait finished");
    return window_pid;
}

/* --------------------------------------------------------------- overlay --- */

typedef struct {
    HANDLE proc;
    HANDLE thread;
    DWORD  pid;
} OVERLAY;

/* Snapshot of the overlay's own status.log. The overlay appends one line per
 * 3000 ticks: "<tick> pid=<game pid> n=<players> ... status=[...]". A line with
 * n > 0 written after the launch proves the overlay attached to the game. */
typedef struct {
    int   attached;   /* a line after `since` had n > 0                    */
    DWORD game_pid;   /* game pid the overlay reported last, 0 when none   */
    int   lines;      /* lines written after `since`                       */
} OVERLAY_STATE;

static void read_overlay_status(DWORD since, OVERLAY_STATE* out) {
    ZeroMemory(out, sizeof(*out));
    wchar_t path[MAX_PATH * 2];
    const wchar_t* base = g_overlay_dir[0] ? g_overlay_dir : g_workdir;
    path_join(base, L"status.log", path, _countof(path));
    FILE* f = _wfopen(path, L"r, ccs=UTF-8");
    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long size = ftell(f);
    if (size <= 0) { fclose(f); return; }
    long start = size > 131072 ? size - 131072 : 0;   /* last 128 KiB */
    if (fseek(f, start, SEEK_SET) != 0) { fclose(f); return; }

    wchar_t line[512];
    int skip_partial = start > 0;
    while (fgetws(line, _countof(line), f)) {
        if (skip_partial) { skip_partial = 0; continue; }
        unsigned long t = 0, pid = 0;
        int n = 0;
        if (swscanf(line, L"%lu pid=%lu n=%d", &t, &pid, &n) != 3) continue;
        if ((DWORD)t < since) continue;
        out->lines++;
        out->game_pid = (DWORD)pid;
        if (n > 0) {
            out->attached = 1;
            break;
        }
    }
    fclose(f);
}

/* A stale runtime cache can keep pointing at a dead world, so a reattach starts
 * from a clean scan. The old file is kept for inspection. */
static void reset_overlay_state(void) {
    const wchar_t* base = g_overlay_dir[0] ? g_overlay_dir : g_workdir;
    wchar_t rt[MAX_PATH * 2], bak[MAX_PATH * 2];
    path_join(base, L"robrun_runtime.txt", rt, _countof(rt));
    path_join(base, L"robrun_runtime.bak", bak, _countof(bak));
    if (GetFileAttributesW(rt) == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileW(bak);
    if (MoveFileW(rt, bak))
        log_add(L"moved robrun_runtime.txt aside for a clean rescan");
    else if (DeleteFileW(rt))
        log_add(L"deleted robrun_runtime.txt for a clean rescan");
}

static void overlay_exe_path(wchar_t* exe, size_t cap, wchar_t* dir, size_t dircap) {
    if (g_overlay_override[0]) {
        wcsncpy_s(exe, cap, g_overlay_override, _TRUNCATE);
        wcsncpy_s(dir, dircap, g_overlay_override, _TRUNCATE);
        wchar_t* s = wcsrchr(dir, L'\\');
        if (s) *s = 0;
        return;
    }
    path_join(g_workdir, OVERLAY_EXE, exe, cap);
    wcsncpy_s(dir, dircap, g_workdir, _TRUNCATE);
}

/* Start one overlay instance. Returns 1 when it is still running after the
 * start-up grace period; a fast exit is reported as a failure because retrying
 * cannot fix a missing WebView2 Runtime or a broken payload. */
static int launch_overlay(OVERLAY* ov) {
    wchar_t exe[MAX_PATH * 2], dir[MAX_PATH * 2], run_dir[MAX_PATH * 2], cmd[MAX_PATH * 4];
    overlay_exe_path(exe, _countof(exe), dir, _countof(dir));
    wcsncpy_s(run_dir, _countof(run_dir), dir, _TRUNCATE);

    /* Give the overlay a random benign name and run it from %TEMP%, so neither
     * its name nor its path points at the tool folder. WebView2Loader.dll and
     * CoreRR.cfg travel with it; status.log and the runtime cache then live in
     * the same temp folder, which is where the launcher reads them from. */
    if (g_masquerade && !g_overlay_override[0]) {
        wchar_t track[MAX_PATH * 2], dest[MAX_PATH * 2], name[64], tmp[MAX_PATH * 2];
        temp_dir(tmp, _countof(tmp));
        path_join(g_workdir, L"masq_overlay.txt", track, _countof(track));
        masq_remove_previous(track);
        masq_pick_name(name, _countof(name));
        _snwprintf_s(dest, _countof(dest), _TRUNCATE, L"%s\\%s.exe", tmp, name);
        if (CopyFileW(exe, dest, FALSE)) {
            wchar_t src[MAX_PATH * 2], dst[MAX_PATH * 2];
            path_join(dir, L"WebView2Loader.dll", src, _countof(src));
            path_join(tmp, L"WebView2Loader.dll", dst, _countof(dst));
            CopyFileW(src, dst, FALSE);
            path_join(dir, L"CoreRR.cfg", src, _countof(src));
            path_join(tmp, L"CoreRR.cfg", dst, _countof(dst));
            if (GetFileAttributesW(dst) == INVALID_FILE_ATTRIBUTES)
                CopyFileW(src, dst, FALSE);

            /* Carry the resolved runtime cache into the temp folder too. Without
             * it the overlay starts a cold class/world discovery, which blocks
             * its read thread before the first snapshot and looks like "waiting
             * for game data" for minutes. The overlay validates every cached
             * pointer, so a stale file is safe: it is simply discarded. */
            path_join(dir, L"robrun_runtime.txt", src, _countof(src));
            path_join(tmp, L"robrun_runtime.txt", dst, _countof(dst));
            CopyFileW(src, dst, FALSE);
            path_join(dir, L"robrun_chain.txt", src, _countof(src));
            path_join(tmp, L"robrun_chain.txt", dst, _countof(dst));
            CopyFileW(src, dst, FALSE);

            masq_write_track(track, dest);
            wcsncpy_s(exe, _countof(exe), dest, _TRUNCATE);
            wcsncpy_s(run_dir, _countof(run_dir), tmp, _TRUNCATE);
            log_add(L"overlay runs as %s.exe in %s", name, tmp);
        } else {
            log_add(L"warning: overlay masquerade copy failed (0x%08lX)", GetLastError());
        }
    }
    wcsncpy_s(g_overlay_dir, _countof(g_overlay_dir), run_dir, _TRUNCATE);

    if (g_expected_pid)
        _snwprintf_s(cmd, _countof(cmd), _TRUNCATE, L"\"%s\" --pid %lu", exe,
                     (unsigned long)g_expected_pid);
    else
        _snwprintf_s(cmd, _countof(cmd), _TRUNCATE, L"\"%s\"", exe);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    ui_status(L"Starting the overlay...");
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, run_dir, &si, &pi)) {
        fail(L"Could not start the overlay (0x%08lX).\n\nPath:\n%s",
             GetLastError(), exe);
    }

    /* The overlay runs until the game exits or End is pressed. Exiting within a
     * couple of seconds means it failed to initialize (for example a missing
     * WebView2 Runtime), so report the exit code instead of closing silently. */
    DWORD wait = WaitForSingleObject(pi.hProcess, 2500);
    if (wait == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        fail(L"The overlay exited immediately (code %lu).\n\n"
             L"Check that the WebView2 Runtime is installed and that WebView2Loader.dll\n"
             L"is beside the overlay:\n%s",
             (unsigned long)code, exe);
    }

    ov->proc = pi.hProcess;
    ov->thread = pi.hThread;
    ov->pid = pi.dwProcessId;
    log_add(L"overlay started (pid %lu)", (unsigned long)ov->pid);
    return 1;
}

/* True when the game has a visible, non-minimized window. A minimized game or
 * one that has not created its window yet cannot show overlay data, so a
 * restart there would only churn. */
static int game_visible(DWORD pid) {
    if (pid) {
        HWND gw = game_window_hwnd(pid);
        if (gw && !IsIconic(gw)) return 1;
    }
    DWORD pids[8];
    int np = find_processes(GAME_EXE_NAME, pids, (int)_countof(pids));
    for (int i = 0; i < np; ++i) {
        HWND gw = game_window_hwnd(pids[i]);
        if (gw && !IsIconic(gw)) return 1;
    }
    return 0;
}

/* Watch the overlay after start-up. An in-match game that the overlay cannot
 * attach to is the case this exists for: rather than leaving "Waiting for game"
 * on screen for minutes, restart the overlay, which reruns discovery with a
 * clean runtime cache. */
static void supervise_overlay(DWORD expected_game_pid) {
    int attempts = 0;
    int hold_logged = 0;
    for (;;) {
        OVERLAY ov;
        if (!launch_overlay(&ov)) return;   /* fail() already reported it */

        DWORD launched = GetTickCount();
        OVERLAY_STATE st;
        int attached = 0;
        int exited = 0;

        for (;;) {
            DWORD elapsed = GetTickCount() - launched;
            read_overlay_status(launched, &st);
            if (st.attached) { attached = 1; break; }
            if (WaitForSingleObject(ov.proc, 0) == WAIT_OBJECT_0) { exited = 1; break; }
            if (elapsed >= (DWORD)g_attach_timeout * 1000u) {
                if (!game_visible(expected_game_pid)) {
                    if (!hold_logged) {
                        log_add(L"game window minimized or absent; holding the reattach");
                        hold_logged = 1;
                    }
                    launched = GetTickCount();
                    pump();
                    Sleep(500);
                    continue;
                }
                break;
            }
            ui_status(L"Overlay started. Waiting for game data...  %lu s",
                      (unsigned long)(elapsed / 1000));
            pump();
            Sleep(500);
        }

        if (attached) {
            log_add(L"overlay attached (game pid %lu)", (unsigned long)st.game_pid);
            if (expected_game_pid && st.game_pid && st.game_pid != expected_game_pid)
                log_add(L"warning: overlay attached to pid %lu, window owner is %lu",
                        (unsigned long)st.game_pid, (unsigned long)expected_game_pid);
            ui_status(L"Overlay attached.  F1 = menu, Insert = overlay on/off, End = quit.");
            CloseHandle(ov.thread);
            CloseHandle(ov.proc);
            Sleep(800);
            return;
        }

        /* A missing game is not something a restart can fix. */
        {
            DWORD pids[8];
            if (find_processes(GAME_EXE_NAME, pids, (int)_countof(pids)) == 0) {
                log_add(L"game exited; leaving the overlay alone");
                CloseHandle(ov.thread);
                CloseHandle(ov.proc);
                return;
            }
        }

        if (attempts >= g_reattach_attempts) {
            log_add(L"overlay did not attach after %d attempt(s); leaving it running%s",
                    attempts + 1, exited ? L" (last instance had exited)" : L"");
            ui_status(L"Overlay is running but has not found game data. See aio.log.");
            CloseHandle(ov.thread);
            CloseHandle(ov.proc);
            Sleep(2500);
            return;
        }

        ++attempts;
        log_add(L"no game data after %lu s; reattaching (%d/%d)%s%s",
                (unsigned long)((GetTickCount() - launched) / 1000),
                attempts, g_reattach_attempts,
                exited ? L", overlay had exited" : L"",
                (st.game_pid && expected_game_pid && st.game_pid != expected_game_pid)
                    ? L", attached to a different pid" : L"");
        if (!exited) {
            TerminateProcess(ov.proc, 1);
            WaitForSingleObject(ov.proc, 5000);
        }
        CloseHandle(ov.thread);
        CloseHandle(ov.proc);
        reset_overlay_state();
        ui_status(L"Reattaching the overlay...");
        pump();
        Sleep(1500);
    }
}

/* ----------------------------------------------------------------- main ---- */

/* Read an optional "--name <path>" argument. Quoted paths are accepted so a
 * folder with spaces works. Returns 1 when a non-empty path was parsed. */
static int parse_path_arg(const wchar_t* cmdline, const wchar_t* name,
                          wchar_t* out, size_t cap) {
    if (!cmdline) return 0;
    const wchar_t* p = wcsstr(cmdline, name);
    if (!p) return 0;
    p += wcslen(name);
    while (*p == L' ' || *p == L'\t') ++p;
    wchar_t quote = 0;
    if (*p == L'"' || *p == L'\'') { quote = *p; ++p; }
    size_t i = 0;
    while (*p && i + 1 < cap) {
        if (quote) {
            if (*p == quote) break;
        } else if (*p == L' ' || *p == L'\t') {
            break;
        }
        out[i++] = *p++;
    }
    out[i] = 0;
    return i > 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show) {
    (void)prev;
    (void)show;

    int ignore_vbs = cmdline && wcsstr(cmdline, L"--ignore-vbs");
    /* --selftest extracts the payload and reports the VBS state, then exits
     * without mapping a driver or waiting for the game. Useful after a rebuild. */
    int selftest = cmdline && wcsstr(cmdline, L"--selftest");
    wchar_t payload_arg[MAX_PATH * 2];
    int has_payload_arg = parse_path_arg(cmdline, L"--payload", payload_arg,
                                         _countof(payload_arg));
    parse_path_arg(cmdline, L"--overlay", g_overlay_override,
                   _countof(g_overlay_override));

    /* --dry-run exercises detection and timing without a driver or an overlay:
     * useful to tune menu_delay without touching the kernel. */
    int dry_run = cmdline && wcsstr(cmdline, L"--dry-run");
    /* --no-map skips mapping but still starts the overlay (diagnostics). */
    int no_map = cmdline && wcsstr(cmdline, L"--no-map");

    /* Settings are needed before the masquerade decision, and --ini lets the
     * renamed copy read the original ini. */
    parse_path_arg(cmdline, L"--ini", g_ini_path, _countof(g_ini_path));
    load_launcher_ini();

    int masq_child = cmdline && wcsstr(cmdline, L"--masq");

    /* The payload folder is needed before the masquerade decision, because the
     * %TEMP% copy must be told where the extracted files live. */
    if (has_payload_arg)
        use_workdir(payload_arg);
    else
        choose_workdir();

    if (g_masquerade && !masq_child && !selftest) {
        if (spawn_masquerade(cmdline)) return 0;
    }

    HANDLE single = CreateMutexW(NULL, TRUE, L"Global\\RobRunAIO");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"Vinny TuSuce Bien is already running.", L"Vinny TuSuce Bien",
                    MB_ICONINFORMATION | MB_OK);
        return 0;
    }

    {
        wchar_t title[64];
        if (masq_child) {
            exe_base_name(title, _countof(title));
        } else {
            wcsncpy_s(title, _countof(title), L"Vinny TuSuce Bien  \u2014  Rob Run", _TRUNCATE);
        }
        create_window(inst, title);
    }
    log_add(L"Vinny TuSuce Bien  \u2014  Rob Run%s", selftest ? L" (selftest)" : L"");

    /* Run after the payload folder exists so the findings land in aio.log. */
    check_virtualization(ignore_vbs, selftest);

    log_add(L"settings: menu_delay=%d menu_wait_window=%d attach_timeout=%d "
            L"reattach_attempts=%d hide_launcher=%d masquerade=%d",
            g_menu_delay, g_menu_wait_window, g_attach_timeout,
            g_reattach_attempts, g_hide_launcher, g_masquerade);

    /* Menu-wait settings: ini first, then command-line overrides. */
    {
        int v = 0;
        if (parse_int_arg(cmdline, L"--menu-delay", &v)) {
            g_menu_delay = v;
            log_add(L"--menu-delay %d", v);
        }
        if (cmdline && wcsstr(cmdline, L"--no-menu-wait")) {
            g_menu_delay = 0;
            g_menu_wait_window = 0;
            log_add(L"--no-menu-wait");
        }
        if (parse_int_arg(cmdline, L"--attach-timeout", &v)) {
            g_attach_timeout = (v < 5) ? 5 : v;
            log_add(L"--attach-timeout %d", g_attach_timeout);
        }
        if (parse_int_arg(cmdline, L"--reattach-attempts", &v)) {
            g_reattach_attempts = (v < 0) ? 0 : (v > 10 ? 10 : v);
            log_add(L"--reattach-attempts %d", g_reattach_attempts);
        }
    }

    ui_status(L"Extracting payload...");
    int ok = extract_resource(AIO_RCDATA_KD2, MAPPER_EXE) &&
             extract_resource(AIO_RCDATA_DRIVER, DRIVER_IMAGE) &&
             extract_resource(AIO_RCDATA_OVERLAY, OVERLAY_EXE) &&
             extract_resource(AIO_RCDATA_WEBVIEW2, WV2_LOADER);
    if (!ok)
        fail(L"The payload could not be extracted.\n\nFolder:\n%s", g_workdir);

    {
        wchar_t image[MAX_PATH * 2];
        char hex[65];
        path_join(g_workdir, DRIVER_IMAGE, image, _countof(image));
        if (sha256_file(image, hex)) {
            log_add(L"%s SHA256 %hs", DRIVER_IMAGE, hex);
            if (_stricmp(hex, KNOWN_BAD_SHA256) == 0)
                fail(L"The driver image is the build associated with the\n"
                     L"2026-11-07 0x50 crash.\n\n"
                     L"%s\n\nRebuild and redeploy the driver before mapping.",
                     image);
        } else {
            log_add(L"warning: could not hash the driver image");
        }
    }

    if (selftest) {
        log_add(L"SELFTEST OK");
        ui_status(L"Selftest passed. Payload is ready.");
        Sleep(600);
        return 0;
    }

    if (dry_run || no_map) {
        log_add(L"%s: skipping driver mapping",
                dry_run ? L"dry run" : L"--no-map");
    } else if (driver_ready()) {
        log_add(L"driver is already mapped in this session; skipping kd2.exe");
        ui_status(L"Driver already mapped.");
    } else if (driver_device_present()) {
        /* The transport is up but the Key has not been read yet; do not map a
         * second image over it. wait_for_driver() below retries the Key. */
        log_add(L"driver device is live but the Key is not readable yet; "
                L"skipping kd2.exe");
        ui_status(L"Driver already mapped.");
    } else {
        if (!run_mapper()) {
            /* A failed mapper run is not fatal when the device is already live
             * from an earlier launch: the transport is what matters. */
            if (!driver_ready())
                fail(L"Driver mapping failed.\n\n"
                     L"kd2.exe returned a non-zero exit code.\n\n"
                     L"Check that:\n"
                     L"  - VBS, Memory Integrity and Hyper-V are disabled\n"
                     L"  - no previous mapping is stuck (reboot if unsure)\n\n"
                     L"The mapper's own window is hidden; MAP-DRIVER.bat shows it.");
            log_add(L"warning: mapper failed but the driver is live; continuing");
        }
    }

    if (!dry_run && !no_map) {
        wait_for_driver();
        if (g_hide_launcher) {
            if (driver_hide_process(GetCurrentProcessId(), 1)) {
                g_launcher_hidden = 1;
                log_add(L"launcher hidden from the process list (hide-only, no restore)");
            } else {
                log_add(L"warning: could not hide the launcher");
            }
        }
    }
    wait_for_game();
    DWORD window_pid = wait_for_menu();

    if (dry_run) {
        log_add(L"DRY RUN OK");
        ui_status(L"Dry run finished: detection and menu timing verified.");
        Sleep(500);
        return 0;
    }

    g_expected_pid = window_pid;
    supervise_overlay(window_pid);
    log_add(L"done");
    return 0;
}
