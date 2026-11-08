// See vk_perf.h.
#include "vk_perf.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace mkwvk {

namespace {

std::mutex mu;
PerfStats machine;   // what the sampler wrote
float ring[kPerfHistory];
int64_t ring_at[kPerfHistory];
int ring_n = 0, ring_pos = 0;
int64_t last_frame_ns = 0;

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- NVML (NVIDIA), loaded on the sampler thread and kept for the process
struct NvmlUtil { unsigned gpu, memory; };
struct NvmlMem { unsigned long long total, free, used; };
struct Nvml {
    bool tried = false, ok = false;
    void* dev = nullptr;
    int (*util)(void*, NvmlUtil*) = nullptr;
    int (*temp)(void*, int, unsigned*) = nullptr;
    int (*mem)(void*, NvmlMem*) = nullptr;
    int (*power)(void*, unsigned*) = nullptr;
    int (*clock)(void*, int, unsigned*) = nullptr;
} nv;

template <class F> F sym(HMODULE m, const char* n) { return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, n))); }

void nvml_open(const char* hint) {
    nv.tried = true;
    HMODULE lib = LoadLibraryA("nvml.dll");
    if (!lib) lib = LoadLibraryA("C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    if (!lib) return;
    auto init = sym<int (*)()>(lib, "nvmlInit_v2");
    auto count = sym<int (*)(unsigned*)>(lib, "nvmlDeviceGetCount_v2");
    auto handle = sym<int (*)(unsigned, void**)>(lib, "nvmlDeviceGetHandleByIndex_v2");
    auto name = sym<int (*)(void*, char*, unsigned)>(lib, "nvmlDeviceGetName");
    nv.util = sym<int (*)(void*, NvmlUtil*)>(lib, "nvmlDeviceGetUtilizationRates");
    nv.temp = sym<int (*)(void*, int, unsigned*)>(lib, "nvmlDeviceGetTemperature");
    nv.mem = sym<int (*)(void*, NvmlMem*)>(lib, "nvmlDeviceGetMemoryInfo");
    nv.power = sym<int (*)(void*, unsigned*)>(lib, "nvmlDeviceGetPowerUsage");
    nv.clock = sym<int (*)(void*, int, unsigned*)>(lib, "nvmlDeviceGetClockInfo");
    unsigned n = 0;
    if (!init || !count || !handle || init() != 0 || count(&n) != 0 || !n) return;
    // (the device Vulkan renders on, by name: another vendor's GPU must not get this card's numbers)
    for (unsigned i = 0; i < n && !nv.dev; i++) {
        void* d = nullptr;
        char nm[96] = "";
        if (handle(i, &d) != 0 || !name || name(d, nm, sizeof nm) != 0 || !nm[0]) continue;
        if (hint[0] && (strstr(hint, nm) || strstr(nm, hint))) nv.dev = d;
    }
    nv.ok = nv.dev != nullptr;
}

void nvml_sample(PerfStats& s) {
    if (!nv.ok) return;
    NvmlUtil u;
    NvmlMem m;
    unsigned v;
    if (nv.util && nv.util(nv.dev, &u) == 0) s.gpu_pct = (float)u.gpu;
    if (nv.mem && nv.mem(nv.dev, &m) == 0) s.vram_used_mb = (float)(m.used >> 20), s.vram_total_mb = (float)(m.total >> 20);
    if (nv.temp && nv.temp(nv.dev, 0 /* NVML_TEMPERATURE_GPU */, &v) == 0) s.gpu_temp_c = (float)v;
    if (nv.power && nv.power(nv.dev, &v) == 0) s.gpu_power_w = v / 1000.0f;
    if (nv.clock && nv.clock(nv.dev, 0 /* NVML_CLOCK_GRAPHICS */, &v) == 0) s.gpu_clock_mhz = (float)v;
}

// ---- CPU and memory
uint64_t ft(const FILETIME& f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }

struct CpuClock {
    uint64_t idle = 0, total = 0, proc = 0, wall = 0;
    bool have = false;
} cpu_prev;

void cpu_sample(PerfStats& s) {
    FILETIME idle, kern, user, c, e, pk, pu;
    if (!GetSystemTimes(&idle, &kern, &user)) return;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &pk, &pu)) return;
    const uint64_t i = ft(idle), t = ft(kern) + ft(user), p = ft(pk) + ft(pu);   // (kernel time includes idle)
    if (cpu_prev.have && t > cpu_prev.total) {
        const double dt = (double)(t - cpu_prev.total);
        s.cpu_total_pct = (float)(100.0 * (1.0 - (double)(i - cpu_prev.idle) / dt));
        s.cpu_game_pct = (float)(100.0 * (double)(p - cpu_prev.proc) / dt);   // (of all logical processors, as the machine's)
    }
    cpu_prev = {i, t, p, 0, true};
}

struct ProcMem {   // PROCESS_MEMORY_COUNTERS, the fields used
    DWORD cb, PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage,
        QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage;
};

