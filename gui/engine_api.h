// CoherencyLatency GUI bridge - shared by the Windows and macOS GUI front ends.
//
// The measurement engine lives in windows/src/CoherencyLatency.cpp, which is a
// cross-platform superset (it compiles unchanged for macOS with osxcross).  The
// GUI links that engine through this small C API so both front ends measure
// with exactly the same code path as the command line build.
//
// Threading: cl_run_matrix() blocks.  Call it from a worker thread and drive the
// UI from the progress callback; call cl_abort_request() from the UI thread to
// stop early.  An aborted run still returns the pairs measured so far.

#ifndef COHERENCY_LATENCY_ENGINE_API_H
#define COHERENCY_LATENCY_ENGINE_API_H

#include <stdint.h>

/* Both front ends link the bridge statically, so no export decoration is
   needed.  Keep the macro so a future DLL build can define CL_API_SHARED. */
#if defined(CL_API_SHARED) && defined(_WIN32)
#define CL_API __declspec(dllexport)
#else
#define CL_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define CL_MAX_SLOTS 256

typedef struct ClCpuInfo {
    int logical;      /* logical processor count */
    int physical;     /* physical core count, 0 when the platform hides it */
    int perf_level0;  /* performance cores, 0 when unknown */
    int perf_level1;  /* efficiency cores, 0 when unknown */
    int cache_line;   /* bytes */
    int groups;       /* processor groups (Windows), 1 elsewhere */
    char model[256];  /* CPU model string, may be empty */
    char arch[32];    /* "x64", "ARM64", "x86 (32-bit)", "arm64" */
    char affinity[192]; /* human readable affinity policy */
    int hard_pinning; /* 1 when threads can really be pinned to a CPU */
    /* Physical core id for every logical slot, -1 when unknown.  Slots that
       share a value are SMT siblings of the same physical core. */
    int slot_core[CL_MAX_SLOTS];
} ClCpuInfo;

typedef struct ClRunConfig {
    int threads;          /* 2 .. ClCpuInfo.logical */
    uint64_t iterations;  /* per worker per round */
    uint64_t runs;        /* rounds per pair */
    int owned;            /* 0 = bounce, 1 = owned */
    int use_affinity;     /* 0 to measure without pinning */
} ClRunConfig;

typedef struct ClProgress {
    int done;             /* pairs finished so far */
    int total;            /* pairs in this run */
    int i;                /* row of the pair that just finished */
    int j;                /* column of the pair that just finished */
    float latency_ns;     /* its value, NaN when the pair could not be pinned */
    int affinity_ok;
    double elapsed_seconds;
} ClProgress;

typedef void (*ClProgressFn)(void* context, const ClProgress* progress);

typedef struct ClResult {
    int n;
    int unpinned_pairs;
    int aborted;
    int pairs_done;       /* how many (i,j) pairs were measured */
    int pairs_total;      /* n*n-n */
    float* cells;         /* n*n floats, NaN = N/A / not measured, diagonal NaN */
    double elapsed_seconds;
} ClResult;

/* Opaque abort flag shared between the UI thread and the run. */
typedef struct ClAbort ClAbort;

CL_API ClAbort* cl_abort_create(void);
CL_API void cl_abort_request(ClAbort* abort_flag);
CL_API int cl_abort_requested(ClAbort* abort_flag);
CL_API void cl_abort_destroy(ClAbort* abort_flag);

CL_API const char* cl_version(void);
CL_API int cl_query_cpu(ClCpuInfo* out);
CL_API int cl_default_threads(void); /* min(logical, 8), at least 2 */

/* Blocking.  Returns 0 on success, 1 on invalid arguments, 2 on a runtime
   error (the message is copied into error_out when it is not NULL).  When the
   run stops because of cl_abort_request(), it still returns 0 and sets
   ClResult.aborted to 1. */
CL_API int cl_run_matrix(const ClRunConfig* config, ClProgressFn progress,
                         void* progress_context, ClAbort* abort_flag,
                         ClResult* out, char* error_out, int error_len);

CL_API void cl_free_result(ClResult* result);

/* "coherency_latency_20261123_211500.csv" - no directory. */
CL_API int cl_default_csv_name(char* buffer, int buffer_len);

/* Writes the same semicolon CSV the CLI writes; 0 on success. */
CL_API int cl_write_csv(const ClResult* result, const char* path,
                        char* error_out, int error_len);

/* "%.1f" style formatting helpers shared by both front ends.  NaN becomes
   "N/A".  Returns the number of characters written (excluding the NUL). */
CL_API int cl_format_ns(float value_ns, char* buffer, int buffer_len);

#ifdef __cplusplus
}
#endif

#endif  /* COHERENCY_LATENCY_ENGINE_API_H */
