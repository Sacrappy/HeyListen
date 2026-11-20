// (heylisten) The performance summary (perf_report.h), ported from Longshot's #983: on only when the player chose "Send
// performance statistics" in the menu (opts.send_perf, hookshot_options.ini's send_performance_stats=, OFF by default) AND the
// build has an address to send to (nx2/addresses.ini's telemetry=, stamped into hookshot_version.txt, or HL_TELEMETRY_URL for a
// test; empty: nothing is measured or written). HOS_PERF_REPORT=1: measured and written the same way with no address, a
// summary for this machine alone that the sender (which still needs an address) never sends.
// Each frame shown is added here, on the thread that presents, to its map's
// totals: a bucket, a few sums and the renderer's counters read since the frame before. No allocation, no file and no lookup a
// frame: the map is asked of the host about once a second, and a summary's JSON is made and written on a thread of its own every
// `period` seconds (RX_VK_PERF_INTERVAL) and at exit into the game folder's perf_pending/, which host/hos_crash_dialog.c's
// hos_perf_send_run sends at the next start. Turning the row off deletes perf_pending/ and perf_sent/.
// The same row also lets the pipeline state records the player's game made that the shipped list lacks go (state_report.h): at the
// session's end they are queued into pipelines_pending/, which hos_pipes_send_run sends at the next start to /telemetry/pipelines.
// HL_TELEMETRY_PIPELINES=0 turns that part off alone.
// (v0.4.3) The frame's parts are measured now (perf_parts.h): the game's step, the renderer's work, the waits for the GPU and
// the pacing sleep fill parts_ms, and the hitch causes cover the driver's compiles, the descriptor work, the swapchain's block,
// the read-backs and the presenting thread's gaps (os_wait). What HeyListen's overlay still does not measure is sent as 0 (the
// draws left out are the replay's own count now), so the payload keeps Longshot's field tables, which the server validates.
// The same row, the same address and HL_TELEMETRY_ZONES (on unless 0) also record, per map, the shader program keys and
// pipeline state keys a map's draws first used (zone_report.h): each draw looks them up here once per key per map, and at the
// session's end the maps not sent before are queued into zones_pending/, which hos_zones_send_run sends at the next start to
// /telemetry/zones, so that a release can preload a zone's pipelines before the player enters it.
// (v0.4.3) The same row, the same address and HL_TELEMETRY_PROFILE (on unless 0) also take the detailed profile (profile_report.h,
// profile_run.cpp): where a map's CPU and GPU time goes. Its batches are written into profile_pending/ every five minutes and at the
// end, and hos_profile_send_run sends them at the next start to /telemetry/profile. HL_PROFILE_LOCAL=1 takes it with no address and
// with the row off, and writes the same JSON into profile_local/ (never sent).
#include "perf_report.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "../../host/hos_telemetry.h"
#include "perf_parts.h"
#include "profile_report.h"
#include "profile_run.h"
#include "report_info.h"
#include "state_report.h"
#include "upscaler_rows.h"
#include "vk_draw.h"
#include "vk_internal.h"
#include "vk_menu.h"
#include "vk_paths.h"
#include "vk_shader.h"
#include "upscaler_rows.h"
#include "vk_framegen.h"
#include "vk_fsr3.h"
#include "vk_temporal.h"
#include "zone_report.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <time.h>
#include <unistd.h>
#endif

namespace mkwvk {

bool shaders_preparing();   // (vk_main.cpp: the start-up screen's shader preparation)

namespace {

namespace pr = perf_report;
namespace fs = fps_stats;
namespace fsys = std::filesystem;

constexpr double kMapEvery = 1;      // (seconds between asking the host where Link is)
constexpr int kMaxPending = 48;      // (summaries waiting in perf_pending at most, about four hours of play: the newest go unwritten)

std::atomic<bool> perf_measure{false};   // (on: the row is on and the build has an address)
std::mutex mu;   // (the presenting thread's frames, against the exit's and a lost device's last summary)
std::map<std::string, pr::MapTotals> maps;
std::map<std::string, bool> seen_maps;   // (maps this session has sent: the server counts a session on a map once)
pr::MapTotals* current = nullptr;
std::string current_map;
bool map_switched = false;   // (the map changed since the frame before: its load is the frame's, perf_frame_shown)
pr::Machine machine;
bool machine_read = false;
fsys::path folder;
std::atomic<int64_t> events[pr::kEvents];
int64_t started_ns = 0, last_movie[5] = {}, last_frame_ns = 0, l_gpu = 0, l_rb = 0, l_cpu = 0, l_skips = 0, l_fresh = 0;
double period = pr::kPeriodDefault;
double since_map = 0, since_summary = 0, avg_ms = 0, last_ms = 0;
bool first = true;
int seq = 0;
int64_t l_failed = 0;
bool primed = false;

int64_t clock_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

#ifdef _WIN32
/// This thread's CPU time, ns: its cycles (QueryThreadCycleTime: the time it ran, kernel and user, not the wall clock)
/// against the performance counter, the scale calibrated once with a busy loop. The summary's os_wait is the wall time
/// this thread was not running, and game_cpu the part of the game's step it was
int64_t thread_cpu_ns() {
    static const double ns_per_cycle = [] {
        ULONG64 c0, c1;
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        if (!QueryThreadCycleTime(GetCurrentThread(), &c0)) return 0.0;
        QueryPerformanceCounter(&a);
        const LONGLONG until = a.QuadPart + f.QuadPart / 250;   // (a busy ~4 ms, once: the scale of the thread's cycles)
        volatile double sink = 0;
        for (;;) {
            sink += 1.0;
            QueryPerformanceCounter(&b);
            if (b.QuadPart >= until || sink < 0) break;
        }
        return QueryThreadCycleTime(GetCurrentThread(), &c1) && c1 > c0
                   ? perf_ticks_to_ns(b.QuadPart - a.QuadPart, f.QuadPart) / double(c1 - c0)
                   : 0.0;
    }();
    ULONG64 c = 0;
    if (ns_per_cycle <= 0 || !QueryThreadCycleTime(GetCurrentThread(), &c)) return 0;
    return (int64_t)((double)c * ns_per_cycle);
}
#else
int64_t thread_cpu_ns() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
#endif

/// The build's address, or HL_TELEMETRY_URL's: "" without one (host/hos_telemetry.h's rule)
std::string address_now() {
    std::string stamped;
    {
        std::ifstream f(game_dir_path() / "hookshot_version.txt", std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        char value[HOS_TELEMETRY_URL_MAX + 8] = "";
        hos_telemetry_stamp_value(text.c_str(), "telemetry", value, sizeof value);
        stamped = value;
    }
    char base[HOS_TELEMETRY_URL_MAX + 8];
    return hos_telemetry_base(getenv(HOS_TELEMETRY_ENV), getenv(HOS_TELEMETRY_TEST_HTTP_ENV), stamped.c_str(), base, sizeof base)
               ? base
               : "";
}

double ram_used_gb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pm{};
    pm.cb = sizeof pm;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm)) return pm.PrivateUsage / 1073741824.0;
#else
    FILE* f = fopen("/proc/self/statm", "r");
    long pages = 0, resident = 0;
    if (f) {
        if (fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
        fclose(f);
        return resident * (double)sysconf(_SC_PAGE_SIZE) / 1073741824.0;
    }
#endif
    return 0;
}

double vram_total_gb() {
    if (!dev.physical) return 0;
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(dev.physical, &mp);
    uint64_t most = 0;
    for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
        if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) most = std::max<uint64_t>(most, mp.memoryHeaps[i].size);
    return most / 1073741824.0;
}

