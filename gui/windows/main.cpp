// ---------------------------------------------------------------------------
// CoherencyLatency 1.2.0 - Windows 11 / Fluent desktop front end (spec v3).
//
// Win32 + GDI/GDI+ + DWM only, /MT static CRT.  Native BUTTON / COMBOBOX /
// PROGRESS_CLASS controls; the NxN heat matrix is its own child window that
// paints from a cached back buffer, so scrolling it never repaints anything
// else.  Shapes come from GDI+, text from GDI (ClearType + CJK font linking).
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "engine_api.h"
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "uxtheme.lib")

using namespace Gdiplus;

namespace {

// Application-private messages.
constexpr UINT WM_CL_PROGRESS = WM_APP + 1;
constexpr UINT WM_CL_DONE = WM_APP + 2;
constexpr UINT WM_CL_AUTOSTART = WM_APP + 3;

enum class Phase { Idle, Running, Finished, Stopped, Failed };

// ---------------------------------------------------------------------------
// Fluent metrics in logical pixels at 96 dpi (multiplied by dpi/96).
// ---------------------------------------------------------------------------
constexpr float kPageMargin = 16.0f;
constexpr float kCardGap = 10.0f;
constexpr float kCardPad = 13.0f;
constexpr float kCardRadius = 8.0f;
constexpr float kCellRadius = 4.0f;
constexpr float kButtonRadius = 4.0f;
constexpr float kCellGap = 3.0f;
constexpr float kCellMax = 40.0f;
constexpr float kCellMin = 18.0f;
constexpr float kCellMinHard = 5.0f;   // colour-map floor: no text at this size
constexpr float kDefaultW = 600.0f;
constexpr float kDefaultH = 880.0f;
constexpr float kMinW = 520.0f;
constexpr float kMinH = 760.0f;
constexpr float kButtonH = 34.0f;
constexpr float kStartW = 140.0f;
constexpr float kStopW = 88.0f;
constexpr float kExportW = 104.0f;
constexpr float kButtonGap = 7.0f;
constexpr float kProgressH = 3.0f;
constexpr float kLegendBarW = 160.0f;
constexpr float kLegendBarH = 8.0f;
constexpr float kScrollBarW = 4.0f;

// WS_CLIPCHILDREN on the frame and WS_CLIPSIBLINGS on every child means the
// parent never paints over a control (flicker item 2).
constexpr DWORD kMainStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                             WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME |
                             WS_CLIPCHILDREN;

constexpr wchar_t kMainClass[] = L"CoherencyLatencyMainWindow";
constexpr wchar_t kMatrixClass[] = L"CoherencyLatencyMatrixWindow";

// Control ids.
constexpr int IDC_START = 1001;
constexpr int IDC_STOP = 1002;
constexpr int IDC_EXPORT = 1003;
constexpr int IDC_PROGRESS = 1005;
constexpr int IDC_MATRIX = 1006;

// ---------------------------------------------------------------------------
// Colour helpers: GDI+ is 0xAARRGGBB, Win32 COLORREF is 0x00BBGGRR.
// ---------------------------------------------------------------------------
inline ARGB Rgb(int r, int g, int b) {
    return 0xFF000000u | (static_cast<ARGB>(r) << 16) |
           (static_cast<ARGB>(g) << 8) | static_cast<ARGB>(b);
}

inline ARGB FromColorRef(COLORREF color) {
    return Rgb(GetRValue(color), GetGValue(color), GetBValue(color));
}

inline COLORREF ToColorRef(ARGB color) {
    return RGB((color >> 16) & 0xFFu, (color >> 8) & 0xFFu, color & 0xFFu);
}

inline ARGB WithAlpha(ARGB color, int alpha) {
    const int a = alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha);
    return (color & 0x00FFFFFFu) | (static_cast<ARGB>(a) << 24);
}

inline ARGB MixColor(ARGB a, ARGB b, float t) {
    const float k = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const int ar = static_cast<int>((a >> 16) & 0xFF), ag = static_cast<int>((a >> 8) & 0xFF),
              ab = static_cast<int>(a & 0xFF);
    const int br = static_cast<int>((b >> 16) & 0xFF), bg = static_cast<int>((b >> 8) & 0xFF),
              bb = static_cast<int>(b & 0xFF);
    return Rgb(ar + static_cast<int>((br - ar) * k + 0.5f),
               ag + static_cast<int>((bg - ag) * k + 0.5f),
               ab + static_cast<int>((bb - ab) * k + 0.5f));
}

// ---------------------------------------------------------------------------
// Semantic palette: light, dark and high-contrast variants (spec 2).
// ---------------------------------------------------------------------------
struct Palette {
    bool dark = false;
    bool highContrast = false;
    ARGB windowBg = 0;
    ARGB cardBg = 0;
    ARGB cardBorder = 0;
    ARGB textPrimary = 0;
    ARGB textSecondary = 0;
    ARGB textTertiary = 0;
    ARGB accent = 0;
    ARGB accentHover = 0;
    ARGB accentPressed = 0;
    ARGB onAccent = 0;
    ARGB buttonBg = 0;
    ARGB buttonHover = 0;
    ARGB buttonPressed = 0;
    ARGB buttonBorder = 0;
    ARGB disabledBg = 0;
    ARGB disabledText = 0;
    ARGB cellIdleBg = 0;
    ARGB cellIdleText = 0;
    ARGB error = 0;
    ARGB scrollThumb = 0;
    ARGB divider = 0;
    ARGB focusRing = 0;
    ARGB trackBg = 0;      // progress-bar trough
    ARGB listBg = 0;       // combo dropdown background
};

// Brushes handed back from WM_CTLCOLOR*; recreated on every theme change.
HBRUSH g_listBrush = nullptr;
HBRUSH g_editBrush = nullptr;

void ReleaseBrushes() {
    if (g_listBrush != nullptr) {
        DeleteObject(g_listBrush);
        g_listBrush = nullptr;
    }
    if (g_editBrush != nullptr) {
        DeleteObject(g_editBrush);
        g_editBrush = nullptr;
    }
}

Palette g_palette;

// ---------------------------------------------------------------------------
// Small utilities.
// ---------------------------------------------------------------------------
std::wstring Fmt(const wchar_t* format, ...) {
    wchar_t buffer[1024] = {};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    return std::wstring(buffer);
}

std::wstring Widen(const char* text) {
    if (text == nullptr || text[0] == '\0') {
        return std::wstring();
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (needed <= 1) {
        return std::wstring();
    }
    std::wstring out(static_cast<size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, &out[0], needed);
    return out;
}

// cl_write_csv() opens the file with std::fopen() and a narrow path, so the
// path has to be encoded in the process ANSI code page, not UTF-8.
std::string NarrowAnsi(const std::wstring& text) {
    std::string out;
    if (text.empty()) {
        return out;
    }
    const int needed = WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, nullptr,
                                           0, nullptr, nullptr);
    if (needed <= 1) {
        return out;
    }
    out.resize(static_cast<size_t>(needed));
    WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, &out[0], needed, nullptr,
                        nullptr);
    out.resize(static_cast<size_t>(needed - 1));
    return out;
}

std::wstring NsText(float value) {
    char buffer[64] = {};
    if (cl_format_ns(value, buffer, static_cast<int>(sizeof(buffer))) != 0) {
        return L"N/A";
    }
    return Widen(buffer);
}

bool ValidCell(float value) { return value == value && value < 1.0e30f; }

float ClampF(float value, float low, float high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

// Spec 4: the heat ramp, identical on every platform.
ARGB HeatColor(float value, float minimum, float maximum) {
    float t = 0.0f;
    const float span = maximum - minimum;
    if (span > 1e-6f && value == value) {
        t = ClampF((value - minimum) / span, 0.0f, 1.0f);
    }
    const float s = (t < 0.5f) ? t * 2.0f : (t - 0.5f) * 2.0f;
    const float c0r = 22.0f, c0g = 183.0f, c0b = 105.0f;
    const float c1r = 244.0f, c1g = 128.0f, c1b = 17.0f;
    const float c2r = 248.0f, c2g = 53.0f, c2b = 51.0f;
    float r, g, b;
    if (t < 0.5f) {
        r = c0r + (c1r - c0r) * s;
        g = c0g + (c1g - c0g) * s;
        b = c0b + (c1b - c0b) * s;
    } else {
        r = c1r + (c2r - c1r) * s;
        g = c1g + (c2g - c1g) * s;
        b = c1b + (c2b - c1b) * s;
    }
    return Rgb(static_cast<int>(ClampF(r, 0.0f, 255.0f) + 0.5f),
               static_cast<int>(ClampF(g, 0.0f, 255.0f) + 0.5f),
               static_cast<int>(ClampF(b, 0.0f, 255.0f) + 0.5f));
}

// ---------------------------------------------------------------------------
// DPI: Per-Monitor v2, then Per-Monitor v1, then system awareness.
// ---------------------------------------------------------------------------
float g_scale = 1.0f;
int g_dpi = 96;
HINSTANCE g_instance = nullptr;

using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
using SetProcessDpiAwarenessFn = HRESULT(WINAPI*)(int);
using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
using GetDpiForSystemFn = UINT(WINAPI*)(void);
using AdjustWindowRectExForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);

HMODULE User32Module() {
    static HMODULE module = GetModuleHandleW(L"user32.dll");
    return module;
}

void EnableDpiAwareness() {
    const HMODULE user32 = User32Module();
    if (user32 != nullptr) {
        auto setContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
        if (setContext != nullptr &&
            setContext(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) {
            return;
        }
    }
    const HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore != nullptr) {
        auto setAwareness = reinterpret_cast<SetProcessDpiAwarenessFn>(
            GetProcAddress(shcore, "SetProcessDpiAwareness"));
        // PROCESS_PER_MONITOR_DPI_AWARE == 2
        if (setAwareness != nullptr && SUCCEEDED(setAwareness(2))) {
            return;
        }
    }
    SetProcessDPIAware();
}

int SystemDpi() {
    const HMODULE user32 = User32Module();
    if (user32 != nullptr) {
        auto getDpi = reinterpret_cast<GetDpiForSystemFn>(
            GetProcAddress(user32, "GetDpiForSystem"));
        if (getDpi != nullptr) {
            const UINT dpi = getDpi();
            if (dpi != 0) {
                return static_cast<int>(dpi);
            }
        }
    }
    int dpi = 96;
    HDC screen = GetDC(nullptr);
    if (screen != nullptr) {
        dpi = GetDeviceCaps(screen, LOGPIXELSX);
        ReleaseDC(nullptr, screen);
    }
    return dpi > 0 ? dpi : 96;
}

int WindowDpi(HWND hwnd) {
    if (hwnd == nullptr) {
        return SystemDpi();
    }
    const HMODULE user32 = User32Module();
    if (user32 != nullptr) {
        auto getDpi = reinterpret_cast<GetDpiForWindowFn>(
            GetProcAddress(user32, "GetDpiForWindow"));
        if (getDpi != nullptr) {
            const UINT dpi = getDpi(hwnd);
            if (dpi != 0) {
                return static_cast<int>(dpi);
            }
        }
    }
    int dpi = 96;
    HDC dc = GetDC(hwnd);
    if (dc != nullptr) {
        dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(hwnd, dc);
    }
    return dpi > 0 ? dpi : 96;
}

void AdjustWindowRectForDpi(RECT* rect, DWORD style, BOOL menu, int dpi) {
    const HMODULE user32 = User32Module();
    if (user32 != nullptr) {
        auto adjust = reinterpret_cast<AdjustWindowRectExForDpiFn>(
            GetProcAddress(user32, "AdjustWindowRectExForDpi"));
        if (adjust != nullptr &&
            adjust(rect, style, menu, 0, static_cast<UINT>(dpi))) {
            return;
        }
    }
    AdjustWindowRectEx(rect, style, menu, 0);
}

int Pxi(float logical) { return static_cast<int>(std::lround(logical * g_scale)); }
float Pxf(float logical) { return logical * g_scale; }

// ---------------------------------------------------------------------------
// Fonts: Segoe UI Variable (fallback Segoe UI), Cascadia Mono (fallback
// Consolas).  GDI font linking supplies Microsoft YaHei UI for CJK text.
// ---------------------------------------------------------------------------
int CALLBACK EnumFamilyProc(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM param) {
    *reinterpret_cast<bool*>(param) = true;
    return 0;
}

bool FamilyExists(const wchar_t* name) {
    LOGFONTW lf = {};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(lf.lfFaceName, LF_FACESIZE, name, _TRUNCATE);
    bool found = false;
    HDC dc = GetDC(nullptr);
    if (dc != nullptr) {
        EnumFontFamiliesExW(dc, &lf, EnumFamilyProc,
                            reinterpret_cast<LPARAM>(&found), 0);
        ReleaseDC(nullptr, dc);
    }
    return found;
}

std::wstring g_uiFont;
std::wstring g_displayFont;
std::wstring g_monoFont;

struct FontSet {
    HFONT title = nullptr;        // 26 semibold, Display
    HFONT subtitle = nullptr;     // 12.5
    HFONT cardTitle = nullptr;    // 13 semibold
    HFONT info = nullptr;         // 12
    HFONT smallText = nullptr;        // 11
    HFONT button = nullptr;       // 13 semibold
    HFONT buttonSecondary = nullptr;  // 13
    HFONT status = nullptr;       // 13
    HFONT sectionTitle = nullptr; // 14 semibold
    HFONT label = nullptr;        // 10.5
    HFONT legend = nullptr;       // 11
    HFONT stats = nullptr;        // 12
    HFONT footer = nullptr;       // 11
    HFONT cellLarge = nullptr;    // 10 bold mono
    HFONT cellMid = nullptr;      // 8.5 bold mono
    HFONT cellSmall = nullptr;    // 7.5 bold mono
    HFONT combo = nullptr;        // 12
};

FontSet g_fonts;
HDC g_measureDc = nullptr;
HBITMAP g_measureBitmap = nullptr;
HGDIOBJ g_measureOldFont = nullptr;

HFONT MakeFont(const std::wstring& family, float logicalSize, int weight) {
    LOGFONTW lf = {};
    lf.lfHeight = -std::max(1, Pxi(logicalSize));
    lf.lfWeight = weight;
    lf.lfItalic = FALSE;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    wcsncpy_s(lf.lfFaceName, LF_FACESIZE, family.c_str(), _TRUNCATE);
    return CreateFontIndirectW(&lf);
}

void DestroyFonts() {
    HFONT* fonts[] = {&g_fonts.title,      &g_fonts.subtitle,
                      &g_fonts.cardTitle,  &g_fonts.info,
                      &g_fonts.smallText,      &g_fonts.button,
                      &g_fonts.buttonSecondary, &g_fonts.status,
                      &g_fonts.sectionTitle, &g_fonts.label,
                      &g_fonts.legend,     &g_fonts.stats,
                      &g_fonts.footer,     &g_fonts.cellLarge,
                      &g_fonts.cellMid,    &g_fonts.cellSmall,
                      &g_fonts.combo};
    for (HFONT* font : fonts) {
        if (*font != nullptr) {
            DeleteObject(*font);
            *font = nullptr;
        }
    }
}

void BuildFonts() {
    if (g_uiFont.empty()) {
        static const wchar_t* const kTextCandidates[] = {
            L"Segoe UI Variable Text", L"Segoe UI"};
        for (const wchar_t* name : kTextCandidates) {
            if (FamilyExists(name)) {
                g_uiFont = name;
                break;
            }
        }
        if (g_uiFont.empty()) {
            g_uiFont = L"Segoe UI";
        }
        static const wchar_t* const kDisplayCandidates[] = {
            L"Segoe UI Variable Display", L"Segoe UI"};
        for (const wchar_t* name : kDisplayCandidates) {
            if (FamilyExists(name)) {
                g_displayFont = name;
                break;
            }
        }
        if (g_displayFont.empty()) {
            g_displayFont = L"Segoe UI";
        }
        static const wchar_t* const kMonoCandidates[] = {L"Cascadia Mono",
                                                         L"Consolas"};
        for (const wchar_t* name : kMonoCandidates) {
            if (FamilyExists(name)) {
                g_monoFont = name;
                break;
            }
        }
        if (g_monoFont.empty()) {
            g_monoFont = L"Consolas";
        }
    }

    DestroyFonts();
    g_fonts.title = MakeFont(g_displayFont, 26.0f, FW_SEMIBOLD);
    g_fonts.subtitle = MakeFont(g_uiFont, 12.5f, FW_NORMAL);
    g_fonts.cardTitle = MakeFont(g_uiFont, 13.0f, FW_SEMIBOLD);
    g_fonts.info = MakeFont(g_uiFont, 12.0f, FW_NORMAL);
    g_fonts.smallText = MakeFont(g_uiFont, 11.0f, FW_NORMAL);
    g_fonts.button = MakeFont(g_uiFont, 13.0f, FW_SEMIBOLD);
    g_fonts.buttonSecondary = MakeFont(g_uiFont, 13.0f, FW_NORMAL);
    g_fonts.status = MakeFont(g_uiFont, 13.0f, FW_NORMAL);
    g_fonts.sectionTitle = MakeFont(g_uiFont, 14.0f, FW_SEMIBOLD);
    g_fonts.label = MakeFont(g_uiFont, 10.5f, FW_NORMAL);
    g_fonts.legend = MakeFont(g_uiFont, 11.0f, FW_NORMAL);
    g_fonts.stats = MakeFont(g_uiFont, 12.0f, FW_NORMAL);
    g_fonts.footer = MakeFont(g_uiFont, 11.0f, FW_NORMAL);
    g_fonts.cellLarge = MakeFont(g_monoFont, 10.0f, FW_BOLD);
    g_fonts.cellMid = MakeFont(g_monoFont, 8.5f, FW_BOLD);
    g_fonts.cellSmall = MakeFont(g_monoFont, 7.5f, FW_BOLD);
    g_fonts.combo = MakeFont(g_uiFont, 12.0f, FW_NORMAL);
}

