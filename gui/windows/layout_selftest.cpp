// CoherencyLatency 1.2.0 - layout self-test for the Windows GUI.
//
// Builds the same font metrics main.cpp feeds into clwin::Compute() (real GDI
// measurements at 96 dpi), lays out the scenarios GUI_SPEC.md cares about and
// prints one row per scenario in points, with PASS/FAIL.  No window, no engine.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>
#include <cmath>

#include "layout_core.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {

// --- font catalogue (must match main.cpp r02/r03) --------------------------
const wchar_t* kUiFamily = L"Segoe UI Variable Text";
const wchar_t* kDisplayFamily = L"Segoe UI Variable Display";
const wchar_t* kMonoFamily = L"Cascadia Mono";

struct FontDesc {
    const wchar_t* family;
    double size;      // points
    int weight;
    bool italic;
};

HFONT MakeFont(const FontDesc& d, double scale) {
    LOGFONTW lf = {};
    // Font sizes are device-independent pixels (WinUI logical px), exactly as
    // main.cpp creates them: lfHeight = -round(size * scale).
    lf.lfHeight = -static_cast<LONG>(std::lround(d.size * scale));
    lf.lfWeight = d.weight;
    lf.lfItalic = d.italic ? TRUE : FALSE;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = d.family == kMonoFamily ? FIXED_PITCH : VARIABLE_PITCH;
    wcscpy_s(lf.lfFaceName, d.family);
    return CreateFontIndirectW(&lf);
}

// One layout unit == one device-independent pixel == one macOS point.  GDI
// measures in pixels, so at scale 1.0 the numbers already match.
constexpr double kPxToPt = 1.0;

HDC g_dc = nullptr;
HFONT g_font = nullptr;
HGDIOBJ g_old = nullptr;
double g_scale = 1.0;

void UseFont(HFONT font) {
    g_old = SelectObject(g_dc, font);
    (void)g_old;
}

double MeasureWidth(const std::wstring& text) {
    if (text.empty()) {
        return 0.0;
    }
    SIZE size = {};
    GetTextExtentPoint32W(g_dc, text.c_str(), static_cast<int>(text.size()), &size);
    return static_cast<double>(size.cx) / g_scale * kPxToPt;
}

double LineHeightOf(HFONT font) {
    HGDIOBJ old = SelectObject(g_dc, font);
    TEXTMETRICW tm = {};
    GetTextMetricsW(g_dc, &tm);
    SelectObject(g_dc, old);
    return static_cast<double>(tm.tmHeight) / g_scale;
}

struct Fonts {
    HFONT title = nullptr;
    HFONT subtitle = nullptr;
    HFONT cardTitle = nullptr;
    HFONT info = nullptr;
    HFONT hint = nullptr;
    HFONT version = nullptr;
    HFONT button = nullptr;
    HFONT status = nullptr;
    HFONT legend = nullptr;
    HFONT stats = nullptr;
    HFONT footer = nullptr;
    HFONT matrixTitle = nullptr;
    HFONT count = nullptr;
    HFONT label = nullptr;
    HFONT threadLabel = nullptr;
    HFONT cell[4] = {};
    HFONT footnote = nullptr;
};

Fonts g_fonts;

void BuildFonts() {
    g_fonts.title = MakeFont({kDisplayFamily, 26.0, FW_SEMIBOLD, false}, g_scale);
    g_fonts.subtitle = MakeFont({kUiFamily, 12.5, FW_NORMAL, false}, g_scale);
    g_fonts.cardTitle = MakeFont({kUiFamily, 13.0, FW_SEMIBOLD, false}, g_scale);
    g_fonts.info = MakeFont({kUiFamily, 12.0, FW_NORMAL, false}, g_scale);
    g_fonts.hint = MakeFont({kUiFamily, 11.0, FW_NORMAL, false}, g_scale);
    g_fonts.version = MakeFont({kUiFamily, 11.0, FW_NORMAL, false}, g_scale);
    g_fonts.button = MakeFont({kUiFamily, 13.0, FW_SEMIBOLD, false}, g_scale);
    g_fonts.status = MakeFont({kUiFamily, 13.0, FW_NORMAL, false}, g_scale);
    g_fonts.legend = MakeFont({kUiFamily, 11.0, FW_NORMAL, false}, g_scale);
    g_fonts.stats = MakeFont({kUiFamily, 12.0, FW_NORMAL, false}, g_scale);
    g_fonts.footer = MakeFont({kUiFamily, 11.0, FW_NORMAL, false}, g_scale);
    g_fonts.matrixTitle = MakeFont({kUiFamily, 13.0, FW_SEMIBOLD, false}, g_scale);
    g_fonts.count = MakeFont({kUiFamily, 12.0, FW_NORMAL, false}, g_scale);
    g_fonts.label = MakeFont({kUiFamily, 10.5, FW_NORMAL, false}, g_scale);
    g_fonts.threadLabel = MakeFont({kUiFamily, 12.0, FW_NORMAL, false}, g_scale);
    g_fonts.footnote = MakeFont({kUiFamily, 11.0, FW_NORMAL, false}, g_scale);
    const double cellSizes[4] = {11.5, 10.0, 9.0, 8.0};
    for (int i = 0; i < 4; ++i) {
        g_fonts.cell[i] = MakeFont({kMonoFamily, cellSizes[i], FW_BOLD, false}, g_scale);
    }
}

