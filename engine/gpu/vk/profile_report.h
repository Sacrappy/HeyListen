// (heylisten) The detailed performance profile's rules (profile_run.cpp samples the CPU and collects the GPU's passes, perf_report.cpp
// queues the batches, the host's sender, host/hos_crash_dialog.c, posts them to <the build's address>/telemetry/profile): WHERE a
// map's time goes, as opposed to the performance summary's what a slow frame waited on. Per map: how many samples of each kind of
// thread were in the game's lifted code, our native engine, the renderer, the driver, the OS..., the guest functions and the engine
// locations with the most samples, and the GPU's time per render pass.
//
// What is sent: counts and our own identities. A lifted function is its guest address (the crash report's guest_frames name them
// the same), an engine location is an offset inside our own binary (the build_id says which build), a GPU pass is a hash of its
// target formats, size class and first program key. No names, no paths, no code or data of the game.
//
// On only with "Send performance statistics" (the same row, the same address) and not HL_TELEMETRY_PROFILE=0; HL_PROFILE_LOCAL=1
// writes the same JSON into the game folder's profile_local/ with no address and with the row off. Batches of the maps that changed
// are written into profile_pending/ every five minutes and at the end, at most kMaxPending waiting, each at most kMaxBodyBytes.
//
// Plain functions and data, no Vulkan, no OS: tests/test_profile_report.cpp checks them.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "perf_report.h"

namespace mkwvk::profile_report {

constexpr int kFormat = 1;                       // ("v" in the payload)
constexpr size_t kMaxMaps = 32;                  // maps in a payload (and a session's maps recorded)
constexpr size_t kMaxThreads = 16;               // thread entries in a map
constexpr size_t kMaxGuest = 32, kMaxEngine = 32, kMaxPasses = 24;
constexpr size_t kMaxBodyBytes = 256 * 1024;     // one batch's body
constexpr int kMaxPending = 8;                   // files waiting in profile_pending (host/hos_perf_send.h HOS_PROFILE_MAX_PENDING)
constexpr int kBatchSeconds = 300;               // between batches
constexpr int kHzDefault = 250, kHzMin = 50, kHzMax = 1000;
constexpr size_t kTableKeep = 1024, kTableMost = 4096;   // a top table's size when it is cut back, and where it is cut
constexpr size_t kPassKeep = 128, kPassMost = 512;
constexpr size_t kRingSamples = 16384;           // samples waiting for their frame to close (older ones are dropped)
constexpr int kHitchFrames = 512;                // how many closed frames' verdicts are kept for the GPU's late read-back

/// The module a sampled program counter is in. The list is fixed: the server knows these names
enum Module { kGameLifted, kNativeEngine, kRenderer, kHost, kAudio, kDriver, kOs, kRuntime, kOther, kModules };
inline const char* module_name(int m) {
    static const char* const t[] = {"game_lifted", "native_engine", "renderer", "host", "audio", "driver", "os", "runtime", "other"};
    return m >= 0 && m < kModules ? t[m] : "other";
}

/// What a thread is for (the entry's "role"; threads of one role are merged)
enum Role { kRoleGame, kRoleRender, kRolePresent, kRoleCompile, kRoleUpload, kRoleAudio, kRoleOther, kRoles };
inline const char* role_name(int r) {
    static const char* const t[] = {"game", "render", "present", "compile", "upload", "audio", "other"};
    return r >= 0 && r < kRoles ? t[r] : "other";
}

/// What a GPU pass is. Renderer-owned passes are labelled exactly (upscale: the FSR 3.1 upscaler's passes, framegen: the frame
/// interpolation's, compute: a game compute dispatch); the game's render passes by pass_kind's heuristic; hdr and copy are in the list
/// for the server and not sent yet (the renderer's own HDR pass and its copies are not timed)
enum PassKind { kShadow, kMain, kPost, kUi, kUpscale, kFramegen, kHdr, kCopy, kCompute, kOtherPass, kPassKinds };
inline const char* pass_kind_name(int k) {
    static const char* const t[] = {"shadow", "main", "post", "ui", "upscale", "framegen", "hdr", "copy", "compute", "other"};
    return k >= 0 && k < kPassKinds ? t[k] : "other";
}

/// The heuristic for a game render pass: no colour target with a depth one is a shadow (or depth) pass; colour and depth is the
/// scene (main); colour alone is a screen effect (post) when it has few draws or is under a quarter of the output's pixels, else
/// the interface (ui) when it has many draws. `out_pixels` is the window's size (0 unknown: every colour-only pass is post)
inline PassKind pass_kind(int colors, bool depth, uint32_t width, uint32_t height, int draws, uint64_t out_pixels) {
    if (!colors) return depth ? kShadow : kOtherPass;
    if (depth) return kMain;
    const uint64_t px = (uint64_t)width * height;
    if (!out_pixels || px * 4 < out_pixels || draws <= 4) return kPost;
    return kUi;
}

/// A pass's identity: the target formats (first one, count, depth), a size class and the first program's key. The size class is the
/// width and height rounded to 64, so that a dynamic resolution's small steps are one pass
inline uint64_t fnv_add(uint64_t h, const void* d, size_t n) {
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)d)[i]) * 1099511628211ull;
    return h;
}
inline uint64_t pass_key(int format0, int colors, bool depth, uint32_t width, uint32_t height, uint64_t first_prog) {
    uint64_t h = 0xCBF29CE484222325ull;
    const uint32_t f[5] = {(uint32_t)format0, (uint32_t)colors | (depth ? 0x100u : 0u), (width + 32) / 64, (height + 32) / 64, 0};
    h = fnv_add(h, f, 16);
    return fnv_add(h, &first_prog, 8);
}
/// A renderer-owned pass's key: its name
inline uint64_t name_key(const std::string& name) { return fnv_add(0xCBF29CE484222325ull, name.data(), name.size()); }

