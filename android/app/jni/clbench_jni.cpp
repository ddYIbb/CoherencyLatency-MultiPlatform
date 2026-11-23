// JNI bridge that runs the CoherencyLatency matrix for the Android UI.
//
// The measurement core is the same code as src/main.cpp (bounce / owned
// workers, round-based thread pair reuse, min-over-runs statistic, symmetric
// averaging).  The CLI presentation layer is replaced by a blocking call that
// reports every finished core pair to a Java callback, so the activity can
// update its matrix while the measurement is still running.

#include <jni.h>
#include <sched.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Measurement core
// ---------------------------------------------------------------------------

constexpr std::size_t kAtomicAlignment = 64;
constexpr std::size_t kWorkerStackBytes = 128 * 1024;

struct alignas(kAtomicAlignment) AtomicCell {
    std::atomic<std::uint64_t> value;

    explicit AtomicCell(std::uint64_t initial = 0) : value(initial) {}
};

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
    int rounds = 0;
    // Index into StartGate::round_end; 0 for the first worker, 1 for the second.
    int slot = 0;
    StartGate* gate = nullptr;
    // Set by the worker after sched_setaffinity.  A false value means this
    // worker could not be pinned, so its pair is reported as N/A instead of
    // pretending the number belongs to the requested CPU pair.
    bool affinity_ok = false;
};

std::uint64_t now_ns() {
    struct timespec ts {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
               static_cast<std::uint64_t>(ts.tv_nsec);
    }
    return 0;
}

inline void spin_pause();

bool configure_current_thread(int cpu_index) {
    // bionic has no pthread_setaffinity_np; sched_setaffinity with the thread
    // id is the Linux equivalent.  A successful call is not enough: a cpuset,
    // offline CPU or vendor scheduler may reject/ignore the request, so verify
    // both the effective mask and the CPU the thread is actually running on.
    // The result decides whether the pair is a real core-to-core sample or
    // N/A.
    if (cpu_index < 0 || cpu_index >= CPU_SETSIZE) {
        char name[32] = {};
        std::snprintf(name, sizeof(name), "cl-%d", cpu_index);
        pthread_setname_np(pthread_self(), name);
        return false;
    }
    bool pinned = false;
    {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu_index, &set);
        if (sched_setaffinity(gettid(), sizeof(set), &set) == 0) {
            cpu_set_t effective;
            CPU_ZERO(&effective);
            if (sched_getaffinity(gettid(), sizeof(effective), &effective) == 0 &&
                CPU_ISSET(cpu_index, &effective)) {
                // setaffinity normally migrates a running task before it
                // returns; a short pause loop covers a slow scheduler.  A
                // pause is better than sched_yield() here: yielding lets the
                // scheduler move the thread away again, which is the opposite
                // of what this loop waits for.
                for (int attempt = 0; attempt < 64; ++attempt) {
                    if (sched_getcpu() == cpu_index) {
                        pinned = true;
                        break;
                    }
                    spin_pause();
                }
                // Fall back to real yields in case the CPU is still busy with
                // the thread we replaced.
                if (!pinned) {
                    for (int attempt = 0; attempt < 16; ++attempt) {
                        if (sched_getcpu() == cpu_index) {
                            pinned = true;
                            break;
                        }
                        sched_yield();
                    }
                }
            }
        }
    }
    char name[32] = {};
    std::snprintf(name, sizeof(name), "cl-%d", cpu_index);
    pthread_setname_np(pthread_self(), name);
    return pinned;
}

[[maybe_unused]] inline void yield_current_thread() {
    std::this_thread::yield();
}