void DestroyFonts() {
    HFONT all[] = {g_fonts.title,      g_fonts.subtitle,  g_fonts.cardTitle,
                   g_fonts.info,       g_fonts.hint,      g_fonts.version,
                   g_fonts.button,     g_fonts.status,    g_fonts.legend,
                   g_fonts.stats,      g_fonts.footer,    g_fonts.matrixTitle,
                   g_fonts.count,      g_fonts.label,     g_fonts.threadLabel,
                   g_fonts.footnote};
    for (HFONT font : all) {
        if (font != nullptr) {
            DeleteObject(font);
        }
    }
    for (HFONT font : g_fonts.cell) {
        if (font != nullptr) {
            DeleteObject(font);
        }
    }
}

void DumpFontMetrics() {
    struct Entry { const wchar_t* name; HFONT font; };
    const Entry entries[] = {
        {L"title 26", g_fonts.title},        {L"subtitle 12.5", g_fonts.subtitle},
        {L"cardTitle 13", g_fonts.cardTitle}, {L"info 12", g_fonts.info},
        {L"hint 11", g_fonts.hint},          {L"version 11", g_fonts.version},
        {L"button 13", g_fonts.button},      {L"status 13", g_fonts.status},
        {L"legend 11", g_fonts.legend},      {L"stats 12", g_fonts.stats},
        {L"footer 11", g_fonts.footer},      {L"matrixTitle 13", g_fonts.matrixTitle},
        {L"count 12", g_fonts.count},        {L"label 10.5", g_fonts.label},
        {L"threadLabel 12", g_fonts.threadLabel}, {L"footnote 11", g_fonts.footnote},
        {L"cell 11.5", g_fonts.cell[0]},     {L"cell 10", g_fonts.cell[1]},
        {L"cell 9", g_fonts.cell[2]},        {L"cell 8", g_fonts.cell[3]},
    };
    std::printf("%-18s %6s %6s %6s %6s %6s\n", "font", "height", "ascent", "descent", "extlead", "a+d");
    for (const Entry& e : entries) {
        HGDIOBJ old = SelectObject(g_dc, e.font);
        TEXTMETRICW tm = {};
        GetTextMetricsW(g_dc, &tm);
        SelectObject(g_dc, old);
        std::printf("%-18s %6.1f %6.1f %6.1f %6.1f %6.1f\n", e.name,
                    static_cast<double>(tm.tmHeight), static_cast<double>(tm.tmAscent),
                    static_cast<double>(tm.tmDescent),
                    static_cast<double>(tm.tmExternalLeading),
                    static_cast<double>(tm.tmAscent + tm.tmDescent));
    }
    std::printf("\n");
}

double TextHeight(HFONT font) {
    return std::ceil(LineHeightOf(font) * kPxToPt);
}

// Builds the metric set main.cpp would hand to clwin::Compute().
clwin::Metrics BuildMetrics(double width, double height, int threads, bool smt,
                            int infoLines) {
    clwin::Metrics m;
    m.width = width;
    m.height = height;
    m.threads = threads;
    m.hasSmt = smt;
    m.infoLineCount = infoLines;

    m.titleH = TextHeight(g_fonts.title);
    m.subtitleH = TextHeight(g_fonts.subtitle);
    m.versionH = TextHeight(g_fonts.version);
    m.cardTitleH = TextHeight(g_fonts.cardTitle);
    m.infoH = TextHeight(g_fonts.info);
    m.hintH = TextHeight(g_fonts.hint);
    m.statusH = TextHeight(g_fonts.status);
    m.footerH = TextHeight(g_fonts.footer);
    m.matrixTitleH = TextHeight(g_fonts.matrixTitle);
    m.countH = TextHeight(g_fonts.count);
    m.legendH = TextHeight(g_fonts.legend);
    m.statsH = TextHeight(g_fonts.stats);
    m.footnoteH = TextHeight(g_fonts.footnote);
    m.threadLabelH = TextHeight(g_fonts.threadLabel);
    m.labelH = TextHeight(g_fonts.label);

    UseFont(g_fonts.title);
    m.titleW = MeasureWidth(L"CoherencyLatency");
    UseFont(g_fonts.subtitle);
    m.subtitleW = MeasureWidth(L"缓存一致性延迟测试 · Windows 桌面版");
    UseFont(g_fonts.version);
    m.versionW = MeasureWidth(L"1.2.0");
    UseFont(g_fonts.threadLabel);
    m.threadLabelW = MeasureWidth(L"线程数");
    UseFont(g_fonts.hint);
    m.threadHintW = MeasureWidth(L"线程越多用时越长");
    UseFont(g_fonts.legend);
    m.lowW = MeasureWidth(L"低");
    m.highW = MeasureWidth(L"高");
    UseFont(g_fonts.label);
    const double t31 = MeasureWidth(L"T31");
    const double t7s = MeasureWidth(L"T7*");
    m.matrixLabelW = std::ceil(std::max(t31, t7s)) + 8.0;
    m.matrixLabelW = std::max(m.matrixLabelW, 26.0);
    m.unitW = MeasureWidth(L"ns/op");
    UseFont(g_fonts.stats);
    m.statsW = MeasureWidth(L"平均 97.9 · 最小 24.5 · 最大 126.6 ns");
    for (int i = 0; i < 4; ++i) {
        UseFont(g_fonts.cell[i]);
        m.cellSampleW[i] = MeasureWidth(L"888.8");
    }
    return m;
}