// ---------------------------------------------------------------------------
// GDI text helpers (ClearType, font linking for CJK).
// ---------------------------------------------------------------------------
SIZE MeasureTextGdi(HFONT font, const std::wstring& text) {
    SIZE size = {0, 0};
    if (font == nullptr || g_measureDc == nullptr || text.empty()) {
        return size;
    }
    HGDIOBJ old = SelectObject(g_measureDc, font);
    GetTextExtentPoint32W(g_measureDc, text.c_str(),
                          static_cast<int>(text.size()), &size);
    SelectObject(g_measureDc, old);
    return size;
}

float TextWidth(HFONT font, const std::wstring& text) {
    return static_cast<float>(MeasureTextGdi(font, text).cx);
}

float LineHeight(HFONT font) {
    if (font == nullptr || g_measureDc == nullptr) {
        return 0.0f;
    }
    HGDIOBJ old = SelectObject(g_measureDc, font);
    TEXTMETRICW metrics = {};
    GetTextMetricsW(g_measureDc, &metrics);
    SelectObject(g_measureDc, old);
    return static_cast<float>(metrics.tmHeight);
}

void DrawTextGdi(HDC dc, const std::wstring& text, HFONT font, ARGB color,
                 const RECT& rect, UINT format) {
    if (dc == nullptr || font == nullptr || text.empty()) {
        return;
    }
    HGDIOBJ old = SelectObject(dc, font);
    const int oldMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldColor = SetTextColor(dc, ToColorRef(color));
    RECT target = rect;
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &target, format);
    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldMode);
    SelectObject(dc, old);
}

void DrawTextLeft(HDC dc, const std::wstring& text, HFONT font, ARGB color,
                  float x, float y, float w, float h, bool ellipsis) {
    RECT r = {static_cast<LONG>(std::lround(x)), static_cast<LONG>(std::lround(y)),
              static_cast<LONG>(std::lround(x + w)),
              static_cast<LONG>(std::lround(y + h))};
    UINT flags = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;
    if (ellipsis) {
        flags |= DT_END_ELLIPSIS;
    }
    DrawTextGdi(dc, text, font, color, r, flags);
}

void DrawTextRight(HDC dc, const std::wstring& text, HFONT font, ARGB color,
                   float x, float y, float w, float h, bool ellipsis) {
    RECT r = {static_cast<LONG>(std::lround(x)), static_cast<LONG>(std::lround(y)),
              static_cast<LONG>(std::lround(x + w)),
              static_cast<LONG>(std::lround(y + h))};
    UINT flags = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_RIGHT;
    if (ellipsis) {
        flags |= DT_END_ELLIPSIS;
    }
    DrawTextGdi(dc, text, font, color, r, flags);
}

void DrawTextCenter(HDC dc, const std::wstring& text, HFONT font, ARGB color,
                    float x, float y, float w, float h) {
    RECT r = {static_cast<LONG>(std::lround(x)), static_cast<LONG>(std::lround(y)),
              static_cast<LONG>(std::lround(x + w)),
              static_cast<LONG>(std::lround(y + h))};
    DrawTextGdi(dc, text, font, color, r,
                DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
}

// ---------------------------------------------------------------------------
// Theme: system dark mode, accent colour, high contrast (spec 2).
// ---------------------------------------------------------------------------
bool RegistryDword(HKEY root, const wchar_t* subKey, const wchar_t* name,
                   DWORD* value) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD size = sizeof(DWORD);
    DWORD type = 0;
    DWORD data = 0;
    const LONG rc = RegQueryValueExW(key, name, nullptr, &type,
                                     reinterpret_cast<LPBYTE>(&data), &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_DWORD) {
        return false;
    }
    *value = data;
    return true;
}

bool SystemUsesDarkMode() {
    DWORD lightTheme = 1;
    if (RegistryDword(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      L"AppsUseLightTheme", &lightTheme)) {
        return lightTheme == 0;
    }
    return false;
}

bool SystemUsesHighContrast() {
    HIGHCONTRASTW hc = {};
    hc.cbSize = sizeof(hc);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0)) {
        return (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    }
    return false;
}

// The accent colour lives in the DWM key as 0xAABBGGRR; DwmGetColorizationColor
// returns 0xAARRGGBB.  Fall back to the Windows 11 default #0067C0.
ARGB SystemAccentColor() {
    DWORD accent = 0;
    if (RegistryDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM",
                      L"AccentColor", &accent)) {
        const int r = static_cast<int>(accent & 0xFF);
        const int g = static_cast<int>((accent >> 8) & 0xFF);
        const int b = static_cast<int>((accent >> 16) & 0xFF);
        if (r + g + b > 30) {
            return Rgb(r, g, b);
        }
    }
    DWORD colorization = 0;
    BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&colorization, &opaque))) {
        const int r = static_cast<int>((colorization >> 16) & 0xFF);
        const int g = static_cast<int>((colorization >> 8) & 0xFF);
        const int b = static_cast<int>(colorization & 0xFF);
        if (r + g + b > 30) {
            return Rgb(r, g, b);
        }
    }
    return Rgb(0x00, 0x67, 0xC0);
}

void BuildPalette() {
    Palette p;
    p.highContrast = SystemUsesHighContrast();
    if (p.highContrast) {
        // High contrast: use the system's own colours, never themed fills.
        const ARGB window = FromColorRef(GetSysColor(COLOR_WINDOW));
        const ARGB text = FromColorRef(GetSysColor(COLOR_WINDOWTEXT));
        const ARGB highlight = FromColorRef(GetSysColor(COLOR_HIGHLIGHT));
        const ARGB highlightText = FromColorRef(GetSysColor(COLOR_HIGHLIGHTTEXT));
        p.dark = GetRValue(GetSysColor(COLOR_WINDOW)) < 100;
        p.windowBg = window;
        p.cardBg = window;
        p.cardBorder = text;
        p.textPrimary = text;
        p.textSecondary = text;
        p.textTertiary = text;
        p.accent = highlight;
        p.accentHover = highlight;
        p.accentPressed = highlight;
        p.onAccent = highlightText;
        p.buttonBg = window;
        p.buttonHover = highlight;
        p.buttonPressed = highlight;
        p.buttonBorder = text;
        p.disabledBg = window;
        p.disabledText = FromColorRef(GetSysColor(COLOR_GRAYTEXT));
        p.cellIdleBg = window;
        p.cellIdleText = text;
        p.error = text;
        p.scrollThumb = text;
        p.divider = text;
        p.focusRing = highlight;
        p.trackBg = window;
        p.listBg = window;
        g_palette = p;
        return;
    }

    p.dark = SystemUsesDarkMode();
    p.accent = SystemAccentColor();
    if (p.dark) {
        p.windowBg = Rgb(0x20, 0x20, 0x20);
        p.cardBg = Rgb(0x2B, 0x2B, 0x2B);
        p.cardBorder = Rgb(0x3A, 0x3A, 0x3A);
        p.textPrimary = Rgb(0xFF, 0xFF, 0xFF);
        p.textSecondary = Rgb(0xC8, 0xC8, 0xC8);
        p.textTertiary = Rgb(0x9A, 0x9A, 0x9A);
        p.buttonBg = Rgb(0x33, 0x33, 0x33);
        p.buttonHover = Rgb(0x3D, 0x3D, 0x3D);
        p.buttonPressed = Rgb(0x2A, 0x2A, 0x2A);
        p.buttonBorder = Rgb(0x45, 0x45, 0x45);
        p.disabledBg = Rgb(0x2B, 0x2B, 0x2B);
        p.disabledText = Rgb(0x6E, 0x6E, 0x6E);
        p.cellIdleBg = Rgb(0x33, 0x33, 0x33);
        p.cellIdleText = Rgb(0x8A, 0x8A, 0x8A);
        p.divider = Rgb(0x3A, 0x3A, 0x3A);
        p.scrollThumb = Rgb(0x6E, 0x6E, 0x6E);
        p.trackBg = Rgb(0x3A, 0x3A, 0x3A);
        p.listBg = Rgb(0x2B, 0x2B, 0x2B);
    } else {
        p.windowBg = Rgb(0xF3, 0xF3, 0xF3);
        p.cardBg = Rgb(0xFF, 0xFF, 0xFF);
        p.cardBorder = Rgb(0xE5, 0xE5, 0xE5);
        p.textPrimary = Rgb(0x1A, 0x1A, 0x1A);
        p.textSecondary = Rgb(0x5D, 0x5D, 0x5D);
        p.textTertiary = Rgb(0x8A, 0x8A, 0x8A);
        p.buttonBg = Rgb(0xFB, 0xFB, 0xFB);
        p.buttonHover = Rgb(0xF0, 0xF0, 0xF0);
        p.buttonPressed = Rgb(0xE6, 0xE6, 0xE6);
        p.buttonBorder = Rgb(0xE0, 0xE0, 0xE0);
        p.disabledBg = Rgb(0xF6, 0xF6, 0xF6);
        p.disabledText = Rgb(0xA8, 0xA8, 0xA8);
        p.cellIdleBg = Rgb(0xF0, 0xF0, 0xF0);
        p.cellIdleText = Rgb(0x9A, 0x9A, 0x9A);
        p.divider = Rgb(0xE5, 0xE5, 0xE5);
        p.scrollThumb = Rgb(0xB0, 0xB0, 0xB0);
        p.trackBg = Rgb(0xE5, 0xE5, 0xE5);
        p.listBg = Rgb(0xFF, 0xFF, 0xFF);
    }
    p.accentHover = MixColor(p.accent, p.dark ? Rgb(0xFF, 0xFF, 0xFF)
                                             : Rgb(0x00, 0x00, 0x00),
                             p.dark ? 0.18f : 0.10f);
    p.accentPressed = MixColor(p.accent, p.dark ? Rgb(0x00, 0x00, 0x00)
                                                : Rgb(0x00, 0x00, 0x00),
                               p.dark ? 0.18f : 0.22f);
    p.onAccent = Rgb(0xFF, 0xFF, 0xFF);
    p.error = p.dark ? Rgb(0xFF, 0x99, 0xA4) : Rgb(0xC4, 0x2B, 0x1C);
    p.focusRing = p.textPrimary;
    g_palette = p;
}

// Immersive dark title bar + caption colour (Windows 10 1809+ / Windows 11).
void ApplyWindowTheme(HWND hwnd) {
    if (hwnd == nullptr) {
        return;
    }
    const BOOL dark = g_palette.dark ? TRUE : FALSE;
    // DWMWA_USE_IMMERSIVE_DARK_MODE = 20 (19 on Windows 10 1809-1909).
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark)))) {
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
    }
    // DWMWA_BORDER_COLOR = 34, DWMWA_CAPTION_COLOR = 35 (Windows 11 only).
    const COLORREF caption = ToColorRef(g_palette.windowBg);
    const COLORREF border = ToColorRef(g_palette.cardBorder);
    DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, 34, &border, sizeof(border));
}

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------
struct UiLayout {
    int clientW = 0;
    int clientH = 0;

    RECT titleRect = {};
    RECT subtitleRect = {};
    RECT versionRect = {};

    RECT deviceCard = {};
    RECT deviceTitle = {};
    struct InfoCell {
        RECT rect = {};
        std::wstring text;
        bool heading = false;
    };
    std::vector<InfoCell> infoCells;

    RECT commandRow = {};
    RECT btnStart = {};
    RECT btnStop = {};
    RECT btnExport = {};

    RECT statusRect = {};
    RECT progressRect = {};

    RECT footerRect = {};

    RECT matrixCard = {};
    RECT matrixTitle = {};
    RECT matrixCount = {};
    RECT matrixClient = {};   // matrix child window, parent client coords
    RECT legendRect = {};
    RECT statsRect = {};
    RECT footnoteRect = {};
    RECT matrixUnit = {};

    // Grid geometry, in the child window's client coordinates.
    int threads = 8;
    float cell = 26.0f;
    float gap = 3.0f;
    float labelW = 34.0f;
    float headerH = 20.0f;
    float gridW = 0.0f;
    float gridH = 0.0f;
    float contentH = 0.0f;   // header + rows
    float maxScrollY = 0.0f;
    float maxScrollX = 0.0f;
    bool scrollY = false;
    bool scrollX = false;
    float naturalCell = 0.0f;   // cell size before the 18..40 pt clamp
    HFONT cellFont = nullptr;   // one mono size for the whole grid
    HFONT labelFont = nullptr;  // axis labels (shrinks for large n)
    bool showCellText = false;  // numbers only when the cells are big enough
    bool showStar = false;      // '*' suffix fits in the axis label column
    float sbW = 0.0f;           // reserved strip for the vertical scrollbar
    float sbH = 0.0f;           // reserved strip for the horizontal scrollbar
    float gridOffsetX = 0.0f;   // centring of the grid inside the viewport
    float gridOffsetY = 0.0f;
};

struct AppState {
    HINSTANCE instance = nullptr;
    HWND hwnd = nullptr;
    HWND matrix = nullptr;
    HWND btnStart = nullptr;
    HWND btnStop = nullptr;
    HWND btnExport = nullptr;
    HWND progress = nullptr;

    ClCpuInfo cpu = {};
    std::vector<int> sibling;
    int threads = 8;
    Phase phase = Phase::Idle;
    std::vector<float> cells;
    int pairsDone = 0;
    int pairsTotal = 0;
    double elapsed = 0.0;
    int lastI = -1;
    int lastJ = -1;
    float lastNs = 0.0f;
    std::wstring errorText;
    std::wstring resultDir;
    std::wstring footerText;
    ClAbort* abortFlag = nullptr;
    std::thread worker;

    float scrollY = 0.0f;
    float scrollX = 0.0f;
    bool draggingThumb = false;
    float dragStartPos = 0.0f;
    float dragStartScroll = 0.0f;

    int hoverButton = 0;   // 1 start, 2 stop, 3 export
    UiLayout layout;
    bool haveLayout = false;
    UINT dpi = 96;
};

AppState g_app;
UiLayout g_layout;

std::vector<std::wstring> g_infoLines;
std::wstring g_footnoteText;

struct ProgressPayload {
    int done = 0;
    int total = 0;
    int i = 0;
    int j = 0;
    int affinityOk = 1;
    float ns = 0.0f;
    double elapsed = 0.0;
};

struct DonePayload {
    int rc = 0;
    int threads = 0;
    ClResult result = {};
    char error[256] = {};
};

void InvalidateMatrix();
void RebuildLayout(HWND hwnd);
void UpdateControls(HWND hwnd);

// ---------------------------------------------------------------------------
// Derived text (spec 0: wording is shared between platforms)
// ---------------------------------------------------------------------------
std::wstring SlotList(int n) {
    if (n <= 4) {
        std::wstring out;
        for (int i = 0; i < n; ++i) {
            if (i != 0) {
                out += L",";
            }
            out += Fmt(L"T%d", i);
        }
        return out;
    }
    return Fmt(L"T0…T%d", n - 1);
}

bool SmtInRange(int n) {
    for (int i = 0; i < n && i < static_cast<int>(g_app.sibling.size()); ++i) {
        const int s = g_app.sibling[static_cast<size_t>(i)];
        if (s > i && s < n) {
            return true;
        }
    }
    return false;
}

std::vector<std::pair<int, int>> SmtPairsInRange(int n) {
    std::vector<std::pair<int, int>> pairs;
    for (int i = 0; i < n && i < static_cast<int>(g_app.sibling.size()); ++i) {
        const int s = g_app.sibling[static_cast<size_t>(i)];
        if (s > i && s < n) {
            pairs.push_back(std::make_pair(i, s));
        }
    }
    return pairs;
}

std::wstring PairListText(const std::vector<std::pair<int, int>>& pairs,
                          size_t maxCount) {
    std::wstring out;
    const size_t shown = std::min(pairs.size(), maxCount);
    for (size_t k = 0; k < shown; ++k) {
        if (k != 0) {
            out += L"、";
        }
        out += Fmt(L"T%d/T%d", pairs[k].first, pairs[k].second);
    }
    if (pairs.size() > shown) {
        out += Fmt(L" 等 %d 组", static_cast<int>(pairs.size()));
    }
    return out;
}

