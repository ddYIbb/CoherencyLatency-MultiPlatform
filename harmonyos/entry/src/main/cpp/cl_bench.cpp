// NAPI measurement core for CoherencyLatency on HarmonyOS.
//
// The measurement core is the same code as the Android JNI bridge
// (bounce / owned workers, round-based thread pair reuse, min-over-runs
// statistic, symmetric averaging).  JNI callbacks are replaced by a small
// state machine that ArkTS polls, so the UI can keep redrawing while the
// matrix is being measured.

#include <napi/native_api.h>
#include <hilog/log.h>
#include <qos/qos.h>

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <ctime>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kTag = "CLBench";
constexpr unsigned int kDomain = 0x0C1B;

// ---------------------------------------------------------------------------
// Measurement core
// ---------------------------------------------------------------------------

// HarmonyOS on aarch64 has 128 byte cache lines on some SoCs and 64 on others;
// 128 keeps the two atomics of owned mode apart on either of them.
constexpr std::size_t kAtomicAlignment = 128;
constexpr std::size_t kWorkerStackBytes = 128 * 1024;

struct alignas(kAtomicAlignment) AtomicCell {
    std::atomic<std::uint64_t> value;

    explicit AtomicCell(std::uint64_t initial = 0) : value(initial) {}
};

// round_start[]/round_end[] are filled by the workers themselves: the start
// right after the worker observed round_go, the end right after its own
// measurement loop returned.  Timing the window from the main thread would add
// its scheduling delay to an edge of every sample.
struct StartGate {
    std::atomic<int> ready{0};
    std::atomic<int> round_go{0};
    std::atomic<int> round_done{0};
    std::atomic<bool> abort{false};
    // Both edges of every sample are stamped by the workers themselves.
    std::uint64_t round_start[2] = {0, 0};
    std::uint64_t round_end[2] = {0, 0};
    // Where each worker really ran during that round (kernel CPU number).
    int round_landing[2] = {-1, -1};
    // Core clock measured immediately before and immediately after the round.
    std::uint32_t round_mhz_before[2] = {0, 0};
    std::uint32_t round_mhz_after[2] = {0, 0};
    std::mutex mutex;
    std::condition_variable cv;
};

struct ThreadConfig {
    std::uint64_t start = 0;
    std::uint64_t iterations = 0;
    AtomicCell* target = nullptr;
    AtomicCell* read_target = nullptr;
    // The physical core key shown in the matrix (lowest CPU of its SMT group) ...
    int physical_core = 0;
    // ... and the affinity mask that makes the vendor scheduler land on it.
    int affinity_mask = 0;
    // >= 0 requests one exact logical CPU inside the mask (used for SMT
    // sibling pairs); -1 keeps the physical-core identity check.
    int exact_cpu = -1;
    // SMT fallback: use the group mask as-is and accept either sibling, then
    // verify that the two workers landed on different hardware threads.
    bool group_pin = false;
    // Recovery passes give both workers interactive QoS and a longer pin
    // window, which is the only lever left when the vendor scheduler parks a
    // prime core under load.
    bool high_qos = false;
    int patient_polls = 80;
    int rounds = 0;
    int slot = 0;
    StartGate* gate = nullptr;
    // Set by the worker after pinning and kept up to date by the placement
    // monitor.  A false value means this pair is reported as N/A instead of
    // pretending the number belongs to the requested CPU pair.
    std::atomic<bool> affinity_ok{false};
    std::atomic<bool> placement_ok{true};
    std::atomic<int> tid{0};
    // Where the worker actually ended up, plus why the pin failed.
    int landing_cpu = -1;
    int pin_attempts = 0;
    int pin_errno = 0;
    char pin_allowed[64] = {};
};

std::uint64_t now_ns() {
    struct timespec ts {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
               static_cast<std::uint64_t>(ts.tv_nsec);
    }
    return 0;
}

inline void spin_pause() {
#if defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

// Clock of the core this thread is running on, in cycles per microsecond
// (= MHz), measured with a dependent add chain.
//
// A cache-line hand-off is answered by the responding core, so what this tool
// reports is really "cycles spent divided by the clock of the core that
// answered".  A phone cluster ramps its clock over tens of milliseconds, so two
// otherwise identical rounds can differ by a factor of two just because one of
// them ran before the ramp finished.  Measuring the clock on both sides of
// every round makes that term observable instead of leaving it inside the
// result.
//
// One dependent add costs one cycle on every ARMv8 core in this class, so the
// duration of N adds is N cycles: MHz = N * 1000 / nanoseconds.  The chain is
// unrolled so the loop overhead stays a constant, and the best of several
// repetitions is kept because an interrupt can only make the window longer.
std::uint32_t measure_core_mhz() {
#if defined(__aarch64__) || defined(__arm__)
    constexpr int kAddsPerRep = 4096;
    constexpr int kReps = 5;
    constexpr int kUnroll = 16;
    std::uint64_t best = ~0ULL;
    for (int rep = 0; rep < kReps; ++rep) {
        std::uint64_t value = 1;
        const std::uint64_t start = now_ns();
        for (int index = 0; index < kAddsPerRep / kUnroll; ++index) {
            __asm__ __volatile__(
                "add %0, %0, #1\n\t"
                "add %0, %0, #1\n\t"
                "add %0, %0, #1\n\t"
                "add %0, %0, #1\n\t"
                "add %0, %0, #1\n\t"
                "add %0, %0, #1\n\t"
                "add %0, %0, #1\n\t"
                "add %0, %0, #1\n\t"
                "add %0, %0, #1\n\t"
                "add %0, %0, #1\n\t"
                "add %0, %0, #1\n\t"
                "add %0, %0, #1\n\t"
                "add %0, %0, #1\n\t"
                "add %0, %0, #1\n\t"
                "add %0, %0, #1\n\t"
                "add %0, %0, #1"
                : "+r"(value));
        }
        const std::uint64_t end = now_ns();
        if (end > start && end - start < best) {
            best = end - start;
        }
    }
    if (best == 0 || best == ~0ULL) {
        return 0;
    }
    return static_cast<std::uint32_t>(kAddsPerRep * 1000ULL / best);
#else
    return 0;
#endif
}

// Read one integer from a sysfs attribute.  Returns -1 when the file is not
// present or the process is not allowed to read it.
int read_sysfs_int(const char* path) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    char buffer[64] = {};
    const ssize_t length = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (length <= 0) {
        return -1;
    }
    buffer[length] = '\0';
    return static_cast<int>(std::strtol(buffer, nullptr, 10));
}

int read_sysfs_int_attr(int cpu, const char* name) {
    char path[160] = {};
    std::snprintf(path, sizeof(path),
                  "/sys/devices/system/cpu/cpu%d/%s", cpu, name);
    return read_sysfs_int(path);
}

// Diagnostic only: the vendor kernel normally denies cpufreq to app processes,
// but if it is readable it is an independent check on measure_core_mhz().
int read_cpufreq_mhz(int cpu) {
    char path[128] = {};
    std::snprintf(path, sizeof(path),
                  "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    char buffer[32] = {};
    const ssize_t length = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (length <= 0) {
        return -1;
    }
    buffer[length] = '\0';
    return static_cast<int>(std::strtol(buffer, nullptr, 10) / 1000);
}

// Which CPU is this thread really running on?
//
// Three kernel interfaces disagree on this platform, so keep all of them
// visible during diagnosis.  /proc/thread-self/stat names the calling thread
// without going through gettid(), which rules out a wrong-tid read.
int parse_processor_from_stat(const char* buffer) {
    // Field 2 (comm) may contain spaces and is wrapped in parentheses, so the
    // token count starts after the last ')'.
    const char* cursor = std::strrchr(buffer, ')');
    if (cursor == nullptr) {
        return -1;
    }
    ++cursor;
    // Fields 3..38 follow; the processor field is number 39.
    int field = 2;
    while (*cursor != '\0') {
        while (*cursor == ' ') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }
        ++field;
        if (field == 39) {
            return static_cast<int>(std::strtol(cursor, nullptr, 10));
        }
        while (*cursor != '\0' && *cursor != ' ') {
            ++cursor;
        }
    }
    return -1;
}

int read_processor_from_stat(const char* path) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    char buffer[1024] = {};
    const ssize_t length = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (length <= 0) {
        return -1;
    }
    buffer[length] = '\0';
    return parse_processor_from_stat(buffer);
}

// /proc/<tid>/stat is regenerated on every read, so the processor field can be
// sampled repeatedly through one descriptor.  Opening the file for every
// monitor tick cost two open/read/close sequences per millisecond, which is
// pure overhead in the placement watchdog and needless scheduler noise.
class ProcStatReader {
public:
    explicit ProcStatReader(const char* path) {
        std::snprintf(path_, sizeof(path_), "%s", path);
        fd_ = open(path_, O_RDONLY | O_CLOEXEC);
    }
    ~ProcStatReader() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }
    ProcStatReader(const ProcStatReader&) = delete;
    ProcStatReader& operator=(const ProcStatReader&) = delete;

    int processor() {
        if (fd_ < 0) {
            return -1;
        }
        char buffer[1024] = {};
        ssize_t length = pread(fd_, buffer, sizeof(buffer) - 1, 0);
        if (length < 0) {
            // A few proc files reject pread(); seek back and use read().
            if (lseek(fd_, 0, SEEK_SET) < 0) {
                return -1;
            }
            length = read(fd_, buffer, sizeof(buffer) - 1);
        }
        if (length <= 0) {
            return -1;
        }
        buffer[length] = '\0';
        return parse_processor_from_stat(buffer);
    }

private:
    char path_[96] = {};
    int fd_ = -1;
};

int probe_current_cpu_threadself() {
    thread_local ProcStatReader reader("/proc/thread-self/stat");
    return reader.processor();
}

int probe_current_cpu_tid() {
    char path[64] = {};
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat",
                  static_cast<int>(gettid()));
    return read_processor_from_stat(path);
}

int probe_task_cpu(int tid) {
    if (tid <= 0) {
        return -1;
    }
    char path[64] = {};
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
    return read_processor_from_stat(path);
}

int probe_current_cpu() {
    int value = probe_current_cpu_threadself();
    if (value >= 0) {
        return value;
    }
    value = probe_current_cpu_tid();
    if (value >= 0) {
        return value;
    }
    return sched_getcpu();
}

// ---------------------------------------------------------------------------
// Per-thread performance request (QoS + uclamp)
// ---------------------------------------------------------------------------
//
// The vendor scheduler treats the prime core specially.  Interactive QoS alone
// is enough for the discovery probe, but a measurement worker can still be
// moved to a mid core after the other worker of the pair starts.  Linux 5.3+
// exposes uclamp through sched_setattr(); raising util_min to 1024 tells the
// vendor scheduler that this thread needs the highest capacity and, unlike a
// plain affinity call, the clamp survives the scheduler's own rebalancing.
// The attributes are captured so a failed request is visible in the log rather
// than silently ignored.
constexpr std::uint64_t kSchedFlagResetOnFork = 0x01;
constexpr std::uint64_t kSchedFlagUtilClampMin = 0x20;
constexpr std::uint64_t kSchedFlagUtilClampMax = 0x40;
constexpr unsigned int kUclampMax = 1024;

// Layout of Linux struct sched_attr (SCHED_ATTR_SIZE_VER1).  The sysroot's
// linux/sched/types.h cannot be included next to <sched.h> because both define
// struct sched_param, so the kernel ABI is mirrored locally.
struct ClSchedAttr {
    std::uint32_t size;
    std::uint32_t sched_policy;
    std::uint64_t sched_flags;
    std::int32_t sched_nice;
    std::uint32_t sched_priority;
    std::uint64_t sched_runtime;
    std::uint64_t sched_deadline;
    std::uint64_t sched_period;
    std::uint32_t sched_util_min;
    std::uint32_t sched_util_max;
};
static_assert(sizeof(ClSchedAttr) == 56, "sched_attr ABI must stay 56 bytes");

struct ThreadPerfState {
    bool qos_captured = false;
    QoS_Level qos = QOS_DEFAULT;
    bool attr_captured = false;
    ClSchedAttr attr {};
    int qos_set_rc = -1;
    int qos_get_rc = -1;
    QoS_Level qos_effective = QOS_DEFAULT;
    int attr_rc = -1;
    int attr_errno = 0;
    bool clamp_verified = false;
    int nice_rc = -1;
    int nice_errno = 0;
    int nice_value = 0;
};

int sched_getattr_raw(pid_t tid, ClSchedAttr* attr) {
    return static_cast<int>(
        syscall(SYS_sched_getattr, tid, attr, static_cast<unsigned int>(sizeof(*attr)), 0U));
}

int sched_setattr_raw(pid_t tid, const ClSchedAttr* attr) {
    return static_cast<int>(syscall(SYS_sched_setattr, tid, attr, 0U));
}

void capture_thread_perf(ThreadPerfState* state) {
    if (state == nullptr) {
        return;
    }
    state->qos_get_rc = OH_QoS_GetThreadQoS(&state->qos);
    state->qos_captured = state->qos_get_rc == 0;
    state->attr.size = static_cast<unsigned int>(sizeof(state->attr));
    state->attr_rc = sched_getattr_raw(0, &state->attr);
    state->attr_captured = state->attr_rc == 0;
    if (!state->attr_captured) {
        state->attr_errno = errno;
    }
}

// Raise the calling worker to interactive QoS and request uclamp 1024/1024.
// The kernel may reject the clamp on older builds; the return value and the
// readback are logged so the next run can distinguish "not applied" from
// "applied but the scheduler still migrated the thread".
void apply_thread_perf(ThreadPerfState* state, int physical, bool high_qos) {
    if (state == nullptr) {
        return;
    }
    const QoS_Level requested_qos =
        high_qos ? QOS_USER_INTERACTIVE
                 : (state->qos_captured ? state->qos : QOS_DEFAULT);
    state->qos_set_rc = OH_QoS_SetThreadQoS(requested_qos);
    state->qos_get_rc =
        state->qos_captured ? OH_QoS_GetThreadQoS(&state->qos_effective) : -1;
    // Only the prime-core workers need the clamp.  Mid/low cores already pin
    // reliably at their inherited QoS; adding a performance request there just
    // invites the scheduler to pull them into the big cluster.
    if (state->attr_captured && high_qos) {
        ClSchedAttr request = state->attr;
        request.size = static_cast<unsigned int>(sizeof(request));
        request.sched_flags =
            (state->attr.sched_flags & kSchedFlagResetOnFork) |
            kSchedFlagUtilClampMin | kSchedFlagUtilClampMax;
        request.sched_util_min = kUclampMax;
        request.sched_util_max = kUclampMax;
        state->attr_rc = sched_setattr_raw(0, &request);
        state->attr_errno = state->attr_rc == 0 ? 0 : errno;
        ClSchedAttr readback {};
        readback.size = static_cast<unsigned int>(sizeof(readback));
        state->clamp_verified =
            sched_getattr_raw(0, &readback) == 0 &&
            readback.sched_util_min == kUclampMax &&
            readback.sched_util_max == kUclampMax;
    }
    // The vendor kernel accepts the uclamp flags but discards them (clamp=0),
    // so try the other documented performance request as well: nice -20.  If
    // the app lacks CAP_SYS_NICE this fails with EPERM and is harmless.
    if (high_qos) {
        errno = 0;
        state->nice_rc = setpriority(PRIO_PROCESS, 0, -20);
        state->nice_errno = state->nice_rc == 0 ? 0 : errno;
        errno = 0;
        state->nice_value = getpriority(PRIO_PROCESS, 0);
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                 "PERF physical=%{public}d high=%{public}d qosCap=%{public}d qosSet=%{public}d qosGet=%{public}d effective=%{public}d attrCap=%{public}d attrSet=%{public}d errno=%{public}d clamp=%{public}d niceRc=%{public}d niceErr=%{public}d nice=%{public}d",
                 physical, high_qos ? 1 : 0, state->qos_captured ? 1 : 0,
                 state->qos_set_rc, state->qos_get_rc,
                 static_cast<int>(state->qos_effective),
                 state->attr_captured ? 1 : 0, state->attr_rc,
                 state->attr_errno, state->clamp_verified ? 1 : 0,
                 state->nice_rc, state->nice_errno, state->nice_value);
}

