// CoherencyLatency 1.2.0 - macOS desktop GUI front end (native AppKit).
//
// GUI_SPEC.md v3 section 3: the macOS front end is a *native* Mac application,
// not a cross-platform skin:
//   * fonts      [NSFont systemFontOfSize:weight:] and
//                monospacedDigitSystemFontOfSize:weight: (SF Pro + PingFang).
//   * colours    semantic NSColor only: label/secondary/tertiary/quaternary,
//                controlBackground, separator, controlAccent, systemRed.
//                No hard-coded greys, so dark mode and the user's contrast
//                settings keep working.  Heat colours are *data* colours and
//                stay hard-coded (GUI_SPEC.md section 4).
//   * controls   real NSButton (rounded bezel, accent bezelColor) and a real
//                NSProgressIndicator (bar, determinate).  There is no thread
//                selector: like the Android and HarmonyOS front ends, a run
//                always measures every logical thread (user decision on
//                2026-11-24, which supersedes the selector in spec section 0).
//   * chrome     standard NSWindow with a transparent titlebar over an
//                NSVisualEffectView material.
//   * menus      App / 编辑 / 测试 / 窗口, with Cmd-R, Cmd-., Cmd-S, Cmd-Q.
//
// Shared information skeleton (GUI_SPEC.md section 0) is identical to the
// Windows front end; only the presentation is Mac-native.
//
// Threading (GUI_SPEC.md section 5): cl_run_matrix() runs on a std::thread and
// never touches AppKit.  The progress callback copies its POD snapshot and hops
// to the main queue with dispatch_async(); the main thread applies the update.
// There is no timer and no polling - the status line changes exactly once per
// finished thread pair (plus once when a run starts and once when it ends).

#import <Cocoa/Cocoa.h>
#import <objc/message.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../engine_api.h"
#include "cl_layout.h"

// ---------------------------------------------------------------------------
// Compile-time facts
// ---------------------------------------------------------------------------

// engine_api.cpp reports "arm64" for every __APPLE__ build, which is wrong for
// the x86_64 slice of the universal binary.  The slice is a compile-time fact,
// so the architecture label is derived here instead.
#if defined(__aarch64__) || defined(__arm64__)
static const char* const kArchName = "arm64";
#elif defined(__x86_64__)
static const char* const kArchName = "x64";
#else
static const char* const kArchName = "unknown";
#endif

// Fixed measurement parameters shared with the Windows GUI (GUI_SPEC.md 5).
static const unsigned long long kIterations = 1000000ULL;
static const unsigned long long kRuns = 3ULL;

// Minimum interval between two UI progress deliveries (50 ms at most 20 Hz);
// the final result is always delivered by handleRunFinishedWithCode:.
static const double kProgressInterval = 0.05;

// ---------------------------------------------------------------------------
// Geometry (GUI_SPEC.md v3 section 1; all values in pt)
// ---------------------------------------------------------------------------

// Locked design (user decision, final): a compact PORTRAIT window, 600x880 pt.
// One vertical stack of full-width blocks, dense spacing, and the matrix card
// owning all remaining height.  There is no thread selector - a run always
// covers every logical thread.
static const CGFloat kDefaultWidth = cl_layout::kDefaultWidth;
static const CGFloat kDefaultHeight = cl_layout::kDefaultHeight;
static const CGFloat kMinWidth = cl_layout::kMinWidth;
static const CGFloat kMinHeight = cl_layout::kMinHeight;

// Every geometry constant lives in cl_layout.h, which is AppKit-free so that
// layout_selftest.cpp can verify the exact same arithmetic on the build host.
using cl_layout::kButtonGap;
using cl_layout::kButtonHeight;
using cl_layout::kCardPadding;
using cl_layout::kCellGap;
using cl_layout::kColumnHeaderHeight;
using cl_layout::kCountWidth;
using cl_layout::kFooterGap;
using cl_layout::kGridBottomGap;
using cl_layout::kInfoSpacing;
using cl_layout::kLegendBarHeight;
using cl_layout::kLegendBarWidth;
using cl_layout::kLegendRowHeight;
using cl_layout::kMargin;
using cl_layout::kMatrixTitleGap;
using cl_layout::kProgressRowHeight;
using cl_layout::kSectionGap;

static const CGFloat kCardRadius = 10.0;      // native macOS card radius
static const CGFloat kCardBorderWidth = 1.0;

static const CGFloat kTitleFontSize = 26.0;
static const CGFloat kSubtitleFontSize = 13.0;
static const CGFloat kVersionFontSize = 11.0;
static const CGFloat kCardTitleFontSize = 13.0;
static const CGFloat kInfoFontSize = 12.0;
static const CGFloat kStatusFontSize = 12.5;

static const CGFloat kMatrixHeaderFontSize = 10.5;
static const CGFloat kCellRadius = 4.0;
static const CGFloat kLegendFontSize = 11.0;
static const CGFloat kStatsFontSize = 11.5;
static const CGFloat kFootnoteFontSize = 11.0;
static const CGFloat kFooterFontSize = 11.0;

// ---------------------------------------------------------------------------
// Semantic colours (GUI_SPEC.md v3 section 3 - no hard-coded greys)
// ---------------------------------------------------------------------------

static NSColor* CLLabelColour(void) { return [NSColor labelColor]; }
static NSColor* CLSecondaryColour(void) { return [NSColor secondaryLabelColor]; }
static NSColor* CLTertiaryColour(void) { return [NSColor tertiaryLabelColor]; }
static NSColor* CLQuaternaryColour(void) { return [NSColor quaternaryLabelColor]; }
static NSColor* CLCardFillColour(void) { return [NSColor controlBackgroundColor]; }

// separatorColor (10.14) and controlAccentColor (10.14) are class properties;
// probing +respondsToSelector: keeps the macOS 10.13 x86_64 slice free of
// availability thunks (__isPlatformVersionAtLeast is not exported by the
// osxcross libSystem stub) while still preferring the modern semantic colours.
typedef NSColor* (*CLColourGetter)(id, SEL);

static NSColor* CLSemanticColour(NSString* selectorName, NSColor* fallback) {
    const SEL selector = NSSelectorFromString(selectorName);
    if (selectorName == nil || ![NSColor respondsToSelector:selector]) {
        return fallback;
    }
    CLColourGetter getter = (CLColourGetter)(void*)objc_msgSend;
    NSColor* colour = getter((id)[NSColor class], selector);
    return (colour != nil) ? colour : fallback;
}

static NSColor* CLCardBorderColour(void) {
    return CLSemanticColour(@"separatorColor", [NSColor gridColor]);
}

static NSColor* CLAccentColour(void) {
    return CLSemanticColour(@"controlAccentColor", [NSColor systemBlueColor]);
}

static NSColor* CLDangerColour(void) { return [NSColor systemRedColor]; }

// Accessibility "Increase contrast" (10.10+).  The selector probe keeps the
// 10.13 deployment target free of availability thunks; the state is read per
// paint, which is cheap and always current.
static BOOL CLIncreaseContrast(void) {
    NSWorkspace* workspace = [NSWorkspace sharedWorkspace];
    const SEL selector =
        NSSelectorFromString(@"accessibilityDisplayShouldIncreaseContrast");
    if (workspace == nil || ![workspace respondsToSelector:selector]) {
        return NO;
    }
    typedef BOOL (*CLBoolGetter)(id, SEL);
    CLBoolGetter getter = (CLBoolGetter)(void*)objc_msgSend;
    return getter(workspace, selector) ? YES : NO;
}

// SF Symbols are macOS 11+; a direct call would trip -Wunguarded-availability and
// emit an __isPlatformVersionAtLeast thunk the osxcross libSystem stub does not
// export, so the class method is reached through respondsToSelector: (same
// pattern as the 10.14 semantic colours).  On older systems the buttons simply
// keep their text, which is the spec's documented fallback.
typedef NSImage* (*CLSymbolGetter)(id, SEL, NSString*, NSString*);

static NSImage* CLSystemSymbol(NSString* name) {
    if (name == nil) {
        return nil;
    }
    const SEL selector =
        NSSelectorFromString(@"imageWithSystemSymbolName:accessibilityDescription:");
    if (![NSImage respondsToSelector:selector]) {
        return nil;
    }
    CLSymbolGetter getter = (CLSymbolGetter)(void*)objc_msgSend;
    NSImage* image = getter((id)[NSImage class], selector, name, name);
    [image setTemplate:YES];
    return image;
}

// Attaches a symbol to a real NSButton, leaving the title in place.
static void CLSetButtonSymbol(NSButton* button, NSString* symbolName) {
    if (button == nil) {
        return;
    }
    NSImage* image = CLSystemSymbol(symbolName);
    if (image == nil) {
        return;
    }
    [button setImage:image];
    [button setImagePosition:NSImageLeft];
    [button setImageScaling:NSImageScaleProportionallyDown];
}

// ---------------------------------------------------------------------------
// Fonts (GUI_SPEC.md v3 section 3: system fonts only, digits monospaced)
// ---------------------------------------------------------------------------

enum CLFontKind {
    CLFontKindRegular = 0,
    CLFontKindSemibold,
    CLFontKindBold,
    CLFontKindDigit,
    CLFontKindDigitBold
};

// NSFont creation is expensive and the matrix used to allocate a fresh font for
// every cell of every repaint.  Fonts are immutable, so they are cached per
// (kind, size).  Sizes are rounded to 0.01 pt for the key; every size in this
// file is a multiple of 0.25 pt.  Purged when the layout font size or the
// effective appearance changes.
static std::map<long long, NSFont*>* gCLFontCache = nullptr;

static std::map<long long, NSFont*>* CLFontCache(void) {
    if (gCLFontCache == nullptr) {
        gCLFontCache = new std::map<long long, NSFont*>();
    }
    return gCLFontCache;
}

// NSLog-style weight-aware class methods (systemFontOfSize:weight:,
// monospacedDigitSystemFontOfSize:weight:) and the NSFontWeight constants are
// macOS 10.11+.  The 10.10 slice reproduces the same hierarchy through the
// pre-10.11 API: probe the selector, otherwise fall back to the plain system
// font and let NSFontManager synthesise the bold trait.
typedef NSFont* (*CLWeightedFontFn)(id, SEL, CGFloat, CGFloat);

static NSFont* CLWeightedFont(NSString* selectorName, CGFloat size, CGFloat weight,
                              NSFont* fallback) {
    const SEL selector = NSSelectorFromString(selectorName);
    if (selectorName != nil && [NSFont respondsToSelector:selector]) {
        CLWeightedFontFn make = (CLWeightedFontFn)(void*)objc_msgSend;
        NSFont* font = make((id)[NSFont class], selector, size, weight);
        if (font != nil) {
            return font;
        }
    }
    return fallback;
}

static NSFont* CLSystemFont(CGFloat size) {
    return CLWeightedFont(@"systemFontOfSize:weight:", size, 0.0,
                          [NSFont systemFontOfSize:size]);
}

static NSFont* CLSystemBoldFont(CGFloat size) {
    NSFont* plain = CLSystemFont(size);
    if (plain == nil) {
        return nil;
    }
    const SEL selector = NSSelectorFromString(@"systemFontOfSize:weight:");
    if ([NSFont respondsToSelector:selector]) {
        return CLWeightedFont(@"systemFontOfSize:weight:", size, 0.4, plain);
    }
    NSFontManager* manager = [NSFontManager sharedFontManager];
    return (manager != nil) ? [manager convertFont:plain toHaveTrait:NSBoldFontMask]
                            : plain;
}