void RefreshDerivedText() {
    g_infoLines.clear();
    const ClCpuInfo& cpu = g_app.cpu;
    const int n = g_app.threads;
    const bool smt = SmtInRange(n);
    const std::vector<std::pair<int, int>> pairs = SmtPairsInRange(n);

    g_infoLines.push_back(Fmt(L"%d 个逻辑线程 · %d B 缓存行 · %s", cpu.logical,
                              cpu.cache_line, Widen(cpu.arch).c_str()));
    if (smt) {
        // Same wording as macOS (gui/macos/main.mm:1031): list the pairs, not a
        // count, so the reader can find them in the matrix.
        g_infoLines.push_back(Fmt(L"物理核 %d 个 · 同核超线程：%s", cpu.physical,
                                  PairListText(pairs, 4).c_str()));
    } else if (cpu.physical > 0) {
        g_infoLines.push_back(Fmt(L"物理核 %d 个", cpu.physical));
    }
    g_infoLines.push_back(Fmt(
        L"测量对象：%s 共 %d 个逻辑线程%s", SlotList(n).c_str(), n,
        smt ? L"；同核两个线程之间的延迟通常约 30 ns"
            : L"；跨核线程延迟通常在 30–120 ns"));
    g_infoLines.push_back(
        Fmt(L"固定参数：%d 个逻辑线程 · 1,000,000 次 · 3 轮 · Bounce", n));

    // The matrix-card footnote is the short reminder for the "*" markers; the
    // long explanation already lives in the device card above.
    g_footnoteText = smt ? L"带 * 的线程互为同核超线程" : std::wstring();
}

std::wstring BuildStatusText() {
    switch (g_app.phase) {
        case Phase::Running:
            if (g_app.lastI >= 0) {
                // cl_format_ns returns "N/A" for an unusable sample; appending
                // " ns" to that reads as a broken unit, so it is dropped.
                const std::wstring ns = NsText(g_app.lastNs);
                const std::wstring value =
                    (ns == L"N/A") ? ns : (ns + L" ns");
                return Fmt(L"测量中 %d/%d · T%d↔T%d %s · 用时 %.1f s",
                           g_app.pairsDone, g_app.pairsTotal, g_app.lastI,
                           g_app.lastJ, value.c_str(), g_app.elapsed);
            }
            return Fmt(L"测量中 0/%d · 用时 %.1f s", g_app.pairsTotal,
                       g_app.elapsed);
        case Phase::Finished:
            return Fmt(L"完成 · %d 个线程对 · 用时 %.1f s", g_app.pairsDone,
                       g_app.elapsed);
        case Phase::Stopped:
            return Fmt(L"已停止 · 完成 %d/%d 个线程对 · 用时 %.1f s",
                       g_app.pairsDone, g_app.pairsTotal, g_app.elapsed);
        case Phase::Failed:
            return Fmt(L"出错：%s", g_app.errorText.c_str());
        case Phase::Idle:
        default:
            return Fmt(L"就绪 · %d 个逻辑线程 · %d×%d 线程矩阵", g_app.threads,
                       g_app.threads, g_app.threads);
    }
}

struct Stats {
    bool valid = false;
    double avg = 0.0;
    float minimum = 0.0f;
    float maximum = 0.0f;
    int count = 0;
};

Stats ComputeStats() {
    Stats stats;
    const int n = g_app.threads;
    const size_t expected = static_cast<size_t>(n) * static_cast<size_t>(n);
    if (g_app.cells.size() != expected) {
        return stats;
    }
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            if (i == j) {
                continue;
            }
            const float value = g_app.cells[static_cast<size_t>(i) * n + j];
            if (!ValidCell(value)) {
                continue;
            }
            if (stats.count == 0) {
                stats.minimum = value;
                stats.maximum = value;
            } else {
                stats.minimum = std::min(stats.minimum, value);
                stats.maximum = std::max(stats.maximum, value);
            }
            sum += value;
            ++stats.count;
        }
    }
    if (stats.count > 0) {
        stats.valid = true;
        stats.avg = sum / stats.count;
    }
    return stats;
}

bool HasData() { return ComputeStats().valid; }

std::wstring BuildStatsText() {
    const Stats stats = ComputeStats();
    if (!stats.valid) {
        return L"平均 – · 最小 – · 最大 – ns";
    }
    return Fmt(L"平均 %.1f · 最小 %.1f · 最大 %.1f ns", stats.avg,
               static_cast<double>(stats.minimum),
               static_cast<double>(stats.maximum));
}

float ProgressFraction() {
    if (g_app.phase == Phase::Finished) {
        return 1.0f;
    }
    if (g_app.pairsTotal <= 0) {
        return 0.0f;
    }
    return ClampF(static_cast<float>(g_app.pairsDone) /
                      static_cast<float>(g_app.pairsTotal),
                  0.0f, 1.0f);
}

int CountTextLines(HFONT font, const std::wstring& text, float maxWidth) {
    // GDI wrapping is used for drawing; this mirrors it for layout by measuring
    // with DT_CALCRECT.
    if (g_measureDc == nullptr || font == nullptr || text.empty()) {
        return 1;
    }
    HGDIOBJ old = SelectObject(g_measureDc, font);
    RECT r = {0, 0, static_cast<LONG>(std::max(10.0f, maxWidth)), 0};
    DrawTextW(g_measureDc, text.c_str(), static_cast<int>(text.size()), &r,
              DT_WORDBREAK | DT_LEFT | DT_NOPREFIX | DT_CALCRECT);
    SelectObject(g_measureDc, old);
    const float line = std::max(1.0f, LineHeight(font));
    return std::max(1, static_cast<int>(std::lround(
                            static_cast<float>(r.bottom - r.top) / line)));
}

// ---------------------------------------------------------------------------
// Layout.  Every metric is in logical points and scaled by Pxf(), so a DPI
// change is just a relayout.  The page never scrolls: the matrix card takes the
// remaining height and the matrix child window scrolls internally (which only
// happens when a cell would fall below 18 pt, per spec 1).
// ---------------------------------------------------------------------------
constexpr wchar_t kTitleText[] = L"CoherencyLatency";
constexpr wchar_t kSubtitleText[] = L"缓存一致性延迟测试 · Windows 桌面版";
constexpr wchar_t kVersionText[] = L"1.2.0";
constexpr wchar_t kDeviceTitleText[] = L"设备信息";
constexpr wchar_t kStartText[] = L"开始测试";
constexpr wchar_t kStopText[] = L"停止";
constexpr wchar_t kExportText[] = L"导出 CSV";
constexpr wchar_t kMatrixTitleText[] = L"线程延迟矩阵";
constexpr wchar_t kMatrixUnitText[] = L"ns/op";
constexpr wchar_t kLegendLowText[] = L"低";
constexpr wchar_t kLegendHighText[] = L"高";

struct Chrome {
    // Tuned against layout_selftest: with these values a 760x620 window gives a
    // 26 pt cell at n=8 (slack +25) and 680x520 still fits without a scroller.
    float afterTitle = 5.0f;
    float afterButtons = 2.0f;
    float afterStatus = 2.0f;
    float afterProgress = 3.0f;
    float cardTitleGap = 3.0f;
    float infoGap = 2.0f;
    float matrixTitleGap = 3.0f;
    float aboveLegend = 2.0f;
    float aboveFootnote = 2.0f;
    float bottomGap = 3.0f;
    bool compact = false;
};

UiLayout BuildLayout(int clientW, int clientH, const Chrome& chrome) {
    UiLayout L;
    L.clientW = clientW;
    L.clientH = clientH;
    L.threads = std::max(1, g_app.threads);

    const float m = Pxf(kPageMargin);
    const float pad = Pxf(kCardPad);
    const float contentW = std::max(320.0f, static_cast<float>(clientW) - 2.0f * m);
    const float contentX = m;
    float y = m;

    // -- 标题区: 标题 + 副标题 + 版本号，同一行 --------------------------------
    const float titleH = LineHeight(g_fonts.title);
    const float subtitleH = LineHeight(g_fonts.subtitle);
    const float titleW = TextWidth(g_fonts.title, kTitleText);
    const float subtitleW = TextWidth(g_fonts.subtitle, kSubtitleText);
    const float versionW = TextWidth(g_fonts.smallText, kVersionText);
    const float headLineH = std::max(titleH, subtitleH);
    const float subtitleOffY = std::max(0.0f, (headLineH - subtitleH) * 0.5f);

    L.titleRect = RECT{static_cast<LONG>(std::lround(contentX)),
                       static_cast<LONG>(std::lround(y)),
                       static_cast<LONG>(std::lround(contentX + titleW)),
                       static_cast<LONG>(std::lround(y + headLineH))};
    const float subtitleX = contentX + titleW + Pxf(12.0f);
    const bool subtitleFits =
        (subtitleX + subtitleW + Pxf(10.0f) + versionW) <= (contentX + contentW);
    L.subtitleRect = RECT{
        static_cast<LONG>(std::lround(subtitleFits ? subtitleX : contentX)),
        static_cast<LONG>(std::lround(subtitleFits ? y + subtitleOffY
                                                   : y + headLineH)),
        static_cast<LONG>(std::lround(subtitleFits ? subtitleX + subtitleW
                                                   : contentX + subtitleW)),
        static_cast<LONG>(std::lround(subtitleFits ? y + subtitleOffY + subtitleH
                                                   : y + headLineH + subtitleH))};
    // Nothing may ever be laid out past the content edge: at a narrow width the
    // subtitle/version are clamped (and ellipsized when painted) instead of
    // running under the window frame.
    const float contentRight = contentX + contentW;
    if (static_cast<float>(L.subtitleRect.right) > contentRight) {
        L.subtitleRect.right = static_cast<LONG>(std::lround(contentRight));
    }
    const float versionX = static_cast<float>(L.subtitleRect.right) + Pxf(8.0f);
    const float versionRight = std::min(versionX + versionW, contentRight);
    L.versionRect = RECT{static_cast<LONG>(std::lround(versionX)),
                         L.subtitleRect.top,
                         static_cast<LONG>(std::lround(std::max(versionX, versionRight))),
                         L.subtitleRect.bottom};
    y += subtitleFits ? headLineH : headLineH + subtitleH;
    y += Pxf(chrome.afterTitle);

    // -- 设备信息卡 ---------------------------------------------------------
    // One full-width column: the window is landscape but the card must never
    // squeeze the info text into an ellipsis, so the lines stack and the card
    // grows in height instead of clipping.
    const float cardTitleH = LineHeight(g_fonts.cardTitle);
    const float infoH = LineHeight(g_fonts.info);
    const int infoCount = static_cast<int>(g_infoLines.size());
    const float infoGap = Pxf(chrome.infoGap);
    const float deviceInnerH =
        cardTitleH + Pxf(chrome.cardTitleGap) +
        static_cast<float>(infoCount) * infoH +
        static_cast<float>(std::max(0, infoCount - 1)) * infoGap;
    const float deviceH = deviceInnerH + 2.0f * pad;

    L.deviceCard = RECT{static_cast<LONG>(std::lround(contentX)),
                        static_cast<LONG>(std::lround(y)),
                        static_cast<LONG>(std::lround(contentX + contentW)),
                        static_cast<LONG>(std::lround(y + deviceH))};
    L.deviceTitle = RECT{L.deviceCard.left + static_cast<LONG>(pad),
                         L.deviceCard.top + static_cast<LONG>(pad),
                         L.deviceCard.right - static_cast<LONG>(pad),
                         L.deviceCard.top + static_cast<LONG>(pad + cardTitleH)};

    const float infoY = y + pad + cardTitleH + Pxf(chrome.cardTitleGap);
    const float innerW = contentW - 2.0f * pad;
    L.infoCells.clear();
    for (int k = 0; k < infoCount; ++k) {
        UiLayout::InfoCell cell;
        const float cy = infoY + static_cast<float>(k) * (infoH + infoGap);
        cell.rect = RECT{static_cast<LONG>(std::lround(contentX + pad)),
                         static_cast<LONG>(std::lround(cy)),
                         static_cast<LONG>(std::lround(contentX + pad + innerW)),
                         static_cast<LONG>(std::lround(cy + infoH))};
        cell.text = g_infoLines[static_cast<size_t>(k)];
        L.infoCells.push_back(cell);
    }
    y += deviceH + Pxf(kCardGap);

    // -- 按钮行（原生按钮，这里只算位置） -----------------------------------
    const float buttonH = Pxf(kButtonH);
    const float startW = Pxf(kStartW);
    const float stopW = Pxf(kStopW);
    const float exportW = Pxf(kExportW);
    const float buttonGap = Pxf(kButtonGap);
    L.commandRow = RECT{static_cast<LONG>(std::lround(contentX)),
                        static_cast<LONG>(std::lround(y)),
                        static_cast<LONG>(std::lround(contentX + contentW)),
                        static_cast<LONG>(std::lround(y + buttonH))};
    L.btnStart = RECT{L.commandRow.left, L.commandRow.top,
                      L.commandRow.left + static_cast<LONG>(std::lround(startW)),
                      L.commandRow.bottom};
    L.btnStop = RECT{L.btnStart.right + static_cast<LONG>(std::lround(buttonGap)),
                     L.commandRow.top,
                     L.btnStart.right + static_cast<LONG>(std::lround(buttonGap + stopW)),
                     L.commandRow.bottom};
    L.btnExport =
        RECT{L.btnStop.right + static_cast<LONG>(std::lround(buttonGap)), L.commandRow.top,
             L.btnStop.right + static_cast<LONG>(std::lround(buttonGap + exportW)),
             L.commandRow.bottom};
    y += buttonH + Pxf(chrome.afterButtons);

    // -- 状态行 + 进度条 ----------------------------------------------------
    const float statusH = LineHeight(g_fonts.status);
    L.statusRect = RECT{static_cast<LONG>(std::lround(contentX)),
                        static_cast<LONG>(std::lround(y)),
                        static_cast<LONG>(std::lround(contentX + contentW)),
                        static_cast<LONG>(std::lround(y + statusH))};
    y += statusH + Pxf(chrome.afterStatus);
    L.progressRect = RECT{static_cast<LONG>(std::lround(contentX)),
                          static_cast<LONG>(std::lround(y)),
                          static_cast<LONG>(std::lround(contentX + contentW)),
                          static_cast<LONG>(std::lround(y + Pxf(kProgressH)))};
    y += Pxf(kProgressH) + Pxf(chrome.afterProgress);

    // -- 底栏（贴底） -------------------------------------------------------
    const float footerH = LineHeight(g_fonts.footer);
    const float footerTop = static_cast<float>(clientH) - m - footerH;
    L.footerRect = RECT{static_cast<LONG>(std::lround(contentX)),
                        static_cast<LONG>(std::lround(footerTop)),
                        static_cast<LONG>(std::lround(contentX + contentW)),
                        static_cast<LONG>(std::lround(footerTop + footerH))};

    // -- 矩阵卡（占满剩余高度） ---------------------------------------------
    const float cardTop = y;
    const float cardBottom = std::max(cardTop + Pxf(120.0f),
                                      footerTop - Pxf(chrome.bottomGap));
    L.matrixCard = RECT{static_cast<LONG>(std::lround(contentX)),
                        static_cast<LONG>(std::lround(cardTop)),
                        static_cast<LONG>(std::lround(contentX + contentW)),
                        static_cast<LONG>(std::lround(cardBottom))};

    const float sectionH = LineHeight(g_fonts.sectionTitle);
    const float unitH = LineHeight(g_fonts.smallText);
    const float unitW = TextWidth(g_fonts.smallText, kMatrixUnitText) + Pxf(8.0f);
    const float titleW2 = TextWidth(g_fonts.sectionTitle, kMatrixTitleText);
    L.matrixTitle = RECT{L.matrixCard.left + static_cast<LONG>(pad),
                         L.matrixCard.top + static_cast<LONG>(pad),
                         static_cast<LONG>(std::lround(L.matrixCard.left + pad + titleW2)),
                         L.matrixCard.top + static_cast<LONG>(pad + sectionH)};
    L.matrixUnit = RECT{static_cast<LONG>(std::lround(L.matrixCard.left + pad + titleW2 +
                                                      Pxf(8.0f))),
                        static_cast<LONG>(std::lround(L.matrixCard.top + pad + sectionH -
                                                      unitH)),
                        static_cast<LONG>(std::lround(L.matrixCard.left + pad + titleW2 +
                                                      Pxf(8.0f) + unitW)),
                        static_cast<LONG>(std::lround(L.matrixCard.top + pad + sectionH))};
    L.matrixCount = RECT{L.matrixCard.left + static_cast<LONG>(pad),
                         L.matrixCard.top + static_cast<LONG>(pad),
                         L.matrixCard.right - static_cast<LONG>(pad),
                         L.matrixCard.top + static_cast<LONG>(pad + sectionH)};

    const float legendH = LineHeight(g_fonts.legend);
    const float statsH = LineHeight(g_fonts.stats);
    const float legendRowH = std::max(legendH, std::max(statsH, Pxf(kLegendBarH)));
    const float footnoteLineH = LineHeight(g_fonts.footer);
    const float cardInnerW = static_cast<float>(L.matrixCard.right - L.matrixCard.left) -
                             2.0f * pad;

    int footnoteLines = 1;
    if (!g_footnoteText.empty()) {
        footnoteLines = std::max(1, std::min(2, CountTextLines(g_fonts.footer,
                                                               g_footnoteText,
                                                               cardInnerW)));
    }
    const float footnoteH = static_cast<float>(footnoteLines) * footnoteLineH;
    const float cardBottomPad = static_cast<float>(L.matrixCard.bottom) - pad;
    L.footnoteRect = RECT{L.matrixCard.left + static_cast<LONG>(pad),
                          static_cast<LONG>(std::lround(cardBottomPad - footnoteH)),
                          L.matrixCard.right - static_cast<LONG>(pad),
                          static_cast<LONG>(std::lround(cardBottomPad))};
    const float legendBottom = static_cast<float>(L.footnoteRect.top) - Pxf(chrome.aboveFootnote);
    L.legendRect = RECT{L.matrixCard.left + static_cast<LONG>(pad),
                        static_cast<LONG>(std::lround(legendBottom - legendRowH)),
                        L.matrixCard.right - static_cast<LONG>(pad),
                        static_cast<LONG>(std::lround(legendBottom))};
    L.statsRect = L.legendRect;

    const float childTop = static_cast<float>(L.matrixTitle.bottom) + Pxf(chrome.matrixTitleGap);
    const float childBottom = static_cast<float>(L.legendRect.top) - Pxf(chrome.aboveLegend);
    L.matrixClient = RECT{L.matrixCard.left + static_cast<LONG>(pad),
                          static_cast<LONG>(std::lround(childTop)),
                          L.matrixCard.right - static_cast<LONG>(pad),
                          static_cast<LONG>(std::lround(std::max(childTop + Pxf(56.0f),
                                                                 childBottom)))};
    L.matrixClient.right = std::max(L.matrixClient.right, L.matrixClient.left + 40);
    // Hard guarantee: no rect may stick out past the client area, whatever the
    // DPI / width combination is.
    const LONG maxRight = static_cast<LONG>(std::lround(contentRight));
    const LONG maxBottom = static_cast<LONG>(std::lround(static_cast<float>(clientH) - m));
    for (RECT* r : {&L.deviceCard, &L.commandRow, &L.statusRect, &L.progressRect,
                    &L.matrixCard, &L.footerRect, &L.matrixClient}) {
        if (r->right > maxRight) {
            r->right = maxRight;
        }
        if (r->bottom > maxBottom) {
            r->bottom = maxBottom;
        }
    }
    if (L.matrixClient.bottom <= L.matrixClient.top) {
        L.matrixClient.bottom = L.matrixClient.top + 1;
    }
    return L;
}

