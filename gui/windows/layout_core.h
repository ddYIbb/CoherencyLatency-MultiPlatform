// CoherencyLatency 1.2.0 - Windows desktop GUI geometry (GUI_SPEC.md v3, section 1).
//
// Pure arithmetic on the content rectangle, in device-independent points
// (1 pt == 1 logical pixel at 96 dpi).  main.cpp measures text with GDI, converts
// to points, calls Compute() and scales the resulting rectangles back to device
// pixels; layout_selftest.cpp feeds the same function a GDI font model at 96 dpi
// so the numbers can be checked without showing a window.  Nothing here touches
// a window, a DC or the engine.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace clwin {

// --- constants (GUI_SPEC.md section 1) -------------------------------------
constexpr double kMargin = 20.0;            // page margin
constexpr double kSectionGap = 12.0;        // gap between stacked cards
constexpr double kCardPadding = 16.0;       // card inner padding
constexpr double kCardRadius = 8.0;         // Fluent card corner radius
constexpr double kCellRadius = 4.0;         // matrix cell corner radius
constexpr double kCellGap = 3.0;            // matrix cell gap
constexpr double kCellMin = 18.0;           // smallest preferred cell
constexpr double kCellHardMin = 14.0;       // relaxed floor, used before any scroller
constexpr double kCellMax = 40.0;           // largest cell
constexpr double kColumnHeaderHeight = 14.0;  // T0..Tn-1 header row inside the matrix child
constexpr double kMatrixTitleGap = 4.0;     // matrix title row -> grid
constexpr double kGridBottomGap = 2.0;      // grid region -> legend row
constexpr double kGridBreathing = 20.0;     // air kept below a grid that already fits
constexpr double kLegendRowHeight = 16.0;
constexpr double kLegendBarHeight = 8.0;
constexpr double kLegendBarWidth = 160.0;
constexpr double kFootnoteGap = 2.0;
constexpr double kFooterGap = 4.0;
constexpr double kButtonHeight = 34.0;
constexpr double kButtonGap = 8.0;
constexpr double kStatusGap = 2.0;          // buttons -> status line
constexpr double kStatusToProgress = 2.0;   // status line -> progress bar
constexpr double kProgressToCard = 4.0;     // progress bar -> matrix card
constexpr double kHeaderGap = 6.0;          // title block -> device card
constexpr double kCardTitleGap = 4.0;       // card title -> its content
constexpr double kInfoSpacing = 2.0;
constexpr double kThreadGap = 4.0;
constexpr double kMinMatrixHeight = 60.0;
constexpr double kMinContentWidth = 240.0;
constexpr double kCountWidth = 64.0;
constexpr double kScrollBarSize = 4.0;      // thin Fluent scroll indicator

struct Rect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    double MaxX() const { return x + width; }
    double MaxY() const { return y + height; }
    bool Empty() const { return width <= 0.0 || height <= 0.0; }
    bool Contains(const Rect& other) const {
        return other.x >= x - 0.01 && other.y >= y - 0.01 &&
               other.MaxX() <= MaxX() + 0.01 && other.MaxY() <= MaxY() + 0.01;
    }
    bool Overlaps(const Rect& other) const {
        return !(other.x >= MaxX() - 0.01 || other.MaxX() <= x + 0.01 ||
                 other.y >= MaxY() - 0.01 || other.MaxY() <= y + 0.01);
    }
};

// Everything main.cpp has to measure with GDI before the layout is computed.
// The self-test fills the same fields from a 96 dpi font model.
struct Metrics {
    double width = 760.0;      // client width in pt
    double height = 620.0;     // client height in pt
    int threads = 8;           // matrix dimension n
    bool hasSmt = false;       // footnote row present?
    int infoLineCount = 4;     // device-info rows after wrapping

    // Text heights (tmHeight scaled to points, rounded up).
    double titleH = 35.0;
    double subtitleH = 17.0;
    double versionH = 15.0;
    double cardTitleH = 18.0;
    double infoH = 16.0;
    double hintH = 15.0;
    double statusH = 17.0;
    double footerH = 15.0;
    double matrixTitleH = 19.0;
    double countH = 16.0;
    double legendH = 15.0;
    double statsH = 16.0;
    double footnoteH = 15.0;
    double threadLabelH = 16.0;
    double labelH = 14.0;         // matrix row/column header text height

