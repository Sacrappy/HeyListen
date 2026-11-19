// (HeyListen) What the prediction of pipelines (predict_run, vk_draw.cpp) holds itself to. The pipelines it makes are built for the
// driver's own cache and let go, so none of them is needed before the game: they are speculative, and a start that is cold (no program
// cache, every shader prepared) runs them all in one go. On the PC 3050 (RTX 3050, 32 GB) two programs of the game's, 502k and 1014k
// words of SPIR-V, each kept the driver's compiler busy for over three minutes and growing past 11 GB of the process's commit (29 GB
// in the user's run, which then refused C++ allocations at the commit limit, "0 MB of commit left", and had the game start only
// after 616 s); every other program, up to 133k words, took 25 s at most and a few hundred MB. So:
//  * a program over kDefaultMaxKWords thousand words is left to the draw that needs it (RX_VK_PREDICT_MAX_KWORDS, 0: no limit);
//  * the prediction stops, the rest left to play, when the system has less than kDefaultReserveMB of commit left
//    (RX_VK_PREDICT_RESERVE_MB, 0: no limit).
// (v0.4.3: the default is 100: on a GTX 1660 SUPER (Turing) one program of 144k words stayed in the driver for 2 hours, the same
// takes ~25 s on Ampere; the programs measured cheap and safe are 40k words or fewer, and a prediction is speculative anyway.)
// No Vulkan: tests/predict_limits_test.cpp.
#pragma once
#include <cstdint>

namespace mkwvk::predictlimits {

constexpr unsigned long long kDefaultMaxKWords = 100;   // (was 300: a 144k-word program sat 2 hours in a GTX 1660 SUPER's driver, v0.4.2; the cheap ones measured are <= 40k)
constexpr unsigned long long kDefaultReserveMB = 3072;

/// A program of `words` 32-bit words of SPIR-V (all its stages) is too big to be predicted when max_kwords is not 0 and it is over it
inline bool too_big(uint64_t words, unsigned long long max_kwords) { return max_kwords && words / 1000 > max_kwords; }

/// The system has less commit left than the prediction wants to leave (reserve_mb 0: it does not look)
inline bool low_commit(unsigned long long left_mb, unsigned long long reserve_mb) { return reserve_mb && left_mb < reserve_mb; }

}  // namespace mkwvk::predictlimits