void read_allowed_list(char* out, std::size_t size) {
    out[0] = '\0';
    const int fd = open("/proc/self/status", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    char buffer[4096] = {};
    const ssize_t length = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (length <= 0) {
        return;
    }
    const char* found = std::strstr(buffer, "Cpus_allowed_list:");
    if (found == nullptr) {
        return;
    }
    found += std::strlen("Cpus_allowed_list:");
    while (*found == '\t' || *found == ' ') {
        ++found;
    }
    std::size_t index = 0;
    while (*found != '\0' && *found != '\n' && index + 1 < size) {
        out[index++] = *found++;
    }
    out[index] = '\0';
}

int read_physical_core_key(int cpu);

// exact == false compares the physical-core key (the normal matrix); exact ==
// true compares the kernel CPU number itself (SMT sibling pairs).
bool landing_matches(int actual_cpu, int expected_cpu, bool exact) {
    if (actual_cpu < 0 || expected_cpu < 0) {
        return expected_cpu < 0;
    }
    if (exact) {
        return actual_cpu == expected_cpu;
    }
    return read_physical_core_key(actual_cpu) == expected_cpu;
}

struct PinResult {
    bool ok = false;
    int attempts = 0;
    int last_errno = 0;
    int mask_cpu = -1;
    int actual_cpu = -1;
    char allowed[64] = {};
};

// Is the calling thread restricted to exactly one CPU?
bool mask_is_single_cpu(int cpu_index) {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(gettid(), sizeof(set), &set) != 0) {
        return false;
    }
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        const bool present = CPU_ISSET(cpu, &set) != 0;
        if (present != (cpu == cpu_index)) {
            return false;
        }
    }
    return true;
}

// Some vendor kernels rewrite a one-CPU mask into the whole SMT group of that
// physical core (e.g. mask 7 -> CPUs 7+8).  That is still a valid pin for a
// physical-core matrix: every CPU in the readback set belongs to one core.
bool mask_binding_acceptable(int mask_cpu) {
    if (mask_is_single_cpu(mask_cpu)) {
        return true;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(gettid(), sizeof(set), &set) != 0) {
        return false;
    }
    int physical = -1;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &set) == 0) {
            continue;
        }
        const int key = read_physical_core_key(cpu);
        if (physical < 0) {
            physical = key;
        } else if (physical != key) {
            return false;
        }
    }
    return physical >= 0;
}

// Pin the calling thread to one affinity mask and verify where it really runs.
//
// On this vendor kernel sched_getcpu() and the raw getcpu syscall are
// virtualised: after binding to mask 4 they both report CPU 4 while the
// thread is actually executing on CPU 5.  The "processor" field of
// /proc/thread-self/stat is the only interface that agrees with measured
// cross-core latency, so it is the sole landing check here.  The mask readback
// is kept as a second, independent fact because some masks are silently
// rewritten to a whole cluster while sched_setaffinity() still returns 0.
//
bool abort_requested();

// expected_cpu < 0 means "accept whatever stable CPU this mask yields" and is
// used by the discovery pass; otherwise the thread must land on that exact
// kernel CPU or the pin is considered failed.
PinResult pin_current_thread(int mask_cpu, int expected_cpu, int patient_polls = 20,
                            bool exact = false) {
    PinResult result;
    result.mask_cpu = mask_cpu;
    cpu_set_t set;
    CPU_ZERO(&set);
    if (mask_cpu <= -100) {
        // Encoded SMT-group mask: -100-first_cpu covers first_cpu and its
        // sibling.  Some vendor kernels only honour a core when both of its
        // threads are in the affinity set.
        const int first = -mask_cpu - 100;
        if (first < 0 || first + 1 >= CPU_SETSIZE) {
            result.last_errno = EINVAL;
            return result;
        }
        CPU_SET(first, &set);
        CPU_SET(first + 1, &set);
    } else {
        if (mask_cpu < 0 || mask_cpu >= CPU_SETSIZE) {
            result.last_errno = EINVAL;
            return result;
        }
        CPU_SET(mask_cpu, &set);
    }

    for (int attempt = 0; attempt < 24; ++attempt) {
        if (abort_requested()) {
            return result;
        }
        ++result.attempts;
        result.last_errno = sched_setaffinity(gettid(), sizeof(set), &set) == 0
                                ? 0
                                : errno;
        for (int spin = 0; spin < 256; ++spin) {
            spin_pause();
        }
        result.actual_cpu = probe_current_cpu_threadself();
        if (result.last_errno == 0 && result.actual_cpu >= 0 &&
            landing_matches(result.actual_cpu, expected_cpu, exact)) {
            result.ok = true;
            return result;
        }
    }

    for (int attempt = 0; attempt < patient_polls; ++attempt) {
        if (abort_requested()) {
            return result;
        }
        ++result.attempts;
        result.last_errno = sched_setaffinity(gettid(), sizeof(set), &set) == 0
                                ? 0
                                : errno;
        struct timespec pause_time {};
        pause_time.tv_nsec = patient_polls > 20 ? 5000000 : 500000;  // 5 ms when patient
        nanosleep(&pause_time, nullptr);
        result.actual_cpu = probe_current_cpu_threadself();
        if (result.last_errno == 0 && result.actual_cpu >= 0 &&
            landing_matches(result.actual_cpu, expected_cpu, exact)) {
            result.ok = true;
            return result;
        }
    }

    read_allowed_list(result.allowed, sizeof(result.allowed));
    return result;
}

// Pin like the discovery probe: apply the mask once, then let the thread
// settle before sampling.  Repeatedly calling sched_setaffinity() in a tight
// loop makes the vendor scheduler keep rewriting the mask; one application
// followed by a short sampling window is what made discovery reliable.
PinResult pin_group_verified(int mask_cpu, int physical, int patient_polls,
                            int min_hits = 18) {
    PinResult result = pin_current_thread(mask_cpu, -1, patient_polls);
    if (!result.ok) {
        return result;
    }
    constexpr int kSamples = 24;
    int hits = 0;
    int last_cpu = result.actual_cpu;
    for (int sample = 0; sample < kSamples; ++sample) {
        for (int spin = 0; spin < 40000; ++spin) {
            spin_pause();
        }
        const int cpu = probe_current_cpu_threadself();
        if (cpu >= 0) {
            last_cpu = cpu;
            if (read_physical_core_key(cpu) == physical) {
                ++hits;
            }
        }
    }
    result.actual_cpu = last_cpu;
    result.ok = hits >= min_hits;
    return result;
}

// ---------------------------------------------------------------------------
// Pinnable core detection
// ---------------------------------------------------------------------------
//
// The vendor scheduler does not preserve CPU numbering: binding to mask 4 may
// land on kernel CPU 5, while sched_getcpu() keeps reporting 4.  A CPU is
// usable only when the real "processor" field stays on one value over several
// samples.  Discovery tries every mask in its own thread and records the
// mask -> actual-CPU mapping used later by Start().

struct CoreBinding {
    // Lowest CPU number of the SMT group; this is the stable physical identity.
    int physical = 0;
    // Preferred affinity CPU number that reaches this physical core.
    int mask = 0;
    // One real CPU observed there (CPU numbers within an SMT group are aliases).
    int sample_cpu = -1;
    // All masks that reached this physical core during discovery, best first.
    std::vector<int> masks;
};

std::vector<CoreBinding> g_core_bindings;

int mask_candidates_for_core(int physical, int* out, int capacity);
int mask_candidates_for_thread(int thread, int* out, int capacity);

constexpr int kTopologyCacheSize = 256;
constexpr unsigned long long kUnknownSiblings = ~0ULL;

// The SMT topology is fixed for the lifetime of the process.  Reading sysfs
// for every placement sample was the second-largest syscall source after the
// /proc monitor, so cache the per-CPU masks once.
struct SiblingMaskCache {
    std::atomic<unsigned long long> values[kTopologyCacheSize];
    SiblingMaskCache() {
        for (auto& value : values) {
            value.store(kUnknownSiblings, std::memory_order_relaxed);
        }
    }
};
SiblingMaskCache g_sibling_mask_cache;

unsigned long long read_thread_siblings_mask(int cpu) {
    if (cpu < 0 || cpu >= CPU_SETSIZE) {
        return 0;
    }
    if (cpu < kTopologyCacheSize) {
        const unsigned long long cached =
            g_sibling_mask_cache.values[cpu].load(std::memory_order_relaxed);
        if (cached != kUnknownSiblings) {
            return cached;
        }
    }
    char path[160] = {};
    std::snprintf(path, sizeof(path),
                  "/sys/devices/system/cpu/cpu%d/topology/thread_siblings", cpu);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    char buffer[64] = {};
    const ssize_t length = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (length <= 0) {
        return 0;
    }
    buffer[length] = '\0';
    const unsigned long long mask = std::strtoull(buffer, nullptr, 16);
    if (cpu < kTopologyCacheSize && mask != 0) {
        g_sibling_mask_cache.values[cpu].store(mask, std::memory_order_relaxed);
    }
    return mask;
}

// Lowest CPU of the thread_siblings mask is a stable physical-core key on
// this platform (0x60 for CPU5+CPU6 -> core 5, 0x18 for CPU3+CPU4 -> core 3).
int read_physical_core_key(int cpu) {
    const unsigned long long mask = read_thread_siblings_mask(cpu);
    if (mask == 0) {
        return cpu;
    }
    return __builtin_ctzll(mask);
}

