// tests/test_profile_report.py: gpu/vk/profile_report.h, the detailed performance profile's rules: the module classifier on fake
// ranges, the thread roles, the hitch tagging of samples, the tables' caps, the payload's JSON and its batches, the gating.
// Without arguments it runs its own checks; "payload" prints a small body, "caps" one with every table over its cap (the python
// test reads both against the documented schema).
#include "../gpu/vk/profile_report.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

namespace pf = mkwvk::profile_report;

static pf::Meta meta() {
    pf::Meta m;
    m.version = "v0.4.3", m.release = true, m.build_id = 0x0123456789abcdefull, m.vendor_id = 0x10DE, m.driver = "NVIDIA 566.36";
    m.gpu = "NVIDIA GeForce RTX 5070", m.cpu = "AMD Ryzen 7 7800X3D", m.os = "Windows 11";
    return m;
}

static pf::MapProfile busy_map(const char* name, int guest_keys, int engine_keys, int passes) {
    pf::MapProfile m;
    m.name = name, m.seconds = 312.5, m.frames = 18000, m.hitch_frames = 12, m.frame_ms_sum = 1234.5, m.gpu = true;
    for (int r = 0; r < pf::kRoles; r++)
        for (int i = 0; i < 20 + r; i++) m.add_cpu(r, (i + r) % pf::kModules, 100 + r * 10 + i % 3, i % 7 == 0);
    for (int i = 0; i < guest_keys; i++) m.guest.add(0x7100000000ull + (uint64_t)i * 4, i % 5 == 0, 1 + i % 9);
    for (int i = 0; i < engine_keys; i++) m.engine.add(pf::engine_key(0x1000 + (uint64_t)i * 16, i % 4 == 0), i % 6 == 0, 1 + i % 11);
    for (int i = 0; i < passes; i++) m.add_pass(i % pf::kPassKinds, 0x9000 + (uint64_t)i, 0.5 + i * 0.01, i % 3 == 0);
    return m;
}

