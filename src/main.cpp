// CoherencyLatency - native macOS/ARM64 port of MicroBenchX CoherencyLatency.
//
// Original project: https://github.com/CXWorld/MicroBenchX (tag v1.0.5)
// Original file:    source/MicroBenchX.Icl/CoherencyLatency.cpp
// Original license: MIT, Copyright (c) 2021 CapFrameX
//
// This port replaces the Windows threading/affinity/timing layer with portable
// C++17 + POSIX APIs.  macOS does not provide hard CPU pinning; the affinity
// policy used here is a best-effort scheduler hint only (see README.md).

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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
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
#endif

namespace {

constexpr const char* kVersion = "1.0.0-macos";
constexpr int kBarWidth = 60;

// Apple Silicon cache lines are commonly 128 bytes; keep this a compile-time
// value so AtomicCell has a fixed layout.  The sysctl value is also reported
// at runtime by --list.
constexpr std::size_t kAtomicAlignment = 128;

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

struct StartGate {
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<bool> abort{false};
};

struct ThreadConfig {
    std::uint64_t start = 0;
    std::uint64_t iterations = 0;
    AtomicCell* target = nullptr;
    AtomicCell* read_target = nullptr;
    int cpu_index = 0;
    bool use_affinity = true;
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
    std::vector<float> filtered;
};

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

std::uint64_t now_ns() {
#if defined(__APPLE__)
    // mach_absolute_time works back to the earliest supported macOS versions,
    // unlike clock_gettime (10.12+) and std::chrono::system_clock (10.12+).
    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t value{};
        mach_timebase_info(&value);
        return value;
    }();
    return static_cast<std::uint64_t>(mach_absolute_time()) *
           static_cast<std::uint64_t>(timebase.numer) /
           static_cast<std::uint64_t>(timebase.denom);
#elif defined(_WIN32)
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (QueryPerformanceFrequency(&frequency) && QueryPerformanceCounter(&counter)) {
        return static_cast<std::uint64_t>(
            (static_cast<double>(counter.QuadPart) * 1000000000.0) /
            static_cast<double>(frequency.QuadPart));
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

#if defined(__APPLE__)
    info.logical = sysctl_int("hw.logicalcpu", 0);
    info.physical = sysctl_int("hw.physicalcpu", info.logical);
    info.perf_level0 = perf_level_cores("hw.perflevel0.logicalcpu");
    info.perf_level1 = perf_level_cores("hw.perflevel1.logicalcpu");
    info.cache_line = sysctl_int("hw.cachelinesize", 128);
    info.model = sysctl_string("machdep.cpu.brand_string");
#endif

    if (info.logical <= 0) {
        info.logical = static_cast<int>(std::thread::hardware_concurrency());
    }
#if defined(_WIN32)
    // Some sandboxed/containerized Windows processes report one processor via
    // hardware_concurrency(); GetActiveProcessorCount gives the host view.
    if (info.logical <= 1) {
        const DWORD active = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        if (active > 0) {
            info.logical = static_cast<int>(active);
        }
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
    std::cout << "CPU: " << (info.model.empty() ? "unknown" : info.model) << '\n';
    std::cout << "Logical CPUs: " << info.logical << '\n';
    std::cout << "Physical CPUs: " << info.physical << '\n';
    if (info.perf_level0 > 0) {
        std::cout << "Performance-level cores (perflevel0): " << info.perf_level0
                  << '\n';
    }
    if (info.perf_level1 > 0) {
        std::cout << "Efficiency-level cores (perflevel1): " << info.perf_level1
                  << '\n';
    }
    std::cout << "Cache line: " << info.cache_line << " bytes\n";
#if defined(__APPLE__)
    std::cout << "Affinity: best-effort scheduler hint only "
                 "(macOS does not expose hard CPU pinning)\n";
#else
    std::cout << "Affinity: platform fallback\n";
#endif
}

// ---------------------------------------------------------------------------
// Thread setup
// ---------------------------------------------------------------------------

void configure_current_thread(int cpu_index, bool use_affinity) {
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
#elif defined(_WIN32)
    if (use_affinity) {
        const std::uintptr_t mask =
            static_cast<std::uintptr_t>(1) << (cpu_index & 63);
        SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(mask));
    }
#else
    if (use_affinity) {
        cpu_set_t set;
        CPU_ZERO(&set);
        if (cpu_index >= 0 && cpu_index < CPU_SETSIZE) {
            CPU_SET(cpu_index, &set);
            pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
        }
    }
    char name[32] = {};
    std::snprintf(name, sizeof(name), "cl-%d", cpu_index);
    pthread_setname_np(pthread_self(), name);
#endif
}

bool begin_worker(ThreadConfig* config) {
    configure_current_thread(config->cpu_index, config->use_affinity);
    config->gate->ready.fetch_add(1, std::memory_order_acq_rel);
    while (!config->gate->go.load(std::memory_order_acquire)) {
        if (config->gate->abort.load(std::memory_order_acquire)) {
            return false;
        }
        std::this_thread::yield();
    }
    return !config->gate->abort.load(std::memory_order_acquire);
}

// Bounce mode: both threads contend with a compare-and-swap on the same
// cache line.  This is the direct analogue of _InterlockedCompareExchange64.
void bounce_worker(ThreadConfig* config) {
    if (!begin_worker(config)) {
        return;
    }

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
    if (!begin_worker(config)) {
        return;
    }

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

float time_threads(int processor1, int processor2, std::uint64_t iterations,
                   ThreadConfig config1, ThreadConfig config2,
                   WorkerFunction worker, bool use_affinity) {
    StartGate gate;
    config1.cpu_index = processor1;
    config2.cpu_index = processor2;
    config1.use_affinity = use_affinity;
    config2.use_affinity = use_affinity;
    config1.gate = &gate;
    config2.gate = &gate;

    std::thread thread1;
    std::thread thread2;

    try {
        thread1 = std::thread(worker, &config1);
        thread2 = std::thread(worker, &config2);
    } catch (...) {
        gate.abort.store(true, std::memory_order_release);
        gate.go.store(true, std::memory_order_release);
        if (thread1.joinable()) {
            thread1.join();
        }
        if (thread2.joinable()) {
            thread2.join();
        }
        throw;
    }

    while (gate.ready.load(std::memory_order_acquire) < 2) {
        std::this_thread::yield();
    }

    const std::uint64_t start = now_ns();
    gate.go.store(true, std::memory_order_release);

    thread1.join();
    thread2.join();

    const std::uint64_t end = now_ns();
    if (end <= start) {
        throw std::runtime_error("monotonic clock did not advance");
    }

    // Each thread performs 'iterations' successful lock operations, so the
    // average latency per locked operation is total time / (2 * iterations).
    const double latency_ns =
        static_cast<double>(end - start) / static_cast<double>(iterations) / 2.0;
    return static_cast<float>(latency_ns);
}

float run_bounce(int processor1, int processor2, std::uint64_t iterations,
                 bool use_affinity) {
    // Use stack allocation: the 128-byte-aligned cell is created and destroyed
    // per run, so a heap allocation here is pure overhead and fragmentation.
    AtomicCell cell(0);

    ThreadConfig config1;
    config1.start = 1;
    config1.iterations = iterations;
    config1.target = &cell;

    ThreadConfig config2;
    config2.start = 2;
    config2.iterations = iterations;
    config2.target = &cell;

    return time_threads(processor1, processor2, iterations, config1, config2,
                        bounce_worker, use_affinity);
}

float run_owned(int processor1, int processor2, std::uint64_t iterations,
                bool use_affinity) {
    // Separate 128-byte-aligned cells keep the two owners on distinct cache
    // lines on Apple Silicon (whose cache line is commonly 128 bytes).
    // Stack storage avoids two heap allocations per run.
    AtomicCell target1(1);
    AtomicCell target2(0);

    ThreadConfig config1;
    config1.start = 3;
    config1.iterations = iterations;
    config1.target = &target1;
    config1.read_target = &target2;

    ThreadConfig config2;
    config2.start = 2;
    config2.iterations = iterations;
    config2.target = &target2;
    config2.read_target = &target1;

    return time_threads(processor1, processor2, iterations, config1, config2,
                        owned_worker, use_affinity);
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

    std::cout << '[';
    for (int i = 0; i < kBarWidth; ++i) {
        if (i < pos) {
            std::cout << '=';
        } else if (i == pos) {
            std::cout << '>';
        } else {
            std::cout << ' ';
        }
    }
    std::cout << "] " << static_cast<int>(progress * 100.0) << " %\r"
              << std::flush;
}

MatrixResult run_matrix(const Options& options, int n) {
    const std::size_t runs = static_cast<std::size_t>(options.runs);

    MatrixResult result;
    result.n = n;
    result.filtered.assign(static_cast<std::size_t>(n) * static_cast<std::size_t>(n),
                           0.0f);

    // Keep only one accumulator per core pair.  The previous version stored
    // n*n*runs floats and reduced them in a second pass; taking the minimum
    // online produces the same matrix while using about 'runs' times less
    // memory and avoiding a second full traversal.
    const bool owned_mode = options.mode == Mode::Owned;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            if (i == j) {
                continue;
            }

            float minimum = std::numeric_limits<float>::infinity();
            for (std::size_t k = 0; k < runs; ++k) {
                const float value =
                    owned_mode
                        ? run_owned(i, j, options.iterations, options.use_affinity)
                        : run_bounce(i, j, options.iterations,
                                     options.use_affinity);
                if (value < minimum) {
                    minimum = value;
                }
            }
            result.filtered[matrix_index(n, i, j)] = minimum;
        }

        if (options.progress) {
            print_progress(i + 1, n);
        }
    }