    // Measured widths of the fixed strings.
    double titleW = 250.0;
    double subtitleW = 280.0;
    double versionW = 30.0;
    double threadLabelW = 40.0;
    double threadHintW = 100.0;
    double lowW = 16.0;
    double highW = 16.0;
    double matrixLabelW = 26.0;   // widest row/column header, e.g. "T31" / "T7*"
    double unitW = 30.0;          // "ns/op"
    double statsW = 210.0;        // "平均 97.9 · 最小 24.5 · 最大 126.6 ns"

    // Native control metrics.
    double comboW = 96.0;
    double comboH = 30.0;
    double startW = 150.0;
    double stopW = 96.0;
    double exportW = 112.0;
    double progressH = 8.0;

    // Width of the widest cell value ("888.8") at each mono size, so the digits
    // can never be clipped; index 0 = 11.5 pt ... index 3 = 8 pt.
    double cellSampleW[4] = {34.0, 30.0, 27.0, 24.0};
};

struct Layout {
    // Window-space rectangles (points).
    Rect title;
    Rect subtitle;
    Rect version;
    Rect deviceCard;
    Rect startButton;
    Rect stopButton;
    Rect exportButton;
    Rect status;
    Rect progress;
    Rect footer;
    Rect matrixCard;

    // Card-space childrens' rectangles, already converted to window space.
    Rect deviceTitle;
    Rect threadLabel;
    Rect combo;
    Rect hint;
    Rect matrixTitle;
    Rect matrixUnit;
    Rect matrixCount;
    Rect matrixChild;   // the only scrolling region
    Rect legendLow;
    Rect legendBar;
    Rect legendHigh;
    Rect stats;
    Rect footnote;

    // Device info block (rows are laid out by the adapter from these).
    Rect infoBlock;
    int infoColumns = 2;
    int infoRows = 2;
    double infoLineStep = 0.0;

    // Matrix geometry, in the matrix child's client coordinates.
    double cell = 26.0;
    double gap = kCellGap;
    double labelW = 26.0;
    double headerH = kColumnHeaderHeight;
    double gridW = 0.0;
    double gridH = 0.0;
    double gridRegionH = 0.0;
    double gridSlack = 0.0;
    double maxScrollX = 0.0;
    double maxScrollY = 0.0;
    double sbW = 0.0;   // reserved vertical scrollbar strip
    double sbH = 0.0;   // reserved horizontal scrollbar strip
    double gridOffsetX = 0.0;
    double gridOffsetY = 0.0;
    bool verticalScroller = false;
    bool horizontalScroller = false;
    double cellFontSize = 11.5;

    // Diagnostics.
    double contentW = 0.0;
    double contentH = 0.0;
    double deviceCardH = 0.0;
    double matrixCardH = 0.0;
    double innerW = 0.0;
    double innerH = 0.0;
    double naturalCell = 0.0;
};

inline double Clamp(double value, double low, double high) {
    return (value < low) ? low : ((value > high) ? high : value);
}

// Digit size inside a cell: 11.5 pt while there is room, stepping down with the
// cell so a 14 pt cell still shows a readable one-decimal value.
inline double CellFontSize(double cellSize) {
    if (cellSize >= 30.0) {
        return 11.5;
    }
    if (cellSize >= 22.0) {
        return 10.0;
    }
    if (cellSize >= 18.0) {
        return 9.0;
    }
    return 8.0;
}

inline int CellFontIndex(double cellSize) {
    if (cellSize >= 30.0) {
        return 0;
    }
    if (cellSize >= 22.0) {
        return 1;
    }
    if (cellSize >= 18.0) {
        return 2;
    }
    return 3;
}