inline std::string hex16(uint64_t v) {
    char b[24];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return b;
}
inline std::string hex_prefixed(uint64_t v) {
    char b[24];
    snprintf(b, sizeof b, "0x%llx", (unsigned long long)v);
    return b;
}

/// ---- the module buckets ----

inline std::string lower(std::string s) {
    for (char& c : s) c = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    return s;
}
inline bool starts(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }

/// The bucket of a module that is not our executable or renderer, by its file name (any case). `exports_icd`: it exports the Vulkan ICD
/// entry point (a driver whatever its name). Unknown DLLs are the runtime: the C++ runtime and the libraries around it
inline Module module_bucket(const std::string& file_name, bool exports_icd) {
    const std::string n = lower(file_name);
    static const char* const os[] = {"ntdll", "kernel32", "kernelbase", "win32u", "user32", "gdi32", "gdi32full", "advapi32", "sechost",
                                     "rpcrt4", "combase", "ole32", "oleaut32", "shell32", "shcore", "ws2_32", "mswsock", "bcrypt",
                                     "bcryptprimitives", "cfgmgr32", "imm32", "msctf", "uxtheme", "dwmapi", "setupapi", "winmm",
                                     "version", "crypt32", "wintrust", "powrprof", "dxgi", "d3d11", "d3d12", "dcomp"};
    for (const char* p : os)
        if (n == std::string(p) + ".dll") return kOs;
    if (exports_icd) return kDriver;
    static const char* const driver[] = {"vulkan-1", "nvoglv", "nvwgf2", "nvapi", "nvcuda", "nvml", "amdvlk", "amdxx", "amdxc", "atidxx",
                                         "atiadlxx", "amdihk", "igvk", "igd", "ig9", "igxe", "igc", "nvlddmkm", "libvulkan", "radeonsi",
                                         "libgl", "libdrm", "libnvidia", "libamdvlk"};
    for (const char* p : driver)
        if (starts(n, p)) return kDriver;
    static const char* const runtime[] = {"ucrtbase", "msvcrt", "msvcp", "vcruntime", "libc++", "libstdc++", "libgcc", "libwinpthread",
                                          "libunwind", "api-ms-win", "ext-ms-win", "libc.so", "libm.so", "libpthread", "ld-linux", "ld-musl"};
    for (const char* p : runtime)
        if (starts(n, p)) return kRuntime;
    return kRuntime;   // (another DLL: SDL, FFmpeg, an overlay: the runtime around the game)
}

