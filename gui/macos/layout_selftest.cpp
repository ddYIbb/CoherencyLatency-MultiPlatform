// CoherencyLatency 1.2.0 - host-side layout self-test (no AppKit required).
//
// GUI_SPEC.md v3 section 1 keeps the whole window geometry in a pure function
// (cl_layout::Compute) precisely so it can be checked on a machine that cannot
// run the Mach-O binary.  This program rebuilds the same cl_layout::Metrics the
// app measures with AppKit - using a font-advance model instead of CoreText -
// and asserts the properties the design depends on:
//
//   * PORTRAIT window, 600 x 880 pt content, minimum 520 x 760;
//   * no thread selector anywhere: the device rows span the full card width and
//     a run always covers every logical thread;
//   * an 8-thread matrix is fully visible at the default size, with numbers;
//   * from 13 threads on the matrix is a colour overview: no glyphs, and a
//     32x32 grid still fits the card width (no horizontal scroller);
//   * the column-header strip is a fixed row outside the scrolling viewport,
//     and every header is centred on its column;
//   * every block stays inside the window and the cards keep their padding.
//
// Exit code 0 means every check passed.

#include "cl_layout.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// --- font advance model ----------------------------------------------------
//
// Approximate SF Pro / PingFang SC advances as a fraction of the point size.
// The layout only needs to know how wide a fixed string is, and the app
// measures the same strings with CoreText, so a model accurate to a few percent
// is enough - but it is deliberately pessimistic (wide) so a passing check
// cannot hide a real clip.
double AdvanceRatio(unsigned int codePoint, bool digitFont) {
    if (codePoint == ' ') {
        return 0.28;
    }
    if (codePoint >= '0' && codePoint <= '9') {
        return digitFont ? 0.60 : 0.56;
    }
    if (codePoint >= 'A' && codePoint <= 'Z') {
        return 0.62;
    }
    if (codePoint >= 'a' && codePoint <= 'z') {
        return 0.51;
    }
    if (codePoint == 0x00B7) {  // middle dot
        return 0.33;
    }
    if (codePoint == 0x2013 || codePoint == 0x2014 || codePoint == '-') {
        return 0.55;
    }
    if (codePoint == 0x2026 || codePoint == 0x2194 || codePoint == 0x00D7 ||
        codePoint == 0x00B0 || codePoint == 0x3001 || codePoint == 0x3002 ||
        codePoint == 0xFF1A || codePoint == 0xFF0C) {
        return 1.00;  // ellipsis, arrow, multiplication sign, CJK punctuation
    }
    if (codePoint >= 0x2E80) {
        return 1.00;  // CJK ideographs and full-width forms
    }
    return 0.55;
}

struct Font {
    double size = 12.0;
    bool digits = false;  // monospacedDigitSystemFontOfSize:
};

double TextWidth(const std::string& text, const Font& font) {
    double width = 0.0;
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        unsigned int codePoint = lead;
        std::size_t length = 1;
        if ((lead & 0xE0) == 0xC0) {
            codePoint = lead & 0x1Fu;
            length = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            codePoint = lead & 0x0Fu;
            length = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            codePoint = lead & 0x07u;
            length = 4;
        }
        for (std::size_t offset = 1; offset < length && index + offset < text.size();
             ++offset) {
            codePoint = (codePoint << 6) |
                        (static_cast<unsigned char>(text[index + offset]) & 0x3Fu);
        }
        width += AdvanceRatio(codePoint, font.digits) * font.size;
        index += length;
    }
    return std::ceil(width);
}

// AppKit reports ceil(ascender - descender + leading); the factor covers the
// usual 1.15..1.32 range for SF Pro at these sizes.
double TextHeight(const Font& font, double lineFactor) {
    return std::ceil(font.size * lineFactor);
}

// Greedy wrapping, same rule as CLWrapText in main.mm.
void Wrap(const std::string& text, const Font& font, double maxWidth,
          std::vector<std::string>* out) {
    if (out == nullptr) {
        return;
    }
    if (maxWidth <= 1.0) {
        out->push_back(text);
        return;
    }
    std::string line;
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        std::size_t length = 1;
        if ((lead & 0xE0) == 0xC0) {
            length = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
        }
        if (index + length > text.size()) {
            length = 1;
        }
        const std::string piece = text.substr(index, length);
        line += piece;
        if (TextWidth(line, font) > maxWidth && line.size() > piece.size()) {
            line.erase(line.size() - piece.size());
            out->push_back(line);
            line = piece;
        }
        index += length;
    }
    if (!line.empty()) {
        out->push_back(line);
    }
}

