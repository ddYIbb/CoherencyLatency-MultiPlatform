// CoherencyLatency 1.2.0 - shared GUI geometry (GUI_SPEC.md v3 section 1).
//
// This header is deliberately free of AppKit.  main.mm measures text with
// AppKit and hands the numbers to cl_layout::Compute(); layout_selftest.cpp
// feeds the same function a font-advance model, so one implementation of the
// layout serves both the real window and an off-host numeric check.  Nothing
// here allocates, draws or touches a window: it is pure arithmetic on the
// content rectangle.
//
// Coordinate system: origin at the top-left of the window content (the views
// are flipped), +y downwards.  Rects for top-level blocks (title, cards,
// buttons, status, progress, footer) are in window space; rects for views that
// are subviews of a card (device title, info rows, matrix title and counter,
// scroll view, legend, statistics, footnote) are in that card's own coordinate
// space, exactly as AppKit wants them.
//
// Locked design (user decision relayed by the Lead, final):
//   * PORTRAIT window, 600 x 880 pt content, minimum 520 x 760;
//   * one vertical stack of full-width blocks: title, device info card, button
//     row, status line, 3 pt progress bar, matrix card, legend/stat/footnote;
//   * no thread selector at all - a run always covers every logical thread;
//   * dense spacing (paddings and gaps trimmed by ~20%, type sizes unchanged);
//   * the matrix card owns all remaining height and the full content width;
//   * from 13 threads on the matrix is a colour overview: no glyphs inside the
//     cells, and the cells shrink until the whole row range fits the card width
//     without a horizontal scroller (the numbers live in the exported CSV).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace cl_layout {

// --- window size (locked design) -------------------------------------------

// 1366x768 / 1280x720 laptops are the low end we must fit, so the default
// content box is 560x640 pt (plus the ~28 pt title bar it stays inside 720) and
// the hard minimum is 440x520 pt.  The layout is unchanged; only the frame the
// user sees at launch is smaller.
constexpr double kDefaultWidth = 560.0;
constexpr double kDefaultHeight = 640.0;
constexpr double kMinWidth = 440.0;
constexpr double kMinHeight = 520.0;

// --- constants shared with main.mm (dense spacing, ~20% below v3) ----------

constexpr double kMargin = 16.0;             // page margin
constexpr double kSectionGap = 10.0;         // gap between stacked blocks
constexpr double kCardPadding = 13.0;        // card inner padding
constexpr double kCellGap = 3.0;             // matrix cell gap
constexpr double kCellMin = 18.0;            // smallest cell before scrolling
constexpr double kCellMax = 120.0;           // widest cell (small thread counts)
constexpr double kColumnHeaderHeight = 12.0; // T0..Tn-1 header row (fixed)
constexpr double kHeaderRowGap = 2.0;        // header row -> grid viewport
constexpr double kMatrixTitleGap = 3.0;      // title row -> grid
constexpr double kGridBottomGap = 3.0;       // grid -> legend row
constexpr double kCountWidth = 64.0;         // "56/56" label on the right
constexpr double kLegendRowHeight = 14.0;
constexpr double kLegendBarWidth = 110.0;
constexpr double kLegendBarHeight = 7.0;
constexpr double kFooterGap = 3.0;
constexpr double kButtonHeight = 28.0;
constexpr double kButtonGap = 7.0;
constexpr double kInfoSpacing = 2.0;
constexpr double kProgressRowHeight = 3.0;   // thin bar (spec: 3 pt)
constexpr double kHeaderGap = 5.0;           // title block -> first card
constexpr double kCardTitleGap = 3.0;        // card title -> its content
constexpr double kButtonToProgress = 3.0;    // buttons -> progress bar
constexpr double kProgressToCard = 5.0;      // progress bar -> matrix card
constexpr double kGridBreathing = 6.0;       // air kept below a fitting grid
constexpr double kStatusButtonGap = 8.0;     // button row -> status line
constexpr double kStatusToProgress = 4.0;    // status line -> progress bar
constexpr double kMinContentWidth = 240.0;
constexpr double kFootnoteGap = 3.0;
// Column density: up to this many threads the grid shows a value in every cell.
// Beyond it the matrix becomes a pure colour overview (the user's words: "this
// chart does not need to be readable, colours give the rough picture; export
// for the details"), so the cells shrink instead of scrolling sideways.
constexpr int kDenseColumns = 12;
// Colour-only cells are sized to fit the card width exactly, so a wide matrix
// never needs a horizontal scroller; this is the smallest cell we allow before
// the gap itself is squeezed.
constexpr double kCellCompactMin = 3.0;
// Tallest a colour-only cell grows: the grid stays a table, not a wall of tiles.
constexpr double kCellCompactMax = 40.0;
// Gaps between colour-only cells shrink with the column count so a 32x32 grid
// still fits the portrait card width.
constexpr double kCellGapCompact = 1.0;
// Widest row-label column in colour-overview mode ("T31" still fits).
constexpr double kCompactLabelWidth = 26.0;
// Below this cell size the T0..Tn-1 axis labels no longer fit without
// overlapping each other, so the grid is drawn as a bare colour map.
constexpr double kAxisLabelMinCell = 12.0;

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

