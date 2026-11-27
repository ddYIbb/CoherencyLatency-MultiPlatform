// CoherencyLatency - Windows compatibility build for x86, x64 and ARM64.
//
// Derived from the macOS/ARM64 port of MicroBenchX CoherencyLatency.
// Original project: https://github.com/CXWorld/MicroBenchX (tag v1.0.5)
// Original file:    source/MicroBenchX.Icl/CoherencyLatency.cpp
// Original license: MIT, Copyright (c) 2021 CapFrameX
//
// The benchmark uses portable C++17 threads and atomics so the same source
// builds and runs on 32-bit x86, x64 and Windows on ARM (ARM64).

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#if defined(_MSC_VER)
#pragma warning(disable : 4324)  // intentional cache-line padding
#endif

#include <algorithm>
#include <atomic>
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <immintrin.h>
#elif defined(_MSC_VER) && (defined(_M_ARM64) || defined(_M_ARM))
#include <intrin.h>
#endif
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(CL_DEBUG_AFFINITY)
#include <cstdarg>
static void cl_debug_log(const char* fmt, ...) {
    FILE* file = std::fopen(
        "C:\\Users\\Administrator\\AppData\\Local\\Temp\\cl_affinity.log", "a");
    if (file == nullptr) {
        return;
    }
    std::va_list args;
    va_start(args, fmt);
    std::vfprintf(file, fmt, args);
    va_end(args);
    std::fclose(file);
}
#endif

#if defined(__APPLE__)
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#endif

namespace {

constexpr const char* kVersion = "1.3.0";
constexpr int kBarWidth = 60;

// Apple Silicon cache lines are commonly 128 bytes; keep this a compile-time
// value so AtomicCell has a fixed layout.  The sysctl value is also reported
// at runtime by --list.
constexpr std::size_t kAtomicAlignment = 128;

// A benchmark worker only needs a few hundred bytes of stack, while the
// platform default reserves 1 MiB per thread.  The matrix creates a fresh
// worker pair for every core pair, so an explicit small stack keeps the
// per-run cost and the address-space footprint low on both platforms.
constexpr std::size_t kWorkerStackBytes = 128 * 1024;
// A short round (few iterations, fast cores) finishes in microseconds.  The
// main thread spins this many pause instructions before falling back to a
// 1 ms sleep so a short round is not charged a syscall.

struct alignas(kAtomicAlignment) AtomicCell {
    std::atomic<std::uint64_t> value;

    explicit AtomicCell(std::uint64_t initial = 0) : value(initial) {}
};

static_assert(alignof(AtomicCell) == kAtomicAlignment,
              "AtomicCell must stay cache-line aligned");
static_assert(sizeof(AtomicCell) % kAtomicAlignment == 0,
              "AtomicCell size must be a multiple of its alignment");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "64-bit atomics must be lock-free for this benchmark");

// One gate is shared by the two workers of a core pair.  Instead of creating
// two new threads for every sample, the pair is created once and then walks a
// sequence of rounds: round_go releases the workers, round_done counts the
// finished workers (two increments per round).
struct StartGate {
    std::atomic<int> ready{0};
    std::atomic<int> round_go{0};
    std::atomic<int> round_done{0};
    std::atomic<bool> abort{false};
};

// One logical slot in the flattened, process-wide CPU numbering.  Windows
// exposes processors as (group, number) pairs once a machine has more than 64
// logical CPUs; keeping the mapping explicit lets the matrix cover all of them
// with SetThreadGroupAffinity instead of silently wrapping at 64.
struct CpuSlot {
    int group = 0;
    int processor = 0;
};

struct CpuTopology {
    int groups = 1;
    std::vector<int> group_sizes;
    std::vector<CpuSlot> slots;

    void refresh() {
#if defined(_WIN32)
        groups = 1;
        group_sizes.clear();
        slots.clear();
        const WORD count = GetActiveProcessorGroupCount();
        if (count > 0) {
            groups = static_cast<int>(count);
        }
        for (int group = 0; group < groups; ++group) {
            const DWORD active =
                GetActiveProcessorCount(static_cast<WORD>(group));
            group_sizes.push_back(static_cast<int>(active));
            for (DWORD processor = 0; processor < active; ++processor) {
                slots.push_back(
                    CpuSlot{group, static_cast<int>(processor)});
            }
        }
#else
        int logical = static_cast<int>(std::thread::hardware_concurrency());
        if (logical <= 0) {
            logical = 1;
        }
        groups = 1;
        group_sizes.assign(1, logical);
        slots.clear();
        slots.reserve(static_cast<std::size_t>(logical));
        for (int processor = 0; processor < logical; ++processor) {
            slots.push_back(CpuSlot{0, processor});
        }
#endif
    }

    int logical_count() const {
        return static_cast<int>(slots.size());
    }

    CpuSlot slot(int index) const {
        if (index < 0 || index >= static_cast<int>(slots.size())) {
            return CpuSlot{-1, -1};
        }
        return slots[static_cast<std::size_t>(index)];
    }

    // Flattened index of a (group, processor) pair, -1 when it is not active.
    int slot_for(int group, int processor) const {
        for (std::size_t index = 0; index < slots.size(); ++index) {
            if (slots[index].group == group &&
                slots[index].processor == processor) {
                return static_cast<int>(index);
            }
        }
        return -1;
    }
};

CpuTopology g_topology;

struct ThreadConfig {
    std::uint64_t start = 0;
    std::uint64_t iterations = 0;
    AtomicCell* target = nullptr;
    AtomicCell* read_target = nullptr;
    int cpu_index = 0;
    int qos_class_hint = -1;  // QoS-only macOS tier placement (-1 = none)
    int rounds = 1;
    bool use_affinity = true;
    // Per-success hand-off stamps (directional measurement).  nullptr keeps the
    // worker free of the extra clock read; stamp_capacity bounds the writes.
    std::uint64_t* stamps = nullptr;
    std::uint64_t stamp_capacity = 0;
    // Set by the worker itself: true when the requested placement was verified
    // (or when no pinning was requested).  A pair whose workers could not be
    // pinned is reported as N/A instead of as a number from the wrong core.
    bool affinity_ok = false;
    StartGate* gate = nullptr;
};

enum class Mode {
    Bounce,
    Owned,
};

struct Options {
    std::uint64_t iterations = 1000000;
    std::uint64_t runs = 10;
    Mode mode = Mode::Bounce;
    int cpus = 0;  // 0 means all logical CPUs
    bool perf_only = false;
    std::string csv_path;
    bool assume_yes = false;
    bool progress = true;
    bool use_affinity = true;
    bool list_only = false;
    bool show_help = false;
    bool show_version = false;
};