// --- the strings main.mm renders ------------------------------------------

const Font kTitle{26.0, false};
const Font kSubtitle{13.0, false};
const Font kVersion{11.0, true};
const Font kCardTitle{13.0, false};
const Font kInfo{12.0, false};
const Font kStatus{12.5, false};
const Font kFooter{11.0, false};
const Font kMatrixTitle{13.0, false};
const Font kMatrixCount{12.0, true};
const Font kFootnote{11.0, false};
const Font kLegend{11.0, false};
const Font kHeader{10.5, true};
const Font kButton{13.0, false};

std::string ThreadSpan(int n) {
    if (n < 2) {
        return "T0";
    }
    return "T0\xE2\x80\xA6T" + std::to_string(n - 1);
}

// Mirrors -[CLRootView deviceInfoRows] for a macOS host: no SMT topology is
// available there (engine_api.cpp fills slot_core only on Windows), so the SMT
// line, the asterisks and the footnote are absent.
std::vector<std::string> DeviceInfoRows(int logical, int threads, bool hasSmt) {
    std::vector<std::string> rows;
    rows.push_back(std::to_string(logical) +
                   " \xE4\xB8\xAA\xE9\x80\xBB\xE8\xBE\x91\xE7\xBA\xBF\xE7\xA8\x8B \xC2\xB7 128 B \xE7\xBC\x93\xE5\xAD\x98\xE8\xA1\x8C \xC2\xB7 x64");
    if (hasSmt) {
        rows.push_back(
            "\xE7\x89\xA9\xE7\x90\x86\xE6\xA0\xB8 4 \xE4\xB8\xAA \xC2\xB7 \xE5\x90\x8C\xE6\xA0\xB8\xE8\xB6\x85\xE7\xBA\xBF\xE7\xA8\x8B\xEF\xBC\x9AT1/T2\xE3\x80\x81T3/T4");
    }
    const std::string tail =
        hasSmt
            ? "\xEF\xBC\x9B\xE5\x90\x8C\xE6\xA0\xB8\xE4\xB8\xA4\xE4\xB8\xAA\xE7\xBA\xBF\xE7\xA8\x8B\xE4\xB9\x8B\xE9\x97\xB4\xE7\x9A\x84\xE5\xBB\xB6\xE8\xBF\x9F\xE9\x80\x9A\xE5\xB8\xB8\xE7\xBA\xA6 30 ns"
            : "\xEF\xBC\x9B\xE8\xB7\xA8\xE6\xA0\xB8\xE7\xBA\xBF\xE7\xA8\x8B\xE5\xBB\xB6\xE8\xBF\x9F\xE9\x80\x9A\xE5\xB8\xB8\xE5\x9C\xA8 30\xE2\x80\x93"
              "120 ns";
    rows.push_back("\xE6\xB5\x8B\xE9\x87\x8F\xE5\xAF\xB9\xE8\xB1\xA1\xEF\xBC\x9A" +
                   ThreadSpan(threads) + " \xE5\x85\xB1 " +
                   std::to_string(threads) +
                   " \xE4\xB8\xAA\xE9\x80\xBB\xE8\xBE\x91\xE7\xBA\xBF\xE7\xA8\x8B" + tail);
    rows.push_back(
        "\xE5\x9B\xBA\xE5\xAE\x9A\xE5\x8F\x82\xE6\x95\xB0\xEF\xBC\x9A" +
        std::to_string(threads) +
        " \xE4\xB8\xAA\xE9\x80\xBB\xE8\xBE\x91\xE7\xBA\xBF\xE7\xA8\x8B \xC2\xB7 1,000,000 \xE6\xAC\xA1 \xC2\xB7 3 \xE8\xBD\xAE \xC2\xB7 Bounce");
    return rows;
}