// Second stage: grid geometry inside the matrix child, then the label column and
// the scrollbar reservation.  Spec 1: the cell is constrained by BOTH axes and
// the whole NxN must be visible at the default size; a scrollbar appears only
// when the formula would drop a cell below 18 pt.
UiLayout ResolveGrid(UiLayout L) {
    const int n = L.threads;
    const float childW = static_cast<float>(L.matrixClient.right - L.matrixClient.left);
    const float childH = static_cast<float>(L.matrixClient.bottom - L.matrixClient.top);
    const float labelSample = std::max(TextWidth(g_fonts.label, L"T31"),
                                       TextWidth(g_fonts.label, L"T7*"));
    const float labelWidth = std::max(std::ceil(labelSample) + Pxf(6.0f), Pxf(22.0f));
    const float cellGap = Pxf(kCellGap);
    const float headerH = std::max(Pxf(13.0f), LineHeight(g_fonts.label));
    const float nf = static_cast<float>(n);
    const float gaps = (nf - 1.0f) * cellGap;
    const float sbThickness = Pxf(kScrollBarW) + Pxf(4.0f);

    float reserveW = 0.0f;
    float reserveH = 0.0f;
    float cell = 0.0f;
    float gridW = 0.0f;
    float gridH = 0.0f;
    float natural = 0.0f;
    // The matrix is a colour map: it must fit the card with no scrolling for any
    // realistic thread count, so the cell is simply the smaller of the two
    // "fit n cells" results and is only floored at the hard minimum (5 pt).
    for (int pass = 0; pass < 3; ++pass) {
        const float availW = childW - labelWidth - Pxf(4.0f) - reserveW;
        const float availH = childH - headerH - reserveH;
        const float byWidth = availW / nf - cellGap;
        const float byHeight = availH / nf - cellGap;
        natural = std::min(byWidth, byHeight);
        cell = std::floor(ClampF(natural, Pxf(kCellMinHard), Pxf(kCellMax)));
        gridW = nf * cell + gaps;
        gridH = nf * cell + gaps;
        const bool needY = gridH > (childH - headerH - reserveH) + 0.5f;
        const bool needX = gridW > (childW - labelWidth - reserveW) + 0.5f;
        const float wantW = needY ? sbThickness : 0.0f;
        const float wantH = needX ? sbThickness : 0.0f;
        if (wantW == reserveW && wantH == reserveH) {
            break;
        }
        reserveW = wantW;
        reserveH = wantH;
    }

    L.cell = cell;
    L.gap = cellGap;
    L.labelW = labelWidth;
    L.headerH = headerH;
    L.gridW = gridW;
    L.gridH = gridH;
    L.contentH = headerH + gridH;
    L.maxScrollY = std::max(0.0f, gridH - (childH - headerH - reserveH));
    L.maxScrollX = std::max(0.0f, gridW - (childW - labelWidth - reserveW));
    L.scrollY = L.maxScrollY > 0.5f;
    L.scrollX = L.maxScrollX > 0.5f;
    L.sbW = L.scrollY ? Pxf(kScrollBarW) : 0.0f;
    L.sbH = L.scrollX ? Pxf(kScrollBarW) : 0.0f;
    L.naturalCell = natural;
    // The grid is centred in the viewport (it is usually height-bound, so the
    // leftover width belongs on both sides, not all on the right).
    L.gridOffsetX = std::max(0.0f, (childW - labelWidth - L.sbW - gridW) * 0.5f);
    L.gridOffsetY = std::max(0.0f, (childH - headerH - L.sbH - gridH) * 0.5f);

    // Axis label font: 10.5 pt normally, one step smaller when a "T31" would not
    // fit inside a cell.  The '*' SMT marker is dropped as soon as it would make
    // neighbouring headers touch (spec allows this for wide grids).
    L.labelFont = g_fonts.label;
    if (TextWidth(g_fonts.label, L"T31") > cell - Pxf(1.0f)) {
        L.labelFont = g_fonts.cellSmall;
    }
    L.showStar = TextWidth(L.labelFont, L"T31*") <= cell - Pxf(1.0f);

    // Numbers inside the cells only when they actually fit (n <= 8 and the cell
    // is at least 18 pt); otherwise the matrix is a pure colour map and the
    // values live in the CSV.
    L.cellFont = g_fonts.cellLarge;
    if (TextWidth(g_fonts.cellLarge, L"888.8") > cell - Pxf(5.0f)) {
        L.cellFont = g_fonts.cellMid;
        if (TextWidth(g_fonts.cellMid, L"888.8") > cell - Pxf(4.0f)) {
            L.cellFont = g_fonts.cellSmall;
        }
    }
    L.showCellText = (n <= 8) && cell >= Pxf(kCellMin) &&
                     TextWidth(L.cellFont, L"888.8") <= cell - Pxf(4.0f);
    return L;
}

void DumpLayout(const UiLayout& L, const char* tag) {
    char path[MAX_PATH] = {};
    if (GetEnvironmentVariableA("CL_LAYOUT_DUMP", path, MAX_PATH) == 0) {
        return;
    }
    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") != 0 || f == nullptr) {
        return;
    }
    fprintf(f,
            "[%s] client=%dx%d scale=%.3f n=%d card=%ld,%ld %ldx%ld child=%ld,%ld "
            "%ldx%ld cell=%.2f gap=%.2f labelW=%.2f headerH=%.2f grid=%.1fx%.1f "
            "availW=%.1f availH=%.1f natural=%.2f maxScrollY=%.1f maxScrollX=%.1f "
            "sbW=%.1f sbH=%.1f\n",
            tag, L.clientW, L.clientH, static_cast<double>(g_scale), L.threads,
            L.matrixCard.left, L.matrixCard.top,
            L.matrixCard.right - L.matrixCard.left,
            L.matrixCard.bottom - L.matrixCard.top, L.matrixClient.left,
            L.matrixClient.top, L.matrixClient.right - L.matrixClient.left,
            L.matrixClient.bottom - L.matrixClient.top, static_cast<double>(L.cell),
            static_cast<double>(L.gap), static_cast<double>(L.labelW),
            static_cast<double>(L.headerH), static_cast<double>(L.gridW),
            static_cast<double>(L.gridH),
            static_cast<double>((L.matrixClient.right - L.matrixClient.left) -
                                L.labelW - Pxf(4.0f) - L.sbW),
            static_cast<double>((L.matrixClient.bottom - L.matrixClient.top) -
                                L.headerH - L.sbH),
            static_cast<double>(L.naturalCell), static_cast<double>(L.maxScrollY),
            static_cast<double>(L.maxScrollX), static_cast<double>(L.sbW),
            static_cast<double>(L.sbH));
    fclose(f);
}

void ComputeLayout(int clientW, int clientH) {
    Chrome comfortable;
    UiLayout L = ResolveGrid(BuildLayout(clientW, clientH, comfortable));
    if (L.naturalCell < Pxf(kCellMin) - 0.5f) {
        Chrome compact;
        compact.afterTitle = 4.0f;
        compact.afterButtons = 2.0f;
        compact.afterStatus = 2.0f;
        compact.afterProgress = 2.0f;
        compact.cardTitleGap = 3.0f;
        compact.infoGap = 1.0f;
        compact.matrixTitleGap = 2.0f;
        compact.aboveLegend = 2.0f;
        compact.aboveFootnote = 1.0f;
        compact.bottomGap = 2.0f;
        compact.compact = true;
        UiLayout compactLayout = ResolveGrid(BuildLayout(clientW, clientH, compact));
        if (compactLayout.naturalCell > L.naturalCell) {
            L = compactLayout;
        }
    }
    g_app.scrollY = ClampF(g_app.scrollY, 0.0f, L.maxScrollY);
    g_app.scrollX = ClampF(g_app.scrollX, 0.0f, L.maxScrollX);
    g_layout = L;
    g_app.layout = L;
    g_app.haveLayout = true;
    static int dumps = 0;
    if (dumps < 6) {
        ++dumps;
        DumpLayout(L, dumps == 1 ? "build" : "relayout");
    }
}
// ---------------------------------------------------------------------------
// Double buffering.  Every window paints into its own cached memory DC and the
// WM_PAINT handler ends with a single BitBlt of the dirty rectangle, so there
// is never a background erase or a partial frame on screen (spec: no flicker).
// ---------------------------------------------------------------------------
struct BackBuffer {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    int width = 0;
    int height = 0;

    void Release() {
        if (dc != nullptr) {
            if (oldBitmap != nullptr) {
                SelectObject(dc, oldBitmap);
            }
            DeleteDC(dc);
        }
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
        }
        dc = nullptr;
        bitmap = nullptr;
        oldBitmap = nullptr;
        width = 0;
        height = 0;
    }

    void Ensure(HDC reference, int w, int h) {
        if (dc != nullptr && w == width && h == height) {
            return;
        }
        Release();
        if (w <= 0 || h <= 0) {
            return;
        }
        screen = reference;
        dc = CreateCompatibleDC(reference);
        if (dc == nullptr) {
            return;
        }
        bitmap = CreateCompatibleBitmap(reference, w, h);
        if (bitmap == nullptr) {
            DeleteDC(dc);
            dc = nullptr;
            return;
        }
        oldBitmap = SelectObject(dc, bitmap);
        width = w;
        height = h;
    }

    HDC screen = nullptr;
};

BackBuffer g_mainBack;
BackBuffer g_matrixBack;

RectF ToRectF(const RECT& r) {
    return RectF(static_cast<REAL>(r.left), static_cast<REAL>(r.top),
                 static_cast<REAL>(r.right - r.left),
                 static_cast<REAL>(r.bottom - r.top));
}

void FillRoundRect(Graphics& g, const RECT& r, float radius, ARGB color) {
    const RectF rect = ToRectF(r);
    GraphicsPath path;
    const float d = radius * 2.0f;
    if (radius <= 0.5f || rect.Width <= d || rect.Height <= d) {
        path.AddRectangle(rect);
    } else {
        path.AddArc(rect.X, rect.Y, d, d, 180.0f, 90.0f);
        path.AddArc(rect.X + rect.Width - d, rect.Y, d, d, 270.0f, 90.0f);
        path.AddArc(rect.X + rect.Width - d, rect.Y + rect.Height - d, d, d, 0.0f, 90.0f);
        path.AddArc(rect.X, rect.Y + rect.Height - d, d, d, 90.0f, 90.0f);
        path.CloseFigure();
    }
    SolidBrush brush((Color(color)));
    g.FillPath(&brush, &path);
}

void StrokeRoundRect(Graphics& g, const RECT& r, float radius, ARGB color, float width) {
    const RectF rect = ToRectF(r);
    GraphicsPath path;
    const float d = radius * 2.0f;
    if (radius <= 0.5f || rect.Width <= d || rect.Height <= d) {
        path.AddRectangle(rect);
    } else {
        path.AddArc(rect.X, rect.Y, d, d, 180.0f, 90.0f);
        path.AddArc(rect.X + rect.Width - d, rect.Y, d, d, 270.0f, 90.0f);
        path.AddArc(rect.X + rect.Width - d, rect.Y + rect.Height - d, d, d, 0.0f, 90.0f);
        path.AddArc(rect.X, rect.Y + rect.Height - d, d, d, 90.0f, 90.0f);
        path.CloseFigure();
    }
    Pen pen((Color(color)), width);
    g.DrawPath(&pen, &path);
}

// The legend is sampled from the very same heat function the cells use, so the
// two can never drift apart (spec 4: 64 samples of the curve).
void DrawLegendGradient(Graphics& g, const RECT& r) {
    const RectF rect = ToRectF(r);
    const float radius = std::min(rect.Height * 0.5f, Pxf(kLegendBarH * 0.5f));
    GraphicsPath clip;
    const float d = radius * 2.0f;
    if (rect.Width > d && rect.Height > d) {
        clip.AddArc(rect.X, rect.Y, d, d, 180.0f, 90.0f);
        clip.AddArc(rect.X + rect.Width - d, rect.Y, d, d, 270.0f, 90.0f);
        clip.AddArc(rect.X + rect.Width - d, rect.Y + rect.Height - d, d, d, 0.0f, 90.0f);
        clip.AddArc(rect.X, rect.Y + rect.Height - d, d, d, 90.0f, 90.0f);
        clip.CloseFigure();
    } else {
        clip.AddRectangle(rect);
    }
    g.SetClip(&clip);
    const int strips = std::max(8, static_cast<int>(std::lround(rect.Width)));
    for (int k = 0; k < strips; ++k) {
        const float t = static_cast<float>(k) / static_cast<float>(strips - 1);
        SolidBrush brush((Color(HeatColor(t, 0.0f, 1.0f))));
        const float x0 = rect.X + rect.Width * static_cast<float>(k) / static_cast<float>(strips);
        const float x1 = rect.X + rect.Width * static_cast<float>(k + 1) / static_cast<float>(strips);
        g.FillRectangle(&brush, RectF(x0, rect.Y, (x1 - x0) + 1.0f, rect.Height));
    }
    g.ResetClip();
}

void DrawCardShape(Graphics& g, const RECT& r) {
    FillRoundRect(g, r, Pxf(kCardRadius), g_palette.cardBg);
    const float penW = std::max(1.0f, g_scale);
    RECT inner = {r.left + static_cast<LONG>(penW * 0.5f),
                  r.top + static_cast<LONG>(penW * 0.5f),
                  r.right - static_cast<LONG>(penW * 0.5f),
                  r.bottom - static_cast<LONG>(penW * 0.5f)};
    StrokeRoundRect(g, inner, Pxf(kCardRadius), g_palette.cardBorder, penW);
}

// ---------------------------------------------------------------------------
// Painting: main window.  GDI+ draws the shapes, GDI draws the text, and the
// whole frame is composed in a cached memory bitmap first, so WM_PAINT is one
// BitBlt of the dirty rectangle and nothing ever flashes (spec: no flicker).
// ---------------------------------------------------------------------------
BackBuffer g_buttonBack;

void FlushGraphics(Graphics& g) { g.Flush(FlushIntentionSync); }

void InvalidateMatrix() {
    if (g_app.matrix != nullptr) {
        InvalidateRect(g_app.matrix, nullptr, FALSE);
    }
}

void InvalidateStatus() {
    if (g_app.hwnd == nullptr || !g_app.haveLayout) {
        return;
    }
    RECT r = g_app.layout.statusRect;
    r.bottom = g_app.layout.progressRect.bottom;
    InvalidateRect(g_app.hwnd, &r, FALSE);
    // The "d/n" counter in the matrix-card header is painted by the main window
    // too, so it has to be repainted on the same per-pair tick; otherwise it
    // stays at 0/56 until the final repaint.
    RECT count = g_app.layout.matrixCount;
    InflateRect(&count, Pxi(2.0f), Pxi(2.0f));
    InvalidateRect(g_app.hwnd, &count, FALSE);
}

const wchar_t* ButtonLabel(UINT id) {
    switch (id) {
        case IDC_START:
            return kStartText;
        case IDC_STOP:
            return kStopText;
        case IDC_EXPORT:
            return kExportText;
        default:
            return L"";
    }
}