// Fill out[] with the logical CPUs that share one physical core.  Returns the
// number of siblings (0 when sysfs is unavailable).
int read_smt_siblings(int physical, int* out, int capacity) {
    const unsigned long long mask = read_thread_siblings_mask(physical);
    int count = 0;
    for (int bit = 0; bit < 64 && count < capacity; ++bit) {
        if ((mask & (1ULL << bit)) != 0) {
            out[count++] = bit;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// Device topology
// ---------------------------------------------------------------------------
//
// CPU numbering is not portable: a four-cluster phone can put its prime core
// on CPU 0, another puts it on CPU 7, and physical_package_id is sometimes the
// cluster id and sometimes 0 for every cluster.  The matrix therefore keys
// "prime" off measured topology, never off a CPU number.

struct CpuTopology {
    int cpu_count = 0;
    int capacity[256] = {};
    int package_id[256] = {};
    int max_freq[256] = {};
    int physical[256] = {};
    int cluster_of[256] = {};
    // Average capacity (or max frequency when capacity is unavailable) of
    // every cluster, and the physical cores in the highest-performance one.
    std::vector<int> cluster_capacity;
    std::vector<int> prime_cores;
    int prime_cluster = -1;

    bool is_prime_physical(int physical_core) const {
        return std::find(prime_cores.begin(), prime_cores.end(),
                         physical_core) != prime_cores.end();
    }
    bool is_prime_thread(int cpu) const {
        return cpu >= 0 && cpu < cpu_count &&
               is_prime_physical(physical[cpu]);
    }
};

CpuTopology build_cpu_topology() {
    CpuTopology topo;
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    topo.cpu_count = (online > 0 && online < 256) ? static_cast<int>(online) : 1;
    bool package_known = topo.cpu_count > 0;
    bool capacity_known = topo.cpu_count > 0;
    bool freq_known = topo.cpu_count > 0;
    for (int cpu = 0; cpu < topo.cpu_count; ++cpu) {
        topo.capacity[cpu] = read_sysfs_int_attr(cpu, "cpu_capacity");
        char path[160] = {};
        std::snprintf(path, sizeof(path),
                      "/sys/devices/system/cpu/cpu%d/topology/physical_package_id",
                      cpu);
        topo.package_id[cpu] = read_sysfs_int(path);
        topo.max_freq[cpu] =
            read_sysfs_int_attr(cpu, "cpufreq/cpuinfo_max_freq");
        topo.physical[cpu] = read_physical_core_key(cpu);
        topo.cluster_of[cpu] = -1;
        if (topo.capacity[cpu] <= 0) {
            capacity_known = false;
        }
        if (topo.package_id[cpu] < 0) {
            package_known = false;
        }
        if (topo.max_freq[cpu] <= 0) {
            freq_known = false;
        }
    }
    const auto distinct_values = [](const int* values, int count) {
        std::vector<int> found;
        for (int index = 0; index < count; ++index) {
            if (values[index] >= 0 &&
                std::find(found.begin(), found.end(), values[index]) ==
                    found.end()) {
                found.push_back(values[index]);
            }
        }
        return static_cast<int>(found.size());
    };
    // Prefer physical_package_id: on HarmonyOS phones it is normally the
    // cluster id.  Some vendors report 0 for every CPU, in which case the
    // static cpu_capacity (or the maximum frequency as a weaker fallback)
    // gives the same grouping.
    const bool use_package =
        package_known && distinct_values(topo.package_id, topo.cpu_count) > 1;
    const bool use_capacity =
        !use_package && capacity_known &&
        distinct_values(topo.capacity, topo.cpu_count) > 1;
    const bool use_freq =
        !use_package && !use_capacity && freq_known &&
        distinct_values(topo.max_freq, topo.cpu_count) > 1;

    std::vector<int> keys;
    for (int cpu = 0; cpu < topo.cpu_count; ++cpu) {
        int key = 0;
        if (use_package) {
            key = topo.package_id[cpu];
        } else if (use_capacity) {
            key = topo.capacity[cpu];
        } else if (use_freq) {
            key = topo.max_freq[cpu];
        }
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
            keys.push_back(key);
        }
    }
    std::sort(keys.begin(), keys.end());
    topo.cluster_capacity.assign(keys.size(), 0);
    std::vector<int> cluster_counts(keys.size(), 0);
    for (int cpu = 0; cpu < topo.cpu_count; ++cpu) {
        int key = 0;
        if (use_package) {
            key = topo.package_id[cpu];
        } else if (use_capacity) {
            key = topo.capacity[cpu];
        } else if (use_freq) {
            key = topo.max_freq[cpu];
        }
        const int cluster = static_cast<int>(
            std::lower_bound(keys.begin(), keys.end(), key) - keys.begin());
        topo.cluster_of[cpu] = cluster;
        const int weight = topo.capacity[cpu] > 0 ? topo.capacity[cpu]
                           : topo.max_freq[cpu] > 0 ? topo.max_freq[cpu] / 1000
                                                     : 0;
        topo.cluster_capacity[cluster] += weight;
        ++cluster_counts[cluster];
    }
    int prime = -1;
    for (std::size_t cluster = 0; cluster < keys.size(); ++cluster) {
        if (cluster_counts[cluster] > 0) {
            topo.cluster_capacity[cluster] /= cluster_counts[cluster];
        }
        if (topo.cluster_capacity[cluster] > 0 &&
            (prime < 0 || topo.cluster_capacity[cluster] >
                              topo.cluster_capacity[prime])) {
            prime = static_cast<int>(cluster);
        }
    }
    // A single homogeneous cluster has no prime tier: do not give every core
    // the interactive-QoS treatment that is meant only for the fastest one.
    topo.prime_cluster = keys.size() > 1 ? prime : -1;
    // A package id is not always one performance tier: some vendors group
    // several clusters under the same package.  Pick the fastest physical
    // cores by their own capacity (or maximum frequency), not by every core in
    // the winning cluster, so a mid core that shares the package is not given
    // the prime-core treatment.
    int prime_weight = 0;
    for (int cpu = 0; cpu < topo.cpu_count; ++cpu) {
        const int weight = topo.capacity[cpu] > 0 ? topo.capacity[cpu]
                           : topo.max_freq[cpu] > 0 ? topo.max_freq[cpu] / 1000
                                                     : 0;
        if (weight > prime_weight) {
            prime_weight = weight;
        }
    }
    if (topo.prime_cluster >= 0 && prime_weight > 0) {
        for (int cpu = 0; cpu < topo.cpu_count; ++cpu) {
            const int weight = topo.capacity[cpu] > 0 ? topo.capacity[cpu]
                               : topo.max_freq[cpu] > 0 ? topo.max_freq[cpu] / 1000
                                                         : 0;
            if (weight == prime_weight &&
                std::find(topo.prime_cores.begin(), topo.prime_cores.end(),
                          topo.physical[cpu]) == topo.prime_cores.end()) {
                topo.prime_cores.push_back(topo.physical[cpu]);
            }
        }
        std::sort(topo.prime_cores.begin(), topo.prime_cores.end());
    }
    char prime_text[96] = {};
    int used = 0;
    for (std::size_t index = 0;
         index < topo.prime_cores.size() && used < 80; ++index) {
        used += std::snprintf(prime_text + used, sizeof(prime_text) - used,
                              index == 0 ? "%d" : ",%d",
                              topo.prime_cores[index]);
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                 "TOPO3 cpus=%{public}d clusters=%{public}d prime=%{public}d primeCores=%{public}s package=%{public}d capacity=%{public}d freq=%{public}d",
                 topo.cpu_count, static_cast<int>(keys.size()),
                 topo.prime_cluster, prime_text, use_package ? 1 : 0,
                 use_capacity ? 1 : 0, use_freq ? 1 : 0);
    return topo;
}

const CpuTopology& cpu_topology() {
    static const CpuTopology topology = build_cpu_topology();
    return topology;
}

// Affinity encoding for a whole SMT group.  A thread-mode cross-core worker
// only has to land somewhere on its own physical core, so give the scheduler
// both siblings instead of one logical CPU.  This is what makes a full 9x9
// thread matrix possible on SoCs whose scheduler refuses a single-thread mask.
int group_mask_for_cpu(int cpu) {
    int siblings[8] = {};
    const int count =
        read_smt_siblings(read_physical_core_key(cpu), siblings, 8);
    if (count >= 2) {
        return -100 - siblings[0];
    }
    return cpu;
}

// "3/4" for a physical core with two SMT threads, "0" for a single-thread core.
std::string physical_threads_string(int physical) {
    const unsigned long long mask = read_thread_siblings_mask(physical);
    if (mask == 0) {
        return std::to_string(physical);
    }
    std::string out;
    for (int cpu = 0; cpu < 64; ++cpu) {
        if ((mask & (1ULL << cpu)) != 0) {
            if (!out.empty()) {
                out.push_back('/');
            }
            out += std::to_string(cpu);
        }
    }
    return out.empty() ? std::to_string(physical) : out;
}

struct ProbeContext {
    int mask = 0;
    int physical = -1;
    int sample_cpu = -1;
    int hits = 0;
    bool stable = false;
    char detail[512] = {};
};

// Count which *physical core* a freshly pinned thread really runs on.  The
// vendor scheduler may alternate between the SMT thread numbers of one core;
// those numbers are aliases.  A majority vote on the physical key is much
// more reliable than requiring one exact CPU number.
struct ProbeVote {
    int physical = -1;
    int sample_cpu = -1;
    int hits = 0;
};

std::vector<int> probe_pinnable_cores() {
    g_core_bindings.clear();
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    const int count = (online > 0 && online < 256) ? static_cast<int>(online) : 1;
    {
        cpu_set_t allowed;
        CPU_ZERO(&allowed);
        sched_getaffinity(gettid(), sizeof(allowed), &allowed);
        char list[128] = {};
        int used = 0;
        for (int cpu = 0; cpu < CPU_SETSIZE && used < 120; ++cpu) {
            if (CPU_ISSET(cpu, &allowed) != 0) {
                used += std::snprintf(list + used, sizeof(list) - used,
                                      used == 0 ? "%d" : ",%d", cpu);
            }
        }
        OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                     "AFFINITY current=%{public}s", list);
        for (int cpu = 0; cpu < count; ++cpu) {
            char path[160] = {};
            std::snprintf(path, sizeof(path),
                          "/sys/devices/system/cpu/cpu%d/online", cpu);
            const bool is_online = access(path, F_OK) != 0 || access(path, R_OK) == 0;
            OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                         "TOPO cpu=%{public}d sib=0x%{public}llx key=%{public}d onlineFile=%{public}d",
                         cpu, read_thread_siblings_mask(cpu),
                         read_physical_core_key(cpu), is_online ? 1 : 0);
            char cluster_path[160] = {};
            char core_path[160] = {};
            char package_path[160] = {};
            std::snprintf(cluster_path, sizeof(cluster_path),
                          "/sys/devices/system/cpu/cpu%d/topology/cluster_id", cpu);
            std::snprintf(core_path, sizeof(core_path),
                          "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu);
            std::snprintf(package_path, sizeof(package_path),
                          "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpu);
            OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                         "TOPO2 cpu=%{public}d cluster=%{public}d core=%{public}d pkg=%{public}d cap=%{public}d maxK=%{public}d curM=%{public}d",
                         cpu, read_sysfs_int(cluster_path),
                         read_sysfs_int(core_path), read_sysfs_int(package_path),
                         read_sysfs_int_attr(cpu, "cpu_capacity"),
                         read_sysfs_int_attr(cpu, "cpufreq/cpuinfo_max_freq"),
                         read_cpufreq_mhz(cpu));
        }
    }
    std::vector<int> mask_codes;
    for (int mask = 0; mask < count; ++mask) {
        mask_codes.push_back(mask);
    }
    // Also try each SMT pair as one affinity set (encoded as -100-first_cpu).
    for (int cpu = 0; cpu < count; ++cpu) {
        const unsigned long long siblings = read_thread_siblings_mask(cpu);
        int first = -1;
        int bits = 0;
        for (int bit = 0; bit < 64; ++bit) {
            if ((siblings & (1ULL << bit)) != 0) {
                if (first < 0) {
                    first = bit;
                }
                ++bits;
            }
        }
        if (bits == 2 && first == cpu && cpu + 1 < count) {
            mask_codes.push_back(-100 - cpu);
        }
    }
    for (const int mask : mask_codes) {
        ProbeContext best;
        best.mask = mask;
        int probe_attempts = 0;
        for (int pass = 0; pass < 3; ++pass) {
        ++probe_attempts;
        ProbeContext context;
        context.mask = mask;
        pthread_t thread{};
        const int created = pthread_create(
            &thread, nullptr,
            [](void* arg) -> void* {
                auto* ctx = static_cast<ProbeContext*>(arg);
                OH_QoS_SetThreadQoS(QOS_USER_INTERACTIVE);
                const PinResult pin = pin_current_thread(ctx->mask, -1);
                if (!pin.ok) {
                    ctx->sample_cpu = pin.actual_cpu;
                    std::snprintf(ctx->detail, sizeof(ctx->detail),
                                  "pin_errno=%d attempts=%d allowed=%s",
                                  pin.last_errno, pin.attempts, pin.allowed);
                    return nullptr;
                }
                ProbeVote votes[8] = {};
                constexpr int kSamples = 24;
                int distinct = 0;
                for (int sample = 0; sample < kSamples; ++sample) {
                    for (int spin = 0; spin < 40000; ++spin) {
                        spin_pause();
                    }
                    const int cpu = probe_current_cpu_threadself();
                    if (cpu < 0) {
                        continue;
                    }
                    const int physical = read_physical_core_key(cpu);
                    int slot = -1;
                    for (int index = 0; index < distinct; ++index) {
                        if (votes[index].physical == physical) {
                            slot = index;
                            break;
                        }
                    }
                    if (slot < 0 && distinct < 8) {
                        slot = distinct++;
                        votes[slot].physical = physical;
                        votes[slot].sample_cpu = cpu;
                    }
                    if (slot >= 0) {
                        ++votes[slot].hits;
                    }
                }
                int best = -1;
                for (int index = 0; index < distinct; ++index) {
                    if (best < 0 || votes[index].hits > votes[best].hits) {
                        best = index;
                    }
                }
                ctx->physical = best >= 0 ? votes[best].physical : -1;
                ctx->sample_cpu = best >= 0 ? votes[best].sample_cpu : -1;
                ctx->hits = best >= 0 ? votes[best].hits : 0;
                // Require a clear majority: at most one quarter of the samples
                // may be elsewhere.  This tolerates one scheduler hiccup but
                // never mistakes a migrating thread for a pinned one.
                ctx->stable = ctx->physical >= 0 &&
                              votes[best].hits * 4 >= kSamples * 3;
                int used = 0;
                for (int index = 0; index < distinct && used < 480; ++index) {
                    used += std::snprintf(
                        ctx->detail + used, sizeof(ctx->detail) - used,
                        index == 0 ? "p%d:%d/%d" : ",p%d:%d/%d",
                        votes[index].physical, votes[index].sample_cpu,
                        votes[index].hits);
                }
                return nullptr;
            },
            &context);
        if (created == 0) {
            pthread_join(thread, nullptr);
        }
        if (context.stable && !best.stable) {
            best = context;
        } else if (context.stable == best.stable && context.hits > best.hits) {
            best = context;
        }
        if (best.stable && best.hits >= 20) {
            break;
        }
        }
        ProbeContext context = best;
        OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                     "PROBE mask=%{public}d physical=%{public}d sample=%{public}d votes=%{public}d/24 stable=%{public}d tries=%{public}d [%{public}s]",
                     context.mask, context.physical, context.sample_cpu,
                     context.hits, context.stable ? 1 : 0, probe_attempts,
                     context.detail);
        if (!context.stable || context.physical < 0) {
            continue;
        }
        bool found = false;
        for (CoreBinding& binding : g_core_bindings) {
            if (binding.physical == context.physical) {
                found = true;
                if (std::find(binding.masks.begin(), binding.masks.end(),
                              context.mask) == binding.masks.end()) {
                    binding.masks.push_back(context.mask);
                }
                // Prefer the mask that lands on the exact CPU it names;
                // otherwise keep the first mask that reaches this core.
                if (context.mask == context.sample_cpu &&
                    binding.mask != binding.sample_cpu) {
                    binding.mask = context.mask;
                    binding.sample_cpu = context.sample_cpu;
                }
                break;
            }
        }
        if (!found) {
            CoreBinding binding;
            binding.physical = context.physical;
            binding.mask = context.mask;
            binding.sample_cpu = context.sample_cpu;
            binding.masks.push_back(context.mask);
            g_core_bindings.push_back(binding);
        }
    }
    std::sort(g_core_bindings.begin(), g_core_bindings.end(),
              [](const CoreBinding& first, const CoreBinding& second) {
                  return first.physical < second.physical;
              });
    std::vector<int> cores;
    for (const CoreBinding& binding : g_core_bindings) {
        cores.push_back(binding.physical);
    }
    return cores;
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

private:
    struct StartData {
        WorkerFunction function = nullptr;
        ThreadConfig* config = nullptr;
    };

    static void run(StartData* data) {
        ThreadConfig* config = data->config;
        StartGate* gate = config->gate;
        // Capture the inherited QoS/sched_attr first, then raise the thread to
        // interactive QoS and uclamp 1024/1024.  The clamp is what keeps a
        // prime-core worker from being rebalanced to a mid core after its
        // partner starts; plain sched_setaffinity() is rewritten by the vendor
        // scheduler under that load.
        ThreadPerfState perf;
        capture_thread_perf(&perf);
        apply_thread_perf(&perf, config->physical_core, config->high_qos);
        config->tid.store(static_cast<int>(gettid()), std::memory_order_release);
        PinResult pin;
        int candidate_count = 1;
        if (config->exact_cpu >= 0) {
            // Thread matrix: try every mask that can reach this exact logical
            // CPU.  The landing check remains exact, so a sibling alias is
            // rejected and the next candidate is tried.
            int candidates[16] = {};
            candidate_count =
                mask_candidates_for_thread(config->exact_cpu, candidates, 16);
            if (candidate_count == 0) {
                candidates[candidate_count++] = config->affinity_mask;
            }
            for (int index = 0; index < candidate_count; ++index) {
                pin = pin_current_thread(candidates[index], config->exact_cpu,
                                         config->patient_polls, true);
                if (pin.ok) {
                    config->affinity_mask = candidates[index];
                    break;
                }
            }
        } else if (config->group_pin) {
            // Cross-core thread pair: any hardware thread of the requested
            // physical core is acceptable.  Try the single-CPU masks that
            // reached this core during discovery first, then the whole SMT
            // group.  The vendor scheduler sometimes rewrites the group mask
            // to a mid core under load, while one of the single masks still
            // lands on the requested core.
            int candidates[16] = {};
            int count = 0;
            int core_masks[8] = {};
            const int core_count =
                mask_candidates_for_core(config->physical_core, core_masks, 8);
            for (int index = 0; index < core_count && count < 16; ++index) {
                bool duplicate = false;
                for (int used = 0; used < count; ++used) {
                    if (candidates[used] == core_masks[index]) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    candidates[count++] = core_masks[index];
                }
            }
            if (count == 0) {
                candidates[count++] = config->affinity_mask;
            }
            candidate_count = count;
            for (int index = 0; index < count; ++index) {
                const int min_hits =
                    cpu_topology().is_prime_physical(config->physical_core) ? 12 : 18;
                pin = pin_group_verified(candidates[index], config->physical_core, 20,
                                         min_hits);
                if (pin.ok) {
                    config->affinity_mask = candidates[index];
                    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                                 "WORKER group pinned physical=%{public}d mask=%{public}d actual=%{public}d",
                                 config->physical_core, candidates[index],
                                 pin.actual_cpu);
                    break;
                }
            }
            if (!pin.ok) {
                OH_LOG_Print(LOG_APP, LOG_WARN, kDomain, kTag,
                             "WORKER group pin failed physical=%{public}d candidates=%{public}d last_mask=%{public}d actual=%{public}d",
                             config->physical_core, candidate_count,
                             config->affinity_mask, pin.actual_cpu);
            }
        } else {
            int candidates[8] = {};
            candidate_count =
                mask_candidates_for_core(config->physical_core, candidates, 8);
            if (candidate_count == 0) {
                candidates[candidate_count++] = config->affinity_mask;
            }
            for (int index = 0; index < candidate_count; ++index) {
                pin = pin_current_thread(candidates[index], config->physical_core,
                                         config->patient_polls);
                if (pin.ok) {
                    config->affinity_mask = candidates[index];
                    break;
                }
            }
        }
        config->affinity_ok.store(pin.ok, std::memory_order_release);
        config->landing_cpu = pin.actual_cpu;
        config->pin_attempts = pin.attempts;
        config->pin_errno = pin.last_errno;
        std::memcpy(config->pin_allowed, pin.allowed, sizeof(config->pin_allowed));
        if (!pin.ok) {
            OH_LOG_Print(LOG_APP, LOG_WARN, kDomain, kTag,
                         "WORKER pin failed physical=%{public}d candidates=%{public}d last_mask=%{public}d actual=%{public}d attempts=%{public}d errno=%{public}d allowed=%{public}s",
                         config->physical_core, candidate_count, config->affinity_mask,
                         pin.actual_cpu, pin.attempts, pin.last_errno, pin.allowed);
        }
        {
            char name[16] = {};
            std::snprintf(name, sizeof(name), "cl-%d", config->physical_core);
            pthread_setname_np(pthread_self(), name);
            std::lock_guard<std::mutex> lock(gate->mutex);
            gate->ready.fetch_add(1, std::memory_order_acq_rel);
        }
        gate->cv.notify_all();

        // Diagnostic: record the clock this worker runs at just before and just
        // after the measurement window.  A cluster ramps its clock over tens of
        // milliseconds, so a window that starts before the ramp finishes shows
        // up as a low MHz reading here.
        gate->round_mhz_before[config->slot] = measure_core_mhz();
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
            // The sample starts where the work starts and ends where the work
            // ends, so the main thread's own scheduling delay never enters the
            // measurement.
            gate->round_start[config->slot] = now_ns();
            data->function(config);
            gate->round_end[config->slot] = now_ns();
            const int landing = probe_current_cpu_threadself();
            gate->round_landing[config->slot] = landing;
            const bool placed =
                config->exact_cpu >= 0
                    ? landing == config->exact_cpu
                    : (landing >= 0 &&
                       read_physical_core_key(landing) == config->physical_core);
            if (!placed) {
                // One line per misplaced round floods the device log buffer and
                // evicts the per-pair summaries, so keep only the first one.
                if (config->placement_ok.exchange(false, std::memory_order_acq_rel)) {
                    OH_LOG_Print(LOG_APP, LOG_WARN, kDomain, kTag,
                                 "WORKER misplaced physical=%{public}d exact=%{public}d landing=%{public}d",
                                 config->physical_core, config->exact_cpu, landing);
                }
            }
            {
                std::lock_guard<std::mutex> lock(gate->mutex);
                gate->round_done.fetch_add(1, std::memory_order_acq_rel);
            }
            gate->cv.notify_all();
        }
        gate->round_mhz_after[config->slot] = measure_core_mhz();
    }

    static void* entry(void* parameter) {
        run(static_cast<StartData*>(parameter));
        return nullptr;
    }

    pthread_t thread_{};
    bool started_ = false;
    StartData start_data_{};
};