// Mirrors -[CLRootView syncLayout]: measure, wrap, then hand over to Compute.
cl_layout::Metrics MakeMetrics(double width, double height, int threads,
                               bool hasSmt, double lineFactor,
                               double buttonPadding) {
    cl_layout::Metrics m;
    m.width = width;
    m.height = height;
    m.threads = threads;
    m.hasSmt = hasSmt;

    m.titleH = TextHeight(kTitle, lineFactor);
    m.subtitleH = TextHeight(kSubtitle, lineFactor);
    m.versionH = TextHeight(kVersion, lineFactor);
    m.cardTitleH = TextHeight(kCardTitle, lineFactor);
    m.infoH = TextHeight(kInfo, lineFactor);
    m.statusH = TextHeight(kStatus, lineFactor);
    m.footerH = TextHeight(kFooter, lineFactor);
    m.matrixTitleH = TextHeight(kMatrixTitle, lineFactor);
    m.matrixCountH = TextHeight(kMatrixCount, lineFactor);
    m.footnoteH = TextHeight(kFootnote, lineFactor);
    m.headerH = TextHeight(kHeader, lineFactor);

    m.titleW = TextWidth("CoherencyLatency", kTitle) + 2.0;
    m.versionW = TextWidth("1.2.0", kVersion) + 2.0;
    m.lowLabelW = TextWidth("\xE4\xBD\x8E", kLegend);
    m.highLabelW = TextWidth("\xE9\xAB\x98", kLegend);
    m.matrixLabelW =
        TextWidth("T" + std::to_string(threads > 1 ? threads - 1 : 0), kHeader) + 10.0;
    m.progressH = 12.0;  // NSProgressIndicator, bar style, small control size

    const double startW =
        TextWidth("\xE5\xBC\x80\xE5\xA7\x8B\xE6\xB5\x8B\xE8\xAF\x95", kButton);
    const double stopW = TextWidth("\xE5\x81\x9C\xE6\xAD\xA2", kButton);
    const double exportW = TextWidth("\xE5\xAF\xBC\xE5\x87\xBA CSV", kButton);
    m.startButtonW = std::max(100.0, startW + buttonPadding);
    m.stopButtonW = std::max(74.0, stopW + buttonPadding);
    m.exportButtonW = std::max(96.0, exportW + buttonPadding);

    const double infoW = cl_layout::InfoWidth(m);
    std::vector<std::string> lines;
    for (const std::string& row : DeviceInfoRows(threads, threads, hasSmt)) {
        Wrap(row, kInfo, infoW, &lines);
    }
    m.infoLineCount = static_cast<int>(lines.size());
    return m;
}

// --- reporting ------------------------------------------------------------

int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const char* what) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        std::printf("  FAIL: %s\n", what);
    }
}

void CheckClose(double value, double expected, double tolerance,
                const char* what) {
    ++gChecks;
    if (std::fabs(value - expected) > tolerance) {
        ++gFailures;
        std::printf("  FAIL: %s (got %.2f, expected %.2f)\n", what, value, expected);
    }
}

