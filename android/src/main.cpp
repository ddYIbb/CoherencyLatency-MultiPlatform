// CoherencyLatency - native Android port of MicroBenchX CoherencyLatency.
//
// Targets: arm64-v8a, armeabi-v7a, x86 and x86_64, NDK r21e.
// 32-bit ABIs build against API 16 (Android 4.1), 64-bit ABIs against API 21.
// The same algorithmic core as the macOS and Windows ports; only the
// CPU-information and thread-affinity layer is Android specific.
//
// Original project: https://github.com/CXWorld/MicroBenchX (tag v1.0.5)
// Original file:    source/MicroBenchX.Icl/CoherencyLatency.cpp
// Original license: MIT, Copyright (c) 2021 CapFrameX
//
// This port replaces the Windows threading/affinity/timing layer with portable
// C++17 + POSIX APIs.  macOS does not provide hard CPU pinning; the affinity
// policy used here is a best-effort scheduler hint only (see README.md).

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
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif
#endif

namespace {

constexpr const char* kVersion = "1.1.0-android";
constexpr int kBarWidth = 60;

// Apple Silicon cache lines are 128 bytes, ARM/ARM64 Android devices and x86
// report 64.  Keep this a compile-time value so AtomicCell has a fixed layout;
// the value actually seen at runtime is printed by --list.
constexpr std::size_t kAtomicAlignment =
#if defined(__APPLE__)
    128;
#else
    64;
#endif

// A benchmark worker only needs a few hundred bytes of stack, while the
// platform default reserves 1 MiB per thread.  The matrix creates a fresh
// worker pair for every core pair, so an explicit small stack keeps the
// per-run cost and the address-space footprint low on both platforms.
constexpr std::size_t kWorkerStackBytes = 128 * 1024;

struct alignas(kAtomicAlignment) AtomicCell {
    std::atomic<std::uint64_t> value;

