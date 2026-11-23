package com.ddyibb.coherencylatency;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.SystemClock;
import android.text.SpannableString;
import android.text.Spanned;
import android.text.style.ForegroundColorSpan;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.ViewGroup;
import android.view.Window;
import android.widget.Button;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.Charset;
import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Date;
import java.util.Locale;

/**
 * Single-screen Android front end for the CoherencyLatency benchmark.
 *
 * <p>All numbers come from the native core in {@code clbench_jni.cpp}; this
 * activity only reflects the matrix while the run is in progress and writes the
 * finished CSV.  The layout adapts to phones, tablets, round watches and
 * Android TV: large screens use a two-column layout, watches use a compact
 * scrollable layout, and every action button is focusable for D-pad remotes.</p>
 */
public class MainActivity extends Activity {

    private static final int REQUEST_EXPORT = 1001;

    /** Present since API 1; avoids the API 19 StandardCharsets class. */
    private static final Charset ASCII = Charset.forName("US-ASCII");

    /** Fixed test parameters: the app always runs with the defaults. */
    private static final long DEFAULT_ITERATIONS = 1000000L;
    private static final int DEFAULT_RUNS = 3;
    private static final boolean DEFAULT_OWNED = false;

    private static final int BACKGROUND = 0xFFF4F6F9;
    private static final int CARD = 0xFFFFFFFF;
    private static final int TEXT_PRIMARY = 0xFF101828;
    private static final int TEXT_SECONDARY = 0xFF667085;
    private static final int TEXT_TERTIARY = 0xFF98A2B3;
    private static final int ACCENT = 0xFF2563EB;
    private static final int ACCENT_DARK = 0xFF1E40AF;
    private static final int ACCENT_BORDER = 0xFFBFD3FE;
    private static final int BORDER = 0xFFE4E7EC;
    private static final int FIELD = 0xFFF8FAFC;
    private static final int STOP_BACKGROUND = 0xFFF2F4F7;
    private static final int FOCUS_BACKGROUND = 0xFFEAF1FF;

    /** Matrix heat scale, matched to the desktop UI: green -> orange -> red. */
    private static final int HEAT_GREEN = 0xFF16B769;
    private static final int HEAT_ORANGE = 0xFFF48011;
    private static final int HEAT_RED = 0xFFF83533;

    private static final byte CELL_STALE = 0;
    private static final byte CELL_VALUE = 1;
    private static final byte CELL_NA = 2;
    private static final byte CELL_PENDING = 3;
    private static final byte CELL_DIAGONAL = 4;

    private ScrollView scrollView;
    private TextView infoText;
    private TextView statusText;
    private TextView matrixText;
    private TextView matrixCaption;
    private TextView matrixNote;
    private TextView savedText;
    private ProgressBar progressBar;
    private Button startButton;
    private Button stopButton;
    private Button exportButton;

    private int logicalCores = 2;
    private int n = 2;
    private float[] live = new float[4];

    /** Cached cell text for the matrix; entries are only re-formatted when their value changes. */
    private int matrixCacheN = -1;
    private int matrixCacheWidth = -1;
    private String[] matrixCells;
    private byte[] matrixCellKind;
    private float[] matrixCellValue;
    private String[] matrixColumnHeaders;
    private String[] matrixRowHeaders;
    private int naPairs = 0;

    /** Progress updates are marshalled through one reusable Runnable (no per-pair allocation). */
    private int progressFirst;
    private int progressSecond;
    private float progressLatency;
    private int progressDone;
    private int progressTotal;
    private final StringBuilder statusBuilder = new StringBuilder(96);
    private boolean running = false;
    private int donePairs = 0;
    private int totalPairs = 0;
    private long startMillis = 0;
    private String lastCsv = null;
    private String lastCsvName = null;

    private boolean isWatch;
    private boolean isTelevision;
    private boolean isRound;
    private boolean isWide;
    private float uiScale = 1f;

    private final Runnable progressUpdate = new Runnable() {
        @Override
        public void run() {
            if (!running) {
                return;
            }
            final int first = progressFirst;
            final int second = progressSecond;
            final float latencyNs = progressLatency;
            final int done = progressDone;
            final int total = progressTotal;
            donePairs = done;
            totalPairs = total;
            if (progressBar.getMax() != total) {
                progressBar.setMax(total);
            }
            progressBar.setProgress(done);
            statusBuilder.setLength(0);
            statusBuilder.append("测量中 ").append(done).append('/').append(total)
                    .append(" · C").append(first).append("↔C").append(second);
            if (latencyNs != latencyNs) {
                statusBuilder.append(" N/A（绑核失败）");
            } else {
                statusBuilder.append(' ');
                appendTenths(statusBuilder, latencyNs);
                statusBuilder.append(" ns · 用时 ");
                appendTenths(statusBuilder, elapsedSeconds());
                statusBuilder.append(" s");
            }
            statusText.setText(statusBuilder.toString());
            renderMatrix(false);
        }
    };