void DumpLayout(const cl_layout::Layout& layout) {
    std::printf("    title      (%.1f,%.1f %.1fx%.1f) subtitle (%.1f,%.1f %.1fx%.1f)\n",
                layout.title.x, layout.title.y, layout.title.width,
                layout.title.height, layout.subtitle.x, layout.subtitle.y,
                layout.subtitle.width, layout.subtitle.height);
    std::printf("    deviceCard (%.1f,%.1f %.1fx%.1f)  infoW %.1f  lines %d\n",
                layout.deviceCard.x, layout.deviceCard.y, layout.deviceCard.width,
                layout.deviceCard.height, layout.infoWidth, layout.infoLineCount);
    std::printf("    buttons    start (%.1f,%.1f %.1fx%.1f) stop (%.1f,%.1f %.1fx%.1f) export (%.1f,%.1f %.1fx%.1f)\n",
                layout.startButton.x, layout.startButton.y,
                layout.startButton.width, layout.startButton.height,
                layout.stopButton.x, layout.stopButton.y, layout.stopButton.width,
                layout.stopButton.height, layout.exportButton.x,
                layout.exportButton.y, layout.exportButton.width,
                layout.exportButton.height);
    std::printf("    status     (%.1f,%.1f %.1fx%.1f)   progress (%.1f,%.1f %.1fx%.1f) row %.1f\n",
                layout.status.x, layout.status.y, layout.status.width,
                layout.status.height, layout.progress.x, layout.progress.y,
                layout.progress.width, layout.progress.height,
                layout.progressRowHeight);
    std::printf("    matrixCard (%.1f,%.1f %.1fx%.1f)\n", layout.matrixCard.x,
                layout.matrixCard.y, layout.matrixCard.width,
                layout.matrixCard.height);
    std::printf("    headerRow  (%.1f,%.1f %.1fx%.1f)  values=%d\n",
                layout.headerRow.x, layout.headerRow.y, layout.headerRow.width,
                layout.headerRow.height, layout.showCellValues ? 1 : 0);
    std::printf("    scrollView (%.1f,%.1f %.1fx%.1f)  doc %.1fx%.1f  vscroll=%d hscroll=%d\n",
                layout.scrollView.x, layout.scrollView.y, layout.scrollView.width,
                layout.scrollView.height, layout.matrixDoc.width,
                layout.matrixDoc.height, layout.verticalScroller ? 1 : 0,
                layout.horizontalScroller ? 1 : 0);
    std::printf("    grid       cell %.1fx%.1f gap %.1f label %.1f  %.1fx%.1f  regionH %.1f slack %+.1f  font %.1f\n",
                layout.cellWidth, layout.cellHeight, layout.cellGap,
                layout.labelWidth, layout.gridWidth, layout.gridHeight,
                layout.gridRegionHeight, layout.gridSlack, layout.cellFontSize);
    std::printf("    legend     low (%.1f,%.1f) bar (%.1f,%.1f %.1fx%.1f) high (%.1f,%.1f) stats (%.1f,%.1f %.1fx%.1f)\n",
                layout.legendLow.x, layout.legendLow.y, layout.legendBar.x,
                layout.legendBar.y, layout.legendBar.width,
                layout.legendBar.height, layout.legendHigh.x,
                layout.legendHigh.y, layout.stats.x, layout.stats.y,
                layout.stats.width, layout.stats.height);
    std::printf("    footer     (%.1f,%.1f %.1fx%.1f)\n", layout.footer.x,
                layout.footer.y, layout.footer.width, layout.footer.height);
}

struct Scenario {
    double width;
    double height;
    int threads;
    bool hasSmt;
    double lineFactor;
    const char* label;
};

void RunScenario(const Scenario& scenario, bool verbose) {
    std::printf("\n[%s] %.0fx%.0f  n=%d  smt=%d  line=%.2f\n", scenario.label,
                scenario.width, scenario.height, scenario.threads,
                scenario.hasSmt ? 1 : 0, scenario.lineFactor);
    const cl_layout::Metrics metrics =
        MakeMetrics(scenario.width, scenario.height, scenario.threads,
                    scenario.hasSmt, scenario.lineFactor, 44.0);
    const cl_layout::Layout layout = cl_layout::Compute(metrics);
    if (verbose) {
        DumpLayout(layout);
    }
    const int problems = cl_layout::Validate(metrics, layout, true);
    Check(problems == 0, "Validate() reports no structural problem");

    // The grid never leaves its document view: this is the "never clip the last
    // column" guarantee, expressed numerically.
    Check(layout.matrixDoc.width + 0.01 >= layout.gridWidth,
          "document view is at least as wide as the grid");
    Check(layout.matrixDoc.height + 0.01 >= layout.gridHeight,
          "document view is at least as tall as the grid");

    // The header strip is a separate, fixed row: it must sit above the viewport
    // so scrolling the grid cannot paint over the card title or the counter.
    Check(layout.headerRow.MaxY() <= layout.scrollView.y + 0.01,
          "the fixed header row ends above the scrolling viewport");
    Check(layout.headerRow.y >= layout.matrixTitle.MaxY() - 0.01,
          "the header row starts below the card title");

    // Colour-overview mode (n > 12) never needs a horizontal scroller.
    if (!layout.showCellValues) {
        Check(!layout.horizontalScroller,
              "colour-overview grid fits the card width (no horizontal scroller)");
        Check(layout.gridWidth <= layout.scrollView.width + 0.5,
              "colour-overview grid is no wider than the viewport");
    } else {
        Check(layout.cellWidth >= cl_layout::kCellMin - 0.01 &&
                  layout.cellHeight >= cl_layout::kCellMin - 0.01,
              "value cells stay at or above the 18 pt minimum");
    }

    // Column headers are centred on their columns.
    if (scenario.threads >= 2) {
        const double pitch = layout.cellWidth + layout.cellGap;
        const double first = cl_layout::ColumnCenterX(layout, 0);
        const double last = cl_layout::ColumnCenterX(layout, scenario.threads - 1);
        CheckClose(last - first, (scenario.threads - 1) * pitch, 0.01,
                   "column centres are one pitch apart");
        CheckClose(last, layout.gridWidth - layout.cellWidth / 2.0, 0.01,
                   "the last column centre is half a cell inside the grid edge");
    }

    // The device card has no selector column: its rows use the whole inner
    // width.  This is what makes the removed 线程数 control testable.
    CheckClose(layout.infoWidth,
               scenario.width - 2.0 * cl_layout::kMargin -
                   2.0 * cl_layout::kCardPadding,
               0.01, "device info rows span the full card width");
}

