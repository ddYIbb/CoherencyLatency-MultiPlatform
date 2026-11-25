package com.ddyibb.coherencylatency;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.util.TypedValue;
import android.view.View;

/**
 * The results matrix: every core pair is a rounded cell whose background is
 * filled with the shared green -> orange -> red heat ramp and whose value is
 * drawn in white, exactly like the Windows, macOS and HarmonyOS front ends.
 *
 * <p>The whole grid is one custom view: the old implementation painted the
 * heat colour into the glyphs of a monospace TextView, which looked nothing
 * like the desktop matrix and re-built a large SpannableString on every
 * progress callback.  Drawing here costs one invalidate per pair.</p>
 */
final class HeatMatrixView extends View {

    private static final int HEAT_GREEN = 0xFF16B769;
    private static final int HEAT_ORANGE = 0xFFF48011;
    private static final int HEAT_RED = 0xFFF83533;
    private static final int HEAT_FAIL = 0xFFD92D20;

    private final Paint cellPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF cellRect = new RectF();

    private final float density;

    private float uiScale = 1f;
    private boolean watch;
    private boolean dense;

    private int n;
    private float[] values = new float[0];
    private float minimum;
    private float maximum;
    private boolean flat;

    private float labelWidth;
    private float cellWidth;
    private float cellHeight;
    private float cellGap;
    private float headerHeight;
    private float cornerRadius;
    private float valueTextPx;
    private float headerTextPx;

    private int idleColor = 0xFFF2F4F7;
    private int diagonalColor = 0xFFEEF2F6;
    private int secondaryColor = 0xFF667085;
    private int tertiaryColor = 0xFF98A2B3;

    HeatMatrixView(Context context) {
        super(context);
        density = context.getResources().getDisplayMetrics().density;
        textPaint.setTypeface(Typeface.MONOSPACE);
        textPaint.setTextAlign(Paint.Align.CENTER);
        setFocusable(false);
        setFocusableInTouchMode(false);
        setClickable(false);
        setLongClickable(false);
    }

    /** Night-mode aware palette and the layout scale (watch 0.8, TV 1.7). */
    void configure(boolean isWatch, float scale, int idle, int diagonal,
                   int secondary, int tertiary) {
        watch = isWatch;
        uiScale = scale;
        idleColor = idle;
        diagonalColor = diagonal;
        secondaryColor = secondary;
        tertiaryColor = tertiary;
        updateMetrics();
        requestLayout();
        invalidate();
    }

    void clear() {
        n = 0;
        values = new float[0];
        requestLayout();
        invalidate();
    }

    /**
     * @param cells the activity's live matrix (read directly, never copied)
     * @param count logical core count (matrix is count x count)
     */
    void setData(float[] cells, int count, float min, float max, boolean isFlat) {
        values = cells != null ? cells : new float[0];
        n = Math.max(0, Math.min(count, 32));
        minimum = min;
        maximum = max;
        flat = isFlat;
        updateMetrics();
        requestLayout();
        invalidate();
    }

    private static float valueSizeFor(int cores) {
        if (cores >= 17) return 7f;
        if (cores >= 13) return 7.5f;
        if (cores >= 10) return 8f;
        if (cores >= 9) return 8.5f;
        if (cores >= 8) return 9.5f;
        if (cores >= 6) return 11f;
        return 11.5f;
    }

    private static float headerSizeFor(int cores) {
        if (cores >= 17) return 7.5f;
        if (cores >= 13) return 8f;
        if (cores >= 10) return 9f;
        return cores >= 9 ? 9.5f : 11f;
    }

