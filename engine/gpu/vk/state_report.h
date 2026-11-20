// (heylisten) The pipeline state telemetry's rules (perf_report.cpp gathers the batch and writes it to pipelines_pending/, the host's
// sender, host/hos_crash_dialog.c, posts it to <the build's address>/telemetry/pipelines): what a player's game recorded in
// hookshot_pipeline_states.bin that the shipped list (hookshot_pipeline_states_shipped.bin, profile/pipeline_states.bin) does not
// have, so that the list a release ships can grow from what players meet.
//
// On only with "Send performance statistics" (the same row, the same address) and not HL_TELEMETRY_PIPELINES=0. A record is the
// state list's own (vk_draw.cpp StateRec, 528 bytes: hashes of the stages' code keys, formats and draw state, nothing of the game's
// code or data); it is checked here by the rules tools/release/check_states.py holds a shipped list to (every field in the range it
// can have, no name, no path), and one that fails is left out. At most kMaxRecords a session; the hashes of those queued are kept
// in hookshot_pipeline_states_sent.bin beside the list, so that no record is queued twice.
//
// Plain functions and data, no Vulkan: tests/test_state_report.cpp checks them.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "../../host/hos_telemetry.h"
#include "perf_report.h"

namespace mkwvk::state_report {

constexpr size_t kRecordSize = 528;    // (vk_draw.cpp StateRec, static_assert there; nx2/pipeline_states.py has the layout)
constexpr size_t kHeaderSize = 12;     // ("HKSTATE1", u32 record size)
constexpr char kListMagic[8] = {'H', 'K', 'S', 'T', 'A', 'T', 'E', '1'};
constexpr char kSentMagic[8] = {'H', 'K', 'S', 'E', 'N', 'T', '1', 0};
/// The records one session queues at most: 1800 of them are 0.95 MB, 1.27 MB as base64. More wait for the next session
constexpr size_t kMaxRecords = 1800;
constexpr size_t kMaxSentFile = 4u << 20;   // (the memory of what was sent: read to here)
constexpr int kFormat = 1;                  // ("v" in the payload)

constexpr const char* kListFile = "hookshot_pipeline_states.bin";
constexpr const char* kShippedFile = "hookshot_pipeline_states_shipped.bin";
constexpr const char* kSentFile = "hookshot_pipeline_states_sent.bin";

inline uint64_t record_hash(const uint8_t* r) {   // (vk_draw.cpp state_hash: FNV-1a over the record)
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < kRecordSize; i++) h = (h ^ r[i]) * 1099511628211ull;
    return h;
}