// The properties the Lead asked to see asserted.
void CheckLockedDesign() {
    std::printf("\n[locked design: portrait, no selector]\n");
    Check(cl_layout::kDefaultHeight > cl_layout::kDefaultWidth,
          "default content size is portrait (height > width)");
    CheckClose(cl_layout::kDefaultWidth, 600.0, 0.01, "default width is 600 pt");
    CheckClose(cl_layout::kDefaultHeight, 880.0, 0.01, "default height is 880 pt");
    CheckClose(cl_layout::kMinWidth, 520.0, 0.01, "minimum width is 520 pt");
    CheckClose(cl_layout::kMinHeight, 760.0, 0.01, "minimum height is 760 pt");

    // No thread selector: the information rows occupy the full card width for
    // every window size, so nothing is reserved for a popup on the right.
    for (double width : {520.0, 600.0, 760.0, 1000.0}) {
        cl_layout::Metrics metrics;
        metrics.width = width;
        metrics.height = 880.0;
        const double expected =
            width - 2.0 * cl_layout::kMargin - 2.0 * cl_layout::kCardPadding;
        CheckClose(cl_layout::InfoWidth(metrics), expected, 0.01,
                   "InfoWidth uses the full card inner width (no selector column)");
    }

    // A run always covers every logical thread: Compute is driven straight from
    // the logical count and the matrix dimension follows it.
    for (int logical : {2, 4, 8, 12, 16, 24, 32, 64}) {
        const cl_layout::Metrics metrics =
            MakeMetrics(600.0, 880.0, logical, false, 1.25, 44.0);
        const cl_layout::Layout layout = cl_layout::Compute(metrics);
        const double lastColumnRight =
            layout.labelWidth + logical * layout.cellWidth +
            (logical - 1) * layout.cellGap;
        CheckClose(lastColumnRight, layout.gridWidth, 0.01,
                   "n columns exactly fill the grid width");
        Check(layout.matrixDoc.width + 0.01 >= lastColumnRight,
              "the last column is inside the scrollable document view");
    }
}

// The default window must show an 8-thread matrix with numbers and no scroller.
void CheckMatrixFits() {
    std::printf("\n[matrix fit at the default 600x880]\n");
    for (int threads : {2, 4, 8, 12}) {
        const cl_layout::Metrics metrics =
            MakeMetrics(600.0, 880.0, threads, false, 1.25, 44.0);
        const cl_layout::Layout layout = cl_layout::Compute(metrics);
        std::printf("  n=%-2d cell %.1fx%.1f  grid %.1fx%.1f  regionH %.1f  slack %+.1f  values=%d  vscroll=%d hscroll=%d\n",
                    threads, layout.cellWidth, layout.cellHeight, layout.gridWidth,
                    layout.gridHeight, layout.gridRegionHeight, layout.gridSlack,
                    layout.showCellValues ? 1 : 0,
                    layout.verticalScroller ? 1 : 0,
                    layout.horizontalScroller ? 1 : 0);
        Check(layout.showCellValues, "up to 12 threads every cell shows a value");
        Check(!layout.verticalScroller,
              "no vertical scroller for a small matrix at the default size");
        Check(!layout.horizontalScroller,
              "no horizontal scroller for a small matrix at the default size");
        Check(layout.gridSlack >= 0.0, "the grid fits the matrix card");
    }
    {
        const cl_layout::Metrics metrics =
            MakeMetrics(600.0, 880.0, 8, false, 1.25, 44.0);
        const cl_layout::Layout layout = cl_layout::Compute(metrics);
        Check(!layout.verticalScroller && !layout.horizontalScroller,
              "the 8x8 matrix needs no scrollbar at 600x880");
        Check(layout.gridWidth <= layout.scrollView.width + 0.5,
              "the 8x8 grid is no wider than the viewport");
        Check(layout.gridHeight <= layout.scrollView.height + 0.5,
              "the 8x8 grid is no taller than the viewport");
        std::printf("  8x8: grid %.1fx%.1f in viewport %.1fx%.1f\n",
                    layout.gridWidth, layout.gridHeight, layout.scrollView.width,
                    layout.scrollView.height);
    }
}