struct CpuInfo {
    int logical = 1;
    int physical = 0;
    int perf_level0 = 0;
    int perf_level1 = 0;
    int cache_line = 64;
    std::string model;
    // Apple Silicon perf tiers (P/M/E or more).  Tier 0 is the fastest
    // cluster band, higher indices are slower.  perf_level0/1 above mirror
    // tier[0]/tier[1] for the existing two-tier UI.
    int perf_tier_count = 0;
    int perf_tier[8] = {};
};

struct MatrixResult {
    int n = 0;
    // unpinned_pairs counts the core pairs whose workers could not be pinned
    // to their requested processors; their cells hold NaN and print as N/A.
    int unpinned_pairs = 0;
    std::vector<float> filtered;
};

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

std::uint64_t now_ns() {
#if defined(__APPLE__)
    // mach_absolute_time works back to the earliest supported macOS versions,
    // unlike clock_gettime (10.12+) and std::chrono::system_clock (10.12+).
    // The timebase is queried once; every call is a single mach trap plus one
    // multiply/shift-free conversion when numer == denom (the Apple Silicon
    // case, where both are 1).
    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t value{};
        mach_timebase_info(&value);
        return value;
    }();
    const std::uint64_t ticks = static_cast<std::uint64_t>(mach_absolute_time());
    if (timebase.numer == timebase.denom) {
        return ticks;
    }
    return ticks * static_cast<std::uint64_t>(timebase.numer) /
           static_cast<std::uint64_t>(timebase.denom);
#elif defined(_WIN32)
    // QueryPerformanceFrequency is constant for the life of the process, so it
    // is resolved once instead of on every sample.  The conversion uses
    // integer arithmetic: q * 1e9 + (r * 1e9) / freq keeps full precision
    // without the rounding of a double division.
    static const std::uint64_t ticks_per_second = [] {
        LARGE_INTEGER frequency;
        if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
            return static_cast<std::uint64_t>(frequency.QuadPart);
        }
        return static_cast<std::uint64_t>(0);
    }();
    LARGE_INTEGER counter;
    if (ticks_per_second != 0 && QueryPerformanceCounter(&counter)) {
        const std::uint64_t ticks = static_cast<std::uint64_t>(counter.QuadPart);
        const std::uint64_t whole = ticks / ticks_per_second;
        const std::uint64_t rest = ticks % ticks_per_second;
        return whole * 1000000000ULL + (rest * 1000000000ULL) / ticks_per_second;
    }
#else
    struct timespec ts {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
               static_cast<std::uint64_t>(ts.tv_nsec);
    }
#endif
    return 0;
}

// A hand-off stamp (v1.2.1 directional measurement).  The window sample of a
// round divides the whole round by the number of hand-offs, so it reports the
// average of both directions of a pair.  Stamping every successful store lets
// the two directions be measured separately.  The stamp has to be much finer
// than the shortest hop (about 25 ns between SMT siblings), so the finest
// counter the platform exposes in user mode is used:
//   x86/x64          TSC behind an lfence (about 0.4 ns ticks on a 2.5 GHz core)
//   ARM64 Linux/OHOS CNTVCT_EL0, the counter now_ns already reads
//   Windows on ARM   no user-mode cycle counter; QPC at 100 ns granularity is
//                    too coarse, so run_pair falls back to the window sample.
std::uint64_t cl_stamp() {
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    _mm_lfence();
#if defined(_MSC_VER)
    return static_cast<std::uint64_t>(__rdtsc());
#else
    return static_cast<std::uint64_t>(__builtin_ia32_rdtsc());
#endif
#elif defined(__aarch64__)
    std::uint64_t value = 0;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(value));
    return value;
#else
    return now_ns();
#endif
}

// ---------------------------------------------------------------------------
// CPU information
// ---------------------------------------------------------------------------

int sysctl_int(const char* name, int fallback) {
#if defined(__APPLE__)
    int value = 0;
    std::size_t length = sizeof(value);
    if (sysctlbyname(name, &value, &length, nullptr, 0) == 0) {
        return value;
    }
#else
    (void)name;
#endif
    return fallback;
}