static NSFont* CLSystemSemiboldFont(CGFloat size) {
    NSFont* plain = CLSystemFont(size);
    if (plain == nil) {
        return nil;
    }
    const SEL selector = NSSelectorFromString(@"systemFontOfSize:weight:");
    if ([NSFont respondsToSelector:selector]) {
        return CLWeightedFont(@"systemFontOfSize:weight:", size, 0.3, plain);
    }
    NSFontManager* manager = [NSFontManager sharedFontManager];
    return (manager != nil) ? [manager convertFont:plain toHaveTrait:NSBoldFontMask]
                            : plain;
}

static NSFont* CLMonospacedFont(CGFloat size) {
    NSFont* fallback = [NSFont fontWithName:@"Menlo" size:size];
    if (fallback == nil) {
        fallback = [NSFont systemFontOfSize:size];
    }
    return CLWeightedFont(@"monospacedDigitSystemFontOfSize:weight:", size, 0.0,
                          fallback);
}

static NSFont* CLMonospacedBoldFont(CGFloat size) {
    NSFont* plain = CLMonospacedFont(size);
    if (plain == nil) {
        return nil;
    }
    const SEL selector = NSSelectorFromString(@"monospacedDigitSystemFontOfSize:weight:");
    if ([NSFont respondsToSelector:selector]) {
        return CLWeightedFont(@"monospacedDigitSystemFontOfSize:weight:", size, 0.4,
                              plain);
    }
    NSFontManager* manager = [NSFontManager sharedFontManager];
    return (manager != nil) ? [manager convertFont:plain toHaveTrait:NSBoldFontMask]
                            : plain;
}

// NSControl.maximumNumberOfLines is macOS 10.11+.  On older systems the field
// keeps its historical single-line behaviour, which is exactly what the design
// wants, so the setter is probed and skipped.
static void CLMakeSingleLine(NSTextField* field) {
    if (field == nil) {
        return;
    }
    const SEL selector = NSSelectorFromString(@"setMaximumNumberOfLines:");
    if (![field respondsToSelector:selector]) {
        return;
    }
    typedef void (*CLIntSetter)(id, SEL, NSInteger);
    CLIntSetter setter = (CLIntSetter)(void*)objc_msgSend;
    setter(field, selector, 1);
}

static NSFont* CLCachedFont(CLFontKind kind, CGFloat size) {
    const long long key =
        static_cast<long long>(kind) * 1000000LL +
        static_cast<long long>(std::llround(static_cast<double>(size) * 100.0));
    std::map<long long, NSFont*>* cache = CLFontCache();
    const std::map<long long, NSFont*>::iterator found = cache->find(key);
    if (found != cache->end()) {
        return found->second;
    }
    NSFont* font = nil;
    switch (kind) {
        case CLFontKindRegular:
            font = CLSystemFont(size);
            break;
        case CLFontKindSemibold:
            font = CLSystemSemiboldFont(size);
            break;
        case CLFontKindBold:
            font = CLSystemBoldFont(size);
            break;
        case CLFontKindDigit:
            font = CLMonospacedFont(size);
            break;
        case CLFontKindDigitBold:
            font = CLMonospacedBoldFont(size);
            break;
    }
    if (font != nil) {
        cache->emplace(key, font);
    }
    return font;
}

// NSButton.bezelColor is macOS 10.12.2+; probe the setter so the 10.10 slice
// simply keeps the stock button rendering.
static void CLSetBezelColour(NSButton* button, NSColor* colour) {
    if (button == nil || colour == nil) {
        return;
    }
    const SEL selector = NSSelectorFromString(@"setBezelColor:");
    if (![button respondsToSelector:selector]) {
        return;
    }
    typedef void (*CLColourSetter)(id, SEL, NSColor*);
    CLColourSetter setter = (CLColourSetter)(void*)objc_msgSend;
    setter(button, selector, colour);
}

static NSFont* CLUIFont(CGFloat size) {
    return CLCachedFont(CLFontKindRegular, size);
}

static NSFont* CLUISemiboldFont(CGFloat size) {
    return CLCachedFont(CLFontKindSemibold, size);
}

static NSFont* CLUIBoldFont(CGFloat size) {
    return CLCachedFont(CLFontKindBold, size);
}

static NSFont* CLDigitFont(CGFloat size) {
    return CLCachedFont(CLFontKindDigit, size);
}

static NSFont* CLDigitBoldFont(CGFloat size) {
    return CLCachedFont(CLFontKindDigitBold, size);
}

// ---------------------------------------------------------------------------
// Text measuring / drawing helpers
// ---------------------------------------------------------------------------

static CGFloat CLTextHeight(NSFont* font) {
    if (font == nil) {
        return 0.0;
    }
    return std::ceil(font.ascender - font.descender + font.leading);
}

static CGFloat CLTextWidth(NSString* text, NSFont* font) {
    if (text == nil || text.length == 0 || font == nil) {
        return 0.0;
    }
    return std::ceil([text sizeWithAttributes:@{NSFontAttributeName : font}].width);
}

// cl_layout uses a plain struct so the host-side self-test can link it without
// AppKit; the app converts each rect once.
static NSRect CLRect(const cl_layout::Rect& rect) {
    return NSMakeRect(static_cast<CGFloat>(rect.x), static_cast<CGFloat>(rect.y),
                      static_cast<CGFloat>(rect.width),
                      static_cast<CGFloat>(rect.height));
}

struct CLTextAttributesKey {
    const void* font;
    const void* colour;
    long alignment;

    bool operator<(const CLTextAttributesKey& other) const {
        if (font != other.font) {
            return font < other.font;
        }
        if (colour != other.colour) {
            return colour < other.colour;
        }
        return alignment < other.alignment;
    }
};

// Drawing attributes (paragraph style + font/colour dictionary) were allocated
// for every string drawn.  Cache them by font/colour/alignment identity.
static std::map<CLTextAttributesKey, NSDictionary*>* gCLTextAttributesCache =
    nullptr;

static std::map<CLTextAttributesKey, NSDictionary*>* CLTextAttributesCache(void) {
    if (gCLTextAttributesCache == nullptr) {
        gCLTextAttributesCache =
            new std::map<CLTextAttributesKey, NSDictionary*>();
    }
    return gCLTextAttributesCache;
}

static NSDictionary* CLTextAttributes(NSFont* font, NSColor* colour,
                                      NSTextAlignment alignment) {
    if (font == nil) {
        return nil;
    }
    CLTextAttributesKey key;
    key.font = (__bridge const void*)font;
    key.colour = (__bridge const void*)colour;
    key.alignment = static_cast<long>(alignment);
    std::map<CLTextAttributesKey, NSDictionary*>* cache =
        CLTextAttributesCache();
    const std::map<CLTextAttributesKey, NSDictionary*>::iterator found =
        cache->find(key);
    if (found != cache->end()) {
        return found->second;
    }
    if (cache->size() > 512) {
        cache->clear();   // bounded: never let appearance churn grow forever
    }
    NSMutableParagraphStyle* style = [[NSMutableParagraphStyle alloc] init];
    style.alignment = alignment;
    style.lineBreakMode = NSLineBreakByClipping;
    NSDictionary* attributes = @{
        NSFontAttributeName : font,
        NSForegroundColorAttributeName : colour,
        NSParagraphStyleAttributeName : style
    };
    cache->emplace(key, attributes);
    return attributes;
}

// Call after an appearance / font-size change: old NSFont instances must not be
// reused, and cached attribute dictionaries retain their fonts.
static void CLPurgeDrawingCaches(void) {
    if (gCLFontCache != nullptr) {
        gCLFontCache->clear();
    }
    if (gCLTextAttributesCache != nullptr) {
        gCLTextAttributesCache->clear();
    }
}

static void CLDrawString(NSString* text, NSRect rect, NSFont* font,
                         NSColor* colour, NSTextAlignment alignment) {
    if (text == nil || text.length == 0 || font == nil) {
        return;
    }
    [text drawInRect:rect withAttributes:CLTextAttributes(font, colour, alignment)];
}

// Fills one rounded matrix cell using the shared CGPath and a translated CTM.
static void CLFillCell(CGContextRef context, CGPathRef path, CGFloat x, CGFloat y,
                       NSColor* colour) {
    CGContextSaveGState(context);
    CGContextTranslateCTM(context, x, y);
    CGContextAddPath(context, path);
    [colour setFill];
    CGContextFillPath(context);
    CGContextRestoreGState(context);
}

// Draws one line vertically centred inside rect.
static void CLDrawStringCentred(NSString* text, NSRect rect, NSFont* font,
                                NSColor* colour) {
    const CGFloat height = CLTextHeight(font);
    const NSRect line = NSMakeRect(rect.origin.x,
                                   rect.origin.y + std::floor((rect.size.height - height) / 2.0),
                                   rect.size.width, height);
    CLDrawString(text, line, font, colour, NSTextAlignmentCenter);
}

// Greedy composed-character wrapping (device info rows).
static void CLWrapText(NSString* text, NSFont* font, CGFloat maxWidth,
                       NSMutableArray<NSString*>* out) {
    if (text == nil || text.length == 0 || out == nil) {
        return;
    }
    if (maxWidth <= 1.0) {
        [out addObject:text];
        return;
    }
    NSMutableString* line = [NSMutableString string];
    NSUInteger index = 0;
    while (index < text.length) {
        const NSRange cluster = [text rangeOfComposedCharacterSequenceAtIndex:index];
        NSString* piece = [text substringWithRange:cluster];
        [line appendString:piece];
        if (CLTextWidth(line, font) > maxWidth && line.length > piece.length) {
            [line deleteCharactersInRange:NSMakeRange(line.length - piece.length,
                                                      piece.length)];
            [out addObject:[NSString stringWithString:line]];
            [line setString:piece];
        }
        index = NSMaxRange(cluster);
    }
    if (line.length > 0) {
        [out addObject:[NSString stringWithString:line]];
    }
}

// ---------------------------------------------------------------------------
// Heat colour (GUI_SPEC.md section 4) - identical in both front ends
// ---------------------------------------------------------------------------

static void CLHeatRGB(float value, float minimum, float maximum, double* rgb) {
    if (rgb == nullptr) {
        return;
    }
    double t = 0.0;
    if (maximum - minimum >= 1e-6f) {
        t = (static_cast<double>(value) - static_cast<double>(minimum)) /
            (static_cast<double>(maximum) - static_cast<double>(minimum));
    }
    if (t < 0.0) {
        t = 0.0;
    }
    if (t > 1.0) {
        t = 1.0;
    }
    const double c0[3] = {22.0, 183.0, 105.0};   // #16B769
    const double c1[3] = {244.0, 128.0, 17.0};   // #F48011
    const double c2[3] = {248.0, 53.0, 51.0};    // #F83533
    const double s = (t < 0.5) ? (t * 2.0) : ((t - 0.5) * 2.0);
    for (int channel = 0; channel < 3; ++channel) {
        const double near = c0[channel] + (c1[channel] - c0[channel]) * s;
        const double far = c1[channel] + (c2[channel] - c1[channel]) * s;
        rgb[channel] = (t >= 0.5) ? far : near;
    }
}

static NSColor* CLHeatColour(float value, float minimum, float maximum) {
    double rgb[3] = {0.0, 0.0, 0.0};
    CLHeatRGB(value, minimum, maximum, rgb);
    return [NSColor colorWithSRGBRed:(CGFloat)std::floor(rgb[0] + 0.5) / 255.0
                               green:(CGFloat)std::floor(rgb[1] + 0.5) / 255.0
                                blue:(CGFloat)std::floor(rgb[2] + 0.5) / 255.0
                               alpha:1.0];
}

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------

enum CLStatusKind {
    CLStatusReady = 0,
    CLStatusRunning,
    CLStatusDone,
    CLStatusStopped,
    CLStatusFailed
};

struct CLStats {
    bool valid = false;
    float minimum = 0.0f;
    float maximum = 0.0f;
    double average = 0.0;
    int count = 0;
};