// Everything main.mm has to measure with AppKit before the layout is computed.
// The self-test supplies the same fields from its own font model.  There is
// deliberately no thread-selector field: the control was removed, and the
// self-test asserts that this struct stays free of one.
struct Metrics {
    double width = kDefaultWidth;    // window content width
    double height = kDefaultHeight;  // window content height
    int threads = 8;                 // matrix dimension n (all logical threads)
    bool hasSmt = false;             // footnote row present?
    int infoLineCount = 4;           // device-info rows after wrapping

    // Text heights (ascender - descender + leading, rounded up).
    double titleH = 31.0;
    double subtitleH = 16.0;
    double versionH = 14.0;
    double cardTitleH = 16.0;
    double infoH = 15.0;
    double statusH = 16.0;
    double footerH = 14.0;
    double matrixTitleH = 16.0;
    double matrixCountH = 15.0;
    double footnoteH = 14.0;
    double headerH = 13.0;   // "T0..Tn-1" column/row header text

    // Measured widths of the fixed strings.
    double titleW = 250.0;
    double versionW = 34.0;
    double lowLabelW = 16.0;
    double highLabelW = 16.0;
    double matrixLabelW = 33.0;  // widest row/column header, "T{n-1}"

    // Native control metrics.
    double startButtonW = 110.0;
    double stopButtonW = 80.0;
    double exportButtonW = 104.0;
    double progressH = kProgressRowHeight;
};

struct Layout {
    // Header.
    Rect title;
    Rect subtitle;
    Rect version;

    // Device card: full-width information rows, no selector.
    Rect deviceCard;
    Rect deviceTitle;
    double infoWidth = 0.0;
    double infoFirstY = 0.0;
    double infoLineHeight = 0.0;
    double infoLineStep = 0.0;
    int infoLineCount = 0;

    // Action row, status line, progress bar.
    Rect status;
    Rect startButton;
    Rect stopButton;
    Rect exportButton;
    Rect progress;

    // Footer and matrix card.
    Rect footer;
    Rect matrixCard;
    Rect matrixTitle;
    Rect matrixCount;
    Rect headerRow;       // fixed column-header strip, OUTSIDE the clip
    Rect scrollView;      // viewport: the only region that may scroll
    Rect matrixDoc;       // document view inside the scroll view
    Rect legendLow;
    Rect legendBar;
    Rect legendHigh;
    Rect stats;
    Rect footnote;

    // Matrix geometry.  Cells fill the card width and are capped in height by
    // the space the card can give them, so the grid is a dense heat table
    // rather than a small square floating in dead space.
    double cellWidth = kCellMax;
    double cellHeight = kCellMax;
    double cellGap = kCellGap;
    double labelWidth = 0.0;
    double columnHeaderHeight = kColumnHeaderHeight;
    double gridWidth = 0.0;
    double gridHeight = 0.0;
    double gridRegionHeight = 0.0;
    double cellFontSize = 11.5;
    double headerFontSize = 10.5;
    double gridSlack = 0.0;
    bool verticalScroller = false;
    bool horizontalScroller = false;
    // Colour-only overview: from 13 threads on the cells carry no text at all
    // (the user: "this chart does not need to be readable, colours are enough,
    // export for the details").
    bool showCellValues = true;
    // False when the cells are too small for the T0..Tn-1 axis labels; the grid
    // is then a bare colour map and the numbers come from the CSV.
    bool showAxisLabels = true;