// Spec 3: the button captions are exactly 开始测试 / 停止 / 导出 CSV.  No access-key
// suffix is appended: the spec's text is authoritative and must match macOS
// pixel for pixel.  Alt+S / Alt+T / Alt+E still work through the message loop.
std::wstring ButtonLabelWithAccessKey(UINT id) {
    return std::wstring(ButtonLabel(id));
}

bool ButtonIsPrimary(UINT id) { return id == IDC_START; }

void DrawFluentButton(const DRAWITEMSTRUCT* item) {
    if (item == nullptr || item->hDC == nullptr) {
        return;
    }
    const UINT id = item->CtlID;
    const bool primary = ButtonIsPrimary(id);
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    const bool focused = (item->itemState & ODS_FOCUS) != 0;
    const bool hot = g_app.hoverButton == static_cast<int>(id);

    RECT rc = item->rcItem;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) {
        return;
    }
    g_buttonBack.Ensure(item->hDC, w, h);
    HDC mem = g_buttonBack.dc;
    if (mem == nullptr) {
        return;
    }

    ARGB fill = 0;
    ARGB label = 0;
    ARGB border = 0;
    bool drawBorder = false;
    if (primary) {
        if (disabled) {
            fill = MixColor(g_palette.accent, g_palette.cardBg, g_palette.dark ? 0.55f : 0.62f);
            label = g_palette.dark ? g_palette.disabledText : Rgb(0xFF, 0xFF, 0xFF);
        } else if (pressed) {
            fill = g_palette.accentPressed;
            label = g_palette.onAccent;
        } else if (hot) {
            fill = g_palette.accentHover;
            label = g_palette.onAccent;
        } else {
            fill = g_palette.accent;
            label = g_palette.onAccent;
        }
    } else {
        if (disabled) {
            fill = g_palette.disabledBg;
            label = g_palette.disabledText;
            border = g_palette.cardBorder;
            drawBorder = true;
        } else if (pressed) {
            fill = g_palette.buttonPressed;
            label = g_palette.textPrimary;
            border = g_palette.buttonBorder;
            drawBorder = true;
        } else if (hot) {
            fill = g_palette.buttonHover;
            label = g_palette.textPrimary;
            border = g_palette.buttonBorder;
            drawBorder = true;
        } else {
            fill = g_palette.buttonBg;
            label = g_palette.textPrimary;
            border = g_palette.buttonBorder;
            drawBorder = true;
        }
    }

    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        SolidBrush backdrop((Color(g_palette.windowBg)));
        g.FillRectangle(&backdrop, 0, 0, w, h);
        RECT body = {0, 0, w, h};
        RECT shape = {0, 0, w - 1, h - 1};
        if (g_palette.highContrast) {
            // High contrast: system button colours, square corners, no fills
            // that the theme cannot override.
            HBRUSH brush = CreateSolidBrush(ToColorRef(fill));
            FillRect(mem, &body, brush);
            DeleteObject(brush);
            DrawEdge(mem, &body, pressed ? EDGE_SUNKEN : EDGE_RAISED,
                     BF_RECT | BF_MIDDLE | BF_ADJUST);
            if (drawBorder) {
                FrameRect(mem, &body, GetSysColorBrush(COLOR_WINDOWFRAME));
            }
        } else {
            FillRoundRect(g, shape, Pxf(kButtonRadius), fill);
            if (drawBorder) {
                StrokeRoundRect(g, shape, Pxf(kButtonRadius), border,
                                std::max(1.0f, g_scale));
            }
        }
        if (focused) {
            RECT ring = {2, 2, w - 2, h - 2};
            ring.left = static_cast<LONG>(std::lround(Pxf(2.0f)));
            ring.top = static_cast<LONG>(std::lround(Pxf(2.0f)));
            ring.right = static_cast<LONG>(std::lround(static_cast<float>(w) - Pxf(2.0f)));
            ring.bottom = static_cast<LONG>(std::lround(static_cast<float>(h) - Pxf(2.0f)));
            StrokeRoundRect(g, ring, Pxf(kButtonRadius - 1.0f),
                            primary && !disabled ? g_palette.onAccent
                                                 : g_palette.textPrimary,
                            std::max(1.0f, g_scale));
        }
        FlushGraphics(g);
    }

    HFONT font = primary ? g_fonts.button : g_fonts.buttonSecondary;
    const std::wstring text = ButtonLabelWithAccessKey(id);
    DrawTextCenter(mem, text, font, label, 0.0f, 0.0f, static_cast<float>(w),
                   static_cast<float>(h));

    BitBlt(item->hDC, 0, 0, w, h, mem, 0, 0, SRCCOPY);
}

void PaintMain(HDC hdc, const RECT& dirty) {
    const int w = g_app.layout.clientW;
    const int h = g_app.layout.clientH;
    if (w <= 0 || h <= 0) {
        return;
    }
    g_mainBack.Ensure(hdc, w, h);
    HDC mem = g_mainBack.dc;
    if (mem == nullptr) {
        return;
    }
    const UiLayout& L = g_app.layout;

    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);

        SolidBrush page((Color(g_palette.windowBg)));
        g.FillRectangle(&page, 0, 0, w, h);

        // -- 设备信息卡 -----------------------------------------------------
        DrawCardShape(g, L.deviceCard);

        // -- 矩阵卡 ---------------------------------------------------------
        DrawCardShape(g, L.matrixCard);

        // Legend gradient bar (spec 4: sampled from the heat curve).
        {
            const float lowW = TextWidth(g_fonts.legend, kLegendLowText);
            const float barX = static_cast<float>(L.legendRect.left) + lowW + Pxf(8.0f);
            const float barW = Pxf(kLegendBarW);
            const float barH = Pxf(kLegendBarH);
            const float midY = (static_cast<float>(L.legendRect.top) +
                                static_cast<float>(L.legendRect.bottom)) * 0.5f;
            RECT bar = {static_cast<LONG>(std::lround(barX)),
                        static_cast<LONG>(std::lround(midY - barH * 0.5f)),
                        static_cast<LONG>(std::lround(barX + barW)),
                        static_cast<LONG>(std::lround(midY + barH * 0.5f))};
            DrawLegendGradient(g, bar);
        }
        FlushGraphics(g);
    }

    // -- 标题区 -------------------------------------------------------------
    DrawTextLeft(mem, kTitleText, g_fonts.title, g_palette.textPrimary,
                 static_cast<float>(L.titleRect.left),
                 static_cast<float>(L.titleRect.top),
                 static_cast<float>(L.titleRect.right - L.titleRect.left),
                 static_cast<float>(L.titleRect.bottom - L.titleRect.top), true);
    DrawTextLeft(mem, kSubtitleText, g_fonts.subtitle, g_palette.textSecondary,
                 static_cast<float>(L.subtitleRect.left),
                 static_cast<float>(L.subtitleRect.top),
                 static_cast<float>(L.subtitleRect.right - L.subtitleRect.left),
                 static_cast<float>(L.subtitleRect.bottom - L.subtitleRect.top), true);
    DrawTextRight(mem, kVersionText, g_fonts.smallText, g_palette.textTertiary,
                  static_cast<float>(L.versionRect.left),
                  static_cast<float>(L.versionRect.top),
                  static_cast<float>(L.versionRect.right - L.versionRect.left),
                  static_cast<float>(L.versionRect.bottom - L.versionRect.top), true);

    // -- 设备信息 -----------------------------------------------------------
    DrawTextLeft(mem, kDeviceTitleText, g_fonts.cardTitle, g_palette.textPrimary,
                 static_cast<float>(L.deviceTitle.left),
                 static_cast<float>(L.deviceTitle.top),
                 static_cast<float>(L.deviceTitle.right - L.deviceTitle.left),
                 static_cast<float>(L.deviceTitle.bottom - L.deviceTitle.top), true);
    for (const UiLayout::InfoCell& cell : L.infoCells) {
        DrawTextLeft(mem, cell.text, g_fonts.info, g_palette.textSecondary,
                     static_cast<float>(cell.rect.left),
                     static_cast<float>(cell.rect.top),
                     static_cast<float>(cell.rect.right - cell.rect.left),
                     static_cast<float>(cell.rect.bottom - cell.rect.top), true);
    }
    // -- 状态行 -------------------------------------------------------------
    {
        const std::wstring status = BuildStatusText();
        const ARGB color = g_app.phase == Phase::Failed ? g_palette.error
                                                        : g_palette.textPrimary;
        DrawTextLeft(mem, status, g_fonts.status, color,
                     static_cast<float>(L.statusRect.left),
                     static_cast<float>(L.statusRect.top),
                     static_cast<float>(L.statusRect.right - L.statusRect.left),
                     static_cast<float>(L.statusRect.bottom - L.statusRect.top), true);
    }

    // -- 进度条（自绘，3 pt，与状态文字同左边缘同宽） ------------------------
    {
        const RECT& pr = L.progressRect;
        const float barH = Pxf(kProgressH);
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        RECT track = pr;
        track.bottom = track.top + static_cast<LONG>(std::lround(barH));
        FillRoundRect(g, track, barH * 0.5f, g_palette.trackBg);
        const float fraction = ClampF(ProgressFraction(), 0.0f, 1.0f);
        const float fullW = static_cast<float>(track.right - track.left);
        const float fillW = fullW * fraction;
        if (fillW > 0.5f) {
            RECT fill = track;
            fill.right = fill.left + static_cast<LONG>(std::lround(fillW));
            FillRoundRect(g, fill, barH * 0.5f, g_palette.accent);
        }
        FlushGraphics(g);
    }

    // -- 矩阵卡文字 ---------------------------------------------------------
    DrawTextLeft(mem, kMatrixTitleText, g_fonts.sectionTitle, g_palette.textPrimary,
                 static_cast<float>(L.matrixTitle.left),
                 static_cast<float>(L.matrixTitle.top),
                 static_cast<float>(L.matrixTitle.right - L.matrixTitle.left),
                 static_cast<float>(L.matrixTitle.bottom - L.matrixTitle.top), true);
    DrawTextLeft(mem, kMatrixUnitText, g_fonts.smallText, g_palette.textTertiary,
                 static_cast<float>(L.matrixUnit.left),
                 static_cast<float>(L.matrixUnit.top),
                 static_cast<float>(L.matrixUnit.right - L.matrixUnit.left),
                 static_cast<float>(L.matrixUnit.bottom - L.matrixUnit.top), true);
    {
        const int done = (g_app.phase == Phase::Idle) ? 0 : g_app.pairsDone;
        const int total = std::max(g_app.pairsTotal,
                                   g_app.threads * (g_app.threads - 1));
        const std::wstring count = Fmt(L"%d/%d", done, total);
        DrawTextRight(mem, count, g_fonts.stats, g_palette.textTertiary,
                      static_cast<float>(L.matrixCount.left),
                      static_cast<float>(L.matrixCount.top),
                      static_cast<float>(L.matrixCount.right - L.matrixCount.left),
                      static_cast<float>(L.matrixCount.bottom - L.matrixCount.top), true);
    }

    // -- 图例 + 统计 --------------------------------------------------------
    {
        const float lowW = TextWidth(g_fonts.legend, kLegendLowText);
        const float rowH = static_cast<float>(L.legendRect.bottom - L.legendRect.top);
        float x = static_cast<float>(L.legendRect.left);
        DrawTextLeft(mem, kLegendLowText, g_fonts.legend, g_palette.textSecondary,
                     x, static_cast<float>(L.legendRect.top), lowW, rowH, false);
        x += lowW + Pxf(8.0f) + Pxf(kLegendBarW) + Pxf(8.0f);
        DrawTextLeft(mem, kLegendHighText, g_fonts.legend, g_palette.textSecondary,
                     x, static_cast<float>(L.legendRect.top),
                     TextWidth(g_fonts.legend, kLegendHighText) + Pxf(4.0f), rowH,
                     false);
        DrawTextRight(mem, BuildStatsText(), g_fonts.stats, g_palette.textSecondary,
                      static_cast<float>(L.statsRect.left),
                      static_cast<float>(L.statsRect.top),
                      static_cast<float>(L.statsRect.right - L.statsRect.left),
                      static_cast<float>(L.statsRect.bottom - L.statsRect.top), true);
    }

    // -- 脚注 ---------------------------------------------------------------
    if (!g_footnoteText.empty()) {
        RECT fr = L.footnoteRect;
        DrawTextGdi(mem, g_footnoteText, g_fonts.footer, g_palette.textTertiary, fr,
                    DT_LEFT | DT_TOP | DT_NOPREFIX | DT_WORDBREAK);
    }

    // -- 底栏 ---------------------------------------------------------------
    DrawTextLeft(mem, g_app.footerText, g_fonts.footer, g_palette.textTertiary,
                 static_cast<float>(L.footerRect.left),
                 static_cast<float>(L.footerRect.top),
                 static_cast<float>(L.footerRect.right - L.footerRect.left),
                 static_cast<float>(L.footerRect.bottom - L.footerRect.top), true);

    BitBlt(hdc, dirty.left, dirty.top, dirty.right - dirty.left,
           dirty.bottom - dirty.top, mem, dirty.left, dirty.top, SRCCOPY);
}

// ---------------------------------------------------------------------------
// Painting: the matrix child window.  It owns its scroll offsets and its own
// back buffer, so a wheel notch repaints exactly this child and nothing else
// (that is the whole flicker fix: the page around it is never invalidated).
// ---------------------------------------------------------------------------
struct GridMetrics {
    int n = 8;
    float cell = 26.0f;
    float gap = 3.0f;
    float labelW = 30.0f;
    float headerH = 18.0f;
    float gridW = 0.0f;
    float gridH = 0.0f;
    float maxScrollY = 0.0f;
    float maxScrollX = 0.0f;
    float sbW = 0.0f;
    float sbH = 0.0f;
    float offsetX = 0.0f;   // grid centring inside the viewport
    float offsetY = 0.0f;
    bool scrollY = false;
    bool scrollX = false;
    HFONT labelFont = nullptr;
    bool showStar = false;
    bool showCellText = false;
    HFONT cellFont = nullptr;
};

GridMetrics GridMetricsFromLayout(const UiLayout& L) {
    GridMetrics g;
    g.n = std::max(1, L.threads);
    g.cell = L.cell;
    g.gap = L.gap;
    g.labelW = L.labelW;
    g.headerH = L.headerH;
    g.gridW = L.gridW;
    g.gridH = L.gridH;
    g.maxScrollY = L.maxScrollY;
    g.maxScrollX = L.maxScrollX;
    g.sbW = L.sbW;
    g.sbH = L.sbH;
    g.scrollY = L.scrollY;
    g.scrollX = L.scrollX;
    g.offsetX = L.gridOffsetX;
    g.offsetY = L.gridOffsetY;
    g.labelFont = (L.labelFont != nullptr) ? L.labelFont : g_fonts.label;
    g.showStar = L.showStar;
    g.showCellText = L.showCellText;
    g.cellFont = (L.cellFont != nullptr) ? L.cellFont : g_fonts.cellLarge;
    return g;
}

// Scrollbar strips live inside the child, at its right and bottom edges.
RECT VBarRect(const GridMetrics& g, int childW, int childH) {
    (void)childH;
    RECT r = {};
    if (!g.scrollY) {
        return r;
    }
    r.right = childW;
    r.left = childW - static_cast<LONG>(std::lround(g.sbW));
    r.top = static_cast<LONG>(std::lround(g.headerH));
    r.bottom = childH - static_cast<LONG>(std::lround(g.sbH));
    return r;
}

RECT HBarRect(const GridMetrics& g, int childW, int childH) {
    (void)childW;
    RECT r = {};
    if (!g.scrollX) {
        return r;
    }
    r.left = static_cast<LONG>(std::lround(g.labelW));
    r.right = childW - static_cast<LONG>(std::lround(g.sbW));
    r.top = childH - static_cast<LONG>(std::lround(g.sbH));
    r.bottom = childH;
    return r;
}

RECT VThumbRect(const GridMetrics& g, int childW, int childH, float scrollY,
                float maxScrollY) {
    RECT bar = VBarRect(g, childW, childH);
    if (bar.right <= bar.left || bar.bottom <= bar.top) {
        return RECT{0, 0, 0, 0};
    }
    const float barH = static_cast<float>(bar.bottom - bar.top);
    const float total = barH + maxScrollY;
    float thumbH = barH;
    if (total > 0.0f) {
        thumbH = barH * (barH / total);
    }
    thumbH = std::max(thumbH, Pxf(24.0f));
    thumbH = std::min(thumbH, barH);
    const float travel = std::max(0.0f, barH - thumbH);
    const float t = (maxScrollY > 0.0f) ? ClampF(scrollY / maxScrollY, 0.0f, 1.0f) : 0.0f;
    const float top = static_cast<float>(bar.top) + travel * t;
    return RECT{bar.left, static_cast<LONG>(std::lround(top)), bar.right,
                static_cast<LONG>(std::lround(top + thumbH))};
}