struct CLModel {
    int logical = 2;
    int physical = 0;
    int cacheLine = 64;
    std::string arch;
    std::string version;
    std::string resultsDir;

    int n = 2;
    std::vector<float> cells;               // n*n, NaN = not measured
    std::vector<int> slotCore;              // -1 when unknown
    std::vector<unsigned char> smtSibling;  // 1 when the slot has an SMT sibling
    bool hasSmt = false;
    std::string smtText;

    int done = 0;
    int total = 0;
    bool running = false;
    bool stopRequested = false;
    bool hasResult = false;
    CLStatusKind status = CLStatusReady;
    std::string errorText;

    int lastI = -1;
    int lastJ = -1;
    float lastLatency = std::numeric_limits<float>::quiet_NaN();
    double elapsed = 0.0;

    // Aggregate statistics are O(n*n); recompute only when a cell changed.
    CLStats cachedStats;
    int statsRevision = 0;
    int statsCachedRevision = -1;
    bool statsCacheValid = false;
};

static CLStats CLComputeStats(const CLModel& model) {
    CLStats stats;
    if (model.n < 1) {
        return stats;
    }
    float minimum = std::numeric_limits<float>::infinity();
    float maximum = -std::numeric_limits<float>::infinity();
    double sum = 0.0;
    int count = 0;
    for (int row = 0; row < model.n; ++row) {
        for (int column = 0; column < model.n; ++column) {
            if (row == column) {
                continue;
            }
            const std::size_t index =
                static_cast<std::size_t>(row) * static_cast<std::size_t>(model.n) +
                static_cast<std::size_t>(column);
            if (index >= model.cells.size()) {
                continue;
            }
            const float value = model.cells[index];
            if (std::isnan(value) || value <= 0.0f) {
                continue;
            }
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
            sum += static_cast<double>(value);
            ++count;
        }
    }
    if (count > 0) {
        stats.valid = true;
        stats.minimum = minimum;
        stats.maximum = maximum;
        stats.average = sum / static_cast<double>(count);
        stats.count = count;
    }
    return stats;
}

static const CLStats& CLCachedStats(CLModel& model) {
    if (!model.statsCacheValid || model.statsCachedRevision != model.statsRevision) {
        model.cachedStats = CLComputeStats(model);
        model.statsCachedRevision = model.statsRevision;
        model.statsCacheValid = true;
    }
    return model.cachedStats;
}

static std::string CLToString(NSString* text) {
    if (text == nil) {
        return std::string();
    }
    const char* utf8 = [text UTF8String];
    return (utf8 != nullptr) ? std::string(utf8) : std::string();
}

static NSString* CLStringFromUtf8(const std::string& text) {
    return [NSString stringWithUTF8String:text.c_str()];
}

static void CLResetCells(CLModel* model, int n) {
    if (model == nullptr) {
        return;
    }
    if (n < 1) {
        n = 1;
    }
    model->n = n;
    model->cells.assign(static_cast<std::size_t>(n) * static_cast<std::size_t>(n),
                        std::numeric_limits<float>::quiet_NaN());
    model->done = 0;
    model->total = n * (n - 1);
    model->lastI = -1;
    model->lastJ = -1;
    ++model->statsRevision;
    model->statsCacheValid = false;
}

// "T0…T7" for the measurement-target sentence (GUI_SPEC.md section 0).
static NSString* CLThreadSpanLabel(int n) {
    if (n < 2) {
        return @"T0";
    }
    return [NSString stringWithFormat:@"T0…T%d", n - 1];
}

// ---------------------------------------------------------------------------
// Matrix geometry (GUI_SPEC.md section 1; portrait design locked by the user)
//
// cl_layout::Compute owns every number - cells, gaps, label column, fonts and
// the column centres - and these views only draw the result, so the picture can
// never disagree with the layout the self-test verifies.
//
//   n <= 12 : a one-decimal value in every cell; cells stay >= 18 pt.
//   n >  12 : colour blocks only.  The user asked for exactly this: the chart
//             does not need to be readable, the colours give the rough picture,
//             the details come from the exported CSV.  The cells shrink until
//             all n columns fit the card width, so the horizontal scroller never
//             appears; a tall grid scrolls vertically inside its own viewport.
//
// The column-header strip is a separate FIXED view above the NSScrollView: it
// never scrolls, and the grid can never paint over the card title, the pair
// counter or the headers.
// ---------------------------------------------------------------------------

// Row label column width, measured with the header font ("T7" -> ~23 pt).
static CGFloat CLMatrixLabelWidth(int n) {
    const int widest = (n > 1) ? (n - 1) : 0;
    NSString* label = [NSString stringWithFormat:@"T%d", widest];
    return CLTextWidth(label, CLDigitFont(kMatrixHeaderFontSize)) + 10.0;
}

// Largest digit size that keeps a "113.5" style value inside the cell.
static CGFloat CLCellFontSize(NSString* text, CGFloat cellWidth, CGFloat base) {
    const CGFloat usable = std::max<CGFloat>(cellWidth - 6.0, 8.0);
    if (text == nil || text.length == 0 || base <= 6.0) {
        return std::max<CGFloat>(base, 6.0);
    }
    // The old loop stepped 0.25 pt at a time from base down to 6 and measured
    // every step.  Digit widths scale linearly with point size, so one measured
    // estimate plus a few corrections is equivalent and much cheaper.
    const CGFloat baseWidth = CLTextWidth(text, CLDigitBoldFont(base));
    if (baseWidth <= usable) {
        return base;
    }
    CGFloat size = std::floor(base * usable / baseWidth * 4.0) / 4.0;
    if (size < 6.0) {
        size = 6.0;
    }
    while (size > 6.0 && CLTextWidth(text, CLDigitBoldFont(size)) > usable) {
        size -= 0.25;
    }
    return std::max<CGFloat>(size, 6.0);
}

// ---------------------------------------------------------------------------
// Card background: a rounded, semantic-coloured container.  Cards are not
// controls, so drawing their background is normal AppKit practice; every
// interactive element below is a real NSControl.
// ---------------------------------------------------------------------------

@interface CLCardView : NSView
@end

@implementation CLCardView {
    NSBezierPath* _cachedPath;
    NSRect _cachedRect;
}

- (BOOL)isFlipped {
    return YES;
}

- (BOOL)isOpaque {
    return NO;
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    _cachedPath = nil;
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    const NSRect rect = NSInsetRect(self.bounds, kCardBorderWidth / 2.0,
                                    kCardBorderWidth / 2.0);
    if (_cachedPath == nil || !NSEqualRects(rect, _cachedRect)) {
        _cachedRect = rect;
        _cachedPath = [NSBezierPath bezierPathWithRoundedRect:rect
                                                      xRadius:kCardRadius
                                                      yRadius:kCardRadius];
    }
    [CLCardFillColour() setFill];
    [_cachedPath fill];
    if (CLIncreaseContrast()) {
        [CLLabelColour() setStroke];   // stronger card outline in high contrast
    } else {
        [CLCardBorderColour() setStroke];
    }
    [_cachedPath setLineWidth:kCardBorderWidth];
    [_cachedPath stroke];
}

// 10.14+; on 10.13 AppKit never sends it, so the override is inert there.
- (void)viewDidChangeEffectiveAppearance {
    [super viewDidChangeEffectiveAppearance];
    _cachedPath = nil;
    [self setNeedsDisplay:YES];
}

@end

// ---------------------------------------------------------------------------
// Matrix view: column headers plus the N x N grid.  Data visualisation, drawn
// with semantic text colours and the shared heat colours.
// ---------------------------------------------------------------------------

@interface CLMatrixView : NSView
@property(nonatomic, assign) CLModel* model;
// A copy of the computed layout: the cells and the fixed header row are both
// positioned with cl_layout::ColumnX / ColumnCenterX, so they cannot drift.
@property(nonatomic, assign) cl_layout::Layout layout;
@end

@implementation CLMatrixView {
    CGPathRef _cellPath;
    CGFloat _cellPathWidth;
    CGFloat _cellPathHeight;
    CGFloat _cellPathRadius;
    NSMutableDictionary<NSNumber*, NSString*>* _valueTextCache;
}

- (BOOL)isFlipped {
    return YES;
}

- (BOOL)isOpaque {
    return NO;
}

- (void)dealloc {
    if (_cellPath != nullptr) {
        CGPathRelease(_cellPath);
    }
}

// One rounded-rect path is shared by every cell: all cells have the same size,
// so the path is built at the origin and translated with the CGContext.
- (CGPathRef)cellPathForWidth:(CGFloat)width
                       height:(CGFloat)height
                       radius:(CGFloat)radius {
    if (_cellPath == nullptr || _cellPathWidth != width ||
        _cellPathHeight != height || _cellPathRadius != radius) {
        if (_cellPath != nullptr) {
            CGPathRelease(_cellPath);
        }
        _cellPath = CGPathCreateWithRoundedRect(
            CGRectMake(0.0, 0.0, width, height), radius, radius, nullptr);
        _cellPathWidth = width;
        _cellPathHeight = height;
        _cellPathRadius = radius;
    }
    return _cellPath;
}

- (void)viewDidChangeEffectiveAppearance {
    [super viewDidChangeEffectiveAppearance];
    _cellPath = nullptr;   // rebuilt lazily by the next draw
    [self setNeedsDisplay:YES];
}

- (NSRect)rectForRow:(int)row column:(int)column {
    CLModel* model = self.model;
    if (model == nullptr || model->n < 1) {
        return NSZeroRect;
    }
    const int n = model->n;
    if (row < 0 || row >= n || column < 0 || column >= n) {
        return NSZeroRect;
    }
    const cl_layout::Layout layout = self.layout;
    const CGFloat x = static_cast<CGFloat>(cl_layout::ColumnX(layout, column));
    const CGFloat y = static_cast<CGFloat>(row) *
                      (layout.cellHeight + layout.cellGap);
    return NSInsetRect(NSMakeRect(x, y, layout.cellWidth, layout.cellHeight), -1.0,
                       -1.0);
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    CLModel* model = self.model;
    if (model == nullptr || model->n < 1) {
        return;
    }
    const cl_layout::Layout layout = self.layout;
    const int n = model->n;
    const CLStats& stats = CLCachedStats(*model);

    const CGFloat gap = static_cast<CGFloat>(layout.cellGap);
    const CGFloat cellW = static_cast<CGFloat>(layout.cellWidth);
    const CGFloat cellH = static_cast<CGFloat>(layout.cellHeight);
    const CGFloat labelW = static_cast<CGFloat>(layout.labelWidth);
    const CGFloat baseFont = static_cast<CGFloat>(layout.cellFontSize);
    // Colour-overview mode (n > 12): no glyphs at all, just the heat colour.
    const BOOL showValues = layout.showCellValues ? YES : NO;
    const BOOL roomy = (cellH >= 14.0) ? YES : NO;
    // Cells are small in overview mode, so the corner radius shrinks with them.
    const CGFloat radius = (cellH >= 10.0) ? kCellRadius
                                           : std::max<CGFloat>(1.0, cellH * 0.25);
    const CGPathRef cellPath = [self cellPathForWidth:cellW height:cellH
                                               radius:radius];
    CGContextRef context = [NSGraphicsContext currentContext].CGContext;

    // The column headers live in CLMatrixHeaderView (a fixed row above the
    // scroll view); this view draws the grid only, so nothing here can ever
    // paint over the card title or the headers while scrolling.
    for (int row = 0; row < n; ++row) {
        const CGFloat y = static_cast<CGFloat>(row) * (cellH + gap);
        const bool star = model->hasSmt &&
                          row < static_cast<int>(model->smtSibling.size()) &&
                          model->smtSibling[static_cast<std::size_t>(row)] != 0;
        NSString* label = [NSString stringWithFormat:@"T%d%@", row,
                                                      star ? @"*" : @""];
        if (roomy) {
            CLDrawStringCentred(label,
                                NSMakeRect(0.0, y, labelW - 4.0, cellH),
                                CLDigitFont(static_cast<CGFloat>(layout.headerFontSize)),
                                CLSecondaryColour());
        }

        for (int column = 0; column < n; ++column) {
            const CGFloat x = static_cast<CGFloat>(cl_layout::ColumnX(layout, column));
            const NSRect cell = NSMakeRect(x, y, cellW, cellH);
            // Only repaint the cells that intersect the dirty rect; a finished
            // pair invalidates exactly one cell (refreshPairRow).
            if (!NSIsEmptyRect(dirtyRect) && !NSIntersectsRect(cell, dirtyRect)) {
                continue;
            }
            if (row == column) {
                CLFillCell(context, cellPath, x, y, CLQuaternaryColour());
                if (showValues) {
                    CLDrawStringCentred(@"x", cell, CLUIFont(baseFont),
                                        CLTertiaryColour());
                }
                continue;
            }
            const std::size_t index = static_cast<std::size_t>(row) * n +
                                      static_cast<std::size_t>(column);
            const float value =
                (index < model->cells.size()) ? model->cells[index]
                                              : std::numeric_limits<float>::quiet_NaN();
            if (std::isnan(value) || value <= 0.0f) {
                CLFillCell(context, cellPath, x, y, CLQuaternaryColour());
                if (showValues) {
                    CLDrawStringCentred(@"·", cell, CLUIFont(baseFont),
                                        CLTertiaryColour());
                }
                continue;
            }
            const float minimum = stats.valid ? stats.minimum : value;
            const float maximum = stats.valid ? stats.maximum : value;
            CLFillCell(context, cellPath, x, y,
                       CLHeatColour(value, minimum, maximum));

            if (!showValues) {
                continue;
            }
            char buffer[32] = {0};
            cl_format_ns(value, buffer, static_cast<int>(sizeof(buffer)));
            NSString* text = [NSString stringWithUTF8String:buffer];
            const CGFloat fontSize = CLCellFontSize(text, cellW, baseFont);
            CLDrawStringCentred(text, cell, CLDigitBoldFont(fontSize),
                                [NSColor whiteColor]);
        }
    }
}