int main(int argc, char** argv) {
    if (argc >= 2 && !strcmp(argv[1], "payload")) {
        pf::MapProfile a = busy_map("Kokiri Forest (child)", 10, 8, 5), b;
        b.name = "Hyrule Field", b.seconds = 60, b.frames = 3600;   // (a map with frames only: no cpu, no gpu block)
        std::printf("%s", pf::payload_json(meta(), {pf::map_json(a), pf::map_json(b)}).c_str());
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "caps")) {
        std::vector<std::string> parts;
        for (int i = 0; i < 4; i++) parts.push_back(pf::map_json(busy_map(("Map " + std::to_string(i)).c_str(), 3000, 6000, 700)));
        std::printf("%s", pf::payload_json(meta(), parts).c_str());
        return 0;
    }

    // the module buckets of a module's file name
    CHECK("ntdll is the OS", pf::module_bucket("ntdll.dll", false) == pf::kOs);
    CHECK("any case", pf::module_bucket("KERNELBASE.DLL", false) == pf::kOs && pf::module_bucket("Win32u.dll", false) == pf::kOs);
    CHECK("the Vulkan loader and NVIDIA's ICD are the driver", pf::module_bucket("vulkan-1.dll", false) == pf::kDriver &&
          pf::module_bucket("nvoglv64.dll", false) == pf::kDriver && pf::module_bucket("amdvlk64.dll", false) == pf::kDriver);
    CHECK("a module that exports the ICD entry is the driver whatever its name", pf::module_bucket("whatever.dll", true) == pf::kDriver);
    CHECK("the C++ runtime", pf::module_bucket("libc++.dll", false) == pf::kRuntime && pf::module_bucket("msvcp140.dll", false) == pf::kRuntime &&
          pf::module_bucket("ucrtbase.dll", false) == pf::kRuntime);
    CHECK("another DLL is the runtime", pf::module_bucket("SDL3.dll", false) == pf::kRuntime);
    CHECK("the OS list is whole names, not prefixes", pf::module_bucket("kernel32x.dll", false) == pf::kRuntime);

    // the engine functions by name
    CHECK("the native engine's namespace", pf::engine_bucket("hl::jobs::JobSystem::run") == pf::kNativeEngine &&
          pf::engine_bucket("hl_math_apply") == pf::kNativeEngine);
    CHECK("audio", pf::engine_bucket("nn_audio_render") == pf::kAudio && pf::engine_bucket("opus_decode") == pf::kAudio);
    CHECK("the rest of the executable is the host", pf::engine_bucket("hos_svc_dispatch") == pf::kHost && pf::engine_bucket("") == pf::kHost);

    // classifier on fake ranges: sorted by lo, a hit inside, none outside or between
    std::vector<pf::Range> ranges;
    ranges.push_back({0x1000, 0x2000, pf::kHost, true, false});
    ranges.push_back({0x5000, 0x6000, pf::kRenderer, false, true});
    ranges.push_back({0x6000, 0x7000, pf::kOs, false, false});
    CHECK("inside the first", pf::range_of(ranges, 0x1000) && pf::range_of(ranges, 0x1fff)->exe);
    CHECK("the end is outside", !pf::range_of(ranges, 0x2000));
    CHECK("between", !pf::range_of(ranges, 0x3000));
    CHECK("below all", !pf::range_of(ranges, 0x10));
    CHECK("the renderer", pf::range_of(ranges, 0x5800) && pf::range_of(ranges, 0x5800)->vk);
    CHECK("the next module starts where the last ends", pf::range_of(ranges, 0x6000)->bucket == pf::kOs);
    CHECK("above all", !pf::range_of(ranges, 0x9000));

    // thread roles by name
    CHECK("the replay thread renders", pf::role_of_name("mkw_vk replay") == pf::kRoleRender && pf::role_of_name("mkw_vk replay worker") == pf::kRoleRender);
    CHECK("the compile workers", pf::role_of_name("mkw_vk worker") == pf::kRoleCompile && pf::role_of_name("mkw_vk serial builds") == pf::kRoleCompile);
    CHECK("the upload threads", pf::role_of_name("mkw_vk capture copy") == pf::kRoleUpload && pf::role_of_name("mkw_vk imports") == pf::kRoleUpload);
    CHECK("the game's threads", pf::role_of_name("Game") == pf::kRoleGame && pf::role_of_name("MainThread") == pf::kRoleGame &&
          pf::role_of_name("Thread_800123") == pf::kRoleGame && pf::role_of_name("hl-worker-3") == pf::kRoleGame);
    CHECK("audio", pf::role_of_name("hos audio renderer") == pf::kRoleAudio);
    CHECK("the display thread presents", pf::role_of_name("mkw_vk display") == pf::kRolePresent);
    CHECK("an unknown or no name is other", pf::role_of_name("") == pf::kRoleOther && pf::role_of_name("tid 5") == pf::kRoleOther);

    // the sampling rate
    CHECK("the default", pf::sample_hz(nullptr) == 250 && pf::sample_hz("") == 250 && pf::sample_hz("abc") == 250);
    CHECK("clamped", pf::sample_hz("1") == 50 && pf::sample_hz("100000") == 1000 && pf::sample_hz("400") == 400);

    CHECK("the batch interval", pf::batch_seconds(nullptr) == 300 && pf::batch_seconds("x") == 300 && pf::batch_seconds("1") == 10 &&
          pf::batch_seconds("60") == 60 && pf::batch_seconds("999999") == 3600);

    // a pass's kind and key
    CHECK("depth only is a shadow", pf::pass_kind(0, true, 2048, 2048, 100, 1920 * 1080) == pf::kShadow);
    CHECK("colour and depth is main", pf::pass_kind(3, true, 1920, 1080, 500, 1920 * 1080) == pf::kMain);
    CHECK("a full screen effect with few draws is post", pf::pass_kind(1, false, 1920, 1080, 1, 1920 * 1080) == pf::kPost);
    CHECK("a small target is post", pf::pass_kind(1, false, 480, 270, 200, 1920 * 1080) == pf::kPost);
    CHECK("many draws to a full screen target without depth is ui", pf::pass_kind(1, false, 1920, 1080, 90, 1920 * 1080) == pf::kUi);
    CHECK("nothing at all is other", pf::pass_kind(0, false, 1, 1, 0, 0) == pf::kOtherPass);
    CHECK("a dynamic resolution's small step keeps the key", pf::pass_key(37, 1, true, 1920, 1080, 5) == pf::pass_key(37, 1, true, 1930, 1075, 5));
    CHECK("another format or program is another key", pf::pass_key(37, 1, true, 1920, 1080, 5) != pf::pass_key(38, 1, true, 1920, 1080, 5) &&
          pf::pass_key(37, 1, true, 1920, 1080, 5) != pf::pass_key(37, 1, true, 1920, 1080, 6));
    CHECK("a name's key", pf::name_key("fsr3 reconstruct") != pf::name_key("fsr3 sharpen") && pf::name_key("a") == pf::name_key("a"));

    // the samples wait for their frame's verdict, and a hitch frame's count into the hitch counters as well
    {
        pf::SampleRing ring(8);
        pf::FrameVerdicts verdicts;
        pf::MapProfile map;
        for (uint64_t f = 0; f < 4; f++)
            for (int i = 0; i < 3; i++) {
                pf::Sample s;
                s.frame = f, s.role = pf::kRoleGame, s.module = pf::kGameLifted, s.key = 0x7100000000ull + (uint64_t)f * 4, s.key_kind = 1, s.tid = 7;
                ring.push(s);
            }
        // frames 0 and 1 are closed (1 a hitch): frames 2 and 3 are open
        verdicts.close(false), verdicts.close(true);
        // (the ring held 8: the 12 pushed leave the last 8: frames 1 (partly), 2 and 3)
        CHECK("the ring is bounded", ring.n == 8 && ring.dropped == 4);
        const size_t took = ring.take_closed(verdicts.closed, [&](const pf::Sample& s) {
            const bool hitch = verdicts.is_hitch(s.frame);
            map.add_cpu(s.role, s.module, s.tid, hitch);
            map.guest.add(s.key, hitch);
        });
        CHECK("only the closed frames' samples are taken", took == 2 && ring.n == 6);
        CHECK("frame 1's two samples count into the hitch counters too", map.role[pf::kRoleGame].samples == 2 &&
              map.role[pf::kRoleGame].hitch_samples == 2 && map.role[pf::kRoleGame].hitch_mod[pf::kGameLifted] == 2);
        CHECK("the guest table's hitch count", map.guest.m.at(0x7100000004ull).hitch == 2 && map.guest.m.at(0x7100000004ull).samples == 2);
        verdicts.close(false), verdicts.close(false);
        map = pf::MapProfile();
        ring.take_closed(verdicts.closed, [&](const pf::Sample& s) { map.add_cpu(s.role, s.module, s.tid, verdicts.is_hitch(s.frame)); });
        CHECK("the rest, none of it a hitch", map.role[pf::kRoleGame].samples == 6 && map.role[pf::kRoleGame].hitch_samples == 0 && ring.n == 0);
        CHECK("a frame is a hitch only when it closed as one", verdicts.is_hitch(1) && !verdicts.is_hitch(0) && !verdicts.is_hitch(99));
        for (int i = 0; i < pf::kHitchFrames + 10; i++) verdicts.close(i == 3);
        CHECK("an old frame's verdict is forgotten", !verdicts.is_hitch(1));
    }

    // a top table is cut back when it grows, and keeps its busiest keys
    {
        pf::Top t;
        for (uint64_t k = 0; k < pf::kTableMost + 50; k++) t.add(k + 1, false, k < 10 ? 1000 : 1);
        CHECK("cut back past the most", t.m.size() <= pf::kTableKeep + 50 && t.m.size() < pf::kTableMost);
        const auto best = t.best(5);
        CHECK("the busiest survive", best.size() == 5 && best[0].second.samples == 1000);
        CHECK("most first", best[0].second.samples >= best[4].second.samples);
    }

    // the JSON: the caps hold, the body is valid and the order is the documented one
    {
        const pf::MapProfile m = busy_map("Castle Town", 300, 300, 100);
        const std::string j = pf::map_json(m);
        size_t guests = 0, engines = 0, passes = 0;
        for (size_t at = 0; (at = j.find("\"addr\"", at)) != std::string::npos; at++) guests++;
        for (size_t at = 0; (at = j.find("\"rva\"", at)) != std::string::npos; at++) engines++;
        for (size_t at = 0; (at = j.find("\"kind\"", at)) != std::string::npos; at++) passes++;
        CHECK("top_guest is capped at 32", guests == pf::kMaxGuest);
        CHECK("top_engine is capped at 32", engines == pf::kMaxEngine);
        CHECK("passes are capped at 24", passes == pf::kMaxPasses);
        CHECK("the renderer's locations say so", j.find("\"module\":\"vk\"") != std::string::npos);
        CHECK("a map without frames, cpu or passes says only its totals", pf::map_json(pf::MapProfile()).find("\"cpu\"") == std::string::npos &&
              pf::map_json(pf::MapProfile()).find("\"gpu\"") == std::string::npos);
        pf::MapProfile nogpu = busy_map("x", 3, 3, 3);
        nogpu.gpu = false;
        CHECK("no timestamps, no gpu block", pf::map_json(nogpu).find("\"gpu\"") == std::string::npos);
    }

    // the payload's top level (no "first")
    {
        const std::string p = pf::payload_json(meta(), {});
        CHECK("the head", p.compare(0, 8, "{\"v\":1,\"") == 0 && p.find("\"build_id\":\"0123456789abcdef\"") != std::string::npos &&
              p.find("\"vendor_id\":4318") != std::string::npos && p.find("\"first\"") == std::string::npos &&
              p.find("\"release\":true") != std::string::npos);
        pf::Meta bare;
        bare.version = "v0.4.3";
        const std::string q = pf::payload_json(bare, {});
        CHECK("unknown plain names are left out", q.find("\"gpu\"") == std::string::npos && q.find("\"cpu\"") == std::string::npos &&
              q.find("\"os\"") == std::string::npos);
        pf::Meta quoted = meta();
        quoted.gpu = std::string("A \"card\" with \\ and a long name ") + std::string(300, 'x');
        const std::string r = pf::payload_json(quoted, {});
        CHECK("a name is escaped and cut at 120", r.find("A \\\"card\\\" with \\\\ and") != std::string::npos && r.size() < 700);
    }

    // the batches: as many maps to a batch as fit, a map alone over the cap left out
    {
        std::vector<std::string> parts(10, std::string(30000, 'a'));
        size_t over = 0;
        auto g = pf::batch_groups(parts, pf::kMaxBodyBytes, &over);
        size_t total = 0;
        for (const auto& b : g) total += b.size();
        CHECK("every map is in a batch, in order", total == 10 && over == 0 && g.size() == 2 && g[0].size() == 8 && g[1][0] == 8);
        parts.push_back(std::string(300 * 1024, 'b'));
        g = pf::batch_groups(parts, pf::kMaxBodyBytes, &over);
        CHECK("a map over the cap alone is left out", over == 1);
        for (const auto& b : g) {
            size_t bytes = 400;
            for (size_t i : b) bytes += parts[i].size() + 1;
            CHECK("each batch is within the cap", bytes <= pf::kMaxBodyBytes);
        }
        std::vector<std::string> small(80, "{}");
        g = pf::batch_groups(small, pf::kMaxBodyBytes);
        CHECK("at most 32 maps a batch", g.size() == 3 && g[0].size() == 32 && g[2].size() == 16);
    }

    // the gating
    CHECK("the row decides first", !strcmp(pf::why_not(false, false, "0"), "Send performance statistics is off"));
    CHECK("then the address", !strcmp(pf::why_not(true, false, "0"), "this build has no telemetry address"));
    CHECK("then the switch", !strcmp(pf::why_not(true, true, "0"), "HL_TELEMETRY_PROFILE=0"));
    CHECK("on", !*pf::why_not(true, true, nullptr) && !*pf::why_not(true, true, "1") && !*pf::why_not(true, true, ""));
    CHECK("the local copy needs a non-zero number", !pf::local_on(nullptr) && !pf::local_on("0") && !pf::local_on("") && pf::local_on("1") && pf::local_on("2"));

    std::printf("ok\n");
    return 0;
}
