// (hookshot) Offline test of the Ampere -> Maxwell lowering (shader_recompiler/frontend/ampere/lower_to_maxwell.h).
// Runs LowerToMaxwell over native programs a run dumped (RX_VK_DUMP_FAILED=<folder>: <stage>_<key>.bin, 16-byte
// instructions) and groups what still fails by reason and opcode, with the first failing instructions' words.
//   lower_test <folder> [samples per reason]      (built by nx2/lower_test.py)
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: lower_test <folder> [samples]\n"); return 2; }
    const int samples = argc > 2 ? std::atoi(argv[2]) : 3;
    struct Group { int n = 0; std::vector<std::string> examples; };
    std::map<std::string, Group> groups;
    int total = 0, ok = 0;
    for (const auto& e : std::filesystem::directory_iterator(argv[1])) {
        if (e.path().extension() != ".bin") continue;
        FILE* f = std::fopen(e.path().string().c_str(), "rb");
        if (!f) continue;
        std::vector<Shader::Ampere::Instruction> code;
        Shader::Ampere::Instruction in;
        while (std::fread(&in, sizeof in, 1, f) == 1) code.push_back(in);
        std::fclose(f);
        total++;
        const auto r = Shader::Ampere::LowerToMaxwell(code);
        if (r) { ok++; if (std::getenv("LOWER_TEST_LIST_OK")) std::printf("ok %s\n", e.path().filename().string().c_str()); continue; }
        const unsigned at = r.error_offset / 16;
        const unsigned op = at < code.size() ? code[at].Bits(0, 12) : 0xFFFFFFFFu;
        char key[200];
        std::snprintf(key, sizeof key, "%s (op %#x)", std::string(r.error).c_str(), op);
        Group& g = groups[key];
        g.n++;
        if ((int)g.examples.size() < samples && at < code.size()) {
            char ex[200];
            const auto& w = code[at].words;
            std::snprintf(ex, sizeof ex, "%s +%#x: %08x %08x %08x %08x", e.path().filename().string().c_str(), r.error_offset,
                          w[0], w[1], w[2], w[3]);
            g.examples.push_back(ex);
        }
    }
    std::printf("%d programs, %d lower, %d fail\n", total, ok, total - ok);
    std::vector<std::pair<int, std::string>> order;
    for (auto& [k, g] : groups) order.push_back({-g.n, k});
    std::sort(order.begin(), order.end());
    for (auto& [n, k] : order) {
        std::printf("%5d  %s\n", -n, k.c_str());
        for (auto& ex : groups[k].examples) std::printf("         %s\n", ex.c_str());
    }
    return 0;
}