@end

// ---------------------------------------------------------------------------
// Column-header strip: a FIXED row between the card title and the scrolling
// grid.  It is not part of the scroll view, so the headers never move and the
// grid can never paint over them (the user reported exactly that clipping on
// another platform).  Every header is centred on its column with the same
// cl_layout::ColumnCenterX the cells use, so a header cannot drift off its
// column.
// ---------------------------------------------------------------------------

@interface CLMatrixHeaderView : NSView
@property(nonatomic, assign) CLModel* model;
@property(nonatomic, assign) cl_layout::Layout layout;
@end

@implementation CLMatrixHeaderView

- (BOOL)isFlipped {
    return YES;
}

- (void)viewDidChangeEffectiveAppearance {
    [super viewDidChangeEffectiveAppearance];
    [self setNeedsDisplay:YES];
}

- (BOOL)isOpaque {
    return NO;
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    CLModel* model = self.model;
    if (model == nullptr || model->n < 1) {
        return;
    }
    const cl_layout::Layout layout = self.layout;
    const int n = model->n;
    const CGFloat cellW = static_cast<CGFloat>(layout.cellWidth);
    const CGFloat height = NSHeight(self.bounds);
    // Overview cells can be narrower than a "T31" label: then the header row
    // stays empty rather than turning into overlapping glyph soup.
    if (cellW < 13.0) {
        return;
    }
    NSFont* font = CLDigitFont(static_cast<CGFloat>(layout.headerFontSize));
    for (int column = 0; column < n; ++column) {
        const bool star = model->hasSmt &&
                          column < static_cast<int>(model->smtSibling.size()) &&
                          model->smtSibling[static_cast<std::size_t>(column)] != 0;
        NSString* label = [NSString stringWithFormat:@"T%d%@", column,
                                                      star ? @"*" : @""];
        const CGFloat centre = static_cast<CGFloat>(cl_layout::ColumnCenterX(layout, column));
        CLDrawStringCentred(label,
                            NSMakeRect(centre - cellW / 2.0, 0.0, cellW, height),
                            font, CLSecondaryColour());
    }
}

@end

// ---------------------------------------------------------------------------
// Legend swatch: 64 samples of the shared heat curve (GUI_SPEC.md section 4 -
// a linear system gradient would band differently from the cells).
// ---------------------------------------------------------------------------

@interface CLHeatLegendView : NSView
@end

@implementation CLHeatLegendView

- (BOOL)isFlipped {
    return YES;
}

- (void)viewDidChangeEffectiveAppearance {
    [super viewDidChangeEffectiveAppearance];
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    const NSRect bar = self.bounds;
    if (NSWidth(bar) < 4.0 || NSHeight(bar) < 2.0) {
        return;
    }
    NSBezierPath* clip = [NSBezierPath bezierPathWithRoundedRect:bar
                                                         xRadius:4.0
                                                         yRadius:4.0];
    [NSGraphicsContext saveGraphicsState];
    [clip addClip];
    const int samples = 64;
    for (int sample = 0; sample < samples; ++sample) {
        const float position =
            (static_cast<float>(sample) + 0.5f) / static_cast<float>(samples);
        const CGFloat x0 = NSMinX(bar) + NSWidth(bar) *
                                              static_cast<CGFloat>(sample) /
                                              static_cast<CGFloat>(samples);
        const CGFloat x1 = NSMinX(bar) + NSWidth(bar) *
                                              static_cast<CGFloat>(sample + 1) /
                                              static_cast<CGFloat>(samples);
        [CLHeatColour(position, 0.0f, 1.0f) setFill];
        NSRectFillUsingOperation(NSMakeRect(x0, NSMinY(bar), (x1 - x0) + 0.5,
                                            NSHeight(bar)),
                                 NSCompositingOperationSourceOver);
    }
    [NSGraphicsContext restoreGraphicsState];
}

@end

// ---------------------------------------------------------------------------
// Root view: owns the model and every subview, and lays them out by hand so the
// two-way cell constraint from GUI_SPEC.md section 1 can be honoured exactly.
// ---------------------------------------------------------------------------

@protocol CLRootViewDelegate;

@interface CLRootView : NSView
@property(nonatomic, weak) id<CLRootViewDelegate> delegate;
- (CLModel*)model;
- (void)setFooterText:(NSString*)text;
- (void)syncLayout;
- (void)refreshAll;
- (void)refreshPairRow:(int)row column:(int)column;
@end

@protocol CLRootViewDelegate <NSObject>
- (void)rootViewDidPressStart:(CLRootView*)view;
- (void)rootViewDidPressStop:(CLRootView*)view;
- (void)rootViewDidPressExport:(CLRootView*)view;
@end

@implementation CLRootView {
    CLModel _model;

    NSTextField* _titleLabel;
    NSTextField* _subtitleLabel;
    NSTextField* _versionLabel;

    CLCardView* _deviceCard;
    NSTextField* _deviceTitleLabel;
    NSMutableArray<NSTextField*>* _infoLabels;

    NSButton* _startButton;
    NSButton* _stopButton;
    NSButton* _exportButton;
    NSTextField* _statusLabel;
    NSProgressIndicator* _progress;

    CLCardView* _matrixCard;
    NSTextField* _matrixTitleLabel;
    NSTextField* _matrixCountLabel;
    CLMatrixHeaderView* _matrixHeaderView;   // fixed row, outside the clip
    NSScrollView* _scrollView;
    CLMatrixView* _matrixView;
    NSTextField* _legendLowLabel;
    CLHeatLegendView* _legendView;
    NSTextField* _legendHighLabel;
    NSTextField* _statsLabel;
    NSTextField* _footnoteLabel;
    NSTextField* _footerLabel;

    BOOL _inLayout;
}

- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self != nil) {
        _model.arch = kArchName;
        _inLayout = NO;
        [self buildSubviews];
        [[NSNotificationCenter defaultCenter]
            addObserver:self
               selector:@selector(accessibilityDisplayOptionsDidChange:)
                   name:NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification
                 object:nil];
    }
    return self;
}

- (BOOL)isFlipped {
    return YES;
}

- (BOOL)isOpaque {
    return NO;
}

- (CLModel*)model {
    return &_model;
}

// --- construction ---------------------------------------------------------

- (NSTextField*)makeLabel:(NSString*)text
                     font:(NSFont*)font
                    colour:(NSColor*)colour {
    NSTextField* label = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 20, 16)];
    label.stringValue = (text != nil) ? text : @"";
    label.font = font;
    label.textColor = colour;
    label.bezeled = NO;
    label.bordered = NO;
    label.drawsBackground = NO;
    label.editable = NO;
    label.selectable = NO;
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    CLMakeSingleLine(label);
    label.usesSingleLineMode = YES;
    label.alignment = NSTextAlignmentLeft;
    [self addSubview:label];
    return label;
}

- (NSButton*)makeButton:(NSString*)title action:(SEL)action {
    NSButton* button = [[NSButton alloc] initWithFrame:NSMakeRect(0, 0, 100, kButtonHeight)];
    [button setTitle:title];
    [button setBezelStyle:NSBezelStyleRounded];
    [button setButtonType:NSButtonTypeMomentaryPushIn];
    [button setTarget:self];
    [button setAction:action];
    [button setControlSize:NSControlSizeRegular];
    [button setFont:CLUIFont(13.0)];
    [self addSubview:button];
    return button;
}