// Computes every sub-rectangle of the window content, in points.  The formulas
// are GUI_SPEC.md section 1:
//
//   availW = card inner width - row label width - 4
//   availH = card inner height - title row - header row - legend row
//            - statistics row - footnote row - gaps
//   cell   = clamp(18, min(availW / n - gap, availH / n - gap), 40)
//
// and a scroller appears only when the clamped cell cannot fit (n is large).
inline Layout Compute(const Metrics& in) {
    Layout out;
    out.contentW = std::max(in.width - 2.0 * kMargin, kMinContentWidth);
    out.contentH = in.height;
    const int n = std::max(in.threads, 1);
    const double contentX = (in.width - out.contentW) * 0.5;

    // 1. Header: product name, inline subtitle, version on the same line.
    const double headLineH = std::max(in.titleH, std::max(in.subtitleH, in.versionH));
    out.title = Rect{contentX, kMargin, in.titleW, in.titleH};
    const double subtitleX = contentX + in.titleW + 12.0;
    const double versionX = in.width - kMargin - in.versionW;
    const double subtitleRight = std::max(versionX - 8.0, subtitleX + 40.0);
    out.subtitle = Rect{subtitleX, kMargin + headLineH - in.subtitleH,
                        subtitleRight - subtitleX, in.subtitleH};
    out.version = Rect{versionX, kMargin + headLineH - in.versionH, in.versionW,
                       in.versionH};
    double y = kMargin + headLineH + kHeaderGap;

    // 2. Device card: info rows on the left, thread selector on the right.
    out.innerW = out.contentW - 2.0 * kCardPadding;
    const double threadColW = std::max(std::max(in.comboW, in.threadHintW),
                                       in.threadLabelW);
    const double columnGap = 24.0;
    const double infoW = std::max(out.innerW - threadColW - columnGap, 120.0);
    out.infoColumns = (infoW >= 330.0) ? 2 : 1;
    const int lines = std::max(in.infoLineCount, 0);
    out.infoRows = (lines + out.infoColumns - 1) / std::max(out.infoColumns, 1);
    const double infoBlockH =
        (out.infoRows > 0)
            ? static_cast<double>(out.infoRows) * in.infoH +
                  static_cast<double>(out.infoRows - 1) * kInfoSpacing
            : 0.0;
    const double leftH = in.cardTitleH + kCardTitleGap + infoBlockH;
    const double threadBlockH = in.threadLabelH + kThreadGap + in.comboH +
                                kThreadGap + in.hintH;
    const double deviceInnerH = std::max(leftH, threadBlockH);
    out.deviceCardH = deviceInnerH + 2.0 * kCardPadding;
    out.deviceCard = Rect{contentX, y, out.contentW, out.deviceCardH};

    out.deviceTitle = Rect{contentX + kCardPadding, y + kCardPadding, infoW,
                           in.cardTitleH};
    out.infoBlock = Rect{contentX + kCardPadding,
                         y + kCardPadding + in.cardTitleH + kCardTitleGap, infoW,
                         infoBlockH};
    out.infoLineStep = in.infoH + kInfoSpacing;

    const double threadX = contentX + out.contentW - kCardPadding - threadColW;
    const double threadY = y + kCardPadding + (deviceInnerH - threadBlockH) * 0.5;
    out.threadLabel = Rect{threadX, threadY, threadColW, in.threadLabelH};
    out.combo = Rect{threadX + threadColW - in.comboW,
                     out.threadLabel.MaxY() + kThreadGap, in.comboW, in.comboH};
    out.hint = Rect{threadX, out.combo.MaxY() + kThreadGap, threadColW, in.hintH};
    y = out.deviceCard.MaxY() + kSectionGap;

    // 3. Action row: the three native buttons, left aligned like a Windows
    //    command bar; the status line sits underneath.
    out.startButton = Rect{contentX, y, in.startW, kButtonHeight};
    out.stopButton = Rect{out.startButton.MaxX() + kButtonGap, y, in.stopW,
                          kButtonHeight};
    out.exportButton = Rect{out.stopButton.MaxX() + kButtonGap, y, in.exportW,
                            kButtonHeight};
    y += kButtonHeight + kStatusGap;

    // 4. Status line and progress bar, both full width.
    out.status = Rect{contentX, y, out.contentW, in.statusH};
    y += in.statusH + kStatusToProgress;
    out.progress = Rect{contentX, y, out.contentW, std::max(in.progressH, 4.0)};
    y += out.progress.height + kProgressToCard;

    // 5. Footer pinned to the bottom.
    out.footer = Rect{contentX, in.height - kMargin - in.footerH, out.contentW,
                      in.footerH};

    // 6. Matrix card takes whatever is left.
    out.matrixCardH = std::max(out.footer.y - kFooterGap - y, kMinMatrixHeight);
    out.matrixCard = Rect{contentX, y, out.contentW, out.matrixCardH};

    const double cardX = out.matrixCard.x;
    const double cardY = out.matrixCard.y;
    const double cardInnerW = out.contentW - 2.0 * kCardPadding;
    const double cardInnerH = out.matrixCardH - 2.0 * kCardPadding;
    out.innerH = cardInnerH;
    const double titleRowH = std::max(in.matrixTitleH, in.countH);
    out.matrixTitle = Rect{cardX + kCardPadding, cardY + kCardPadding,
                           std::max(cardInnerW - kCountWidth - in.unitW - 16.0, 80.0),
                           titleRowH};
    out.matrixUnit = Rect{out.matrixTitle.MaxX() + 8.0,
                          cardY + kCardPadding + titleRowH - in.countH,
                          in.unitW, in.countH};
    out.matrixCount = Rect{cardX + out.contentW - kCardPadding - kCountWidth,
                           cardY + kCardPadding + titleRowH - in.countH,
                           kCountWidth, in.countH};

    const double cardInnerBottom = cardY + out.matrixCardH - kCardPadding;
    const double footnoteH = in.hasSmt ? (in.footnoteH + kFootnoteGap) : 0.0;
    out.footnote = in.hasSmt
                       ? Rect{cardX + kCardPadding, cardInnerBottom - in.footnoteH,
                              cardInnerW, in.footnoteH}
                       : Rect{cardX + kCardPadding, cardInnerBottom, cardInnerW, 0.0};
    const double legendTop = cardInnerBottom - footnoteH - kLegendRowHeight;
    const double gridTop = cardY + kCardPadding + titleRowH + kMatrixTitleGap;
    out.gridRegionH = std::max(legendTop - kGridBottomGap - gridTop, 0.0);
    out.matrixChild = Rect{cardX + kCardPadding, gridTop, cardInnerW,
                           out.gridRegionH};

    out.legendLow = Rect{cardX + kCardPadding,
                         legendTop + (kLegendRowHeight - in.legendH) * 0.5, in.lowW,
                         in.legendH};
    out.legendBar = Rect{out.legendLow.MaxX() + 6.0,
                         legendTop + (kLegendRowHeight - kLegendBarHeight) * 0.5,
                         kLegendBarWidth, kLegendBarHeight};
    out.legendHigh = Rect{out.legendBar.MaxX() + 6.0,
                          legendTop + (kLegendRowHeight - in.legendH) * 0.5, in.highW,
                          in.legendH};
    out.stats = Rect{cardX + kCardPadding, legendTop + (kLegendRowHeight - in.statsH) * 0.5,
                     cardInnerW, in.statsH};

    // 7. Grid geometry: two-way cell constraint.
    out.labelW = in.matrixLabelW;
    out.headerH = std::max(kColumnHeaderHeight, in.labelH);
    const double gap = kCellGap;
    const double availW = std::max(cardInnerW - out.labelW - 4.0, 0.0);
    const double availH = std::max(out.gridRegionH - out.headerH - kGridBreathing, 0.0);
    const double nf = static_cast<double>(n);
    out.naturalCell = std::min(availW / nf - gap, availH / nf - gap);

    const auto heightFor = [&](double cellSize) {
        return out.headerH + nf * cellSize + (nf - 1.0) * gap;
    };
    const auto widthFor = [&](double cellSize) {
        return out.labelW + nf * cellSize + (nf - 1.0) * gap;
    };

    double cell = std::floor(Clamp(out.naturalCell, kCellMin, kCellMax));
    if (heightFor(cell) > out.gridRegionH + 0.5 ||
        widthFor(cell) > cardInnerW + 0.5) {
        const double relaxed =
            std::floor(Clamp(out.naturalCell, kCellHardMin, kCellMax));
        if (heightFor(relaxed) <= out.gridRegionH + 0.5 &&
            widthFor(relaxed) <= cardInnerW + 0.5) {
            cell = relaxed;
        }
    }
    out.cell = cell;
    out.gap = gap;
    out.gridW = widthFor(cell);
    out.gridH = heightFor(cell);
    out.gridSlack = out.gridRegionH - out.gridH;
    out.verticalScroller = out.gridH > out.gridRegionH + 0.5;
    out.horizontalScroller = out.gridW > cardInnerW + 0.5;
    out.maxScrollY = std::max(0.0, out.gridH - out.gridRegionH);
    out.maxScrollX = std::max(0.0, out.gridW - cardInnerW);
    out.sbW = out.verticalScroller ? kScrollBarSize : 0.0;
    out.sbH = out.horizontalScroller ? kScrollBarSize : 0.0;
    out.cellFontSize = CellFontSize(cell);

    // The grid is centred in the viewport (it is height-bound, so the leftover
    // width belongs on both sides).
    out.gridOffsetX = std::max(0.0, (cardInnerW - out.labelW - out.gridW) * 0.5);

    // Cell font: the largest of 11.5 / 10 / 9 / 8 pt whose sample still fits.
    const int fontIndex = CellFontIndex(cell);
    const double sampleW = in.cellSampleW[fontIndex];
    out.gridOffsetY = 0.0;
    if ((cell - 6.0) < sampleW) {
        out.cellFontSize = 8.0;
    }
    return out;
}

