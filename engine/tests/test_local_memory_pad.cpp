// The local memory array's declared size (backend/spirv/local_memory_pad.h): off by default, padded only inside its window.
#include <cstdint>
#include <cstdio>

#include "shader_recompiler/backend/spirv/local_memory_pad.h"

using namespace Shader::Backend::SPIRV;

static int bad = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); bad++; } } while (0)

int main() {
    // (off by default: no program's array changes)
    CHECK(kLocalPadDefault == 0);
    CHECK(PaddedLocalWords(1868, kLocalPadDefault, kLocalPadFromDefault) == 1868);
    CHECK(PaddedLocalWords(1920, 0, 1024) == 1920);
    // (on: the two programs' arrays, 1868 and 1920 words, are declared with the target; the arrays the driver already leaves in memory,
    // and the small ones every other program has, are not)
    CHECK(PaddedLocalWords(1868, 8192, kLocalPadFromDefault) == 8192);
    CHECK(PaddedLocalWords(1920, 8192, kLocalPadFromDefault) == 8192);
    CHECK(PaddedLocalWords(2316, 8192, kLocalPadFromDefault) == 8192);   // (inside the window: padded too)
    CHECK(PaddedLocalWords(8192, 8192, kLocalPadFromDefault) == 8192);
    CHECK(PaddedLocalWords(20000, 8192, kLocalPadFromDefault) == 20000);   // (never made smaller)
    CHECK(PaddedLocalWords(1023, 8192, kLocalPadFromDefault) == 1023);
    CHECK(PaddedLocalWords(1024, 8192, kLocalPadFromDefault) == 8192);
    CHECK(PaddedLocalWords(16, 8192, kLocalPadFromDefault) == 16);
    CHECK(PaddedLocalWords(16, 8192, 1) == 8192);
    // (the switches' text)
    CHECK(LocalPadNumber(nullptr, 7) == 7 && LocalPadNumber("", 7) == 7 && LocalPadNumber("0", 7) == 7 && LocalPadNumber("-3", 7) == 7);
    CHECK(LocalPadNumber("abc", 7) == 7 && LocalPadNumber("12x", 7) == 7 && LocalPadNumber("99999999999", 7) == 7);
    CHECK(LocalPadNumber("8192", 0) == 8192 && LocalPadNumber("1", 0) == 1);
    std::printf("local_memory_pad: %d failures\n", bad);
    return bad ? 1 : 0;
}