    private void updateMetrics() {
        if (n <= 0) {
            return;
        }
        // Cell geometry mirrors the HarmonyOS card: the grid may scroll
        // horizontally, so nine or more cores keep a readable minimum width
        // instead of being squeezed into the card.
        if (n >= 13) {
            labelWidth = 22f;
            cellWidth = 20f;
            cellGap = 0f;
        } else if (n >= 10) {
            labelWidth = 21f;
            cellWidth = 24f;
            cellGap = 0f;
        } else if (n >= 9) {
            labelWidth = 27f;
            cellWidth = 30f;
            cellGap = 1f;
        } else if (n == 8) {
            labelWidth = 40f;
            cellWidth = 33f;
            cellGap = 2f;
        } else if (n == 7) {
            labelWidth = 40f;
            cellWidth = 37f;
            cellGap = 2f;
        } else if (n >= 6) {
            labelWidth = 40f;
            cellWidth = 44f;
            cellGap = 2f;
        } else {
            labelWidth = 40f;
            cellWidth = 46f;
            cellGap = 2f;
        }
        // A watch has a much narrower card; shrink the fixed metrics with it.
        // The numbers above are dp, so they must be multiplied by the display
        // density before use as raw pixels.
        final float shrink = watch ? 0.8f : 1f;
        final float metric = density * uiScale * shrink;
        labelWidth = labelWidth * metric;
        cellWidth = cellWidth * metric;
        cellGap = cellGap * metric;
        cellHeight = 26f * metric;
        cornerRadius = 5f * metric;
        headerTextPx = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP,
                headerSizeFor(n) * uiScale * shrink, getResources().getDisplayMetrics());
        valueTextPx = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP,
                valueSizeFor(n) * uiScale * shrink, getResources().getDisplayMetrics());
        headerHeight = headerTextPx + 8f * density * uiScale * shrink;
        dense = n >= 13;
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        if (n <= 0) {
            setMeasuredDimension(0, 0);
            return;
        }
        final float width = labelWidth + n * cellWidth + (n - 1) * cellGap;
        final float height = headerHeight + n * cellHeight + (n - 1) * cellGap;
        setMeasuredDimension((int) Math.ceil(width) + getPaddingLeft() + getPaddingRight(),
                (int) Math.ceil(height) + getPaddingTop() + getPaddingBottom());
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (n <= 0) {
            return;
        }
        final float left = getPaddingLeft();
        final float top = getPaddingTop();

        // Column headers, centred over their column.
        textPaint.setTypeface(Typeface.MONOSPACE);
        textPaint.setTextSize(headerTextPx);
        textPaint.setColor(tertiaryColor);
        textPaint.setFakeBoldText(false);
        for (int column = 0; column < n; ++column) {
            final float centerX = left + labelWidth + column * (cellWidth + cellGap)
                    + cellWidth * 0.5f;
            drawCentred(canvas, "C" + column, centerX, top, headerHeight);
        }

        for (int row = 0; row < n; ++row) {
            final float y = top + headerHeight + row * (cellHeight + cellGap);
            // Row label (header font: the value font below is only for cells).
            textPaint.setTypeface(Typeface.MONOSPACE);
            textPaint.setFakeBoldText(false);
            textPaint.setTextSize(headerTextPx);
            textPaint.setColor(tertiaryColor);
            drawCentred(canvas, "C" + row, left + labelWidth * 0.5f, y, cellHeight);

            for (int column = 0; column < n; ++column) {
                final float x = left + labelWidth + column * (cellWidth + cellGap);
                cellRect.set(x, y, x + cellWidth, y + cellHeight);
                int fill;
                String text;
                int textColor;
                if (row == column) {
                    fill = diagonalColor;
                    text = "x";
                    textColor = tertiaryColor;
                } else if (values.length <= row * n + column) {
                    fill = idleColor;
                    text = "·";
                    textColor = tertiaryColor;
                } else {
                    final float value = values[row * n + column];
                    if (value != value) {
                        fill = HEAT_FAIL;
                        text = "N/A";
                        textColor = 0xFFFFFFFF;
                    } else if (value <= 0f) {
                        fill = idleColor;
                        text = "·";
                        textColor = tertiaryColor;
                    } else {
                        fill = flat ? HEAT_GREEN : heatColor(value, minimum, maximum);
                        text = formatTenths(value);
                        textColor = 0xFFFFFFFF;
                    }
                }
                cellPaint.setColor(fill);
                canvas.drawRoundRect(cellRect, cornerRadius, cornerRadius, cellPaint);
                if (!dense) {
                    textPaint.setTypeface(Typeface.MONOSPACE);
                    textPaint.setFakeBoldText(row != column && textColor == 0xFFFFFFFF);
                    textPaint.setTextSize(valueTextPx);
                    textPaint.setColor(textColor);
                    drawCentred(canvas, text, x + cellWidth * 0.5f, y, cellHeight);
                }
            }
        }
    }

    private void drawCentred(Canvas canvas, String text, float centerX, float top,
                             float height) {
        final float baseline = top + (height - (textPaint.descent() + textPaint.ascent())) * 0.5f;
        canvas.drawText(text, centerX, baseline, textPaint);
    }

    /** Green -> orange -> red interpolation shared by every front end. */
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
        return 0xFF000000
                | (Math.round(android.graphics.Color.red(from)
                        + (android.graphics.Color.red(to) - android.graphics.Color.red(from)) * k) << 16)
                | (Math.round(android.graphics.Color.green(from)
                        + (android.graphics.Color.green(to) - android.graphics.Color.green(from)) * k) << 8)
                | Math.round(android.graphics.Color.blue(from)
                        + (android.graphics.Color.blue(to) - android.graphics.Color.blue(from)) * k);
    }

    /** One-decimal text without allocating a formatter per cell. */
    private static String formatTenths(double value) {
        StringBuilder builder = new StringBuilder(8);
        boolean negative = value < 0d;
        if (negative) {
            value = -value;
        }
        long tenths = Math.round(value * 10d);
        if (negative) {
            builder.append('-');
        }
        builder.append(tenths / 10L).append('.').append(tenths % 10L);
        return builder.toString();
    }
}
