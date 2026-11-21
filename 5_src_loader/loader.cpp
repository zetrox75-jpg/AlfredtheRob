/* loader.cpp - Vinny TuSuce Bien public loader.
 *
 * Key-free build: no license server, no key field, no cached credentials.
 * It finds RobRunAIO.exe beside itself (or in payload\) and starts it elevated.
 */

#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#include <shellapi.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <string>
#include <cstring>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

using namespace Gdiplus;

#include "brand_resources.h"

static HWND g_hwnd = nullptr;
static int g_dpi = 96;
static bool g_hover = false;
static bool g_busy = false;
static bool g_status_error = false;

static HFONT g_f_brand = nullptr;
static HFONT g_f_brand_small = nullptr;
static HFONT g_f_eyebrow = nullptr;
static HFONT g_f_body = nullptr;
static HFONT g_f_button = nullptr;
static Image* g_logo = nullptr;
static ULONG_PTR g_gdiplus = 0;
static std::wstring g_status = L"Ready.";

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static void draw_text(HDC dc, int x, int y, const wchar_t* text) {
    TextOutW(dc, x, y, text, (int)wcslen(text));
}

static void* load_rc(HINSTANCE inst, int id, DWORD& size) {
    HRSRC r = FindResourceW(inst, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (!r) return nullptr;
    size = SizeofResource(inst, r);
    HGLOBAL h = LoadResource(inst, r);
    return h ? LockResource(h) : nullptr;
}

static void load_brand(HINSTANCE inst) {
    DWORD n = 0;
    void* p = load_rc(inst, IDR_BRAND_PNG, n);
    if (p && n) {
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, n);
        if (mem) {
            void* dst = GlobalLock(mem);
            if (dst) {
                memcpy(dst, p, n);
                GlobalUnlock(mem);
                IStream* stream = nullptr;
                if (CreateStreamOnHGlobal(mem, TRUE, &stream) == S_OK) {
                    g_logo = Image::FromStream(stream);
                    stream->Release();
                }
            } else {
                GlobalFree(mem);
            }
        }
    }
    DWORD count = 0;
    p = load_rc(inst, IDR_FONT_SPACE, n);
    if (p && n) AddFontMemResourceEx(p, n, nullptr, &count);
    p = load_rc(inst, IDR_FONT_INTER, n);
    if (p && n) AddFontMemResourceEx(p, n, nullptr, &count);
}

static void make_fonts() {
    g_f_brand = CreateFontW(-S(16), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH, L"Space Grotesk");
    g_f_brand_small = CreateFontW(-S(8), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH, L"Space Grotesk");
    g_f_eyebrow = CreateFontW(-S(7), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Space Grotesk");
    g_f_body = CreateFontW(-S(10), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Inter");
    g_f_button = CreateFontW(-S(16), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Space Grotesk");
}

static void round_rect(Graphics& g, RectF r, float rad, Color fill, Color border) {
    GraphicsPath path;
    float d = rad * 2;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
    SolidBrush b(fill);
    g.FillPath(&b, &path);
    if (border.GetA() > 0) {
        Pen p(border, 1.0f);
        g.DrawPath(&p, &path);
    }
}

static std::wstring exe_dir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s = path;
    size_t i = s.find_last_of(L"\\/");
    return i == std::wstring::npos ? L"." : s.substr(0, i);
}

static bool launch_payload(std::wstring& error) {
    const std::wstring dir = exe_dir();
    const wchar_t* names[] = { L"\\RobRunAIO.exe", L"\\payload\\RobRunAIO.exe", L"\\RobRunPort.exe" };
    std::wstring target;
    for (const wchar_t* name : names) {
        std::wstring candidate = dir + name;
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            target = candidate;
            break;
        }
    }
    if (target.empty()) {
        error = L"RobRunAIO.exe was not found next to the loader.";
        return false;
    }

    SHELLEXECUTEINFOW si = {};
    si.cbSize = sizeof(si);
    si.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    si.lpVerb = L"runas";
    si.lpFile = target.c_str();
    si.lpDirectory = dir.c_str();
    si.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&si)) {
        error = L"Windows refused to start the payload.";
        return false;
    }
    if (si.hProcess) CloseHandle(si.hProcess);
    return true;
}

static void start_launch(HWND hwnd) {
    if (g_busy) return;
    g_busy = true;
    g_status_error = false;
    g_status = L"Launching...";
    InvalidateRect(hwnd, nullptr, TRUE);

    std::wstring error;
    if (launch_payload(error)) {
        g_status = L"Launched. You can close this window.";
    } else {
        g_status_error = true;
        g_status = error;
    }
    g_busy = false;
    InvalidateRect(hwnd, nullptr, TRUE);
}