// ---------------------------------------------------------------------------
// Run state shared with ArkTS
// ---------------------------------------------------------------------------

enum RunState : int {
    kIdle = 0,
    kRunning = 1,
    kFinished = 2,
    kAborted = 3,
    kFailed = 4,
};

struct SharedState {
    std::mutex mutex;
    std::vector<float> matrix;
    std::vector<int> cpus;
    int n = 0;
    std::string csv;
    std::string error;
    // "first,second,value;..." for the same-core SMT thread pairs.
    std::string thread_pairs;

    std::atomic<int> state{kIdle};
    std::atomic<bool> abort{false};
    std::atomic<int> done{0};
    std::atomic<int> total{0};
    std::atomic<int> row{-1};
    std::atomic<int> column{-1};
    std::atomic<float> latency{0.0f};
    std::atomic<int> thread_done{0};
    std::atomic<int> thread_total{0};
};

SharedState g_shared;
std::thread g_runner;

bool abort_requested() {
    return g_shared.abort.load(std::memory_order_acquire);
}

void abort_pair(StartGate& gate, int rounds) {
    gate.abort.store(true, std::memory_order_release);
    gate.round_go.store(rounds, std::memory_order_release);
}

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

struct PairOutcome {
    float latency_ns = std::numeric_limits<float>::quiet_NaN();
    // Spread of the samples inside this call.  A call that sat in one clock /
    // scheduler state for its whole window has median == minimum; one that
    // saw both states has a median well above the minimum.
    float median_ns = std::numeric_limits<float>::quiet_NaN();
    bool affinity_ok = false;
    bool aborted = true;
};

// A same-core SMT sibling pair.  It is not part of the physical-core matrix;
// it is the "thread-to-thread" part of the Windows matrix that can still be
// measured when the vendor scheduler refuses to bind the secondary thread on
// its own.
struct ThreadPairResult {
    int first = 0;
    int second = 0;
    float latency_ns = std::numeric_limits<float>::quiet_NaN();
};

// Sampling budget.  The window is stamped by the workers at both edges, so its
// only error term is the loop entry/exit code, but the *clock* of both cores is
// part of the measurement: a hand-off is answered by the responding core, so a
// core running at half speed reports twice the latency.  A phone cluster ramps
// its clock over tens of milliseconds, which is why a 20k-exchange window (a
// few ms) used to land at whatever frequency happened to be active and made the
// same cell swing by 50%.  Each window is now ~20 ms, the first round of every
// pair is a discarded warm-up, and kSamplesPerDirection measured rounds are
// reduced with a minimum, so a disturbed round costs one round and never the
// result.
// A round is one measured window.  The total exchange count per direction is
// kept constant (rounds * iterations), but the window is cut finer: the
// minimum is taken over many more independent windows, so one window that
// happened to straddle a clock or scheduler state costs a smaller fraction of
// the samples instead of setting the whole cell.
constexpr std::uint64_t kCrossIterations = 500;
constexpr std::uint64_t kSiblingIterations = 500;
constexpr std::uint64_t kPrimeIterations = 500;
// Exchange cost is ~100 ns, so 800 windows of 500 exchanges still add up to
// only ~80 ms per direction - the run time is dominated by thread creation and
// the pin retries, not by this.  More independent windows therefore cost almost
// nothing and make the minimum far less of a lottery.
constexpr std::uint64_t kSamplesPerDirection = 800;
// A call that only kept a handful of rounds has no usable minimum: a single
// surviving round is whatever that one window happened to measure, and one
// such call produced a 1.6 us cell among 25 ns siblings.  Below this many kept
// rounds the call counts as failed and is retried instead of reported.
constexpr std::size_t kMinKeptRounds = 64;
// A prime-core hand-off has two discrete speeds on this SoC: a fast plateau at
// ~85-120 ns and a ~1.5x slower one at ~146-175 ns.  Both appear inside one
// window - the prime core changes clock state within the ~80 ms a call takes -
// so a call usually contains at least one fast round and its minimum is fast.
// Sometimes it does not: the whole window sits in the slow state, the minimum
// is slow too, and more rounds inside that window cannot help.  Only a fresh
// window can.  A prime value above this threshold therefore means "no fast
// round was found yet" and the pair is re-measured, keeping the smaller value.
constexpr float kSlowPlateauNs = 135.0f;
// The 135 ns number above is calibrated for Kirin 9000S.  A device with slower
// memory or a different clock plan can sit above it all the time, which would
// trigger 16 useless extra windows for every prime pair.  Scale the threshold
// from the measured same-core SMT latency instead: the slow prime state is
// roughly five times a sibling hand-off on every SoC seen so far.  The fixed
// value remains the fallback until the first sibling pair has been measured.
std::atomic<float> g_sibling_latency_ns{0.0f};
float slow_plateau_threshold() {
    const float sibling = g_sibling_latency_ns.load(std::memory_order_relaxed);
    return sibling > 0.0f ? std::max(80.0f, sibling * 5.0f) : kSlowPlateauNs;
}
// Fresh windows allowed for a prime pair before its value is accepted as is.
// Each one costs a pin plus ~80 ms of measurement, and only pairs that are
// actually stuck pay it.
constexpr int kPrimeExtraWindows = 16;
// Pause between those windows.  Re-measuring immediately only re-enters the
// same state, so the wait has to be long enough for the core to ramp.
constexpr int kSlowPlateauWaitMs = 0;
// Per-round logging floods the device log buffer and evicts the per-pair
// summary lines, so it stays off unless a specific run is being investigated.
constexpr bool kLogRounds = false;

// Latency estimate for one direction.
//
// Every disturbance on this platform - a preemption, a migration, a frequency
// dip - can only *lengthen* the measured window: the workers do the same
// number of exchanges no matter what, so noise is strictly additive.  The
// median is therefore biased upward whenever more than half the rounds are
// disturbed, which is exactly what the matrix showed (the same pair swinging
// between 130 ns and 500 ns across runs).  The minimum of several short,
// individually validated rounds is the standard estimator for additive noise:
// one clean round is enough to recover the true latency, and a round that
// happens to be clean is clean for the whole window, so the number does not
// drift with the noise level.  The original Windows tool used the same rule
// (minimum over ten rounds).
float minimum_of(std::vector<float>& values) {
    if (values.empty()) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    return *std::min_element(values.begin(), values.end());
}

// Low quantile of the round samples.  The minimum is the unbiased estimator
// for additive noise, but with hundreds of short rounds it is a single lucky
// round: one round that happened to start while the cache line was still
// exclusive can report 20% below the plateau and drag the whole cell with it.
// The 5th percentile sits on the plateau (the samples pile up just above the
// true value) yet still ignores the disturbed tail, so it keeps the accuracy
// of the minimum and loses its run-to-run lottery.
float low_quantile_of(const std::vector<float>& values, double quantile) {
    if (values.empty()) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    std::vector<float> copy = values;
    std::size_t index = static_cast<std::size_t>(quantile * copy.size());
    if (index >= copy.size()) {
        index = copy.size() - 1;
    }
    std::nth_element(copy.begin(), copy.begin() + index, copy.end());
    return copy[index];
}

float median_of(const std::vector<float>& values) {
    if (values.empty()) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    std::vector<float> copy = values;
    const std::size_t middle = copy.size() / 2;
    std::nth_element(copy.begin(), copy.begin() + middle, copy.end());
    float median = copy[middle];
    if ((copy.size() & 1U) == 0U) {
        const float lower =
            *std::max_element(copy.begin(), copy.begin() + middle);
        median = (median + lower) / 2.0f;
    }
    return median;
}