- (void)buildSubviews {
    _titleLabel = [self makeLabel:@"CoherencyLatency" font:CLUIBoldFont(kTitleFontSize)
                          colour:CLLabelColour()];
    _subtitleLabel = [self makeLabel:@"缓存一致性延迟测试 · macOS 桌面版"
                                font:CLUIFont(kSubtitleFontSize)
                              colour:CLSecondaryColour()];
    _versionLabel = [self makeLabel:@"1.2.0" font:CLDigitFont(kVersionFontSize)
                             colour:CLTertiaryColour()];
    _versionLabel.alignment = NSTextAlignmentRight;

    // Device card.
    _deviceCard = [[CLCardView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)];
    [self addSubview:_deviceCard];
    _deviceTitleLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 20, 16)];
    _deviceTitleLabel.stringValue = @"设备信息";
    _deviceTitleLabel.font = CLUISemiboldFont(kCardTitleFontSize);
    _deviceTitleLabel.textColor = CLLabelColour();
    _deviceTitleLabel.bezeled = NO;
    _deviceTitleLabel.bordered = NO;
    _deviceTitleLabel.drawsBackground = NO;
    _deviceTitleLabel.editable = NO;
    _deviceTitleLabel.selectable = NO;
    CLMakeSingleLine(_deviceTitleLabel);
    _deviceTitleLabel.usesSingleLineMode = YES;
    [_deviceCard addSubview:_deviceTitleLabel];

    _infoLabels = [NSMutableArray array];
    for (int slot = 0; slot < 8; ++slot) {
        NSTextField* label = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 20, 16)];
        label.font = CLUIFont(kInfoFontSize);
        label.textColor = CLSecondaryColour();
        label.bezeled = NO;
        label.bordered = NO;
        label.drawsBackground = NO;
        label.editable = NO;
        label.selectable = NO;
        label.lineBreakMode = NSLineBreakByClipping;
        CLMakeSingleLine(label);
        label.usesSingleLineMode = YES;
        label.hidden = YES;
        [_deviceCard addSubview:label];
        [_infoLabels addObject:label];
    }

    // Action row, status line and progress.
    _startButton = [self makeButton:@"开始测试" action:@selector(pressStart:)];
    _stopButton = [self makeButton:@"停止" action:@selector(pressStop:)];
    _exportButton = [self makeButton:@"导出 CSV" action:@selector(pressExport:)];
    _startButton.toolTip = @"开始测试（⌘R）";
    _stopButton.toolTip = @"停止（⌘.）";
    _exportButton.toolTip = @"导出 CSV（⌘S）";
    CLSetButtonSymbol(_startButton, @"play.fill");
    CLSetButtonSymbol(_stopButton, @"stop.fill");
    CLSetButtonSymbol(_exportButton, @"square.and.arrow.up");

    // macOS: the prominent action is tinted with the user's accent colour and
    // AppKit picks a contrasting title colour.
    if ([_startButton respondsToSelector:@selector(setBezelColor:)]) {
        CLSetBezelColour(_startButton, CLAccentColour());
    }
    [_stopButton setEnabled:NO];
    [_exportButton setEnabled:NO];

    _statusLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 200, 18)];
    _statusLabel.bezeled = NO;
    _statusLabel.bordered = NO;
    _statusLabel.drawsBackground = NO;
    _statusLabel.editable = NO;
    _statusLabel.selectable = NO;
    _statusLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    CLMakeSingleLine(_statusLabel);
    _statusLabel.usesSingleLineMode = YES;
    [self addSubview:_statusLabel];

    _progress = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(0, 0, 100,
                                                                      kProgressRowHeight)];
    [_progress setStyle:NSProgressIndicatorStyleBar];
    [_progress setIndeterminate:NO];
    [_progress setControlSize:NSControlSizeSmall];
    [_progress setMinValue:0.0];
    [_progress setMaxValue:1.0];
    [_progress setDoubleValue:0.0];
    [_progress sizeToFit];   // bar style -> AppKit's recommended height
    [self addSubview:_progress];

    // Matrix card.
    _matrixCard = [[CLCardView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)];
    [self addSubview:_matrixCard];

    _matrixTitleLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 200, 17)];
    _matrixTitleLabel.stringValue = @"线程延迟矩阵 · ns/op";
    _matrixTitleLabel.font = CLUISemiboldFont(kCardTitleFontSize);
    _matrixTitleLabel.textColor = CLLabelColour();
    _matrixTitleLabel.bezeled = NO;
    _matrixTitleLabel.bordered = NO;
    _matrixTitleLabel.drawsBackground = NO;
    _matrixTitleLabel.editable = NO;
    _matrixTitleLabel.selectable = NO;
    [_matrixCard addSubview:_matrixTitleLabel];

    _matrixCountLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 72, 17)];
    _matrixCountLabel.stringValue = @"0/0";
    _matrixCountLabel.font = CLDigitFont(kInfoFontSize);
    _matrixCountLabel.textColor = CLSecondaryColour();
    _matrixCountLabel.alignment = NSTextAlignmentRight;
    _matrixCountLabel.bezeled = NO;
    _matrixCountLabel.bordered = NO;
    _matrixCountLabel.drawsBackground = NO;
    _matrixCountLabel.editable = NO;
    _matrixCountLabel.selectable = NO;
    [_matrixCard addSubview:_matrixCountLabel];

    // The header strip is a sibling of the scroll view, not part of it: that is
    // what keeps "T0..Tn-1" fixed while the grid scrolls, and keeps the grid
    // from painting over the card title or the counter.
    _matrixHeaderView = [[CLMatrixHeaderView alloc] initWithFrame:NSMakeRect(0, 0, 100,
                                                                             kColumnHeaderHeight)];
    _matrixHeaderView.model = &_model;
    [_matrixCard addSubview:_matrixHeaderView];

    _matrixView = [[CLMatrixView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)];
    _matrixView.model = &_model;

    _scrollView = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)];
    _scrollView.drawsBackground = NO;
    _scrollView.borderType = NSNoBorder;
    _scrollView.hasVerticalScroller = NO;
    _scrollView.hasHorizontalScroller = NO;
    _scrollView.autohidesScrollers = YES;
    _scrollView.scrollerStyle = NSScrollerStyleOverlay;
    _scrollView.documentView = _matrixView;
    [_matrixCard addSubview:_scrollView];

    _legendLowLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 16, 16)];
    _legendLowLabel.stringValue = @"低";
    _legendLowLabel.font = CLUIFont(kLegendFontSize);
    _legendLowLabel.textColor = CLSecondaryColour();
    _legendLowLabel.bezeled = NO;
    _legendLowLabel.bordered = NO;
    _legendLowLabel.drawsBackground = NO;
    _legendLowLabel.editable = NO;
    _legendLowLabel.selectable = NO;
    [_matrixCard addSubview:_legendLowLabel];

    _legendView = [[CLHeatLegendView alloc] initWithFrame:NSMakeRect(0, 0, kLegendBarWidth,
                                                                    kLegendBarHeight)];
    [_matrixCard addSubview:_legendView];

    _legendHighLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 16, 16)];
    _legendHighLabel.stringValue = @"高";
    _legendHighLabel.font = CLUIFont(kLegendFontSize);
    _legendHighLabel.textColor = CLSecondaryColour();
    _legendHighLabel.bezeled = NO;
    _legendHighLabel.bordered = NO;
    _legendHighLabel.drawsBackground = NO;
    _legendHighLabel.editable = NO;
    _legendHighLabel.selectable = NO;
    [_matrixCard addSubview:_legendHighLabel];

    _statsLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 260, 16)];
    _statsLabel.stringValue = @"平均 -- · 最小 -- · 最大 -- ns";
    _statsLabel.font = CLDigitFont(kStatsFontSize);
    _statsLabel.textColor = CLSecondaryColour();
    _statsLabel.alignment = NSTextAlignmentRight;
    _statsLabel.bezeled = NO;
    _statsLabel.bordered = NO;
    _statsLabel.drawsBackground = NO;
    _statsLabel.editable = NO;
    _statsLabel.selectable = NO;
    [_matrixCard addSubview:_statsLabel];

    _footnoteLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 200, 14)];
    // Only shown when the engine reports SMT siblings; on macOS slot_core is
    // always -1 (fill_slot_cores is Windows-only), so this stays hidden.
    _footnoteLabel.stringValue =
        @"带 * 的线程互为同核超线程；这些格子通常约 30 ns";
    _footnoteLabel.font = CLUIFont(kFootnoteFontSize);
    _footnoteLabel.textColor = CLTertiaryColour();
    _footnoteLabel.bezeled = NO;
    _footnoteLabel.bordered = NO;
    _footnoteLabel.drawsBackground = NO;
    _footnoteLabel.editable = NO;
    _footnoteLabel.selectable = NO;
    _footnoteLabel.hidden = YES;
    [_matrixCard addSubview:_footnoteLabel];

    _footerLabel = [self makeLabel:@"" font:CLUIFont(kFooterFontSize)
                            colour:CLTertiaryColour()];
}

// --- actions --------------------------------------------------------------

- (void)pressStart:(id)sender {
    (void)sender;
    [self.delegate rootViewDidPressStart:self];
}

- (void)pressStop:(id)sender {
    (void)sender;
    [self.delegate rootViewDidPressStop:self];
}

- (void)pressExport:(id)sender {
    (void)sender;
    [self.delegate rootViewDidPressExport:self];
}

// --- device information ---------------------------------------------------

- (NSArray<NSString*>*)deviceInfoRows {
    NSMutableArray<NSString*>* rows = [NSMutableArray array];
    [rows addObject:[NSString stringWithFormat:@"%d 个逻辑线程 · %d B 缓存行 · %s",
                                               _model.logical, _model.cacheLine,
                                               _model.arch.c_str()]];
    if (_model.hasSmt && _model.physical > 0) {
        NSString* smt = _model.smtText.empty() ? @"T1/T2、T3/T4"
                                               : CLStringFromUtf8(_model.smtText);
        [rows addObject:[NSString stringWithFormat:@"物理核 %d 个 · 同核超线程：%@",
                                                   _model.physical, smt]];
    } else if (_model.physical > 0) {
        [rows addObject:[NSString stringWithFormat:@"物理核 %d 个", _model.physical]];
    }
    {
        NSString* tail = _model.hasSmt
                             ? @"；同核两个线程之间的延迟通常约 30 ns"
                             : @"；跨核线程延迟通常在 30–120 ns";
        [rows addObject:[NSString stringWithFormat:@"测量对象：%@ 共 %d 个逻辑线程%@",
                                                   CLThreadSpanLabel(_model.n), _model.n,
                                                   tail]];
    }
    [rows addObject:[NSString stringWithFormat:
                                   @"固定参数：%d 个逻辑线程 · 1,000,000 次 · 3 轮 · Bounce",
                                   _model.n]];
    return rows;
}

- (void)setFooterText:(NSString*)text {
    _footerLabel.stringValue = (text != nil) ? text : @"";
}

// --- layout ---------------------------------------------------------------

