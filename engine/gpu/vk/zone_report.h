// (heylisten) The per-zone shader and pipeline usage's rules (perf_report.cpp records it on the draw path and queues the
// batch, the host's sender, host/hos_crash_dialog.c, posts it to <the build's address>/telemetry/zones): which shader
// program keys and which pipeline state keys a map's draws first used, in first-use order, so that the next release can
// preload the next zone's pipelines during its transitions and let go of the ones it does not use.
//
// What a key is: the renderer's own 64-bit identities, and nothing of the game's code or data. A program key is the hash
// the renderer knows a shader program by (Program::key, vk_shader.cpp); a state key is the hash it knows a graphics
// pipeline by (pipeline_key, vk_draw.cpp: what the pipeline notes hookshot_pipelines.bin keep, and what the precompile
// makes of each state list record). Both are recorded where the draw looks them up: once per key per map.
//
// On only with "Send performance statistics" (the same row, the same address) and not HL_TELEMETRY_ZONES=0. At most
// kMaxZones maps a session, each with at most kMaxPrograms / kMaxStates keys; a use count is kept per key bucketed (1,
// 2-9, 10+), and a use within kArrivalSeconds of the map's entry counts as the area's arrival. At the session's end the
// maps not queued before for this version and these stamps (hookshot_zones_sent.bin keeps their hashes, one u64 each)
// are written to zones_pending as JSON batches of at most kMaxBatchBytes, sent at the next start.
//
// Plain functions and data, no Vulkan: tests/test_zone_report.cpp checks them.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../host/hos_telemetry.h"
#include "perf_report.h"

namespace mkwvk::zone_report {

constexpr int kFormat = 1;                  // ("v" in the payload)
constexpr double kArrivalSeconds = 10;      // a map's arrival: the first seconds after its entry
constexpr size_t kMaxZones = 32;            // maps a session records at most (the rest is not recorded)
constexpr size_t kMaxPrograms = 4096;       // program keys per map
constexpr size_t kMaxStates = 8192;         // pipeline state keys per map
constexpr size_t kCountCap = 10;             // the top bucket's count: past it a key's count is not kept
constexpr size_t kMaxBatchBytes = 400 * 1024;   // one queued batch's body: a few hundred KB
constexpr size_t kMaxSentFile = 4u << 20;   // (the memory of what was sent: read to here)

constexpr const char* kSentFile = "hookshot_zones_sent.bin";

/// One key's use in one map: the order it was first used in, how much, and whether that was the area's arrival
struct ZoneUse {
    uint64_t key = 0;
    uint32_t first = 0;    // its first use's index within the visit it was first used in
    uint32_t count = 1;    // uses on this map this session, kept to kCountCap (the top bucket)
    bool arrival = false;  // its first use was within the arrival window
};

/// The keys of one kind (programs, states) one map used, in first-use order
struct ZoneKeys {
    std::unordered_map<uint64_t, size_t> at;   // key -> its ZoneUse's place in uses
    std::vector<ZoneUse> uses;
    size_t capped = 0;   // uses of new keys past the cap, not recorded
};

/// One map's session: what its draws first used
struct Zone {
    std::string name;
    int visits = 0;   // (times entered; the recorder counts the first entry too)
    uint32_t next_first_program = 0, next_first_state = 0;   // first-use indexes within the current visit
    ZoneKeys programs, states;
};

/// One use of a key on the map: the dedupe, the use count, the order and the cap. `next_first` is the current visit's
/// first-use index counter of this kind; `arrival` says this use is within the map's arrival window. False when the
/// key was known, or new but past `cap`
inline bool note_use(ZoneKeys& ks, uint64_t key, bool arrival, uint32_t& next_first, size_t cap) {
    if (const auto it = ks.at.find(key); it != ks.at.end()) {
        ZoneUse& u = ks.uses[it->second];
        if (u.count < kCountCap) u.count++;   // (the top bucket: past it nothing is counted)
        return false;
    }
    if (ks.uses.size() >= cap) {
        ks.capped++;
        return false;
    }
    ks.at.emplace(key, ks.uses.size());
    ks.uses.push_back({key, next_first++, 1, arrival});
    return true;
}

/// The use count's bucket: 0 one use, 1 two to nine, 2 ten and more
inline int count_bucket(uint32_t count) { return count <= 1 ? 0 : count < kCountCap ? 1 : 2; }

inline std::string hex16(uint64_t v) {
    char b[24];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return b;
}

/// What the payload says besides the maps
struct Meta {
    std::string version, driver;   // (as the performance summary's: perf_report::Machine)
    bool release = false;
    uint32_t vendor_id = 0;        // (the GPU's PCI vendor: 0x10DE, 0x1002, 0x8086)
    uint64_t state_stamp = 0, shader_stamp = 0;
};

/// One map's part of the body: its name, its visits, the arrival window the flag is against, and the keys, each
/// [hex key, first index, count bucket, arrival flag] in first-use order
inline std::string zone_json(const Zone& z) {
    namespace pr = perf_report;
    std::string o = "{\"name\":" + pr::json_text(z.name, 120) + ",\"visits\":" + std::to_string(z.visits) +
                    ",\"arrival_s\":" + pr::json_num(kArrivalSeconds) + ",\"programs\":[";
    bool comma = false;
    for (const ZoneUse& u : z.programs.uses) {
        o += std::string(comma ? "," : "") + "[\"" + hex16(u.key) + "\"," + std::to_string(u.first) + "," +
             std::to_string(count_bucket(u.count)) + "," + (u.arrival ? "1" : "0") + "]";
        comma = true;
    }
    o += "],\"states\":[";
    comma = false;
    for (const ZoneUse& u : z.states.uses) {
        o += std::string(comma ? "," : "") + "[\"" + hex16(u.key) + "\"," + std::to_string(u.first) + "," +
             std::to_string(count_bucket(u.count)) + "," + (u.arrival ? "1" : "0") + "]";
        comma = true;
    }
    return o + "]}";
}

/// The body posted to /telemetry/zones (format 1): the version, the release flag, the stamps as the pipelines batch
/// carries them, the GPU's vendor and the driver, then the maps in the order given. No path, no name, no other hardware
inline std::string payload_json(const Meta& m, const std::vector<std::string>& parts) {
    namespace pr = perf_report;
    std::string o = "{\"v\":" + std::to_string(kFormat) + ",\"version\":" + pr::json_text(m.version, 40) +
                    ",\"release\":" + (m.release ? "true" : "false") + ",\"state_stamp\":\"" + hex16(m.state_stamp) +
                    "\",\"shader_stamp\":\"" + hex16(m.shader_stamp) + "\",\"vendor_id\":" +
                    std::to_string(m.vendor_id) + ",\"driver\":" + pr::json_text(m.driver, 60) + ",\"maps\":[";
    bool comma = false;
    for (const std::string& p : parts) o += std::string(comma ? "," : "") + p, comma = true;
    return o + "]}";
}

/// hookshot_zones_sent.bin: "HKZSENT1" and the hashes (u64, little endian) of the maps already queued, each the hash of
/// the map's name with this version and these stamps. A file that is not one reads as empty
constexpr char kSentMagic[8] = {'H', 'K', 'Z', 'S', 'E', 'N', 'T', '1'};

inline uint64_t fnv_add(uint64_t h, const void* d, size_t n) {
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)d)[i]) * 1099511628211ull;
    return h;
}
/// What the keys mean this session: the version and the two stamps, hashed (a batch's identity besides its maps)
inline uint64_t stamp_seed(const std::string& version, uint64_t state_stamp, uint64_t shader_stamp) {
    uint64_t h = fnv_add(0xCBF29CE484222325ull, version.data(), version.size());
    h = fnv_add(h, &state_stamp, 8), h = fnv_add(h, &shader_stamp, 8);
    return h;
}
/// A map's entry in the sent file: its name with the seed. A map is queued once per version and stamps on a PC
inline uint64_t zone_sent_hash(uint64_t seed, const std::string& map) {
    return fnv_add(seed ^ 0x5A4F4E45ull, map.data(), map.size());
}