PairOutcome run_pair(int processor1, int processor2, int mask1, int mask2,
                     std::uint64_t iterations, std::uint64_t runs,
                     bool owned_mode, int exact1 = -1, int exact2 = -1,
                     bool high_qos = false, bool patient = false,
                     bool group_pin = false, bool reverse_start = false,
                     int desired1 = -1, int desired2 = -1,
                     bool force_pair_qos = false) {
    AtomicCell target1(owned_mode ? 1u : 0u);
    AtomicCell target2(0);

    StartGate gate;

    // An exact sibling pin is either accepted within the first few polls or
    // rewritten by the vendor scheduler; waiting the full patient window just
    // burns 0.8 s per candidate before falling back to the group mask.
    const bool exact_siblings =
        exact1 >= 0 && exact2 >= 0 && processor1 == processor2;
    const int pin_patient_polls = exact_siblings ? 8 : (patient ? 160 : 80);

    ThreadConfig config1;
    config1.start = owned_mode ? 3 : 1;
    config1.iterations = iterations;
    config1.target = &target1;
    config1.read_target = &target2;
    config1.physical_core = processor1;
    config1.affinity_mask = mask1;
    config1.exact_cpu = group_pin ? -1 : exact1;
    config1.group_pin = group_pin;
    // Only the prime-core worker gets interactive QoS; raising the other
    // worker too makes the vendor scheduler move it into the big cluster.
    // The CPU5/CPU6 SMT pair is special: at default QoS the scheduler
    // repeatedly migrates the CPU5 worker to CPU7/3/4 while the CPU6 worker
    // stays put, so the pair collapses to one hardware thread.  Keep both
    // p5 siblings at interactive QoS and see whether that pins CPU5.
    const bool prime_worker1 = cpu_topology().is_prime_physical(processor1);
    config1.high_qos = high_qos && (prime_worker1 || force_pair_qos);
    config1.patient_polls = pin_patient_polls;
    // One extra round per pair: it is pinned, measured and validated like every
    // other round but its value is thrown away.  It is what brings both cores
    // up to their working clock, so the sampled rounds are not the first thing
    // the pair does after being parked.
    config1.rounds = static_cast<int>(runs) + 1;
    config1.slot = 0;
    config1.gate = &gate;

    ThreadConfig config2;
    config2.start = 2;
    config2.iterations = iterations;
    config2.target = owned_mode ? &target2 : &target1;
    config2.read_target = &target1;
    config2.physical_core = processor2;
    config2.affinity_mask = mask2;
    config2.exact_cpu = group_pin ? -1 : exact2;
    config2.group_pin = group_pin;
    const bool prime_worker2 = cpu_topology().is_prime_physical(processor2);
    config2.high_qos = high_qos && (prime_worker2 || force_pair_qos);
    config2.patient_polls = pin_patient_polls;
    config2.rounds = static_cast<int>(runs) + 1;
    config2.slot = 1;
    config2.gate = &gate;

    WorkerThread thread1;
    WorkerThread thread2;
    const WorkerFunction worker = owned_mode ? owned_worker : bounce_worker;

    // While the workers are pinned and both report the right CPU, keep reading
    // the real processor field of their task entries for the whole round.  On
    // this platform sched_getcpu() lies about the placement, so the watchdog
    // and the workers both use /proc/<tid>/stat; any migration at any moment
    // invalidates the pair instead of silently changing the number.
    std::atomic<bool> monitor_stop{false};
    std::atomic<bool> monitor_ok{true};
    // Counted per round: a migration invalidates the round it happened in, not
    // the whole pair.
    std::atomic<int> monitor_violations{0};
    std::atomic<std::uint64_t> monitor_last_violation{0};
    std::thread monitor;
    const auto stop_monitor = [&monitor_stop, &monitor] {
        monitor_stop.store(true, std::memory_order_release);
        if (monitor.joinable()) {
            monitor.join();
        }
    };
    const auto wait_ready = [&](int count) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(2);
        while (gate.ready.load(std::memory_order_acquire) < count) {
            if (gate.abort.load(std::memory_order_acquire) || abort_requested() ||
                std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    };

    try {
        // The vendor scheduler only lets the highest physical core be pinned
        // when its worker is created before the other worker of the pair; when
        // it is created second it is immediately migrated to a mid core.
        // For SMT siblings the lower CPU must also occupy its sibling first,
        // otherwise both workers collapse onto the same hardware thread.
        const bool sequential = group_pin ||
                                (exact1 >= 0 && exact2 >= 0) ||
                                cpu_topology().is_prime_physical(processor1) ||
                                cpu_topology().is_prime_physical(processor2);
        if (sequential) {
            const int settle_ms =
                (exact1 >= 0 && exact2 >= 0 && processor1 == processor2) ? 20 : 3;
            bool thread1_first = processor1 >= processor2;
            if (exact1 >= 0 && exact2 >= 0) {
                thread1_first = exact1 <= exact2;
            }
            if (reverse_start && group_pin) {
                thread1_first = !thread1_first;
            }
            if (thread1_first) {
                thread1.start(worker, &config1);
                wait_ready(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(settle_ms));
                thread2.start(worker, &config2);
            } else {
                thread2.start(worker, &config2);
                wait_ready(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(settle_ms));
                thread1.start(worker, &config1);
            }
        } else if (processor1 >= processor2) {
            thread1.start(worker, &config1);
            thread2.start(worker, &config2);
        } else {
            thread2.start(worker, &config2);
            thread1.start(worker, &config1);
        }

        if (!wait_for_condition(gate, std::chrono::seconds(5),
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

        if (!config1.affinity_ok.load(std::memory_order_acquire) ||
            !config2.affinity_ok.load(std::memory_order_acquire)) {
            // Do not spend a full measurement on a pair that is already known
            // to be on the wrong CPU; the caller can retry it immediately.
            OH_LOG_Print(LOG_APP, LOG_WARN, kDomain, kTag,
                         "PAIRFAIL pin p1=%{public}d p2=%{public}d a1=%{public}d a2=%{public}d land1=%{public}d land2=%{public}d tries1=%{public}d tries2=%{public}d",
                         processor1, processor2,
                         config1.affinity_ok.load(std::memory_order_acquire),
                         config2.affinity_ok.load(std::memory_order_acquire),
                         config1.landing_cpu, config2.landing_cpu,
                         config1.pin_attempts, config2.pin_attempts);
            abort_pair(gate, config1.rounds);
            gate.cv.notify_all();
            stop_monitor();
            thread1.join();
            thread2.join();
            PairOutcome outcome;
            outcome.aborted = false;
            return outcome;
        }

        if (group_pin ||
            (processor1 == processor2 && exact1 >= 0 && exact2 >= 0)) {
            const int landed1 = config1.landing_cpu;
            const int landed2 = config2.landing_cpu;
            // With two exact logical CPUs requested the two workers must also
            // land in the requested order: a swapped landing would put the
            // other direction's number into this cell.
            const bool exact_pair = exact1 >= 0 && exact2 >= 0;
            const bool distinct =
                landed1 >= 0 && landed2 >= 0 && landed1 != landed2 &&
                (exact_pair ? (landed1 == exact1 && landed2 == exact2)
                            : (landing_matches(landed1, processor1, false) &&
                               landing_matches(landed2, processor2, false)));
            if (!distinct) {
                OH_LOG_Print(LOG_APP, LOG_WARN, kDomain, kTag,
                             "PAIRFAIL distinct p1=%{public}d p2=%{public}d land1=%{public}d land2=%{public}d a1=%{public}d a2=%{public}d",
                             processor1, processor2, landed1, landed2,
                             config1.affinity_ok.load(std::memory_order_acquire),
                             config2.affinity_ok.load(std::memory_order_acquire));
                abort_pair(gate, config1.rounds);
                gate.cv.notify_all();
                stop_monitor();
                thread1.join();
                thread2.join();
                PairOutcome outcome;
                outcome.aborted = false;
                return outcome;
            }
        }

        monitor = std::thread([&] {
            char path1[64] = {};
            char path2[64] = {};
            std::snprintf(path1, sizeof(path1), "/proc/self/task/%d/stat",
                          config1.tid.load(std::memory_order_acquire));
            std::snprintf(path2, sizeof(path2), "/proc/self/task/%d/stat",
                          config2.tid.load(std::memory_order_acquire));
            ProcStatReader stat1(path1);
            ProcStatReader stat2(path2);
            while (!monitor_stop.load(std::memory_order_acquire)) {
                const int cpu1 = stat1.processor();
                const bool ok1 =
                    cpu1 < 0 ||
                    landing_matches(cpu1, exact1 >= 0 ? exact1 : processor1,
                                    exact1 >= 0);
                if (!ok1) {
                    monitor_ok.store(false, std::memory_order_release);
                    monitor_violations.fetch_add(1, std::memory_order_relaxed);
                    monitor_last_violation.store(now_ns(), std::memory_order_relaxed);
                }
                const int cpu2 = stat2.processor();
                const bool ok2 =
                    cpu2 < 0 ||
                    landing_matches(cpu2, exact2 >= 0 ? exact2 : processor2,
                                    exact2 >= 0);
                if (!ok2) {
                    monitor_ok.store(false, std::memory_order_release);
                    monitor_violations.fetch_add(1, std::memory_order_relaxed);
                    monitor_last_violation.store(now_ns(), std::memory_order_relaxed);
                }
                if (group_pin && cpu1 >= 0 && cpu2 >= 0 && cpu1 == cpu2) {
                    monitor_ok.store(false, std::memory_order_release);
                    monitor_violations.fetch_add(1, std::memory_order_relaxed);
                    monitor_last_violation.store(now_ns(), std::memory_order_relaxed);
                }
                // Polling /proc twice per millisecond was itself a source of
                // disturbance: the watchdog is unpinned and its syscalls can
                // land on a worker's core.  A coarser poll still catches a
                // lasting migration, and a short one now only costs a single
                // round, which the minimum-of-rounds estimator discards.
                std::this_thread::sleep_for(std::chrono::microseconds(1000));
            }
        });

        // Every round is judged on its own: the pin is checked where the worker
        // really ran during that round, and a migration observed inside the
        // round drops that round instead of the whole pair.  The pair value is
        // the minimum of the surviving rounds, because every disturbance here
        // can only make a window longer.
        std::vector<float> samples;
        samples.reserve(static_cast<std::size_t>(config1.rounds));
        // Landing of every sample.  With a group mask the scheduler may put a
        // worker on either SMT sibling from round to round, and the two
        // siblings are not interchangeable: on this SoC they differ by up to
        // 50% for the same partner.  Mixing both into one minimum therefore
        // reports "the best sibling" instead of the labelled thread, which is
        // exactly what made some cells swing between runs.
        std::vector<std::pair<int, int>> landings;
        landings.reserve(static_cast<std::size_t>(config1.rounds));
        const auto expected_landing = [&](int slot) {
            if (slot == 0) {
                return exact1 >= 0 ? exact1 : processor1;
            }
            return exact2 >= 0 ? exact2 : processor2;
        };
        int discarded_rounds = 0;
        for (int round = 0; round < config1.rounds; ++round) {
            if (abort_requested()) {
                abort_pair(gate, config1.rounds);
                stop_monitor();
                thread1.join();
                thread2.join();
                return PairOutcome{};
            }

            target1.value.store(owned_mode ? 1u : 0u, std::memory_order_relaxed);
            target2.value.store(0u, std::memory_order_relaxed);
            gate.round_start[0] = 0;
            gate.round_start[1] = 0;
            gate.round_end[0] = 0;
            gate.round_end[1] = 0;
            gate.round_landing[0] = -1;
            gate.round_landing[1] = -1;
            const int violations_before =
                monitor_violations.load(std::memory_order_acquire);

            gate.round_go.store(round + 1, std::memory_order_release);
            // A round of "iterations" bounce exchanges takes roughly
            // 2 * iterations * latency.  On this SoC the small cores are several
            // times slower than the big ones, so a fixed 500 ms cap aborts a
            // legitimate run; 60 s is far above any realistic round yet still
            // catches a genuinely stuck worker.  abort() is polled every 20 ms,
            // so Stop stays responsive regardless of this timeout.
            const bool finished = wait_for_condition(
                gate, std::chrono::seconds(60), [&gate, round] {
                    return gate.round_done.load(std::memory_order_acquire) >=
                           2 * (round + 1);
                });
            if (!finished) {
                abort_pair(gate, config1.rounds);
                gate.cv.notify_all();
                stop_monitor();
                thread1.join();
                thread2.join();
                if (abort_requested()) {
                    return PairOutcome{};
                }
                throw std::runtime_error(
                    "benchmark thread did not finish a round");
            }
            // Both edges come from the workers, so the window contains the
            // work and nothing but the work: no main-thread wake-up delay at
            // either end.
            const std::uint64_t start =
                gate.round_start[0] > gate.round_start[1] ? gate.round_start[0]
                                                          : gate.round_start[1];
            const std::uint64_t end =
                gate.round_end[0] > gate.round_end[1] ? gate.round_end[0]
                                                      : gate.round_end[1];
            if (start == 0 || end <= start) {
                throw std::runtime_error("monotonic clock did not advance");
            }

            const bool placed0 =
                landing_matches(gate.round_landing[0], expected_landing(0),
                                exact1 >= 0);
            const bool placed1 =
                landing_matches(gate.round_landing[1], expected_landing(1),
                                exact2 >= 0);
            const bool distinct_landing =
                !group_pin ||
                (gate.round_landing[0] >= 0 && gate.round_landing[1] >= 0 &&
                 gate.round_landing[0] != gate.round_landing[1]);
            const bool clean_round =
                monitor_violations.load(std::memory_order_acquire) ==
                violations_before;
            // A group mask lets the scheduler put the worker on either SMT
            // sibling from round to round, and on this SoC the two siblings
            // can differ by up to 2x for the same partner.  A thread-level
            // cell names one specific logical CPU, so rounds that ran on the
            // other sibling are dropped here instead of being mixed into the
            // minimum.
            const bool wanted_landing =
                (desired1 < 0 || gate.round_landing[0] == desired1) &&
                (desired2 < 0 || gate.round_landing[1] == desired2);
            if (placed0 && placed1 && distinct_landing && clean_round &&
                wanted_landing) {
                if (round > 0) {
                    const float sample_ns = static_cast<float>(
                        static_cast<double>(end - start) /
                        static_cast<double>(iterations) / 2.0);
                    samples.push_back(sample_ns);
                    landings.emplace_back(gate.round_landing[0],
                                          gate.round_landing[1]);
                    if (kLogRounds &&
                        (cpu_topology().is_prime_physical(processor1) ||
                         cpu_topology().is_prime_physical(processor2))) {
                        OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                                     "PAIRSAMPLE p1=%{public}d p2=%{public}d round=%{public}d l1=%{public}d l2=%{public}d ns=%{public}.1f",
                                     processor1, processor2, round,
                                     gate.round_landing[0],
                                     gate.round_landing[1],
                                     static_cast<double>(sample_ns));
                    }
                }
            } else {
                ++discarded_rounds;
                if (kLogRounds) {
                    OH_LOG_Print(LOG_APP, LOG_DEBUG, kDomain, kTag,
                                 "ROUNDDROP p1=%{public}d p2=%{public}d round=%{public}d land1=%{public}d land2=%{public}d clean=%{public}d",
                                 processor1, processor2, round,
                                 gate.round_landing[0], gate.round_landing[1],
                                 clean_round);
                }
            }
        }

        stop_monitor();
        thread1.join();
        thread2.join();
        PairOutcome outcome;
        // Every disturbance on this platform only ever makes a window longer:
        // preemption, an interrupt, a migration or a clock ramp all add time to
        // a round and never subtract from it.  The minimum over many short
        // rounds is therefore the estimate of the undisturbed hand-off.  A low
        // quantile was tried instead and is strictly worse - it discards
        // exactly the cleanest rounds, so it can only report a larger value.
        float landing_ns = minimum_of(samples);
        // Informational only: the sibling combination the scheduler used most
        // often for this pair.
        int landing_l1 = -1;
        int landing_l2 = -1;
        int landing_n = static_cast<int>(samples.size());
        if (!landings.empty()) {
            std::map<std::pair<int, int>, int> counts;
            for (const std::pair<int, int>& landing : landings) {
                ++counts[landing];
            }
            int best = -1;
            for (const auto& entry : counts) {
                if (entry.second > best) {
                    best = entry.second;
                    landing_l1 = entry.first.first;
                    landing_l2 = entry.first.second;
                }
            }
        }
        outcome.latency_ns = landing_ns;
        outcome.median_ns = samples.empty() ? landing_ns : median_of(samples);
        if (processor1 == processor2 && !std::isnan(landing_ns)) {
            const float previous =
                g_sibling_latency_ns.load(std::memory_order_relaxed);
            if (previous <= 0.0f || landing_ns < previous) {
                g_sibling_latency_ns.store(landing_ns, std::memory_order_relaxed);
            }
        }
        const bool enough_samples = samples.size() >= kMinKeptRounds;
        outcome.affinity_ok =
            config1.affinity_ok.load(std::memory_order_acquire) &&
            config2.affinity_ok.load(std::memory_order_acquire) &&
            enough_samples;
        if (!enough_samples && !samples.empty()) {
            OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                         "PAIRTHIN p1=%{public}d p2=%{public}d kept=%{public}d need=%{public}d",
                         processor1, processor2,
                         static_cast<int>(samples.size()),
                         static_cast<int>(kMinKeptRounds));
        }
        OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                     "PAIRLAND p1=%{public}d p2=%{public}d l1=%{public}d l2=%{public}d",
                     processor1, processor2, gate.round_landing[0],
                     gate.round_landing[1]);
        if (!samples.empty()) {
            OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                         "PAIRSAMPLES p1=%{public}d p2=%{public}d l1=%{public}d l2=%{public}d ln=%{public}d n=%{public}d min=%{public}.1f p5=%{public}.1f med=%{public}.1f max=%{public}.1f",
                         processor1, processor2, landing_l1, landing_l2,
                         landing_n,
                         static_cast<int>(samples.size()),
                         static_cast<double>(minimum_of(samples)),
                         static_cast<double>(low_quantile_of(samples, 0.05)),
                         static_cast<double>(median_of(samples)),
                         static_cast<double>(*std::max_element(samples.begin(), samples.end())));
        }
        if (cpu_topology().is_prime_physical(processor1) ||
            cpu_topology().is_prime_physical(processor2)) {
            OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                         "PAIRCLOCK p1=%{public}d p2=%{public}d min=%{public}.1f med=%{public}.1f mhz1a=%{public}u mhz1b=%{public}u mhz2a=%{public}u mhz2b=%{public}u",
                         processor1, processor2,
                         static_cast<double>(minimum_of(samples)),
                         static_cast<double>(median_of(samples)),
                         gate.round_mhz_before[0], gate.round_mhz_after[0],
                         gate.round_mhz_before[1], gate.round_mhz_after[1]);
            std::map<std::pair<int, int>, std::vector<float>> combos;
            for (std::size_t index = 0; index < samples.size() && index < landings.size(); ++index) {
                combos[landings[index]].push_back(samples[index]);
            }
            for (const auto& combo : combos) {
                if (combo.second.size() < 16) {
                    continue;
                }
                std::vector<float> combo_samples = combo.second;
                OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                             "PAIRCOMBO p1=%{public}d p2=%{public}d l1=%{public}d l2=%{public}d n=%{public}d min=%{public}.1f med=%{public}.1f",
                             processor1, processor2, combo.first.first,
                             combo.first.second,
                             static_cast<int>(combo_samples.size()),
                             static_cast<double>(minimum_of(combo_samples)),
                             static_cast<double>(median_of(combo_samples)));
            }
        }
        if (discarded_rounds > 0) {
            OH_LOG_Print(LOG_APP, LOG_DEBUG, kDomain, kTag,
                         "PAIRROUNDS p1=%{public}d p2=%{public}d kept=%{public}d dropped=%{public}d",
                         processor1, processor2,
                         static_cast<int>(samples.size()), discarded_rounds);
        }
        if (!outcome.affinity_ok &&
            (cpu_topology().is_prime_physical(processor1) ||
             cpu_topology().is_prime_physical(processor2))) {
            OH_LOG_Print(LOG_APP, LOG_WARN, kDomain, kTag,
                         "PAIRFAIL result p1=%{public}d p2=%{public}d a1=%{public}d a2=%{public}d pl1=%{public}d pl2=%{public}d mon=%{public}d lat=%{public}f",
                         processor1, processor2,
                         config1.affinity_ok.load(std::memory_order_acquire),
                         config2.affinity_ok.load(std::memory_order_acquire),
                         config1.placement_ok.load(std::memory_order_acquire),
                         config2.placement_ok.load(std::memory_order_acquire),
                         monitor_ok.load(std::memory_order_acquire),
                         static_cast<double>(outcome.latency_ns));
        }
        outcome.aborted = false;
        return outcome;
    } catch (...) {
        abort_pair(gate, config1.rounds);
        gate.cv.notify_all();
        stop_monitor();
        thread1.join();
        thread2.join();
        throw;
    }
}

