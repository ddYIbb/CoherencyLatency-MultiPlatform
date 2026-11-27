// CoherencyLatency GUI bridge implementation.
//
// The engine is compiled into this translation unit on purpose: the Windows and
// macOS command line builds use exactly the same file, so the GUI cannot drift
// away from the measured baseline.

#define CL_NO_MAIN 1
// The engine file is shared with the command line build, so it still contains
// the CLI-only helpers (print_usage, parse_options, ...).  A GUI build never
// calls them; silence the "unused function" diagnostics they produce here
// instead of adding GUI-specific ifdefs to the shared measurement code.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../windows/src/CoherencyLatency.cpp"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "engine_api.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

double now_seconds_since(std::uint64_t start_ns) {
    const std::uint64_t now = now_ns();
    if (now <= start_ns) {
        return 0.0;
    }
    return static_cast<double>(now - start_ns) / 1e9;
}

void copy_text(char* out, int len, const std::string& text) {
    if (out == nullptr || len <= 0) {
        return;
    }
    std::snprintf(out, static_cast<std::size_t>(len), "%s", text.c_str());
}

void fill_arch_and_affinity(ClCpuInfo* info) {
    std::snprintf(info->arch, sizeof(info->arch), "%s", "unknown");
#if defined(__APPLE__)
    std::snprintf(info->arch, sizeof(info->arch), "%s", "arm64");
#elif defined(_M_ARM64)
    std::snprintf(info->arch, sizeof(info->arch), "%s", "ARM64");
#elif defined(_M_X64) || defined(__x86_64__)
    std::snprintf(info->arch, sizeof(info->arch), "%s", "x64");
#elif defined(_M_IX86) || defined(__i386__)
    std::snprintf(info->arch, sizeof(info->arch), "%s", "x86 (32-bit)");
#endif

#if defined(__APPLE__)
    std::snprintf(info->affinity, sizeof(info->affinity), "%s",
                  "best effort QoS only (macOS has no hard CPU pinning)");
    info->hard_pinning = 0;
#elif defined(_WIN32)
    std::snprintf(info->affinity, sizeof(info->affinity), "%s",
                  "SetThreadGroupAffinity, placement verified");
    info->hard_pinning = 1;
#else
    std::snprintf(info->affinity, sizeof(info->affinity), "%s",
                  "sched_setaffinity, placement verified");
    info->hard_pinning = 1;
#endif
}

void fill_slot_cores(ClCpuInfo* info) {
    for (int slot = 0; slot < CL_MAX_SLOTS; ++slot) {
        info->slot_core[slot] = -1;
    }
#if defined(_WIN32)
    // GetLogicalProcessorInformationEx(RelationProcessorCore) reports one
    // entry per physical core with the logical processors that belong to it.
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (length > 0) {
        std::vector<unsigned char> buffer(length);
        if (GetLogicalProcessorInformationEx(
                RelationProcessorCore,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
                    buffer.data()),
                &length)) {
            int core_id = 0;
            std::size_t offset = 0;
            // EfficiencyClass (Win10 1709+): 0 = least performant (E),
            // higher values = more performant tiers (P, or P/M on newer
            // SoCs).  Collect the distinct values to build tier counts.
            std::vector<unsigned char> core_classes;
            while (offset < length) {
                const auto* entry =
                    reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                        buffer.data() + offset);
                if (entry->Relationship == RelationProcessorCore) {
                    const PROCESSOR_RELATIONSHIP& relationship = entry->Processor;
                    core_classes.push_back(relationship.EfficiencyClass);
                    for (WORD g = 0; g < relationship.GroupCount; ++g) {
                        const GROUP_AFFINITY& mask = relationship.GroupMask[g];
                        for (int bit = 0; bit < 64; ++bit) {
                            if ((mask.Mask >> bit) & 1) {
                                const int slot =
                                    g_topology.slot_for(mask.Group, bit);
                                if (slot >= 0 && slot < CL_MAX_SLOTS) {
                                    info->slot_core[slot] = core_id;
                                }
                            }
                        }
                    }
                    ++core_id;
                }
                offset += entry->Size;
            }
            info->physical = core_id;
            // Build perf tiers: sort distinct class values ascending so
            // tier 0 is the efficiency tier and the last tier is the
            // performance tier, mirroring the macOS hw.perflevelN order.
            std::vector<unsigned char> distinct = core_classes;
            std::sort(distinct.begin(), distinct.end());
            distinct.erase(
                std::unique(distinct.begin(), distinct.end()), distinct.end());
            info->perf_tier_count =
                static_cast<int>(distinct.size() > 8 ? 8 : distinct.size());
            // Count logical slots per tier, not physical cores: SMT siblings
            // belong to the same tier, and the matrix addresses slots.
            offset = 0;
            for (int tier = 0; tier < info->perf_tier_count; ++tier) {
                info->perf_tier[tier] = 0;
            }
            while (offset < length) {
                const auto* entry2 =
                    reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                        buffer.data() + offset);
                if (entry2->Relationship == RelationProcessorCore) {
                    const PROCESSOR_RELATIONSHIP& rel2 = entry2->Processor;
                    int slot_bits = 0;
                    for (WORD g2 = 0; g2 < rel2.GroupCount; ++g2) {
                        const ULONGLONG m2 = rel2.GroupMask[g2].Mask;
                        for (int bit = 0; bit < 64; ++bit) {
                            if ((m2 >> bit) & 1) {
                                ++slot_bits;
                            }
                        }
                    }
                    // Tier of this core = index of its class in distinct.
                    for (int tier = 0; tier < info->perf_tier_count; ++tier) {
                        if (rel2.EfficiencyClass ==
                            distinct[static_cast<std::size_t>(tier)]) {
                            info->perf_tier[tier] += slot_bits;
                            break;
                        }
                    }
                }
                offset += entry2->Size;
            }
            if (info->perf_tier_count > 0) {
                info->perf_level0 = info->perf_tier[0];
            }
            if (info->perf_tier_count > 1) {
                info->perf_level1 = info->perf_tier[1];
            }
        }
    }