/// A named engine function's bucket (host/hos_profile.c asks this with the symbol table's name): our native engine (the hl:: namespace
/// and the hl_ glue), the audio code (the HLE's, the decoders'), else the host. "" (no name) is the host
inline Module engine_bucket(const std::string& name) {
    const std::string n = lower(name);
    if (starts(n, "hl::") || starts(n, "hl_") || n.find("::hl::") != std::string::npos || starts(n, "_zn2hl")) return kNativeEngine;
    if (starts(n, "nn_audio") || starts(n, "opus_") || starts(n, "celt_") || starts(n, "silk_") ||
        n.find("audio") != std::string::npos)
        return kAudio;
    return kHost;
}

/// A range of addresses and what it is (the module list's entry)
struct Range {
    uintptr_t lo = 0, hi = 0;   // [lo, hi)
    int bucket = kOther;
    bool exe = false, vk = false;
};
/// The range holding pc in a list sorted by lo; nullptr when none does
inline const Range* range_of(const std::vector<Range>& ranges, uintptr_t pc) {
    size_t lo = 0, hi = ranges.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (ranges[mid].lo <= pc) lo = mid + 1;
        else hi = mid;
    }
    if (!lo) return nullptr;
    const Range& r = ranges[lo - 1];
    return pc < r.hi ? &r : nullptr;
}

/// What the thread's role is, from its name (SetThreadDescription's): the renderer's and the host's own names. A name no rule knows
/// is "other" (the OS's pool threads, the driver's, unnamed ones); the guest's own threads are the game
inline Role role_of_name(const std::string& name) {
    const std::string n = lower(name);
    if (n.empty()) return kRoleOther;
    if (n.find("audio") != std::string::npos) return kRoleAudio;
    if (starts(n, "mkw_vk replay") || n == "hos held submits") return kRoleRender;
    if (starts(n, "mkw_vk display")) return kRolePresent;
    if (starts(n, "mkw_vk worker") || starts(n, "mkw_vk serial builds") || n.find("compile") != std::string::npos ||
        n.find("translat") != std::string::npos)
        return kRoleCompile;
    if (starts(n, "mkw_vk capture") || starts(n, "mkw_vk imports") || n.find("upload") != std::string::npos ||
        n.find("decode") != std::string::npos)
        return kRoleUpload;
    if (n == "game" || n == "mainthread" || starts(n, "thread_") || starts(n, "hl-worker-")) return kRoleGame;
    return kRoleOther;
}

/// HL_PROFILE_HZ: a whole number 50 to 1000, else 250
inline int sample_hz(const char* env) {
    char* end = nullptr;
    const long v = env ? strtol(env, &end, 10) : 0;
    if (!env || end == env || *end) return kHzDefault;
    return (int)std::clamp<long>(v, kHzMin, kHzMax);
}

/// ---- the per-map data ----

struct Counts {
    int64_t samples = 0, hitch = 0;
};

/// A table of keys with their samples, cut back to its best kTableKeep when it grows past kTableMost (memory stays bounded; a key
/// seen rarely may lose its early counts, a busy one never does)
struct Top {
    std::unordered_map<uint64_t, Counts> m;
    void add(uint64_t key, bool hitch, int64_t n = 1) {
        Counts& c = m[key];
        c.samples += n;
        if (hitch) c.hitch += n;
        if (m.size() > kTableMost) cut();
    }
    void cut() {
        std::vector<std::pair<uint64_t, Counts>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.samples > b.second.samples; });
        v.resize(std::min(v.size(), kTableKeep));
        m.clear();
        for (const auto& e : v) m.emplace(e.first, e.second);
    }
    /// The n keys with the most samples, most first (ties by key)
    std::vector<std::pair<uint64_t, Counts>> best(size_t n) const {
        std::vector<std::pair<uint64_t, Counts>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
            return a.second.samples != b.second.samples ? a.second.samples > b.second.samples : a.first < b.first;
        });
        if (v.size() > n) v.resize(n);
        return v;
    }
};