// Measure the two SMT threads of the same physical core.  The vendor scheduler
// refuses to bind CPU 4/6/8 on their own, but a group mask (both siblings in the
// affinity set) is accepted when the two workers start one after the other.
bool run_thread_pairs(std::uint64_t iterations, std::uint64_t runs,
                      bool owned_mode, std::vector<ThreadPairResult>* results) {
    g_shared.thread_done.store(0, std::memory_order_release);

    int total = 0;
    for (const CoreBinding& binding : g_core_bindings) {
        int siblings[8] = {};
        if (read_smt_siblings(binding.physical, siblings, 8) >= 2) {
            ++total;
        }
    }
    g_shared.thread_total.store(total, std::memory_order_release);

    for (const CoreBinding& binding : g_core_bindings) {
        if (abort_requested()) {
            return false;
        }
        int siblings[8] = {};
        const int count = read_smt_siblings(binding.physical, siblings, 8);
        if (count < 2) {
            continue;
        }
        const int first = siblings[0];
        const int second = siblings[1];
        const int group_mask = -100 - first;
        float best = std::numeric_limits<float>::quiet_NaN();
        const std::uint64_t pair_iters =
            std::min<std::uint64_t>(iterations, 200000);
        // Interactive QoS pulls small/mid cores up to the big cluster, which is
        // exactly the migration that breaks the CPU 3/4 sibling pair; the
        // matrix already measures those cores fine at default QoS.  Only the
        // reserved prime core needs the interactive boost to be reachable.
        const bool pair_qos =
            cpu_topology().is_prime_physical(binding.physical);
        for (int attempt = 0; attempt < 10 && std::isnan(best); ++attempt) {
            PairOutcome outcome =
                run_pair(binding.physical, binding.physical, group_mask, group_mask,
                         pair_iters, 1, owned_mode, first, second, pair_qos, true);
            if (outcome.aborted) {
                return false;
            }
            if (outcome.affinity_ok && !std::isnan(outcome.latency_ns)) {
                best = outcome.latency_ns;
            }
        }
        // If the exact-sibling pin is rejected, let both workers share the
        // group mask and keep the attempt only when they land on different
        // hardware threads of the same physical core.
        for (int attempt = 0; attempt < 10 && std::isnan(best); ++attempt) {
            PairOutcome outcome =
                run_pair(binding.physical, binding.physical, group_mask, group_mask,
                         pair_iters, 1, owned_mode, -1, -1, pair_qos, true, true,
                         false, -1, -1, true);
            if (outcome.aborted) {
                return false;
            }
            if (outcome.affinity_ok && !std::isnan(outcome.latency_ns)) {
                best = outcome.latency_ns;
            }
        }
        ThreadPairResult result;
        result.first = first;
        result.second = second;
        result.latency_ns = best;
        results->push_back(result);
        g_shared.thread_done.fetch_add(1, std::memory_order_acq_rel);
    }
    return true;
}

// ---------------------------------------------------------------------------
// CSV
// ---------------------------------------------------------------------------

