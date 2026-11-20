// tests/test_state_report.py: gpu/vk/state_report.h, the pipeline state telemetry's rules: which records are new, the dedupe and what was
// sent before, the cap, the gating, the payload. Without arguments it runs its own checks; "verdict <file of records>" prints record_ok
// of each 528 bytes (the python test compares them with tools/release/check_states.py), "payload <player list> <shipped list> <sent
// file or -> <cap>" prints the body that would be posted.
#include "../gpu/vk/state_report.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

namespace sr = mkwvk::state_report;

using Rec = std::string;   // (528 bytes)

static void put32(Rec& r, size_t at, uint32_t v) { for (int i = 0; i < 4; i++) r[at + i] = (char)(v >> (8 * i) & 0xFF); }
static void put64(Rec& r, size_t at, uint64_t v) { for (int i = 0; i < 8; i++) r[at + i] = (char)(v >> (8 * i) & 0xFF); }

/// A record a shipped list may hold: a pipeline of one colour target, one sample, a depth target, no texture answers
static Rec good(uint64_t id) {
    Rec r(sr::kRecordSize, '\0');
    put64(r, 0, 0x1000 + id), put64(r, 8, 0x77), put64(r, 32, id);   // (code[0], code[1], variant)
    put32(r, 44, 3), put32(r, 48, 1), put32(r, 52, 1), put32(r, 56, 126), put32(r, 60, 1), put32(r, 64, 37);   // (topology, targets, samples, depth, has depth, colour 0)
    return r;
}
static bool ok(const Rec& r) { return sr::record_ok((const uint8_t*)r.data()); }
static std::string list_of(const std::vector<Rec>& recs, size_t size = sr::kRecordSize) {
    std::string s(sr::kListMagic, 8);
    for (int i = 0; i < 4; i++) s += (char)(size >> (8 * i) & 0xFF);
    for (const Rec& r : recs) s += r;
    return s;
}
static std::string unbase64(const std::string& in) {
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (v < 0) continue;
        acc = acc << 6 | (uint32_t)v, bits += 6;
        if (bits >= 8) bits -= 8, out += (char)(acc >> bits & 0xFF);
    }
    return out;
}
static std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
    if (argc >= 3 && !strcmp(argv[1], "verdict")) {
        const std::string data = slurp(argv[2]);
        for (size_t at = 0; at + sr::kRecordSize <= data.size(); at += sr::kRecordSize) std::printf("%d\n", sr::record_ok((const uint8_t*)data.data() + at) ? 1 : 0);
        return 0;
    }
    if (argc >= 6 && !strcmp(argv[1], "payload")) {
        bool valid = false;
        const auto sent = sr::sent_decode(strcmp(argv[4], "-") ? slurp(argv[4]) : std::string(), &valid);
        const sr::Batch b = sr::select_new(slurp(argv[2]), sr::list_hashes(slurp(argv[3])), sent, (size_t)atoi(argv[5]));
        sr::Meta m;
        m.version = "v0.4.0", m.release = true, m.driver = "NVIDIA 566.36", m.vendor_id = 0x10DE, m.state_stamp = 0x0123456789abcdefull, m.shader_stamp = 0xfedcba9876543210ull;
        std::printf("%s", sr::payload_json(m, b).c_str());
        return 0;
    }

    // the format: the layout the shipped list has, and the hash vk_draw.cpp's state_hash makes
    CHECK("record size", sr::kRecordSize == 528 && sr::kHeaderSize == 12);
    {
        Rec zero(sr::kRecordSize, '\0');
        CHECK("FNV-1a over the record", sr::record_hash((const uint8_t*)zero.data()) != 0 && sr::record_hash((const uint8_t*)good(1).data()) != sr::record_hash((const uint8_t*)good(2).data()));
    }

    // a record a list may hold, and each way it may not be
    CHECK("a good record", ok(good(1)));
    {
        struct Bad { const char* what; size_t at; uint64_t v; int width; };
        const Bad bad[] = {
            {"no vertex stage", 0, 0, 8}, {"colour type 16", 96, 16, 1}, {"a blend with bit 37", 104, 1ull << 37, 8}, {"a blend factor NVN does not have", 104, 0x0Cull << 3, 8},
            {"a blend equation NVN does not have", 104, 6ull << 31, 8}, {"mesh 2", 40, 2, 4}, {"topology 11", 44, 11, 4}, {"9 targets", 48, 9, 4}, {"3 samples", 52, 3, 4},
            {"0 samples", 52, 0, 4}, {"128 samples", 52, 128, 4}, {"depth format 185", 56, 185, 4}, {"has depth 2", 60, 2, 4}, {"colour format 185", 68, 185, 4},
            {"17 attributes", 232, 17, 4}, {"a stride over 16 bits", 236, 0x10000, 4}, {"a divisor over 20 bits", 300, 0x100000, 4}, {"blend enables over 8 bits", 364, 0x100, 4},
            {"polygon mode over 16 bits", 372, 0x10000, 4}, {"depth clamp 2", 376, 2, 4}, {"alpha to coverage 2", 380, 2, 4}, {"padding", 384, 1, 4},
            {"25 answers", 388, 25, 4}, {"33 questions", 392, 33, 4}, {"an answer past its count", 396, 1, 4}, {"an answer with stage 6", 396, 6u << 28, 4},
            {"a question past its count", 492, 1, 1}, {"the tail padding", 524, 1, 1},
        };
        for (const Bad& b : bad) {
            Rec r = good(3);
            if (b.width == 8) put64(r, b.at, b.v);
            else if (b.width == 4) put32(r, b.at, (uint32_t)b.v);
            else r[b.at] = (char)b.v;
            if (!std::strcmp(b.what, "no vertex stage")) put64(r, 0, 0);
            CHECK(b.what, !ok(r));
        }
        Rec r = good(4);
        put32(r, 388, 2), put32(r, 396, 5u << 28 | 3u << 12), put32(r, 400, 1u << 8), put32(r, 392, 2), r[492] = 1, r[493] = 2;
        CHECK("answers and questions within their counts", ok(r));
    }
    // no name and no path: the shipped list's own rules (a path or the user's name would be in a hash by chance only)
    {
        Rec r = good(5);
        memcpy(&r[200], "Zelda", 5);
        CHECK("a name", !ok(r));
        r = good(5);
        const char* wide = "n\0a\0v\0i\0";
        memcpy(&r[210], wide, 8);
        CHECK("a name as UTF-16", !ok(r));
        r = good(5);
        memcpy(&r[220], "C:\\Users\\x", 10);
        CHECK("a Windows path", !ok(r));
        r = good(5);
        memcpy(&r[220], "d:/abc", 6);
        CHECK("a path with a slash", !ok(r));
        r = good(5);
        memcpy(&r[220], "\\\\srv", 5);
        CHECK("a network path", !ok(r));
        r = good(5);
        memcpy(&r[220], "\\\\a b", 5);
        CHECK("two backslashes and a space are no path", ok(r));
        r = good(5);
        memcpy(&r[220], "c:\\ab", 5);
        CHECK("a drive and two name characters are no path", ok(r));
    }

    // the list: what a list is
    {
        const Rec a = good(1), b = good(2);
        CHECK("a list's records", sr::list_records(list_of({a, b})).size() == 2);
        CHECK("not a list: no magic", sr::list_records("HKSTATE2\x10\x02\0\0" + a).empty());
        CHECK("not a list: another record size", sr::list_records(list_of({a}, 520)).empty());
        CHECK("not a list: too short", sr::list_records("HKSTATE1").empty() && sr::list_records("").empty());
        CHECK("a record cut short is not one", sr::list_records(list_of({a, b}).substr(0, sr::kHeaderSize + 700)).size() == 1);
    }

    // the new records: not in the shipped list, not sent before, once each, fit to send, the first `cap`
    {
        std::vector<Rec> player;
        for (uint64_t i = 1; i <= 6; i++) player.push_back(good(i));
        player.push_back(good(3));   // (twice)
        Rec bad = good(7);
        put32(bad, 384, 9);
        player.push_back(bad);
        const std::string list = list_of(player);
        const auto shipped = sr::list_hashes(list_of({good(1), good(2)}));
        const auto none = std::unordered_set<uint64_t>();
        CHECK("the shipped hashes", shipped.size() == 2 && shipped.count(sr::record_hash((const uint8_t*)good(1).data())));
        sr::Batch b = sr::select_new(list, shipped, none);
        CHECK("the shipped list's records are left out", b.in_shipped == 2 && b.count == 4);
        CHECK("a record twice is once", b.twice == 1);
        CHECK("a record that fails the rules is left out", b.refused == 1);
        CHECK("in the player's order", b.list == list_of({good(3), good(4), good(5), good(6)}));
        CHECK("a hash for each", b.hashes.size() == 4 && b.hashes[0] == sr::record_hash((const uint8_t*)good(3).data()));
        // what was sent before
        const std::vector<uint64_t> done = {b.hashes[0], b.hashes[1]};
        const auto sent = sr::sent_decode(sr::sent_encode(done, true));
        CHECK("the sent file round-trips", sent.size() == 2 && sent.count(done[0]) && sent.count(done[1]));
        sr::Batch again = sr::select_new(list, shipped, sent);
        CHECK("what was sent is not queued again", again.already_sent == 3 /* (the repeat of one of them too) */ && again.count == 2 && again.list == list_of({good(5), good(6)}));
        std::unordered_set<uint64_t> all = sent;
        for (uint64_t h : again.hashes) all.insert(h);
        CHECK("then nothing is left", sr::select_new(list, shipped, all).count == 0 && sr::select_new(list, shipped, all).list.empty());
        // the cap
        sr::Batch capped = sr::select_new(list, shipped, none, 3);
        CHECK("at most the cap", capped.count == 3 && capped.over_cap == 1 && capped.list == list_of({good(3), good(4), good(5)}));
        CHECK("the ones over the cap wait for the next session", sr::select_new(list, shipped, sr::sent_decode(sr::sent_encode(capped.hashes, true)), 3).list == list_of({good(6)}));
        std::vector<Rec> many;
        for (uint64_t i = 1; i <= sr::kMaxRecords + 200; i++) many.push_back(good(i));
        const sr::Batch big = sr::select_new(list_of(many), none, none);
        CHECK("the session's cap: 1800 records, under 1 MB", big.count == sr::kMaxRecords && big.over_cap == 200 && big.list.size() < 1000000 && sr::kMaxRecords <= 2000);
        CHECK("nothing in the shipped list is new", sr::select_new(list_of({good(1)}), sr::list_hashes(list_of({good(1)})), none).count == 0);
        CHECK("no list, nothing", sr::select_new("", shipped, none).count == 0 && sr::select_new("garbage", shipped, none).count == 0);
    }

    // what is remembered
    {
        bool valid = true;
        CHECK("no file: nothing sent, not valid", sr::sent_decode("", &valid).empty() && !valid);
        CHECK("not a sent file: empty", sr::sent_decode("HKSTATE1xxxxxxxx", &valid).empty() && !valid);
        const std::string first = sr::sent_encode({1, 2}, true), more = sr::sent_encode({3}, false);
        CHECK("a new file has its header, an appended batch has none", first.size() == 8 + 16 && more.size() == 8);
        CHECK("appended", sr::sent_decode(first + more, &valid).size() == 3 && valid);
        CHECK("a hash cut short is not read", sr::sent_decode(first + "abc").size() == 2);
        CHECK("little endian", first[8] == 1 && first[9] == 0 && first[16] == 2);
    }

    // the encoding
    CHECK("base64", sr::base64("") == "" && sr::base64("f") == "Zg==" && sr::base64("fo") == "Zm8=" && sr::base64("foo") == "Zm9v" && sr::base64("foobar") == "Zm9vYmFy");
    CHECK("base64 of every byte", unbase64(sr::base64(std::string("\x00\xff\x10\x80\x7f", 5))) == std::string("\x00\xff\x10\x80\x7f", 5));

    // the payload: the release, the stamps, the GPU's vendor, the driver and the records, nothing else
    {
        sr::Batch b = sr::select_new(list_of({good(1), good(2)}), {}, {});
        sr::Meta m;
        m.version = "v0.4.0", m.release = true, m.driver = "NVIDIA 566.36", m.vendor_id = 0x10DE, m.state_stamp = 0x0123456789abcdefull, m.shader_stamp = 0xfedcba9876543210ull;
        const std::string p = sr::payload_json(m, b);
        const std::string head = "{\"v\":1,\"version\":\"v0.4.0\",\"release\":true,\"state_stamp\":\"0123456789abcdef\",\"shader_stamp\":\"fedcba9876543210\","
                                 "\"vendor_id\":4318,\"driver\":\"NVIDIA 566.36\",\"record_size\":528,\"count\":2,\"states\":\"";
        CHECK("the payload's fields, in order", p.compare(0, head.size(), head) == 0 && p.substr(p.size() - 2) == "\"}");
        CHECK("the records are the list as it is stored, base64", unbase64(p.substr(head.size(), p.size() - head.size() - 2)) == list_of({good(1), good(2)}));
        m.version = "v\"1\\x\n" + std::string(100, 'a'), m.driver = std::string(200, 'd');
        const std::string q = sr::payload_json(m, b);
        CHECK("a version and a driver are cut at the server's limits and escaped", q.find("\"version\":\"v\\\"1\\\\x ") != std::string::npos && q.find(std::string(61, 'd')) == std::string::npos);
    }

    // the gating: the row, an address, the separate switch
    {
        CHECK("sends with the row, an address and the switch unset", !*sr::why_not(true, true, nullptr));
        CHECK("the switch set to 1 or empty does not turn it off", !*sr::why_not(true, true, "1") && !*sr::why_not(true, true, ""));
        CHECK("the row off: nothing", *sr::why_not(false, true, nullptr) && std::string(sr::why_not(false, true, nullptr)).find("off") != std::string::npos);
        CHECK("no address: nothing", *sr::why_not(true, false, nullptr));
        CHECK("HL_TELEMETRY_PIPELINES=0: nothing", std::string(sr::why_not(true, true, "0")) == "HL_TELEMETRY_PIPELINES=0");
        CHECK("the row decides first", std::string(sr::why_not(false, false, "0")).find("Send performance statistics") == 0);
        CHECK("00 is not 0", !*sr::why_not(true, true, "00") && hos_telemetry_pipelines_on("off") && !hos_telemetry_pipelines_on("0"));
    }
    std::printf("ok\n");
    return 0;
}