/// The samples of one role in one map
struct RoleAgg {
    int64_t samples = 0, hitch_samples = 0;
    int64_t mod[kModules] = {}, hitch_mod[kModules] = {};
    std::set<uint32_t> threads;   // (the thread ids that had a sample: "count")
};

/// An engine location's key: the offset inside the module, and a flag for the renderer's DLL (the exe's have none)
constexpr uint64_t kVkFlag = 1ull << 63;
inline uint64_t engine_key(uint64_t rva, bool vk) { return (rva & 0xFFFFFFFFull) | (vk ? kVkFlag : 0); }

/// What a pass took in a map
struct PassAgg {
    int64_t count = 0;
    double ms_sum = 0, ms_max = 0, hitch_ms_sum = 0;
};

struct MapProfile {
    std::string name;
    double seconds = 0;
    int64_t frames = 0, hitch_frames = 0;
    double frame_ms_sum = 0;
    int hz = kHzDefault;
    RoleAgg role[kRoles];
    Top guest, engine;
    std::map<std::pair<int, uint64_t>, PassAgg> passes;   // (kind, key)
    bool gpu = false;   // (timestamps are on: the payload has a gpu block even before the first pass is read back)
    bool any() const {
        if (frames > 0) return true;
        for (const RoleAgg& r : role)
            if (r.samples) return true;
        return !passes.empty();
    }
    int64_t cpu_samples() const {
        int64_t n = 0;
        for (const RoleAgg& r : role) n += r.samples;
        return n;
    }
    void add_cpu(int role_id, int module, uint32_t tid, bool hitch) {
        RoleAgg& r = role[role_id];
        r.samples++, r.mod[module]++;
        if (hitch) r.hitch_samples++, r.hitch_mod[module]++;
        if (r.threads.size() < 4096) r.threads.insert(tid);
    }
    void add_pass(int kind, uint64_t key, double ms, bool hitch) {
        PassAgg& p = passes[{kind, key}];
        p.count++, p.ms_sum += ms, p.ms_max = std::max(p.ms_max, ms);
        if (hitch) p.hitch_ms_sum += ms;
        if (passes.size() > kPassMost) cut_passes();
    }
    void cut_passes() {
        std::vector<std::pair<std::pair<int, uint64_t>, PassAgg>> v(passes.begin(), passes.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.ms_sum > b.second.ms_sum; });
        v.resize(std::min(v.size(), kPassKeep));
        passes.clear();
        for (const auto& e : v) passes.emplace(e.first, e.second);
    }
};

/// A frame's samples wait for it to close: the frame's index when taken, the map, what and where. The ring is bounded: when full the
/// oldest is dropped
struct Sample {
    uint64_t frame = 0, key = 0;
    uint16_t map = 0;
    uint8_t role = 0, module = 0, key_kind = 0;   // key_kind: 0 none, 1 guest address, 2 engine location
    uint32_t tid = 0;
};
struct SampleRing {
    std::vector<Sample> buf;
    size_t head = 0, n = 0, dropped = 0;
    explicit SampleRing(size_t cap = kRingSamples) : buf(cap) {}
    void push(const Sample& s) {
        if (n == buf.size()) head = (head + 1) % buf.size(), n--, dropped++;
        buf[(head + n) % buf.size()] = s, n++;
    }
    /// The samples of frames before `closed` (in order taken) to `sink(sample)`; they leave the ring
    template <class F>
    size_t take_closed(uint64_t closed, F&& sink) {
        size_t k = 0;
        while (n && buf[head].frame < closed) {
            sink(buf[head]);
            head = (head + 1) % buf.size(), n--, k++;
        }
        return k;
    }
};