// From 13 threads on: colour blocks only, and the whole width fits the card.
void CheckWideMatrixOverview() {
    std::printf("\n[wide matrix: colour overview, no horizontal scroller]\n");
    for (int threads : {13, 16, 24, 32, 64}) {
        const cl_layout::Metrics metrics =
            MakeMetrics(600.0, 880.0, threads, false, 1.25, 44.0);
        const cl_layout::Layout layout = cl_layout::Compute(metrics);
        std::printf("  n=%-2d cell %.1fx%.1f gap %.0f  grid %.1fx%.1f  viewport %.1fx%.1f  values=%d vscroll=%d hscroll=%d\n",
                    threads, layout.cellWidth, layout.cellHeight, layout.cellGap,
                    layout.gridWidth, layout.gridHeight, layout.scrollView.width,
                    layout.scrollView.height, layout.showCellValues ? 1 : 0,
                    layout.verticalScroller ? 1 : 0,
                    layout.horizontalScroller ? 1 : 0);
        Check(!layout.showCellValues,
              "from 13 threads the cells carry no glyphs (colour overview)");
        Check(!layout.horizontalScroller,
              "the colour-overview grid never needs a horizontal scroller");
        Check(layout.gridWidth <= layout.scrollView.width + 0.5,
              "the colour-overview grid fits the card width");
        Check(layout.matrixDoc.width + 0.01 >= layout.gridWidth,
              "the document view still contains the full grid width");
        Check(layout.matrixDoc.height + 0.01 >= layout.gridHeight,
              "the document view still contains the full grid height");
    }
    {
        const cl_layout::Metrics metrics =
            MakeMetrics(600.0, 880.0, 32, false, 1.25, 44.0);
        const cl_layout::Layout layout = cl_layout::Compute(metrics);
        std::printf("  32x32: cell %.1f pt, grid %.1fx%.1f in viewport %.1fx%.1f\n",
                    layout.cellWidth, layout.gridWidth, layout.gridHeight,
                    layout.scrollView.width, layout.scrollView.height);
        Check(layout.cellWidth >= cl_layout::kCellCompactMin - 0.01,
              "32-thread cells stay above the compact floor");
        Check(layout.cellWidth <= 20.0,
              "32-thread cells are small enough to fit the portrait card");
        // Vertical scrolling is allowed but not required: the portrait card is
        // tall enough that a 32x32 grid fits outright. What matters is that the
        // grid never overflows the viewport horizontally and that, when it does
        // scroll, the document view holds the whole grid (the header row sits
        // outside the clip, so it can never be overpainted).
        Check(!layout.horizontalScroller,
              "a 32x32 grid never needs a horizontal scroller");
        if (layout.verticalScroller) {
            Check(layout.matrixDoc.height + 0.01 >= layout.gridHeight,
                  "a scrolling 32x32 grid is fully contained by its document view");
        }
    }
}

// Font-metric robustness: the real AppKit text heights are only known at run
// time, so the fit has to hold across the plausible range.
void CheckLineHeightSweep() {
    std::printf("\n[line-height sweep 1.15..1.35 at 600x880, n=8]\n");
    double tightest = 1e9;
    for (double factor = 1.15; factor <= 1.351; factor += 0.01) {
        const cl_layout::Metrics metrics =
            MakeMetrics(600.0, 880.0, 8, false, factor, 44.0);
        const cl_layout::Layout layout = cl_layout::Compute(metrics);
        tightest = std::min(tightest, layout.gridSlack);
        Check(!layout.verticalScroller && !layout.horizontalScroller,
              "no scroller at every plausible text height");
        Check(cl_layout::Validate(metrics, layout, false) == 0,
              "layout stays structurally valid at every text height");
    }
    std::printf("  tightest slack %+.1f pt\n", tightest);
    Check(tightest >= 0.0, "the grid fits even with the tallest plausible text");
}

