// tests/test_zone_report.py: gpu/vk/zone_report.h, the per-zone shader and pipeline usage's rules: the recorder (the
// dedupe per map, the first-use order within a visit, the caps, the count buckets, the arrival flag), the sent file
// (hookshot_zones_sent.bin), the batch JSON against the documented schema and the gating. Without arguments it runs its
// own checks; "payload <file>" prints the body that would be posted, its zones read from the file: "map <name> <visits>",
// then "program|state <16 hex digits> <first> <count> <arrival>" lines in first-use order.
#include "../gpu/vk/zone_report.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

namespace zr = mkwvk::zone_report;

static uint64_t key1(uint64_t n) { return 0x1000000000000000ull + n; }

int main(int argc, char** argv) {
    if (argc >= 3 && !strcmp(argv[1], "payload")) {
        std::ifstream f(argv[2]);
        zr::Meta m;
        m.version = "v0.4.0", m.release = true, m.driver = "NVIDIA 566.36", m.vendor_id = 0x10DE;
        m.state_stamp = 0x0123456789abcdefull, m.shader_stamp = 0xfedcba9876543210ull;
        std::vector<std::string> parts;
        std::vector<std::unique_ptr<zr::Zone>> zones;
        std::string word;
        while (f >> word) {
            if (word == "map") {
                std::string name;
                int visits = 1;
                f >> name >> visits;
                zones.push_back(std::make_unique<zr::Zone>());
                zones.back()->name = name, zones.back()->visits = visits;
            } else {
                std::string hex;
                uint64_t first = 0, count = 1, arrival = 0;
                f >> hex >> first >> count >> arrival;
                CHECK("a key is 16 hex digits", hex.size() == 16);
                const uint64_t k = std::stoull(hex, nullptr, 16);
                CHECK("a place to note it", !zones.empty());
                const bool program = word == "program";
                zr::Zone& z = *zones.back();
                zr::ZoneKeys& ks = program ? z.programs : z.states;
                uint32_t& next = program ? z.next_first_program : z.next_first_state;
                const size_t cap = program ? zr::kMaxPrograms : zr::kMaxStates;
                for (uint64_t i = 0; i < count; i++) zr::note_use(ks, k, arrival != 0, next, cap);
                CHECK("the place it was noted", ks.uses.back().first == first);
            }
        }
        for (const auto& z : zones) parts.push_back(zr::zone_json(*z));
        std::printf("%s", zr::payload_json(m, parts).c_str());
        return 0;
    }

    // the recorder: once per key per map, the first-use order within a visit, the count and its cap, the arrival flag
    {
        zr::Zone z;
        z.name = "Kokiri Forest (child)";
        CHECK("the first use is new", zr::note_use(z.programs, key1(1), true, z.next_first_program, zr::kMaxPrograms));
        CHECK("the first use's index", z.programs.uses.size() == 1 && z.programs.uses[0].key == key1(1) &&
              z.programs.uses[0].first == 0 && z.programs.uses[0].count == 1 && z.programs.uses[0].arrival);
        CHECK("the same key is not new", !zr::note_use(z.programs, key1(1), false, z.next_first_program, zr::kMaxPrograms));
        CHECK("the second use counts", z.programs.uses[0].count == 2);
        CHECK("the arrival flag is of the first use", z.programs.uses[0].arrival);
        for (int i = 0; i < 20; i++) zr::note_use(z.programs, key1(1), false, z.next_first_program, zr::kMaxPrograms);
        CHECK("the count is kept to the top bucket's", z.programs.uses[0].count == zr::kCountCap);
        CHECK("another key, the next index", zr::note_use(z.programs, key1(2), false, z.next_first_program, zr::kMaxPrograms) &&
              z.programs.uses[1].first == 1 && !z.programs.uses[1].arrival);
        CHECK("the other kind apart", zr::note_use(z.states, key1(1), true, z.next_first_state, zr::kMaxStates) &&
              z.states.uses[0].first == 0);
        // a new visit: the indexes restart, a key's first use is of the visit it was first used in, the count goes on
        z.visits = 2, z.next_first_program = z.next_first_state = 0;
        CHECK("a key of the visit before is not new", !zr::note_use(z.programs, key1(2), true, z.next_first_program, zr::kMaxPrograms));
        CHECK("the arrival flag of its own first use stands, and its count grows", z.programs.uses[1].count == 2 && !z.programs.uses[1].arrival);
        zr::note_use(z.programs, key1(3), true, z.next_first_program, zr::kMaxPrograms);
        CHECK("a new key of the new visit has the visit's own index", z.programs.uses[2].first == 0 && z.programs.uses[2].arrival);
        // the caps
        zr::ZoneKeys small;
        uint32_t next = 0;
        for (uint64_t i = 0; i < 3; i++) zr::note_use(small, key1(i), false, next, 2);
        CHECK("at most the cap, the rest not recorded", small.uses.size() == 2 && small.capped >= 1);
        zr::ZoneKeys big;
        next = 0;
        for (uint64_t i = 0; i < zr::kMaxPrograms + 50; i++) zr::note_use(big, i + 1, false, next, zr::kMaxPrograms);
        CHECK("the map's cap", big.uses.size() == zr::kMaxPrograms && big.capped >= 50);
        CHECK("the buckets", zr::count_bucket(1) == 0 && zr::count_bucket(2) == 1 && zr::count_bucket(9) == 1 &&
              zr::count_bucket(zr::kCountCap) == 2 && zr::count_bucket(zr::kCountCap + 1) == 2);
        CHECK("a batch holds a map at both caps", zr::kMaxPrograms * 30 + zr::kMaxStates * 30 + 100 < zr::kMaxBatchBytes);
    }

    // the encoding: one map's part, exactly the documented schema
    {
        zr::Zone z;
        z.name = "Hyrule Field \"(adult)\"", z.visits = 3;
        zr::note_use(z.programs, 0x0011223344556677ull, true, z.next_first_program, zr::kMaxPrograms);
        zr::note_use(z.programs, 0x0011223344556677ull, true, z.next_first_program, zr::kMaxPrograms);
        zr::note_use(z.states, 0x8899aabbccddeeffull, false, z.next_first_state, zr::kMaxStates);
        for (size_t i = 0; i < zr::kCountCap; i++) zr::note_use(z.states, 0x8899aabbccddeeffull, false, z.next_first_state, zr::kMaxStates);
        const std::string j = zr::zone_json(z);
        const std::string want = "{\"name\":\"Hyrule Field \\\"(adult)\\\"\",\"visits\":3,\"arrival_s\":10,"
                                "\"programs\":[[\"0011223344556677\",0,1,1]],"
                                "\"states\":[[\"8899aabbccddeeff\",0,2,0]]}";
        CHECK("one map's part, the keys in first-use order", j == want);
        zr::Zone none;
        none.name = "Title and intro";
        CHECK("a map with nothing on it", zr::zone_json(none) ==
              "{\"name\":\"Title and intro\",\"visits\":0,\"arrival_s\":10,\"programs\":[],\"states\":[]}");
        CHECK("hex16", zr::hex16(0) == "0000000000000000" && zr::hex16(0x0123456789abcdefull) == "0123456789abcdef");
    }

    // the payload: the head's fields in the documented order, then the maps
    {
        zr::Meta m;
        m.version = "v0.4.0", m.release = true, m.driver = "NVIDIA 566.36", m.vendor_id = 0x10DE;
        m.state_stamp = 0x0123456789abcdefull, m.shader_stamp = 0xfedcba9876543210ull;
        const std::string p = zr::payload_json(m, {zr::zone_json(zr::Zone()), "{\"x\":1}"});
        const std::string head = "{\"v\":1,\"version\":\"v0.4.0\",\"release\":true,\"state_stamp\":\"0123456789abcdef\","
                                 "\"shader_stamp\":\"fedcba9876543210\",\"vendor_id\":4318,\"driver\":\"NVIDIA 566.36\",\"maps\":[";
        CHECK("the head's fields, in order", !p.compare(0, head.size(), head) && p.substr(p.size() - 3) == "}]}");
        CHECK("the maps joined with commas", p.substr(head.size(), p.size() - head.size() - 2).find(",{\"x\":1}") != std::string::npos);
        CHECK("no maps is an empty list", zr::payload_json(m, {}).substr(zr::payload_json(m, {}).size() - 10) == "\"maps\":[]}");
        m.version = "v\"1\\x\n" + std::string(100, 'a'), m.driver = std::string(200, 'd'), m.release = false;
        const std::string q = zr::payload_json(m, {});
        CHECK("the version and the driver are cut at the server's limits and escaped",
              q.find("\"version\":\"v\\\"1\\\\x ") != std::string::npos && q.find(std::string(61, 'd')) == std::string::npos &&
              q.find("\"release\":false") != std::string::npos);
    }

    // what was sent before: the seed of the version and the stamps, the map's hash with it
    {
        const uint64_t s1 = zr::stamp_seed("v0.4.0", 1, 2);
        CHECK("the seed", s1 != zr::stamp_seed("v0.4.1", 1, 2) && s1 != zr::stamp_seed("v0.4.0", 2, 2) &&
              s1 != zr::stamp_seed("v0.4.0", 1, 3) && s1 == zr::stamp_seed("v0.4.0", 1, 2));
        CHECK("a map's hash", zr::zone_sent_hash(s1, "Kokiri Forest (child)") != zr::zone_sent_hash(s1, "Kokiri Forest") &&
              zr::zone_sent_hash(s1, "Kokiri Forest (child)") != zr::zone_sent_hash(zr::stamp_seed("v0.4.1", 1, 2), "Kokiri Forest (child)"));
        bool valid = true;
        CHECK("no file: nothing sent, not valid", zr::sent_decode("", &valid).empty() && !valid);
        CHECK("another file's magic: empty", zr::sent_decode("HKSENT1xxxxxxxx", &valid).empty() && !valid);
        const std::string first = zr::sent_encode({1, 2}, true), more = zr::sent_encode({3}, false);
        CHECK("a new file has its header, an appended batch none", first.size() == 8 + 16 && more.size() == 8);
        CHECK("appended", zr::sent_decode(first + more, &valid).size() == 3 && valid);
        CHECK("a hash cut short is not read", zr::sent_decode(first + "abc").size() == 2);
        CHECK("little endian", (unsigned char)first[8] == 1 && (unsigned char)first[16] == 2);
    }

    // the zones to queue: not sent before, with something on them, in first-visit order
    {
        zr::Zone a, b, empty, sent_one;
        a.name = "Kokiri Forest (child)", b.name = "Hyrule Field", empty.name = "Title and intro", sent_one.name = "Market";
        uint32_t next = 0;
        zr::note_use(a.programs, key1(1), true, next, zr::kMaxPrograms);
        zr::note_use(b.states, key1(2), true, next = 0, zr::kMaxStates);
        zr::note_use(sent_one.programs, key1(3), true, next = 0, zr::kMaxPrograms);
        const std::vector<const zr::Zone*> all = {&a, &sent_one, &empty, &b};
        const uint64_t seed = zr::stamp_seed("v0.4.0", 1, 2);
        std::unordered_set<uint64_t> sent = {zr::zone_sent_hash(seed, "Market")};
        size_t already = 99;
        const std::vector<const zr::Zone*> fresh = zr::unsent_zones(all, seed, sent, &already);
        CHECK("not sent before, and only maps with something on them", fresh.size() == 2 && fresh[0] == &a && fresh[1] == &b && already == 1);
        sent.insert(zr::zone_sent_hash(seed, "Kokiri Forest (child)")), sent.insert(zr::zone_sent_hash(seed, "Hyrule Field"));
        CHECK("then nothing is left, and an empty map is never counted as sent", zr::unsent_zones(all, seed, sent, &already).empty() && already == 3);
    }

    // the batches: as many maps as fit a batch, a map alone over the cap left for the next session
    {
        zr::Zone a, b;
        a.name = "a", b.name = "b";
        uint32_t next = 0;
        zr::note_use(a.programs, key1(1), false, next, zr::kMaxPrograms);
        zr::note_use(b.states, key1(2), false, next = 0, zr::kMaxStates);
        std::vector<const zr::Zone*> zones;
        for (int i = 0; i < 40; i++) zones.push_back(i % 2 ? &b : &a);
        size_t over = 99;
        std::vector<std::vector<const zr::Zone*>> groups = zr::batch_groups(zones, 900, &over);
        CHECK("as many maps as fit, in first-visit order", groups.size() >= 2 && over == 0);
        size_t total = 0;
        for (const auto& g : groups) total += g.size();
        CHECK("every one of them queued", total == 40);
        for (size_t i = 1; i < groups.size(); i++) CHECK("a group is never empty", !groups[i].empty());
        zr::Zone huge;
        huge.name = "h";
        for (uint64_t i = 0; i < 400; i++) zr::note_use(huge.programs, i, false, next = 0, zr::kMaxPrograms);
        over = 99;
        const std::vector<std::vector<const zr::Zone*>> none = zr::batch_groups({&huge}, 128, &over);
        CHECK("a map alone over the cap is left for the next session", none.empty() && over == 1);
    }

    // the gating: the row, the address, the separate switch
    {
        CHECK("sends with the row, an address and the switch unset", !*zr::why_not(true, true, nullptr));
        CHECK("the switch set to 1 or empty does not turn it off", !*zr::why_not(true, true, "1") && !*zr::why_not(true, true, ""));
        CHECK("the row off: nothing", *zr::why_not(false, true, nullptr) && std::string(zr::why_not(false, true, nullptr)).find("off") != std::string::npos);
        CHECK("no address: nothing", *zr::why_not(true, false, nullptr));
        CHECK("HL_TELEMETRY_ZONES=0: nothing", std::string(zr::why_not(true, true, "0")) == "HL_TELEMETRY_ZONES=0");
        CHECK("the row decides first", std::string(zr::why_not(false, false, "0")).find("Send performance statistics") == 0);
        CHECK("00 is not 0, and a word is not either", !*zr::why_not(true, true, "00") && hos_telemetry_zones_on("off") && !hos_telemetry_zones_on("0"));
    }
    std::printf("ok\n");
    return 0;
}