namespace detail {
inline uint32_t u32(const uint8_t* r, size_t at) {
    return (uint32_t)r[at] | (uint32_t)r[at + 1] << 8 | (uint32_t)r[at + 2] << 16 | (uint32_t)r[at + 3] << 24;
}
inline uint64_t u64(const uint8_t* r, size_t at) { return (uint64_t)u32(r, at) | (uint64_t)u32(r, at + 4) << 32; }
inline bool vk_format(uint32_t v) { return v <= 184 || (v >= 1000000000u && v < 1001000000u); }
inline bool blend_ok(uint64_t v) {   // (check_states.py blend_ok: NVN's factors and equations in a 37-bit field)
    if (v >> 37) return false;
    for (int s : {3, 10, 17, 24}) {
        const unsigned f = (unsigned)(v >> s & 127);
        if (!(f <= 0xB || (f >= 0x10 && f <= 0x13) || (f >= 0x61 && f <= 0x64))) return false;
    }
    return (v >> 31 & 7) <= 5 && (v >> 34 & 7) <= 5;
}
/// A name no list may hold (check_states.py NAMES), as ASCII or UTF-16, in any case
inline bool has_name(const uint8_t* r) {
    static const char* const names[] = {"zelda", "ocarina", "hyrule", "navi", "kokiri", "nintendo", "switch", "hookshot", "heylisten", "users", "appdata"};
    uint8_t low[kRecordSize];
    for (size_t i = 0; i < kRecordSize; i++) low[i] = (r[i] >= 'A' && r[i] <= 'Z') ? (uint8_t)(r[i] + 32) : r[i];
    for (const char* n : names) {
        const size_t len = strlen(n);
        for (size_t i = 0; i + len <= kRecordSize; i++)
            if (!memcmp(low + i, n, len)) return true;
        for (size_t i = 0; i + 2 * len <= kRecordSize; i++) {
            size_t k = 0;
            while (k < len && low[i + 2 * k] == (uint8_t)n[k] && low[i + 2 * k + 1] == 0) k++;
            if (k == len) return true;
        }
    }
    return false;
}
inline bool path_char(uint8_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '.' || c == '-';
}
/// A path (check_states.py PATH): a drive letter, a colon, a separator and three name characters; or two backslashes and three
inline bool has_path(const uint8_t* r) {
    for (size_t i = 0; i + 6 <= kRecordSize; i++) {
        const uint8_t c = r[i];
        if (((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) && r[i + 1] == ':' && (r[i + 2] == '\\' || r[i + 2] == '/') &&
            path_char(r[i + 3]) && path_char(r[i + 4]) && path_char(r[i + 5]))
            return true;
    }
    for (size_t i = 0; i + 5 <= kRecordSize; i++)
        if (r[i] == '\\' && r[i + 1] == '\\' && path_char(r[i + 2]) && path_char(r[i + 3]) && path_char(r[i + 4]) && r[i + 2] != ' ' &&
            r[i + 3] != ' ' && r[i + 4] != ' ')   // (a UNC name: no spaces)
            return true;
    return false;
}
}   // namespace detail

/// Whether a record is one a shipped list may hold (tools/release/check_states.py, field by field): each value is in the range it can
/// have, which leaves no room for an address or a host pointer, and no name or path is in it
inline bool record_ok(const uint8_t* r) {
    using namespace detail;
    if (!u64(r, 0)) return false;   // (no vertex or mesh stage)
    for (int k = 0; k < 8; k++)
        if (r[96 + k] > 15) return false;
    for (int k = 0; k < 8; k++)
        if (!blend_ok(u64(r, 104 + 8 * k))) return false;
    if (u32(r, 40) > 1 || u32(r, 44) > 10 || u32(r, 48) > 8) return false;   // (mesh, topology, targets)
    const uint32_t samples = u32(r, 52);
    if (!(samples >= 1 && samples <= 64 && !(samples & (samples - 1)))) return false;
    if (!vk_format(u32(r, 56)) || u32(r, 60) > 1) return false;   // (depth format, has depth)
    for (int k = 0; k < 8; k++)
        if (!vk_format(u32(r, 64 + 4 * k))) return false;
    if (u32(r, 232) > 16) return false;   // (attribute count)
    for (int k = 0; k < 16; k++)
        if (u32(r, 236 + 4 * k) > 0xFFFF || u32(r, 300 + 4 * k) > 0xFFFFF) return false;   // (strides, divisors)
    if (u32(r, 364) > 0xFF || u32(r, 372) > 0xFFFF || u32(r, 376) > 1 || u32(r, 380) > 1 || u32(r, 384) != 0) return false;
    const uint32_t nanswers = u32(r, 388), nquestions = u32(r, 392);
    if (nanswers > 24 || nquestions > 32) return false;
    for (int k = 0; k < 24; k++) {
        const uint32_t a = u32(r, 396 + 4 * k);
        if ((a >> 28) > 5 || ((a >> 9) & 7) != 0) return false;
        if ((unsigned)k >= nanswers && a) return false;   // (nothing past the answers it has)
    }
    for (size_t i = 492 + nquestions; i < 524; i++)
        if (r[i]) return false;
    for (size_t i = 524; i < kRecordSize; i++)
        if (r[i]) return false;
    return !has_name(r) && !has_path(r);
}

/// The records of a state list's bytes, as pointers into it (none when it is not a list: its magic and record size are checked;
/// a record cut short at the end is not one)
inline std::vector<const uint8_t*> list_records(const std::string& bytes) {
    std::vector<const uint8_t*> out;
    if (bytes.size() < kHeaderSize || memcmp(bytes.data(), kListMagic, 8)) return out;
    const uint8_t* b = (const uint8_t*)bytes.data();
    if (detail::u32(b, 8) != kRecordSize) return out;
    for (size_t at = kHeaderSize; at + kRecordSize <= bytes.size(); at += kRecordSize) out.push_back(b + at);
    return out;
}

/// The hashes of every record in a list (the shipped one: what the player's game already has the pipelines of)
inline std::unordered_set<uint64_t> list_hashes(const std::string& bytes) {
    std::unordered_set<uint64_t> out;
    for (const uint8_t* r : list_records(bytes)) out.insert(record_hash(r));
    return out;
}

/// hookshot_pipeline_states_sent.bin: "HKSENT1\0" and the hashes (u64, little endian) of the records already queued. A file that is
/// not one reads as empty
inline std::unordered_set<uint64_t> sent_decode(const std::string& bytes, bool* valid = nullptr) {
    std::unordered_set<uint64_t> out;
    const bool ok = bytes.size() >= 8 && !memcmp(bytes.data(), kSentMagic, 8);
    if (valid) *valid = ok;
    if (!ok) return out;
    for (size_t at = 8; at + 8 <= bytes.size(); at += 8) out.insert(detail::u64((const uint8_t*)bytes.data(), at));
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

/// What select_new found
struct Batch {
    std::string list;                 // (the records as a state list: header and records, the exact bytes the player's list holds)
    std::vector<uint64_t> hashes;     // (of each record in it)
    size_t count = 0;
    size_t in_shipped = 0, already_sent = 0, refused = 0, twice = 0, over_cap = 0;
};

/// The records of the player's list that are not in the shipped list, not queued before, pass record_ok and are not repeated: the
/// first `cap` of them in the list's order
inline Batch select_new(const std::string& player_list, const std::unordered_set<uint64_t>& shipped,
                        const std::unordered_set<uint64_t>& sent, size_t cap = kMaxRecords) {
    Batch b;
    std::unordered_set<uint64_t> seen;
    std::string body;
    for (const uint8_t* r : list_records(player_list)) {
        const uint64_t h = record_hash(r);
        if (shipped.count(h)) { b.in_shipped++; continue; }
        if (sent.count(h)) { b.already_sent++; continue; }
        if (!seen.insert(h).second) { b.twice++; continue; }
        if (!record_ok(r)) { b.refused++; continue; }
        if (b.count >= cap) { b.over_cap++; continue; }
        body.append((const char*)r, kRecordSize);
        b.hashes.push_back(h);
        b.count++;
    }
    if (b.count) {
        b.list.append(kListMagic, 8);
        for (int i = 0; i < 4; i++) b.list += (char)(kRecordSize >> (8 * i) & 0xFF);
        b.list += body;
    }
    return b;
}

inline std::string base64(const std::string& in) {
    static const char* const t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const uint32_t n = (uint32_t)(uint8_t)in[i] << 16 | (i + 1 < in.size() ? (uint32_t)(uint8_t)in[i + 1] << 8 : 0) |
                           (i + 2 < in.size() ? (uint32_t)(uint8_t)in[i + 2] : 0);
        out += t[n >> 18 & 63], out += t[n >> 12 & 63];
        out += i + 1 < in.size() ? t[n >> 6 & 63] : '=';
        out += i + 2 < in.size() ? t[n & 63] : '=';
    }
    return out;
}

/// What the payload says besides the records
struct Meta {
    std::string version, driver;   // (as the performance summary's: perf_report::Machine)
    bool release = false;
    uint32_t vendor_id = 0;        // (the GPU's PCI vendor: 0x10DE, 0x1002, 0x8086)
    uint64_t state_stamp = 0, shader_stamp = 0;
};

inline std::string hex16(uint64_t v) {
    char b[24];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return b;
}

/// The body posted to /telemetry/pipelines (format 1). `states` is the batch's list, base64: decoded it is a state list file, which
/// tools/release/check_states.py and nx2/pipeline_states.py read as they are. Release, the two stamps, the GPU's vendor and the
/// driver as the performance summary says it (the server derives the class); no path, name, id or other hardware
inline std::string payload_json(const Meta& m, const Batch& b) {
    namespace pr = mkwvk::perf_report;
    return "{\"v\":" + std::to_string(kFormat) + ",\"version\":" + pr::json_text(m.version, 40) + ",\"release\":" + (m.release ? "true" : "false") +
           ",\"state_stamp\":\"" + hex16(m.state_stamp) + "\",\"shader_stamp\":\"" + hex16(m.shader_stamp) +
           "\",\"vendor_id\":" + std::to_string(m.vendor_id) + ",\"driver\":" + pr::json_text(m.driver, 60) +
           ",\"record_size\":" + std::to_string(kRecordSize) + ",\"count\":" + std::to_string(b.count) + ",\"states\":\"" + base64(b.list) + "\"}";
}

/// Why nothing is queued, "" when it may be: the row first (the opt-in), then the address, then the separate switch
inline const char* why_not(bool row_on, bool has_address, const char* env_switch) {
    if (!row_on) return "Send performance statistics is off";
    if (!has_address) return "this build has no telemetry address";
    if (!hos_telemetry_pipelines_on(env_switch)) return "HL_TELEMETRY_PIPELINES=0";
    return "";
}

}   // namespace mkwvk::state_report