// A short user-space pause for the handshake spin loops.  sched_yield() is a
// system call and it also hands the CPU to whatever else is runnable, which is
// the opposite of what a microsecond handshake needs: on the Android targets it
// accounted for roughly a third of the total CPU time of a matrix run.
inline void spin_pause() {
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

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

class WorkerThread {
public:
    WorkerThread() = default;
    ~WorkerThread() { join(); }

    WorkerThread(const WorkerThread&) = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;

    void start(WorkerFunction function, ThreadConfig* config) {
        start_data_.function = function;
        start_data_.config = config;

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
    }

    void join() {
        if (started_) {
            pthread_join(thread_, nullptr);
            started_ = false;
        }
    }

    bool joinable() const { return started_; }

private:
    struct StartData {
        WorkerFunction function = nullptr;
        ThreadConfig* config = nullptr;
    };

    static void run(StartData* data) {
        ThreadConfig* config = data->config;
        StartGate* gate = config->gate;
        config->affinity_ok = configure_current_thread(config->cpu_index);
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
            // Re-check after every round: a vendor scheduler or cpuset change
            // can silently move the thread even when the initial pin worked.
            if (config->affinity_ok && sched_getcpu() != config->cpu_index) {
                config->affinity_ok = false;
            }
            data->function(config);
            if (config->affinity_ok && sched_getcpu() != config->cpu_index) {
                config->affinity_ok = false;
            }
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

    static void* entry(void* parameter) {
        run(static_cast<StartData*>(parameter));
        return nullptr;
    }

    pthread_t thread_{};
    bool started_ = false;
    StartData start_data_{};
};

// Set when the user presses Stop (or the activity goes away).
std::atomic<bool> g_abort{false};

bool abort_requested() {
    return g_abort.load(std::memory_order_acquire);
}

void abort_pair(StartGate& gate, int rounds) {
    gate.abort.store(true, std::memory_order_release);
    gate.round_go.store(rounds, std::memory_order_release);
}

struct PairOutcome {
    float latency_ns = std::numeric_limits<float>::quiet_NaN();
    bool affinity_ok = false;
    bool aborted = true;
};

// Sleeps on the gate until the predicate is true.  The wait is sliced so a
// Stop request from the UI is still noticed while the main thread sleeps; the
// slice is short enough to be invisible to the user and long enough that the
// main thread no longer burns a core in sched_yield() polling.
constexpr std::chrono::milliseconds kWaitSlice(20);

template <typename Predicate>
bool wait_for_condition(StartGate& gate, std::chrono::milliseconds timeout,
                        Predicate predicate) {
    std::unique_lock<std::mutex> lock(gate.mutex);
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (predicate()) {
            return true;
        }
        if (gate.abort.load(std::memory_order_acquire) || abort_requested()) {
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        gate.cv.wait_for(lock, kWaitSlice);
    }
}

// Runs one core pair and returns both its fastest sample and whether both
// workers really stayed on the requested CPUs.  A failed pin is not an abort:
// the pair is still measured, but its result is reported as N/A.
PairOutcome run_pair(int processor1, int processor2, std::uint64_t iterations,
                     std::uint64_t runs, bool owned_mode) {
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
    config1.gate = &gate;

    ThreadConfig config2;
    config2.start = 2;
    config2.iterations = iterations;
    config2.target = owned_mode ? &target2 : &target1;
    config2.read_target = &target1;
    config2.cpu_index = processor2;
    config2.rounds = static_cast<int>(runs);
    config2.slot = 1;
    config2.gate = &gate;

    WorkerThread thread1;
    WorkerThread thread2;
    const WorkerFunction worker = owned_mode ? owned_worker : bounce_worker;

    try {
        thread1.start(worker, &config1);
        thread2.start(worker, &config2);

        if (!wait_for_condition(gate, std::chrono::milliseconds(2000),
                                [&gate] {
                                    return gate.ready.load(
                                               std::memory_order_acquire) >= 2;
                                })) {
            abort_pair(gate, config1.rounds);
            gate.cv.notify_all();
            thread1.join();
            thread2.join();
            if (abort_requested()) {
                return PairOutcome{};
            }
            throw std::runtime_error("benchmark thread did not start");
        }

        float minimum = std::numeric_limits<float>::infinity();
        for (int round = 0; round < config1.rounds; ++round) {
            if (abort_requested()) {
                abort_pair(gate, config1.rounds);
                thread1.join();
                thread2.join();
                return PairOutcome{};
            }

            target1.value.store(owned_mode ? 1u : 0u, std::memory_order_relaxed);
            target2.value.store(0u, std::memory_order_relaxed);
            gate.round_end[0] = 0;
            gate.round_end[1] = 0;

            const std::uint64_t begin = now_ns();
            gate.round_go.store(round + 1, std::memory_order_release);
            const bool finished = wait_for_condition(
                gate, std::chrono::milliseconds(500), [&gate, round] {
                    return gate.round_done.load(std::memory_order_acquire) >=
                           2 * (round + 1);
                });
            if (!finished) {
                abort_pair(gate, config1.rounds);
                gate.cv.notify_all();
                thread1.join();
                thread2.join();
                if (abort_requested()) {
                    return PairOutcome{};
                }
                throw std::runtime_error(
                    "benchmark thread did not finish a round");
            }
            // The workers publish their own end timestamps; the window closes
            // where the slower of the two stopped working.
            const std::uint64_t end =
                gate.round_end[0] > gate.round_end[1] ? gate.round_end[0]
                                                      : gate.round_end[1];
            if (end <= begin) {
                throw std::runtime_error("monotonic clock did not advance");
            }

            const double latency_ns = static_cast<double>(end - begin) /
                                      static_cast<double>(iterations) / 2.0;
            const float value = static_cast<float>(latency_ns);
            if (value < minimum) {
                minimum = value;
            }
        }

        thread1.join();
        thread2.join();
        PairOutcome outcome;
        outcome.latency_ns = minimum;
        outcome.affinity_ok = config1.affinity_ok && config2.affinity_ok;
        outcome.aborted = false;
        return outcome;
    } catch (...) {
        abort_pair(gate, config1.rounds);
        gate.cv.notify_all();
        thread1.join();
        thread2.join();
        throw;
    }
}

// ---------------------------------------------------------------------------
// Java callback sink
// ---------------------------------------------------------------------------

struct ProgressSink {
    JNIEnv* env = nullptr;
    jobject callback = nullptr;
    jmethodID on_pair = nullptr;
    bool failed = false;
};

void emit_pair(ProgressSink& sink, int first, int second, float latency_ns,
               int done, int total) {
    if (sink.failed || sink.env == nullptr || sink.callback == nullptr ||
        sink.on_pair == nullptr) {
        return;
    }
    sink.env->CallVoidMethod(sink.callback, sink.on_pair, first, second,
                             latency_ns, done, total);
    if (sink.env->ExceptionCheck()) {
        sink.env->ExceptionClear();
        sink.failed = true;
    }
}

// ---------------------------------------------------------------------------
// Device information
// ---------------------------------------------------------------------------

std::string read_property(const char* name) {
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get(name, value) > 0) {
        return std::string(value);
    }
    return std::string();
}

std::string architecture_name() {
#if defined(__aarch64__)
    return "arm64-v8a";
#elif defined(__arm__)
    return "armeabi-v7a";
#elif defined(__x86_64__)
    return "x86_64";
#elif defined(__i386__)
    return "x86";
#else
    return "unknown";
#endif
}

std::string read_topology_text(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "r");
    if (file == nullptr) {
        return {};
    }
    char buffer[128] = {};
    const bool ok = std::fgets(buffer, sizeof(buffer), file) != nullptr;
    std::fclose(file);
    if (!ok) {
        return {};
    }
    std::string value(buffer);
    while (!value.empty() &&
           (value.back() == '\n' || value.back() == '\r' ||
            value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    return value;
}

int read_topology_int(const std::string& path) {
    const std::string text = read_topology_text(path);
    if (text.empty()) {
        return -1;
    }
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') {
        return -1;
    }
    return static_cast<int>(value);
}

// Informational only: the matrix still uses logical slots.  Returns 0 when the
// kernel does not expose the topology, never a guessed number.
int physical_core_count(int logical) {
    std::vector<long long> keys;
    keys.reserve(static_cast<std::size_t>(logical));
    for (int cpu = 0; cpu < logical; ++cpu) {
        const std::string base =
            "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/";
        const int package = read_topology_int(base + "topology/physical_package_id");
        const int core = read_topology_int(base + "topology/core_id");
        if (package < 0 || core < 0) {
            continue;
        }
        const long long key =
            static_cast<long long>(package) * 100000LL + core;
        bool duplicate = false;
        for (const long long previous : keys) {
            if (previous == key) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            keys.push_back(key);
        }
    }
    return static_cast<int>(keys.size());
}

// "model|logical cores|cache line|abi|physical cores (0 = unknown)"
std::string device_info() {
    std::string model = read_property("ro.soc.model");
    if (model.empty()) {
        model = read_property("ro.board.platform");
    }
    if (model.empty()) {
        model = read_property("ro.product.model");
    }
    if (model.empty()) {
        model = "Android";
    }

    // Android 4.x bionic reports 1 for _SC_NPROCESSORS_CONF, so take the
    // larger of the two counters instead of trusting CONF alone.
    long configured = sysconf(_SC_NPROCESSORS_CONF);
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    if (online > configured) {
        configured = online;
    }
    if (configured <= 0) {
        configured = 1;
    }

    long line_size = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    if (line_size < 32 || line_size > 4096) {
        line_size = 64;
    }

    const int physical = physical_core_count(static_cast<int>(configured));

    char buffer[256] = {};
    std::snprintf(buffer, sizeof(buffer), "%s|%ld|%ld|%s|%d", model.c_str(),
                  configured, line_size, architecture_name().c_str(), physical);
    return std::string(buffer);
}

// ---------------------------------------------------------------------------
// CSV
// ---------------------------------------------------------------------------

std::string build_csv(const std::vector<float>& matrix, int n) {
    std::string output;
    output.reserve(static_cast<std::size_t>(n) * (n * 12 + 8) + 16);

    char buffer[64] = {};

    // Preserve the CLI layout: leading ';' then the column headers.
    output.push_back(';');
    for (int column = 0; column < n; ++column) {
        if (column != 0) {
            output.push_back(';');
        }
        std::snprintf(buffer, sizeof(buffer), "Core %d", column);
        output += buffer;
    }
    output.push_back('\n');

    for (int row = 0; row < n; ++row) {
        std::snprintf(buffer, sizeof(buffer), "Core %d;", row);
        output += buffer;
        for (int column = 0; column < n; ++column) {
            if (column != 0) {
                output.push_back(';');
            }
            if (row == column) {
                output.push_back('x');
            } else {
                const float value =
                    matrix[static_cast<std::size_t>(row) * n + column];
                if (std::isnan(value)) {
                    output += "N/A";
                } else {
                    std::snprintf(buffer, sizeof(buffer), "%.6g",
                                  static_cast<double>(value));
                    output += buffer;
                }
            }
        }
        output.push_back('\n');
    }

    return output;
}

// ---------------------------------------------------------------------------
// Result state shared with the Java side
// ---------------------------------------------------------------------------

struct ResultState {
    std::mutex mutex;
    std::vector<float> matrix;
    int n = 0;
    std::string csv;
    std::string error;
    bool valid = false;
};

ResultState g_result;

void store_result(std::vector<float> matrix, int n) {
    std::string csv = build_csv(matrix, n);
    std::lock_guard<std::mutex> lock(g_result.mutex);
    g_result.matrix = std::move(matrix);
    g_result.n = n;
    g_result.csv = std::move(csv);
    g_result.valid = true;
}

void store_error(const std::string& message) {
    std::lock_guard<std::mutex> lock(g_result.mutex);
    g_result.error = message;
    g_result.valid = false;
}

// ---------------------------------------------------------------------------
// JNI entry points
// ---------------------------------------------------------------------------

jstring to_java_string(JNIEnv* env, const std::string& value) {
    return env->NewStringUTF(value.c_str());
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_ddyibb_coherencylatency_NativeBench_deviceInfo(JNIEnv* env, jclass) {
    return to_java_string(env, device_info());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ddyibb_coherencylatency_NativeBench_runMatrix(JNIEnv* env, jclass,
                                                       jint cores,
                                                       jlong iterations,
                                                       jint runs,
                                                       jboolean owned_mode,
                                                       jobject callback) {
    if (cores < 1) {
        return 2;
    }
    if (iterations < 1 || iterations > 100000000LL) {
        return 3;
    }
    if (runs < 1 || runs > 1000) {
        return 4;
    }

    g_abort.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_result.mutex);
        g_result.valid = false;
        g_result.error.clear();
    }

    const int n = static_cast<int>(cores);
    const int total_pairs = n * n - n;

    ProgressSink sink;
    if (callback != nullptr) {
        sink.env = env;
        sink.callback = callback;
        jclass callback_class = env->GetObjectClass(callback);
        sink.on_pair = env->GetMethodID(callback_class, "onPair", "(IIFII)V");
        if (sink.on_pair == nullptr) {
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
            }
            store_error("progress callback is missing onPair(IIFII)V");
            return 5;
        }
    }

    std::vector<float> matrix(static_cast<std::size_t>(n) * n, 0.0f);
    int done_pairs = 0;
    bool aborted = false;

    try {
        for (int row = 0; row < n && !aborted; ++row) {
            for (int column = 0; column < n; ++column) {
                if (row == column) {
                    continue;
                }
                if (abort_requested()) {
                    aborted = true;
                    break;
                }
                const PairOutcome pair =
                    run_pair(row, column,
                             static_cast<std::uint64_t>(iterations),
                             static_cast<std::uint64_t>(runs),
                             owned_mode == JNI_TRUE);
                if (pair.aborted) {
                    aborted = true;
                    break;
                }
                // A pair whose workers could not really be pinned is still
                // consumed, so the remaining pairs finish; its value is NaN and
                // the Java layer renders/export it as N/A.
                const float value =
                    pair.affinity_ok
                        ? pair.latency_ns
                        : std::numeric_limits<float>::quiet_NaN();
                matrix[static_cast<std::size_t>(row) * n + column] = value;
                ++done_pairs;

                emit_pair(sink, row, column, value, done_pairs, total_pairs);
                if (sink.failed) {
                    aborted = true;
                    break;
                }
            }
        }
    } catch (const std::exception& error) {
        store_error(error.what());
        return 6;
    } catch (...) {
        store_error("unknown native error");
        return 6;
    }

    if (aborted) {
        return 1;
    }

    // Same symmetry as the CLI: average every pair with its transpose.
    for (int row = 0; row < n; ++row) {
        for (int column = row + 1; column < n; ++column) {
            const std::size_t first = static_cast<std::size_t>(row) * n + column;
            const std::size_t second = static_cast<std::size_t>(column) * n + row;
            if (std::isnan(matrix[first]) || std::isnan(matrix[second])) {
                // One direction failed to pin: the symmetric cell is N/A too.
                matrix[first] = std::numeric_limits<float>::quiet_NaN();
                matrix[second] = std::numeric_limits<float>::quiet_NaN();
                continue;
            }
            const float average = (matrix[first] + matrix[second]) / 2.0f;
            matrix[first] = average;
            matrix[second] = average;
        }
    }

    store_result(std::move(matrix), n);
    return 0;
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_ddyibb_coherencylatency_NativeBench_matrix(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lock(g_result.mutex);
    const jsize length = static_cast<jsize>(g_result.matrix.size());
    jfloatArray array = env->NewFloatArray(length);
    if (array != nullptr && length > 0) {
        env->SetFloatArrayRegion(array, 0, length, g_result.matrix.data());
    }
    return array;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_ddyibb_coherencylatency_NativeBench_csv(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lock(g_result.mutex);
    if (!g_result.valid) {
        return nullptr;
    }
    return to_java_string(env, g_result.csv);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_ddyibb_coherencylatency_NativeBench_lastError(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lock(g_result.mutex);
    return to_java_string(env, g_result.error);
}

extern "C" JNIEXPORT void JNICALL
Java_com_ddyibb_coherencylatency_NativeBench_abort(JNIEnv*, jclass) {
    g_abort.store(true, std::memory_order_release);
}