std::string build_csv(const std::vector<float>& matrix,
                     const std::vector<int>& cpus,
                     const std::vector<ThreadPairResult>& thread_pairs,
                     bool thread_mode) {
    const int n = static_cast<int>(cpus.size());
    std::string output;
    output.reserve(static_cast<std::size_t>(n) * (n * 12 + 8) + 16);

    char buffer[64] = {};

    output.push_back(';');
    for (int column = 0; column < n; ++column) {
        if (column != 0) {
            output.push_back(';');
        }
        std::snprintf(buffer, sizeof(buffer), thread_mode ? "Thread %d" : "Core %d",
                      cpus[column]);
        output += buffer;
    }
    output.push_back('\n');

    for (int row = 0; row < n; ++row) {
        std::snprintf(buffer, sizeof(buffer), thread_mode ? "Thread %d;" : "Core %d;",
                      cpus[row]);
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

    if (!thread_mode && !thread_pairs.empty()) {
        output += "\nSame-core SMT thread pairs (ns/op)\n";
        output += "CPU A;CPU B;Latency\n";
        for (const ThreadPairResult& pair : thread_pairs) {
            std::snprintf(buffer, sizeof(buffer), "CPU %d;CPU %d;",
                          pair.first, pair.second);
            output += buffer;
            if (std::isnan(pair.latency_ns)) {
                output += "N/A";
            } else {
                std::snprintf(buffer, sizeof(buffer), "%.6g",
                              static_cast<double>(pair.latency_ns));
                output += buffer;
            }
            output.push_back('\n');
        }
    }

    return output;
}

// ---------------------------------------------------------------------------
// Matrix runner
// ---------------------------------------------------------------------------

void run_matrix(const std::vector<int>& cpus, const std::vector<int>& masks,
                std::uint64_t iterations, std::uint64_t runs, bool owned_mode,
                bool thread_mode) {
    const int n = static_cast<int>(cpus.size());
    std::vector<float> matrix(static_cast<std::size_t>(n) * n, 0.0f);
    const int total_pairs = n * n - n;
    g_shared.total.store(total_pairs, std::memory_order_relaxed);
    g_shared.done.store(0, std::memory_order_relaxed);

    int done_pairs = 0;
    bool aborted = false;
    // Measure every pair touching the parked core first, while discovery has
    // just kept it awake, instead of last.
    std::vector<std::pair<int, int>> pair_order;
    pair_order.reserve(static_cast<std::size_t>(total_pairs));
    // One run of a pair already measures the round trip in both directions
    // and divides the elapsed time by two.  The old loop measured the
    // transpose as a second, independent pair and then averaged the two;
    // that duplicated every p7 retry for no accuracy gain.  Measure the
    // upper triangle once and mirror it below.
    for (int row = 0; row < n; ++row) {
        for (int column = row + 1; column < n; ++column) {
            pair_order.emplace_back(row, column);
        }
    }
    const int parked_core = n - 1;
    const auto is_prime_index = [&](int index) {
        return thread_mode ? cpu_topology().is_prime_thread(cpus[index])
                           : cpu_topology().is_prime_physical(cpus[index]);
    };
    const auto parked_index = [&](int index) { return is_prime_index(index); };
    if (n >= 2) {
        std::stable_sort(pair_order.begin(), pair_order.end(),
                         [&parked_index](const std::pair<int, int>& first,
                                         const std::pair<int, int>& second) {
                             const bool first_parked =
                                 parked_index(first.first) || parked_index(first.second);
                             const bool second_parked =
                                 parked_index(second.first) || parked_index(second.second);
                             return first_parked && !second_parked;
                         });
    }
    try {
        for (const std::pair<int, int>& pair_index : pair_order) {
            const int row = pair_index.first;
            const int column = pair_index.second;
            if (abort_requested()) {
                aborted = true;
                break;
            }
            // Thread mode distinguishes two cases.  SMT siblings of one
            // physical core must be pinned exactly, because that is the only
            // way to measure a same-core thread pair.  For two different
            // physical cores only the core identity matters: use the whole SMT
            // group as the affinity set so the scheduler can use either
            // sibling.  This fills the full 9x9 thread matrix instead of
            // leaving every T3/T4/T5/T6/T7/T8 cross pair N/A.
            // The worker identity is the canonical physical-core key, not the
            // logical thread number: landing_matches() compares against that
            // key.  Passing T6 (=6) or T8 (=8) directly made every landing on
            // their sibling (5 or 7) look like a failed pin.
            const int physical_row = read_physical_core_key(cpus[row]);
            const int physical_column = read_physical_core_key(cpus[column]);
            const bool same_physical = physical_row == physical_column;
            std::uint64_t pair_runs = runs;
            std::uint64_t pair_iters = iterations;
            int exact1 = -1;
            int exact2 = -1;
            int pair_mask1 = masks[row];
            int pair_mask2 = masks[column];
            bool pair_qos = false;
            bool pair_patient = false;
            bool pair_group = false;
            const int group_mask1 = group_mask_for_cpu(cpus[row]);
            const int group_mask2 = group_mask_for_cpu(cpus[column]);
            if (thread_mode) {
                pair_patient = true;
                // Keep every benchmark worker at the highest application QoS.
                // The vendor scheduler migrates default-QoS threads off mid
                // cores under load, which is what made T4/T5 return N/A.
                pair_qos = true;
                if (same_physical) {
                    // Same-core SMT siblings: try to pin each logical thread
                    // exactly.  If the scheduler refuses one sibling, fall
                    // back to the group mask and require two distinct landings.
                    pair_group = false;
                    exact1 = cpus[row];
                    exact2 = cpus[column];
                    pair_mask1 = cpus[row];
                    pair_mask2 = cpus[column];
                    pair_runs = kSamplesPerDirection;
                    pair_iters = std::min<std::uint64_t>(
                        iterations,
                        cpu_topology().is_prime_physical(physical_row)
                            ? kPrimeIterations
                            : kSiblingIterations);
                } else {
                    // Cross-core thread pairs accept either sibling of the
                    // physical core.  Forcing one exact logical CPU was tried
                    // and rejected: the vendor scheduler rewrites one-CPU masks
                    // on the mid and prime cores, which dropped 60% of the
                    // rounds and doubled the run time without making the
                    // numbers more stable.  The landing is recorded per round
                    // instead, and the estimate only uses rounds that landed on
                    // the same pair of CPUs (see landing_estimate below).
                    pair_group = true;
                    pair_mask1 = group_mask1;
                    pair_mask2 = group_mask2;
                    pair_runs = kSamplesPerDirection;
                    pair_iters = std::min<std::uint64_t>(iterations,
                                                         kCrossIterations);
                }
            }
            // Both halves of the matrix are measured, in both directions.
            //
            // A bounce round trip hands one cache line back and forth, so it is
            // tempting to measure row->column once and copy the number into
            // column->row.  That is what this runner used to do, and it hides a
            // real asymmetry: the thread that issues the first exchange runs the
            // critical path while the other one is only reacting.  The original
            // Windows tool measured every ordered pair separately; this does the
            // same, so the upper-right and lower-left triangles are two
            // independent samples that agree to a few percent instead of being
            // one value printed twice.
            const bool prime_pair =
                cpu_topology().is_prime_physical(physical_row) ||
                cpu_topology().is_prime_physical(physical_column);
            const int retry_limit =
                thread_mode
                    ? (same_physical ? (prime_pair ? 40 : 20)
                                     : (prime_pair ? 40 : 8))
                    : (is_prime_index(row) ? 2 : 0);
            const auto measure_direction = [&](bool reverse) {
                const int proc_a = reverse ? physical_column : physical_row;
                const int proc_b = reverse ? physical_row : physical_column;
                const int mask_a = reverse ? pair_mask2 : pair_mask1;
                const int mask_b = reverse ? pair_mask1 : pair_mask2;
                const int exact_a = reverse ? exact2 : exact1;
                const int exact_b = reverse ? exact1 : exact2;
                const int group_a = reverse ? group_mask2 : group_mask1;
                const int group_b = reverse ? group_mask1 : group_mask2;
                // Thread-level cross-core cells name one logical CPU each; in
                // group mode the worker may land on either SMT sibling, so the
                // round filter keeps only the rounds that ran on the CPU this
                // cell is labelled with.
                const int desired_a = -1;
                const int desired_b = -1;
                PairOutcome result =
                    run_pair(proc_a, proc_b, mask_a, mask_b, pair_iters,
                             pair_runs, owned_mode, exact_a, exact_b, pair_qos,
                             pair_patient, pair_group, false, desired_a,
                             desired_b);
                // The prime core is reachable only in stochastic windows; a
                // handful of patient retries turns that into a reliable pin
                // without changing what is measured.
                for (int retry = 0; retry < retry_limit && !result.affinity_ok;
                     ++retry) {
                    // Spacing the attempts a little further apart gives the
                    // vendor scheduler time to reopen a window instead of
                    // hammering the same closed one 40 times in a row.
                    std::this_thread::sleep_for(std::chrono::milliseconds(
                        same_physical ? 20 : 10));
                    result = run_pair(proc_a, proc_b, mask_a, mask_b, pair_iters,
                                      pair_runs, owned_mode, exact_a, exact_b,
                                      pair_qos, pair_patient, pair_group,
                                      thread_mode && !same_physical &&
                                          (retry & 1) != 0,
                                      desired_a, desired_b,
                                      same_physical && retry >= 4);
                }
                // Last-resort exact pin for the prime core.  The group mask
                // above lets the vendor scheduler use either SMT sibling, but
                // on some launches the scheduler rewrites the whole group mask
                // to a mid core and the pair is lost.  Pinning the two named
                // logical CPUs directly is narrower and only attempted after
                // every group mask has failed, so it cannot slow down the
                // normal path.
                if (thread_mode && !same_physical && prime_pair &&
                    !result.affinity_ok) {
                    const int thread_a = reverse ? cpus[column] : cpus[row];
                    const int thread_b = reverse ? cpus[row] : cpus[column];
                    for (int retry = 0; retry < 24 && !result.affinity_ok;
                         ++retry) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(20));
                        result = run_pair(proc_a, proc_b, thread_a, thread_b,
                                          pair_iters, pair_runs, owned_mode,
                                          thread_a, thread_b, pair_qos, true,
                                          false, (retry & 1) != 0, -1, -1,
                                          true);
                    }
                    if (result.affinity_ok) {
                        OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                                     "PAIREXACT p1=%{public}d p2=%{public}d lat=%{public}.1f",
                                     proc_a, proc_b,
                                     static_cast<double>(result.latency_ns));
                    }
                }
                // Slow-plateau escape for the prime core.  A window that sits
                // in the slow state for its whole length reports the slow
                // value in every round, so its own minimum is slow too and
                // neither more rounds nor a lower quantile can recover the
                // fast one.  A second, independent window can, and keeping the
                // smaller of the two is the same estimator one level up: the
                // minimum over windows instead of over rounds.  It costs one
                // more pin per direction, not more exchanges per window.
                for (int window = 0;
                     window < kPrimeExtraWindows && result.affinity_ok &&
                     prime_pair && !same_physical &&
                     result.latency_ns > slow_plateau_threshold();
                     ++window) {
                    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                                 "PAIRSLOW p1=%{public}d p2=%{public}d win=%{public}d lat=%{public}.1f",
                                 proc_a, proc_b, window,
                                 static_cast<double>(result.latency_ns));
                    // The plateau lasts for a while on the device, not for one
                    // call: back-to-back windows land in the same state, so
                    // give the core time to leave it before asking again.
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(kSlowPlateauWaitMs));
                    const PairOutcome fresh =
                        run_pair(proc_a, proc_b, mask_a, mask_b, pair_iters,
                                 pair_runs, owned_mode, exact_a, exact_b,
                                 pair_qos, pair_patient, pair_group,
                                 (window & 1) != 0, desired_a, desired_b);
                    if (fresh.aborted) {
                        return fresh;
                    }
                    if (!fresh.affinity_ok || std::isnan(fresh.latency_ns)) {
                        continue;
                    }
                    if (fresh.latency_ns < result.latency_ns) {
                        OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                                     "PAIRWINDOW p1=%{public}d p2=%{public}d first=%{public}.1f second=%{public}.1f",
                                     proc_a, proc_b,
                                     static_cast<double>(result.latency_ns),
                                     static_cast<double>(fresh.latency_ns));
                        result = fresh;
                    }
                }
                // Exact-pin fallback: the vendor scheduler rewrites a one-CPU
                // mask on some threads (always the sibling of a prime core).
                // The group mask is still a valid measurement when the two
                // workers land on two different hardware threads, so use it
                // rather than reporting N/A - but log it, because such a cell
                // may describe the sibling instead of the labelled thread.
                if (thread_mode && same_physical && !result.affinity_ok) {
                    // First keep asking for the two exact logical CPUs, only
                    // with the group mask so the vendor scheduler has more room
                    // to accept it.  A swapped landing would measure the other
                    // direction, so it is not good enough yet.
                    for (int retry = 0; retry < 20 && !result.affinity_ok;
                         ++retry) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        result = run_pair(proc_a, proc_b, group_a, group_b,
                                          pair_iters, pair_runs, owned_mode,
                                          exact_a, exact_b, pair_qos, pair_patient,
                                          true, (retry & 1) != 0,
                                          -1, -1, true);
                    }
                    if (result.affinity_ok) {
                        OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                                     "PAIRFALLBACK p1=%{public}d p2=%{public}d exact=%{public}d/%{public}d lat=%{public}.1f",
                                     proc_a, proc_b, exact_a, exact_b,
                                     static_cast<double>(result.latency_ns));
                    } else {
                        // Last resort: accept either order and log it, so the
                        // cell is filled instead of reported as N/A.  Only the
                        // same physical core is involved, so the number still
                        // describes the labelled thread pair.
                        for (int retry = 0; retry < 20 && !result.affinity_ok;
                             ++retry) {
                            std::this_thread::sleep_for(
                                std::chrono::milliseconds(10));
                            result = run_pair(proc_a, proc_b, group_a, group_b,
                                              pair_iters, pair_runs, owned_mode,
                                              -1, -1, pair_qos, pair_patient,
                                              true, (retry & 1) != 0,
                                              -1, -1, true);
                        }
                        if (result.affinity_ok) {
                            OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                                         "PAIRSWAP p1=%{public}d p2=%{public}d exact=%{public}d/%{public}d lat=%{public}.1f",
                                         proc_a, proc_b, exact_a, exact_b,
                                         static_cast<double>(result.latency_ns));
                        }
                    }
                }
                // Replication on the prime core.  This SoC has two discrete
                // states for a prime-core hand-off: the fast plateau and a
                // ~1.9x slower one.  The slow state lasts for a whole window -
                // inside such a call the median equals the minimum - so more
                // rounds cannot escape it, only a fresh call can.  A second
                // call and the smaller of the two values removes most of the
                // run-to-run swing that the T7/T8 row and column showed.
                if (false && thread_mode && !same_physical && prime_pair &&
                    result.affinity_ok) {
                    // A successful prime call closes the pin window for a
                    // while, so the second sample needs the same patient
                    // retry treatment as the first one instead of a single
                    // attempt.  Take the smaller of the two independent
                    // calls: a call that sat in the slow plateau for its
                    // whole window cannot be rescued by more rounds, only by
                    // a fresh window.
                    int repl_attempts = 0;
                    for (int retry = 0; retry < 16; ++retry) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(10));
                        ++repl_attempts;
                        const PairOutcome repeat =
                            run_pair(proc_a, proc_b, mask_a, mask_b, pair_iters,
                                     pair_runs, owned_mode, exact_a, exact_b,
                                     pair_qos, pair_patient, pair_group,
                                     (retry & 1) != 0);
                        if (repeat.aborted) {
                            return repeat;
                        }
                        if (!repeat.affinity_ok) {
                            continue;
                        }
                        OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                                     "PAIRREPL p1=%{public}d p2=%{public}d tries=%{public}d first=%{public}.1f second=%{public}.1f",
                                     proc_a, proc_b, repl_attempts,
                                     static_cast<double>(result.latency_ns),
                                     static_cast<double>(repeat.latency_ns));
                        if (repeat.latency_ns < result.latency_ns) {
                            result = repeat;
                        }
                        break;
                    }
                }
                return result;
            };

            const PairOutcome forward = measure_direction(false);
            if (forward.aborted) {
                aborted = true;
                break;
            }
            const PairOutcome backward = measure_direction(true);
            if (backward.aborted) {
                aborted = true;
                break;
            }
            float upper = forward.affinity_ok
                              ? forward.latency_ns
                              : std::numeric_limits<float>::quiet_NaN();
            float lower = backward.affinity_ok
                              ? backward.latency_ns
                              : std::numeric_limits<float>::quiet_NaN();
            if (std::isnan(upper) != std::isnan(lower)) {
                // One direction could not be pinned at all.  The round trip it
                // would have measured is the same physical path, so reuse the
                // direction that did work instead of reporting a hole; the log
                // keeps the fact that this cell is not an independent sample.
                const float reused = std::isnan(upper) ? lower : upper;
                OH_LOG_Print(LOG_APP, LOG_DEBUG, kDomain, kTag,
                             "PAIRMIRROR p1=%{public}d p2=%{public}d forward=%{public}d",
                             physical_row, physical_column,
                             forward.affinity_ok ? 1 : 0);
                upper = reused;
                lower = reused;
            }
            const float value = (std::isnan(upper) || std::isnan(lower))
                                    ? std::numeric_limits<float>::quiet_NaN()
                                    : (upper + lower) / 2.0f;
            matrix[static_cast<std::size_t>(row) * n + column] = upper;
            matrix[static_cast<std::size_t>(column) * n + row] = lower;
            done_pairs += 2;

            g_shared.row.store(row, std::memory_order_relaxed);
            g_shared.column.store(column, std::memory_order_relaxed);
            g_shared.latency.store(value, std::memory_order_relaxed);
            g_shared.done.store(done_pairs, std::memory_order_release);
        }
    } catch (const std::exception& error) {
        {
            std::lock_guard<std::mutex> lock(g_shared.mutex);
            g_shared.error = error.what();
        }
        g_shared.state.store(kFailed, std::memory_order_release);
        return;
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(g_shared.mutex);
            g_shared.error = "unknown native error";
        }
        g_shared.state.store(kFailed, std::memory_order_release);
        return;
    }

    if (aborted) {
        g_shared.state.store(kAborted, std::memory_order_release);
        return;
    }

    // Recovery sweep: directions that could not be pinned are retried with a
    // patient pin and the highest QoS before they are reported as N/A.  Each
    // triangle is recovered on its own: a direction that already produced a
    // number is never overwritten by a noisier repeat.
    for (int row = 0; row < n && !aborted; ++row) {
        for (int column = row + 1; column < n; ++column) {
            const std::size_t first = static_cast<std::size_t>(row) * n + column;
            const std::size_t second = static_cast<std::size_t>(column) * n + row;
            if (!std::isnan(matrix[first]) && !std::isnan(matrix[second])) {
                continue;
            }
            const int physical_row = read_physical_core_key(cpus[row]);
            const int physical_column = read_physical_core_key(cpus[column]);
            const bool recovery_prime =
                cpu_topology().is_prime_physical(physical_row) ||
                cpu_topology().is_prime_physical(physical_column);
            const std::uint64_t recovery_runs =
                thread_mode ? kSamplesPerDirection : runs;
            // A shorter round gives the scheduler less time to migrate the
            // parked worker; the pair is retried, so losing a little sample
            // noise budget is a fair trade for completing the matrix.
            const std::uint64_t recovery_iters = std::min<std::uint64_t>(
                iterations, recovery_prime ? kPrimeIterations : kCrossIterations);
            // Thread mode recovers with the same group mask as the main pass;
            // an exact retry only repeats a mask the scheduler already rewrote.
            const bool recovery_group = thread_mode;
            const int recovery_mask_row =
                recovery_group ? group_mask_for_cpu(cpus[row]) : masks[row];
            const int recovery_mask_column =
                recovery_group ? group_mask_for_cpu(cpus[column]) : masks[column];
            const auto recover_direction = [&](bool reverse) -> float {
                const int proc_a = reverse ? physical_column : physical_row;
                const int proc_b = reverse ? physical_row : physical_column;
                const int mask_a =
                    reverse ? recovery_mask_column : recovery_mask_row;
                const int mask_b =
                    reverse ? recovery_mask_row : recovery_mask_column;
                for (int attempt = 0; attempt < 20; ++attempt) {
                    if (abort_requested()) {
                        return std::numeric_limits<float>::quiet_NaN();
                    }
                    // The scheduler per-pair landing windows are transient.  A
                    // pair that failed the main pass often succeeds a couple of
                    // seconds later, so cool down halfway through the attempts
                    // instead of declaring N/A straight away.
                    if (attempt == 10) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                    }
                    PairOutcome pair = run_pair(
                        proc_a, proc_b, mask_a, mask_b, recovery_iters,
                        recovery_runs, owned_mode, -1, -1, thread_mode, true,
                        recovery_group, (attempt & 1) != 0);
                    if (pair.affinity_ok && !std::isnan(pair.latency_ns)) {
                        return pair.latency_ns;
                    }
                }
                return std::numeric_limits<float>::quiet_NaN();
            };
            if (std::isnan(matrix[first])) {
                matrix[first] = recover_direction(false);
            }
            if (std::isnan(matrix[second])) {
                matrix[second] = recover_direction(true);
            }
            if (abort_requested()) {
                aborted = true;
            }
        }
    }
    if (aborted) {
        g_shared.state.store(kAborted, std::memory_order_release);
        return;
    }

    // Both triangles are independent samples, so nothing is averaged here: the
    // two numbers are allowed to differ by the few percent that a real
    // direction asymmetry plus sampling noise produces.  Only a direction that
    // could not be pinned at all borrows its transpose, so the matrix has no
    // hole; a measured direction always keeps its own value.
    for (int row = 0; row < n; ++row) {
        for (int column = row + 1; column < n; ++column) {
            const std::size_t first = static_cast<std::size_t>(row) * n + column;
            const std::size_t second = static_cast<std::size_t>(column) * n + row;
            const bool first_missing = std::isnan(matrix[first]);
            const bool second_missing = std::isnan(matrix[second]);
            if (first_missing && second_missing) {
                continue;
            }
            if (first_missing) {
                matrix[first] = matrix[second];
            } else if (second_missing) {
                matrix[second] = matrix[first];
            }
        }
    }

    // In core mode the physical-core matrix cannot show the two SMT threads of
    // one core; measure them separately so the app still reports
    // thread-to-thread latency.  Thread mode already has every logical thread
    // on both axes, so that extra pass is unnecessary.
    std::vector<ThreadPairResult> thread_pairs;
    std::string thread_text;
    if (!thread_mode) {
        if (!run_thread_pairs(iterations, runs, owned_mode, &thread_pairs)) {
            g_shared.state.store(kAborted, std::memory_order_release);
            return;
        }
        for (const ThreadPairResult& pair : thread_pairs) {
            char buffer[96] = {};
            if (std::isnan(pair.latency_ns)) {
                std::snprintf(buffer, sizeof(buffer), "%d,%d,N/A;", pair.first,
                              pair.second);
            } else {
                std::snprintf(buffer, sizeof(buffer), "%d,%d,%.6g;", pair.first,
                              pair.second, static_cast<double>(pair.latency_ns));
            }
            thread_text += buffer;
        }
    }

    std::string csv = build_csv(matrix, cpus, thread_pairs, thread_mode);
    {
        std::lock_guard<std::mutex> lock(g_shared.mutex);
        g_shared.matrix = std::move(matrix);
        g_shared.cpus = cpus;
        g_shared.n = n;
        g_shared.csv = std::move(csv);
        g_shared.thread_pairs = std::move(thread_text);
        g_shared.error.clear();
    }
    g_shared.state.store(kFinished, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// NAPI helpers
// ---------------------------------------------------------------------------

napi_value undefined_value(napi_env env) {
    napi_value result = nullptr;
    napi_get_undefined(env, &result);
    return result;
}

napi_value make_int(napi_env env, int value) {
    napi_value result = nullptr;
    napi_create_int32(env, value, &result);
    return result;
}

napi_value make_double(napi_env env, double value) {
    napi_value result = nullptr;
    napi_create_double(env, value, &result);
    return result;
}

napi_value make_bool(napi_env env, bool value) {
    napi_value result = nullptr;
    napi_get_boolean(env, value, &result);
    return result;
}

napi_value make_string(napi_env env, const std::string& value) {
    napi_value result = nullptr;
    napi_create_string_utf8(env, value.c_str(), value.size(), &result);
    return result;
}

bool read_int(napi_env env, napi_value value, int64_t* out) {
    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, value, &type) != napi_ok) {
        return false;
    }
    if (type != napi_number) {
        return false;
    }
    return napi_get_value_int64(env, value, out) == napi_ok;
}