    // Diagnostics for the self-test.
    double contentWidth = 0.0;
    double contentHeight = 0.0;
    double deviceCardHeight = 0.0;
    double matrixCardHeight = 0.0;
    double innerWidth = 0.0;
    double innerHeight = 0.0;
    double progressRowHeight = kProgressRowHeight;
};

// True when the caller asked for a layout dump (CL_GUI_LAYOUT_DUMP=1).  Used by
// the app so the numbers can be inspected on a machine that cannot run AppKit.
inline bool DumpEnabled() {
    const char* value = std::getenv("CL_GUI_LAYOUT_DUMP");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

// Digit size inside a cell: 11.5 pt while there is room, stepping down with
// the cell so an 18 pt cell still shows a readable one-decimal value.
inline double CellFontSize(double cellHeight) {
    if (cellHeight >= 30.0) {
        return 11.5;
    }
    if (cellHeight >= 22.0) {
        return 10.0;
    }
    if (cellHeight >= 18.0) {
        return 9.0;
    }
    return 8.0;
}

inline double Clamp(double value, double low, double high) {
    return (value < low) ? low : ((value > high) ? high : value);
}

// Width the device-info rows wrap into: the full card inner width, because the
// thread selector that used to sit on the right is gone.
inline double InfoWidth(const Metrics& in) {
    const double contentW = std::max(in.width - 2.0 * kMargin, kMinContentWidth);
    return std::max(contentW - 2.0 * kCardPadding, 120.0);
}

// Centre x of one matrix column, in matrix-view coordinates.  The fixed column
// header and the cells are both positioned from this single function, so a
// header can never drift off the column it labels (the user reported exactly
// that misalignment on another platform).
inline double ColumnCenterX(const Layout& out, int column) {
    return out.labelWidth +
           static_cast<double>(column) * (out.cellWidth + out.cellGap) +
           out.cellWidth / 2.0;
}

// Left edge of one matrix column, same coordinate space.
inline double ColumnX(const Layout& out, int column) {
    return out.labelWidth +
           static_cast<double>(column) * (out.cellWidth + out.cellGap);
}

// Computes every sub-rect of the window content.
//
// Matrix sizing (the user's requirement, relayed by the Lead: the matrix is a
// colour overview - "it does not need to be readable, colours give the rough
// picture, export for the details" - and must fill the card width):
//
//   n <= 12 : cellWidth  = clamp(floor(availW / n - 3), 18, 120)
//             cellHeight = clamp(min(cellWidth, floor(availH / n - 3)), 18, 120)
//             a one-decimal value is drawn in every cell
//   n >  12 : cellWidth  = clamp(floor((availW - (n-1)*1) / n), 3, 40)
//             cellHeight = clamp(floor((availH - (n-1)*1) / n), 3, 40)
//             colour blocks only, no glyphs, and the grid always fits the card
//             width so the horizontal scroller never appears
//
// The column-header strip is laid out separately (out.headerRow) and lives
// outside the scrolling viewport, so scrolling can never paint over the card
// title, the pair counter or the headers themselves.
inline Layout Compute(const Metrics& in) {
    Layout out;
    out.contentWidth = std::max(in.width - 2.0 * kMargin, kMinContentWidth);
    out.contentHeight = in.height;
    const int n = std::max(in.threads, 1);

    // 1. Header: the product name with the version flush right, and the subtitle
    //    on its own line underneath (a portrait window is too narrow to set the
    //    subtitle beside the title without crowding it).
    out.title = Rect{kMargin, kMargin, in.titleW, in.titleH};
    out.version = Rect{in.width - kMargin - in.versionW,
                       kMargin + in.titleH - in.versionH - 4.0, in.versionW,
                       in.versionH};
    out.subtitle = Rect{kMargin, out.title.MaxY() + 1.0, out.contentWidth,
                        in.subtitleH};
    double y = out.subtitle.MaxY() + kHeaderGap;

    // 2. Device card: a title row and the information rows, full width.
    out.innerWidth = out.contentWidth - 2.0 * kCardPadding;
    out.infoWidth = InfoWidth(in);
    out.infoLineCount = std::max(in.infoLineCount, 0);
    out.infoLineHeight = in.infoH;
    out.infoLineStep = in.infoH + kInfoSpacing;

    const double infoBlockH =
        static_cast<double>(out.infoLineCount) * in.infoH +
        static_cast<double>(std::max(out.infoLineCount - 1, 0)) * kInfoSpacing;
    const double deviceH = 2.0 * kCardPadding + in.cardTitleH + kCardTitleGap +
                           infoBlockH;
    out.deviceCardHeight = deviceH;
    out.deviceCard = Rect{kMargin, y, out.contentWidth, deviceH};
    out.deviceTitle = Rect{kCardPadding, kCardPadding, out.infoWidth,
                           in.cardTitleH};
    out.infoFirstY = kCardPadding + in.cardTitleH + kCardTitleGap;
    y = out.deviceCard.MaxY() + kSectionGap;

    // 3. Action row: the three native buttons share one full-width row.  In a
    //    portrait window the buttons get the whole width, and the status line
    //    sits on its own line underneath (the order the spec lists).
    out.startButton = Rect{kMargin, y, in.startButtonW, kButtonHeight};
    out.stopButton = Rect{out.startButton.MaxX() + kButtonGap, y, in.stopButtonW,
                          kButtonHeight};
    out.exportButton = Rect{out.stopButton.MaxX() + kButtonGap, y,
                            in.exportButtonW, kButtonHeight};
    y += kButtonHeight + kStatusButtonGap;

    // 4. Status line, then the thin progress bar.
    out.status = Rect{kMargin, y, out.contentWidth, in.statusH};
    y += in.statusH + kStatusToProgress;

    out.progressRowHeight = std::max(kProgressRowHeight, in.progressH);
    out.progress = Rect{kMargin,
                        y + std::floor((out.progressRowHeight - in.progressH) / 2.0),
                        out.contentWidth, in.progressH};
    y += out.progressRowHeight + kProgressToCard;

    // 5. Footer pinned to the bottom.
    out.footer = Rect{kMargin, in.height - kMargin - in.footerH, out.contentWidth,
                      in.footerH};

    // 6. Matrix card takes whatever is left between the progress row and the
    //    footer; it never shrinks below 60 pt.
    const double matrixH = std::max(out.footer.y - kFooterGap - y, 60.0);
    out.matrixCardHeight = matrixH;
    out.matrixCard = Rect{kMargin, y, out.contentWidth, matrixH};

    const double innerW = out.contentWidth - 2.0 * kCardPadding;
    const double innerH = matrixH - 2.0 * kCardPadding;
    out.innerHeight = innerH;
    const double titleRowH = std::max(in.matrixTitleH, in.matrixCountH);
    out.matrixTitle = Rect{kCardPadding, kCardPadding,
                           std::max(innerW - kCountWidth - 8.0, 60.0), titleRowH};
    out.matrixCount = Rect{kCardPadding + innerW - kCountWidth, kCardPadding,
                           kCountWidth, titleRowH};

    const double footnoteH = in.hasSmt ? (in.footnoteH + kFootnoteGap) : 0.0;

    // The column-header strip is a FIXED row: it sits between the card title and
    // the scrolling grid viewport, so scrolling the grid can never paint over
    // "线程延迟矩阵 · ns/op" or the pair counter, and the headers never move.
    const double headerRowH = std::max(kColumnHeaderHeight, in.headerH);
    const double headerTop = kCardPadding + titleRowH + kMatrixTitleGap;
    // A squeezed colour map drops the T0..Tn-1 axis labels entirely: at cell
    // sizes below kAxisLabelMinCell the labels overlap each other, and giving
    // their 26 pt column and 13 pt strip back to the grid is worth more than a
    // row of unreadable numbers.  The CSV carries the values.
    {
        const double probeTop = headerTop + headerRowH + kHeaderRowGap;
        const double probeLegend =
            kCardPadding + innerH - footnoteH - kLegendRowHeight;
        const double probeRegion =
            std::max(probeLegend - kGridBottomGap - probeTop, 0.0);
        const double probeW = std::max(innerW - in.matrixLabelW - 4.0, 0.0);
        const double probeCell = std::floor(std::min(
            probeW / static_cast<double>(n), probeRegion / static_cast<double>(n)));
        out.showAxisLabels = probeCell >= kAxisLabelMinCell;
    }
    out.headerRow = Rect{kCardPadding, headerTop, innerW,
                         out.showAxisLabels ? headerRowH : 0.0};
    const double gridTop = out.headerRow.MaxY() + kHeaderRowGap;

    const double legendTop = kCardPadding + innerH - footnoteH - kLegendRowHeight;
    out.gridRegionHeight = std::max(legendTop - kGridBottomGap - gridTop, 0.0);

    // Matrix sizing.
    //
    //   n <= 12 : a value in every cell; the grid fills the card width and the
    //             cells are at least kCellMin tall so the numbers stay legible.
    //   n >  12 : colour-only overview (the user's request).  The cells shrink
    //             until n columns fit the card width exactly, so a 32x32 grid
    //             never turns on the horizontal scroller; vertical scrolling is
    //             allowed and stays inside the grid viewport.
    out.showCellValues = (n <= kDenseColumns);
    // The row-label column only needs to hold "T{n-1}"; in colour-overview mode
    // it is capped so the cells get the width back.
    out.labelWidth = out.showCellValues
                         ? in.matrixLabelW
                         : std::min(in.matrixLabelW, kCompactLabelWidth);
    if (!out.showAxisLabels) {
        out.labelWidth = 0.0;
    }
    const double availW = std::max(innerW - out.labelWidth - 4.0, 0.0);
    const double availH = std::max(out.gridRegionHeight - kGridBreathing, 0.0);

    const double gap = out.showCellValues ? kCellGap : kCellGapCompact;
    out.cellGap = gap;
    const double columns = static_cast<double>(n);
    if (out.showCellValues) {
        const double widthCell =
            Clamp(std::floor(availW / columns - gap), kCellMin, kCellMax);
        const double heightCell = std::floor(availH / columns - gap);
        out.cellWidth = widthCell;
        out.cellHeight = Clamp(std::min(widthCell, heightCell), kCellMin, kCellMax);
    } else {
        const double widthCell = Clamp(
            std::floor((availW - (columns - 1.0) * gap) / columns),
            kCellCompactMin, kCellCompactMax);
        const double heightCell = Clamp(
            std::floor((availH - (columns - 1.0) * gap) / columns),
            kCellCompactMin, kCellCompactMax);
        out.cellWidth = widthCell;
        out.cellHeight = heightCell;
    }
    out.gridWidth = out.labelWidth + columns * out.cellWidth +
                    (columns - 1.0) * out.cellGap;
    out.gridHeight =
        columns * out.cellHeight + (columns - 1.0) * out.cellGap;
    out.gridSlack = out.gridRegionHeight - out.gridHeight;
    out.cellFontSize = CellFontSize(out.cellHeight);
    out.headerFontSize = 10.5;

    out.verticalScroller = out.gridHeight > out.gridRegionHeight + 0.5;
    out.horizontalScroller = out.gridWidth > innerW + 0.5;
    out.scrollView = Rect{kCardPadding, gridTop, innerW, out.gridRegionHeight};
    out.matrixDoc = Rect{0.0, 0.0, std::max(innerW, out.gridWidth),
                         std::max(out.gridRegionHeight, out.gridHeight)};

    // Legend row: 低 [64-sample heat bar] 高 on the left, statistics flush right.
    const double lowW = in.lowLabelW + 2.0;
    const double highW = in.highLabelW + 2.0;
    out.legendLow = Rect{kCardPadding, legendTop, lowW, kLegendRowHeight};
    out.legendBar = Rect{kCardPadding + lowW + 5.0,
                         legendTop + std::floor((kLegendRowHeight - kLegendBarHeight) / 2.0),
                         kLegendBarWidth, kLegendBarHeight};
    out.legendHigh = Rect{out.legendBar.MaxX() + 5.0, legendTop, highW,
                          kLegendRowHeight};
    const double statsX = out.legendHigh.MaxX() + 10.0;
    out.stats = Rect{statsX, legendTop,
                     std::max(kCardPadding + innerW - statsX, 60.0),
                     kLegendRowHeight};
    out.footnote = in.hasSmt
                       ? Rect{kCardPadding, legendTop + kLegendRowHeight + kFootnoteGap,
                              innerW, footnoteH - kFootnoteGap}
                       : Rect{};
    return out;
}

// Human-readable dump (also used by the app behind CL_GUI_LAYOUT_DUMP=1).
inline void Dump(const Metrics& in, const Layout& out, FILE* stream) {
    if (stream == nullptr) {
        return;
    }
    std::fprintf(stream,
                 "content %.0fx%.0f  n=%d  smt=%d\n"
                 "  header      title (%.1f,%.1f %.1fx%.1f) subtitle (%.1f,%.1f %.1fx%.1f) version (%.1f,%.1f %.1fx%.1f)\n"
                 "  deviceCard  (%.1f,%.1f %.1fx%.1f)  infoW %.1f  lines %d  firstY %.1f step %.1f\n"
                 "  actions     status (%.1f,%.1f %.1fx%.1f) start (%.1f,%.1f %.1fx%.1f) stop (%.1f,%.1f %.1fx%.1f) export (%.1f,%.1f %.1fx%.1f)\n"
                 "  progress    (%.1f,%.1f %.1fx%.1f) row %.1f\n"
                 "  matrixCard  (%.1f,%.1f %.1fx%.1f)  inner %.1fx%.1f\n"
                 "  headerRow   (%.1f,%.1f %.1fx%.1f)  values=%d\n"
                 "  scrollView  (%.1f,%.1f %.1fx%.1f)  doc %.1fx%.1f  vscroll=%d hscroll=%d\n"
                 "  grid        cell %.1fx%.1f gap %.1f label %.1f size %.1fx%.1f regionH %.1f slack %+.1f\n"
                 "  legend      low (%.1f,%.1f %.1fx%.1f) bar (%.1f,%.1f %.1fx%.1f) high (%.1f,%.1f %.1fx%.1f) stats (%.1f,%.1f %.1fx%.1f) footnote (%.1f,%.1f %.1fx%.1f)\n"
                 "  footer      (%.1f,%.1f %.1fx%.1f)  cellFont %.2f  headerFont %.2f\n",
                 in.width, in.height, in.threads, in.hasSmt ? 1 : 0,
                 out.title.x, out.title.y, out.title.width, out.title.height,
                 out.subtitle.x, out.subtitle.y, out.subtitle.width,
                 out.subtitle.height, out.version.x, out.version.y,
                 out.version.width, out.version.height,
                 out.deviceCard.x, out.deviceCard.y, out.deviceCard.width,
                 out.deviceCard.height, out.infoWidth, out.infoLineCount,
                 out.infoFirstY, out.infoLineStep,
                 out.status.x, out.status.y, out.status.width, out.status.height,
                 out.startButton.x, out.startButton.y, out.startButton.width,
                 out.startButton.height, out.stopButton.x, out.stopButton.y,
                 out.stopButton.width, out.stopButton.height, out.exportButton.x,
                 out.exportButton.y, out.exportButton.width, out.exportButton.height,
                 out.progress.x, out.progress.y, out.progress.width,
                 out.progress.height, out.progressRowHeight,
                 out.matrixCard.x, out.matrixCard.y, out.matrixCard.width,
                 out.matrixCard.height, out.contentWidth - 2.0 * kCardPadding,
                 out.innerHeight,
                 out.headerRow.x, out.headerRow.y, out.headerRow.width,
                 out.headerRow.height, out.showCellValues ? 1 : 0,
                 out.scrollView.x, out.scrollView.y, out.scrollView.width,
                 out.scrollView.height, out.matrixDoc.width, out.matrixDoc.height,
                 out.verticalScroller ? 1 : 0, out.horizontalScroller ? 1 : 0,
                 out.cellWidth, out.cellHeight, out.cellGap, out.labelWidth,
                 out.gridWidth, out.gridHeight, out.gridRegionHeight,
                 out.gridSlack,
                 out.legendLow.x, out.legendLow.y, out.legendLow.width,
                 out.legendLow.height, out.legendBar.x, out.legendBar.y,
                 out.legendBar.width, out.legendBar.height, out.legendHigh.x,
                 out.legendHigh.y, out.legendHigh.width, out.legendHigh.height,
                 out.stats.x, out.stats.y, out.stats.width, out.stats.height,
                 out.footnote.x, out.footnote.y, out.footnote.width,
                 out.footnote.height,
                 out.footer.x, out.footer.y, out.footer.width, out.footer.height,
                 out.cellFontSize, out.headerFontSize);
}

// Structural checks every layout must satisfy, independent of font metrics.
inline int Validate(const Metrics& in, const Layout& out, bool verbose) {
    int problems = 0;
    const Rect window{0.0, 0.0, in.width, in.height};
    const Rect* inside[] = {&out.title,       &out.subtitle,   &out.version,
                            &out.deviceCard,  &out.startButton, &out.stopButton,
                            &out.exportButton, &out.status,    &out.progress,
                            &out.matrixCard,  &out.footer};
    for (const Rect* rect : inside) {
        if (!window.Contains(*rect)) {
            if (verbose) {
                std::fprintf(stderr,
                             "  FAIL: rect (%.1f,%.1f %.1fx%.1f) leaves the window\n",
                             rect->x, rect->y, rect->width, rect->height);
            }
            ++problems;
        }
    }
    // Children of the two cards live in card space (see the note at the top).
    const Rect deviceLocal{0.0, 0.0, out.deviceCard.width, out.deviceCard.height};
    const Rect matrixLocal{0.0, 0.0, out.matrixCard.width, out.matrixCard.height};
    if (!deviceLocal.Contains(out.deviceTitle)) {
        if (verbose) std::fprintf(stderr, "  FAIL: device title leaves the card\n");
        ++problems;
    }
    // The device card carries information rows across its whole width: there is
    // no thread-selector column on the right any more.
    if (out.infoWidth < out.deviceCard.width - 2.0 * kCardPadding - 0.01) {
        if (verbose) {
            std::fprintf(stderr,
                         "  FAIL: device info rows %.1f narrower than the card %.1f\n",
                         out.infoWidth, out.deviceCard.width - 2.0 * kCardPadding);
        }
        ++problems;
    }
    if (out.infoWidth > out.deviceCard.width - 2.0 * kCardPadding + 0.01) {
        if (verbose) std::fprintf(stderr, "  FAIL: device info rows overflow the card\n");
        ++problems;
    }

    // Vertical order, top to bottom, with the documented gaps.
    if (out.subtitle.y < out.title.y + out.title.height - 0.01) {
        if (verbose) std::fprintf(stderr, "  FAIL: subtitle overlaps the title\n");
        ++problems;
    }
    if (out.deviceCard.y < out.subtitle.MaxY()) {
        if (verbose) std::fprintf(stderr, "  FAIL: device card overlaps the title\n");
        ++problems;
    }
    if (out.startButton.y < out.deviceCard.MaxY() + kSectionGap - 0.01) {
        if (verbose) std::fprintf(stderr, "  FAIL: action row overlaps the device card\n");
        ++problems;
    }
    if (out.status.y < out.startButton.MaxY() + kStatusButtonGap - 0.01) {
        if (verbose) std::fprintf(stderr, "  FAIL: status line overlaps the button row\n");
        ++problems;
    }
    if (out.progress.y < out.status.MaxY() - 0.01) {
        if (verbose) std::fprintf(stderr, "  FAIL: progress bar overlaps the status line\n");
        ++problems;
    }
    if (out.matrixCard.y < out.progress.MaxY() + kProgressToCard - 0.01) {
        if (verbose) std::fprintf(stderr, "  FAIL: matrix card overlaps the progress bar\n");
        ++problems;
    }
    if (out.matrixCard.MaxY() > out.footer.y - kFooterGap + 0.01) {
        if (verbose) std::fprintf(stderr, "  FAIL: matrix card overlaps the footer\n");
        ++problems;
    }
    if (!matrixLocal.Contains(out.scrollView) ||
        !matrixLocal.Contains(out.headerRow) ||
        !matrixLocal.Contains(out.matrixTitle) ||
        !matrixLocal.Contains(out.matrixCount)) {
        if (verbose) std::fprintf(stderr, "  FAIL: matrix content leaves the matrix card\n");
        ++problems;
    }
    if (!matrixLocal.Contains(out.legendLow) ||
        !matrixLocal.Contains(out.legendBar) ||
        !matrixLocal.Contains(out.legendHigh) ||
        !matrixLocal.Contains(out.stats)) {
        if (verbose) std::fprintf(stderr, "  FAIL: legend/stats leave the matrix card\n");
        ++problems;
    }
    if (!out.status.Empty() && out.status.Overlaps(out.startButton)) {
        if (verbose) std::fprintf(stderr, "  FAIL: status line overlaps 开始测试\n");
        ++problems;
    }
    if (out.stopButton.x < out.startButton.MaxX() + kButtonGap - 0.01 ||
        out.exportButton.x < out.stopButton.MaxX() + kButtonGap - 0.01) {
        if (verbose) std::fprintf(stderr, "  FAIL: button row overlaps itself\n");
        ++problems;
    }
    const double cellLow = out.showCellValues ? kCellMin : kCellCompactMin;
    const double cellHigh = out.showCellValues ? kCellMax : kCellCompactMax;
    if (out.cellWidth < cellLow - 0.01 || out.cellWidth > cellHigh + 0.01 ||
        out.cellHeight < cellLow - 0.01 || out.cellHeight > cellHigh + 0.01) {
        if (verbose) {
            std::fprintf(stderr, "  FAIL: cell %.2fx%.2f outside clamp(%.0f..%.0f)\n",
                         out.cellWidth, out.cellHeight, cellLow, cellHigh);
        }
        ++problems;
    }
    // Colour-overview mode must never need a horizontal scroller: the whole
    // wide grid has to fit the card width.
    if (!out.showCellValues && out.horizontalScroller) {
        if (verbose) {
            std::fprintf(stderr,
                         "  FAIL: colour-overview grid %.1f wider than the card %.1f\n",
                         out.gridWidth, out.scrollView.width);
        }
        ++problems;
    }
    // Column headers must be centred on their columns: the spacing between
    // consecutive centres is exactly one pitch, and the last centre is half a
    // cell inside the right edge of the grid (gridWidth already includes the row
    // label column).
    if (in.threads >= 2) {
        const double pitch = out.cellWidth + out.cellGap;
        const double first = ColumnCenterX(out, 0);
        const double last = ColumnCenterX(out, in.threads - 1);
        if (std::fabs((last - first) - (in.threads - 1) * pitch) > 0.01 ||
            std::fabs((out.gridWidth - out.cellWidth / 2.0) - last) > 0.01) {
            if (verbose) {
                std::fprintf(stderr,
                             "  FAIL: column headers are not centred on their columns\n");
            }
            ++problems;
        }
    }

    // The fixed header strip is never painted over by the scrolling grid.
    if (out.headerRow.MaxY() > out.scrollView.y + 0.01) {
        if (verbose) std::fprintf(stderr, "  FAIL: grid viewport overlaps the header row\n");
        ++problems;
    }
    if (out.headerRow.y < kCardPadding + out.matrixTitle.height - 0.01) {
        if (verbose) std::fprintf(stderr, "  FAIL: header row overlaps the card title\n");
        ++problems;
    }
    // No clipping, ever: the document view is at least as large as the grid,
    // and a grid larger than the viewport always turns its scroller on.
    if (out.matrixDoc.width + 0.01 < out.gridWidth ||
        out.matrixDoc.height + 0.01 < out.gridHeight) {
        if (verbose) std::fprintf(stderr, "  FAIL: document view clips the grid\n");
        ++problems;
    }
    if (out.gridWidth > out.scrollView.width + 0.5 && !out.horizontalScroller) {
        if (verbose) std::fprintf(stderr, "  FAIL: grid wider than viewport, no scroller\n");
        ++problems;
    }
    if (out.gridHeight > out.scrollView.height + 0.5 && !out.verticalScroller) {
        if (verbose) std::fprintf(stderr, "  FAIL: grid taller than viewport, no scroller\n");
        ++problems;
    }
    // The scroll view is the only scrolling region: the card content above it
    // (title row, counter) and below it (legend, footnote) must stay visible.
    if (out.scrollView.y < kCardPadding - 0.01) {
        if (verbose) std::fprintf(stderr, "  FAIL: scroll view starts above the card padding\n");
        ++problems;
    }
    if (out.scrollView.MaxY() > out.legendBar.y - kGridBottomGap + 0.01) {
        if (verbose) std::fprintf(stderr, "  FAIL: grid region overlaps the legend row\n");
        ++problems;
    }
    if (in.hasSmt && out.footnote.MaxY() > matrixLocal.height - kCardPadding + 0.01) {
        if (verbose) std::fprintf(stderr, "  FAIL: footnote leaves the matrix card\n");
        ++problems;
    }
    return problems;
}

}  // namespace cl_layout