std::string sysctl_string(const char* name) {
#if defined(__APPLE__)
    std::size_t length = 0;
    if (sysctlbyname(name, nullptr, &length, nullptr, 0) != 0 || length == 0) {
        return {};
    }
    std::string value(length, '\0');
    if (sysctlbyname(name, value.data(), &length, nullptr, 0) != 0) {
        return {};
    }
    if (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
#else
    (void)name;
    return {};
#endif
}

int perf_level_cores(const char* name) {
    int value = sysctl_int(name, 0);
    if (value == 0) {
        const std::string max_name = std::string(name) + "_max";
        value = sysctl_int(max_name.c_str(), 0);
    }
    return value;
}

CpuInfo read_cpu_info() {
    CpuInfo info;
    g_topology.refresh();

#if defined(__APPLE__)
    info.logical = sysctl_int("hw.logicalcpu", 0);
    info.physical = sysctl_int("hw.physicalcpu", info.logical);
    // Enumerate every perf tier the kernel reports (two on current Apple
    // Silicon; three or more on future multi-tier SoCs).  hw.perflevelN
    // keys appear only for tiers that exist, so stop at the first miss.
    for (int tier = 0; tier < 8; ++tier) {
        const int count = perf_level_cores(
            ("hw.perflevel" + std::to_string(tier) + ".logicalcpu").c_str());
        if (count <= 0) {
            break;
        }
        info.perf_tier[info.perf_tier_count++] = count;
    }
    info.perf_level0 =
        info.perf_tier_count > 0 ? info.perf_tier[0] : 0;
    info.perf_level1 =
        info.perf_tier_count > 1 ? info.perf_tier[1] : 0;
    info.cache_line = sysctl_int("hw.cachelinesize", 128);
    info.model = sysctl_string("machdep.cpu.brand_string");
#endif

#if defined(_WIN32)
    // g_topology.refresh() has already enumerated every processor group, so
    // this covers machines with more than 64 logical CPUs as well.  Windows
    // does not expose a stable physical-core number to user mode; leave it
    // unknown rather than presenting logical processors as physical cores.
    info.logical = g_topology.logical_count();
    info.physical = 0;
#else
    if (info.logical <= 0) {
        info.logical = static_cast<int>(std::thread::hardware_concurrency());
    }
#endif
    if (info.logical <= 0) {
        info.logical = 1;
    }
#if !defined(_WIN32)
    if (info.physical <= 0) {
        info.physical = info.logical;
    }
#endif
    if (info.cache_line < 32 || info.cache_line > 4096) {
        info.cache_line = 64;
    }

#if defined(_WIN32)
    char buffer[256] = {};
    const DWORD length =
        GetEnvironmentVariableA("PROCESSOR_IDENTIFIER", buffer, sizeof(buffer));
    if (length > 0) {
        info.model = buffer;
    }
#endif

    return info;
}

void print_cpu_info(const CpuInfo& info) {
    std::printf("CPU: %s\n", info.model.empty() ? "unknown" : info.model.c_str());
    std::printf("Logical CPUs: %d\n", info.logical);
    if (info.physical > 0) {
        std::printf("Physical CPUs: %d\n", info.physical);
    } else {
        std::printf("Physical CPUs: unknown (logical slots only)\n");
    }
#if defined(_WIN32)
    std::printf("Processor groups: %d", g_topology.groups);
    for (std::size_t index = 0; index < g_topology.group_sizes.size(); ++index) {
        std::printf("%s%d", index == 0 ? " (" : " + ",
                    g_topology.group_sizes[index]);
    }
    std::printf(" logical CPUs)\n");
#endif
#if defined(_M_ARM64)
    std::printf("Architecture: ARM64\n");
#elif defined(_M_X64)
    std::printf("Architecture: x64\n");
#elif defined(_M_IX86)
    std::printf("Architecture: x86 (32-bit)\n");
#endif
    if (info.perf_level0 > 0) {
        std::printf("Performance-level cores (perflevel0): %d\n", info.perf_level0);
    }
    if (info.perf_level1 > 0) {
        std::printf("Efficiency-level cores (perflevel1): %d\n", info.perf_level1);
    }
    std::printf("Cache line: %d bytes\n", info.cache_line);
#if defined(_WIN32)
    std::printf("Affinity: SetThreadGroupAffinity (processor-group aware, "
                "placement verified)\n");
#elif defined(__APPLE__)
    std::printf("Affinity: best-effort / unverified scheduler hint "
                "(macOS does not expose hard CPU pinning)\n");
#else
    std::printf("Affinity: sched_setaffinity (placement verified)\n");
#endif
}

// ---------------------------------------------------------------------------
// Thread setup
// ---------------------------------------------------------------------------

void yield_current_thread();

// Returns true when the requested placement was applied (or when no hard
// pinning was requested).  macOS only has best-effort hints, so it always
// reports success; Windows and Linux report a failure that becomes N/A.
bool configure_current_thread(int cpu_index, bool use_affinity,
                                 int qos_class_hint) {
#if !defined(__APPLE__)
    (void)qos_class_hint;  // QoS hints only exist on macOS.
#endif
#if defined(__APPLE__)
    // Give benchmark workers the highest user QoS class.  QoS is not CPU
    // pinning, but it reduces the chance of an unrelated task preempting them.
    if (use_affinity) {
        // pthread_set_qos_class_self_np is only available on macOS 10.10+.
        // Resolve it dynamically so the same binary also runs on older macOS.
        using SetQosClassSelfFn = int (*)(qos_class_t, int);
        static const SetQosClassSelfFn set_qos =
            reinterpret_cast<SetQosClassSelfFn>(
                dlsym(RTLD_DEFAULT, "pthread_set_qos_class_self_np"));
        if (set_qos != nullptr && qos_class_hint >= 0) {
            // Best-effort tier placement: each slot's QoS band nudges the
            // scheduler toward the matching cluster tier (UIQ -> P,
            // USER_INITIATED -> M, BACKGROUND -> E).  macOS has no hard
            // per-core pinning, so slots map to core *classes*, not cores.
            set_qos(static_cast<qos_class_t>(qos_class_hint), 0);
        }

        // Experimental macOS affinity hint.  Threads with the same tag are
        // encouraged to share an L2 cache; separate tags give the scheduler a
        // hint that these workers should not share one.
        thread_affinity_policy_data_t policy = {};
        policy.affinity_tag = cpu_index + 1;
        thread_policy_set(pthread_mach_thread_np(pthread_self()),
                          THREAD_AFFINITY_POLICY,
                          reinterpret_cast<thread_policy_t>(&policy),
                          THREAD_AFFINITY_POLICY_COUNT);
    }

    char name[32] = {};
    std::snprintf(name, sizeof(name), "cl-%d", cpu_index);
    pthread_setname_np(name);
    // macOS has no hard pinning, so the placement is always "as requested".
    return true;
#elif defined(_WIN32)
    bool ok = true;
    if (use_affinity) {
        const CpuSlot wanted = g_topology.slot(cpu_index);
        if (wanted.group < 0) {
            return false;
        }
        // Processor groups are mandatory above 64 logical CPUs.  Assigning the
        // thread to the group first and then applying a single bit inside that
        // group is the only way to address every CPU; a plain
        // SetThreadAffinityMask would wrap at 64.
        GROUP_AFFINITY affinity = {};
        affinity.Group = static_cast<WORD>(wanted.group);
        affinity.Mask = static_cast<KAFFINITY>(1)
                        << (wanted.processor & (sizeof(KAFFINITY) * 8 - 1));
        ok = SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr) != 0;
        if (ok) {
            // The kernel applies a fresh affinity mask at the next scheduling
            // point.  A tight SwitchToThread() loop can finish in a few
            // microseconds when no other thread is ready on the current CPU,
            // i.e. before the thread is actually moved -- a correctly pinned
            // pair then looked "unpinned" and became N/A.  Sleep in 1 ms slices
            // (Sleep always releases the CPU) for up to 100 ms, then verify.
            PROCESSOR_NUMBER actual = {};
            const ULONGLONG deadline = GetTickCount64() + 100;
            for (;;) {
                GetCurrentProcessorNumberEx(&actual);
                if (actual.Group == affinity.Group &&
                    static_cast<int>(actual.Number) == wanted.processor) {
                    break;
                }
                if (GetTickCount64() >= deadline) {
                    break;
                }
                Sleep(1);
            }
            GetCurrentProcessorNumberEx(&actual);
            ok = actual.Group == affinity.Group &&
                 static_cast<int>(actual.Number) == wanted.processor;
#if defined(CL_DEBUG_AFFINITY)
            if (!ok) {
                cl_debug_log("[pin-fail] cpu=%d wanted=%d/%d actual=%d/%d\n",
                             cpu_index, static_cast<int>(wanted.group),
                             wanted.processor, static_cast<int>(actual.Group),
                             static_cast<int>(actual.Number));
            }
#endif
        }
    }
    return ok;
#else
    bool ok = true;
    if (use_affinity) {
        cpu_set_t set;
        CPU_ZERO(&set);
        if (cpu_index >= 0 && cpu_index < CPU_SETSIZE) {
            CPU_SET(cpu_index, &set);
            ok = pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
            if (ok) {
                cpu_set_t actual;
                CPU_ZERO(&actual);
                ok = pthread_getaffinity_np(pthread_self(), sizeof(actual),
                                            &actual) == 0 &&
                     CPU_ISSET(cpu_index, &actual);
            }
        } else {
            ok = false;
        }
    }
    char name[32] = {};
    std::snprintf(name, sizeof(name), "cl-%d", cpu_index);
    pthread_setname_np(pthread_self(), name);
    return ok;
#endif
}