// Structural checks that must hold for every font metric set.  Returns the
// number of problems; verbose prints each one.
inline int Validate(const Metrics& in, const Layout& out, bool verbose) {
    int problems = 0;
    const Rect window{0.0, 0.0, in.width, in.height};
    const Rect* topLevel[] = {&out.title,     &out.subtitle,   &out.version,
                              &out.deviceCard, &out.startButton, &out.stopButton,
                              &out.exportButton, &out.status,    &out.progress,
                              &out.matrixCard, &out.footer};
    for (const Rect* rect : topLevel) {
        if (!window.Contains(*rect)) {
            if (verbose) {
                std::fprintf(stderr,
                             "  FAIL: rect (%.1f,%.1f %.1fx%.1f) leaves the window\n",
                             rect->x, rect->y, rect->width, rect->height);
            }
            ++problems;
        }
    }
    const Rect* deviceChildren[] = {&out.deviceTitle, &out.threadLabel, &out.combo,
                                    &out.hint, &out.infoBlock};
    for (const Rect* child : deviceChildren) {
        if (!out.deviceCard.Contains(*child)) {
            if (verbose) {
                std::fprintf(stderr,
                             "  FAIL: device child (%.1f,%.1f %.1fx%.1f) leaves the card\n",
                             child->x, child->y, child->width, child->height);
            }
            ++problems;
        }
    }
    const Rect* matrixChildren[] = {&out.matrixTitle, &out.matrixUnit, &out.matrixCount,
                                    &out.matrixChild, &out.legendLow, &out.legendBar,
                                    &out.legendHigh, &out.stats};
    for (const Rect* child : matrixChildren) {
        if (!out.matrixCard.Contains(*child)) {
            if (verbose) {
                std::fprintf(stderr,
                             "  FAIL: matrix child (%.1f,%.1f %.1fx%.1f) leaves the card\n",
                             child->x, child->y, child->width, child->height);
            }
            ++problems;
        }
    }
    if (in.hasSmt && !out.footnote.Empty() && !out.matrixCard.Contains(out.footnote)) {
        if (verbose) std::fprintf(stderr, "  FAIL: footnote leaves the matrix card\n");
        ++problems;
    }
    // Vertical order and gaps.
    if (out.deviceCard.y < out.title.y + out.title.height - 0.01) {
        if (verbose) std::fprintf(stderr, "  FAIL: device card overlaps the title\n");
        ++problems;
    }
    if (out.startButton.y < out.deviceCard.MaxY() + kSectionGap - 0.01) {
        if (verbose) std::fprintf(stderr, "  FAIL: buttons overlap the device card\n");
        ++problems;
    }
    if (out.status.y < out.startButton.MaxY() - 0.01) {
        if (verbose) std::fprintf(stderr, "  FAIL: status line overlaps the buttons\n");
        ++problems;
    }
    if (out.progress.y < out.status.MaxY() - 0.01) {
        if (verbose) std::fprintf(stderr, "  FAIL: progress bar overlaps the status line\n");
        ++problems;
    }
    if (out.matrixCard.y < out.progress.MaxY() - 0.01) {
        if (verbose) std::fprintf(stderr, "  FAIL: matrix card overlaps the progress bar\n");
        ++problems;
    }
    if (out.matrixCard.MaxY() > out.footer.y - kFooterGap + 0.01) {
        if (verbose) std::fprintf(stderr, "  FAIL: matrix card overlaps the footer\n");
        ++problems;
    }
    if (out.status.Overlaps(out.startButton) || out.status.Overlaps(out.stopButton) ||
        out.status.Overlaps(out.exportButton)) {
        if (verbose) std::fprintf(stderr, "  FAIL: status line overlaps a button\n");
        ++problems;
    }
    // Matrix internals: the grid region may not run into the legend row, and
    // the title row may not run into the grid.
    if (out.matrixChild.MaxY() > out.legendBar.y - kGridBottomGap + 0.01) {
        if (verbose) std::fprintf(stderr, "  FAIL: grid region overlaps the legend\n");
        ++problems;
    }
    if (out.matrixTitle.MaxY() > out.matrixChild.y + 0.01) {
        if (verbose) std::fprintf(stderr, "  FAIL: matrix title overlaps the grid region\n");
        ++problems;
    }
    if (out.cell < kCellHardMin - 0.01 || out.cell > kCellMax + 0.01) {
        if (verbose) {
            std::fprintf(stderr, "  FAIL: cell %.2f outside clamp(%.0f..%.0f)\n",
                         out.cell, kCellHardMin, kCellMax);
        }
        ++problems;
    }
    if (!out.verticalScroller && out.gridH > out.gridRegionH + 0.5) {
        if (verbose) std::fprintf(stderr, "  FAIL: grid taller than viewport, no scroller\n");
        ++problems;
    }
    if (out.verticalScroller && out.gridH <= out.gridRegionH + 0.5) {
        if (verbose) std::fprintf(stderr, "  FAIL: scroller shown although the grid fits\n");
        ++problems;
    }
    if (out.horizontalScroller && out.gridW <= out.innerW + 0.5) {
        if (verbose) std::fprintf(stderr, "  FAIL: h-scroller shown although the grid fits\n");
        ++problems;
    }
    return problems;
}