// ---------------------------------------------------------------------------
// NAPI entry points
// ---------------------------------------------------------------------------

std::string architecture_name() {
#if defined(__aarch64__)
    return "arm64-v8a";
#elif defined(__arm__)
    return "armeabi-v7a";
#elif defined(__x86_64__)
    return "x86_64";
#else
    return "unknown";
#endif
}

napi_value GetVersion(napi_env env, napi_callback_info) {
    return make_string(env, "1.2.6-harmonyos");
}

// sysconf() in the OHOS sysroot has no _SC_LEVEL1_DCACHE_LINESIZE, so the
// coherency line size comes from the cpufreq-adjacent cache topology in sysfs,
// which is also what the kernel itself uses.
long read_cache_line_size() {
    const int fd = open("/sys/devices/system/cpu/cpu0/cache/index0/"
                        "coherency_line_size",
                        O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 128;
    }
    char buffer[32] = {};
    const ssize_t length = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (length <= 0) {
        return 128;
    }
    const long value = std::strtol(buffer, nullptr, 10);
    if (value < 32 || value > 4096) {
        return 128;
    }
    return value;
}

int mask_for_core(int physical) {
    for (const CoreBinding& binding : g_core_bindings) {
        if (binding.physical == physical) {
            return binding.mask;
        }
    }
    return -1;
}

// All masks known to reach a physical core, preferred first.  Under load some
// masks that worked during discovery are silently ignored by the vendor
// scheduler, so the worker retries every alias before reporting the pair N/A.
int mask_candidates_for_core(int physical, int* out, int capacity) {
    int count = 0;
    const auto add = [&](int mask) {
        if (count >= capacity) {
            return;
        }
        for (int index = 0; index < count; ++index) {
            if (out[index] == mask) {
                return;
            }
        }
        out[count++] = mask;
    };
    for (const CoreBinding& binding : g_core_bindings) {
        if (binding.physical != physical) {
            continue;
        }
        if (binding.mask >= 0) {
            add(binding.mask);
        }
        for (const int candidate : binding.masks) {
            add(candidate);
        }
        break;
    }
    // Discovery is stochastic: it may only have recorded the group mask for
    // the prime core.  The topology file is authoritative, so always offer the
    // single-thread aliases and the SMT group as fallbacks.
    int siblings[8] = {};
    const int sibling_count = read_smt_siblings(physical, siblings, 8);
    if (sibling_count >= 2) {
        add(-100 - siblings[0]);
    }
    add(physical);
    for (int index = 0; index < sibling_count; ++index) {
        add(siblings[index]);
    }
    return count;
}

// Affinity masks that can plausibly make a worker land on one exact logical
// thread.  The first candidate is the thread's own CPU number; the SMT group
// mask is accepted by kernels that ignore a single-thread mask, and the other
// aliases cover quietly rewritten masks.  Landing is still verified exactly.
int mask_candidates_for_thread(int thread, int* out, int capacity) {
    if (thread < 0 || thread >= CPU_SETSIZE || capacity <= 0) {
        return 0;
    }
    int count = 0;
    const auto add = [&](int mask) {
        if (mask < 0 || mask >= CPU_SETSIZE || count >= capacity) {
            if (mask > -100 || count >= capacity) {
                return;
            }
        }
        for (int index = 0; index < count; ++index) {
            if (out[index] == mask) {
                return;
            }
        }
        out[count++] = mask;
    };
    add(thread);
    int siblings[8] = {};
    const int physical = read_physical_core_key(thread);
    const int sibling_count = read_smt_siblings(physical, siblings, 8);
    if (sibling_count >= 2) {
        add(-100 - siblings[0]);
        for (int index = 0; index < sibling_count; ++index) {
            add(siblings[index]);
        }
    }
    int physical_masks[8] = {};
    const int physical_count = mask_candidates_for_core(physical, physical_masks, 8);
    for (int index = 0; index < physical_count; ++index) {
        add(physical_masks[index]);
    }
    return count;
}

// "physical:threads;..." for the probed core list, e.g.
// "0:0;1:1;2:2;3:3/4;5:5/6" -- 3/4 and 5/6 are SMT thread pairs.
std::string core_topology_string() {
    // Report the hardware topology, not just the cores that happened to be
    // pinnable during this launch.  The prime core is parked most of the time
    // and is often missing from the probe, but T7/T8 still exist as an SMT
    // pair and must be labelled as such in the matrix header.
    std::string out;
    std::vector<int> seen;
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    const int count = (online > 0 && online < 256) ? static_cast<int>(online) : 1;
    for (int cpu = 0; cpu < count; ++cpu) {
        const int physical = read_physical_core_key(cpu);
        if (std::find(seen.begin(), seen.end(), physical) != seen.end()) {
            continue;
        }
        seen.push_back(physical);
        if (!out.empty()) {
            out.push_back(';');
        }
        out += std::to_string(physical);
        out.push_back(':');
        out += physical_threads_string(physical);
    }
    return out;
}

// "logical cores|cache line|abi"
napi_value GetDeviceInfo(napi_env env, napi_callback_info) {
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores <= 0) {
        cores = 1;
    }
    const long line_size = read_cache_line_size();
    char buffer[128] = {};
    std::snprintf(buffer, sizeof(buffer), "%ld|%ld|%s", cores, line_size,
                  architecture_name().c_str());
    return make_string(env, buffer);
}

napi_value ProbeCores(napi_env env, napi_callback_info) {
    const std::vector<int> cpus = probe_pinnable_cores();
    napi_value array = nullptr;
    napi_create_array_with_length(env, cpus.size(), &array);
    for (std::size_t index = 0; index < cpus.size(); ++index) {
        napi_set_element(env, array, static_cast<uint32_t>(index),
                         make_int(env, cpus[index]));
    }
    return array;
}

napi_value ProbeThreads(napi_env env, napi_callback_info) {
    // Keep the physical bindings fresh: mask_candidates_for_thread uses them
    // as extra aliases when an exact thread mask is rewritten by the scheduler.
    if (g_core_bindings.empty()) {
        probe_pinnable_cores();
    }
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    if (count <= 0) {
        count = 1;
    }
    if (count > CPU_SETSIZE) {
        count = CPU_SETSIZE;
    }
    // Report every online logical thread, not only the ones that happened to
    // pass discovery this launch.  The prime core is often parked and missing
    // from a single probe, yet becomes reachable through the patient retry
    // path once the matrix starts; filtering here would silently shrink a
    // 9-thread phone to a 7-thread matrix.  Truly unpinnable rows can only
    // show N/A, which is preferable to hiding the hardware.
    napi_value array = nullptr;
    napi_create_array_with_length(env, static_cast<size_t>(count), &array);
    for (long cpu = 0; cpu < count; ++cpu) {
        napi_set_element(env, array, static_cast<uint32_t>(cpu),
                         make_int(env, static_cast<int>(cpu)));
    }
    return array;
}

napi_value CoreTopology(napi_env env, napi_callback_info) {
    return make_string(env, core_topology_string());
}

napi_value Start(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value argv[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 4) {
        return make_int(env, 2);
    }
    bool is_array = false;
    if (napi_is_array(env, argv[0], &is_array) != napi_ok || !is_array) {
        return make_int(env, 2);
    }
    uint32_t core_count = 0;
    if (napi_get_array_length(env, argv[0], &core_count) != napi_ok ||
        core_count < 2 || core_count > 256) {
        return make_int(env, 2);
    }
    std::vector<int> cpus;
    cpus.reserve(core_count);
    for (uint32_t index = 0; index < core_count; ++index) {
        napi_value element = nullptr;
        int64_t cpu = 0;
        if (napi_get_element(env, argv[0], index, &element) != napi_ok ||
            !read_int(env, element, &cpu) || cpu < 0 || cpu >= CPU_SETSIZE) {
            return make_int(env, 2);
        }
        for (const int existing : cpus) {
            if (existing == static_cast<int>(cpu)) {
                return make_int(env, 2);
            }
        }
        cpus.push_back(static_cast<int>(cpu));
    }
    bool thread_mode = false;
    if (argc >= 5) {
        napi_get_value_bool(env, argv[4], &thread_mode);
    }
    int64_t iterations = 0;
    int64_t runs = 0;
    bool owned = false;
    if (!read_int(env, argv[1], &iterations) || iterations < 1 ||
        iterations > 100000000LL) {
        return make_int(env, 3);
    }
    if (!read_int(env, argv[2], &runs) || runs < 1 || runs > 1000) {
        return make_int(env, 4);
    }
    napi_get_value_bool(env, argv[3], &owned);

    const int previous = g_shared.state.load(std::memory_order_acquire);
    if (previous == kRunning) {
        return make_int(env, 1);
    }
    if (g_runner.joinable()) {
        g_runner.join();
    }

    g_shared.abort.store(false, std::memory_order_release);
    g_shared.done.store(0, std::memory_order_relaxed);
    g_shared.row.store(-1, std::memory_order_relaxed);
    g_shared.column.store(-1, std::memory_order_relaxed);
    g_shared.latency.store(0.0f, std::memory_order_relaxed);
    g_shared.thread_done.store(0, std::memory_order_relaxed);
    g_shared.thread_total.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_shared.mutex);
        g_shared.matrix.clear();
        g_shared.cpus.clear();
        g_shared.csv.clear();
        g_shared.thread_pairs.clear();
        g_shared.error.clear();
        g_shared.n = 0;
    }
    g_shared.state.store(kRunning, std::memory_order_release);

    std::vector<int> core_list = cpus;
    std::vector<int> mask_list;
    mask_list.reserve(core_list.size());
    if (thread_mode) {
        // Refresh the alias table right before the run.  The prime core is
        // parked most of the time; a probe taken while the page was opening
        // can miss it even though it becomes reachable a second later.  A
        // fresh 0.1 s probe gives the retry path the masks it needs.
        probe_pinnable_cores();
        // A thread's own CPU number is only the first candidate; the worker
        // resolves group and alias masks itself and verifies the exact landing.
        mask_list.assign(core_list.begin(), core_list.end());
    } else {
        if (g_core_bindings.empty()) {
            probe_pinnable_cores();
        }
        for (const int core : core_list) {
            const int mask = mask_for_core(core);
            if (mask < 0) {
                break;
            }
            mask_list.push_back(mask);
        }
        if (mask_list.size() != core_list.size()) {
            // A stale probe (CPU hotplug, governor change): refresh once and retry.
            probe_pinnable_cores();
            mask_list.clear();
            for (const int core : core_list) {
                const int mask = mask_for_core(core);
                if (mask < 0) {
                    return make_int(env, 2);
                }
                mask_list.push_back(mask);
            }
        }
    }
    const std::uint64_t iters = static_cast<std::uint64_t>(iterations);
    const std::uint64_t round_count = static_cast<std::uint64_t>(runs);
    try {
        g_runner = std::thread([core_list, mask_list, iters, round_count, owned,
                                thread_mode] {
            run_matrix(core_list, mask_list, iters, round_count, owned, thread_mode);
        });
    } catch (...) {
        g_shared.state.store(kFailed, std::memory_order_release);
        return make_int(env, 6);
    }
    return make_int(env, 0);
}

napi_value Poll(napi_env env, napi_callback_info) {
    napi_value result = nullptr;
    napi_create_object(env, &result);

    const int state = g_shared.state.load(std::memory_order_acquire);
    const int done = g_shared.done.load(std::memory_order_acquire);
    const int total = g_shared.total.load(std::memory_order_relaxed);
    const int row = g_shared.row.load(std::memory_order_relaxed);
    const int column = g_shared.column.load(std::memory_order_relaxed);
    const float latency = g_shared.latency.load(std::memory_order_relaxed);
    const int thread_done = g_shared.thread_done.load(std::memory_order_acquire);
    const int thread_total = g_shared.thread_total.load(std::memory_order_relaxed);
    std::string error;
    // The error string only changes on failure; taking the mutex on every
    // 120 ms poll while a measurement is running is avoidable contention.
    if (state == kFailed) {
        std::lock_guard<std::mutex> lock(g_shared.mutex);
        error = g_shared.error;
    }

    napi_set_named_property(env, result, "state", make_int(env, state));
    napi_set_named_property(env, result, "done", make_int(env, done));
    napi_set_named_property(env, result, "total", make_int(env, total));
    napi_set_named_property(env, result, "row", make_int(env, row));
    napi_set_named_property(env, result, "column", make_int(env, column));
    napi_set_named_property(env, result, "latency", make_double(env, latency));
    napi_set_named_property(env, result, "threadDone", make_int(env, thread_done));
    napi_set_named_property(env, result, "threadTotal", make_int(env, thread_total));
    napi_set_named_property(env, result, "running", make_bool(env, state == kRunning));
    napi_set_named_property(env, result, "error", make_string(env, error));
    return result;
}

napi_value Matrix(napi_env env, napi_callback_info) {
    std::vector<float> copy;
    int n = 0;
    {
        std::lock_guard<std::mutex> lock(g_shared.mutex);
        copy = g_shared.matrix;
        n = g_shared.n;
    }
    if (n <= 0) {
        napi_value empty = nullptr;
        napi_create_array_with_length(env, 0, &empty);
        return empty;
    }
    napi_value array = nullptr;
    napi_create_array_with_length(env, copy.size(), &array);
    for (std::size_t index = 0; index < copy.size(); ++index) {
        napi_set_element(env, array, static_cast<uint32_t>(index),
                         make_double(env, static_cast<double>(copy[index])));
    }
    return array;
}

napi_value ThreadPairs(napi_env env, napi_callback_info) {
    std::string pairs;
    {
        std::lock_guard<std::mutex> lock(g_shared.mutex);
        pairs = g_shared.thread_pairs;
    }
    return make_string(env, pairs);
}

napi_value Csv(napi_env env, napi_callback_info) {
    std::string csv;
    {
        std::lock_guard<std::mutex> lock(g_shared.mutex);
        csv = g_shared.csv;
    }
    return make_string(env, csv);
}

napi_value Abort(napi_env env, napi_callback_info) {
    g_shared.abort.store(true, std::memory_order_release);
    return undefined_value(env);
}

napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"version", nullptr, GetVersion, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"deviceInfo", nullptr, GetDeviceInfo, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"start", nullptr, Start, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"poll", nullptr, Poll, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"matrix", nullptr, Matrix, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"csv", nullptr, Csv, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"threadPairs", nullptr, ThreadPairs, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"abort", nullptr, Abort, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"probeCores", nullptr, ProbeCores, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"probeThreads", nullptr, ProbeThreads, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"coreTopology", nullptr, CoreTopology, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, "libclbench loaded");
    return exports;
}

}  // namespace

static napi_module clbenchModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "clbench",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterClBenchModule(void) {
    napi_module_register(&clbenchModule);
}