const char* YesNo(bool value) { return value ? "yes" : "no"; }

struct Scenario {
    const char* name;
    double width;
    double height;
    int threads;
    bool smt;
    int infoLines;
    // Expectations.
    double minCell;
    double maxCell;
    bool expectNoScroller;
};

int RunScenarios(bool verbose) {
    const Scenario scenarios[] = {
        {"default 760x620 n=8 no-smt", 760.0, 620.0, 8, false, 3, 26.0, 40.0, true},
        {"default 760x620 n=8 smt", 760.0, 620.0, 8, true, 4, 24.0, 40.0, true},
        {"minimum 680x520 n=8 no-smt", 680.0, 520.0, 8, false, 3, 14.0, 18.0, true},
        {"minimum 680x520 n=8 smt", 680.0, 520.0, 8, true, 4, 14.0, 18.0, true},
        {"default 760x620 n=16", 760.0, 620.0, 16, false, 4, 14.0, 40.0, false},
        {"default 760x620 n=32", 760.0, 620.0, 32, false, 4, 14.0, 40.0, false},
        {"large 1100x760 n=8 smt", 1100.0, 760.0, 8, true, 5, 26.0, 40.0, true},
        {"large 1100x760 n=16 smt", 1100.0, 760.0, 16, true, 5, 14.0, 40.0, false},
        {"wide 1400x900 n=32 smt", 1400.0, 900.0, 32, true, 5, 14.0, 40.0, false},
    };

    std::printf("%-30s %-10s %6s %8s %8s %8s %6s %5s %5s %6s %s\n", "scenario",
                "content", "cell", "gridW", "gridH", "region", "slack", "vscr",
                "hscr", "font", "result");
    int failures = 0;
    for (const Scenario& sc : scenarios) {
        const clwin::Metrics metrics =
            BuildMetrics(sc.width, sc.height, sc.threads, sc.smt, sc.infoLines);
        const clwin::Layout layout = clwin::Compute(metrics);
        const int problems = clwin::Validate(metrics, layout, verbose);
        bool ok = true;
        std::string why;
        if (layout.cell < sc.minCell - 0.01 || layout.cell > sc.maxCell + 0.01) {
            ok = false;
            why += "cell ";
        }
        if (sc.expectNoScroller &&
            (layout.verticalScroller || layout.horizontalScroller)) {
            ok = false;
            why += "scroller ";
        }
        if (layout.gridSlack < 0.0) {
            ok = false;
            why += "negative-slack ";
        }
        if (problems != 0) {
            ok = false;
            why += "validate ";
        }
        if (sc.width <= 760.5 && sc.height >= 619.5 && sc.threads == 8 &&
            !sc.smt && layout.gridSlack < 20.0) {
            ok = false;
            why += "slack<20 ";
        }
        char content[32];
        std::snprintf(content, sizeof(content), "%.0fx%.0f", sc.width, sc.height);
        std::printf("%-30s %-10s %6.1f %8.1f %8.1f %8.1f %+6.1f %5s %5s %6.1f %s\n",
                    sc.name, content, layout.cell, layout.gridW, layout.gridH,
                    layout.gridRegionH, layout.gridSlack, YesNo(layout.verticalScroller),
                    YesNo(layout.horizontalScroller), layout.cellFontSize,
                    ok ? "PASS" : ("FAIL " + why).c_str());
        if (!ok) {
            ++failures;
        }
        if (verbose) {
            clwin::Dump(metrics, layout, stdout);
        }
    }
    std::printf("\n%d scenario(s), %d failure(s)\n",
                static_cast<int>(sizeof(scenarios) / sizeof(scenarios[0])), failures);
    return failures;
}

}  // namespace

int main(int argc, char** argv) {
    bool verbose = (argc > 1);
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--dump") {
            verbose = true;
        }
    }
    SetProcessDPIAware();
    g_dc = CreateCompatibleDC(nullptr);
    if (g_dc == nullptr) {
        std::fprintf(stderr, "CreateCompatibleDC failed\n");
        return 2;
    }
    BuildFonts();
    std::printf("CoherencyLatency layout self-test (points, 96 dpi reference)\n");
    std::printf("ui=%ls display=%ls mono=%ls\n\n", kUiFamily, kDisplayFamily,
                kMonoFamily);
    DumpFontMetrics();
    const int failures = RunScenarios(verbose);
    DestroyFonts();
    DeleteDC(g_dc);
    return failures == 0 ? 0 : 1;
}