RECT HThumbRect(const GridMetrics& g, int childW, int childH, float scrollX,
                float maxScrollX) {
    RECT bar = HBarRect(g, childW, childH);
    if (bar.right <= bar.left || bar.bottom <= bar.top) {
        return RECT{0, 0, 0, 0};
    }
    const float barW = static_cast<float>(bar.right - bar.left);
    const float total = barW + maxScrollX;
    float thumbW = barW;
    if (total > 0.0f) {
        thumbW = barW * (barW / total);
    }
    thumbW = std::max(thumbW, Pxf(24.0f));
    thumbW = std::min(thumbW, barW);
    const float travel = std::max(0.0f, barW - thumbW);
    const float t = (maxScrollX > 0.0f) ? ClampF(scrollX / maxScrollX, 0.0f, 1.0f) : 0.0f;
    const float left = static_cast<float>(bar.left) + travel * t;
    return RECT{static_cast<LONG>(std::lround(left)), bar.top,
                static_cast<LONG>(std::lround(left + thumbW)), bar.bottom};
}

void PaintMatrix(HDC hdc, const RECT& dirty) {
    HWND hwnd = g_app.matrix;
    if (hwnd == nullptr) {
        return;
    }
    RECT client = {};
    GetClientRect(hwnd, &client);
    const int w = client.right;
    const int h = client.bottom;
    if (w <= 0 || h <= 0) {
        return;
    }
    g_matrixBack.Ensure(hdc, w, h);
    HDC mem = g_matrixBack.dc;
    if (mem == nullptr) {
        return;
    }

    const UiLayout& L = g_app.layout;
    const GridMetrics gm = GridMetricsFromLayout(L);
    const int n = gm.n;
    const float cell = gm.cell;
    const float step = cell + gm.gap;
    const float scrollY = ClampF(g_app.scrollY, 0.0f, gm.maxScrollY);
    const float scrollX = ClampF(g_app.scrollX, 0.0f, gm.maxScrollX);
    const float gridX = gm.labelW + gm.offsetX - scrollX;
    const float gridY = gm.headerH + gm.offsetY - scrollY;

    {
        // Background first: one solid fill, no themed brush, no erase.
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        SolidBrush bg((Color(g_palette.cardBg)));
        g.FillRectangle(&bg, 0, 0, w, h);
        FlushGraphics(g);
    }

    const Stats stats = ComputeStats();
    const int clipL = static_cast<int>(std::lround(gm.labelW));
    const int clipT = static_cast<int>(std::lround(gm.headerH));
    const int clipR = w - static_cast<int>(std::lround(gm.sbW));
    const int clipB = h - static_cast<int>(std::lround(gm.sbH));
    const float radius = Pxf(kCellRadius);

    // ---- 色块区（严格裁剪到网格视口，永远不会画到标题行/行标签上） --------
    if (clipR > clipL && clipB > clipT) {
        SaveDC(mem);
        IntersectClipRect(mem, clipL, clipT, clipR, clipB);

        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);

        for (int i = 0; i < n; ++i) {
            const float y = gridY + static_cast<float>(i) * step;
            if (y + cell < static_cast<float>(clipT) - 1.0f ||
                y > static_cast<float>(clipB) + 1.0f) {
                continue;
            }
            for (int j = 0; j < n; ++j) {
                const float x = gridX + static_cast<float>(j) * step;
                if (x + cell < static_cast<float>(clipL) - 1.0f ||
                    x > static_cast<float>(clipR) + 1.0f) {
                    continue;
                }
                const RECT rc = {static_cast<LONG>(std::lround(x)),
                                 static_cast<LONG>(std::lround(y)),
                                 static_cast<LONG>(std::lround(x + cell)),
                                 static_cast<LONG>(std::lround(y + cell))};
                const float value =
                    g_app.cells.size() == static_cast<size_t>(n) * static_cast<size_t>(n)
                        ? g_app.cells[static_cast<size_t>(i) * n + j]
                        : std::numeric_limits<float>::quiet_NaN();
                const bool measured = (i != j) && ValidCell(value);
                ARGB fill = g_palette.cellIdleBg;
                if (measured) {
                    fill = HeatColor(value, stats.valid ? stats.minimum : 0.0f,
                                     stats.valid ? stats.maximum : 1.0f);
                }
                FillRoundRect(g, rc, radius, fill);
            }
        }
        FlushGraphics(g);
        RestoreDC(mem, -1);
    }

    // ---- 滚动条（仅在网格真的放不下时出现，安全网） ----------------------
    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        if (gm.scrollY) {
            const RECT thumb = VThumbRect(gm, w, h, scrollY, gm.maxScrollY);
            const RECT track = {static_cast<LONG>(std::lround(static_cast<float>(w) - gm.sbW)),
                                static_cast<LONG>(std::lround(gm.headerH)), w,
                                static_cast<LONG>(std::lround(static_cast<float>(h) - gm.sbH))};
            FillRoundRect(g, track, gm.sbW * 0.5f, WithAlpha(g_palette.scrollThumb, 48));
            RECT t2 = thumb;
            t2.right = static_cast<LONG>(std::lround(static_cast<float>(thumb.left) + gm.sbW));
            FillRoundRect(g, t2, gm.sbW * 0.5f, g_palette.scrollThumb);
        }
        if (gm.scrollX) {
            const RECT thumb = HThumbRect(gm, w, h, scrollX, gm.maxScrollX);
            const RECT track = {static_cast<LONG>(std::lround(gm.labelW)),
                                static_cast<LONG>(std::lround(static_cast<float>(h) - gm.sbH)),
                                static_cast<LONG>(std::lround(static_cast<float>(w) - gm.sbW)), h};
            FillRoundRect(g, track, gm.sbH * 0.5f, WithAlpha(g_palette.scrollThumb, 48));
            RECT t2 = thumb;
            t2.bottom = static_cast<LONG>(std::lround(static_cast<float>(thumb.top) + gm.sbH));
            FillRoundRect(g, t2, gm.sbH * 0.5f, g_palette.scrollThumb);
        }
        FlushGraphics(g);
    }

    BitBlt(hdc, dirty.left, dirty.top, dirty.right - dirty.left,
           dirty.bottom - dirty.top, mem, dirty.left, dirty.top, SRCCOPY);

    // 文字一律画在窗口 DC 上、且在所有 GDI+ 调用之后：GDI+ 的 FlushGraphics 会
    // 把同一块内存 DC 里的 GDI 文字冲掉，所以标签/数字只能在 BitBlt 之后落到 hdc。
    // ---- 单元格数字（只有格子够大时才有，n <= 8） ------------------------
    if (gm.showCellText && clipR > clipL && clipB > clipT) {
        SaveDC(hdc);
        IntersectClipRect(hdc, clipL, clipT, clipR, clipB);
        for (int i = 0; i < n; ++i) {
            const float y = gridY + static_cast<float>(i) * step;
            if (y + cell < static_cast<float>(clipT) - 1.0f ||
                y > static_cast<float>(clipB) + 1.0f) {
                continue;
            }
            for (int j = 0; j < n; ++j) {
                if (i == j) {
                    continue;
                }
                const float x = gridX + static_cast<float>(j) * step;
                if (x + cell < static_cast<float>(clipL) - 1.0f ||
                    x > static_cast<float>(clipR) + 1.0f) {
                    continue;
                }
                const float value =
                    g_app.cells.size() == static_cast<size_t>(n) * static_cast<size_t>(n)
                        ? g_app.cells[static_cast<size_t>(i) * n + j]
                        : std::numeric_limits<float>::quiet_NaN();
                if (!ValidCell(value)) {
                    continue;
                }
                const std::wstring text = Fmt(L"%.1f", static_cast<double>(value));
                DrawTextCenter(hdc, text, gm.cellFont, g_palette.onAccent, x, y, cell, cell);
            }
        }
        RestoreDC(hdc, -1);
    }

    // ---- 列标签（固定顶部，逐列对齐到单元格中轴） ------------------------
    {
        SaveDC(hdc);
        IntersectClipRect(hdc, clipL, 0, clipR, clipT);
        for (int j = 0; j < n; ++j) {
            const float x = gridX + static_cast<float>(j) * step;
            if (x + cell < static_cast<float>(clipL) - 1.0f ||
                x > static_cast<float>(clipR) + 1.0f) {
                continue;
            }
            const bool smt = gm.showStar && j < static_cast<int>(g_app.sibling.size()) &&
                             g_app.sibling[static_cast<size_t>(j)] >= 0;
            const std::wstring label = smt ? Fmt(L"T%d*", j) : Fmt(L"T%d", j);
            DrawTextCenter(hdc, label, gm.labelFont, g_palette.textSecondary, x, 0.0f,
                           cell, gm.headerH);
        }
        RestoreDC(hdc, -1);
    }

    // ---- 行标签（固定左侧） ----------------------------------------------
    {
        SaveDC(hdc);
        IntersectClipRect(hdc, 0, clipT, clipL, clipB);
        for (int i = 0; i < n; ++i) {
            const float y = gridY + static_cast<float>(i) * step;
            if (y + cell < static_cast<float>(clipT) - 1.0f ||
                y > static_cast<float>(clipB) + 1.0f) {
                continue;
            }
            const bool smt = gm.showStar && i < static_cast<int>(g_app.sibling.size()) &&
                             g_app.sibling[static_cast<size_t>(i)] >= 0;
            const std::wstring label = smt ? Fmt(L"T%d*", i) : Fmt(L"T%d", i);
            RECT rc = {0, static_cast<LONG>(std::lround(y)),
                       static_cast<LONG>(std::lround(gm.labelW - Pxf(4.0f))),
                       static_cast<LONG>(std::lround(y + cell))};
            DrawTextGdi(hdc, label, gm.labelFont, g_palette.textSecondary, rc,
                        DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
        }
        RestoreDC(hdc, -1);
    }
}

// ---------------------------------------------------------------------------
// Engine driving.  cl_run_matrix() runs on a std::thread and reports progress
// through PostMessage + a heap snapshot the UI thread frees.
// ---------------------------------------------------------------------------
// Spec 5: results go to the user's Documents folder.  Use the Shell Known
// Folder API (FOLDERID_Documents) so redirected / OneDrive Documents folders are
// honoured, and append exactly one folder name -- never a second "Documents".
std::wstring ResolveResultDir() {
    std::wstring dir;
    bool haveDocuments = false;

    PWSTR knownPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr,
                                       &knownPath)) &&
        knownPath != nullptr && knownPath[0] != L'\0') {
        dir = knownPath;
        haveDocuments = true;
    }
    if (knownPath != nullptr) {
        CoTaskMemFree(knownPath);
    }

    if (!haveDocuments) {
        wchar_t path[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr,
                                       SHGFP_TYPE_CURRENT, path)) &&
            path[0] != L'\0') {
            dir = path;   // CSIDL_PERSONAL already *is* Documents
            haveDocuments = true;
        }
    }

    if (!haveDocuments) {
        wchar_t path[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            return std::wstring();
        }
        dir = path;
        if (dir.empty()) {
            return dir;
        }
        if (dir.back() != L'\\') {
            dir += L'\\';
        }
        dir += L"Documents";
    }

    if (!dir.empty() && dir.back() != L'\\') {
        dir += L'\\';
    }
    dir += L"CoherencyLatency";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void BuildFooterText() {
    if (g_app.resultDir.empty()) {
        g_app.footerText = L"CoherencyLatency 1.2.0";
        return;
    }
    g_app.footerText = Fmt(L"CoherencyLatency 1.2.0 · 结果自动保存到 %s",
                           g_app.resultDir.c_str());
}

void RefreshDerived() {
    RefreshDerivedText();
    BuildFooterText();
}

std::wstring DefaultCsvPath() {
    char name[256] = {};
    if (cl_default_csv_name(name, static_cast<int>(sizeof(name))) != 0) {
        return std::wstring();
    }
    std::wstring path = g_app.resultDir;
    if (!path.empty() && path.back() != L'\\') {
        path += L'\\';
    }
    path += Widen(name);
    return path;
}

bool WriteCsvTo(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }
    ClResult result = {};
    result.n = g_app.threads;
    result.pairs_done = g_app.pairsDone;
    result.pairs_total = g_app.pairsTotal;
    result.aborted = g_app.phase == Phase::Stopped ? 1 : 0;
    result.elapsed_seconds = g_app.elapsed;
    std::vector<float> cells = g_app.cells;
    result.cells = cells.empty() ? nullptr : cells.data();
    char error[256] = {};
    const std::string narrow = NarrowAnsi(path);
    if (cl_write_csv(&result, narrow.c_str(), error, static_cast<int>(sizeof(error))) !=
        0) {
        OutputDebugStringW(Fmt(L"cl_write_csv failed: %s\n", Widen(error).c_str()).c_str());
        return false;
    }
    return true;
}

void AutoSaveCsv() {
    const std::wstring path = DefaultCsvPath();
    if (!path.empty()) {
        WriteCsvTo(path);
    }
}

void ProgressCallback(void* context, const ClProgress* progress) {
    HWND hwnd = static_cast<HWND>(context);
    if (hwnd == nullptr || progress == nullptr) {
        return;
    }
    auto* payload = new (std::nothrow) ProgressPayload();
    if (payload == nullptr) {
        return;
    }
    payload->done = progress->done;
    payload->total = progress->total;
    payload->i = progress->i;
    payload->j = progress->j;
    payload->affinityOk = progress->affinity_ok;
    payload->ns = progress->latency_ns;
    payload->elapsed = progress->elapsed_seconds;
    if (!PostMessageW(hwnd, WM_CL_PROGRESS, 0, reinterpret_cast<LPARAM>(payload))) {
        delete payload;
    }
}

// Copy the measured matrix into the UI, averaging (i,j) with (j,i).  When the
// run was stopped early, a pair measured in only one direction is mirrored.
void FinalizeRun(ClResult* result) {
    const int n = result->n;
    if (n <= 0 || result->cells == nullptr) {
        return;
    }
    g_app.cells.assign(static_cast<size_t>(n) * static_cast<size_t>(n),
                       std::numeric_limits<float>::quiet_NaN());
    if (!result->aborted) {
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                g_app.cells[static_cast<size_t>(i) * n + j] =
                    result->cells[static_cast<size_t>(i) * n + j];
            }
        }
        return;
    }
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            const float a = result->cells[static_cast<size_t>(i) * n + j];
            const float b = result->cells[static_cast<size_t>(j) * n + i];
            const bool va = ValidCell(a);
            const bool vb = ValidCell(b);
            float value = std::numeric_limits<float>::quiet_NaN();
            if (va && vb) {
                value = (a + b) * 0.5f;
            } else if (va) {
                value = a;
            } else if (vb) {
                value = b;
            }
            g_app.cells[static_cast<size_t>(i) * n + j] = value;
            g_app.cells[static_cast<size_t>(j) * n + i] = value;
        }
    }
}

void StartRun(HWND hwnd) {
    if (g_app.phase == Phase::Running) {
        return;
    }
    if (g_app.worker.joinable()) {
        g_app.worker.join();
    }
    if (g_app.abortFlag == nullptr) {
        g_app.abortFlag = cl_abort_create();
    }
    if (g_app.abortFlag == nullptr) {
        g_app.phase = Phase::Failed;
        g_app.errorText = L"无法创建中止标志";
        return;
    }
    // A fresh abort object may still carry the request from the previous run.
    if (cl_abort_requested(g_app.abortFlag)) {
        cl_abort_destroy(g_app.abortFlag);
        g_app.abortFlag = cl_abort_create();
        if (g_app.abortFlag == nullptr) {
            g_app.phase = Phase::Failed;
            g_app.errorText = L"无法创建中止标志";
            return;
        }
    }

    const int n = g_app.threads;
    g_app.cells.assign(static_cast<size_t>(n) * static_cast<size_t>(n),
                       std::numeric_limits<float>::quiet_NaN());
    g_app.pairsDone = 0;
    g_app.pairsTotal = n * (n - 1);
    g_app.elapsed = 0.0;
    g_app.lastI = -1;
    g_app.lastJ = -1;
    g_app.lastNs = 0.0f;
    g_app.errorText.clear();
    g_app.phase = Phase::Running;
    g_app.scrollY = 0.0f;
    g_app.scrollX = 0.0f;

    ClRunConfig config = {};
    config.threads = n;
    config.iterations = 1000000ull;
    config.runs = 3ull;
    config.owned = 0;
    config.use_affinity = 1;

    HWND target = hwnd;
    ClAbort* abortFlag = g_app.abortFlag;
    g_app.worker = std::thread([target, config, abortFlag]() {
        auto* done = new (std::nothrow) DonePayload();
        if (done == nullptr) {
            return;
        }
        done->threads = config.threads;
        done->rc = cl_run_matrix(&config, ProgressCallback, target, abortFlag,
                                 &done->result, done->error,
                                 static_cast<int>(sizeof(done->error)));
        if (!PostMessageW(target, WM_CL_DONE, 0, reinterpret_cast<LPARAM>(done))) {
            if (done->rc == 0) {
                cl_free_result(&done->result);
            }
            delete done;
        }
    });

    UpdateControls(hwnd);
    RebuildLayout(hwnd);
    InvalidateMatrix();
    InvalidateRect(hwnd, nullptr, FALSE);
}

