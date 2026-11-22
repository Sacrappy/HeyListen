// Copyright (C) 2026 Sacrappy

// The Texture resolution option's Low setting (vk_texture.cpp create): how many of a game texture's top mip levels it leaves out.
// A level left out takes a quarter of the one above it, so one level less is a texture 4x smaller. Only large ones lose any
// (UI, text and characters' small maps keep every level), and always one level at least stays in the image.
// tests/test_tex_low.py
#pragma once
#include <algorithm>
#include <cstdint>

namespace tex_low {

/// The longest side from which a texture is large (RX_VK_TEX_LOW_DIM; 1024: a 1024-texel map becomes 512, a 2048 one 1024)
constexpr uint32_t kDefaultMinDim = 1024;

/// Top levels of a 2D texture (w x h texels, `levels` mip levels in its storage, `already` of them left out by the size rules
/// already) that Low leaves out as well: 0 under min_dim, 1 from min_dim, 2 from 4 x min_dim (4096 by default: 8192 becomes 2048).
/// A texture with no mip chain cannot lose a level (there is no smaller one to keep), and the result keeps at least one level.
inline uint32_t levels_dropped(uint32_t w, uint32_t h, uint32_t levels, uint32_t already, uint32_t min_dim) {
    if (!min_dim || levels <= already + 1) return 0;
    const uint32_t side = std::max(w, h) >> already;   // (what the size rules left)
    uint32_t n = side >= min_dim ? 1 : 0;
    if (side >= 4ull * min_dim) n = 2;
    return std::min(n, levels - already - 1);
}

}  // namespace tex_low
