package com.ddyibb.coherencylatency;

/**
 * Thin JNI facade for the native CoherencyLatency measurement core.
 *
 * <p>The measurement itself lives in {@code jni/clbench_jni.cpp}, which is the
 * same algorithm as the command line port (bounce / owned workers, one thread
 * pair per core pair, fastest sample of all runs).</p>
 */
public final class NativeBench {

    static {
        System.loadLibrary("clbench");
    }

    private NativeBench() {
    }

    /** Receives one callback per finished core pair, on the benchmark thread. */
    public interface Progress {
        void onPair(int first, int second, float latencyNs, int done, int total);
    }

    /** "model|logical cores|cache line|abi" for the device header. */
    public static native String deviceInfo();

    /**
     * Runs the full matrix synchronously.
     *
     * @return 0 success, 1 aborted, 2 bad core count, 3 bad iterations,
     *         4 bad run count, 5 callback problem, 6 native error
     */
    public static native int runMatrix(int cores, long iterations, int runs,
                                       boolean ownedMode, Progress progress);

    /** Final symmetric matrix (row-major, diagonal zero) of the last run. */
    public static native float[] matrix();

    /** Semicolon CSV of the last successful run, or null. */
    public static native String csv();

    /** Last native error message (empty when the last call succeeded). */
    public static native String lastError();

    /** Requests the running matrix to stop after the current pair. */
    public static native void abort();
}