inline std::unordered_set<uint64_t> sent_decode(const std::string& bytes, bool* valid = nullptr) {
    std::unordered_set<uint64_t> out;
    const bool ok = bytes.size() >= 8 && !memcmp(bytes.data(), kSentMagic, 8);
    if (valid) *valid = ok;
    if (!ok) return out;
    const uint8_t* b = (const uint8_t*)bytes.data();
    for (size_t at = 8; at + 8 <= bytes.size(); at += 8) {
        uint64_t h = 0;
        for (int i = 0; i < 8; i++) h |= (uint64_t)b[at + i] << (8 * i);
        out.insert(h);
    }
    return out;
}
/// What to append to the sent file (the header too when it is new: `fresh`)
inline std::string sent_encode(const std::vector<uint64_t>& hashes, bool fresh) {
    std::string out;
    if (fresh) out.append(kSentMagic, 8);
    for (uint64_t h : hashes)
        for (int i = 0; i < 8; i++) out += (char)(h >> (8 * i) & 0xFF);
    return out;
}

/// The zones that have something on them and were not queued before for this seed, in first-visit order; *already is how
/// many were skipped as queued before. A zone with no key on it is neither queued nor remembered: it says nothing yet
inline std::vector<const Zone*> unsent_zones(const std::vector<const Zone*>& zones, uint64_t seed,
                                             const std::unordered_set<uint64_t>& sent, size_t* already = nullptr) {
    std::vector<const Zone*> out;
    size_t done = 0;
    for (const Zone* z : zones) {
        if (z->programs.uses.empty() && z->states.uses.empty()) continue;
        if (sent.count(zone_sent_hash(seed, z->name))) {
            done++;
            continue;
        }
        out.push_back(z);
    }
    if (already) *already = done;
    return out;
}

/// The batches: the zones in first-visit order, as many to a batch as fit in `max_bytes` (the head counted against each
/// map, to be safe). A zone alone over the cap is left out entirely (*over: how many), so the next session can try it when
/// its maps have fewer keys
inline std::vector<std::vector<const Zone*>> batch_groups(const std::vector<const Zone*>& zones, size_t max_bytes,
                                                          size_t* over = nullptr) {
    std::vector<std::vector<const Zone*>> out;
    size_t left = 0;
    std::vector<const Zone*> group;
    size_t group_bytes = 0;
    const size_t head = 160;
    for (const Zone* z : zones) {
        const size_t bytes = head + zone_json(*z).size();
        if (bytes > max_bytes) {
            left++;
            continue;
        }
        if (group_bytes && group_bytes + bytes > max_bytes) out.push_back(std::move(group)), group = {}, group_bytes = 0;
        group.push_back(z), group_bytes += bytes;
    }
    if (!group.empty()) out.push_back(std::move(group));
    if (over) *over = left;
    return out;
}

/// Why nothing is queued, "" when it may be: the row first (the opt-in), then the address, then the separate switch
inline const char* why_not(bool row_on, bool has_address, const char* env_switch) {
    if (!row_on) return "Send performance statistics is off";
    if (!has_address) return "this build has no telemetry address";
    if (!hos_telemetry_zones_on(env_switch)) return "HL_TELEMETRY_ZONES=0";
    return "";
}

}   // namespace mkwvk::zone_report