void CheckMinimumWindow() {
    std::printf("\n[minimum window 520x760]\n");
    for (int threads : {2, 8, 16, 32}) {
        const cl_layout::Metrics metrics =
            MakeMetrics(520.0, 760.0, threads, false, 1.32, 44.0);
        const cl_layout::Layout layout = cl_layout::Compute(metrics);
        std::printf("  n=%-2d cell %.1fx%.1f grid %.1fx%.1f regionH %.1f vscroll=%d hscroll=%d\n",
                    threads, layout.cellWidth, layout.cellHeight, layout.gridWidth,
                    layout.gridHeight, layout.gridRegionHeight,
                    layout.verticalScroller ? 1 : 0,
                    layout.horizontalScroller ? 1 : 0);
        Check(cl_layout::Validate(metrics, layout, true) == 0,
              "minimum window layout is structurally valid");
        Check(layout.status.width >= 200.0,
              "the status line keeps a usable width at the minimum size");
        Check(layout.exportButton.MaxX() <= 520.0 - cl_layout::kMargin + 0.01,
              "the button row fits inside the minimum width");
    }
}

void CheckSmtVariant() {
    std::printf("\n[SMT variant: the footnote row must fit too]\n");
    const cl_layout::Metrics metrics =
        MakeMetrics(600.0, 880.0, 8, true, 1.25, 44.0);
    const cl_layout::Layout layout = cl_layout::Compute(metrics);
    std::printf("  footnote (%.1f,%.1f %.1fx%.1f)  grid %.1fx%.1f  slack %+.1f\n",
                layout.footnote.x, layout.footnote.y, layout.footnote.width,
                layout.footnote.height, layout.gridWidth, layout.gridHeight,
                layout.gridSlack);
    Check(!layout.footnote.Empty(), "the SMT footnote is laid out");
    Check(cl_layout::Validate(metrics, layout, true) == 0,
          "SMT layout is structurally valid");
    Check(!layout.verticalScroller,
          "the 8x8 matrix still fits with the footnote row present");
}

}  // namespace

int main(int argc, char** argv) {
    bool verbose = false;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "-v") == 0) {
            verbose = true;
        }
    }

    std::printf("CoherencyLatency 1.2.0 - macOS layout self-test (no AppKit)\n");
    std::printf("default %.0fx%.0f pt (portrait), minimum %.0fx%.0f pt, no thread selector\n",
                cl_layout::kDefaultWidth, cl_layout::kDefaultHeight,
                cl_layout::kMinWidth, cl_layout::kMinHeight);
    std::printf("constants: margin %.0f  sectionGap %.0f  cardPadding %.0f  cellGap %.0f  cell %.0f..%.0f  denseColumns %d\n",
                cl_layout::kMargin, cl_layout::kSectionGap, cl_layout::kCardPadding,
                cl_layout::kCellGap, cl_layout::kCellMin, cl_layout::kCellMax,
                cl_layout::kDenseColumns);

    CheckLockedDesign();
    CheckMatrixFits();
    CheckWideMatrixOverview();
    CheckLineHeightSweep();
    CheckMinimumWindow();
    CheckSmtVariant();

    std::printf("\n[default window, per thread count]\n");
    const Scenario scenarios[] = {
        {600.0, 880.0, 2, false, 1.25, "600x880 n=2"},
        {600.0, 880.0, 4, false, 1.25, "600x880 n=4"},
        {600.0, 880.0, 8, false, 1.25, "600x880 n=8"},
        {600.0, 880.0, 12, false, 1.25, "600x880 n=12"},
        {600.0, 880.0, 16, false, 1.25, "600x880 n=16"},
        {600.0, 880.0, 32, false, 1.25, "600x880 n=32"},
        {520.0, 760.0, 8, false, 1.32, "520x760 n=8"},
        {600.0, 880.0, 8, true, 1.25, "600x880 n=8 SMT"},
    };
    for (const Scenario& scenario : scenarios) {
        RunScenario(scenario, verbose);
    }

    std::printf("\n%d checks, %d failure(s)\n", gChecks, gFailures);
    if (gFailures != 0) {
        std::printf("RESULT: FAIL\n");
        return 1;
    }
    std::printf("RESULT: PASS\n");
    return 0;
}