void OnProgressMessage(const ProgressPayload* payload) {
    if (payload == nullptr) {
        return;
    }
    g_app.pairsDone = payload->done;
    g_app.pairsTotal = payload->total;
    g_app.lastI = payload->i;
    g_app.lastJ = payload->j;
    g_app.lastNs = payload->ns;
    g_app.elapsed = payload->elapsed;
    if (payload->i >= 0 && payload->j >= 0 &&
        g_app.cells.size() ==
            static_cast<size_t>(g_app.threads) * static_cast<size_t>(g_app.threads)) {
        g_app.cells[static_cast<size_t>(payload->i) * g_app.threads + payload->j] =
            payload->ns;
    }
    // Spec 5: the status line and the progress bar change once per finished
    // thread pair -- there is no timer anywhere in this program.
    InvalidateStatus();
    InvalidateRect(g_app.progress, nullptr, FALSE);
    InvalidateMatrix();
}

void OnDoneMessage(DonePayload* payload) {
    if (payload == nullptr) {
        return;
    }
    if (payload->rc == 0) {
        FinalizeRun(&payload->result);
        g_app.pairsDone = payload->result.pairs_done;
        g_app.pairsTotal = payload->result.pairs_total;
        g_app.elapsed = payload->result.elapsed_seconds;
        g_app.phase = payload->result.aborted ? Phase::Stopped : Phase::Finished;
        cl_free_result(&payload->result);
        if (g_app.phase == Phase::Finished || g_app.phase == Phase::Stopped) {
            if (HasData()) {
                AutoSaveCsv();
            }
        }
    } else {
        g_app.phase = Phase::Failed;
        g_app.errorText = Widen(payload->error);
        if (g_app.errorText.empty()) {
            g_app.errorText = L"未知错误";
        }
        if (payload->rc == 1) {
            cl_free_result(&payload->result);
        }
    }
    delete payload;
    if (g_app.worker.joinable()) {
        g_app.worker.join();
    }
    UpdateControls(g_app.hwnd);
    RebuildLayout(g_app.hwnd);
    InvalidateMatrix();
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

// Drain queued engine messages before tearing the window down, so no payload
// leaks when the run outlives the UI.
void DrainPendingMessages() {
    MSG msg = {};
    while (PeekMessageW(&msg, nullptr, WM_CL_PROGRESS, WM_CL_DONE, PM_REMOVE)) {
        if (msg.message == WM_CL_PROGRESS) {
            delete reinterpret_cast<ProgressPayload*>(msg.lParam);
        } else if (msg.message == WM_CL_DONE) {
            auto* done = reinterpret_cast<DonePayload*>(msg.lParam);
            if (done != nullptr) {
                if (done->rc == 0) {
                    cl_free_result(&done->result);
                }
                delete done;
            }
        }
    }
}

void StopAndJoin() {
    if (g_app.abortFlag != nullptr) {
        cl_abort_request(g_app.abortFlag);
    }
    if (g_app.worker.joinable()) {
        g_app.worker.join();
    }
    DrainPendingMessages();
}

// ---------------------------------------------------------------------------
// Controls, window procedures and the message loop.
// ---------------------------------------------------------------------------
void UpdateControls(HWND hwnd) {
    const bool running = g_app.phase == Phase::Running;
    const bool data = HasData();
    if (g_app.btnStart != nullptr) {
        EnableWindow(g_app.btnStart, !running);
    }
    if (g_app.btnStop != nullptr) {
        EnableWindow(g_app.btnStop, running);
    }
    if (g_app.btnExport != nullptr) {
        EnableWindow(g_app.btnExport, !running && data);
    }
    if (hwnd != nullptr) {
        InvalidateRect(g_app.btnStart, nullptr, FALSE);
        InvalidateRect(g_app.btnStop, nullptr, FALSE);
        InvalidateRect(g_app.btnExport, nullptr, FALSE);
    }
}

void RebuildLayout(HWND hwnd) {
    RECT client = {};
    GetClientRect(hwnd, &client);
    if (client.right <= 0 || client.bottom <= 0) {
        return;
    }
    ComputeLayout(client.right, client.bottom);
}

void PositionControls(HWND) {
    if (!g_app.haveLayout) {
        return;
    }
    const UiLayout& L = g_app.layout;
    struct Placement {
        HWND control;
        RECT rect;
    };
    const Placement placements[] = {
        {g_app.btnStart, L.btnStart},
        {g_app.btnStop, L.btnStop},
        {g_app.btnExport, L.btnExport},
        {g_app.progress, L.progressRect},
        {g_app.matrix, L.matrixClient},
    };
    HDWP defer = BeginDeferWindowPos(static_cast<int>(sizeof(placements) /
                                                      sizeof(placements[0])));
    for (const Placement& p : placements) {
        if (p.control == nullptr) {
            continue;
        }
        const int w = p.rect.right - p.rect.left;
        const int h = p.rect.bottom - p.rect.top;
        if (defer != nullptr) {
            defer = DeferWindowPos(defer, p.control, nullptr, p.rect.left, p.rect.top,
                                   w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            SetWindowPos(p.control, nullptr, p.rect.left, p.rect.top, w, h,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    if (defer != nullptr) {
        EndDeferWindowPos(defer);
    }
    // Native controls repaint themselves on resize; the matrix child and the
    // page chrome are ours.
    InvalidateMatrix();
}

void ApplyTheme(HWND hwnd) {
    ReleaseBrushes();
    BuildPalette();
    ApplyWindowTheme(hwnd);
    if (hwnd != nullptr) {
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME);
    }
}

void ApplyDpi(HWND hwnd, int dpi) {
    g_dpi = dpi > 0 ? dpi : 96;
    g_scale = static_cast<float>(g_dpi) / 96.0f;
    BuildFonts();
    if (g_app.btnStart != nullptr) {
        SendMessageW(g_app.btnStart, WM_SETFONT,
                     reinterpret_cast<WPARAM>(g_fonts.button), TRUE);
    }
    if (g_app.btnStop != nullptr) {
        SendMessageW(g_app.btnStop, WM_SETFONT,
                     reinterpret_cast<WPARAM>(g_fonts.buttonSecondary), TRUE);
    }
    if (g_app.btnExport != nullptr) {
        SendMessageW(g_app.btnExport, WM_SETFONT,
                     reinterpret_cast<WPARAM>(g_fonts.buttonSecondary), TRUE);
    }
    g_mainBack.Release();
    g_matrixBack.Release();
    g_buttonBack.Release();
    if (hwnd != nullptr) {
        RebuildLayout(hwnd);
        PositionControls(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

bool CanStart() { return g_app.phase != Phase::Running; }
bool CanStop() { return g_app.phase == Phase::Running; }
bool CanExport() { return g_app.phase != Phase::Running && HasData(); }

void ExportCsv(HWND hwnd) {
    if (!CanExport()) {
        return;
    }
    wchar_t path[MAX_PATH * 2] = {};
    const std::wstring suggested = DefaultCsvPath();
    if (!suggested.empty()) {
        wcsncpy_s(path, suggested.c_str(), _TRUNCATE);
    }
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"CSV 文件 (*.csv)\0*.csv\0所有文件 (*.*)\0*.*\0\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    ofn.lpstrTitle = L"导出 CSV";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = L"csv";
    if (!GetSaveFileNameW(&ofn)) {
        return;
    }
    if (!WriteCsvTo(path)) {
        MessageBoxW(hwnd, L"无法写入 CSV 文件。", L"导出 CSV", MB_OK | MB_ICONERROR);
    }
}

void ScrollBy(float dy, float dx) {
    if (!g_app.haveLayout) {
        return;
    }
    const GridMetrics gm = GridMetricsFromLayout(g_app.layout);
    const float newY = ClampF(g_app.scrollY + dy, 0.0f, gm.maxScrollY);
    const float newX = ClampF(g_app.scrollX + dx, 0.0f, gm.maxScrollX);
    if (newY == g_app.scrollY && newX == g_app.scrollX) {
        return;
    }
    g_app.scrollY = newY;
    g_app.scrollX = newX;
    // Only the matrix child is repainted -- never the page around it.
    InvalidateMatrix();
}

void OnMatrixWheel(int wheelDelta) {
    if (!g_app.haveLayout) {
        return;
    }
    const GridMetrics gm = GridMetricsFromLayout(g_app.layout);
    if (gm.maxScrollY <= 0.0f) {
        return;
    }
    const UINT lines = 3;
    ULONG perLine = 0;
    if (!SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &perLine, 0)) {
        perLine = 3;
    }
    const float rows = perLine == WHEEL_PAGESCROLL
                           ? static_cast<float>(gm.maxScrollY)
                           : static_cast<float>(lines);
    const float step = rows * (gm.cell + gm.gap);
    const float dy = (static_cast<float>(wheelDelta) / static_cast<float>(WHEEL_DELTA)) *
                     -step;
    ScrollBy(dy, 0.0f);
}

void ScrollToThumb(int y) {
    const GridMetrics gm = GridMetricsFromLayout(g_app.layout);
    if (gm.maxScrollY <= 0.0f) {
        return;
    }
    RECT track = VBarRect(gm, g_app.layout.matrixClient.right -
                                  g_app.layout.matrixClient.left,
                          g_app.layout.matrixClient.bottom -
                              g_app.layout.matrixClient.top);
    const float barH = static_cast<float>(track.bottom - track.top);
    const float thumbH = static_cast<float>(VThumbRect(gm,
                                                       g_app.layout.matrixClient.right -
                                                           g_app.layout.matrixClient.left,
                                                       g_app.layout.matrixClient.bottom -
                                                           g_app.layout.matrixClient.top,
                                                       g_app.scrollY, gm.maxScrollY)
                                          .bottom -
                                      VThumbRect(gm,
                                                 g_app.layout.matrixClient.right -
                                                     g_app.layout.matrixClient.left,
                                                 g_app.layout.matrixClient.bottom -
                                                     g_app.layout.matrixClient.top,
                                                 g_app.scrollY, gm.maxScrollY)
                                          .top);
    const float travel = std::max(1.0f, barH - thumbH);
    const float t = ClampF(static_cast<float>(y - track.top) / travel, 0.0f, 1.0f);
    g_app.scrollY = t * gm.maxScrollY;
    InvalidateMatrix();
}

void ScrollToThumbX(int x) {
    const GridMetrics gm = GridMetricsFromLayout(g_app.layout);
    if (gm.maxScrollX <= 0.0f) {
        return;
    }
    RECT track = HBarRect(gm, g_app.layout.matrixClient.right -
                                 g_app.layout.matrixClient.left,
                          g_app.layout.matrixClient.bottom -
                              g_app.layout.matrixClient.top);
    const float barW = static_cast<float>(track.right - track.left);
    const float travel = std::max(1.0f, barW - Pxf(24.0f));
    const float t = ClampF(static_cast<float>(x - track.left) / travel, 0.0f, 1.0f);
    g_app.scrollX = t * gm.maxScrollX;
    InvalidateMatrix();
}

LRESULT PathMatrixProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                       bool& handled);

// ---------------------------------------------------------------------------
// Matrix child window
// ---------------------------------------------------------------------------
LRESULT CALLBACK MatrixProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    bool handled = false;
    const LRESULT result = PathMatrixProc(hwnd, message, wParam, lParam, handled);
    if (handled) {
        return result;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT PathMatrixProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                       bool& handled) {
    handled = true;
    switch (message) {
        case WM_ERASEBKGND:
            // (1) Never let Windows erase us: the back buffer covers 100 %.
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps = {};
            HDC dc = BeginPaint(hwnd, &ps);
            if (dc != nullptr) {
                RECT dirty = ps.rcPaint;
                if (dirty.right <= dirty.left || dirty.bottom <= dirty.top) {
                    RECT client = {};
                    GetClientRect(hwnd, &client);
                    dirty = client;
                }
                PaintMatrix(dc, dirty);
                EndPaint(hwnd, &ps);
            }
            return 0;
        }

        case WM_MOUSEWHEEL: {
            // (7) Handled here, return 0, never forwarded, no UpdateWindow().
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            OnMatrixWheel(delta);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            const GridMetrics gm = GridMetricsFromLayout(g_app.layout);
            RECT client = {};
            GetClientRect(hwnd, &client);
            RECT thumb = VThumbRect(gm, client.right, client.bottom, g_app.scrollY,
                                    gm.maxScrollY);
            RECT track = VBarRect(gm, client.right, client.bottom);
            if (gm.scrollY && x >= track.left && x < track.right && y >= track.top &&
                y < track.bottom) {
                if (y >= thumb.top && y < thumb.bottom) {
                    g_app.draggingThumb = true;
                    SetCapture(hwnd);
                } else {
                    ScrollToThumb(y);
                }
                return 0;
            }
            RECT htrack = HBarRect(gm, client.right, client.bottom);
            if (gm.scrollX && y >= htrack.top && y < htrack.bottom &&
                x >= htrack.left && x < htrack.right) {
                ScrollToThumbX(x);
                return 0;
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (g_app.draggingThumb) {
                ScrollToThumb(GET_Y_LPARAM(lParam));
                return 0;
            }
            const GridMetrics gm = GridMetricsFromLayout(g_app.layout);
            RECT client = {};
            GetClientRect(hwnd, &client);
            RECT thumb = VThumbRect(gm, client.right, client.bottom, g_app.scrollY,
                                    gm.maxScrollY);
            RECT track = VBarRect(gm, client.right, client.bottom);
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            const bool overBar = gm.scrollY && x >= track.left && x < track.right &&
                                 y >= track.top && y < track.bottom;
            SetCursor(LoadCursorW(nullptr, overBar ? IDC_HAND : IDC_ARROW));
            (void)thumb;
            return 0;
        }

        case WM_LBUTTONUP:
            if (g_app.draggingThumb) {
                g_app.draggingThumb = false;
                ReleaseCapture();
                return 0;
            }
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            break;

        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;

        case WM_KEYDOWN: {
            const GridMetrics gm = GridMetricsFromLayout(g_app.layout);
            const float step = gm.cell + gm.gap;
            switch (wParam) {
                case VK_DOWN:
                    ScrollBy(step, 0.0f);
                    return 0;
                case VK_UP:
                    ScrollBy(-step, 0.0f);
                    return 0;
                case VK_NEXT:
                    ScrollBy(static_cast<float>(gm.maxScrollY), 0.0f);
                    return 0;
                case VK_PRIOR:
                    ScrollBy(-static_cast<float>(gm.maxScrollY), 0.0f);
                    return 0;
                case VK_LEFT:
                    ScrollBy(0.0f, -step);
                    return 0;
                case VK_RIGHT:
                    ScrollBy(0.0f, step);
                    return 0;
                case VK_HOME:
                    ScrollBy(-static_cast<float>(gm.maxScrollY),
                             -static_cast<float>(gm.maxScrollX));
                    return 0;
                case VK_END:
                    ScrollBy(static_cast<float>(gm.maxScrollY),
                             static_cast<float>(gm.maxScrollX));
                    return 0;
                default:
                    break;
            }
            break;
        }

        case WM_SIZE:
            g_matrixBack.Release();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_NCHITTEST:
            break;

        default:
            handled = false;
            break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Main window procedure
// ---------------------------------------------------------------------------
void CreateChildControls(HWND hwnd) {
    const DWORD childStyle = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS;
    g_app.btnStart = CreateWindowExW(
        0, L"BUTTON", L"", childStyle | WS_TABSTOP | WS_GROUP | BS_OWNERDRAW, 0, 0,
        10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_START)),
        g_instance, nullptr);
    g_app.btnStop = CreateWindowExW(
        0, L"BUTTON", L"", childStyle | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STOP)), g_instance, nullptr);
    g_app.btnExport = CreateWindowExW(
        0, L"BUTTON", L"", childStyle | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXPORT)), g_instance, nullptr);
    // The bar is painted by PaintMain (GDI+), because the native control always
    // draws its own themed colours and ignores PBM_SETBARCOLOR under visual
    // styles.  The control object is kept for accessibility/tab order but stays
    // hidden, so it never paints over the custom bar.
    g_app.progress = CreateWindowExW(
        0, PROGRESS_CLASSW, nullptr, (childStyle & ~WS_VISIBLE) | PBS_SMOOTH, 0, 0,
        10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROGRESS)),
        g_instance, nullptr);
    // Classic (un-themed) progress bar: with visual styles on, PBM_SETBKCOLOR /
    // PBM_SETBARCOLOR are ignored and the bar paints a bright white slab that
    // ignores the spec colours.
    SetWindowTheme(g_app.progress, L"", L"");
    SendMessageW(g_app.progress, PBM_SETRANGE32, 0, 1000);
    SendMessageW(g_app.progress, PBM_SETPOS, 0, 0);

    g_app.matrix = CreateWindowExW(
        0, kMatrixClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP, 0, 0,
        10, 10, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_MATRIX)), g_instance, nullptr);

}

void SetProgressFromState() {
    if (g_app.progress == nullptr) {
        return;
    }
    const int position = static_cast<int>(std::lround(ProgressFraction() * 1000.0f));
    SendMessageW(g_app.progress, PBM_SETPOS, static_cast<WPARAM>(position), 0);
}

