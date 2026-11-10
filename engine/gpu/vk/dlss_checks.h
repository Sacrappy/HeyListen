/* (hookshot) Two small checks of NVIDIA DLSS's (gpu/vk/vk_dlss.cpp), as plain functions: which scene sizes DLSS can take
   for an output, and a Linux path (UTF-8) for NGX's wide strings. No Vulkan, no NGX: tests/test_dlss_checks.cpp
   checks them. */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace mkwvk::dlss_checks {

/// Whether a scene of w x h can be DLSS's input for an output of ow x oh: the output 1 to 3 times its size on each axis
/// (DLAA at 1, Ultra Performance at 3: 512x288 for 1536x864, 683x384 for 2048x1152). False while the output is not known
/// (0). The scene's camera and the actor motion take only these, and the upscale draws only these.
inline bool scene_size_fits(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh) {
    return w && h && ow && oh && w <= ow && h <= oh && (uint64_t)w * 3 >= ow && (uint64_t)h * 3 >= oh;
}

/// Whether s is all ASCII: NGX's Linux library takes only such paths (vk_dlss.cpp wide())
inline bool ascii(std::string_view s) {
    for (const char c : s)
        if ((unsigned char)c >= 0x80) return false;
    return true;
}

/// UTF-8 bytes (a Linux path) as wide characters: UTF-32 where wchar_t has 32 bits (Linux), UTF-16 where it has 16. Decoded
/// here, not through the process's locale. Bytes that are not well-formed UTF-8 (a stray continuation byte, a cut sequence,
/// an overlong form, a surrogate, past U+10FFFF) become U+FFFD, one for each maximal ill-formed part (Unicode's practice);
/// *bad is set when that happened. U+FFFD rather than the raw byte as a character: the raw byte would name another, real
/// path (0xE9 alone would become "é", which NGX writes back as C3 A9), U+FFFD names none.
inline std::wstring utf8_to_wide(std::string_view s, bool* bad = nullptr) {
    std::wstring out;
    out.reserve(s.size());
    const auto put = [&](uint32_t cp) {
        if (sizeof(wchar_t) == 2 && cp > 0xFFFF) {
            cp -= 0x10000;
            out += (wchar_t)(0xD800 + (cp >> 10));
            out += (wchar_t)(0xDC00 + (cp & 0x3FF));
        } else {
            out += (wchar_t)cp;
        }
    };
    const auto replace = [&] {
        put(0xFFFD);
        if (bad) *bad = true;
    };
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char b = (unsigned char)s[i];
        if (b < 0x80) {
            put(b), i++;
            continue;
        }
        // (the lead byte: how many continuation bytes follow, and the first one's range, Unicode's table 3-7)
        int need = 0;
        uint32_t cp = 0;
        unsigned char lo = 0x80, hi = 0xBF;
        if (b >= 0xC2 && b <= 0xDF) {
            need = 1, cp = b & 0x1F;
        } else if (b >= 0xE0 && b <= 0xEF) {
            need = 2, cp = b & 0x0F;
            if (b == 0xE0) lo = 0xA0;   // (no overlongs)
            if (b == 0xED) hi = 0x9F;   // (no surrogates)
        } else if (b >= 0xF0 && b <= 0xF4) {
            need = 3, cp = b & 0x07;
            if (b == 0xF0) lo = 0x90;   // (no overlongs)
            if (b == 0xF4) hi = 0x8F;   // (nothing past U+10FFFF)
        } else {   // (a continuation byte alone, C0, C1, F5 to FF)
            replace(), i++;
            continue;
        }
        size_t j = i + 1;
        int got = 0;
        for (; got < need && j < s.size(); got++, j++) {
            const unsigned char c = (unsigned char)s[j];
            if (c < lo || c > hi) break;
            cp = (cp << 6) | (c & 0x3F);
            lo = 0x80, hi = 0xBF;
        }
        if (got < need) replace();   // (the lead and the good bytes after it: one U+FFFD; the bad byte is read again)
        else put(cp);
        i = j;
    }
    return out;
}

} // namespace mkwvk::dlss_checks
