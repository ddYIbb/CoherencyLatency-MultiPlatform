// Headless self test for the GUI bridge.  Build it the same way the GUI is
// built (cl /MT /DCL_NO_MAIN with engine_api.cpp) and run a tiny matrix.
//
//   selftest.exe [threads] [iterations] [runs]
//
// It prints the CPU info the GUI would show, runs one matrix with progress
// callbacks, writes a CSV and prints the matrix -- so a broken bridge is caught
// before either front end is packaged.

#include "engine_api.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <thread>

namespace {

void on_progress(void* context, const ClProgress* progress) {
    int* count = static_cast<int*>(context);
    ++*count;
    if (progress->done == progress->total || progress->done % 7 == 0) {
        char text[32];
        cl_format_ns(progress->latency_ns, text, sizeof(text));
        std::printf("  %2d/%2d  T%d<->T%d  %s ns  %.2fs\n", progress->done,
                    progress->total, progress->i, progress->j, text,
                    progress->elapsed_seconds);
    }
}

}  // namespace

int main(int argc, char** argv) {
    int threads = argc > 1 ? std::atoi(argv[1]) : 4;
    const double abort_after = argc > 4 ? std::atof(argv[4]) : 0.0;
    const unsigned long long iterations =
        argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1000000ULL;
    const unsigned long long runs =
        argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 3ULL;

    std::printf("version: %s\n", cl_version());
    std::printf("default threads: %d\n", cl_default_threads());

    ClCpuInfo info;
    if (cl_query_cpu(&info) != 0) {
        std::printf("cl_query_cpu failed\n");
        return 1;
    }
    std::printf("logical=%d physical=%d cache_line=%d arch=%s groups=%d\n",
                info.logical, info.physical, info.cache_line, info.arch,
                info.groups);
    std::printf("model=%s\n", info.model);
    std::printf("affinity=%s hard_pinning=%d\n", info.affinity,
                info.hard_pinning);
    std::printf("slot_core:");
    for (int slot = 0; slot < info.logical && slot < 32; ++slot) {
        std::printf(" %d", info.slot_core[slot]);
    }
    std::printf("\n");

    ClRunConfig config = {};
    config.threads = threads;
    config.iterations = iterations;
    config.runs = runs;
    config.owned = 0;
    config.use_affinity = 1;

    ClAbort* abort_flag = cl_abort_create();
    ClResult result = {};
    char error[256] = {};
    int progress_calls = 0;

    std::printf("running %dx%d matrix (%llu iters x %llu runs)%s...\n", threads,
                threads, iterations, runs,
                abort_after > 0.0 ? " with a pending abort request" : "");
    if (abort_after > 0.0) {
        // Abort from a helper thread so the run has to notice it mid-matrix.
        std::thread([abort_flag, abort_after]() {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<int>(abort_after * 1000)));
            cl_abort_request(abort_flag);
        }).detach();
    }
    const int rc = cl_run_matrix(&config, &on_progress, &progress_calls,
                                 abort_flag, &result, error, sizeof(error));
    if (rc != 0) {
        std::printf("cl_run_matrix failed: rc=%d %s\n", rc, error);
        cl_abort_destroy(abort_flag);
        return 2;
    }

    std::printf("done: pairs %d/%d unpinned=%d aborted=%d elapsed=%.2fs progress_calls=%d\n",
                result.pairs_done, result.pairs_total, result.unpinned_pairs,
                result.aborted, result.elapsed_seconds, progress_calls);

    float minimum = 0.0f;
    float maximum = 0.0f;
    double sum = 0.0;
    int counted = 0;
    for (int i = 0; i < result.n; ++i) {
        for (int j = 0; j < result.n; ++j) {
            const float value =
                result.cells[static_cast<std::size_t>(i) * result.n + j];
            if (i == j || value != value) {
                std::printf("%8s", i == j ? "x" : "N/A");
                continue;
            }
            std::printf("%8.1f", static_cast<double>(value));
            if (counted == 0 || value < minimum) minimum = value;
            if (counted == 0 || value > maximum) maximum = value;
            sum += value;
            ++counted;
        }
        std::printf("\n");
    }
    if (counted > 0) {
        std::printf("average %.1f  min %.1f  max %.1f  (%d cells)\n",
                    sum / counted, static_cast<double>(minimum),
                    static_cast<double>(maximum), counted);
    }

    char name[128] = {};
    cl_default_csv_name(name, sizeof(name));
    if (cl_write_csv(&result, name, error, sizeof(error)) == 0) {
        std::printf("csv: %s\n", name);
    } else {
        std::printf("csv failed: %s\n", error);
    }

    cl_free_result(&result);
    cl_abort_destroy(abort_flag);
    return 0;
}