- (void)syncLayout {
    if (_inLayout) {
        return;
    }
    _inLayout = YES;
    // Geometry/font-size change: drop cached fonts and attribute dictionaries.
    CLPurgeDrawingCaches();

    const NSRect bounds = self.bounds;
    cl_layout::Metrics metrics;
    metrics.width = NSWidth(bounds);
    metrics.height = NSHeight(bounds);
    metrics.threads = std::max(_model.n, 1);
    metrics.hasSmt = _model.hasSmt;

    // --- measure text ----------------------------------------------------
    NSFont* titleFont = CLUIBoldFont(kTitleFontSize);
    NSFont* subtitleFont = CLUIFont(kSubtitleFontSize);
    NSFont* versionFont = CLDigitFont(kVersionFontSize);
    NSFont* cardTitleFont = CLUISemiboldFont(kCardTitleFontSize);
    NSFont* infoFont = CLUIFont(kInfoFontSize);
    NSFont* legendFont = CLUIFont(kLegendFontSize);
    NSFont* footerFont = CLUIFont(kFooterFontSize);

    metrics.titleH = CLTextHeight(titleFont);
    metrics.subtitleH = CLTextHeight(subtitleFont);
    metrics.versionH = CLTextHeight(versionFont);
    metrics.cardTitleH = CLTextHeight(cardTitleFont);
    metrics.infoH = CLTextHeight(infoFont);
    metrics.statusH = CLTextHeight(CLUIFont(kStatusFontSize));
    metrics.footerH = CLTextHeight(footerFont);
    metrics.matrixTitleH = CLTextHeight(cardTitleFont);
    metrics.matrixCountH = CLTextHeight(CLDigitFont(kInfoFontSize));
    metrics.footnoteH = CLTextHeight(CLUIFont(kFootnoteFontSize));
    // Height of the "T0..Tn-1" header row, which now lives in its own view.
    metrics.headerH = CLTextHeight(CLDigitFont(kMatrixHeaderFontSize));

    metrics.titleW = CLTextWidth(_titleLabel.stringValue, titleFont) + 2.0;
    metrics.versionW = CLTextWidth(_versionLabel.stringValue, versionFont) + 2.0;
    metrics.lowLabelW = CLTextWidth(_legendLowLabel.stringValue, legendFont);
    metrics.highLabelW = CLTextWidth(_legendHighLabel.stringValue, legendFont);
    metrics.matrixLabelW = CLMatrixLabelWidth(metrics.threads);
    // The progress bar keeps the height AppKit recommends for bar style, so the
    // row always fits the native control.
    metrics.progressH = std::max<CGFloat>(NSHeight(_progress.frame), 6.0);

    [_startButton sizeToFit];
    [_stopButton sizeToFit];
    [_exportButton sizeToFit];
    metrics.startButtonW =
        std::max<CGFloat>(100.0, NSWidth(_startButton.frame) + 10.0);
    metrics.stopButtonW =
        std::max<CGFloat>(74.0, NSWidth(_stopButton.frame) + 10.0);
    metrics.exportButtonW =
        std::max<CGFloat>(96.0, NSWidth(_exportButton.frame) + 10.0);

    // Device-info rows wrap into the width the layout will hand them; the line
    // count feeds back into the card height.
    NSMutableArray<NSString*>* lines = [NSMutableArray array];
    {
        const CGFloat infoW = cl_layout::InfoWidth(metrics);
        for (NSString* row in [self deviceInfoRows]) {
            CLWrapText(row, infoFont, infoW, lines);
        }
    }
    metrics.infoLineCount = static_cast<int>(lines.count);

    const cl_layout::Layout layout = cl_layout::Compute(metrics);

    if (cl_layout::DumpEnabled()) {
        cl_layout::Dump(metrics, layout, stderr);
    }

    // --- header ----------------------------------------------------------
    _titleLabel.frame = CLRect(layout.title);
    _subtitleLabel.frame = CLRect(layout.subtitle);
    _versionLabel.frame = CLRect(layout.version);

    // --- device card -----------------------------------------------------
    _deviceCard.frame = CLRect(layout.deviceCard);
    _deviceTitleLabel.frame = CLRect(layout.deviceTitle);
    for (NSUInteger index = 0; index < _infoLabels.count; ++index) {
        NSTextField* label = _infoLabels[index];
        if (static_cast<int>(index) < metrics.infoLineCount) {
            NSString* text = lines[index];
            if (![label.stringValue isEqualToString:text]) {
                label.stringValue = text;
            }
            label.hidden = NO;
            label.frame = NSMakeRect(kCardPadding,
                                     layout.infoFirstY +
                                         static_cast<CGFloat>(index) *
                                             layout.infoLineStep,
                                     layout.infoWidth, layout.infoLineHeight);
        } else {
            label.hidden = YES;
            label.frame = NSZeroRect;
        }
    }
    // --- action row, status and progress ---------------------------------
    _statusLabel.frame = CLRect(layout.status);
    _startButton.frame = CLRect(layout.startButton);
    _stopButton.frame = CLRect(layout.stopButton);
    _exportButton.frame = CLRect(layout.exportButton);
    _progress.frame = CLRect(layout.progress);

    // --- footer and matrix card ------------------------------------------
    _footerLabel.frame = CLRect(layout.footer);
    _matrixCard.frame = CLRect(layout.matrixCard);
    _matrixTitleLabel.frame = CLRect(layout.matrixTitle);
    _matrixCountLabel.frame = CLRect(layout.matrixCount);

    // The grid and its fixed header share one layout copy, so the headers are
    // centred with exactly the column arithmetic the cells use.
    _matrixView.layout = layout;
    _matrixHeaderView.layout = layout;
    _matrixHeaderView.frame = CLRect(layout.headerRow);

    _scrollView.frame = CLRect(layout.scrollView);
    _scrollView.hasVerticalScroller = layout.verticalScroller;
    _scrollView.hasHorizontalScroller = layout.horizontalScroller;
    [_matrixView setFrameSize:NSMakeSize(layout.matrixDoc.width,
                                         layout.matrixDoc.height)];

    _legendLowLabel.frame = CLRect(layout.legendLow);
    _legendView.frame = CLRect(layout.legendBar);
    _legendHighLabel.frame = CLRect(layout.legendHigh);
    _statsLabel.frame = CLRect(layout.stats);
    if (_model.hasSmt && !layout.footnote.Empty()) {
        _footnoteLabel.hidden = NO;
        _footnoteLabel.frame = CLRect(layout.footnote);
    } else {
        _footnoteLabel.hidden = YES;
        _footnoteLabel.frame = NSZeroRect;
    }

    _inLayout = NO;
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self syncLayout];
}

- (void)viewDidChangeEffectiveAppearance {
    [super viewDidChangeEffectiveAppearance];
    CLPurgeDrawingCaches();
    [self setNeedsDisplay:YES];
    [_matrixCard setNeedsDisplay:YES];
    [_matrixView setNeedsDisplay:YES];
    [_matrixHeaderView setNeedsDisplay:YES];
    [_legendView setNeedsDisplay:YES];
}

- (void)accessibilityDisplayOptionsDidChange:(NSNotification*)notification {
    (void)notification;
    [self setNeedsDisplay:YES];
    [_matrixCard setNeedsDisplay:YES];
    [_matrixView setNeedsDisplay:YES];
    [_matrixHeaderView setNeedsDisplay:YES];
    [_legendView setNeedsDisplay:YES];
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    [self syncLayout];
}

// --- refreshing -----------------------------------------------------------

- (NSAttributedString*)statusAttributedString {
    NSFont* ui = CLUIFont(kStatusFontSize);
    NSFont* digits = CLDigitFont(kStatusFontSize);
    NSColor* colour = (_model.status == CLStatusFailed) ? CLDangerColour()
                                                        : CLLabelColour();
    NSMutableAttributedString* line = [[NSMutableAttributedString alloc] init];
    void (^add)(NSString*, NSFont*) = ^(NSString* text, NSFont* font) {
        if (text == nil) {
            return;
        }
        NSAttributedString* piece = [[NSAttributedString alloc]
            initWithString:text
                attributes:@{NSFontAttributeName : font,
                             NSForegroundColorAttributeName : colour}];
        [line appendAttributedString:piece];
    };

    switch (_model.status) {
        case CLStatusReady:
            add(@"就绪 · ", ui);
            add([NSString stringWithFormat:@"%d", _model.logical], digits);
            add(@" 个逻辑线程 · ", ui);
            add([NSString stringWithFormat:@"%d×%d", _model.n, _model.n], digits);
            add(@" 线程矩阵", ui);
            break;
        case CLStatusRunning: {
            add(@"测量中 ", ui);
            add([NSString stringWithFormat:@"%d/%d", _model.done, _model.total], digits);
            if (_model.lastI >= 0 && _model.lastJ >= 0) {
                add(@" · ", ui);
                add([NSString stringWithFormat:@"T%d↔T%d ", _model.lastI,
                                                _model.lastJ],
                    ui);
                if (std::isnan(_model.lastLatency)) {
                    add(@"N/A", digits);
                } else {
                    char buffer[32] = {0};
                    cl_format_ns(_model.lastLatency, buffer,
                                 static_cast<int>(sizeof(buffer)));
                    add([NSString stringWithUTF8String:buffer], digits);
                    add(@" ns", ui);
                }
            }
            if (_model.done > 0) {
                add(@" · 用时 ", ui);
                add([NSString stringWithFormat:@"%.1f s", _model.elapsed], digits);
            } else {
                add(@" · 用时 ", ui);
                add(@"0.0 s", digits);
            }
            break;
        }
        case CLStatusDone:
            add(@"完成 · ", ui);
            add([NSString stringWithFormat:@"%d", _model.total], digits);
            add(@" 个线程对 · 用时 ", ui);
            add([NSString stringWithFormat:@"%.1f s", _model.elapsed], digits);
            break;
        case CLStatusStopped:
            add(@"已停止 · 完成 ", ui);
            add([NSString stringWithFormat:@"%d/%d", _model.done, _model.total], digits);
            add(@" 个线程对 · 用时 ", ui);
            add([NSString stringWithFormat:@"%.1f s", _model.elapsed], digits);
            break;
        case CLStatusFailed:
            add(@"出错：", ui);
            add(CLStringFromUtf8(_model.errorText), ui);
            break;
    }
    return line;
}

- (NSString*)statsText {
    const CLStats& stats = CLCachedStats(_model);
    if (!stats.valid) {
        return @"平均 -- · 最小 -- · 最大 -- ns";
    }
    char minBuffer[32] = {0};
    char maxBuffer[32] = {0};
    char avgBuffer[32] = {0};
    cl_format_ns(stats.minimum, minBuffer, static_cast<int>(sizeof(minBuffer)));
    cl_format_ns(stats.maximum, maxBuffer, static_cast<int>(sizeof(maxBuffer)));
    cl_format_ns(static_cast<float>(stats.average), avgBuffer,
                 static_cast<int>(sizeof(avgBuffer)));
    return [NSString stringWithFormat:@"平均 %s · 最小 %s · 最大 %s ns", avgBuffer,
                                      minBuffer, maxBuffer];
}

- (void)updateStatusAndProgress {
    _statusLabel.attributedStringValue = [self statusAttributedString];
    const double total = (_model.total > 0) ? static_cast<double>(_model.total) : 1.0;
    [_progress setMaxValue:total];
    [_progress setDoubleValue:static_cast<double>(_model.done)];
}

- (void)updateControls {
    _startButton.enabled = !_model.running;
    _stopButton.enabled = _model.running && !_model.stopRequested;
    _exportButton.enabled = _model.hasResult && !_model.running;
    _matrixCountLabel.stringValue =
        [NSString stringWithFormat:@"%d/%d", _model.done, _model.total];
    _statsLabel.stringValue = [self statsText];
}

- (void)refreshAll {
    [self updateControls];
    [self updateStatusAndProgress];
    [self syncLayout];
    [_matrixView setNeedsDisplay:YES];
}

// One finished pair changes exactly three things: the status line, the progress
// bar (and the "12/56" counter plus the statistics they summarise) and that one
// cell.  Nothing else repaints (GUI_SPEC.md section 5).
- (void)refreshPairRow:(int)row column:(int)column {
    [self updateStatusAndProgress];
    _matrixCountLabel.stringValue =
        [NSString stringWithFormat:@"%d/%d", _model.done, _model.total];
    _statsLabel.stringValue = [self statsText];
    const NSRect cell = [_matrixView rectForRow:row column:column];
    if (!NSIsEmptyRect(cell)) {
        [_matrixView setNeedsDisplayInRect:cell];
    }
}

@end

// ---------------------------------------------------------------------------
// Application delegate: owns the window and drives the engine.
// ---------------------------------------------------------------------------

@class CLAppDelegate;

static void CLProgressThunk(void* context, const ClProgress* progress);
static void CLInstallMainMenu(CLAppDelegate* delegate);

@interface CLAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate,
                                     CLRootViewDelegate, NSMenuItemValidation>
@end

@interface CLAppDelegate ()
- (CLModel*)model;
- (void)handleProgress:(ClProgress)progress;
- (void)enqueueProgress:(ClProgress)progress;
- (void)drainProgress;
- (void)handleRunFinishedWithCode:(int)code
                           result:(ClResult)result
                          message:(NSString*)message;
- (void)startRun;
- (void)requestStop;
- (void)beginExportPanel;
- (BOOL)writeCurrentResultToPath:(NSString*)path message:(NSString**)message;
- (void)autoSaveCSV;
- (NSString*)resultsDirectory;
- (void)presentMessage:(NSString*)title info:(NSString*)info;
- (void)stopAndWaitForWorker;
@end

@implementation CLAppDelegate {
    NSWindow* _window;
    CLRootView* _rootView;
    ClAbort* _abort;
    std::thread _worker;
    ClResult _lastResult;
    BOOL _closing;

    // Progress coalescing: the engine signals every finished pair, but the
    // window is repainted at most once per kProgressInterval.
    std::mutex _progressMutex;
    ClProgress _pendingProgress;
    BOOL _hasPendingProgress;
    BOOL _progressScheduled;
    double _lastProgressDispatch;
}

- (instancetype)init {
    self = [super init];
    if (self != nil) {
        std::memset(&_lastResult, 0, sizeof(_lastResult));
        _closing = NO;
        _hasPendingProgress = NO;
        _progressScheduled = NO;
        _lastProgressDispatch = 0.0;
        _abort = cl_abort_create();
    }
    return self;
}

- (void)dealloc {
    if (_worker.joinable()) {
        if (_abort != nullptr) {
            cl_abort_request(_abort);
        }
        _worker.join();
    }
    if (_abort != nullptr) {
        cl_abort_destroy(_abort);
        _abort = nullptr;
    }
    cl_free_result(&_lastResult);
}