    explicit AtomicCell(std::uint64_t initial = 0) : value(initial) {}
};

static_assert(alignof(AtomicCell) == kAtomicAlignment,
              "AtomicCell must stay cache-line aligned");
static_assert(sizeof(AtomicCell) % kAtomicAlignment == 0,
              "AtomicCell size must be a multiple of its alignment");
// Every 64-bit target keeps 64-bit atomics lock-free.  32-bit ARM needs the
// compiler's out-of-line helpers, which the build script links explicitly.
static_assert(sizeof(void*) >= 8 ||
                  std::atomic<std::uint64_t>::is_always_lock_free,
              "64-bit atomics must be lock-free on 64-bit targets");

// One gate is shared by the two workers of a core pair.  Instead of creating
// two new threads for every sample, the pair is created once and then walks a
// sequence of rounds: round_go releases the workers, round_done counts the
// finished workers (two increments per round).
// round_end[] is filled by the workers themselves, right after their own
// measurement loop returned.  The main thread used to observe the end of the
// window by polling, so its own scheduling delay was added to every sample;
// taking the timestamp where the work actually stops removes that bias.
// round_end[] is plain memory: the release fetch_add on round_done publishes
// it and the acquire load in the waiter is what makes it visible.
struct StartGate {
    std::atomic<int> ready{0};
    std::atomic<int> round_go{0};
    std::atomic<int> round_done{0};
    std::atomic<bool> abort{false};
    std::uint64_t round_end[2] = {0, 0};
    // The waiting thread sleeps here instead of spinning, so a run no longer
    // burns one extra core in sched_yield() calls.
    std::mutex mutex;
    std::condition_variable cv;
};

struct ThreadConfig {
    std::uint64_t start = 0;
    std::uint64_t iterations = 0;
    AtomicCell* target = nullptr;
    AtomicCell* read_target = nullptr;
    int cpu_index = 0;
    int rounds = 1;
    // Index into StartGate::round_end; 0 for the first worker, 1 for the second.
    int slot = 0;
    bool use_affinity = true;
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
    // bionic has provided clock_gettime since API 21.  CLOCK_MONOTONIC never
    // steps and keeps running while the process is migrated between cores,
    // which is exactly what a cross-core latency measurement needs.
    struct timespec ts {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
               static_cast<std::uint64_t>(ts.tv_nsec);
    }
#endif
    return 0;
}

// ---------------------------------------------------------------------------
// CPU information
// ---------------------------------------------------------------------------

#if defined(__APPLE__)
// The sysctl helpers only exist on Apple platforms.  Keeping them inside the
// #if avoids unused-function warnings on Android and Linux builds.
int sysctl_int(const char* name, int fallback) {
    int value = 0;
    std::size_t length = sizeof(value);
    if (sysctlbyname(name, &value, &length, nullptr, 0) == 0) {
        return value;
    }
    return fallback;
}

std::string sysctl_string(const char* name) {
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
}

int perf_level_cores(const char* name) {
    int value = sysctl_int(name, 0);
    if (value == 0) {
        const std::string max_name = std::string(name) + "_max";
        value = sysctl_int(max_name.c_str(), 0);
    }
    return value;
}
#endif  // defined(__APPLE__)

CpuInfo read_cpu_info() {
    CpuInfo info;

#if defined(__APPLE__)
    info.logical = sysctl_int("hw.logicalcpu", 0);
    info.physical = sysctl_int("hw.physicalcpu", info.logical);
    info.perf_level0 = perf_level_cores("hw.perflevel0.logicalcpu");
    info.perf_level1 = perf_level_cores("hw.perflevel1.logicalcpu");
    info.cache_line = sysctl_int("hw.cachelinesize", 128);
    info.model = sysctl_string("machdep.cpu.brand_string");
#endif

#if defined(_WIN32)
    // GetSystemInfo is available on every supported Windows version.  On
    // modern systems try GetActiveProcessorCount dynamically so >64 logical
    // CPUs are still reported without adding a Windows 7-only import.
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    if (system_info.dwNumberOfProcessors > 0) {
        info.logical = static_cast<int>(system_info.dwNumberOfProcessors);
    }
    if (info.logical <= 1) {
        using GetActiveProcessorCountFn = DWORD(WINAPI*)(WORD);
        const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        const auto get_active = kernel
            ? reinterpret_cast<GetActiveProcessorCountFn>(
                  GetProcAddress(kernel, "GetActiveProcessorCount"))
            : nullptr;
        if (get_active != nullptr) {
            const DWORD active = get_active(0xFFFF);
            if (active > 0) {
                info.logical = static_cast<int>(active);
            }
        }
    }
#elif defined(__ANDROID__)
    // std::thread::hardware_concurrency() under-reports on some big.LITTLE
    // devices; ask libc directly.  _SC_NPROCESSORS_CONF is the number of
    // configured cores, which is what the matrix wants (logical slots).
    // Android 4.x bionic answers 1 for _SC_NPROCESSORS_CONF, so take the
    // larger of the two counters instead of trusting CONF alone.
    const long configured = sysconf(_SC_NPROCESSORS_CONF);
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    const long logical = configured > online ? configured : online;
    info.logical = static_cast<int>(logical > 0 ? logical : 1);
    if (info.logical <= 0) {
        info.logical = static_cast<int>(std::thread::hardware_concurrency());
    }
    // Do not claim a physical-core count: Android does not expose the
    // topology in a stable way, and the matrix only uses logical slots.
    info.physical = info.logical;

    // Most ARM64 kernels answer this; a few vendor kernels return 0, in which
    // case the 64-byte default from CpuInfo is kept.
    const long line_size = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    if (line_size >= 32 && line_size <= 4096) {
        info.cache_line = static_cast<int>(line_size);
    }

    // ro.soc.model exists from Android 12; ro.board.platform and
    // ro.product.model cover older releases and the emulator.
    char property[PROP_VALUE_MAX] = {};
    if (__system_property_get("ro.soc.model", property) > 0 && property[0] != '\0') {
        info.model = property;
    } else if (__system_property_get("ro.board.platform", property) > 0 &&
               property[0] != '\0') {
        info.model = property;
    } else if (__system_property_get("ro.product.model", property) > 0 &&
               property[0] != '\0') {
        info.model = property;
    }
#else
    if (info.logical <= 0) {
        info.logical = static_cast<int>(std::thread::hardware_concurrency());
    }
#endif
    if (info.logical <= 0) {
        info.logical = 1;
    }
    if (info.physical <= 0) {
        info.physical = info.logical;
    }
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
    std::printf("Physical CPUs: %d\n", info.physical);
#if defined(_M_ARM64) || defined(__aarch64__)
    std::printf("Architecture: ARM64 (aarch64)\n");
#elif defined(__arm__)
    std::printf("Architecture: ARM (32-bit armeabi-v7a)\n");
#elif defined(_M_X64) || defined(__x86_64__)
    std::printf("Architecture: x64\n");
#elif defined(_M_IX86) || defined(__i386__)
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
    std::printf("Affinity: SetThreadAffinityMask (logical processor pinning)\n");
#elif defined(__APPLE__)
    std::printf("Affinity: best-effort scheduler hint only "
                "(macOS does not expose hard CPU pinning)\n");
#elif defined(__ANDROID__)
    std::printf("Affinity: sched_setaffinity (logical processor pinning, "
                "verified)\n");
#else
    std::printf("Affinity: platform fallback\n");
#endif
}

// ---------------------------------------------------------------------------
// Thread setup
// ---------------------------------------------------------------------------

void yield_current_thread();

// A short user-space pause for the handshake spin loops.  sched_yield() is a
// system call and it also hands the CPU to whatever else is runnable, which is
// the opposite of what a microsecond handshake needs: on the Android targets it
// accounted for roughly a third of the total CPU time of a matrix run.
inline void spin_pause() {
#if defined(_MSC_VER)
    // winnt.h maps this to PAUSE on x86 and YIELD on ARM64.
    YieldProcessor();
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

// Returns true when the requested placement was applied (or when no hard
// pinning was requested).  macOS only has best-effort hints, so it always
// reports success; Windows and Android/Linux report a failure that becomes N/A.
bool configure_current_thread(int cpu_index, bool use_affinity) {
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
        if (set_qos != nullptr) {
            set_qos(QOS_CLASS_USER_INTERACTIVE, 0);
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
#if defined(_WIN64)
        const std::uintptr_t mask =
            static_cast<std::uintptr_t>(1) << (cpu_index & 63);
#else
        const std::uintptr_t mask =
            static_cast<std::uintptr_t>(1) << (cpu_index & 31);
#endif
        // A zero return value means the mask was rejected (for example a
        // processor group that the thread cannot use).  Verify the actual
        // placement afterwards, because the mask alone is only a request.
        ok = SetThreadAffinityMask(GetCurrentThread(),
                                   static_cast<DWORD_PTR>(mask)) != 0;
        if (ok) {
            for (int attempt = 0; attempt < 64; ++attempt) {
                if (GetCurrentProcessorNumber() ==
                    static_cast<DWORD>(cpu_index)) {
                    break;
                }
                yield_current_thread();
            }
            ok = GetCurrentProcessorNumber() == static_cast<DWORD>(cpu_index);
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
#if defined(__ANDROID__)
            // bionic does not ship pthread_setaffinity_np; sched_setaffinity
            // with the thread id is the Linux equivalent.  A successful call is
            // not enough: a cpuset, an offline CPU or a vendor scheduler can
            // still leave the thread elsewhere, so verify both the effective
            // mask and the CPU the thread is actually running on.  The result
            // decides whether the pair is a real core-to-core sample or N/A.
            ok = sched_setaffinity(gettid(), sizeof(set), &set) == 0;
            if (ok) {
                cpu_set_t actual;
                CPU_ZERO(&actual);
                ok = sched_getaffinity(gettid(), sizeof(actual), &actual) == 0 &&
                     CPU_ISSET(cpu_index, &actual);
            }
            if (ok) {
                // setaffinity normally migrates a running task before it
                // returns; a short spin covers a slow scheduler.  Spinning
                // first avoids one sched_yield() syscall per attempt, and the
                // yield loop below stays as the fallback for a migration that
                // only happens at the next schedule point.
                ok = false;
                for (int attempt = 0; attempt < 64; ++attempt) {
                    if (sched_getcpu() == cpu_index) {
                        ok = true;
                        break;
                    }
                    spin_pause();
                }
                for (int attempt = 0; !ok && attempt < 16; ++attempt) {
                    yield_current_thread();
                    ok = sched_getcpu() == cpu_index;
                }
            }
#else
            ok = pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
            if (ok) {
                cpu_set_t actual;
                CPU_ZERO(&actual);
                ok = pthread_getaffinity_np(pthread_self(), sizeof(actual),
                                            &actual) == 0 &&
                     CPU_ISSET(cpu_index, &actual);
            }
#endif
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

    while (current <= limit) {
        std::uint64_t expected = current - 1;
        if (target.compare_exchange_strong(expected, current,
                                           std::memory_order_seq_cst,
                                           std::memory_order_seq_cst)) {
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

    while (current <= limit) {
        if (read_target.load(std::memory_order_acquire) == current - 1) {
            target.store(current, std::memory_order_release);
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
            configure_current_thread(config->cpu_index, config->use_affinity);
        {
            std::lock_guard<std::mutex> lock(gate->mutex);
            gate->ready.fetch_add(1, std::memory_order_acq_rel);
        }
        gate->cv.notify_all();

        for (int round = 0; round < config->rounds; ++round) {
            while (gate->round_go.load(std::memory_order_acquire) <= round) {
                if (gate->abort.load(std::memory_order_acquire)) {
                    gate->cv.notify_all();
                    return;
                }
                spin_pause();
            }
            if (gate->abort.load(std::memory_order_acquire)) {
                gate->cv.notify_all();
                return;
            }
#if !defined(__APPLE__) && !defined(_WIN32)
            // A vendor scheduler or a cpuset change can move the thread even
            // after the initial pin succeeded; keep the pair honest.
            if (config->affinity_ok && sched_getcpu() != config->cpu_index) {
                config->affinity_ok = false;
            }
            data->function(config);
            if (config->affinity_ok && sched_getcpu() != config->cpu_index) {
                config->affinity_ok = false;
            }
#else
            data->function(config);
#endif
            // The sample ends where the work ends, not where the main thread
            // gets around to noticing it.  Publishing the timestamp here keeps
            // the main thread's own wake-up delay out of the measurement.
            gate->round_end[config->slot] = now_ns();
            // The counter is bumped under the mutex so a notify cannot slip
            // into the window between the waiter's predicate check and its
            // registration.  The timestamp above is already written, so the
            // lock never sits inside the measured window.
            {
                std::lock_guard<std::mutex> lock(gate->mutex);
                gate->round_done.fetch_add(1, std::memory_order_acq_rel);
            }
            gate->cv.notify_all();
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
    float latency_ns = std::numeric_limits<float>::quiet_NaN();
    bool affinity_ok = false;
};

// Runs all samples of one core pair and returns the fastest one, which is the
// same statistic the original computed by storing n*n*runs values and taking
// the minimum afterwards.
PairOutcome run_pair(int processor1, int processor2, std::uint64_t iterations,
                     std::uint64_t runs, bool owned_mode, bool use_affinity) {
    // Separate 128-byte-aligned cells keep the two owners on distinct cache
    // lines on Apple Silicon (whose cache line is commonly 128 bytes).
    // Stack storage avoids two heap allocations per run.
    AtomicCell target1(owned_mode ? 1u : 0u);
    AtomicCell target2(0);

    StartGate gate;

    ThreadConfig config1;
    config1.start = owned_mode ? 3 : 1;
    config1.iterations = iterations;
    config1.target = &target1;
    config1.read_target = &target2;
    config1.cpu_index = processor1;
    config1.rounds = static_cast<int>(runs);
    config1.slot = 0;
    config1.use_affinity = use_affinity;
    config1.gate = &gate;

    ThreadConfig config2;
    config2.start = 2;
    config2.iterations = iterations;
    config2.target = owned_mode ? &target2 : &target1;
    config2.read_target = &target1;
    config2.cpu_index = processor2;
    config2.rounds = static_cast<int>(runs);
    config2.slot = 1;
    config2.use_affinity = use_affinity;
    config2.gate = &gate;

    WorkerThread thread1;
    WorkerThread thread2;

    const WorkerFunction worker = owned_mode ? owned_worker : bounce_worker;
    try {
        thread1.start(worker, &config1);
        thread2.start(worker, &config2);
    } catch (...) {
        gate.abort.store(true, std::memory_order_release);
        gate.round_go.store(config1.rounds, std::memory_order_release);
        gate.cv.notify_all();
        if (thread1.joinable()) {
            thread1.join();
        }
        if (thread2.joinable()) {
            thread2.join();
        }
        throw;
    }

    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        gate.cv.wait_for(lock, std::chrono::milliseconds(2000), [&gate] {
            return gate.ready.load(std::memory_order_acquire) >= 2 ||
                   gate.abort.load(std::memory_order_acquire);
        });
    }

    float minimum = std::numeric_limits<float>::infinity();
    try {
        for (int round = 0; round < config1.rounds; ++round) {
            // Both workers are waiting for the next round here, so the shared
            // cells can be reset without any additional synchronisation.
            target1.value.store(owned_mode ? 1u : 0u, std::memory_order_relaxed);
            target2.value.store(0u, std::memory_order_relaxed);
            gate.round_end[0] = 0;
            gate.round_end[1] = 0;

            const std::uint64_t start = now_ns();
            gate.round_go.store(round + 1, std::memory_order_release);
            {
                // Sleeping instead of spinning: the workers publish their own
                // end timestamps, so nothing is lost by waiting in the kernel.
                // The timeout only guards against a worker that died.
                std::unique_lock<std::mutex> lock(gate.mutex);
                gate.cv.wait_for(lock, std::chrono::milliseconds(500), [&gate, round] {
                    return gate.round_done.load(std::memory_order_acquire) >=
                               2 * (round + 1) ||
                           gate.abort.load(std::memory_order_acquire);
                });
            }
            if (gate.round_done.load(std::memory_order_acquire) <
                2 * (round + 1)) {
                throw std::runtime_error("benchmark thread did not finish a round");
            }
            const std::uint64_t end =
                gate.round_end[0] > gate.round_end[1] ? gate.round_end[0]
                                                      : gate.round_end[1];
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
        }
    } catch (...) {
        // Release any worker still waiting for a round, then join them before
        // propagating the error.
        gate.abort.store(true, std::memory_order_release);
        gate.round_go.store(config1.rounds, std::memory_order_release);
        gate.cv.notify_all();
        thread1.join();
        thread2.join();
        throw;
    }

    thread1.join();
    thread2.join();

    PairOutcome outcome;
    outcome.affinity_ok = config1.affinity_ok && config2.affinity_ok;
    outcome.latency_ns = outcome.affinity_ok
                             ? minimum
                             : std::numeric_limits<float>::quiet_NaN();
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
    result.filtered.assign(static_cast<std::size_t>(n) * static_cast<std::size_t>(n),
                           0.0f);

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
    if (options.progress) {
        print_progress(0, total_pairs);
    }
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            if (i == j) {
                continue;
            }
            const PairOutcome outcome =
                run_pair(i, j, options.iterations, options.runs, owned_mode,
                         options.use_affinity);
            result.filtered[matrix_index(n, i, j)] = outcome.latency_ns;
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

    // Same symmetry as the original: average each off-diagonal pair with its
    // transpose.  The diagonal is already zero.  A NaN (unpinned) cell keeps
    // both directions N/A, because averaging with NaN yields NaN.
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            const float average =
                (result.filtered[matrix_index(n, i, j)] +
                 result.filtered[matrix_index(n, j, i)]) /
                2.0f;
            result.filtered[matrix_index(n, i, j)] = average;
            result.filtered[matrix_index(n, j, i)] = average;
        }
    }

    result.unpinned_pairs = unpinned_pairs;
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
                if (value != value) {
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

// adb shell starts in "/" which is not writable, and Termux users may launch
// from a read-only directory.  When the implicit CSV path cannot be created the
// port retries in a directory that always exists on Android instead of aborting
// the run after the measurement has already been done.
#if defined(__ANDROID__)
std::string android_fallback_csv_path(const std::string& name) {
    const char* tmp = std::getenv("TMPDIR");
    if (tmp != nullptr && tmp[0] != '\0') {
        return std::string(tmp) + "/" + name;
    }
    return std::string("/data/local/tmp/") + name;
}
#endif

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
                if (value != value) {
                    std::fputs("N/A", output);
                } else {
                    // %.6g matches the original std::defaultfloat
                    // << setprecision(6).
                    std::fprintf(output, "%.6g", static_cast<double>(value));
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
    std::printf("CoherencyLatency %s%s", kVersion, " - native Android port of MicroBenchX CoherencyLatency\n\n");
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
        "detectable perflevel0 count (no effect on Android)\n"
        "      --csv PATH       CSV output path (default: core_latencies_<time>.csv "
        "in cwd)\n"
        "  -y, --yes            Do not wait for Enter before/after the test\n"
        "      --no-progress    Disable the progress bar\n"
        "      --no-affinity    Disable the best-effort affinity/QoS hint\n"
        "      --list           Print CPU information and exit\n"
        "  -h, --help           Show this help\n"
        "      --version        Show version\n\n");
    std::printf("%s", "Notes:\n"
        "  Android pins through sched_setaffinity and then verifies the\n"
        "  effective mask and the CPU each worker really runs on.  When a\n"
        "  worker cannot be pinned (offline CPU, cpuset, big.LITTLE or vendor\n"
        "  scheduler), the affected cells are reported as N/A instead of as a\n"
        "  number measured on the wrong core.  The matrix is not a verified\n"
        "  core-to-core topology; its indices are logical slots, not physical\n"
        "  core IDs.\n"
        "  Phones throttle quickly: keep runs short, unplug thermal clauses by\n"
        "  closing other apps, and prefer --cpus 2/4 for exploratory runs.\n"
        "  A full all-core matrix can take minutes on a phone.\n");
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
#elif defined(__ANDROID__)
        std::printf("Placement: Android affinity requests are advisory; matrix "
                    "indices are logical slots, not verified physical core IDs.\n");
#endif

#if defined(__APPLE__)
        if (sysctl_int("sysctl.proc_translated", 0) != 0) {
            std::printf("Warning: running under Rosetta 2; this is not a native "
                        "arm64 execution. Use the arm64 build.\n");
        }
#elif defined(__ANDROID__) && defined(__aarch64__)
        std::printf("Runtime: Android ARM64 (aarch64), native\n");
#elif defined(__ANDROID__) && defined(__arm__)
        std::printf("Runtime: Android armeabi-v7a (32-bit ARM)\n");
#elif defined(__ANDROID__) && (defined(__x86_64__) || defined(__amd64__))
        std::printf("Runtime: Android x86_64%s\n",
                    cpu.model == "ranchu" ? " (emulator)" : "");
#elif defined(__ANDROID__)
        std::printf("Runtime: Android x86 (32-bit)%s\n",
                    cpu.model == "ranchu" ? " (emulator)" : "");
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
        // Measurement is already complete here, so an unwritable working
        // directory must not discard the results.  On Android the implicit
        // file is retried once inside a guaranteed-writable directory.  An
        // explicit --csv path is still an error so user intent is respected.
        std::string written_path;
        try {
            write_csv(csv_path, result);
            written_path = csv_path;
        } catch (const std::runtime_error& error) {
#if defined(__ANDROID__)
            if (options.csv_path.empty()) {
                const std::string fallback = android_fallback_csv_path(csv_path);
                write_csv(fallback, result);
                std::printf("Note: working directory is not writable (%s)\n",
                            error.what());
                written_path = fallback;
            }
#endif
            if (written_path.empty()) {
                throw;
            }
        }
        std::printf("Latencies written to %s\n", written_path.c_str());

        if (!options.assume_yes) {
            wait_for_enter("Press Enter to close...");
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 2;
    }
}