    private final NativeBench.Progress progressCallback =
            new NativeBench.Progress() {
                @Override
                public void onPair(int first, int second, float latencyNs,
                                   int done, int total) {
                    // Store the sample immediately: if several callbacks arrive
                    // before the UI thread drains its queue, the shared fields
                    // only keep the newest values but no matrix entry is lost.
                    if (live != null && n > 0 && first < n && second < n) {
                        live[first * n + second] = latencyNs;
                    }
                    progressFirst = first;
                    progressSecond = second;
                    progressLatency = latencyNs;
                    progressDone = done;
                    progressTotal = total;
                    runOnUiThread(progressUpdate);
                }
            };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        readProfile();
        setContentView(buildContent());
        applyWindowFlags();
        configureFromNative();
        requestTvFocus();
    }

    @Override
    protected void onDestroy() {
        if (running) {
            NativeBench.abort();
        }
        super.onDestroy();
    }

    private void applyWindowFlags() {
        Window window = getWindow();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            window.setStatusBarColor(BACKGROUND);
        }
        int flags = 0;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            flags |= View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR;
        }
        if (isWatch || isTelevision) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                // Modern edge-to-edge path: the legacy flags are unreliable on
                // Android 11+ and can leave a nav-bar inset on TV emulators.
                Api30Bars.enterImmersive(window);
            } else {
                flags |= View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION;
            }
        }
        window.getDecorView().setSystemUiVisibility(flags);
    }

    /** isScreenRound() arrived in API 23; keep it out of the main class body. */
    private static final class Api23Round {
        static boolean isRound(Configuration configuration) {
            return configuration.isScreenRound();
        }
    }

    /**
     * Android 11+ system-bar API kept in its own class: on Android 4.x the
     * verifier would reject a method that mentions WindowInsetsController, so
     * nothing here is ever loaded below API 30.
     */
    private static final class Api30Bars {
        static void enterImmersive(final Window window) {
            window.setDecorFitsSystemWindows(false);
            WindowInsetsController controller = window.getDecorView().isAttachedToWindow()
                    ? window.getInsetsController() : null;
            if (controller != null) {
                controller.hide(WindowInsets.Type.statusBars()
                        | WindowInsets.Type.navigationBars());
                controller.setSystemBarsBehavior(
                        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
            window.getDecorView().post(new Runnable() {
                @Override
                public void run() {
                    WindowInsetsController late = window.getInsetsController();
                    if (late != null) {
                        late.hide(WindowInsets.Type.statusBars()
                                | WindowInsets.Type.navigationBars());
                        late.setSystemBarsBehavior(
                                WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                    }
                }
            });
        }
    }

    private void readProfile() {
        Configuration configuration = getResources().getConfiguration();
        int uiMode = configuration.uiMode & Configuration.UI_MODE_TYPE_MASK;
        int widthDp = configuration.screenWidthDp;
        int heightDp = configuration.screenHeightDp;
        // Debug-only profile override so the TV/watch layouts can be checked on
        // an ordinary phone emulator. Release builds ignore the extra entirely.
        String forced = null;
        if ((getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0) {
            forced = getIntent().getStringExtra("force_profile");
        }
        boolean forceTv = "tv".equals(forced);
        boolean forceWatch = "watch".equals(forced) || "watch_round".equals(forced);
        isTelevision = forceTv || (!forceWatch && (uiMode == Configuration.UI_MODE_TYPE_TELEVISION
                || getPackageManager().hasSystemFeature(PackageManager.FEATURE_LEANBACK)));
        isWatch = forceWatch || (!forceTv && (uiMode == Configuration.UI_MODE_TYPE_WATCH
                || (widthDp <= 280 && heightDp <= 280)));
        isRound = "watch_round".equals(forced)
                || (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M
                && Api23Round.isRound(configuration));
        isWide = !isWatch && widthDp >= 600 && widthDp > heightDp;
        uiScale = isWatch ? 0.80f : isTelevision ? 1.70f : 1.0f;
    }

    // -----------------------------------------------------------------------
    // Layout
    // -----------------------------------------------------------------------

    private View buildContent() {
        scrollView = new ScrollView(this);
        scrollView.setFillViewport(isTelevision);
        scrollView.setBackgroundColor(BACKGROUND);
        scrollView.setClipToPadding(false);
        scrollView.setVerticalScrollBarEnabled(!isWatch);
        scrollView.setFocusable(false);
        scrollView.setDescendantFocusability(ViewGroup.FOCUS_AFTER_DESCENDANTS);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        if (isTelevision) {
            root.setGravity(Gravity.CENTER_VERTICAL);
        }
        int horizontalPadding = isWatch
                ? (isRound ? 16 : 10)
                : isTelevision ? 44 : isWide ? 28 : 20;
        int verticalPadding = isWatch ? (isRound ? 32 : 10) : isTelevision ? 28 : 18;
        int bottomPadding = isWatch ? (isRound ? 56 : 16) : isTelevision ? 36 : 30;
        root.setPadding(dp(horizontalPadding), dp(verticalPadding),
                dp(horizontalPadding), dp(bottomPadding));
        scrollView.addView(root, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        if (isWide) {
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setBaselineAligned(false);
            LinearLayout left = column();
            LinearLayout right = column();
            row.addView(left, new LinearLayout.LayoutParams(
                    0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f));
            LinearLayout.LayoutParams rightParams = new LinearLayout.LayoutParams(
                    0, ViewGroup.LayoutParams.WRAP_CONTENT,
                    isTelevision ? 1.15f : 1.05f);
            rightParams.leftMargin = dp(isTelevision ? 32 : 20);
            row.addView(right, rightParams);
            root.addView(row, new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT));

            addHeader(left);
            addInfoCard(left);
            addActionButtons(left);
            addProgress(left);
            addExport(left);
            addMatrix(right, 0);
            addFooter(right);
        } else if (isWatch) {
            // On a watch the first screen must expose Start/Stop; device
            // details and the matrix stay below the fold.
            addHeader(root);
            addActionButtons(root);
            addProgress(root);
            addMatrix(root, 8);
            addExport(root);
            addInfoCard(root);
        } else {
            addHeader(root);
            addInfoCard(root);
            addActionButtons(root);
            addProgress(root);
            addMatrix(root, 16);
            addExport(root);
            addFooter(root);
        }

        return scrollView;
    }

    private LinearLayout column() {
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        return column;
    }

    private void addHeader(LinearLayout parent) {
        TextView title = text("CoherencyLatency", isWatch ? 23f : 27f,
                TEXT_PRIMARY, true);
        if (isWatch) {
            title.setGravity(Gravity.CENTER_HORIZONTAL);
        }
        parent.addView(title);

        TextView subtitle = text(
                isWatch ? "缓存一致性测试" : "缓存一致性延迟测试 · Android 原生版",
                isWatch ? 12f : 13f, TEXT_SECONDARY, false);
        if (isWatch) {
            subtitle.setGravity(Gravity.CENTER_HORIZONTAL);
        }
        LinearLayout.LayoutParams subtitleParams = wrapParams();
        subtitleParams.topMargin = dp(2);
        parent.addView(subtitle, subtitleParams);
    }

    private void addInfoCard(LinearLayout parent) {
        LinearLayout infoCard = card(isWatch ? 10 : 16);
        if (!isWatch) {
            infoCard.addView(sectionTitle("设备信息"));
        }
        infoText = text("读取中…", isWatch ? 11f : 12.5f, TEXT_SECONDARY, false);
        infoText.setTypeface(Typeface.MONOSPACE);
        infoText.setLineSpacing(dp(isWatch ? 2 : 3), 1.0f);
        infoText.setPadding(0, dp(isWatch ? 2 : 8), 0, 0);
        infoCard.addView(infoText);

        if (!isWatch) {
            TextView fixedParams = text(
                    "固定参数：全部逻辑核心 · 1,000,000 次 · 3 轮 · Bounce",
                    11.5f, TEXT_TERTIARY, false);
            fixedParams.setPadding(0, dp(6), 0, 0);
            infoCard.addView(fixedParams);
        }
        parent.addView(infoCard);
    }

    private void addActionButtons(LinearLayout parent) {
        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams buttonsParams = wrapParams();
        buttonsParams.topMargin = dp(isWatch ? 10 : 16);
        parent.addView(buttons, buttonsParams);

        startButton = actionButton("开始测试", ACCENT, Color.WHITE, 0, true);
        startButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startTest();
            }
        });
        LinearLayout.LayoutParams startParams = new LinearLayout.LayoutParams(
                0, dp(buttonHeightDp()), 2f);
        buttons.addView(startButton, startParams);

        stopButton = actionButton("停止", STOP_BACKGROUND, TEXT_PRIMARY, BORDER, false);
        stopButton.setEnabled(false);
        stopButton.setAlpha(0.5f);
        stopButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                stopTest();
            }
        });
        LinearLayout.LayoutParams stopParams = new LinearLayout.LayoutParams(
                0, dp(buttonHeightDp()), 1f);
        stopParams.leftMargin = dp(isWatch ? 8 : 10);
        buttons.addView(stopButton, stopParams);
    }

    private void addProgress(LinearLayout parent) {
        progressBar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progressBar.setMax(1);
        progressBar.setProgress(0);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            progressBar.setProgressTintList(ColorStateList.valueOf(ACCENT));
            progressBar.setProgressBackgroundTintList(ColorStateList.valueOf(BORDER));
        }
        LinearLayout.LayoutParams barParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(isWatch ? 4 : isTelevision ? 8 : 6));
        barParams.topMargin = dp(isWatch ? 10 : isTelevision ? 22 : 18);
        parent.addView(progressBar, barParams);

        statusText = text("就绪", 12.5f, TEXT_SECONDARY, false);
        LinearLayout.LayoutParams statusParams = wrapParams();
        statusParams.topMargin = dp(isWatch ? 6 : 8);
        parent.addView(statusText, statusParams);
    }

    private void addMatrix(LinearLayout parent, int topMarginDp) {
        LinearLayout matrixCard = card(topMarginDp);
        matrixCaption = sectionTitle("结果矩阵 · ns/op");
        matrixCard.addView(matrixCaption);

        HorizontalScrollView hScroll = new HorizontalScrollView(this);
        hScroll.setHorizontalScrollBarEnabled(false);
        hScroll.setFocusable(false);
        LinearLayout.LayoutParams scrollParams = wrapParams();
        scrollParams.topMargin = dp(isWatch ? 6 : 8);
        matrixCard.addView(hScroll, scrollParams);

        matrixText = text("", isWatch ? 10f : 12.5f, TEXT_PRIMARY, false);
        matrixText.setTypeface(Typeface.MONOSPACE);
        matrixText.setTextIsSelectable(false);
        matrixText.setFocusable(false);
        matrixText.setClickable(false);
        matrixText.setLongClickable(false);
        matrixText.setLineSpacing(dp(isWatch ? 3 : 5), 1.0f);
        hScroll.addView(matrixText, new HorizontalScrollView.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        matrixNote = text("N/A = 该核心对未能绑定到指定 CPU", isWatch ? 9.5f : 11f,
                TEXT_TERTIARY, false);
        matrixNote.setVisibility(View.GONE);
        LinearLayout.LayoutParams noteParams = wrapParams();
        noteParams.topMargin = dp(6);
        matrixCard.addView(matrixNote, noteParams);
        parent.addView(matrixCard);
    }

    private void addExport(LinearLayout parent) {
        exportButton = actionButton("导出 CSV…", CARD, ACCENT, ACCENT_BORDER, false);
        exportButton.setEnabled(false);
        exportButton.setAlpha(0.5f);
        exportButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                exportCsv();
            }
        });
        LinearLayout.LayoutParams exportParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(buttonHeightDp()));
        exportParams.topMargin = dp(isWatch ? 8 : 12);
        parent.addView(exportButton, exportParams);

        savedText = text(" ", 11.5f, TEXT_TERTIARY, false);
        LinearLayout.LayoutParams savedParams = wrapParams();
        savedParams.topMargin = dp(6);
        parent.addView(savedText, savedParams);
        setSavedMessage(null);
    }

    private void addFooter(LinearLayout parent) {
        if (isWatch) {
            return;
        }
        TextView footer = text("数值是每次原子操作的往返延迟，越低越好。导出后可导入表格软件。",
                11.5f, TEXT_TERTIARY, false);
        LinearLayout.LayoutParams footerParams = wrapParams();
        footerParams.topMargin = dp(isTelevision ? 24 : 18);
        parent.addView(footer, footerParams);
    }

    private void configureFromNative() {
        String raw = NativeBench.deviceInfo();
        String model = "Android";
        int logical = 2;
        int cacheLine = 64;
        String abi = "unknown";
        int physical = 0;
        if (raw != null) {
            String[] parts = raw.split("\\|");
            if (parts.length > 0 && !parts[0].isEmpty()) {
                model = parts[0];
            }
            if (parts.length > 1) {
                logical = safeInt(parts[1], 2);
            }
            if (parts.length > 2) {
                cacheLine = safeInt(parts[2], 64);
            }
            if (parts.length > 3) {
                abi = parts[3];
            }
            if (parts.length > 4) {
                physical = safeInt(parts[4], 0);
            }
        }
        logical = Math.max(1, Math.min(logical, 32));
        String deviceLine = model + "\n" + logical + " 个逻辑核心";
        if (physical > 0) {
            deviceLine += " · " + physical + " 个物理核";
        }
        deviceLine += " · " + cacheLine + " B 缓存行";
        if (!isWatch) {
            deviceLine += " · " + abi;
        }
        infoText.setText(deviceLine);

        logicalCores = logical;
        n = logicalCores;
        live = new float[n * n];
        Arrays.fill(live, -1f);
        renderMatrix(false);
        statusText.setText("就绪 · " + doneSummary());
    }

    // -----------------------------------------------------------------------
    // Test lifecycle
    // -----------------------------------------------------------------------

    private void startTest() {
        if (running) {
            return;
        }
        final int cores = logicalCores;
        final long iterations = DEFAULT_ITERATIONS;
        final int runs = DEFAULT_RUNS;
        final boolean owned = DEFAULT_OWNED;

        n = cores;
        live = new float[cores * cores];
        Arrays.fill(live, -1f);
        donePairs = 0;
        totalPairs = cores * (cores - 1);
        running = true;
        lastCsv = null;
        lastCsvName = null;
        setSavedMessage(null);

        matrixCaption.setText("实时原始值 · ns/op");
        matrixNote.setVisibility(View.GONE);
        statusText.setText("准备中…");
        progressBar.setMax(Math.max(1, totalPairs));
        progressBar.setProgress(0);
        renderMatrix(false);
        setRunning(true);

        startMillis = SystemClock.elapsedRealtime();
        new Thread(new Runnable() {
            @Override
            public void run() {
                int code;
                try {
                    code = NativeBench.runMatrix(cores, iterations, runs, owned, progressCallback);
                } catch (Throwable t) {
                    code = 6;
                }
                final int result = code;
                final long elapsedMs = SystemClock.elapsedRealtime() - startMillis;
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        finishTest(result, elapsedMs);
                    }
                });
            }
        }, "cl-matrix").start();
    }

    private void stopTest() {
        if (!running) {
            return;
        }
        statusText.setText("正在停止…");
        NativeBench.abort();
    }

    private void finishTest(int code, long elapsedMs) {
        running = false;
        setRunning(false);

        if (code == 0) {
            float[] result = NativeBench.matrix();
            if (result != null && result.length == n * n) {
                live = result;
            }
            donePairs = totalPairs;
            progressBar.setProgress(totalPairs);
            renderMatrix(true);
            matrixCaption.setText("最终结果 · 双向平均 · ns/op");
            final int naPairs = countNaPairs();
            if (naPairs > 0) {
                statusText.setText(String.format(Locale.US,
                        "完成 · %d 个核心对 · %d 个 N/A（绑核失败） · 用时 %.2f s",
                        totalPairs, naPairs, elapsedMs / 1000.0));
            } else {
                statusText.setText(String.format(Locale.US,
                        "完成 · %d 个核心对 · 用时 %.2f s",
                        totalPairs, elapsedMs / 1000.0));
            }

            lastCsv = NativeBench.csv();
            lastCsvName = "core_latencies_" + timestamp() + ".csv";
            if (lastCsv != null) {
                exportButton.setEnabled(true);
                exportButton.setAlpha(1f);
                autoSave(lastCsv, lastCsvName);
            }
        } else if (code == 1) {
            renderMatrix(false);
            matrixCaption.setText("已停止 · 部分结果");
            final int naPairs = countNaPairs();
            if (naPairs > 0) {
                statusText.setText(String.format(Locale.US,
                        "已停止 · 完成 %d/%d 个核心对 · %d 个 N/A",
                        donePairs, totalPairs, naPairs));
            } else {
                statusText.setText(String.format(Locale.US,
                        "已停止 · 完成 %d/%d 个核心对", donePairs, totalPairs));
            }
            setSavedMessage(null);
        } else {
            renderMatrix(false);
            matrixCaption.setText("结果矩阵 · ns/op");
            statusText.setText("测试失败：" + describeError(code));
            setSavedMessage(null);
        }
        requestTvFocus();
    }

    private void setRunning(boolean isRunning) {
        startButton.setEnabled(!isRunning);
        startButton.setAlpha(isRunning ? 0.45f : 1f);
        stopButton.setEnabled(isRunning);
        stopButton.setAlpha(isRunning ? 1f : 0.5f);
        if (isRunning) {
            exportButton.setEnabled(false);
            exportButton.setAlpha(0.45f);
        }
        requestTvFocus();
    }

    private void requestTvFocus() {
        if (!isTelevision || startButton == null) {
            return;
        }
        startButton.post(new Runnable() {
            @Override
            public void run() {
                if (isFinishing()) {
                    return;
                }
                Button target = running ? stopButton
                        : (exportButton != null && exportButton.isEnabled()
                        ? exportButton : startButton);
                if (target != null && target.isEnabled()) {
                    target.requestFocus();
                }
            }
        });
    }

    // -----------------------------------------------------------------------
    // Rendering
    // -----------------------------------------------------------------------

    private void renderMatrix(boolean finalView) {
        if (matrixText == null) {
            return;
        }
        if (live == null || n <= 0 || live.length < n * n) {
            matrixText.setText("");
            naPairs = 0;
            return;
        }
        final int columnWidth = isWatch ? 6 : isTelevision ? 8 : 7;
        ensureMatrixCache(columnWidth);

        float min = Float.MAX_VALUE;
        float max = -Float.MAX_VALUE;
        int naCount = 0;
        for (int row = 0; row < n; ++row) {
            int base = row * n;
            for (int column = 0; column < n; ++column) {
                if (row == column) {
                    continue;
                }
                float value = live[base + column];
                if (value != value) {
                    ++naCount;
                } else if (value > 0f) {
                    if (value < min) {
                        min = value;
                    }
                    if (value > max) {
                        max = value;
                    }
                }
            }
        }
        naPairs = naCount;
        final boolean flat = !(max - min >= 1e-6f);

        StringBuilder builder = new StringBuilder(
                n * (6 + n * columnWidth + 1) + 8);
        builder.append("      ");
        for (int column = 0; column < n; ++column) {
            builder.append(matrixColumnHeaders[column]);
        }
        builder.append('\n');
        for (int row = 0; row < n; ++row) {
            builder.append(matrixRowHeaders[row]);
            int base = row * n;
            for (int column = 0; column < n; ++column) {
                int index = base + column;
                if (row == column) {
                    if (matrixCellKind[index] != CELL_DIAGONAL) {
                        matrixCells[index] = padLeft("x", columnWidth);
                        matrixCellKind[index] = CELL_DIAGONAL;
                    }
                } else {
                    float value = live[index];
                    byte kind;
                    if (value != value) {
                        kind = CELL_NA;
                    } else if (value <= 0f) {
                        kind = CELL_PENDING;
                    } else {
                        kind = CELL_VALUE;
                    }
                    // Re-format only cells whose displayed value changed: the
                    // per-pair render used to run n*n String.format calls.
                    if (matrixCellKind[index] != kind
                            || (kind == CELL_VALUE && Float.floatToIntBits(
                                    matrixCellValue[index]) != Float.floatToIntBits(value))) {
                        if (kind == CELL_VALUE) {
                            matrixCells[index] = padLeft(formatTenths(value), columnWidth);
                        } else if (kind == CELL_NA) {
                            matrixCells[index] = padLeft("N/A", columnWidth);
                        } else {
                            matrixCells[index] = padLeft("·", columnWidth);
                        }
                        matrixCellKind[index] = kind;
                        matrixCellValue[index] = value;
                    }
                }
                builder.append(matrixCells[index]);
            }
            builder.append('\n');
        }

        SpannableString text = new SpannableString(builder.toString());
        int rowStride = 6 + n * columnWidth + 1;
        int rowStart = rowStride;
        for (int row = 0; row < n; ++row) {
            int base = row * n;
            for (int column = 0; column < n; ++column) {
                int index = base + column;
                int start = rowStart + 6 + column * columnWidth;
                int end = start + columnWidth;
                byte kind = matrixCellKind[index];
                if (kind == CELL_VALUE) {
                    int color = flat ? HEAT_GREEN : heatColor(live[index], min, max);
                    text.setSpan(new ForegroundColorSpan(color), start, end,
                            Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
                } else if (kind == CELL_DIAGONAL || kind == CELL_PENDING
                        || kind == CELL_NA) {
                    text.setSpan(new ForegroundColorSpan(TEXT_SECONDARY), start, end,
                            Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
                }
            }
            rowStart += rowStride;
        }
        matrixText.setText(text);
        updateMatrixNote();
    }

    /** (Re)builds the per-cell text cache when the core count or layout changes. */
    private void ensureMatrixCache(int columnWidth) {
        if (matrixCacheN == n && matrixCacheWidth == columnWidth
                && matrixCells != null) {
            return;
        }
        matrixCacheN = n;
        matrixCacheWidth = columnWidth;
        int cells = n * n;
        matrixCells = new String[cells];
        matrixCellKind = new byte[cells];
        matrixCellValue = new float[cells];
        matrixColumnHeaders = new String[n];
        matrixRowHeaders = new String[n];
        for (int i = 0; i < n; ++i) {
            matrixColumnHeaders[i] = padLeft("C" + i, columnWidth);
            matrixRowHeaders[i] = padRight("C" + i, 6);
        }
    }

    /** Green -> orange -> red interpolation used by the matrix heat map. */
    private static int heatColor(float value, float min, float max) {
        if (!(max - min >= 1e-6f)) {
            return HEAT_GREEN;
        }
        float t = (value - min) / (max - min);
        if (t < 0f) {
            t = 0f;
        } else if (t > 1f) {
            t = 1f;
        }
        final int from;
        final int to;
        final float k;
        if (t <= 0.5f) {
            from = HEAT_GREEN;
            to = HEAT_ORANGE;
            k = t * 2f;
        } else {
            from = HEAT_ORANGE;
            to = HEAT_RED;
            k = (t - 0.5f) * 2f;
        }
        int red = Math.round(Color.red(from) + (Color.red(to) - Color.red(from)) * k);
        int green = Math.round(Color.green(from) + (Color.green(to) - Color.green(from)) * k);
        int blue = Math.round(Color.blue(from) + (Color.blue(to) - Color.blue(from)) * k);
        return Color.rgb(red, green, blue);
    }

    private int countNaPairs() {
        return naPairs;
    }

    private void updateMatrixNote() {
        if (matrixNote == null) {
            return;
        }
        matrixNote.setVisibility(naPairs > 0 ? View.VISIBLE : View.GONE);
    }

    private static String padLeft(String value, int width) {
        int padding = width - value.length();
        if (padding <= 0) {
            return value;
        }
        StringBuilder builder = new StringBuilder(width);
        for (int i = 0; i < padding; ++i) {
            builder.append(' ');
        }
        return builder.append(value).toString();
    }

    private static String padRight(String value, int width) {
        int padding = width - value.length();
        if (padding <= 0) {
            return value;
        }
        StringBuilder builder = new StringBuilder(width);
        builder.append(value);
        for (int i = 0; i < padding; ++i) {
            builder.append(' ');
        }
        return builder.toString();
    }

    /**
     * One-decimal number without String.format: the progress callback and the
     * matrix both used to allocate a formatter for every cell / every pair.
     */
    private static String formatTenths(double value) {
        StringBuilder builder = new StringBuilder(8);
        appendTenths(builder, value);
        return builder.toString();
    }

    private static void appendTenths(StringBuilder builder, double value) {
        if (value != value) {
            builder.append("N/A");
            return;
        }
        boolean negative = value < 0d;
        if (negative) {
            value = -value;
        }
        long tenths = Math.round(value * 10d);
        if (negative) {
            builder.append('-');
        }
        builder.append(tenths / 10L).append('.').append(tenths % 10L);
    }

    private String doneSummary() {
        if (isWatch) {
            return n + " 核 · " + n + "×" + n + " 矩阵";
        }
        return String.format(Locale.US, "%d 个逻辑核心 · 建议 %d×%d 矩阵",
                n, n, n);
    }

    // -----------------------------------------------------------------------
    // Export
    // -----------------------------------------------------------------------

    private void autoSave(final String csv, final String name) {
        // The CSV write no longer runs on the UI thread; the hint is posted
        // back so setSavedMessage() still touches its views on the main thread.
        new Thread(new Runnable() {
            @Override
            public void run() {
                String message;
                try {
                    File base = getExternalFilesDir(null);
                    if (base == null) {
                        base = getFilesDir();
                    }
                    File directory = new File(base, "results");
                    if (!directory.exists() && !directory.mkdirs()) {
                        throw new IOException("无法创建结果目录");
                    }
                    File file = new File(directory, name);
                    FileOutputStream output = new FileOutputStream(file);
                    try {
                        output.write(csv.getBytes(ASCII));
                    } finally {
                        output.close();
                    }
                    message = "已自动保存：" + file.getAbsolutePath();
                } catch (Exception error) {
                    message = "自动保存失败：" + error.getMessage();
                }
                final String result = message;
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        setSavedMessage(result);
                    }
                });
            }
        }, "cl-autosave").start();
    }

    private void exportCsv() {
        if (lastCsv == null || lastCsvName == null) {
            Toast.makeText(this, "请先完成一次测试", Toast.LENGTH_SHORT).show();
            return;
        }
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.KITKAT) {
            // No system save dialog before Android 4.4: write a second copy
            // into the app folder and tell the user where it went.
            autoSave(lastCsv, lastCsvName);
            Toast.makeText(this, "正在保存到应用文件夹：" + lastCsvName,
                    Toast.LENGTH_LONG).show();
            return;
        }
        Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("text/csv");
        intent.putExtra(Intent.EXTRA_TITLE, lastCsvName);
        try {
            startActivityForResult(intent, REQUEST_EXPORT);
        } catch (ActivityNotFoundException error) {
            Toast.makeText(this, "系统没有可用的文件保存界面", Toast.LENGTH_LONG).show();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_EXPORT || resultCode != RESULT_OK || data == null) {
            return;
        }
        Uri uri = data.getData();
        if (uri == null) {
            return;
        }
        try {
            OutputStream output = getContentResolver().openOutputStream(uri);
            if (output == null) {
                throw new IOException("无法打开目标文件");
            }
            try {
                output.write(lastCsv.getBytes(ASCII));
                output.flush();
            } finally {
                output.close();
            }
            Toast.makeText(this, "CSV 已导出", Toast.LENGTH_LONG).show();
            String exportedName = lastCsvName != null ? lastCsvName : "CSV";
            setSavedMessage("已导出 CSV：" + exportedName);
        } catch (Exception error) {
            Toast.makeText(this, "导出失败：" + error.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    // -----------------------------------------------------------------------
    // Device input: D-pad focus is native to Button, rotary wheel is explicit.
    // -----------------------------------------------------------------------

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (isWatch && scrollView != null && event != null
                && event.getAction() == MotionEvent.ACTION_SCROLL
                && (event.getSource() & InputDevice.SOURCE_ROTARY_ENCODER)
                == InputDevice.SOURCE_ROTARY_ENCODER) {
            float delta = event.getAxisValue(MotionEvent.AXIS_SCROLL);
            if (delta != 0f) {
                scrollView.scrollBy(0, Math.round(-delta * dp(36)));
                return true;
            }
        }
        return super.dispatchGenericMotionEvent(event);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            applyWindowFlags();
            requestTvFocus();
        }
    }

    // -----------------------------------------------------------------------
    // Small UI helpers
    // -----------------------------------------------------------------------

    private int buttonHeightDp() {
        return isWatch ? 44 : isTelevision ? 72 : 48;
    }

    private TextView text(String value, float sizeSp, int color, boolean bold) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, sizeSp * uiScale);
        view.setTextColor(color);
        if (bold) {
            view.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        }
        return view;
    }

    private TextView sectionTitle(String value) {
        TextView view = text(value, 12.5f, TEXT_TERTIARY, true);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            view.setLetterSpacing(0.06f);
        }
        return view;
    }

    private LinearLayout card(int topMarginDp) {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        int padding = isWatch ? 10 : isTelevision ? 22 : 16;
        card.setPadding(dp(padding), dp(isWatch ? 9 : 14), dp(padding),
                dp(isWatch ? 10 : 16));
        card.setBackground(roundRect(CARD, isWatch ? 12 : 14, 0, 0));
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            card.setElevation(dp(isWatch ? 0.5f : 1.5f));
        }
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        params.topMargin = dp(topMarginDp);
        card.setLayoutParams(params);
        return card;
    }

    private Button actionButton(String label, int background, int foreground,
                                int stroke, boolean primary) {
        Button button = new Button(this);
        button.setText(label);
        button.setAllCaps(false);
        button.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15 * uiScale);
        button.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        button.setTextColor(foreground);
        button.setGravity(Gravity.CENTER);
        button.setIncludeFontPadding(false);
        button.setMinWidth(0);
        button.setMinimumWidth(0);
        button.setMinHeight(0);
        button.setMinimumHeight(0);
        button.setPadding(dp(12), dp(6), dp(12), dp(6));
        button.setBackground(buttonBackground(background, stroke, primary));
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            button.setBackgroundTintList(null);
        }
        // focusableInTouchMode makes the first tap only move focus to the
        // button, so every button needed two taps.  Plain focusable keeps
        // D-pad/remote navigation (once the window leaves touch mode) without
        // stealing the tap.
        button.setFocusable(true);
        button.setFocusableInTouchMode(false);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            button.setStateListAnimator(null);
        }
        return button;
    }

    private Drawable buttonBackground(int normal, int stroke, boolean primary) {
        StateListDrawable states = new StateListDrawable();
        states.addState(new int[]{android.R.attr.state_focused},
                roundRect(primary ? ACCENT_DARK : FOCUS_BACKGROUND,
                        12, primary ? Color.WHITE : ACCENT, 2));
        states.addState(new int[]{android.R.attr.state_pressed},
                roundRect(primary ? 0xFF1D4ED8 : 0xFFE4E7EC, 12, stroke,
                        stroke == 0 ? 0 : 1));
        states.addState(new int[]{}, roundRect(normal, 12, stroke,
                stroke == 0 ? 0 : 1));
        return states;
    }

    private GradientDrawable roundRect(int color, int radiusDp, int strokeColor,
                                       int strokeDp) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(color);
        drawable.setCornerRadius(dp(radiusDp));
        if (strokeDp > 0) {
            drawable.setStroke(dp(strokeDp), strokeColor);
        }
        return drawable;
    }

    private LinearLayout.LayoutParams wrapParams() {
        return new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private int dp(float value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private double elapsedSeconds() {
        return (SystemClock.elapsedRealtime() - startMillis) / 1000.0;
    }

    private void setSavedMessage(String message) {
        if (savedText == null) {
            return;
        }
        if (message == null || message.trim().isEmpty()) {
            savedText.setText(" ");
            savedText.setVisibility(isWatch ? View.GONE : View.VISIBLE);
            return;
        }
        if (isWatch) {
            savedText.setText(message.startsWith("已导出") ? "已导出" : "已自动保存");
        } else {
            savedText.setText(message);
        }
        savedText.setVisibility(View.VISIBLE);
    }

    private static int safeInt(String value, int fallback) {
        try {
            return Integer.parseInt(value.trim());
        } catch (NumberFormatException error) {
            return fallback;
        }
    }

    private static String timestamp() {
        return new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(new Date());
    }

    private String describeError(int code) {
        switch (code) {
            case 2:
                return "核心数无效";
            case 3:
                return "迭代次数无效";
            case 4:
                return "采样轮数无效";
            case 5:
                return "进度回调不可用";
            default:
                String message = NativeBench.lastError();
                if (message == null || message.isEmpty()) {
                    return "原生错误";
                }
                return message;
        }
    }
}