// One-line-per-scenario table row plus the detail dump.
inline void DumpRow(const Metrics& in, const Layout& out, FILE* stream) {
    std::fprintf(stream,
                 "%.0fx%.0f n=%-2d smt=%d cell=%5.1f grid=%6.1fx%6.1f region=%5.1f "
                 "slack=%+6.1f v=%s h=%s cellFont=%4.1f cardH=%5.1f\n",
                 in.width, in.height, in.threads, in.hasSmt ? 1 : 0, out.cell,
                 out.gridW, out.gridH, out.gridRegionH, out.gridSlack,
                 out.verticalScroller ? "yes" : "no",
                 out.horizontalScroller ? "yes" : "no", out.cellFontSize,
                 out.matrixCardH);
}

inline void Dump(const Metrics& in, const Layout& out, FILE* stream) {
    if (stream == nullptr) {
        return;
    }
    std::fprintf(stream,
                 "content %.0fx%.0f n=%d smt=%d\n"
                 "  title (%.1f,%.1f %.1fx%.1f) subtitle (%.1f,%.1f %.1fx%.1f) version (%.1f,%.1f %.1fx%.1f)\n"
                 "  device (%.1f,%.1f %.1fx%.1f) title (%.1f,%.1f %.1fx%.1f) info (%.1f,%.1f %.1fx%.1f) cols %d rows %d\n"
                 "  thread label (%.1f,%.1f %.1fx%.1f) combo (%.1f,%.1f %.1fx%.1f) hint (%.1f,%.1f %.1fx%.1f)\n"
                 "  buttons start (%.1f,%.1f %.1fx%.1f) stop (%.1f,%.1f %.1fx%.1f) export (%.1f,%.1f %.1fx%.1f)\n"
                 "  status (%.1f,%.1f %.1fx%.1f) progress (%.1f,%.1f %.1fx%.1f) footer (%.1f,%.1f %.1fx%.1f)\n"
                 "  matrixCard (%.1f,%.1f %.1fx%.1f) title (%.1f,%.1f %.1fx%.1f) unit (%.1f,%.1f %.1fx%.1f) count (%.1f,%.1f %.1fx%.1f)\n"
                 "  child (%.1f,%.1f %.1fx%.1f) legend low (%.1f,%.1f %.1fx%.1f) bar (%.1f,%.1f %.1fx%.1f) high (%.1f,%.1f %.1fx%.1f) stats (%.1f,%.1f %.1fx%.1f)\n"
                 "  footnote (%.1f,%.1f %.1fx%.1f)\n"
                 "  grid cell %.1f gap %.1f label %.1f header %.1f size %.1fx%.1f slack %+.1f v=%d h=%d\n",
                 in.width, in.height, in.threads, in.hasSmt ? 1 : 0, out.title.x,
                 out.title.y, out.title.width, out.title.height, out.subtitle.x,
                 out.subtitle.y, out.subtitle.width, out.subtitle.height,
                 out.version.x, out.version.y, out.version.width, out.version.height,
                 out.deviceCard.x, out.deviceCard.y, out.deviceCard.width,
                 out.deviceCard.height, out.deviceTitle.x, out.deviceTitle.y,
                 out.deviceTitle.width, out.deviceTitle.height, out.infoBlock.x,
                 out.infoBlock.y, out.infoBlock.width, out.infoBlock.height,
                 out.infoColumns, out.infoRows, out.threadLabel.x, out.threadLabel.y,
                 out.threadLabel.width, out.threadLabel.height, out.combo.x, out.combo.y,
                 out.combo.width, out.combo.height, out.hint.x, out.hint.y,
                 out.hint.width, out.hint.height, out.startButton.x, out.startButton.y,
                 out.startButton.width, out.startButton.height, out.stopButton.x,
                 out.stopButton.y, out.stopButton.width, out.stopButton.height,
                 out.exportButton.x, out.exportButton.y, out.exportButton.width,
                 out.exportButton.height, out.status.x, out.status.y, out.status.width,
                 out.status.height, out.progress.x, out.progress.y, out.progress.width,
                 out.progress.height, out.footer.x, out.footer.y, out.footer.width,
                 out.footer.height, out.matrixCard.x, out.matrixCard.y,
                 out.matrixCard.width, out.matrixCard.height, out.matrixTitle.x,
                 out.matrixTitle.y, out.matrixTitle.width, out.matrixTitle.height,
                 out.matrixUnit.x, out.matrixUnit.y, out.matrixUnit.width,
                 out.matrixUnit.height, out.matrixCount.x, out.matrixCount.y,
                 out.matrixCount.width, out.matrixCount.height, out.matrixChild.x,
                 out.matrixChild.y, out.matrixChild.width, out.matrixChild.height,
                 out.legendLow.x, out.legendLow.y, out.legendLow.width,
                 out.legendLow.height, out.legendBar.x, out.legendBar.y,
                 out.legendBar.width, out.legendBar.height, out.legendHigh.x,
                 out.legendHigh.y, out.legendHigh.width, out.legendHigh.height,
                 out.stats.x, out.stats.y, out.stats.width, out.stats.height,
                 out.footnote.x, out.footnote.y, out.footnote.width,
                 out.footnote.height, out.cell, out.gap, out.labelW, out.headerH,
                 out.gridW, out.gridH, out.gridSlack, out.verticalScroller ? 1 : 0,
                 out.horizontalScroller ? 1 : 0);
}

}  // namespace clwin