void ApplyProgressTheme() {
    if (g_app.progress == nullptr) {
        return;
    }
    if (g_palette.highContrast) {
        SendMessageW(g_app.progress, PBM_SETBKCOLOR, 0, CLR_DEFAULT);
        SendMessageW(g_app.progress, PBM_SETBARCOLOR, 0, CLR_DEFAULT);
        return;
    }
    SendMessageW(g_app.progress, PBM_SETBKCOLOR, 0, ToColorRef(g_palette.trackBg));
    SendMessageW(g_app.progress, PBM_SETBARCOLOR, 0, ToColorRef(g_palette.accent));
}

void StartRunFromUi() {
    if (!CanStart()) {
        return;
    }
    StartRun(g_app.hwnd);
    SetProgressFromState();
}

void StopFromUi() {
    if (!CanStop() || g_app.abortFlag == nullptr) {
        return;
    }
    cl_abort_request(g_app.abortFlag);
    // The current pair still finishes; WM_CL_DONE turns the phase into 已停止.
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            g_app.hwnd = hwnd;
            g_app.dpi = static_cast<UINT>(WindowDpi(hwnd));
            CreateChildControls(hwnd);
            ApplyDpi(hwnd, static_cast<int>(g_app.dpi));
            // The window was created with the DPI guessed before the HWND
            // existed (SystemDpi, often 96).  If the real monitor DPI differs,
            // the client area is still the old pixel size while every number
            // in the layout is in points, so the content would overflow the
            // window on the right and the bottom.  Resize exactly once to the
            // default content size in points, clamped to the work area.
            RECT clientNow = {};
            GetClientRect(hwnd, &clientNow);
            const int wantW = Pxi(kDefaultW);
            const int wantH = Pxi(kDefaultH);
            if (clientNow.right != wantW || clientNow.bottom != wantH) {
                RECT work = {};
                SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
                const int availW = static_cast<int>(work.right - work.left);
                const int availH = static_cast<int>(work.bottom - work.top);
                RECT frame = {0, 0, std::min(wantW, availW),
                              std::min(wantH, availH)};
                AdjustWindowRectForDpi(&frame, kMainStyle, FALSE,
                                       static_cast<int>(g_app.dpi));
                RECT wr = {};
                GetWindowRect(hwnd, &wr);
                SetWindowPos(hwnd, nullptr, wr.left, wr.top,
                             frame.right - frame.left, frame.bottom - frame.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            RebuildLayout(hwnd);
            PositionControls(hwnd);
            UpdateControls(hwnd);
            ApplyProgressTheme();
            SetProgressFromState();
            return 0;
        }

        case WM_ERASEBKGND:
            // (1) Fully covered by the back buffer; never erase to the class brush.
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps = {};
            HDC dc = BeginPaint(hwnd, &ps);
            if (dc != nullptr) {
                RECT dirty = ps.rcPaint;
                if (dirty.right <= dirty.left || dirty.bottom <= dirty.top) {
                    RECT client = {};
                    GetClientRect(hwnd, &client);
                    dirty = client;
                }
                PaintMain(dc, dirty);
                EndPaint(hwnd, &ps);
            }
            return 0;
        }

        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                RebuildLayout(hwnd);
                PositionControls(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            if (info != nullptr) {
                RECT r = {0, 0, Pxi(kMinW), Pxi(kMinH)};
                AdjustWindowRectForDpi(&r, kMainStyle, FALSE, g_dpi);
                info->ptMinTrackSize.x = r.right - r.left;
                info->ptMinTrackSize.y = r.bottom - r.top;
            }
            return 0;
        }

        case WM_DPICHANGED: {
            const int dpi = HIWORD(wParam);
            auto* suggested = reinterpret_cast<RECT*>(lParam);
            // Guard against a re-entrant loop: SetWindowPos below can deliver
            // another WM_DPICHANGED with the same DPI, and applying the
            // suggested rect again would grow the window by the scale factor
            // each time (observed: 1.5x, then 2.25x).
            if (dpi == static_cast<int>(g_app.dpi) && g_app.haveLayout) {
                return 0;
            }
            g_app.dpi = static_cast<UINT>(dpi);
            ApplyDpi(hwnd, dpi);
            if (suggested != nullptr) {
                // The suggested rect can exceed the work area when a DPI change
                // arrives while the window is already sized for that DPI, which
                // would push the window off-screen.  Clamp it to the monitor.
                RECT target = *suggested;
                RECT work = {};
                SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
                const int w = std::min(target.right - target.left,
                                       work.right - work.left);
                const int h = std::min(target.bottom - target.top,
                                       work.bottom - work.top);
                if (target.left < work.left) {
                    target.left = work.left;
                }
                if (target.top < work.top) {
                    target.top = work.top;
                }
                if (target.left + w > work.right) {
                    target.left = std::max(work.left, work.right - w);
                }
                if (target.top + h > work.bottom) {
                    target.top = std::max(work.top, work.bottom - h);
                }
                SetWindowPos(hwnd, nullptr, target.left, target.top, w, h,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            RebuildLayout(hwnd);
            PositionControls(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED: {
            const bool colorSet =
                message == WM_THEMECHANGED ||
                (wParam == 0 && lParam != 0 &&
                 wcscmp(reinterpret_cast<const wchar_t*>(lParam),
                        L"ImmersiveColorSet") == 0);
            if (colorSet) {
                ApplyTheme(hwnd);
                ApplyDpi(hwnd, static_cast<int>(g_app.dpi));
                ApplyProgressTheme();
                InvalidateRect(hwnd, nullptr, FALSE);
                InvalidateMatrix();
            }
            return 0;
        }

        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);
            if (id == IDC_START && (code == BN_CLICKED || code == BN_DOUBLECLICKED)) {
                StartRunFromUi();
                return 0;
            }
            if (id == IDC_STOP && (code == BN_CLICKED || code == BN_DOUBLECLICKED)) {
                StopFromUi();
                return 0;
            }
            if (id == IDC_EXPORT && (code == BN_CLICKED || code == BN_DOUBLECLICKED)) {
                ExportCsv(hwnd);
                return 0;
            }
            if (id == IDOK) {
                StartRunFromUi();
                return 0;
            }
            if (id == IDCANCEL) {
                StopFromUi();
                return 0;
            }
            break;
        }

        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (item != nullptr) {
                if (item->CtlID == IDC_START || item->CtlID == IDC_STOP ||
                    item->CtlID == IDC_EXPORT) {
                    DrawFluentButton(item);
                    return TRUE;
                }
            }
            break;
        }

        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, ToColorRef(g_palette.textPrimary));
            SetBkColor(dc, ToColorRef(g_palette.listBg));
            if (g_listBrush == nullptr) {
                g_listBrush = CreateSolidBrush(ToColorRef(g_palette.listBg));
            }
            return reinterpret_cast<LRESULT>(g_listBrush);
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, ToColorRef(g_palette.textPrimary));
            SetBkColor(dc, ToColorRef(g_palette.listBg));
            if (g_editBrush == nullptr) {
                g_editBrush = CreateSolidBrush(ToColorRef(g_palette.listBg));
            }
            return reinterpret_cast<LRESULT>(g_editBrush);
        }

        case WM_MOUSEMOVE: {
            // Track hover for the owner-drawn buttons (Fluent hover state).
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            int hover = 0;
            if (PtInRect(&g_app.layout.btnStart, pt)) {
                hover = IDC_START;
            } else if (PtInRect(&g_app.layout.btnStop, pt)) {
                hover = IDC_STOP;
            } else if (PtInRect(&g_app.layout.btnExport, pt)) {
                hover = IDC_EXPORT;
            }
            if (hover != g_app.hoverButton) {
                g_app.hoverButton = hover;
                InvalidateRect(g_app.btnStart, nullptr, FALSE);
                InvalidateRect(g_app.btnStop, nullptr, FALSE);
                InvalidateRect(g_app.btnExport, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            break;
        }

        case WM_MOUSELEAVE:
            if (g_app.hoverButton != 0) {
                g_app.hoverButton = 0;
                InvalidateRect(g_app.btnStart, nullptr, FALSE);
                InvalidateRect(g_app.btnStop, nullptr, FALSE);
                InvalidateRect(g_app.btnExport, nullptr, FALSE);
            }
            return 0;

        case WM_MOUSEWHEEL:
            // The page does not scroll; the wheel belongs to the matrix child.
            // Forward it without letting DefWindowProc do anything else.
            if (g_app.matrix != nullptr) {
                SendMessageW(g_app.matrix, WM_MOUSEWHEEL, wParam, lParam);
            }
            return 0;

        case WM_CL_PROGRESS: {
            auto* payload = reinterpret_cast<ProgressPayload*>(lParam);
            OnProgressMessage(payload);
            delete payload;
            SetProgressFromState();
            return 0;
        }

        case WM_CL_DONE: {
            auto* payload = reinterpret_cast<DonePayload*>(lParam);
            OnDoneMessage(payload);
            SetProgressFromState();
            return 0;
        }

        case WM_CL_AUTOSTART:
            StartRunFromUi();
            return 0;

        case WM_CLOSE:
            if (g_app.phase == Phase::Running) {
                StopAndJoin();
            } else if (g_app.worker.joinable()) {
                g_app.worker.join();
            }
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------
bool InitMeasureDc() {
    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return false;
    }
    g_measureDc = CreateCompatibleDC(screen);
    if (g_measureDc != nullptr) {
        g_measureBitmap = CreateCompatibleBitmap(screen, 8, 8);
        if (g_measureBitmap != nullptr) {
            g_measureOldFont = SelectObject(g_measureDc, g_measureBitmap);
        }
        SetBkMode(g_measureDc, TRANSPARENT);
        SetTextAlign(g_measureDc, TA_LEFT | TA_TOP);
        SetMapMode(g_measureDc, MM_TEXT);
    }
    ReleaseDC(nullptr, screen);
    return g_measureDc != nullptr && g_measureBitmap != nullptr;
}

void ShutdownMeasureDc() {
    if (g_measureDc != nullptr) {
        if (g_measureOldFont != nullptr) {
            SelectObject(g_measureDc, g_measureOldFont);
        }
        DeleteDC(g_measureDc);
        g_measureDc = nullptr;
    }
    if (g_measureBitmap != nullptr) {
        DeleteObject(g_measureBitmap);
        g_measureBitmap = nullptr;
    }
}

// Slots that share a ClCpuInfo::slot_core value are SMT siblings.  When the
// engine reports no usable grouping (physical unknown), fall back to the usual
// i ^ 1 layout so the UI can still mark hyper-threaded pairs.
void ComputeSiblings(const ClCpuInfo& cpu) {
    g_app.sibling.assign(static_cast<size_t>(CL_MAX_SLOTS), -1);
    if (cpu.logical <= 0) {
        return;
    }
    bool grouped = false;
    for (int i = 0; i < cpu.logical && i < CL_MAX_SLOTS; ++i) {
        if (cpu.slot_core[i] < 0) {
            continue;
        }
        bool shared = false;
        for (int j = 0; j < i; ++j) {
            if (cpu.slot_core[j] == cpu.slot_core[i]) {
                shared = true;
                break;
            }
        }
        if (shared) {
            grouped = true;
            break;
        }
    }
    if (grouped) {
        for (int i = 0; i < cpu.logical && i < CL_MAX_SLOTS; ++i) {
            for (int j = 0; j < cpu.logical && j < CL_MAX_SLOTS; ++j) {
                if (i != j && cpu.slot_core[i] >= 0 &&
                    cpu.slot_core[i] == cpu.slot_core[j]) {
                    g_app.sibling[static_cast<size_t>(i)] = j;
                    break;
                }
            }
        }
        return;
    }
    if (cpu.physical > 0 && cpu.physical * 2 == cpu.logical) {
        for (int i = 0; i < cpu.logical && i < CL_MAX_SLOTS; ++i) {
            g_app.sibling[static_cast<size_t>(i)] = i ^ 1;
        }
    }
}

struct LaunchOptions {
    int threads = 0;      // 0 = engine default
    bool autostart = false;
};

LaunchOptions ParseArgs() {
    LaunchOptions options;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return options;
    }
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--threads") == 0 && i + 1 < argc) {
            const int n = _wtoi(argv[i + 1]);
            if (n >= 2) {
                options.threads = n;
            }
            ++i;
        } else if (_wcsicmp(argv[i], L"--autostart") == 0) {
            options.autostart = true;
        }
    }
    LocalFree(argv);
    return options;
}

bool RegisterWindowClasses(HINSTANCE instance) {
    WNDCLASSEXW mainClass = {};
    mainClass.cbSize = sizeof(mainClass);
    mainClass.style = CS_HREDRAW | CS_VREDRAW;
    mainClass.lpfnWndProc = MainProc;
    mainClass.hInstance = instance;
    mainClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));
    mainClass.hIconSm = mainClass.hIcon;
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mainClass.hbrBackground = nullptr;   // (1) no class brush: we never erase
    mainClass.lpszClassName = kMainClass;
    if (RegisterClassExW(&mainClass) == 0) {
        return false;
    }

    WNDCLASSEXW matrixClass = {};
    matrixClass.cbSize = sizeof(matrixClass);
    matrixClass.style = CS_HREDRAW | CS_VREDRAW;
    matrixClass.lpfnWndProc = MatrixProc;
    matrixClass.hInstance = instance;
    matrixClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    matrixClass.hbrBackground = nullptr;  // (1) painted from the back buffer
    matrixClass.lpszClassName = kMatrixClass;
    return RegisterClassExW(&matrixClass) != 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    EnableDpiAwareness();
    g_instance = instance;

    INITCOMMONCONTROLSEX commonControls = {};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&commonControls);

    GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    if (GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr) != Ok) {
        return 1;
    }

    g_dpi = SystemDpi();
    g_scale = static_cast<float>(g_dpi) / 96.0f;
    if (!InitMeasureDc()) {
        GdiplusShutdown(gdiplusToken);
        return 1;
    }

    BuildPalette();
    BuildFonts();

    if (cl_query_cpu(&g_app.cpu) != 0) {
        g_app.cpu.logical = 8;
        g_app.cpu.physical = 8;
        g_app.cpu.cache_line = 64;
        strcpy_s(g_app.cpu.arch, sizeof(g_app.cpu.arch), "x64");
    }
    ComputeSiblings(g_app.cpu);

    // No thread selector: the program always measures every logical thread the
    // OS reports, exactly like the Android and HarmonyOS builds.  cl_query_cpu
    // already clamped the count to CL_MAX_SLOTS.
    const LaunchOptions options = ParseArgs();
    int threads = g_app.cpu.logical > 0 ? g_app.cpu.logical : 8;
    threads = std::max(2, std::min(threads, static_cast<int>(CL_MAX_SLOTS)));
    if (options.threads > 0 && options.threads < threads) {
        // Debug-only override used by the screenshot harness for short runs.
        threads = std::max(2, options.threads);
    }
    g_app.threads = threads;
    g_app.pairsTotal = threads * (threads - 1);

    g_app.resultDir = ResolveResultDir();
    RefreshDerived();
    BuildFooterText();

    if (!RegisterWindowClasses(instance)) {
        ShutdownMeasureDc();
        DestroyFonts();
        GdiplusShutdown(gdiplusToken);
        return 1;
    }

    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    RECT frame = {0, 0, Pxi(kDefaultW), Pxi(kDefaultH)};
    AdjustWindowRectForDpi(&frame, kMainStyle, FALSE, g_dpi);
    const int winW = frame.right - frame.left;
    const int winH = frame.bottom - frame.top;
    int winX = work.left + ((work.right - work.left) - winW) / 2;
    int winY = work.top + ((work.bottom - work.top) - winH) / 2;
    if (winY < work.top) {
        winY = work.top;
    }
    if (winX < work.left) {
        winX = work.left;
    }

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kMainClass, L"CoherencyLatency 1.2.0",
                                kMainStyle, winX, winY, std::max(winW, Pxi(kMinW)),
                                std::max(winH, Pxi(kMinH)), nullptr, nullptr, instance,
                                nullptr);
    if (hwnd == nullptr) {
        ShutdownMeasureDc();
        DestroyFonts();
        GdiplusShutdown(gdiplusToken);
        return 1;
    }

    const HICON icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));
    if (icon != nullptr) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    if (options.autostart) {
        PostMessageW(hwnd, WM_CL_AUTOSTART, 0, 0);
    }

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        // Alt+S / Alt+T / Alt+E work from any focus, plus Esc stops a run.
        if (message.message == WM_SYSKEYDOWN) {
            const bool alt = (message.lParam & (1 << 29)) != 0;
            if (alt) {
                const int key = static_cast<int>(message.wParam);
                if (key == 'S') {
                    if (CanStart()) {
                        StartRunFromUi();
                    }
                    continue;
                }
                if (key == 'T') {
                    StopFromUi();
                    continue;
                }
                if (key == 'E') {
                    ExportCsv(g_app.hwnd);
                    continue;
                }
            }
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            if (CanStop()) {
                StopFromUi();
                continue;
            }
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    StopAndJoin();
    if (g_app.abortFlag != nullptr) {
        cl_abort_destroy(g_app.abortFlag);
        g_app.abortFlag = nullptr;
    }
    g_mainBack.Release();
    g_matrixBack.Release();
    g_buttonBack.Release();
    ReleaseBrushes();
    DestroyFonts();
    ShutdownMeasureDc();
    GdiplusShutdown(gdiplusToken);
    return static_cast<int>(message.wParam);
}