/// The frames closed so far and which of them were hitches (a ring of the last kHitchFrames: the GPU's passes are read back some
/// frames late and ask)
struct FrameVerdicts {
    uint64_t closed = 0;
    std::vector<uint64_t> frame_at = std::vector<uint64_t>(kHitchFrames, ~0ull);
    std::vector<uint8_t> hitch = std::vector<uint8_t>(kHitchFrames, 0);
    void close(bool is_hitch) {
        frame_at[closed % kHitchFrames] = closed, hitch[closed % kHitchFrames] = is_hitch ? 1 : 0;
        closed++;
    }
    /// Whether frame f closed as a hitch (false when it is open, or too old to remember)
    bool is_hitch(uint64_t f) const { return f < closed && frame_at[f % kHitchFrames] == f && hitch[f % kHitchFrames]; }
};

/// ---- the JSON ----

inline std::string modules_json(const int64_t (&v)[kModules]) {
    std::string o = "{";
    for (int m = 0; m < kModules; m++) o += std::string(m ? "," : "") + "\"" + module_name(m) + "\":" + std::to_string(v[m]);
    return o + "}";
}

inline std::string top_json(const std::vector<std::pair<uint64_t, Counts>>& best, bool guest) {
    std::string o = "[";
    bool comma = false;
    for (const auto& [key, c] : best) {
        o += std::string(comma ? "," : "") + "{\"" + (guest ? "addr" : "rva") + "\":\"" +
             hex_prefixed(guest ? key : (key & ~kVkFlag)) + "\"";
        if (!guest && (key & kVkFlag)) o += ",\"module\":\"vk\"";
        o += ",\"samples\":" + std::to_string(c.samples) + ",\"hitch_samples\":" + std::to_string(c.hitch) + "}";
        comma = true;
    }
    return o + "]";
}

/// One map's part of the body (the format in docs/performance-statistics.md), within the caps
inline std::string map_json(const MapProfile& m) {
    namespace pr = perf_report;
    std::string o = "{\"map\":" + pr::json_text(m.name, 120) + ",\"seconds\":" + pr::json_num(m.seconds) +
                    ",\"frames\":" + std::to_string(m.frames) + ",\"hitch_frames\":" + std::to_string(m.hitch_frames);
    if (m.cpu_samples() > 0) {
        o += ",\"cpu\":{\"hz\":" + std::to_string(m.hz) + ",\"threads\":[";
        bool comma = false;
        size_t entries = 0;
        for (int r = 0; r < kRoles && entries < kMaxThreads; r++) {
            const RoleAgg& a = m.role[r];
            if (!a.samples) continue;
            o += std::string(comma ? "," : "") + "{\"role\":\"" + role_name(r) + "\",\"count\":" +
                 std::to_string(std::max<size_t>(1, a.threads.size())) + ",\"samples\":" + std::to_string(a.samples) +
                 ",\"hitch_samples\":" + std::to_string(a.hitch_samples) + ",\"modules\":" + modules_json(a.mod) +
                 ",\"hitch_modules\":" + modules_json(a.hitch_mod) + "}";
            comma = true, entries++;
        }
        o += "],\"top_guest\":" + top_json(m.guest.best(kMaxGuest), true) + ",\"top_engine\":" + top_json(m.engine.best(kMaxEngine), false) + "}";
    }
    if (m.gpu && !m.passes.empty()) {
        std::vector<std::pair<std::pair<int, uint64_t>, PassAgg>> v(m.passes.begin(), m.passes.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
            return a.second.ms_sum != b.second.ms_sum ? a.second.ms_sum > b.second.ms_sum : a.first < b.first;
        });
        if (v.size() > kMaxPasses) v.resize(kMaxPasses);
        o += ",\"gpu\":{\"frame_ms_sum\":" + pr::json_num(m.frame_ms_sum) + ",\"passes\":[";
        bool comma = false;
        for (const auto& [id, p] : v) {
            o += std::string(comma ? "," : "") + "{\"kind\":\"" + pass_kind_name(id.first) + "\",\"key\":\"" + hex16(id.second) +
                 "\",\"count\":" + std::to_string(p.count) + ",\"ms_sum\":" + pr::json_num(p.ms_sum) + ",\"ms_max\":" +
                 pr::json_num(p.ms_max) + ",\"hitch_ms_sum\":" + pr::json_num(p.hitch_ms_sum) + "}";
            comma = true;
        }
        o += "]}";
    }
    return o + "}";
}