- (CLModel*)model {
    return (_rootView != nil) ? [_rootView model] : nullptr;
}

// --- startup --------------------------------------------------------------

- (void)applicationWillFinishLaunching:(NSNotification*)notification {
    (void)notification;
    CLInstallMainMenu(self);
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;

    _rootView = [[CLRootView alloc]
        initWithFrame:NSMakeRect(0, 0, kDefaultWidth, kDefaultHeight)];
    _rootView.delegate = self;
    CLModel* model = [_rootView model];

    ClCpuInfo info;
    std::memset(&info, 0, sizeof(info));
    if (cl_query_cpu(&info) == 0) {
        model->logical = std::max(info.logical, 2);
        model->physical = info.physical;
        model->cacheLine = info.cache_line;
        model->slotCore.assign(info.slot_core, info.slot_core + CL_MAX_SLOTS);
    } else {
        model->logical = 2;
        model->physical = 0;
        model->cacheLine = 64;
        model->slotCore.assign(CL_MAX_SLOTS, -1);
    }
    model->arch = kArchName;
    model->version = cl_version();
    model->resultsDir = CLToString([self resultsDirectory]);

    // SMT siblings come from the topology the engine reports.  macOS leaves
    // slot_core at -1, so no thread is ever marked as an SMT sibling there and
    // the SMT-only lines, asterisks and footnote stay hidden (GUI_SPEC.md 0).
    const int logical = model->logical;
    model->smtSibling.assign(static_cast<std::size_t>(logical), 0);
    model->hasSmt = false;
    model->smtText.clear();
    {
        std::vector<int> reported(static_cast<std::size_t>(logical), 0);
        std::string text;
        for (int slot = 0; slot < logical; ++slot) {
            const int core = model->slotCore[static_cast<std::size_t>(slot)];
            if (core < 0 || reported[static_cast<std::size_t>(slot)] != 0) {
                continue;
            }
            std::string group;
            int members = 0;
            for (int other = slot; other < logical; ++other) {
                if (model->slotCore[static_cast<std::size_t>(other)] != core) {
                    continue;
                }
                if (!group.empty()) {
                    group += "/";
                }
                group += "T" + std::to_string(other);
                reported[static_cast<std::size_t>(other)] = 1;
                ++members;
            }
            if (members < 2) {
                continue;
            }
            for (int other = slot; other < logical; ++other) {
                if (model->slotCore[static_cast<std::size_t>(other)] == core) {
                    model->smtSibling[static_cast<std::size_t>(other)] = 1;
                }
            }
            if (!text.empty()) {
                text += "、";
            }
            text += group;
            model->hasSmt = true;
        }
        model->smtText = text;
    }

    // No thread selector: a run always covers every logical thread, so the
    // matrix dimension is the logical thread count (Android/HarmonyOS behave the
    // same way, and the 固定参数 line states the real number).
    model->n = std::max(model->logical, 2);
    CLResetCells(model, model->n);
    model->status = CLStatusReady;

    [self buildWindow];
    [self updateFooter];
    [_rootView refreshAll];
    [_rootView syncLayout];
}

- (void)updateFooter {
    CLModel* model = [self model];
    if (model == nullptr) {
        return;
    }
    NSString* directory = CLStringFromUtf8(model->resultsDir);
    NSString* home = NSHomeDirectory();
    if (home.length > 0 && [directory hasPrefix:home]) {
        directory = [@"~" stringByAppendingString:[directory substringFromIndex:home.length]];
    }
    [_rootView setFooterText:[NSString stringWithFormat:
                                           @"CoherencyLatency %s · 结果自动保存到 %@",
                                           model->version.c_str(), directory]];
}

- (NSString*)resultsDirectory {
    NSString* documents =
        [NSHomeDirectory() stringByAppendingPathComponent:@"Documents"];
    return [documents stringByAppendingPathComponent:@"CoherencyLatency"];
}

- (void)buildWindow {
    const NSRect frame = NSMakeRect(0, 0, kDefaultWidth, kDefaultHeight);
    const NSWindowStyleMask style = NSWindowStyleMaskTitled |
                                    NSWindowStyleMaskClosable |
                                    NSWindowStyleMaskMiniaturizable |
                                    NSWindowStyleMaskResizable;
    _window = [[NSWindow alloc] initWithContentRect:frame
                                          styleMask:style
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    [_window setReleasedWhenClosed:NO];
    [_window setTitle:@"CoherencyLatency"];
    [_window setBackgroundColor:[NSColor windowBackgroundColor]];
    // Behind-window vibrancy needs a non-opaque window; the material still owns
    // the visible colour, and windowBackgroundColor remains the fallback.
    [_window setOpaque:NO];
    [_window setDelegate:self];
    [_window setContentMinSize:NSMakeSize(kMinWidth, kMinHeight)];
    _window.titlebarAppearsTransparent = YES;

    NSVisualEffectView* backdrop =
        [[NSVisualEffectView alloc] initWithFrame:NSMakeRect(0, 0, kDefaultWidth,
                                                             kDefaultHeight)];
    backdrop.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    backdrop.state = NSVisualEffectStateFollowsWindowActiveState;
    // NSVisualEffectMaterialWindowBackground is 10.14+ (it shares the enum's
    // raw value 12); NSVisualEffectMaterialTitlebar is the pre-10.14 window
    // chrome material.  The numeric form avoids an availability thunk that the
    // osxcross libSystem stub does not export.
    const NSOperatingSystemVersion windowMaterialSince = {10, 14, 0};
    if ([[NSProcessInfo processInfo] isOperatingSystemAtLeastVersion:windowMaterialSince]) {
        backdrop.material = (NSVisualEffectMaterial)12;
    } else {
        backdrop.material = NSVisualEffectMaterialTitlebar;
    }
    backdrop.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

    _rootView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [_rootView setFrame:NSMakeRect(0, 0, kDefaultWidth, kDefaultHeight)];
    [backdrop addSubview:_rootView];
    [_window setContentView:backdrop];
    [_window setContentSize:NSMakeSize(kDefaultWidth, kDefaultHeight)];
    [_window center];

    // The window remembers its size, but this is a PORTRAIT design: a restored
    // frame that is wider than it is tall (a leftover from the older landscape
    // build), or smaller than the minimum, is replaced by the default size.
    [_window setFrameAutosaveName:@"CoherencyLatencyMainWindow"];
    const NSRect restored = [_window contentRectForFrameRect:[_window frame]];
    if (NSWidth(restored) < kMinWidth - 0.5 ||
        NSHeight(restored) < kMinHeight - 0.5 ||
        NSWidth(restored) > NSHeight(restored)) {
        [_window setContentSize:NSMakeSize(kDefaultWidth, kDefaultHeight)];
        [_window center];
    }
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

// --- CLRootViewDelegate ---------------------------------------------------

- (void)rootViewDidPressStart:(CLRootView*)view {
    (void)view;
    [self startRun];
}

- (void)rootViewDidPressStop:(CLRootView*)view {
    (void)view;
    [self requestStop];
}

- (void)rootViewDidPressExport:(CLRootView*)view {
    (void)view;
    [self beginExportPanel];
}

// --- menu -----------------------------------------------------------------

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    CLModel* model = [self model];
    if (model == nullptr) {
        return NO;
    }
    if (item.action == @selector(startTest:)) {
        return !model->running;
    }
    if (item.action == @selector(stopTest:)) {
        return model->running && !model->stopRequested;
    }
    if (item.action == @selector(exportCSV:)) {
        return model->hasResult && !model->running;
    }
    return YES;
}

- (void)startTest:(id)sender {
    (void)sender;
    [self startRun];
}

- (void)stopTest:(id)sender {
    (void)sender;
    [self requestStop];
}

- (void)exportCSV:(id)sender {
    (void)sender;
    [self beginExportPanel];
}

// --- run ------------------------------------------------------------------

- (void)startRun {
    CLModel* model = [self model];
    if (model == nullptr || model->running) {
        return;
    }
    if (_worker.joinable()) {
        _worker.join();
    }
    cl_free_result(&_lastResult);
    std::memset(&_lastResult, 0, sizeof(_lastResult));

    CLResetCells(model, model->n);
    model->status = CLStatusRunning;
    model->running = true;
    model->stopRequested = false;
    model->hasResult = false;
    model->errorText.clear();
    model->elapsed = 0.0;
    model->lastLatency = std::numeric_limits<float>::quiet_NaN();

    if (_abort != nullptr) {
        cl_abort_destroy(_abort);
    }
    _abort = cl_abort_create();

    [_rootView refreshAll];

    ClRunConfig config;
    std::memset(&config, 0, sizeof(config));
    config.threads = model->n;
    config.iterations = kIterations;
    config.runs = kRuns;
    config.owned = 0;          // Mode::Bounce
    config.use_affinity = 1;

    CLAppDelegate* strongSelf = self;
    ClAbort* abortFlag = _abort;
    _worker = std::thread([strongSelf, config, abortFlag]() {
        ClResult result;
        std::memset(&result, 0, sizeof(result));
        char error[256] = {0};
        const int code = cl_run_matrix(&config, &CLProgressThunk,
                                       (__bridge void*)strongSelf, abortFlag,
                                       &result, error,
                                       static_cast<int>(sizeof(error)));
        NSString* message =
            (error[0] != '\0') ? [NSString stringWithUTF8String:error] : @"";
        dispatch_async(dispatch_get_main_queue(), ^{
            [strongSelf handleRunFinishedWithCode:code result:result
                                          message:message];
        });
    });
}

- (void)requestStop {
    CLModel* model = [self model];
    if (model == nullptr || !model->running || model->stopRequested) {
        return;
    }
    model->stopRequested = true;
    cl_abort_request(_abort);
    [_rootView refreshAll];
}

static void CLProgressThunk(void* context, const ClProgress* progress) {
    if (context == nullptr || progress == nullptr) {
        return;
    }
    CLAppDelegate* delegate = (__bridge CLAppDelegate*)context;
    const ClProgress snapshot = *progress;
    [delegate enqueueProgress:snapshot];
}

// Main queue: deliver the newest coalesced snapshot.
- (void)drainProgress {
    ClProgress progress;
    BOOL has = NO;
    {
        std::lock_guard<std::mutex> lock(_progressMutex);
        _progressScheduled = NO;
        if (_hasPendingProgress) {
            progress = _pendingProgress;
            _hasPendingProgress = NO;
            has = YES;
            _lastProgressDispatch = CFAbsoluteTimeGetCurrent();
        }
    }
    if (has) {
        [self handleProgress:progress];
    }
}

// Worker thread: keep only the latest snapshot; at most one main-queue block
// is in flight, and consecutive deliveries are at least kProgressInterval
// apart.  The final state is always delivered by handleRunFinished.
- (void)enqueueProgress:(ClProgress)progress {
    BOOL schedule = NO;
    double delay = 0.0;
    {
        std::lock_guard<std::mutex> lock(_progressMutex);
        _pendingProgress = progress;
        _hasPendingProgress = YES;
        if (!_progressScheduled) {
            _progressScheduled = YES;
            const double now = CFAbsoluteTimeGetCurrent();
            const double earliest = _lastProgressDispatch + kProgressInterval;
            delay = (earliest > now) ? (earliest - now) : 0.0;
            schedule = YES;
        }
    }
    if (!schedule) {
        return;
    }
    CLAppDelegate* strongSelf = self;
    if (delay > 0.0) {
        const int64_t when = static_cast<int64_t>(delay * 1e9);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, when),
                       dispatch_get_main_queue(), ^{
                           [strongSelf drainProgress];
                       });
    } else {
        dispatch_async(dispatch_get_main_queue(), ^{
            [strongSelf drainProgress];
        });
    }
}