const char* upscaler_name(int v) {
    static const char* const t[] = {"Game network", "DLSS", "DLAA", "FSR 3.1", "FSR 4", "FSR 4 AA", "Off"};
    static_assert(sizeof t / sizeof t[0] == upscaler_rows::kUpscalers, "a name for each upscaler");
    return v >= 0 && v < upscaler_rows::kUpscalers ? t[v] : "other";
}
const char* preset_name(int v) {
    static const char* const t[] = {"Custom", "Minimum", "Low", "Balanced", "High", "Ultra"};
    return v >= 0 && v < 6 ? t[v] : "Auto";
}
/// The Frame rate cap row's value (host/nn_vi.c: 0 60, 90, 120, 144, 165, 240), HOS_FPS_MAX over it
int fps_cap_now() {
    static const int env_max = getenv("HOS_FPS_MAX") ? std::max(1, atoi(getenv("HOS_FPS_MAX"))) : 0;
    if (env_max) return env_max;
    static const int caps[] = {60, 90, 120, 144, 165, 240};
    return opts.fps_cap >= 0 && opts.fps_cap < 6 ? caps[opts.fps_cap] : 60;
}

/// The machine, once: the GPU and its driver from Vulkan, the rest from report_info's words
void read_machine(const char* version) {
    machine.version = version ? version : "";
    {   // (hookshot_version.txt's source= against the log's: the release flag)
        std::ifstream f(game_dir_path() / "hookshot_version.txt", std::ios::binary);
        std::string stamp((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        machine.release = pr::release_flag(machine.version, stamp);
    }
    machine.version = machine.version.substr(0, machine.version.find(" ("));   // ("<version> (source <hash>)": the version)
    machine.gpu = dev.props.deviceName;
    machine.driver = report_info::driver_text();
    machine.vram_gb = vram_total_gb();
    const std::string cpu = report_info::cpu_text();   // ("<cpu> (<n> threads), <ram> GB")
    const size_t threads = cpu.find(" ("), gb = cpu.find(" GB");
    machine.cpu = cpu.substr(0, threads);
    if (gb != std::string::npos) {
        const size_t from = cpu.rfind(", ", gb);
        machine.ram_gb = atof(cpu.c_str() + (from == std::string::npos ? 0 : from + 2));
    }
    const std::string os = report_info::os_text();
    machine.os = os.substr(0, os.find(" ("));
    machine.kernel = pr::kernel_of(os);
    machine_read = true;
}

/// Frame generation, the DLSS model and the Render resolution as they are in effect now, not as the rows say: the rows keep a choice
/// the PC cannot run (an upscaler without its runtime, frame generation with the window offscreen or its images not made)
void read_effective_settings() {
    int mult = 1;
    bool automatic = false;
    machine.framegen = pr::framegen_name(framegen_engine(&mult, &automatic));
    machine.framegen_x = mult, machine.framegen_auto = automatic;
    const int up = fsr3_wanted() ? (int)upscaler_rows::FSR3 : temporal_mode();   // (what runs: the Upscaler row's own, vk_menu.cpp)
    static const char* const env = getenv("RX_VK_DLSS_PRESET");   // (vk_dlss.cpp wanted_preset's rule; the VRAM-aware first-run
    //  default is the row's own here, vk_menu.cpp menu_first_run_model: the row value already carries what runs)
    machine.dlss_model = pr::dlss_model_name(
        (int)upscaler_rows::dlss_preset(opts.dlss_model, env && *env ? env : nullptr), upscaler_rows::is_dlss(up));
    machine.render_scale = upscaler_rows::full_scale(up) ? 100 : upscaler_rows::percent(upscaler_rows::snap(up, opts.resolution));
}

void read_settings() {
    machine.upscaler = upscaler_name(upscaler_rows::mode_for(opts.dlss, opts.resolution));   // (DLAA and FSR 4 AA: the native choice with DLSS or FSR 4)
    machine.preset = preset_name(opts.preset);
    machine.fps_cap = fps_cap_now();
    read_effective_settings();
}

constexpr int kMaxZonesPending = 4;   // (batches waiting in zones_pending at most: host/hos_perf_send.h HOS_ZONES_MAX_PENDING)
namespace zr = zone_report;
std::mutex zones_mu;                    // (the draw threads' per-draw lookups, against the map switches and the queueing)
std::vector<std::unique_ptr<zr::Zone>> zone_slots;   // (the session's maps, in first-visit order; only appended to)
std::map<std::string, uint32_t> zone_ids;
uint32_t zone_now = (uint32_t)zr::kMaxZones;   // (the map the draws record into: its place in zone_slots, or kMaxZones: none)
int64_t zone_arrival_end = 0;          // (clock_ns: uses before it are the area's arrival)
std::string zone_map;                  // (the name the recorder is on)
int zones_runs = 0;
bool zones_told_full = false;
std::atomic<bool> zones_measure{false};   // (on: the row is on, an address and HL_TELEMETRY_ZONES not 0: read once a second)
int64_t zone_uses = 0;                 // (the draw threads' lookups, for the log)

/// The host named a map (mu held): the recorder's own switch of maps, not held back by the summary's map cap. The
/// visits count and the arrival window are per entry, the first-use indexes restart with each visit
void zone_enter(const std::string& name) {
    zone_map = name;
    if (!zones_measure.load(std::memory_order_relaxed)) return;
    std::lock_guard lk(zones_mu);
    auto it = zone_ids.find(name);
    if (it == zone_ids.end()) {
        if (zone_slots.size() >= zr::kMaxZones) {
            zone_now = (uint32_t)zr::kMaxZones;
            if (!zones_told_full)
                zones_told_full = true,
                logf("vk: zones: %zu maps this session already: %s is not recorded (a batch holds %zu maps)", zone_slots.size(),
                     name.c_str(), zr::kMaxZones);
            return;
        }
        zone_slots.push_back(std::make_unique<zr::Zone>());
        zone_slots.back()->name = name;
        zone_slots.back()->visits = 1;   // (this entry is its first: a map toured once is 1)
        it = zone_ids.emplace(name, (uint32_t)zone_slots.size() - 1).first;
    } else {
        zone_slots[it->second]->visits++;
    }
    zr::Zone& z = *zone_slots[it->second];
    z.next_first_program = z.next_first_state = 0;
    zone_arrival_end = clock_ns() + (int64_t)(zr::kArrivalSeconds * 1e9);
    zone_now = it->second;
}

/// (vk_draw.cpp, once a draw's program / pipeline key) the zone usage: a set lookup per draw, on the path that looked
/// the key up, and the whole first-use note (the order, the arrival flag) only once per key per map
void zone_note(bool program, uint64_t key) {
    if (!zones_measure.load(std::memory_order_relaxed)) return;
    std::lock_guard lk(zones_mu);
    if (zone_now == (uint32_t)zr::kMaxZones) return;
    zr::Zone& z = *zone_slots[zone_now];
    zr::note_use(program ? z.programs : z.states, key, clock_ns() < zone_arrival_end,
                 program ? z.next_first_program : z.next_first_state, program ? zr::kMaxPrograms : zr::kMaxStates);
    zone_uses++;
}

// ---- the detailed profile (profile_report.h): its gate, its map and frame feed, and its batches ----

namespace pf = profile_report;
void at_exit();
bool write_summary(const fsys::path& dir, const std::string& name, const std::string& json, const char* sub, int limit);
constexpr int kMaxProfilePending = 8;   // (batches waiting in profile_pending at most: host/hos_perf_send.h HOS_PROFILE_MAX_PENDING)
constexpr int kMaxProfileLocal = 1000;  // (files in profile_local at most: a tester's own copies)
double since_profile = 0;               // (seconds of play since the last batch)
int profile_seq = 0;
std::string profile_map_name;           // (the map the profile is on)
std::string profile_host_map;           // (the map the host named last, where_now: the profile's own, not held to the summary's 16)
bool profile_told = false;

/// Whether the profile may be sent: the row, an address and not HL_TELEMETRY_PROFILE=0 (the row decides first)
bool profile_gate_telemetry() {
    return !*pf::why_not(opts.send_perf == 1, telemetry_available(), getenv(HOS_TELEMETRY_PROFILE_ENV));
}
/// HL_PROFILE_LOCAL=1: the profile is taken (and copied into profile_local/) with no address and with the row off
bool profile_local() {
    static const bool v = pf::local_on(getenv("HL_PROFILE_LOCAL"));
    return v;
}

/// The map's name as the summaries name it: the host's, else the start-up screen or the title
std::string map_name_of(const char* host_map) {
    return host_map[0] ? host_map : shaders_preparing() ? pr::kPreparingShaders : pr::kTitleAndIntro;
}

/// The profile's batch (mu held): the maps that have anything since the last one, in as many files as the cap needs, into
/// profile_pending (the row, an address) and profile_local (HL_PROFILE_LOCAL). `final`: at the end, written now (the process ends
/// next); else on a thread of its own
void profile_queue(bool final) {
    if (!profile_running()) return;
    since_profile = 0;
    const bool tele = profile_gate_telemetry(), local = profile_local();
    std::vector<pf::MapProfile> taken = profile_take(final);
    if (taken.empty() || (!tele && !local)) return;
    if (!machine_read) {
        char map[120] = "";
        int64_t movie[5] = {};
        read_machine(host->perf_info ? host->perf_info(map, sizeof map, movie) : nullptr);
    }
    pf::Meta meta;
    meta.version = machine.version, meta.release = machine.release, meta.build_id = profile_build_id();
    meta.vendor_id = dev.props.vendorID, meta.driver = machine.driver;
    meta.gpu = machine.gpu, meta.cpu = machine.cpu, meta.os = machine.os;
    std::vector<std::string> parts;
    for (const pf::MapProfile& m : taken) parts.push_back(pf::map_json(m));
    size_t over = 0;
    const std::vector<std::vector<size_t>> groups = pf::batch_groups(parts, pf::kMaxBodyBytes, &over);
    const fsys::path dir = game_dir_path();
    const long long at = (long long)std::time(nullptr);
    size_t files = 0;
    for (const std::vector<size_t>& g : groups) {
        std::vector<std::string> sub;
        for (size_t i : g) sub.push_back(parts[i]);
        std::string json = pf::payload_json(meta, sub);
        char name[48];
        snprintf(name, sizeof name, "%lld-%d", at, profile_seq++);
        const auto write = [dir, n = std::string(name), j = std::move(json), tele, local] {
            if (tele && !write_summary(dir, n, j, "profile_pending", kMaxProfilePending))
                logf("vk: profile: a batch not queued: %d are waiting to be sent already, or the file could not be written", kMaxProfilePending);
            if (local) write_summary(dir, n, j, "profile_local", kMaxProfileLocal);
        };
        if (final) write();
        else std::thread(write).detach();
        files++;
    }
    logf("vk: profile: %zu maps in %zu file%s (%s%s%s)%s; %s", taken.size(), files, files == 1 ? "" : "s", tele ? "profile_pending" : "",
         tele && local ? ", " : "", local ? "profile_local" : "", over ? ", a map over the cap left out" : "", profile_stats().c_str());
}

/// Starts or stops the profile as the gates say (mu held; every 64th frame and at each row change)
void profile_follow() {
    const bool want = profile_gate_telemetry() || profile_local(), was = profile_running();
    if (want == was) return;
    if (!want) {
        profile_stop();
        profile_map_name.clear();
        return;
    }
    folder = game_dir_path();
    if (!machine_read) {
        char map[120] = "";
        int64_t movie[5] = {};
        read_machine(host->perf_info ? host->perf_info(map, sizeof map, movie) : nullptr);
    }
    static bool registered = false;
    if (!registered) registered = true, std::atexit(at_exit);
    since_profile = 0;
    if (!profile_start(pf::sample_hz(getenv("HL_PROFILE_HZ"))) && !profile_told)
        profile_told = true, logf("vk: profile: neither the CPU sampler nor the GPU timestamps can run here");
}

/// The frame feed (mu held): the map as the host names it, about once a second, and each frame's time and verdict
void profile_feed(const std::string& map_name, double ms, bool hitch) {
    if (!profile_running()) return;
    static thread_local bool registered = false;
    if (!registered) registered = true, profile_register_thread(pf::kRolePresent);   // (the presenting thread: its name says nothing)
    if (map_name != profile_map_name) profile_map_name = map_name, profile_map(map_name);
    profile_frame(ms, hitch);
    since_profile += ms / 1000.0;
    static const int batch_s = pf::batch_seconds(getenv("HL_PROFILE_BATCH_S"));
    if (since_profile >= batch_s) profile_queue(false);
}

/// HL_PROFILE_LOCAL with the performance summary off: the profile alone, from the presenting thread's frames (mu held)
void profile_local_frame(int64_t now) {
    static double since_map = 1e9, avg = 0;
    static std::string map = "";
    const int64_t before = last_frame_ns;
    last_frame_ns = now;
    if (!before || now - before > 2000000000LL) return;
    const double ms = (double)(now - before) / 1e6;
    if (since_map >= kMapEvery) {
        char name[120] = "";
        int64_t movie[5] = {};
        if (host->perf_info) host->perf_info(name, sizeof name, movie);
        map = map_name_of(name), since_map = 0;
    }
    const double usual = avg ? avg : ms;
    avg = usual * 0.98 + ms * 0.02;
    since_map += ms / 1000.0;
    profile_feed(map, ms, pr::is_hitch(ms, std::max(1000.0 / fps_cap_now(), usual)));
}

/// The map from the host, and the movies' totals since the last ask, onto the map they played on. True when Link is on a map
/// the summary has no room for (pr::kMaxMaps): it stays on the one before, for a summary to make room
bool where_now() {
    char map[120] = "";
    int64_t movie[5] = {};
    const char* version = host->perf_info ? host->perf_info(map, sizeof map, movie) : nullptr;
    if (!machine_read) read_machine(version);
    // (before a map: the start-up screen while the shaders are prepared, then the intro, the title and the file select)
    const std::string name = map[0] ? map : shaders_preparing() ? pr::kPreparingShaders : pr::kTitleAndIntro;
    profile_host_map = name;
    if (name != zone_map) zone_enter(name);   // (the zone recorder's own map switch: not held back by the summary's map cap)
    const bool full = current && name != current_map && !maps.count(name) && maps.size() >= (size_t)pr::kMaxMaps;
    if (!full && (!current || name != current_map)) {
        if (current) map_switched = true;   // (an area change: the frame that spans its load carries it, perf_frame_shown)
        current_map = name;
        current = &maps[name];
        current->fresh = !seen_maps.count(name);
    }
    current->movie_shown += movie[0] - last_movie[0];
    current->movie_dropped += movie[1] - last_movie[1];
    current->movie_late += movie[2] - last_movie[2];
    current->movie_decode_ms += (movie[3] - last_movie[3]) / 1e6;
    current->movie_fallbacks += movie[4] - last_movie[4];
    memcpy(last_movie, movie, sizeof last_movie);
    return full;
}

/// A report into dir/<sub>/<name>.json, whole or nothing; false when `limit` are waiting there already or it could not be written
bool write_summary(const fsys::path& dir, const std::string& name, const std::string& json, const char* sub = "perf_pending",
                   int limit = kMaxPending) {
    const fsys::path pending = dir / sub;
    std::error_code ec;
    {
        int waiting = 0;
        for (fsys::directory_iterator it(pending, ec), end; !ec && it != end; it.increment(ec))
            waiting += it->path().extension() == ".json";
        if (waiting >= limit) return false;
    }
    fsys::create_directory(pending, ec);
    const fsys::path tmp = pending / (name + ".tmp"), path = pending / (name + ".json");
    bool ok;
    {
        std::ofstream f(tmp, std::ios::binary);
        ok = f && f.write(json.data(), (std::streamsize)json.size()) && f.flush();
    }
    if (ok) fsys::rename(tmp, path, ec);
    if (!ok || ec) fsys::remove(tmp, ec);   // (whole or nothing: the sender never reads half)
    return ok && !ec;
}

constexpr int kMaxPipelinesPending = 4;   // (batches waiting in pipelines_pending at most: host/hos_perf_send.h HOS_PIPES_MAX_PENDING)
std::mutex states_mu;
int states_runs = 0;
uint64_t state_stamp = 0, shader_stamp = 0;
bool stamps_read = false;

/// A file's bytes up to `limit` (none when it is not there)
std::string read_file(const fsys::path& p, size_t limit) {
    std::ifstream f(p, std::ios::binary);
    std::string s;
    if (!f) return s;
    s.resize(limit);
    f.read(s.data(), (std::streamsize)limit);
    s.resize((size_t)f.gcount());
    return s;
}

/// The two stamps the records are sent with, read once (the shader stamp asks the host: not at the very end of the process)
void read_stamps() {
    if (stamps_read) return;
    stamps_read = true;
    state_stamp = pipeline_state_stamp(), shader_stamp = program_cache_stamp();
}

/// The session's end (mu held): the records this player's game made that the shipped list lacks go into pipelines_pending/ for the
/// next start to send, and are remembered in hookshot_pipeline_states_sent.bin. Says in the log how many, or why not. Needs the
/// performance statistics row, an address and not HL_TELEMETRY_PIPELINES=0; at most state_report::kMaxRecords a session
void states_queue() {
    namespace sr = state_report;
    std::unique_lock lk(states_mu, std::try_to_lock);   // (the window's close and the exit may both come here: the second finds nothing new)
    if (!lk) return;
    const bool first = states_runs++ == 0;
    const char* why = sr::why_not(opts.send_perf == 1, telemetry_available(), getenv(HOS_TELEMETRY_PIPELINES_ENV));
    const fsys::path dir = game_dir_path();
    std::string list, shipped_bytes;
    std::unordered_set<uint64_t> shipped;
    if (!*why) {
        shipped_bytes = read_file(dir / sr::kShippedFile, 64u << 20);
        shipped = sr::list_hashes(shipped_bytes);
        list = read_file(dir / sr::kListFile, 64u << 20);
        if (shipped.empty()) why = "no shipped pipeline state list to tell new records from";
        else if (sr::list_records(list).empty()) why = "this session recorded no pipeline states";
    }
    if (*why) {
        if (first) logf("vk: pipeline states: nothing queued to send: %s", why);
        return;
    }
    bool valid = false;
    const std::string sent_bytes = read_file(dir / sr::kSentFile, sr::kMaxSentFile);
    const std::unordered_set<uint64_t> sent = sr::sent_decode(sent_bytes, &valid);
    const sr::Batch batch = sr::select_new(list, shipped, sent);
    if (!batch.count) {
        if (first) logf("vk: pipeline states: 0 new records (%zu in the shipped list, %zu sent before, %zu not fit to send)", batch.in_shipped, batch.already_sent, batch.refused);
        return;
    }
    if (!machine_read) {
        char map[120] = "";
        int64_t movie[5] = {};
        read_machine(host->perf_info ? host->perf_info(map, sizeof map, movie) : nullptr);
    }
    read_stamps();
    sr::Meta meta;
    meta.version = machine.version, meta.release = machine.release, meta.driver = machine.driver;
    meta.vendor_id = dev.props.vendorID, meta.state_stamp = state_stamp, meta.shader_stamp = shader_stamp;
    char name[48];
    snprintf(name, sizeof name, "%lld-0", (long long)std::time(nullptr));
    if (!write_summary(dir, name, sr::payload_json(meta, batch), "pipelines_pending", kMaxPipelinesPending)) {
        logf("vk: pipeline states: %zu new records not queued: %d batches are waiting to be sent already, or the file could not be written", batch.count, kMaxPipelinesPending);
        return;
    }
    {   // (remembered once queued: the same records are never queued again)
        std::ofstream f(dir / sr::kSentFile, std::ios::binary | (valid ? std::ios::app : std::ios::trunc));
        const std::string add = sr::sent_encode(batch.hashes, !valid);
        f.write(add.data(), (std::streamsize)add.size());
    }
    logf("vk: pipeline states: %zu new records queued in pipelines_pending, sent at the next start (%zu in the shipped list, %zu sent before, %zu left for the next session)",
         batch.count, batch.in_shipped, batch.already_sent, batch.over_cap);
}

/// The session's end (mu held): the maps whose keys were not queued before for this version and these stamps go to
/// zones_pending/ for the next start to send, and are remembered in hookshot_zones_sent.bin. Says in the log how many
/// maps, or why not. Needs the performance statistics row, an address and not HL_TELEMETRY_ZONES=0
void zones_queue() {
    std::unique_lock lk(zones_mu, std::try_to_lock);   // (the window's close and the exit may both come here: the second finds nothing new)
    if (!lk) return;
    const bool first = zones_runs++ == 0;
    const char* why = zr::why_not(opts.send_perf == 1, telemetry_available(), getenv(HOS_TELEMETRY_ZONES_ENV));
    const fsys::path dir = game_dir_path();
    std::vector<const zr::Zone*> all;
    for (const auto& z : zone_slots) all.push_back(z.get());
    size_t uses = 0, capped = 0;
    for (const zr::Zone* z : all) uses += z->programs.uses.size() + z->states.uses.size(), capped += z->programs.capped + z->states.capped;
    if (*why) {
        if (first) logf("vk: zones: nothing queued to send: %s", why);
        return;
    }
    if (!machine_read) {
        char map[120] = "";
        int64_t movie[5] = {};
        read_machine(host->perf_info ? host->perf_info(map, sizeof map, movie) : nullptr);
    }
    read_stamps();
    bool valid = false;
    const std::string sent_bytes = read_file(dir / zr::kSentFile, zr::kMaxSentFile);
    const std::unordered_set<uint64_t> sent = zr::sent_decode(sent_bytes, &valid);
    const uint64_t seed = zr::stamp_seed(machine.version, state_stamp, shader_stamp);
    size_t already = 0;
    const std::vector<const zr::Zone*> fresh = zr::unsent_zones(all, seed, sent, &already);
    if (fresh.empty()) {
        if (first) logf("vk: zones: 0 maps to send (%lld lookups this session, %zu keys, %zu maps sent before for this version)",
                        (long long)zone_uses, uses, already);
        return;
    }
    size_t over = 0;
    const std::vector<std::vector<const zr::Zone*>> groups = zr::batch_groups(fresh, zr::kMaxBatchBytes, &over);
    zr::Meta meta;
    meta.version = machine.version, meta.release = machine.release, meta.driver = machine.driver;
    meta.vendor_id = dev.props.vendorID, meta.state_stamp = state_stamp, meta.shader_stamp = shader_stamp;
    const long long at = (long long)std::time(nullptr);
    size_t queued = 0, batches = 0;
    std::vector<uint64_t> marked;
    for (const std::vector<const zr::Zone*>& g : groups) {
        std::vector<std::string> parts;
        for (const zr::Zone* z : g) parts.push_back(zr::zone_json(*z));
        char name[48];
        snprintf(name, sizeof name, "%lld-%zu", at, batches);
        if (!write_summary(dir, name, zr::payload_json(meta, parts), "zones_pending", kMaxZonesPending)) {
            logf("vk: zones: %zu maps not queued: %d batches are waiting to be sent already, or the file could not be written",
                 fresh.size() - queued, kMaxZonesPending);
            break;
        }
        batches++, queued += g.size();
        for (const zr::Zone* z : g) marked.push_back(zr::zone_sent_hash(seed, z->name));
    }
    if (!marked.empty()) {   // (remembered once queued: the same map is never queued twice for the same version and stamps)
        std::ofstream f(dir / zr::kSentFile, std::ios::binary | (valid ? std::ios::app : std::ios::trunc));
        const std::string add = zr::sent_encode(marked, !valid);
        f.write(add.data(), (std::streamsize)add.size());
    }
    if (queued)
        logf("vk: zones: %zu maps (%lld lookups, %zu keys) queued in %zu batch%s in zones_pending, sent at the next start "
             "(%zu maps sent before, %zu over a batch alone, %zu uses of keys past the caps)",
             queued, (long long)zone_uses, uses, batches, batches == 1 ? "" : "es", already, over, capped);
}

/// The player turned the row off (or the address went): what is waiting and what was kept goes, nothing more is measured
void switch_off() {
    perf_measure.store(false, std::memory_order_relaxed);
    perf_parts::on.store(false, std::memory_order_relaxed);
    zones_measure.store(false, std::memory_order_relaxed);
    if (!profile_local()) profile_stop();   // (the gates say it: profile_follow stops it anyway; here its waiting batches go too)
    maps.clear(), current = nullptr, primed = false, last_frame_ns = 0;
    {
        std::lock_guard lk(zones_mu);
        zone_slots.clear(), zone_ids.clear(), zone_now = (uint32_t)zr::kMaxZones, zone_map.clear(), zones_told_full = false;
    }
    std::error_code ec;
    for (const char* sub : {"perf_pending", "perf_sent", "pipelines_pending", "pipelines_sent", "zones_pending", "zones_sent", "profile_pending", "profile_sent"}) {
        for (fsys::directory_iterator it(folder / sub, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string ext = it->path().extension().string(), leaf = it->path().filename().string();
            if (ext == ".json" || ext == ".tmp" || leaf == ".next" || leaf.rfind(".sending.", 0) == 0) fsys::remove(it->path(), ec);
        }
    }
    // (the pipeline records deleted before they were sent must not stay marked as sent)
    fsys::remove(folder / state_report::kSentFile, ec);
    fsys::remove(folder / zr::kSentFile, ec);
    logf("vk: performance statistics off: what was waiting in perf_pending and perf_sent (and the pipeline state records in "
         "pipelines_pending and pipelines_sent, the per-zone shader and pipeline keys in zones_pending and zones_sent, and the performance "
         "profile in profile_pending and profile_sent) is deleted");
}

/// The summary of what came in since the one before (under mu), written on a thread of its own unless `now` (the exit, a
/// lost device: the process ends next)
void summarize(bool now) {
    if (!current) return;
    where_now();
    read_settings();
    {   // (the renderer's events, onto the map they ended on: there are few)
        int64_t translated = 0, failed = 0;
        shader_stats(&translated, &failed);
        current->event[pr::kProgramFailed] += failed - l_failed, l_failed = failed;
        for (int e = 0; e < pr::kEvents; e++) current->event[e] += events[e].exchange(0);
    }
    bool any = false;
    for (const auto& [n, t] : maps) any = any || t.any();
    if (!any) return;   // (nothing since the last: the exit right after a lost device's summary)
    pr::Memory mem;
    uint64_t used = 0, allowed = 0;
    if (vram_usage_now(&used, &allowed)) mem.vram_used_gb = used / 1073741824.0, mem.vram_allowed_gb = allowed / 1073741824.0;
    mem.ram_used_gb = ram_used_gb();
    const double session_s = (clock_ns() - started_ns) / 1e9;
    std::string json = pr::summary_json(machine, first, session_s, current_map, &mem, maps);
    char name[48];
    snprintf(name, sizeof name, "%lld-%d", (long long)std::time(nullptr), seq++);
    for (auto& [n, t] : maps)
        if (t.any()) seen_maps[n] = true;
    maps.clear();
    current = &maps[current_map];
    current->fresh = !seen_maps.count(current_map);   // (a map entered just before this summary, with nothing yet: still new)
    first = false;
    if (now) return write_summary(folder, name, json);
    std::thread([dir = folder, n = std::string(name), j = std::move(json)] { write_summary(dir, n, j); }).detach();
}

// (an exit that runs atexit: Windows' ExitProcess, exit(). The window's close writes it first, mkwvk_close_asked; then
// this finds nothing new. Only tried: ExitProcess has already ended the other threads, and one may have held mu)
void at_exit() {
    if (!perf_measure.load(std::memory_order_relaxed) && !profile_running()) return;
    std::unique_lock lk(mu, std::try_to_lock);
    if (!lk) return;
    if (perf_measure.load(std::memory_order_relaxed)) {
        summarize(true);
        states_queue();
        zones_queue();
    }
    profile_queue(true);
}

/// HOS_PERF_REPORT=1 (from the real environment): a summary with no address to send to, measured and written the same
/// way for this machine alone; the sender still needs an address, so nothing ever leaves the game folder
bool perf_local() {
    static const bool local = [] {
        const char* e = getenv("HOS_PERF_REPORT");
        return e && *e && atoi(e) != 0;
    }();
    return local;
}

/// Whether the summary should be measured now: the row is on and the build has an address (or HOS_PERF_REPORT: local).
/// Read once
bool wanted() {
    static const bool available = !address_now().empty();
    return (available || perf_local()) && opts.send_perf == 1;
}

/// The row changed (checked about once a second, under mu): on starts a session's measuring, off deletes what was kept
void follow_row() {
    profile_follow();
    const bool on = wanted(), was = perf_measure.load(std::memory_order_relaxed);
    if (on == was) return;
    if (!on) return switch_off();
    folder = game_dir_path();
    {   // (the zone recorder starts on the map it is already on, and a map's keys are recorded whole from its next entry)
        std::lock_guard lk(zones_mu);
        zone_map.clear();
    }
    zones_measure.store(hos_telemetry_zones_on(getenv(HOS_TELEMETRY_ZONES_ENV)), std::memory_order_relaxed);
    {   // (a summary cut short by an exit before: its .tmp, never sent and never counted)
        std::error_code ec;
        for (fsys::directory_iterator it(folder / "perf_pending", ec), end; !ec && it != end; it.increment(ec))
            if (it->path().extension() == ".tmp") fsys::remove(it->path(), ec);
    }
    static bool registered = false;
    if (!registered) registered = true, std::atexit(at_exit);
    period = pr::summary_period(getenv("RX_VK_PERF_INTERVAL"));
    started_ns = clock_ns();
    int64_t translated = 0;
    shader_stats(&translated, &l_failed);   // (the renderer's counts so far: only what comes after is counted)
    l_gpu = gpu_time_ns(), l_rb = readback_wait_ns();
    (void)thread_cpu_ns();   // (its cycles calibrated here, once: the presenting thread's os_wait needs them)
    read_stamps();
    logf("vk: performance statistics on (the Send performance statistics row)%s: a summary every %.0f s into perf_pending",
         perf_local() && address_now().empty() ? ", HOS_PERF_REPORT: local, no address so nothing is ever sent" : "", period);
    perf_measure.store(true, std::memory_order_relaxed);
    perf_parts::on.store(true, std::memory_order_relaxed);
}

}   // namespace

bool telemetry_available() {
    static const bool available = !address_now().empty();
    return available;
}

void perf_event(int e) {
    if (e >= 0 && e < pr::kEvents && perf_measure.load(std::memory_order_relaxed)) events[e].fetch_add(1, std::memory_order_relaxed);
}

void zone_note_program(uint64_t key) { zone_note(true, key); }
void zone_note_state(uint64_t key) { zone_note(false, key); }

void perf_frame_shown() {
    static std::atomic<unsigned> calls{0};
    // (off: a look at the row every 64th frame, and nothing else)
    if (!perf_measure.load(std::memory_order_relaxed)) {
        if ((calls.fetch_add(1, std::memory_order_relaxed) & 63) == 0) {
            std::lock_guard lk(mu);
            follow_row();
        }
        if (!perf_measure.load(std::memory_order_relaxed)) {
            if (profile_running()) {   // (HL_PROFILE_LOCAL alone: the profile's own frame feed)
                std::lock_guard lk(mu);
                profile_local_frame(clock_ns());
            }
            return;
        }
    }
    const int64_t now = clock_ns();
    // (the renderer's counters since the frame before: what the slow frames waited on)
    static int64_t l_prof[PROF_COUNT];
    static double l_wait_ms, l_shader_ms, l_pipe_ms, l_link_ms, l_lib_ms;
    static int64_t l_render = 0, l_wait_ns = 0, l_present_ns = 0;
    double wait_ms_total = 0;
    int64_t skips = 0, waits = 0;
    draw_stats3(&skips, &waits, &wait_ms_total);
    const int64_t fresh_skips = draw_pipe_fresh_skips();
    const double shader_ms = draw_shader_wait_ms();
    int64_t passes = 0, mesh = 0, links = 0, lib_hits = 0, lib_cold = 0, lib_built = 0;
    double pipe_ms = 0, link_ms = 0, lib_build_ms = 0;
    draw_stats2(&passes, &mesh, &pipe_ms);
    draw_stats4(&links, &link_ms, &lib_hits, &lib_cold, &lib_built, &lib_build_ms);
    const int64_t render_ns = perf_parts::t_render, wait_in = perf_parts::t_wait, present_in = perf_parts::t_present;
    const int64_t cpu = thread_cpu_ns(), rb = readback_wait_ns();
    const auto dp = [](int i) { return (prof_ns[i] - l_prof[i]) / 1e6; };
    pr::Waits w;
    w.translate = std::max(0.0, shader_ms - l_shader_ms);
    w.pipeline = std::max(0.0, (wait_ms_total - l_wait_ms) - w.translate);
    w.upload = dp(PROF_UPLOAD);
    w.memsync = dp(PROF_MEMSYNC);
    w.readback = std::max(0.0, (rb - l_rb) / 1e6);
    w.driver_compile = std::max(0.0, pipe_ms - l_pipe_ms) + std::max(0.0, link_ms - l_link_ms) +
                       std::max(0.0, lib_build_ms - l_lib_ms);
    w.descriptors = dp(PROF_DESC);
    w.present = std::max(0.0, (present_in - l_present_ns) / 1e6);
    for (int i = 0; i < PROF_COUNT; i++) l_prof[i] = prof_ns[i];
    l_wait_ms = wait_ms_total, l_shader_ms = shader_ms, l_pipe_ms = pipe_ms, l_link_ms = link_ms, l_lib_ms = lib_build_ms;
    const int64_t gpu = gpu_time_ns();
    fs::Frame f;
    f.gpu_ms = (float)std::max<int64_t>(0, gpu - l_gpu) / 1e6f;
    l_gpu = gpu;
    // (the parts on this thread, since the frame before: the deltas of the scopes perf_parts.h keeps here; the pacing
    // sleep the host pushed at this present, the GPU waits inside the renderer's calls (the recording slot, the read-backs,
    // the swapchain), the renderer's own work beside them. f.ms is known only under the lock below, where they are cut to it)
    const double pace_ms = perf_parts::pace_us.exchange(0, std::memory_order_relaxed) / 1000.0;
    const double rb_ms = w.readback, wait_ms = (wait_in - l_wait_ns) / 1e6, present_ms = w.present;
    const double render_ms = (render_ns - l_render) / 1e6, cpu_ms = (cpu - l_cpu) / 1e6;
    const int64_t left_out = (skips - l_skips) + (fresh_skips - l_fresh);
    l_render = render_ns, l_wait_ns = wait_in, l_present_ns = present_in;
    l_rb = rb, l_cpu = cpu, l_skips = skips, l_fresh = fresh_skips;
    f.left_out = (uint16_t)std::clamp<int64_t>(left_out, 0, 0xffff);

    std::lock_guard lk(mu);
    const int64_t before = last_frame_ns;
    last_frame_ns = now;
    // (the first frame only takes the counters: they hold everything since the game started; a gap over 2 s, a pause or a
    // stall of the window, is no frame)
    if (!primed || !before || now - before > 2000000000LL) {
        primed = true;
        return;
    }
    f.ms = (float)((now - before) / 1e6);
    if ((calls.fetch_add(1, std::memory_order_relaxed) & 63) == 0) follow_row();
    if (!perf_measure.load(std::memory_order_relaxed)) return;
    if (!current || since_map >= kMapEvery) {
        // (a 17th map in one period: that summary now, with the 16 it has, then the new map in the next)
        if (where_now()) summarize(false), since_summary = 0, where_now();
        since_map = 0;
    }
    {   // (the frame's parts, cut to its ms: game + render + gpu_wait + pace is the whole frame)
        f.part[fs::kPace] = (float)std::clamp(pace_ms, 0.0, (double)f.ms);
        const double rest = (double)f.ms - f.part[fs::kPace];
        f.part[fs::kGpuWait] = (float)std::clamp(rb_ms + wait_ms + present_ms, 0.0, std::max(0.0, rest));
        f.part[fs::kRender] =
            (float)std::clamp(render_ms - wait_ms - present_ms, 0.0, std::max(0.0, rest - f.part[fs::kGpuWait]));
        f.part[fs::kGame] = (float)std::max(0.0, rest - f.part[fs::kGpuWait] - f.part[fs::kRender]);
        // (the game's CPU at work, and the wall time this thread was neither running nor in the renderer nor pacing)
        w.os_wait = std::max(0.0, (double)f.part[fs::kGame] - std::max(0.0, cpu_ms - (double)f.part[fs::kRender]));
    }
    // (the target: the cap's. A hitch is twice the longer of the target and the recent average, before this frame: a frame
    // twice the usual, never every frame of a game the GPU holds under its cap)
    const double usual = avg_ms ? avg_ms : f.ms;
    avg_ms = usual * 0.98 + f.ms * 0.02;
    const double target = 1000.0 / fps_cap_now();
    // (a map was loading, or the frame just after one: the stall a load makes is presented once the load has ended, so
    // the flag is kept for that frame too: host/hos_main.c pushes Sequence_IsLoadingBanc's level, as it changes. A map
    // change marks the frame as well: the load blocks the game thread, so the flag is never seen from its tick)
    static bool load_last = false;
    const bool load_now = perf_parts::loading.load(std::memory_order_relaxed) != 0;
    const bool loading = load_now || load_last || map_switched;
    load_last = load_now, map_switched = false;
    pr::add_frame(*current, f, w, target, last_ms, usual, loading);
    profile_feed(profile_host_map, f.ms, pr::is_hitch(f.ms, std::max(target, usual)));
    last_ms = f.ms;
    since_map += f.ms / 1000.0, since_summary += f.ms / 1000.0;
    if (since_summary >= period) summarize(false), since_summary = 0;
}

void perf_report_final(int event) {
    const bool measuring = perf_measure.load(std::memory_order_relaxed);
    if (measuring) perf_event(event);
    std::lock_guard lk(mu);
    if (measuring) summarize(true);
    states_queue();   // (off: logs why it queues nothing)
    zones_queue();    // (the same moment: the per-zone keys are the recorder's own)
    profile_queue(true);   // (and the profile's last batch)
}

}   // namespace mkwvk