/// What the payload says besides the maps. gpu, cpu and os are plain names (at most 120 characters), left out when empty
struct Meta {
    std::string version, driver, gpu, cpu, os;
    bool release = false;
    uint32_t vendor_id = 0;   // (the GPU's PCI vendor: 0x10DE, 0x1002, 0x8086)
    uint64_t build_id = 0;    // (the executable's: 16 hex digits)
};

/// The body posted to /telemetry/profile (format 1): the version, the release flag, the build id, the GPU's vendor and the driver,
/// the plain names of the GPU, CPU and OS when known, then the maps in the order given. No path, no user, no machine name
inline std::string payload_json(const Meta& m, const std::vector<std::string>& parts) {
    namespace pr = perf_report;
    std::string o = "{\"v\":" + std::to_string(kFormat) + ",\"version\":" + pr::json_text(m.version, 40) +
                    ",\"release\":" + (m.release ? "true" : "false") + ",\"build_id\":\"" + hex16(m.build_id) + "\",\"vendor_id\":" +
                    std::to_string(m.vendor_id) + ",\"driver\":" + pr::json_text(m.driver, 60);
    if (!m.gpu.empty()) o += ",\"gpu\":" + pr::json_text(m.gpu, 120);
    if (!m.cpu.empty()) o += ",\"cpu\":" + pr::json_text(m.cpu, 120);
    if (!m.os.empty()) o += ",\"os\":" + pr::json_text(m.os, 120);
    o += ",\"maps\":[";
    bool comma = false;
    for (const std::string& p : parts) o += std::string(comma ? "," : "") + p, comma = true;
    return o + "]}";
}

/// The batches: the maps in the order given, as many to a batch as fit in max_bytes (the head counted against each), at most
/// kMaxMaps to a batch. A map alone over the cap is left out (*over: how many)
inline std::vector<std::vector<size_t>> batch_groups(const std::vector<std::string>& parts, size_t max_bytes, size_t* over = nullptr) {
    std::vector<std::vector<size_t>> out;
    std::vector<size_t> group;
    size_t group_bytes = 0, left = 0;
    const size_t head = 400;
    for (size_t i = 0; i < parts.size(); i++) {
        const size_t bytes = parts[i].size() + 1;
        if (head + bytes > max_bytes) {
            left++;
            continue;
        }
        if (!group.empty() && (head + group_bytes + bytes > max_bytes || group.size() >= kMaxMaps)) out.push_back(std::move(group)), group = {}, group_bytes = 0;
        group.push_back(i), group_bytes += bytes;
    }
    if (!group.empty()) out.push_back(std::move(group));
    if (over) *over = left;
    return out;
}

/// HL_TELEMETRY_PROFILE: on unless exactly "0" (hos_telemetry.h's rule for the other two)
inline bool profile_switch_on(const char* env) { return !(env && env[0] == '0' && env[1] == 0); }
/// HL_PROFILE_BATCH_S: seconds between batches, a whole number 10 to 3600, else 300 (a short run of ours wants one before it ends)
inline int batch_seconds(const char* env) {
    char* end = nullptr;
    const long v = env ? strtol(env, &end, 10) : 0;
    if (!env || end == env || *end) return kBatchSeconds;
    return (int)std::clamp<long>(v, 10, 3600);
}
/// HL_PROFILE_LOCAL=1 (any non-zero whole number): the local copy
inline bool local_on(const char* env) {
    char* end = nullptr;
    const long v = env ? strtol(env, &end, 10) : 0;
    return env && end != env && !*end && v != 0;
}

/// Why nothing is sent, "" when it may be: the row first (the opt-in), then the address, then the separate switch
inline const char* why_not(bool row_on, bool has_address, const char* env_switch) {
    if (!row_on) return "Send performance statistics is off";
    if (!has_address) return "this build has no telemetry address";
    if (!profile_switch_on(env_switch)) return "HL_TELEMETRY_PROFILE=0";
    return "";
}

}   // namespace mkwvk::profile_report