static void paint_ui(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);

    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    LinearGradientBrush bg(Point(0, 0), Point(0, rc.bottom), Color(255, 12, 15, 20), Color(255, 8, 9, 13));
    g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);

    SolidBrush lime_glow(Color(20, 165, 255, 54));
    g.FillEllipse(&lime_glow, rc.right - S(230), -S(110), S(300), S(300));
    SolidBrush purple_glow(Color(24, 155, 92, 255));
    g.FillEllipse(&purple_glow, -S(130), rc.bottom - S(230), S(340), S(340));

    if (g_logo) {
        g.DrawImage(g_logo, RectF((REAL)S(24), (REAL)S(22), (REAL)S(38), (REAL)S(38)));
    }

    Pen divider(Color(255, 42, 47, 60), 1.0f);
    g.DrawLine(&divider, (REAL)S(24), (REAL)S(78), (REAL)(rc.right - S(24)), (REAL)S(78));

    RectF button((REAL)S(32), (REAL)S(190), (REAL)(rc.right - S(64)), (REAL)S(64));
    Color fill = g_busy ? Color(255, 120, 140, 90)
               : g_hover ? Color(255, 190, 255, 110)
               : Color(255, 165, 255, 54);
    round_rect(g, button, (float)S(10), fill, Color(0, 0, 0, 0));

    HDC hdc = g.GetHDC();
    SetBkMode(hdc, TRANSPARENT);

    HFONT old = (HFONT)SelectObject(hdc, g_f_brand);
    SetTextCharacterExtra(hdc, S(2));
    SIZE words = {};
    GetTextExtentPoint32W(hdc, L"HERITIE ", 8, &words);
    SetTextColor(hdc, RGB(245, 245, 247));
    draw_text(hdc, S(76), S(26), L"HERITIE");
    SetTextColor(hdc, RGB(165, 255, 54));
    draw_text(hdc, S(76) + words.cx, S(26), L"DESCIMES");
    SetTextCharacterExtra(hdc, 0);
    SelectObject(hdc, g_f_brand_small);
    SetTextColor(hdc, RGB(146, 152, 165));
    draw_text(hdc, S(76), S(50), L"ROB RUN  //  PUBLIC BUILD");

    SelectObject(hdc, g_f_eyebrow);
    SetTextColor(hdc, RGB(146, 152, 165));
    SetTextCharacterExtra(hdc, S(2));
    draw_text(hdc, S(32), S(102), L"STATUS");
    SetTextCharacterExtra(hdc, 0);

    SelectObject(hdc, g_f_body);
    SetTextColor(hdc, g_status_error ? RGB(255, 70, 85) : RGB(245, 245, 247));
    RECT sr = { S(32), S(120), rc.right - S(32), S(170) };
    DrawTextW(hdc, g_status.c_str(), -1, &sr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);

    SelectObject(hdc, g_f_button);
    SetTextColor(hdc, RGB(8, 9, 13));
    SIZE label = {};
    GetTextExtentPoint32W(hdc, L"LAUNCH", 6, &label);
    int bx = (int)button.X + ((int)button.Width - label.cx) / 2;
    int by = (int)button.Y + ((int)button.Height - label.cy) / 2;
    draw_text(hdc, bx, by, L"LAUNCH");

    SelectObject(hdc, g_f_body);
    SetTextColor(hdc, RGB(146, 152, 165));
    draw_text(hdc, S(32), S(272), L"Keep RobRunAIO.exe beside this loader. It starts elevated.");

    SelectObject(hdc, g_f_brand_small);
    SetTextColor(hdc, RGB(146, 152, 165));
    SetTextCharacterExtra(hdc, S(2));
    draw_text(hdc, S(24), rc.bottom - S(30), L"Vinny TuSuce Bien");
    SIZE right = {};
    GetTextExtentPoint32W(hdc, L"PUBLIC BUILD", 12, &right);
    draw_text(hdc, rc.right - S(24) - right.cx, rc.bottom - S(30), L"PUBLIC BUILD");
    SetTextCharacterExtra(hdc, 0);

    SelectObject(hdc, old);
    g.ReleaseHDC(hdc);

    Pen footer_line(Color(255, 42, 47, 60), 1.0f);
    g.DrawLine(&footer_line, (REAL)S(24), (REAL)(rc.bottom - S(44)), (REAL)(rc.right - S(24)), (REAL)(rc.bottom - S(44)));

    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_dpi = (int)GetDpiForWindow(hwnd);
        if (g_dpi <= 0) g_dpi = 96;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint_ui(hwnd);
        return 0;
    case WM_MOUSEMOVE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT button = { S(32), S(190), rc.right - S(32), S(254) };
        bool hover = PtInRect(&button, pt) != FALSE;
        if (hover != g_hover) {
            g_hover = hover;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_hover) {
            g_hover = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_SETCURSOR: {
        if (LOWORD(lp) == HTCLIENT && g_hover) {
            SetCursor(LoadCursorW(nullptr, (LPCWSTR)IDC_HAND));
            return TRUE;
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        RECT button = { S(32), S(190), rc.right - S(32), S(254) };
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (PtInRect(&button, pt)) start_launch(hwnd);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_RETURN || wp == VK_SPACE) start_launch(hwnd);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    GdiplusStartupInput input;
    if (GdiplusStartup(&g_gdiplus, &input, nullptr) != Ok) return 1;

    load_brand(inst);
    make_fonts();

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"HeritieLoader";
    RegisterClassW(&wc);

    int w = 560, h = 430;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Vinny TuSuce Bien  \u2014  Rob Run",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return 1;
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_logo) { delete g_logo; g_logo = nullptr; }
    GdiplusShutdown(g_gdiplus);
    return 0;
}


