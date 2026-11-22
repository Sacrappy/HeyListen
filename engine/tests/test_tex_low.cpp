// Copyright (C) 2026 Sacrappy

// tex_low::levels_dropped (gpu/vk/tex_low.h): which game textures the Texture resolution option's Low shrinks, and by how much.
// tests/test_tex_low.py
#include "../gpu/vk/tex_low.h"

#include <cstdio>

static int failures = 0;
static void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

int main() {
    using tex_low::levels_dropped;
    const uint32_t D = tex_low::kDefaultMinDim;
    expect(D == 1024, "a texture is large from 1024 texels");
    expect(levels_dropped(512, 512, 10, 0, D) == 0, "a 512 map keeps every level");
    expect(levels_dropped(1023, 1023, 10, 0, D) == 0, "just under the size keeps every level");
    expect(levels_dropped(768, 768, 10, 0, D) == 0, "a 768 map (character textures) keeps every level");
    expect(levels_dropped(1024, 1024, 11, 0, D) == 1, "a 1024 map loses its top level (512)");
    expect(levels_dropped(1024, 512, 10, 0, D) == 1, "the longest side decides");
    expect(levels_dropped(2048, 2048, 12, 0, D) == 1, "a 2048 map loses one (1024)");
    expect(levels_dropped(4095, 4095, 12, 0, D) == 1, "just under four times the size loses one");
    expect(levels_dropped(4096, 4096, 13, 0, D) == 2, "a 4096 map loses two (1024)");
    expect(levels_dropped(8192, 8192, 14, 0, D) == 2, "an 8192 map loses two (2048)");
    expect(levels_dropped(1024, 1024, 1, 0, D) == 0, "no mip chain: no level to keep instead");
    expect(levels_dropped(4096, 4096, 2, 0, D) == 1, "two levels: one stays");
    expect(levels_dropped(4096, 4096, 3, 0, D) == 2, "three levels: two go, one stays");
    expect(levels_dropped(8192, 8192, 6, 2, D) == 1, "the size rules left 2048 of 8192: under four times the size, one more goes");
    expect(levels_dropped(16384, 16384, 10, 2, D) == 2, "16384 with two already out is 4096: two more");
    expect(levels_dropped(1024, 1024, 5, 4, D) == 0, "only the last level left");
    expect(levels_dropped(1024, 1024, 5, 0, 0) == 0, "a size of 0 turns it off");
    expect(levels_dropped(512, 512, 8, 0, 512) == 1, "RX_VK_TEX_LOW_DIM=512: a 512 map loses one");
    expect(levels_dropped(2048, 2048, 12, 0, 512) == 2, "RX_VK_TEX_LOW_DIM=512: 2048 is four times: two");
    if (failures) return 1;
    std::printf("tex_low: ok\n");
    return 0;
}