    if (options.progress) {
        std::cout << '\n';
    }

    // Same symmetry as the original: average each off-diagonal pair with its
    // transpose.  The diagonal is already zero.
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

    return result;
}

void print_matrix(const MatrixResult& result) {
    std::cout << std::fixed << std::setprecision(6);
    for (int i = 0; i < result.n; ++i) {
        for (int j = 0; j < result.n; ++j) {
            if (j != 0) {
                std::cout << ',';
            }
            if (i == j) {
                std::cout << 'x';
            } else {
                std::cout << result.filtered[matrix_index(result.n, i, j)];
            }
        }
        std::cout << '\n';
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

    std::ostringstream filename;
    filename << "core_latencies_"
             << std::put_time(&local_time, "%Y%m%d_%H%M%S") << ".csv";
    return filename.str();
}

void write_csv(const std::string& path, const MatrixResult& result) {
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        throw std::runtime_error("failed to open CSV file for writing: " + path);
    }

    // Match the original stream formatting: defaultfloat with precision 6.
    output << std::defaultfloat << std::setprecision(6);

    // Preserve the original semicolon-delimited layout exactly.
    output << ';';
    for (int i = 0; i < result.n; ++i) {
        if (i != 0) {
            output << ';';
        }
        output << "Core " << i;
    }
    output << '\n';

    for (int i = 0; i < result.n; ++i) {
        output << "Core " << i << ';';
        for (int j = 0; j < result.n; ++j) {
            if (j != 0) {
                output << ';';
            }
            if (i == j) {
                output << 'x';
            } else {
                output << result.filtered[matrix_index(result.n, i, j)];
            }
        }
        output << '\n';
    }

    if (!output) {
        throw std::runtime_error("failed while writing CSV file: " + path);
    }
    output.close();
    if (!output) {
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
    std::cout
        << "CoherencyLatency " << kVersion
        << " - native macOS/ARM64 port of MicroBenchX CoherencyLatency\n\n"
        << "Usage:\n"
        << "  " << program << " [options]\n"
        << "  " << program << " [iterations] [bounce|owned]\n\n"
        << "Options:\n"
        << "  -i, --iterations N   Successful lock operations per thread "
           "(default: 1000000)\n"
        << "  -r, --runs N         Samples per core pair (default: 10)\n"
        << "  -m, --mode MODE      bounce (same cache line, default) or owned "
           "(separate lines)\n"
        << "      --cpus N         Use at most N logical slots (default: all logical "
           "CPUs)\n"
        << "      --perf-only      On Apple Silicon, limit logical slots to the "
           "detectable perflevel0 count\n"
        << "      --csv PATH       CSV output path (default: core_latencies_<time>.csv "
           "in cwd)\n"
        << "  -y, --yes            Do not wait for Enter before/after the test\n"
        << "      --no-progress    Disable the progress bar\n"
        << "      --no-affinity    Disable the best-effort affinity/QoS hint\n"
        << "      --list           Print CPU information and exit\n"
        << "  -h, --help           Show this help\n"
        << "      --version        Show version\n\n"
        << "Notes:\n"
        << "  macOS cannot pin threads to individual cores.  The port uses a\n"
        << "  best-effort thread-affinity policy hint; results can vary with the\n"
        << "  scheduler, P/E-core placement, thermal state and background load.\n"
        << "  The matrix is not a verified core-to-core topology; its indices are\n"
        << "  logical slots, not physical core IDs.\n"
        << "  For reproducible numbers, close other applications, connect power,\n"
        << "  and consider --perf-only on Apple Silicon.\n";
}

bool stdout_is_tty() {
#if defined(_WIN32)
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

void wait_for_enter(const char* prompt) {
    std::cout << prompt << std::flush;
    std::string line;
    std::getline(std::cin, line);
}

const char* mode_name(Mode mode) {
    return mode == Mode::Owned ? "owned" : "bounce";
}

}  // namespace

int main(int argc, char** argv) {
    std::ios::sync_with_stdio(false);

    try {
        Options options = parse_options(argc, argv);

        if (options.show_help) {
            print_usage(argv[0]);
            return 0;
        }
        if (options.show_version) {
            std::cout << "CoherencyLatency " << kVersion << '\n';
            return 0;
        }

        const CpuInfo cpu = read_cpu_info();
        std::cout << "CoherencyLatency " << kVersion << '\n';
        print_cpu_info(cpu);

#if defined(__APPLE__)
        std::cout << "Placement: macOS provides no hard CPU pinning; matrix indices "
                     "are logical slots, not verified physical core IDs.\n";
#endif

#if defined(__APPLE__)
        if (sysctl_int("sysctl.proc_translated", 0) != 0) {
            std::cout << "Warning: running under Rosetta 2; this is not a native "
                         "arm64 execution. Use the arm64 build.\n";
        }
#elif !defined(__aarch64__) && !defined(_M_ARM64)
        std::cout << "Note: this is a non-ARM development build; affinity is a "
                     "fallback.\n";
#endif

        if (options.list_only) {
            return 0;
        }

        int cpu_count = cpu.logical;
        if (options.perf_only) {
            if (cpu.perf_level0 > 0) {
                cpu_count = cpu.perf_level0;
            } else {
                std::cout << "Note: perflevel0 core count is unavailable; using "
                             "all logical CPUs.\n";
            }
        }
        if (options.cpus > 0) {
            cpu_count = std::min(cpu_count, options.cpus);
        }
        if (cpu_count < 2) {
            throw std::runtime_error("at least 2 CPUs are required");
        }

        std::cout << "Mode: " << mode_name(options.mode)
                  << "; CPUs: " << cpu_count
                  << "; iterations: " << options.iterations
                  << "; runs: " << options.runs << '\n';

        if (!options.assume_yes) {
            wait_for_enter("Press Enter to start the test...");
        }

        if (!stdout_is_tty()) {
            options.progress = false;
        }

        const MatrixResult result = run_matrix(options, cpu_count);

        std::cout << "Latency matrix (ns per operation; diagonal x):\n";
        print_matrix(result);

        const std::string csv_path =
            options.csv_path.empty() ? default_csv_name() : options.csv_path;
        write_csv(csv_path, result);
        std::cout << "Latencies written to " << csv_path << '\n';

        if (!options.assume_yes) {
            wait_for_enter("Press Enter to close...");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