#endif
}

}  // namespace

extern "C" {

struct ClAbort {
    volatile long requested = 0;
};

ClAbort* cl_abort_create(void) { return new ClAbort(); }

void cl_abort_request(ClAbort* abort_flag) {
    if (abort_flag != nullptr) {
#if defined(_WIN32)
        InterlockedExchange(&abort_flag->requested, 1);
#else
        __atomic_store_n(&abort_flag->requested, 1, __ATOMIC_SEQ_CST);
#endif
    }
}

int cl_abort_requested(ClAbort* abort_flag) {
    if (abort_flag == nullptr) {
        return 0;
    }
#if defined(_WIN32)
    return InterlockedCompareExchange(&abort_flag->requested, 0, 0) != 0 ? 1 : 0;
#else
    return __atomic_load_n(&abort_flag->requested, __ATOMIC_SEQ_CST) != 0 ? 1 : 0;
#endif
}

void cl_abort_destroy(ClAbort* abort_flag) { delete abort_flag; }

const char* cl_version(void) { return kVersion; }

int cl_query_cpu(ClCpuInfo* out) {
    if (out == nullptr) {
        return 1;
    }
    std::memset(out, 0, sizeof(*out));
    const CpuInfo info = read_cpu_info();
    out->logical = info.logical;
    out->physical = info.physical;
    out->perf_level0 = info.perf_level0;
    out->perf_level1 = info.perf_level1;
    out->perf_tier_count = info.perf_tier_count;
    for (int tier = 0; tier < info.perf_tier_count && tier < 8; ++tier) {
        out->perf_tier[tier] = info.perf_tier[tier];
    }
    out->cache_line = info.cache_line;
    out->groups = g_topology.groups;
    std::snprintf(out->model, sizeof(out->model), "%s", info.model.c_str());
    fill_arch_and_affinity(out);
    fill_slot_cores(out);
    if (out->physical <= 0 && info.physical > 0) {
        out->physical = info.physical;
    }
    if (out->physical > out->logical) {
        out->physical = out->logical;
    }
    return 0;
}

int cl_default_threads(void) {
    ClCpuInfo info;
    if (cl_query_cpu(&info) != 0 || info.logical <= 0) {
        return 2;
    }
    int threads = info.logical < 8 ? info.logical : 8;
    if (threads < 2) {
        threads = 2;
    }
    return threads;
}

int cl_run_matrix(const ClRunConfig* config, ClProgressFn progress,
                  void* progress_context, ClAbort* abort_flag, ClResult* out,
                  char* error_out, int error_len) {
    if (config == nullptr || out == nullptr) {
        copy_text(error_out, error_len, "invalid argument");
        return 1;
    }
    std::memset(out, 0, sizeof(*out));

    ClCpuInfo info;
    cl_query_cpu(&info);

    const int n = config->threads;
    if (n < 2 || n > info.logical || n > CL_MAX_SLOTS) {
        copy_text(error_out, error_len, "invalid thread count");
        return 1;
    }
    if (config->iterations == 0 || config->runs == 0) {
        copy_text(error_out, error_len, "invalid iterations or runs");
        return 1;
    }

    try {
        Options options;
        options.iterations = config->iterations;
        options.runs = config->runs;
        options.mode = config->owned ? Mode::Owned : Mode::Bounce;
        options.cpus = n;
        options.use_affinity = config->use_affinity != 0;
        options.progress = false;

        const std::size_t cell_count =
            static_cast<std::size_t>(n) * static_cast<std::size_t>(n);
        // Idle cells start at -1 (matches the Android port): NaN now means a
        // pair that failed to produce a sample, so GUIs can paint a distinct
        // failure cell for it.
        std::vector<float> cells(cell_count, -1.0f);

        const int total_pairs = n * n - n;
        int done_pairs = 0;
        int unpinned_pairs = 0;
        int aborted = 0;
        const std::uint64_t start_ns = now_ns();

        for (int i = 0; i < n && aborted == 0; ++i) {
            for (int j = 0; j < n; ++j) {
                if (i == j) {
                    continue;
                }
                if (cl_abort_requested(abort_flag)) {
                    aborted = 1;
                    break;
                }
                int slot_qos[CL_MAX_SLOTS];
                for (int slot = 0; slot < CL_MAX_SLOTS; ++slot) {
                    slot_qos[slot] = -1;
                }
#if defined(__APPLE__)
                if (options.use_affinity && info.perf_tier_count > 1) {
                    int base = 0;
                    for (int tier = 0; tier < info.perf_tier_count; ++tier) {
                        int qos;
                        if (tier == 0) {
                            qos = QOS_CLASS_USER_INTERACTIVE;
                        } else if (tier == info.perf_tier_count - 1) {
                            qos = QOS_CLASS_BACKGROUND;
                        } else {
                            qos = QOS_CLASS_USER_INITIATED;
                        }
                        const int count = info.perf_tier[tier];
                        for (int k = 0;
                             k < count && base + k < CL_MAX_SLOTS; ++k) {
                            slot_qos[base + k] = qos;
                        }
                        base += count;
                    }
                }
#endif
                const PairOutcome outcome =
                    run_pair(i, j, slot_qos[i], slot_qos[j],
                             options.iterations, options.runs,
                             options.mode == Mode::Owned,
                             options.use_affinity);
#if defined(CL_DEBUG_AFFINITY)
                cl_debug_log("[pair] %d %d ok=%d ns=%.3f\n", i, j,
                             outcome.affinity_ok ? 1 : 0,
                             static_cast<double>(outcome.latency_ns));
#endif
                // Cell [i][j] is the hand-off from thread i to thread j.  The
                // directional sample is used when it survived the guard in
                // run_pair, otherwise the window average of the pair.
                const float cell_ns =
                    outcome.forward_ns == outcome.forward_ns
                        ? outcome.forward_ns
                        : outcome.latency_ns;
                cells[matrix_index(n, i, j)] = cell_ns;
                if (!outcome.affinity_ok) {
                    ++unpinned_pairs;
                }
                ++done_pairs;
                if (progress != nullptr) {
                    ClProgress update;
                    update.done = done_pairs;
                    update.total = total_pairs;
                    update.i = i;
                    update.j = j;
                    update.latency_ns = cell_ns;
                    update.affinity_ok = outcome.affinity_ok ? 1 : 0;
                    update.elapsed_seconds = now_seconds_since(start_ns);
                    progress(progress_context, &update);
                }
            }
        }

        if (!aborted && cl_abort_requested(abort_flag)) {
            aborted = 1;
        }

        // No averaging and no mirroring: cell [i][j] is the measurement of
        // thread i signalling thread j.  A stopped run simply keeps the cells it
        // already measured and leaves the rest N/A, which is what the Android
        // and HarmonyOS front ends show after "stop".

        out->n = n;
        out->unpinned_pairs = unpinned_pairs;
        out->aborted = aborted;
        out->pairs_done = done_pairs;
        out->pairs_total = total_pairs;
        out->elapsed_seconds = now_seconds_since(start_ns);
        out->cells = new float[cell_count];
        std::memcpy(out->cells, cells.data(), cell_count * sizeof(float));
        return 0;
    } catch (const std::exception& error) {
        copy_text(error_out, error_len, error.what());
        return 2;
    } catch (...) {
        copy_text(error_out, error_len, "unknown failure");
        return 2;
    }
}

void cl_free_result(ClResult* result) {
    if (result != nullptr) {
        delete[] result->cells;
        result->cells = nullptr;
    }
}

int cl_default_csv_name(char* buffer, int buffer_len) {
    if (buffer == nullptr || buffer_len <= 0) {
        return 1;
    }
    const std::string name = default_csv_name();
    std::snprintf(buffer, static_cast<std::size_t>(buffer_len), "%s",
                  name.c_str());
    return 0;
}

int cl_write_csv(const ClResult* result, const char* path, char* error_out,
                 int error_len) {
    if (result == nullptr || path == nullptr || result->cells == nullptr) {
        copy_text(error_out, error_len, "invalid argument");
        return 1;
    }
    try {
        MatrixResult matrix;
        matrix.n = result->n;
        matrix.unpinned_pairs = result->unpinned_pairs;
        matrix.filtered.assign(result->cells,
                               result->cells + static_cast<std::size_t>(result->n) *
                                                   static_cast<std::size_t>(result->n));
        write_csv(path, matrix);
        return 0;
    } catch (const std::exception& error) {
        copy_text(error_out, error_len, error.what());
        return 2;
    }
}

int cl_format_ns(float value_ns, char* buffer, int buffer_len) {
    if (buffer == nullptr || buffer_len <= 0) {
        return 0;
    }
    if (value_ns != value_ns) {
        return std::snprintf(buffer, static_cast<std::size_t>(buffer_len), "%s",
                             "N/A");
    }
    return std::snprintf(buffer, static_cast<std::size_t>(buffer_len), "%.1f",
                         static_cast<double>(value_ns));
}

}  // extern "C"
