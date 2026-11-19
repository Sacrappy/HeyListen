// (hookshot, stream SH1) How many words a program's local memory array is declared with. Pure: tests/test_local_memory_pad.cpp.
//
// A Maxwell program's local memory (STL/LDL) is one Private array of u32 in SPIR-V. NVIDIA's compiler promotes an array of up to a few
// thousand words to SSA values: one value a word at every basic block, a phi for each at every join of the program. OoT's mesh programs
// of 300k to 1M words (14k-28k blocks) carry 1868 to 1920 words of it, and their driver compile took over three minutes and grew
// the process's C heaps by ~1 GB/s up to ~29 GB (RTX 3050, 8 GB VRAM, 32 GB RAM: the commit limit, the compile failed). The same
// two programs with the array declared larger (nothing else changed) compile in 20 s and 0.45 GB (475k words) and 147 s and
// 2.5 GB (987k words): above its limit the driver leaves the array in memory. Two other mesh programs of the game (296k and 309k
// words) already carry 2276 and 2316 words and always compiled in the 25 s of everything else.
//
// Declaring more words than the program addresses changes nothing a program can observe: every access stays inside the words it
// had, the zero initializer covers the added ones too, and an access past the old end (undefined before) now reads or writes a
// word of its own. RX_VK_LOCAL_PAD=<words> (default off): a local memory of at least RX_VK_LOCAL_PAD_FROM words (1024) and fewer
// than <words> is declared with <words>. The scratch the driver reserves for the program grows with it (4 bytes a word a thread).
#pragma once

#include <cstdint>
#include <cstdlib>

namespace Shader::Backend::SPIRV {

inline constexpr std::uint32_t kLocalPadDefault = 0;        // (off)
inline constexpr std::uint32_t kLocalPadFromDefault = 1024;

/// A switch's text: a decimal number of words. Unset, empty, zero, negative or not a number: `fallback`.
inline std::uint32_t LocalPadNumber(const char* text, std::uint32_t fallback) {
    if (!text || !*text) {
        return fallback;
    }
    char* end{};
    const long long value{std::strtoll(text, &end, 10)};
    if (end == text || *end != '\0' || value <= 0 || value > 0x10000000ll) {
        return fallback;
    }
    return static_cast<std::uint32_t>(value);
}

/// The words the array is declared with: `words` unless the padding is on (`target` != 0) and the array is `from` words or more and
/// shorter than `target`
inline std::uint32_t PaddedLocalWords(std::uint32_t words, std::uint32_t target, std::uint32_t from) {
    return (target != 0 && words >= from && words < target) ? target : words;
}

} // namespace Shader::Backend::SPIRV