void cpu_relax() {
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    _mm_pause();
#elif defined(_MSC_VER) && (defined(_M_ARM64) || defined(_M_ARM))
    __yield();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

void yield_current_thread() {
#if defined(_WIN32)
    SwitchToThread();
#else
    std::this_thread::yield();
#endif
}

// Bounce mode: both threads contend with a compare-and-swap on the same
// cache line.  This is the direct analogue of _InterlockedCompareExchange64.
void bounce_worker(ThreadConfig* config) {
    std::atomic<std::uint64_t>& target = config->target->value;
    std::uint64_t current = config->start;
    const std::uint64_t limit = 2 * config->iterations;
    // The stamp is taken right after the store committed, so a pair of stamps
    // measures the time the peer needed to observe the store, without the
    // anticipation bias a pre-store stamp would introduce.
    std::uint64_t* stamps = config->stamps;
    const std::uint64_t capacity = config->stamp_capacity;
    std::uint64_t stored = 0;

    while (current <= limit) {
        std::uint64_t expected = current - 1;
        if (target.compare_exchange_strong(expected, current,
                                           std::memory_order_seq_cst,
                                           std::memory_order_seq_cst)) {
            if (stamps != nullptr && stored < capacity) {
                stamps[stored] = cl_stamp();
            }
            ++stored;
            current += 2;
        }
    }
}

// Owned mode: each thread writes to its own cache line and polls the other
// thread's line with acquire/release semantics.
void owned_worker(ThreadConfig* config) {
    std::atomic<std::uint64_t>& read_target = config->read_target->value;
    std::atomic<std::uint64_t>& target = config->target->value;
    std::uint64_t current = config->start;
    const std::uint64_t limit = 2 * config->iterations;
    std::uint64_t* stamps = config->stamps;
    const std::uint64_t capacity = config->stamp_capacity;
    std::uint64_t stored = 0;

    while (current <= limit) {
        if (read_target.load(std::memory_order_acquire) == current - 1) {
            target.store(current, std::memory_order_release);
            if (stamps != nullptr && stored < capacity) {
                stamps[stored] = cl_stamp();
            }
            ++stored;
            current += 2;
        }
    }
}

using WorkerFunction = void (*)(ThreadConfig*);

// A worker pair that stays alive for all samples of one core pair.  Creating
// and destroying two threads per sample dominated the run time of short
// measurements; the pair now synchronises through round counters instead.
class WorkerThread {
public:
    WorkerThread() = default;
    ~WorkerThread() { join(); }

    WorkerThread(const WorkerThread&) = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;

    void start(WorkerFunction function, ThreadConfig* config) {
        start_data_.function = function;
        start_data_.config = config;
#if defined(_WIN32)
        handle_ = CreateThread(nullptr, kWorkerStackBytes, &WorkerThread::entry,
                               &start_data_, 0, nullptr);
        if (handle_ == nullptr) {
            throw std::runtime_error("failed to create benchmark thread");
        }
#else
        pthread_attr_t attributes;
        if (pthread_attr_init(&attributes) != 0) {
            throw std::runtime_error("failed to create benchmark thread");
        }
        pthread_attr_setstacksize(&attributes, kWorkerStackBytes);
        const int result = pthread_create(&thread_, &attributes,
                                          &WorkerThread::entry, &start_data_);
        pthread_attr_destroy(&attributes);
        if (result != 0) {
            throw std::runtime_error("failed to create benchmark thread");
        }
        started_ = true;
#endif
    }

    void join() {
#if defined(_WIN32)
        if (handle_ != nullptr) {
            WaitForSingleObject(handle_, INFINITE);
            CloseHandle(handle_);
            handle_ = nullptr;
        }
#else
        if (started_) {
            pthread_join(thread_, nullptr);
            started_ = false;
        }
#endif
    }

    bool joinable() const {
#if defined(_WIN32)
        return handle_ != nullptr;
#else
        return started_;
#endif
    }

private:
    struct StartData {
        WorkerFunction function = nullptr;
        ThreadConfig* config = nullptr;
    };

    // Worker body: pin the thread once, then serve every round of the pair.
    static void run(StartData* data) {
        ThreadConfig* config = data->config;
        StartGate* gate = config->gate;
        config->affinity_ok =
            configure_current_thread(config->cpu_index, config->use_affinity,
                                  config->qos_class_hint);
        gate->ready.fetch_add(1, std::memory_order_acq_rel);

        for (int round = 0; round < config->rounds; ++round) {
            while (gate->round_go.load(std::memory_order_acquire) <= round) {
                if (gate->abort.load(std::memory_order_acquire)) {
                    return;
                }
                yield_current_thread();
            }
            if (gate->abort.load(std::memory_order_acquire)) {
                return;
            }
            data->function(config);
            // A vendor scheduler or a cpuset change can move a worker even
            // after the initial placement succeeded.  Check once per round so
            // a migrated pair is reported as N/A instead of as a number from
            // the wrong processor.  The check is outside the measurement loop.
#if defined(_WIN32)
            if (config->affinity_ok && config->use_affinity) {
                const CpuSlot wanted = g_topology.slot(config->cpu_index);
                PROCESSOR_NUMBER current = {};
                GetCurrentProcessorNumberEx(&current);
                if (current.Group != static_cast<WORD>(wanted.group) ||
                    static_cast<int>(current.Number) != wanted.processor) {
#if defined(CL_DEBUG_AFFINITY)
                    cl_debug_log(
                        "[moved] cpu=%d round=%d wanted=%d/%d actual=%d/%d\n",
                        config->cpu_index, round, static_cast<int>(wanted.group),
                        wanted.processor, static_cast<int>(current.Group),
                        static_cast<int>(current.Number));
#endif
                    config->affinity_ok = false;
                }
            }
#elif !defined(__APPLE__)
            if (config->affinity_ok && config->use_affinity &&
                sched_getcpu() != config->cpu_index) {
                config->affinity_ok = false;
            }
#endif
            gate->round_done.fetch_add(1, std::memory_order_acq_rel);
        }
    }

#if defined(_WIN32)
    static DWORD WINAPI entry(LPVOID parameter) {
        run(static_cast<StartData*>(parameter));
        return 0;
    }

    HANDLE handle_ = nullptr;
#else
    static void* entry(void* parameter) {
        run(static_cast<StartData*>(parameter));
        return nullptr;
    }

    pthread_t thread_{};
    bool started_ = false;
#endif
    StartData start_data_{};
};

// One core pair's result.  latency_ns is NaN when either worker could not be
// pinned to its requested processor, which is reported as N/A instead of as a
// number measured on the wrong core.
struct PairOutcome {
    // Window sample: the round divided by the number of hand-offs, i.e. the
    // average of both directions.  Kept as the fallback for platforms whose
    // clock cannot resolve a single hop.
    float latency_ns = std::numeric_limits<float>::quiet_NaN();
    // Directional samples.  forward_ns is processor1 -> processor2, so cell
    // [i][j] reports the hand-off from thread i to thread j and the two halves
    // of the matrix are independent measurements.
    float forward_ns = std::numeric_limits<float>::quiet_NaN();
    float backward_ns = std::numeric_limits<float>::quiet_NaN();
    bool affinity_ok = false;
};

// Runs all samples of one core pair and returns the fastest one, which is the
// same statistic the original computed by storing n*n*runs values and taking
// the minimum afterwards.
PairOutcome run_pair(int processor1, int processor2, int qos1, int qos2,
                     std::uint64_t iterations,
                     std::uint64_t runs, bool owned_mode, bool use_affinity) {
    // Separate 128-byte-aligned cells keep the two owners on distinct cache
    // lines on Apple Silicon (whose cache line is commonly 128 bytes).
    // Stack storage avoids two heap allocations per run.
    AtomicCell target1(owned_mode ? 1u : 0u);
    AtomicCell target2(0);

    StartGate gate;

    // Directional measurement buffers.  They are reused by every round, and a
    // round always performs exactly 'iterations' successful stores per worker,
    // so no entry can keep a stale value from an earlier round.
    const bool use_stamps = iterations >= 8 && iterations <= (1ULL << 22);
    std::vector<std::uint64_t> stamps1;
    std::vector<std::uint64_t> stamps2;
    if (use_stamps) {
        stamps1.assign(static_cast<std::size_t>(iterations), 0);
        stamps2.assign(static_cast<std::size_t>(iterations), 0);
    }

    ThreadConfig config1;
    config1.start = owned_mode ? 3 : 1;
    config1.iterations = iterations;
    config1.target = &target1;
    config1.read_target = &target2;
    config1.cpu_index = processor1;
    config1.qos_class_hint = qos1;
    config1.rounds = static_cast<int>(runs);
    config1.use_affinity = use_affinity;
    config1.gate = &gate;
    config1.stamps = use_stamps ? stamps1.data() : nullptr;
    config1.stamp_capacity = use_stamps ? iterations : 0;

    ThreadConfig config2;
    config2.start = 2;
    config2.iterations = iterations;
    config2.target = owned_mode ? &target2 : &target1;
    config2.read_target = &target1;
    config2.cpu_index = processor2;
    config2.qos_class_hint = qos2;
    config2.rounds = static_cast<int>(runs);
    config2.use_affinity = use_affinity;
    config2.gate = &gate;
    config2.stamps = use_stamps ? stamps2.data() : nullptr;
    config2.stamp_capacity = use_stamps ? iterations : 0;

    WorkerThread thread1;
    WorkerThread thread2;

    const WorkerFunction worker = owned_mode ? owned_worker : bounce_worker;
    try {
        thread1.start(worker, &config1);
        thread2.start(worker, &config2);
    } catch (...) {
        gate.abort.store(true, std::memory_order_release);
        gate.round_go.store(config1.rounds, std::memory_order_release);
        if (thread1.joinable()) {
            thread1.join();
        }
        if (thread2.joinable()) {
            thread2.join();
        }
        throw;
    }

    while (gate.ready.load(std::memory_order_acquire) < 2) {
        cpu_relax();
    }

    float minimum = std::numeric_limits<float>::infinity();
    // Per-round directional means and their window values; the final
    // directional latency comes only from rounds whose window value is close
    // to the best round, so noisy rounds cannot decide the mean.
    std::vector<double> round_forward;
    std::vector<double> round_backward;
    std::vector<float> round_window;
    const auto wait_round = [&gate](int round) {
        // A pair needs only a few microseconds to finish a short round, so a
        // syscall-based wait would outlast the round and, on Windows, could
        // quantise the measured round to a scheduler tick.  Yield instead:
        // SwitchToThread keeps the main thread ready without burning a whole
        // core for long, and rounds stay at their true duration.
        while (gate.round_done.load(std::memory_order_acquire) <
               2 * (round + 1)) {
            yield_current_thread();
        }
    };
    try {
        for (int round = 0; round < config1.rounds; ++round) {
            // Both workers are waiting for the next round here, so the shared
            // cells can be reset without any additional synchronisation.
            target1.value.store(owned_mode ? 1u : 0u, std::memory_order_relaxed);
            target2.value.store(0u, std::memory_order_relaxed);

            const std::uint64_t start = now_ns();
            const std::uint64_t ticks_begin = use_stamps ? cl_stamp() : 0;
            gate.round_go.store(round + 1, std::memory_order_release);
            wait_round(round);
            const std::uint64_t end = now_ns();
            const std::uint64_t ticks_end = use_stamps ? cl_stamp() : 0;
            if (end <= start) {
                throw std::runtime_error("monotonic clock did not advance");
            }

            // Each thread performs 'iterations' successful lock operations, so
            // the average latency per locked operation is total time /
            // (2 * iterations).
            const double latency_ns = static_cast<double>(end - start) /
                                      static_cast<double>(iterations) / 2.0;
            const float value = static_cast<float>(latency_ns);
            if (value < minimum) {
                minimum = value;
            }
            // Directional samples.  The hand-off chain is fixed per mode, so
            // the k-th store of one worker is paired with the store of the
            // other worker it caused:
            //   bounce: w1 stores 1,3,5... and w2 stores 2,4,6...
            //           w1[k] -> w2[k] -> w1[k+1]
            //   owned:  w2 stores 2,4,6... and w1 stores 3,5,7...
            //           w2[k] -> w1[k] -> w2[k+1]
            // Both stamps follow their store, so the difference is the time
            // the peer needed to observe it.
            if (use_stamps && round > 0 && ticks_end > ticks_begin && end > start) {
                const double ns_per_tick = static_cast<double>(end - start) /
                                           static_cast<double>(ticks_end - ticks_begin);
                // Resolution guard: an ARM arch timer at ~19.2 MHz ticks
                // every ~52 ns, coarser than an SMT hop; such stamps carry
                // heavy quantization noise, so the directional path stays
                // off and the window value fills every cell.
                if (ns_per_tick > 10.0) {
                    goto after_direction;
                }
                double sum_forward = 0.0;
                double sum_backward = 0.0;
                std::uint64_t count_forward = 0;
                std::uint64_t count_backward = 0;
                const auto add_hop = [ns_per_tick](std::uint64_t later,
                                                   std::uint64_t earlier,
                                                   double* sum,
                                                   std::uint64_t* count) {
                    if (later <= earlier) {
                        return;
                    }
                    const double hop =
                        static_cast<double>(later - earlier) * ns_per_tick;
                    // Anything above 100 us is a descheduled worker, not a
                    // hand-off; one such sample must not decide the mean.
                    if (hop < 100000.0) {
                        *sum += hop;
                        ++*count;
                    }
                };
                if (!owned_mode) {
                    for (std::size_t k = 0; k < iterations; ++k) {
                        add_hop(stamps2[k], stamps1[k], &sum_forward,
                                &count_forward);
                    }
                    for (std::size_t k = 0; k + 1 < iterations; ++k) {
                        add_hop(stamps1[k + 1], stamps2[k], &sum_backward,
                                &count_backward);
                    }
                } else {
                    for (std::size_t k = 0; k < iterations; ++k) {
                        add_hop(stamps1[k], stamps2[k], &sum_backward,
                                &count_backward);
                    }
                    for (std::size_t k = 0; k + 1 < iterations; ++k) {
                        add_hop(stamps2[k + 1], stamps1[k], &sum_forward,
                                &count_forward);
                    }
                }
                if (count_forward > iterations / 2 &&
                    count_backward > iterations / 2) {
                    const double forward =
                        sum_forward / static_cast<double>(count_forward);
                    const double backward =
                        sum_backward / static_cast<double>(count_backward);
                    const double average = 0.5 * (forward + backward);
                    // The stamps must describe the same hand-offs as the
                    // window sample.  A mean far away from it means the clock
                    // cannot resolve a hop or a worker was descheduled, so the
                    // round is dropped and the window sample stays in charge.
                    if (average > 0.25 * static_cast<double>(value) &&
                        average < 4.0 * static_cast<double>(value)) {
                        round_forward.push_back(forward);
                        round_backward.push_back(backward);
                        round_window.push_back(value);
                    }
                }
            }
        }
    } catch (...) {
        // Release any worker still waiting for a round, then join them before
        // propagating the error.
        gate.abort.store(true, std::memory_order_release);
        gate.round_go.store(config1.rounds, std::memory_order_release);
        thread1.join();
        thread2.join();
        throw;
    }

    after_direction:;
    thread1.join();
    thread2.join();

    PairOutcome outcome;
    outcome.affinity_ok = config1.affinity_ok && config2.affinity_ok;
#if defined(CL_DEBUG_AFFINITY)
    if (!outcome.affinity_ok) {
        cl_debug_log("[pair-fail] %d<->%d c1=%d c2=%d\n", processor1, processor2,
                     config1.affinity_ok ? 1 : 0, config2.affinity_ok ? 1 : 0);
    }
#endif
    outcome.latency_ns = outcome.affinity_ok
                             ? minimum
                             : std::numeric_limits<float>::quiet_NaN();
    if (outcome.affinity_ok && !round_forward.empty()) {
        // Only rounds whose window value is close to the best round decide the
        // directional latency; a round with background load would otherwise
        // inflate the mean of its hops.
        float best_window = round_window[0];
        for (const float window : round_window) {
            if (window < best_window) {
                best_window = window;
            }
        }
        const float window_limit =
            best_window <= 0.0f ? best_window : best_window * 1.35f;
        double best_forward = std::numeric_limits<double>::infinity();
        double best_backward = std::numeric_limits<double>::infinity();
        for (std::size_t k = 0; k < round_forward.size(); ++k) {
            if (round_window[k] <= window_limit) {
                if (round_forward[k] < best_forward) {
                    best_forward = round_forward[k];
                }
                if (round_backward[k] < best_backward) {
                    best_backward = round_backward[k];
                }
            }
        }
        if (best_forward < std::numeric_limits<double>::infinity() &&
            best_backward < std::numeric_limits<double>::infinity()) {
            outcome.forward_ns = static_cast<float>(best_forward);
            outcome.backward_ns = static_cast<float>(best_backward);
        }
    }
    return outcome;
}
// ---------------------------------------------------------------------------
// Matrix
// ---------------------------------------------------------------------------

std::size_t matrix_index(int n, int i, int j) {
    return static_cast<std::size_t>(i) * static_cast<std::size_t>(n) +
           static_cast<std::size_t>(j);
}

void print_progress(int done, int total) {
    const double progress =
        total > 0 ? static_cast<double>(done) / static_cast<double>(total) : 1.0;
    const int pos = static_cast<int>(kBarWidth * progress);

    char bar[kBarWidth + 1];
    for (int i = 0; i < kBarWidth; ++i) {
        bar[i] = i < pos ? '=' : (i == pos ? '>' : ' ');
    }
    bar[kBarWidth] = '\0';

    std::printf("[%s] %d %%\r", bar, static_cast<int>(progress * 100.0));
    std::fflush(stdout);
}

MatrixResult run_matrix(const Options& options, int n) {
    MatrixResult result;
    result.n = n;
    // -1 marks "not measured yet"; NaN marks a failed pair.  Both print as
    // N/A in the CSV (v1.3.0 spec: a stopped run leaves unmeasured pairs N/A).
    result.filtered.assign(static_cast<std::size_t>(n) * static_cast<std::size_t>(n),
                           -1.0f);

    // Keep only one accumulator per core pair.  The previous version stored
    // n*n*runs floats and reduced them in a second pass; taking the minimum
    // online produces the same matrix while using about 'runs' times less
    // memory and avoiding a second full traversal.
    // The original prints the bar once per row, which with many cores leaves it
    // frozen for tens of seconds between updates.  Updating after every pair
    // keeps the bar alive without changing what is measured.
    const bool owned_mode = options.mode == Mode::Owned;
    const int total_pairs = n * n - n;
    int done_pairs = 0;
    int unpinned_pairs = 0;
    // macOS: map each slot to the QoS band of its core tier so the matrix
    // shows a legible P/M/E structure (see configure_current_thread).
    // On other platforms slot_qos stays -1 (no QoS hint); the QOS_CLASS_*
    // constants only exist in the macOS SDK, so the mapping is compiled in
    // only for __APPLE__ builds.
    constexpr int kMaxQosSlots = 256;
    int slot_qos[kMaxQosSlots];
    for (int slot = 0; slot < kMaxQosSlots; ++slot) {
        slot_qos[slot] = -1;
    }
#if defined(__APPLE__)
    const CpuInfo cpu_info = read_cpu_info();
    if (options.use_affinity && cpu_info.perf_tier_count > 1) {
        int base = 0;
        for (int tier = 0; tier < cpu_info.perf_tier_count; ++tier) {
            const int count = cpu_info.perf_tier[tier];
            // Fastest tier -> UIQ, slowest -> BACKGROUND, middle -> USER_INITIATED.
            int qos;
            if (tier == 0) {
                qos = QOS_CLASS_USER_INTERACTIVE;
            } else if (tier == cpu_info.perf_tier_count - 1) {
                qos = QOS_CLASS_BACKGROUND;
            } else {
                qos = QOS_CLASS_USER_INITIATED;
            }
            for (int k = 0; k < count && base + k < kMaxQosSlots; ++k) {
                slot_qos[base + k] = qos;
            }
            base += count;
        }
    }
#endif
    if (options.progress) {
        print_progress(0, total_pairs);
    }
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            if (i == j) {
                continue;
            }
            const PairOutcome outcome =
                run_pair(i, j, slot_qos[i], slot_qos[j], options.iterations,
                         options.runs, owned_mode, options.use_affinity);
            // Cell [i][j] is the hand-off from thread i to thread j.  The
            // directional sample is used when it survived the guard, otherwise
            // the window average of the pair (see run_pair).
            result.filtered[matrix_index(n, i, j)] =
                outcome.forward_ns == outcome.forward_ns
                    ? outcome.forward_ns
                    : outcome.latency_ns;
            if (!outcome.affinity_ok) {
                ++unpinned_pairs;
            }
            if (options.progress) {
                print_progress(++done_pairs, total_pairs);
            }
        }
    }

    if (options.progress) {
        std::fputc('\n', stdout);
    }

    result.unpinned_pairs = unpinned_pairs;

    // Cell [i][j] holds the measurement of thread i signalling thread j, so the
    // two directions of a pair are reported separately: the matrix and the CSV
    // may legitimately be asymmetric.  The diagonal stays zero.

    return result;
}