- (void)handleProgress:(ClProgress)progress {
    if (_closing) {
        return;
    }
    CLModel* model = [self model];
    if (model == nullptr || !model->running) {
        return;
    }
    if (progress.i >= 0 && progress.i < model->n && progress.j >= 0 &&
        progress.j < model->n) {
        model->cells[static_cast<std::size_t>(progress.i) * model->n +
                     static_cast<std::size_t>(progress.j)] = progress.latency_ns;
        ++model->statsRevision;
    }
    model->done = progress.done;
    model->total = progress.total;
    model->lastI = progress.i;
    model->lastJ = progress.j;
    model->lastLatency = progress.latency_ns;
    model->elapsed = progress.elapsed_seconds;
    // Only the pair that just finished plus the status/progress band change.
    [_rootView refreshPairRow:progress.i column:progress.j];
}

- (void)handleRunFinishedWithCode:(int)code
                           result:(ClResult)result
                          message:(NSString*)message {
    // The worker dispatched this block just before returning from its thread
    // body, so the main thread does not join it here: the join happens on the
    // next start or when the window closes, which keeps the UI thread free.
    if (_closing) {
        cl_free_result(&result);
        return;
    }
    CLModel* model = [self model];
    if (model == nullptr) {
        cl_free_result(&result);
        return;
    }
    model->running = false;
    model->stopRequested = false;

    if (code != 0) {
        model->status = CLStatusFailed;
        model->errorText = CLToString(message.length > 0 ? message : @"未知错误");
        cl_free_result(&result);
        [_rootView refreshAll];
        return;
    }

    if (result.cells != nullptr && result.n == model->n) {
        const std::size_t count = static_cast<std::size_t>(result.n) *
                                  static_cast<std::size_t>(result.n);
        model->cells.assign(result.cells, result.cells + count);
        ++model->statsRevision;
        model->statsCacheValid = false;
    }
    model->done = result.pairs_done;
    model->total = result.pairs_total;
    model->elapsed = result.elapsed_seconds;
    model->hasResult = (result.pairs_done > 0);
    model->status = (result.aborted != 0) ? CLStatusStopped : CLStatusDone;
    model->lastI = -1;
    model->lastJ = -1;

    cl_free_result(&_lastResult);
    _lastResult = result;  // ownership of cells moves to the delegate

    [_rootView refreshAll];
    // GUI_SPEC.md section 5: the CSV is written automatically when a run
    // finishes; a stopped run keeps its partial matrix for 导出 CSV.
    if (model->status == CLStatusDone && model->hasResult) {
        [self autoSaveCSV];
    }
}

// --- CSV ------------------------------------------------------------------

- (BOOL)writeCurrentResultToPath:(NSString*)path message:(NSString**)message {
    if (_lastResult.cells == nullptr) {
        if (message != nullptr) {
            *message = @"没有可导出的结果";
        }
        return NO;
    }
    char error[256] = {0};
    const char* utf8 = [path fileSystemRepresentation];
    if (utf8 == nullptr) {
        if (message != nullptr) {
            *message = @"路径无效";
        }
        return NO;
    }
    const int code =
        cl_write_csv(&_lastResult, utf8, error, static_cast<int>(sizeof(error)));
    if (code != 0) {
        if (message != nullptr) {
            *message = (error[0] != '\0') ? [NSString stringWithUTF8String:error]
                                          : @"写入失败";
        }
        return NO;
    }
    return YES;
}

- (void)autoSaveCSV {
    NSString* directory = [self resultsDirectory];
    NSFileManager* manager = [NSFileManager defaultManager];
    NSError* error = nil;
    if (![manager createDirectoryAtPath:directory
            withIntermediateDirectories:YES
                             attributes:nil
                                  error:&error]) {
        [self presentMessage:@"无法创建结果目录"
                        info:(error != nil ? [error localizedDescription] : directory)];
        return;
    }
    char name[256] = {0};
    if (cl_default_csv_name(name, static_cast<int>(sizeof(name))) != 0) {
        [self presentMessage:@"无法生成 CSV 文件名" info:directory];
        return;
    }
    NSString* fileName = [NSString stringWithUTF8String:name];
    NSString* path = [directory stringByAppendingPathComponent:fileName];
    NSString* failure = nil;
    if (![self writeCurrentResultToPath:path message:&failure]) {
        [self presentMessage:@"自动保存 CSV 失败" info:(failure != nil ? failure : path)];
    }
}

- (void)beginExportPanel {
    if (_lastResult.cells == nullptr) {
        return;
    }
    NSSavePanel* panel = [NSSavePanel savePanel];
    [panel setTitle:@"导出 CSV"];
    [panel setNameFieldLabel:@"存储为："];
    [panel setPrompt:@"导出"];
    [panel setAllowedFileTypes:@[ @"csv" ]];
    [panel setCanCreateDirectories:YES];
    [panel setDirectoryURL:[NSURL fileURLWithPath:[self resultsDirectory]]];
    char name[256] = {0};
    if (cl_default_csv_name(name, static_cast<int>(sizeof(name))) == 0) {
        [panel setNameFieldStringValue:[NSString stringWithUTF8String:name]];
    }
    CLAppDelegate* strongSelf = self;
    [panel beginSheetModalForWindow:_window
                  completionHandler:^(NSModalResponse response) {
                      if (response != NSModalResponseOK) {
                          return;
                      }
                      NSURL* url = [panel URL];
                      if (url == nil) {
                          return;
                      }
                      NSString* failure = nil;
                      if (![strongSelf writeCurrentResultToPath:[url path]
                                                        message:&failure]) {
                          [strongSelf presentMessage:@"导出 CSV 失败"
                                                info:(failure != nil ? failure
                                                                     : [url path])];
                      }
                  }];
}

- (void)presentMessage:(NSString*)title info:(NSString*)info {
    NSAlert* alert = [[NSAlert alloc] init];
    [alert setMessageText:title];
    [alert setInformativeText:(info != nil ? info : @"")];
    [alert addButtonWithTitle:@"好"];
    if (_window != nil) {
        [alert beginSheetModalForWindow:_window completionHandler:nil];
    } else {
        [alert runModal];
    }
}

// --- lifecycle ------------------------------------------------------------

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;
    return YES;
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    (void)sender;
    [self stopAndWaitForWorker];
    return NSTerminateNow;
}

// GUI_SPEC.md section 3: the layout must react to window resizing.  The root
// view's autoresizing already triggers syncLayout, this keeps it explicit.
- (void)windowDidResize:(NSNotification*)notification {
    (void)notification;
    [_rootView syncLayout];
}

- (void)windowWillClose:(NSNotification*)notification {
    (void)notification;
    [self stopAndWaitForWorker];
}

// GUI_SPEC.md section 5: closing the window while a run is active aborts it and
// waits for the worker thread before exiting.
- (void)stopAndWaitForWorker {
    _closing = YES;
    if (_worker.joinable()) {
        CLModel* model = [self model];
        if (model != nullptr) {
            model->stopRequested = true;
        }
        if (_abort != nullptr) {
            cl_abort_request(_abort);
        }
        _worker.join();
    }
}

@end

// ---------------------------------------------------------------------------
// Main menu: the four standard Mac menus plus the test actions.
// ---------------------------------------------------------------------------

static NSMenuItem* CLAddItem(NSMenu* menu, NSString* title, SEL action,
                             NSString* key) {
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title
                                                  action:action
                                           keyEquivalent:(key != nil ? key : @"")];
    [menu addItem:item];
    return item;
}

static void CLInstallMainMenu(CLAppDelegate* delegate) {
    NSMenu* mainMenu = [[NSMenu alloc] initWithTitle:@""];

    // App menu.
    NSMenuItem* appItem = [[NSMenuItem alloc] initWithTitle:@"CoherencyLatency"
                                                    action:nil
                                             keyEquivalent:@""];
    NSMenu* appMenu = [[NSMenu alloc] initWithTitle:@"CoherencyLatency"];
    CLAddItem(appMenu, @"关于 CoherencyLatency",
              @selector(orderFrontStandardAboutPanel:), @"");
    [appMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* hide = CLAddItem(appMenu, @"隐藏 CoherencyLatency",
                                 @selector(hide:), @"h");
    [hide setTarget:NSApp];
    [appMenu addItem:[NSMenuItem separatorItem]];
    CLAddItem(appMenu, @"退出 CoherencyLatency", @selector(terminate:), @"q");
    [appItem setSubmenu:appMenu];
    [mainMenu addItem:appItem];

    // 编辑 menu (standard responder actions).
    NSMenuItem* editItem = [[NSMenuItem alloc] initWithTitle:@"编辑"
                                                      action:nil
                                               keyEquivalent:@""];
    NSMenu* editMenu = [[NSMenu alloc] initWithTitle:@"编辑"];
    CLAddItem(editMenu, @"撤销", @selector(undo:), @"z");
    NSMenuItem* redo = CLAddItem(editMenu, @"重做", @selector(redo:), @"Z");
    [redo setKeyEquivalentModifierMask:(NSEventModifierFlagCommand |
                                       NSEventModifierFlagShift)];
    [editMenu addItem:[NSMenuItem separatorItem]];
    CLAddItem(editMenu, @"剪切", @selector(cut:), @"x");
    CLAddItem(editMenu, @"复制", @selector(copy:), @"c");
    CLAddItem(editMenu, @"粘贴", @selector(paste:), @"v");
    CLAddItem(editMenu, @"全选", @selector(selectAll:), @"a");
    [editItem setSubmenu:editMenu];
    [mainMenu addItem:editItem];

    // 测试 menu: the app-specific commands.
    NSMenuItem* testItem = [[NSMenuItem alloc] initWithTitle:@"测试"
                                                      action:nil
                                               keyEquivalent:@""];
    NSMenu* testMenu = [[NSMenu alloc] initWithTitle:@"测试"];
    NSMenuItem* start = CLAddItem(testMenu, @"开始测试", @selector(startTest:), @"r");
    [start setTarget:delegate];
    NSMenuItem* stop = CLAddItem(testMenu, @"停止", @selector(stopTest:), @".");
    [stop setTarget:delegate];
    [testMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* exportItem = CLAddItem(testMenu, @"导出 CSV", @selector(exportCSV:),
                                       @"s");
    [exportItem setTarget:delegate];
    [testItem setSubmenu:testMenu];
    [mainMenu addItem:testItem];

    // 窗口 menu.
    NSMenuItem* windowItem = [[NSMenuItem alloc] initWithTitle:@"窗口"
                                                        action:nil
                                                 keyEquivalent:@""];
    NSMenu* windowMenu = [[NSMenu alloc] initWithTitle:@"窗口"];
    CLAddItem(windowMenu, @"最小化", @selector(performMiniaturize:), @"m");
    CLAddItem(windowMenu, @"缩放", @selector(performZoom:), @"");
    [windowMenu addItem:[NSMenuItem separatorItem]];
    CLAddItem(windowMenu, @"前置全部窗口", @selector(arrangeInFront:), @"");
    [windowItem setSubmenu:windowMenu];
    [mainMenu addItem:windowItem];
    [NSApp setWindowsMenu:windowMenu];

    [NSApp setMainMenu:mainMenu];
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

// NSApplication keeps its delegate unowned, so the delegate is held here for the
// whole process lifetime instead of in an ARC local that could die at last use.
static CLAppDelegate* gAppDelegate = nil;

int main(int argc, const char* argv[]) {
    (void)argc;
    (void)argv;
    @autoreleasepool {
        NSApplication* application = [NSApplication sharedApplication];
        [application setActivationPolicy:NSApplicationActivationPolicyRegular];
        gAppDelegate = [[CLAppDelegate alloc] init];
        [application setDelegate:gAppDelegate];
        [application run];
    }
    return 0;
}