void mem_sample(PerfStats& s) {
    MEMORYSTATUSEX ms{sizeof ms};
    if (GlobalMemoryStatusEx(&ms)) {
        s.ram_total_mb = (float)(ms.ullTotalPhys >> 20);
        s.ram_used_mb = (float)((ms.ullTotalPhys - ms.ullAvailPhys) >> 20);
    }
    static auto get = sym<BOOL(WINAPI*)(HANDLE, ProcMem*, DWORD)>(GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo");
    ProcMem pm{};
    pm.cb = sizeof pm;
    if (get && get(GetCurrentProcess(), &pm, sizeof pm)) s.ram_game_mb = (float)(pm.WorkingSetSize >> 20);
}

void cpu_name(char* out, size_t n) {
    out[0] = 0;
    DWORD sz = (DWORD)n;
    RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString", RRF_RT_REG_SZ, nullptr,
                 out, &sz);
    // (the name is padded with spaces on some machines)
    for (char* p = out; *p == ' ';) memmove(p, p + 1, strlen(p));
}

// ---- the sampler thread
std::condition_variable cv;
bool want = false, alive = false;
char gpu_hint[96];
float vram_total_hint = -1;

void sampler() {
    PerfStats s{};
    snprintf(s.gpu_name, sizeof s.gpu_name, "%s", gpu_hint);
    cpu_name(s.cpu_name, sizeof s.cpu_name);
    if (!nv.tried) nvml_open(gpu_hint);
    cpu_prev = {};
    std::unique_lock lk(mu);
    while (want) {
        lk.unlock();
        s.cpu_total_pct = s.cpu_game_pct = -1;
        s.ram_used_mb = s.ram_total_mb = s.ram_game_mb = -1;
        s.gpu_pct = s.gpu_temp_c = s.gpu_power_w = s.gpu_clock_mhz = s.vram_used_mb = -1;
        s.vram_total_mb = vram_total_hint;
        cpu_sample(s);
        mem_sample(s);
        nvml_sample(s);
        lk.lock();
        std::memcpy(machine.cpu_name, s.cpu_name, sizeof s.cpu_name);
        std::memcpy(machine.gpu_name, s.gpu_name, sizeof s.gpu_name);
        machine.cpu_total_pct = s.cpu_total_pct, machine.cpu_game_pct = s.cpu_game_pct;
        machine.ram_used_mb = s.ram_used_mb, machine.ram_total_mb = s.ram_total_mb, machine.ram_game_mb = s.ram_game_mb;
        machine.gpu_pct = s.gpu_pct, machine.gpu_temp_c = s.gpu_temp_c, machine.gpu_power_w = s.gpu_power_w;
        machine.gpu_clock_mhz = s.gpu_clock_mhz, machine.vram_used_mb = s.vram_used_mb, machine.vram_total_mb = s.vram_total_mb;
        if (want) cv.wait_for(lk, std::chrono::milliseconds(500));
    }
    alive = false;   // (in the locked section of the last check: a new start sees it and starts a new thread)
}

} // namespace

void perf_sampling(bool on, const char* gpu_name, float vram_total_mb) {
    std::lock_guard lk(mu);
    if (on) {
        snprintf(gpu_hint, sizeof gpu_hint, "%s", gpu_name ? gpu_name : "");
        vram_total_hint = vram_total_mb;
        want = true;
        if (!alive) {
            machine = {};
            machine.cpu_total_pct = machine.cpu_game_pct = machine.ram_used_mb = machine.ram_total_mb = machine.ram_game_mb = -1;
            machine.gpu_pct = machine.gpu_temp_c = machine.gpu_power_w = machine.gpu_clock_mhz = machine.vram_used_mb =
                machine.vram_total_mb = -1;
            alive = true;
            std::thread(sampler).detach();
        }
    } else {
        want = false;
        cv.notify_all();
    }
}

void perf_note_frame() {
    const int64_t now = now_ns();
    std::lock_guard lk(mu);
    if (last_frame_ns && now - last_frame_ns < 2000000000LL) {
        ring[ring_pos] = (float)((now - last_frame_ns) / 1e6);
        ring_at[ring_pos] = now;
        ring_pos = (ring_pos + 1) % kPerfHistory;
        if (ring_n < kPerfHistory) ring_n++;
    }
    last_frame_ns = now;
}

void perf_snapshot(PerfStats* out) {
    const int64_t now = now_ns();
    std::lock_guard lk(mu);
    *out = machine;
    double sum = 0, mx = 0;
    int n = 0;
    for (int i = 0; i < ring_n; i++) {
        const int k = (ring_pos - 1 - i + kPerfHistory) % kPerfHistory;
        if (now - ring_at[k] > 1000000000LL) break;   // the last second
        sum += ring[k];
        if (ring[k] > mx) mx = ring[k];
        n++;
    }
    out->fps = n && sum > 0 ? (float)(n * 1000.0 / sum) : 0;
    out->frame_ms_avg = n ? (float)(sum / n) : 0;
    out->frame_ms_max = (float)mx;
    for (int i = 0; i < kPerfHistory; i++) {
        const int age = kPerfHistory - 1 - i;
        out->frame_ms[i] = age < ring_n ? ring[(ring_pos - 1 - age + 2 * kPerfHistory) % kPerfHistory] : 0;
    }
}

} // namespace mkwvk