void print_matrix(const MatrixResult& result) {
    for (int i = 0; i < result.n; ++i) {
        for (int j = 0; j < result.n; ++j) {
            if (j != 0) {
                std::fputc(',', stdout);
            }
            if (i == j) {
                std::fputc('x', stdout);
            } else {
                const float value =
                    result.filtered[matrix_index(result.n, i, j)];
                if (value != value || value < 0.0f) {
                    // The pair could not be pinned (NaN) or was never measured
                    // (stopped early): no number belongs to that cell.
                    std::fputs("N/A", stdout);
                } else {
                    // %.6f matches the original std::fixed << setprecision(6).
                    std::printf("%.6f", static_cast<double>(value));
                }
            }
        }
        std::fputc('\n', stdout);
    }
}

std::string default_csv_name() {
    const std::time_t now = std::time(nullptr);
    std::tm local_time = {};

#if defined(_WIN32)
    if (localtime_s(&local_time, &now) != 0) {
        throw std::runtime_error("failed to get local time");
    }
#else
    if (localtime_r(&now, &local_time) == nullptr) {
        throw std::runtime_error("failed to get local time");
    }
#endif

    char stamp[32] = {};
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local_time);
    return std::string("core_latencies_") + stamp + ".csv";
}

void write_csv(const std::string& path, const MatrixResult& result) {
    // Text mode keeps the original ofstream behaviour (CRLF on Windows, LF on
    // macOS) so the CSV bytes are unchanged.
    std::FILE* output = std::fopen(path.c_str(), "w");
    if (output == nullptr) {
        throw std::runtime_error("failed to open CSV file for writing: " + path);
    }

    // Preserve the original semicolon-delimited layout exactly.
    std::fputc(';', output);
    for (int i = 0; i < result.n; ++i) {
        if (i != 0) {
            std::fputc(';', output);
        }
        std::fprintf(output, "Core %d", i);
    }
    std::fputc('\n', output);

    for (int i = 0; i < result.n; ++i) {
        std::fprintf(output, "Core %d;", i);
        for (int j = 0; j < result.n; ++j) {
            if (j != 0) {
                std::fputc(';', output);
            }
            if (i == j) {
                std::fputc('x', output);
            } else {
                const float value =
                    result.filtered[matrix_index(result.n, i, j)];
                if (value != value || value < 0.0f) {
                    std::fputs("N/A", output);
                } else {
                    // %.6g matches the original std::defaultfloat << setprecision(6).
                    std::fprintf(output, "%.6g",
                                 static_cast<double>(value));
                }
            }
        }
        std::fputc('\n', output);
    }

    if (std::ferror(output) != 0) {
        std::fclose(output);
        throw std::runtime_error("failed while writing CSV file: " + path);
    }
    if (std::fclose(output) != 0) {
        throw std::runtime_error("failed to close CSV file: " + path);
    }
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

bool take_value(int& index, int argc, char** argv, const std::string& argument,
                const std::string& name, std::string& value) {
    if (argument == name) {
        if (index + 1 >= argc) {
            throw std::runtime_error("missing value for " + name);
        }
        value = argv[++index];
        return true;
    }

    const std::string prefix = name + "=";
    if (argument.size() > prefix.size() &&
        argument.compare(0, prefix.size(), prefix) == 0) {
        value = argument.substr(prefix.size());
        return true;
    }

    return false;
}

std::uint64_t parse_u64(const std::string& text, const char* name) {
    if (text.empty()) {
        throw std::runtime_error(std::string("empty value for ") + name);
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long long value =
        std::strtoull(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0') {
        throw std::runtime_error(std::string("invalid integer for ") + name +
                                 ": " + text);
    }
    return static_cast<std::uint64_t>(value);
}

int parse_int(const std::string& text, const char* name) {
    const std::uint64_t value = parse_u64(text, name);
    if (value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error(std::string("value too large for ") + name);
    }
    return static_cast<int>(value);
}

Options parse_options(int argc, char** argv) {
    Options options;
    bool positional_iterations_seen = false;
    bool positional_mode_seen = false;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        std::string value;

        if (argument == "-h" || argument == "--help") {
            options.show_help = true;
        } else if (argument == "--version") {
            options.show_version = true;
        } else if (take_value(i, argc, argv, argument, "--iterations", value) ||
                   take_value(i, argc, argv, argument, "-i", value)) {
            options.iterations = parse_u64(value, "iterations");
        } else if (take_value(i, argc, argv, argument, "--runs", value) ||
                   take_value(i, argc, argv, argument, "-r", value)) {
            options.runs = parse_u64(value, "runs");
        } else if (take_value(i, argc, argv, argument, "--mode", value) ||
                   take_value(i, argc, argv, argument, "-m", value)) {
            if (value == "bounce") {
                options.mode = Mode::Bounce;
            } else if (value == "owned") {
                options.mode = Mode::Owned;
            } else {
                throw std::runtime_error("invalid mode (expected bounce or owned): " +
                                         value);
            }
        } else if (take_value(i, argc, argv, argument, "--cpus", value)) {
            options.cpus = parse_int(value, "cpus");
        } else if (take_value(i, argc, argv, argument, "--csv", value)) {
            options.csv_path = value;
        } else if (argument == "--perf-only") {
            options.perf_only = true;
        } else if (argument == "--yes" || argument == "-y") {
            options.assume_yes = true;
        } else if (argument == "--no-progress") {
            options.progress = false;
        } else if (argument == "--no-affinity") {
            options.use_affinity = false;
        } else if (argument == "--list") {
            options.list_only = true;
        } else if (!argument.empty() && argument[0] != '-') {
            // Compatibility with the original positional CLI:
            //   coherency_latency [iterations] [bounce|owned]
            if (!positional_iterations_seen) {
                options.iterations = parse_u64(argument, "iterations");
                positional_iterations_seen = true;
            } else if (!positional_mode_seen) {
                if (argument == "bounce") {
                    options.mode = Mode::Bounce;
                } else if (argument == "owned") {
                    options.mode = Mode::Owned;
                } else {
                    throw std::runtime_error(
                        "invalid positional mode (expected bounce or owned): " +
                        argument);
                }
                positional_mode_seen = true;
            } else {
                throw std::runtime_error("unexpected positional argument: " +
                                         argument);
            }
        } else {
            throw std::runtime_error("unknown option: " + argument);
        }
    }

    if (options.iterations == 0) {
        throw std::runtime_error("iterations must be greater than zero");
    }
    if (options.iterations > std::numeric_limits<std::uint64_t>::max() / 2) {
        throw std::runtime_error("iterations is too large");
    }
    if (options.runs == 0) {
        throw std::runtime_error("runs must be greater than zero");
    }
    if (options.runs > 100000) {
        throw std::runtime_error("runs is unreasonably large (maximum 100000)");
    }

    return options;
}

void print_usage(const char* program) {
    std::printf("CoherencyLatency %s%s", kVersion, " - Windows compatibility build for x86 / x64 / ARM64\n\n");
    std::printf("Usage:\n  %s [options]\n  %s [iterations] [bounce|owned]\n\n",
                program, program);
    std::printf(
        "Options:\n"
        "  -i, --iterations N   Successful lock operations per thread "
        "(default: 1000000)\n"
        "  -r, --runs N         Samples per core pair (default: 10)\n"
        "  -m, --mode MODE      bounce (same cache line, default) or owned "
        "(separate lines)\n"
        "      --cpus N         Use at most N logical slots (default: all logical "
        "CPUs)\n"
        "      --perf-only      On Apple Silicon, limit logical slots to the "
        "detectable perflevel0 count\n"
        "      --csv PATH       CSV output path (default: core_latencies_<time>.csv "
        "in cwd)\n"
        "  -y, --yes            Do not wait for Enter before/after the test\n"
        "      --no-progress    Disable the progress bar\n"
        "      --no-affinity    Disable CPU pinning / affinity hints (results then\n"
        "                       describe whichever cores the scheduler chose)\n"
        "      --list           Print CPU information and exit\n"
        "  -h, --help           Show this help\n"
        "      --version        Show version\n\n");
    std::printf("%s", "Notes:\n"
        "  On Windows each worker is assigned to one logical processor with\n"
        "  SetThreadGroupAffinity and the actual (group, processor) placement is\n"
        "  verified with GetCurrentProcessorNumberEx.  Machines with more than\n"
        "  64 logical CPUs are covered through processor groups instead of\n"
        "  wrapping at 64.  The matrix indices are flattened logical slots, not\n"
        "  verified physical core IDs.\n"
        "  A placement is re-checked after every round; if the scheduler moved a\n"
        "  worker, that core pair is reported as N/A instead of as a number\n"
        "  measured on the wrong processor.\n"
        "  For reproducible numbers, close other applications and use AC power.\n"
        "  Use x86 for 32-bit Windows, x64 for 64-bit Windows, and ARM64 for\n"
        "  Windows on ARM.  The bundled .exe launcher selects the matching build.\n");
}

bool stdout_is_tty() {
#if defined(_WIN32)
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

void wait_for_enter(const char* prompt) {
    std::fputs(prompt, stdout);
    std::fflush(stdout);
    char buffer[256];
    if (std::fgets(buffer, sizeof(buffer), stdin) == nullptr) {
        return;
    }
}

const char* mode_name(Mode mode) {
    return mode == Mode::Owned ? "owned" : "bounce";
}

}  // namespace

#if !defined(CL_NO_MAIN)
int main(int argc, char** argv) {
    // A single large stdout buffer replaces the per-insertion buffering of the
    // iostream path and keeps the matrix output to one write per chunk.
    std::setvbuf(stdout, nullptr, _IOFBF, 1 << 16);

    try {
        Options options = parse_options(argc, argv);

        if (options.show_help) {
            print_usage(argv[0]);
            return 0;
        }
        if (options.show_version) {
            std::printf("CoherencyLatency %s\n", kVersion);
            return 0;
        }

        const CpuInfo cpu = read_cpu_info();
        std::printf("CoherencyLatency %s\n", kVersion);
        print_cpu_info(cpu);

#if defined(__APPLE__)
        std::printf("Placement: macOS provides no hard CPU pinning; matrix indices "
                    "are logical slots, not verified physical core IDs.\n");
#endif

#if defined(__APPLE__)
        if (sysctl_int("sysctl.proc_translated", 0) != 0) {
            std::printf("Warning: running under Rosetta 2; this is not a native "
                        "arm64 execution. Use the arm64 build.\n");
        }
#elif defined(_M_ARM64)
        std::printf("Runtime: native ARM64\n");
#elif defined(_M_X64)
        std::printf("Runtime: x64\n");
#elif defined(_M_IX86)
        std::printf("Runtime: x86 (32-bit)\n");
#else
        std::printf("Note: this is a non-ARM development build; affinity is a "
                    "fallback.\n");
#endif

        if (options.list_only) {
            return 0;
        }

        int cpu_count = cpu.logical;
        if (options.perf_only) {
            if (cpu.perf_level0 > 0) {
                cpu_count = cpu.perf_level0;
            } else {
                std::printf("Note: perflevel0 core count is unavailable; using "
                            "all logical CPUs.\n");
            }
        }
        if (options.cpus > 0) {
            cpu_count = std::min(cpu_count, options.cpus);
        }
        if (cpu_count < 2) {
            throw std::runtime_error("at least 2 CPUs are required");
        }

        std::printf("Mode: %s; CPUs: %d; iterations: %llu; runs: %llu\n",
                    mode_name(options.mode), cpu_count,
                    static_cast<unsigned long long>(options.iterations),
                    static_cast<unsigned long long>(options.runs));

        if (!options.assume_yes) {
            wait_for_enter("Press Enter to start the test...");
        }

        if (!stdout_is_tty()) {
            options.progress = false;
        }

        const MatrixResult result = run_matrix(options, cpu_count);

        std::printf("Latency matrix (ns per operation; diagonal x):\n");
        print_matrix(result);

        if (result.unpinned_pairs > 0) {
            std::printf("Warning: %d core pair(s) could not be pinned to the "
                        "requested processors; those cells are N/A.\n",
                        result.unpinned_pairs);
        }

        const std::string csv_path =
            options.csv_path.empty() ? default_csv_name() : options.csv_path;
        write_csv(csv_path, result);
        std::printf("Latencies written to %s\n", csv_path.c_str());

        if (!options.assume_yes) {
            wait_for_enter("Press Enter to close...");
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 2;
    }
}
#endif  // CL_NO_MAIN
