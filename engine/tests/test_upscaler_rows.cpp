#include "../gpu/vk/upscaler_rows.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

int main() {
    namespace ur = mkwvk::upscaler_rows;
    using ur::DLAA;
    using ur::DLSS;
    using ur::GAME;
    constexpr int kNative = ur::kNativeAa;
    // (the values saved before keep their shares; the new ones are DLSS's ratios, each a twelfth of the output)
    CHECK("saved values keep their shares", ur::percent(0) == 0 && ur::percent(1) == 100 && ur::percent(2) == 75 &&
                                                ur::percent(3) == 67 && ur::percent(4) == 50);
    CHECK("Balanced and Ultra Performance", ur::percent(5) == 58 && ur::percent(6) == 33);
    // (the native choice, DLAA and FSR 4's native AA: a new value after the saved ones, no renumbering; 100% of the output)
    CHECK("the native choice is 7, the last", kNative == 7 && ur::kValues == 8 && ur::percent(kNative) == 100);
    CHECK("a value past the row is Auto", ur::percent(8) == 0 && ur::percent(-1) == 0);
    for (int v = 1; v < ur::kValues; v++) {
        const int k = (int)(ur::percent(v) * 12 / 100.0 + 0.5);
        CHECK("each share is a twelfth", k * 100 / 12 == ur::percent(v) || (k * 100 + 11) / 12 == ur::percent(v));
    }
    CHECK("DLSS's mode names", !std::strcmp(ur::dlss_mode_name(3), "Quality") && !std::strcmp(ur::dlss_mode_name(5), "Balanced") &&
                                   !std::strcmp(ur::dlss_mode_name(4), "Performance") &&
                                   !std::strcmp(ur::dlss_mode_name(6), "Ultra Performance"));
    CHECK("no mode name for Auto, 100%, 75% or native", !ur::dlss_mode_name(0) && !ur::dlss_mode_name(1) && !ur::dlss_mode_name(2) &&
                                                          !ur::dlss_mode_name(kNative));

    // the game's own: Auto, 100, 75, 67, 50, as before
    CHECK("game: Right", ur::step(GAME, 0, 1) == 1 && ur::step(GAME, 1, 1) == 2 && ur::step(GAME, 3, 1) == 4 && ur::step(GAME, 4, 1) == 0);
    CHECK("game: Left", ur::step(GAME, 0, -1) == 4 && ur::step(GAME, 1, -1) == 0 && ur::step(GAME, 4, -1) == 3);
    // DLSS: Auto, DLAA (native), Quality 67, Balanced 58, Performance 50, Ultra Performance 33
    CHECK("dlss: Right", ur::step(DLSS, 0, 1) == kNative && ur::step(DLSS, kNative, 1) == 3 && ur::step(DLSS, 3, 1) == 5 &&
                             ur::step(DLSS, 5, 1) == 4 && ur::step(DLSS, 4, 1) == 6 && ur::step(DLSS, 6, 1) == 0);
    CHECK("dlss: Left", ur::step(DLSS, 0, -1) == 6 && ur::step(DLSS, 3, -1) == kNative && ur::step(DLSS, kNative, -1) == 0 &&
                            ur::step(DLSS, 6, -1) == 4);
    // a value that is not a step (saved with the other upscaler): its neighbours by size
    CHECK("dlss from 75%", ur::step(DLSS, 2, 1) == 3 && ur::step(DLSS, 2, -1) == kNative);
    CHECK("game from 58%", ur::step(GAME, 5, 1) == 4 && ur::step(GAME, 5, -1) == 3);
    CHECK("game from 33%", ur::step(GAME, 6, 1) == 0 && ur::step(GAME, 6, -1) == 4);
    CHECK("game from the native choice (a DLSS value saved)", ur::step(GAME, kNative, 1) == 1 && ur::step(GAME, kNative, -1) == 0);
    // the Upscaler that runs may be the mode DLAA (DLSS at native): its Render resolution steps as DLSS's
    CHECK("DLAA's row is DLSS's", ur::step(DLAA, 3, 1) == 5 && ur::step(DLAA, kNative, -1) == 0 && ur::snap(DLAA, 1) == 3);

    // another upscaler: the nearest step (the larger on a tie), Auto stays Auto; the native choice is never a nearest step
    CHECK("snap to DLSS", ur::snap(DLSS, 0) == 0 && ur::snap(DLSS, 1) == 3 && ur::snap(DLSS, 2) == 3 && ur::snap(DLSS, 3) == 3 &&
                              ur::snap(DLSS, 4) == 4);
    CHECK("snap to the game's", ur::snap(GAME, 5) == 4 && ur::snap(GAME, 6) == 4 && ur::snap(GAME, 3) == 3 && ur::snap(GAME, 1) == 1);
    CHECK("a value past the row snaps to Auto", ur::snap(GAME, 9) == 0 && ur::snap(DLSS, -3) == 0);
    CHECK("native is DLSS's and FSR 4's step", ur::snap(DLSS, kNative) == kNative && ur::snap(ur::FSR4, kNative) == kNative &&
                                                    ur::snap(DLAA, kNative) == kNative && ur::snap(ur::FSR4_AA, kNative) == kNative);
    CHECK("native snaps to 100% elsewhere", ur::snap(GAME, kNative) == 1 && ur::snap(ur::OFF, kNative) == 1 &&
                                                ur::snap(ur::FSR3, kNative) == 3);
    CHECK("100% (1) is not DLAA: a preset's 100% with DLSS is Quality", ur::snap(DLSS, 1) == 3 && ur::snap(ur::FSR4, 1) == 3);

    // the Upscaler row: the game's own, DLSS where DLSS runs, then Off (always there: every GPU). DLAA is not a stop
    CHECK("upscaler with DLSS", ur::step_upscaler(0, 1, true) == 1 && ur::step_upscaler(1, 1, true) == ur::OFF &&
                                    ur::step_upscaler(ur::OFF, 1, true) == 0 && ur::step_upscaler(0, -1, true) == ur::OFF &&
                                    ur::step_upscaler(ur::OFF, -1, true) == 1);
    CHECK("upscaler without DLSS: the game's own and Off", ur::step_upscaler(0, 1, false) == ur::OFF &&
                                                               ur::step_upscaler(ur::OFF, 1, false) == 0 &&
                                                               ur::step_upscaler(0, -1, false) == ur::OFF &&
                                                               ur::step_upscaler(2, -1, false) == ur::OFF);
    CHECK("upscaler from a bad value", ur::step_upscaler(ur::kUpscalers, 1, true) == 1 && ur::step_upscaler(-1, 1, true) == 1);
    CHECK("DLAA is DLSS to the row", ur::step_upscaler(DLAA, 1, true) == ur::OFF && ur::step_upscaler(DLAA, -1, true) == GAME);

    // AMD FSR 3.1 on the same row, after DLSS: stepping visits only what runs here, and wraps
    {
        const unsigned all = ur::bit(GAME) | ur::dlss_bits(true) | ur::bit(ur::FSR3);
        const unsigned fsr3 = ur::bit(GAME) | ur::bit(ur::FSR3);
        CHECK("FSR 3.1 after DLAA", ur::FSR3 == 3);
        CHECK("DLSS's and FSR 4's masks are one bit each", ur::dlss_bits(true) == ur::bit(DLSS) && ur::dlss_bits(false) == 0u &&
                                                               ur::fsr4_bits(true) == ur::bit(ur::FSR4) && ur::fsr4_bits(false) == 0u);
        CHECK("upscaler steps through all three, then Off", ur::step_upscaler(GAME, 1, all) == DLSS &&
                                                                ur::step_upscaler(DLSS, 1, all) == ur::FSR3 &&
                                                                ur::step_upscaler(ur::FSR3, 1, all) == ur::OFF &&
                                                                ur::step_upscaler(ur::OFF, 1, all) == GAME &&
                                                                ur::step_upscaler(GAME, -1, all) == ur::OFF &&
                                                                ur::step_upscaler(ur::OFF, -1, all) == ur::FSR3 &&
                                                                ur::step_upscaler(ur::FSR3, -1, all) == DLSS);
        CHECK("upscaler without DLSS skips it", ur::step_upscaler(GAME, 1, fsr3) == ur::FSR3 &&
                                                  ur::step_upscaler(ur::FSR3, 1, fsr3) == ur::OFF &&
                                                  ur::step_upscaler(ur::OFF, 1, fsr3) == GAME &&
                                                  ur::step_upscaler(GAME, -1, fsr3) == ur::OFF);
        CHECK("an upscaler that cannot run here steps as the game's own", ur::step_upscaler(DLSS, 1, fsr3) == ur::FSR3 &&
                                                                            ur::step_upscaler(ur::FSR3, 1, ur::bit(GAME)) == ur::OFF);
        CHECK("the DLSS-only form is the mask's", ur::step_upscaler(DLAA, 1, true) == ur::OFF && ur::step_upscaler(GAME, -1, true) == ur::OFF);
        CHECK("FSR 3.1's modes are DLSS's shares", ur::scales_by_mode(ur::FSR3) && ur::scales_by_mode(DLSS) &&
                                                     !ur::scales_by_mode(DLAA) && !ur::scales_by_mode(GAME));
        CHECK("FSR 3.1 steps as DLSS does without its native choice", ur::step(ur::FSR3, 0, 1) == 3 && ur::step(ur::FSR3, 3, 1) == 5 &&
                                                                          ur::step(ur::FSR3, 6, 1) == 0 && ur::snap(ur::FSR3, 2) == 3);
        CHECK("the model row waits with FSR 3.1", ur::model_fixed(ur::FSR3, false) && !ur::model_fixed(DLSS, false) &&
                                                    !ur::model_fixed(DLAA, false));
    }
    // AMD FSR 4 and its native AA after FSR 3.1 (saved values keep their meaning), on the same row and mask
    {
        using ur::FSR3;
        using ur::FSR4;
        using ur::FSR4_AA;
        const unsigned every = ur::bit(GAME) | ur::dlss_bits(true) | ur::bit(FSR3) | ur::fsr4_bits(true);
        const unsigned fsr4 = ur::bit(GAME) | ur::fsr4_bits(true);
        CHECK("FSR 4 after FSR 3.1", FSR4 == 4 && FSR4_AA == 5);
        CHECK("upscaler steps through all four, then Off", ur::step_upscaler(FSR3, 1, every) == FSR4 &&
                                                               ur::step_upscaler(FSR4, 1, every) == ur::OFF &&
                                                               ur::step_upscaler(ur::OFF, 1, every) == GAME &&
                                                               ur::step_upscaler(GAME, -1, every) == ur::OFF &&
                                                               ur::step_upscaler(ur::OFF, -1, every) == FSR4 &&
                                                               ur::step_upscaler(FSR4, -1, every) == FSR3);
        CHECK("upscaler with FSR 4 alone", ur::step_upscaler(GAME, 1, fsr4) == FSR4 && ur::step_upscaler(FSR4, 1, fsr4) == ur::OFF &&
                                               ur::step_upscaler(ur::OFF, 1, fsr4) == GAME && ur::step_upscaler(GAME, -1, fsr4) == ur::OFF);
        CHECK("FSR 4's native AA is FSR 4 to the row", ur::step_upscaler(FSR4_AA, 1, every) == ur::OFF &&
                                                          ur::step_upscaler(FSR4_AA, -1, every) == FSR3 &&
                                                          ur::step_upscaler(FSR4_AA, 1, fsr4) == ur::OFF);
        CHECK("a saved FSR 4 where it cannot run steps as the game's own",
              ur::step_upscaler(FSR4, 1, ur::bit(GAME) | ur::dlss_bits(true)) == DLSS &&
              ur::step_upscaler(FSR4, -1, ur::bit(GAME) | ur::bit(FSR3)) == ur::OFF &&
              ur::step_upscaler(ur::OFF, -1, ur::bit(GAME) | ur::bit(FSR3)) == FSR3);
        CHECK("engines", ur::is_dlss(DLSS) && ur::is_dlss(DLAA) && !ur::is_dlss(FSR3) && !ur::is_dlss(FSR4) &&
                             ur::is_fsr4(FSR4) && ur::is_fsr4(FSR4_AA) && !ur::is_fsr4(FSR3) && !ur::is_fsr4(DLAA) &&
                             !ur::is_fsr4(GAME));
        CHECK("full scale", ur::full_scale(DLAA) && ur::full_scale(FSR4_AA) && !ur::full_scale(FSR3) && !ur::full_scale(FSR4) &&
                                !ur::full_scale(DLSS) && !ur::full_scale(GAME));
        CHECK("FSR 4's modes are DLSS's shares", ur::scales_by_mode(FSR4) && !ur::scales_by_mode(FSR4_AA));
        for (int v = 0; v < ur::kValues; v++)
            for (int dir : {1, -1})
                CHECK("FSR 4 steps as DLSS", ur::step(FSR4, v, dir) == ur::step(DLSS, v, dir) && ur::snap(FSR4, v) == ur::snap(DLSS, v));
        CHECK("FSR 4's native AA steps as FSR 4", ur::step(FSR4_AA, 3, 1) == 5 && ur::snap(FSR4_AA, 5) == 5 &&
                                                     ur::step(FSR4_AA, 0, 1) == kNative);
        CHECK("the model row waits with FSR 4", ur::model_fixed(FSR4, false) && ur::model_fixed(FSR4_AA, false));
    }
    {   // (a saved Balanced 58% through every Upscaler: each draws its nearest step, none changes the saved value, and it comes back)
        const int saved = 5;
        CHECK("Balanced is a step of FSR 3.1, FSR 4 and DLSS", ur::snap(ur::FSR3, saved) == saved && ur::snap(ur::FSR4, saved) == saved &&
                                                                 ur::snap(ur::DLSS, saved) == saved);
        CHECK("the game's own draws the nearest step", ur::snap(ur::GAME, saved) == 4);
        int shown = saved;
        for (int up : {ur::GAME, ur::FSR3, ur::DLAA, ur::FSR4, ur::FSR4_AA, ur::DLSS, ur::GAME, ur::FSR3}) shown = ur::snap(up, saved);   // (the row never writes back)
        CHECK("back on an Upscaler with the step, the choice is whole", shown == saved);
        CHECK("a step from the shown value", ur::step(ur::GAME, ur::snap(ur::GAME, saved), 1) == 0 && ur::step(ur::GAME, ur::snap(ur::GAME, saved), -1) == 3);
        // (the native choice through the Upscalers: the game's own draws 100%, DLSS and FSR 4 draw it as DLAA / Native AA, and it
        // comes back)
        CHECK("the native choice goes round the Upscalers whole", ur::snap(ur::GAME, kNative) == 1 && ur::snap(ur::FSR3, kNative) == 3 &&
                                                                    ur::snap(DLSS, kNative) == kNative && ur::snap(ur::FSR4, kNative) == kNative);
    }
    // the mode the chain runs: DLSS at the native choice is DLAA, FSR 4 at it is its native AA; nothing else has one
    CHECK("mode for DLSS and FSR 4 at native", ur::mode_for(DLSS, kNative) == DLAA && ur::mode_for(ur::FSR4, kNative) == ur::FSR4_AA);
    CHECK("mode for the others at native", ur::mode_for(GAME, kNative) == GAME && ur::mode_for(ur::FSR3, kNative) == ur::FSR3 &&
                                               ur::mode_for(ur::OFF, kNative) == ur::OFF);
    CHECK("mode for a scaled share", ur::mode_for(DLSS, 3) == DLSS && ur::mode_for(DLSS, 0) == DLSS && ur::mode_for(DLSS, 1) == DLSS &&
                                         ur::mode_for(ur::FSR4, 6) == ur::FSR4);
    CHECK("engine of a mode", ur::engine(DLAA) == DLSS && ur::engine(ur::FSR4_AA) == ur::FSR4 && ur::engine(DLSS) == DLSS &&
                                  ur::engine(GAME) == GAME && ur::engine(ur::OFF) == ur::OFF && ur::engine(ur::FSR3) == ur::FSR3);
    CHECK("the native choice is in DLSS's and FSR 4's steps only",
          ur::has_native_aa(DLSS) && ur::has_native_aa(DLAA) && ur::has_native_aa(ur::FSR4) && ur::has_native_aa(ur::FSR4_AA) &&
              !ur::has_native_aa(GAME) && !ur::has_native_aa(ur::FSR3) && !ur::has_native_aa(ur::OFF));
    {   // an options file from before: DLAA (2) is DLSS (1) with the native choice (7), FSR 4's native AA (5) is FSR 4 (4) with it
        int up = DLAA, res = 3;
        CHECK("saved DLAA", ur::migrate(up, res) && up == DLSS && res == kNative);
        up = ur::FSR4_AA, res = 0;
        CHECK("saved FSR 4 native AA", ur::migrate(up, res) && up == ur::FSR4 && res == kNative);
        for (int saved : {0, 1, 3, 4, 6}) {   // (every other saved value stays, with its Render resolution)
            up = saved, res = 5;
            CHECK("the other saved Upscalers stay", !ur::migrate(up, res) && up == saved && res == 5);
        }
        up = DLAA, res = kNative;   // (idempotent: migrated once, the next load changes nothing)
        CHECK("migrated, then nothing", ur::migrate(up, res) && !ur::migrate(up, res) && up == DLSS && res == kNative);
        // (and the chain then runs DLAA / FSR 4's native AA, as the saved choice did)
        up = DLAA, res = 3;
        ur::migrate(up, res);
        CHECK("migrated DLAA runs as DLAA", ur::mode_for(up, res) == DLAA);
        up = ur::FSR4_AA, res = 3;
        ur::migrate(up, res);
        CHECK("migrated FSR 4 native AA runs as it did", ur::mode_for(up, res) == ur::FSR4_AA);
    }
    // the Window scaling filter: needed only when the window is not the picture's size (unknown: needed)
    CHECK("filter not needed at the output's size", !ur::filter_needed(2560, 1440, 2560, 1440));
    CHECK("filter needed otherwise", ur::filter_needed(2048, 1152, 2560, 1440) && ur::filter_needed(2560, 1440, 2560, 1080) &&
                                         ur::filter_needed(0, 0, 0, 0) && ur::filter_needed(2560, 1440, 0, 0));
    // the DLSS model row: its presets (NVIDIA's numbers, K = 11), RX_VK_DLSS_PRESET over it, and when it applies
    CHECK("model presets", ur::model_preset(ur::MODEL_K) == 11 && ur::model_preset(ur::MODEL_M) == 13 &&
                               ur::model_preset(ur::MODEL_L) == 12 && ur::model_preset(ur::MODEL_DEFAULT) == 0);
    CHECK("saved model numbers keep their meaning", ur::MODEL_K == 0 && ur::MODEL_M == 1 && ur::MODEL_L == 2 && ur::MODEL_DEFAULT == 3 &&
                                                        ur::MODEL_CNN == 4 && ur::kModels == 5 && ur::model_preset(ur::MODEL_CNN) == 5 && ur::kPresetE == 5);
    CHECK("model: a value past the row is K", ur::model_preset(-1) == 11 && ur::model_preset(ur::kModels) == 11);
    CHECK("model without the switch", ur::dlss_preset(ur::MODEL_M, nullptr) == 13 && ur::dlss_preset(ur::MODEL_DEFAULT, nullptr) == 0);
    CHECK("RX_VK_DLSS_PRESET wins", ur::dlss_preset(ur::MODEL_M, "K") == 11 && ur::dlss_preset(ur::MODEL_K, "m") == 13 &&
                                        ur::dlss_preset(ur::MODEL_K, "J") == 10 && ur::dlss_preset(ur::MODEL_M, "0") == 0);
    CHECK("RX_VK_DLSS_PRESET not a preset", !ur::preset_env(nullptr) && !ur::preset_env("") && !ur::preset_env("1") &&
                                                !ur::preset_env("05") && ur::dlss_preset(ur::MODEL_M, "7") == 13 &&
                                                ur::dlss_preset(ur::MODEL_L, "") == 12);
    // the first run's model by the GPU (only when the ini has none): DLSS 4 (K) on RTX 20 and 30, DLSS 4.5 (M) on RTX 40 and 50
    {
        CHECK("PCI ids by generation", !ur::fp8_gpu(0x10de, 0x1e04) /*2080 Ti*/ && !ur::fp8_gpu(0x10de, 0x2204) /*3090*/ &&
                                           !ur::fp8_gpu(0x10de, 0x2584) /*3050*/ && !ur::fp8_gpu(0x10de, 0x25ff) &&
                                           ur::fp8_gpu(0x10de, 0x2684) /*4090*/ && ur::fp8_gpu(0x10de, 0x2803) /*4060 Ti*/ &&
                                           ur::fp8_gpu(0x10de, 0x2b85) /*5090*/ && ur::fp8_gpu(0x10de, 0x2f04) /*5070*/);
        CHECK("PCI ids: not NVIDIA is not FP8", !ur::fp8_gpu(0x1002, 0x744c) && !ur::fp8_gpu(0x8086, 0x56a0) && !ur::fp8_gpu(0, 0x2684));
        CHECK("first-run model per generation", ur::default_model(0x10de, 0x1e04) == ur::MODEL_K && ur::default_model(0x10de, 0x2204) == ur::MODEL_K &&
                                                    ur::default_model(0x10de, 0x2584) == ur::MODEL_K && ur::default_model(0x10de, 0x2684) == ur::MODEL_M &&
                                                    ur::default_model(0x10de, 0x2b85) == ur::MODEL_M);
        {
            const uint64_t gb6 = 6ull << 30, gb7 = 7ull << 30, gb8 = 8ull << 30;
            CHECK("first run under 7 GB: the CNN on any RTX", ur::default_model(0x10de, 0x2584, gb6) == ur::MODEL_CNN &&
                                                                 ur::default_model(0x10de, 0x1e04, gb6) == ur::MODEL_CNN &&
                                                                 ur::default_model(0x10de, 0x2684, gb6) == ur::MODEL_CNN &&
                                                                 ur::default_model(0x10de, 0x2b85, gb7 - 1) == ur::MODEL_CNN);
            CHECK("first run at 7 GB and more, or not known: by generation", ur::default_model(0x10de, 0x2584, gb7) == ur::MODEL_K &&
                                                                              ur::default_model(0x10de, 0x2204, gb8) == ur::MODEL_K &&
                                                                              ur::default_model(0x10de, 0x2684, gb8) == ur::MODEL_M &&
                                                                              ur::default_model(0x10de, 0x2684, 0) == ur::MODEL_M &&
                                                                              ur::default_model(0x10de, 0x2584, 0) == ur::MODEL_K);
            CHECK("the CNN is the row's for every mode, RX_VK_DLSS_PRESET over it", ur::dlss_preset(ur::MODEL_CNN, nullptr) == 5 &&
                                                                                    ur::dlss_preset(ur::MODEL_CNN, "K") == 11 &&
                                                                                    ur::dlss_preset(ur::MODEL_K, nullptr) == 11);
        }
        // (a model in the ini is never changed by the GPU: model_preset has no GPU argument, and a 30 series card runs
        // DLSS 4.5 L with DLAA as chosen)
        CHECK("an RTX 30 can choose 4.5 L for DLAA", ur::dlss_preset(ur::MODEL_L, nullptr) == 12 && ur::dlss_preset(ur::MODEL_M, nullptr) == 13 &&
                                                        !ur::model_fixed(ur::DLAA, false));
    }
    CHECK("preset letters", ur::preset_letter(11) == 'K' && ur::preset_letter(13) == 'M' && ur::preset_letter(0) == '0');
    CHECK("model row shown with DLSS built and DLSS or DLAA chosen", ur::model_shown(true, DLSS) && ur::model_shown(true, DLAA) &&
                                                                         !ur::model_shown(false, DLSS) && !ur::model_shown(true, GAME) &&
                                                                         !ur::model_shown(true, ur::FSR3) && !ur::model_shown(true, ur::OFF));
    CHECK("model row fixed with the game's own or the switch", ur::model_fixed(GAME, false) && !ur::model_fixed(DLSS, false) &&
                                                               !ur::model_fixed(DLAA, false) && ur::model_fixed(DLSS, true));
    {   // Off: a new value after the saved ones (no renumbering), on every GPU, last in the row's order
        using ur::OFF;
        CHECK("saved values keep their numbers", GAME == 0 && DLSS == 1 && DLAA == 2 && ur::FSR3 == 3 && ur::FSR4 == 4 &&
                                                     ur::FSR4_AA == 5);
        CHECK("Off is 6, the last", OFF == 6 && ur::kUpscalers == 7);
        CHECK("Off is no engine", ur::is_off(OFF) && !ur::is_off(GAME) && !ur::is_off(ur::FSR4_AA) && !ur::is_dlss(OFF) &&
                                      !ur::is_fsr4(OFF) && !ur::scales_by_mode(OFF) && !ur::full_scale(OFF));
        CHECK("Off is there with nothing else available", ur::step_upscaler(GAME, 1, 0u) == OFF && ur::step_upscaler(OFF, 1, 0u) == GAME &&
                                                               ur::step_upscaler(GAME, -1, ur::bit(GAME)) == OFF);
        // (Render resolution applies to Off as to the game's own: its steps, and a saved Balanced or Ultra Performance snaps)
        for (int v = 0; v < ur::kValues; v++)
            for (int dir : {1, -1})
                CHECK("Off steps as the game's own", ur::step(OFF, v, dir) == ur::step(GAME, v, dir) && ur::snap(OFF, v) == ur::snap(GAME, v));
        CHECK("Off has a Render resolution", ur::snap(OFF, 5) == 4 && ur::snap(OFF, 1) == 1);
        CHECK("the model row waits with Off", ur::model_fixed(OFF, false));
        // (a saved 0 to 6 steps through what runs, in number order, round the whole row: DLAA and FSR 4's native AA are no stops)
        int at = GAME;
        const int order[] = {1, 3, 4, 6, 0};
        for (int want : order) {
            at = ur::step_upscaler(at, 1, ur::bit(GAME) | ur::dlss_bits(true) | ur::bit(ur::FSR3) | ur::fsr4_bits(true));
            CHECK("the row visits every stop in number order", at == want);
        }
        for (unsigned mask = 0; mask < (1u << ur::kUpscalers); mask++)   // (never DLAA or FSR 4's native AA, whatever can run)
            for (int v = -1; v <= ur::kUpscalers; v++)
                for (int dir : {1, -1}) {
                    const int next = ur::step_upscaler(v, dir, mask);
                    CHECK("the row never stops at DLAA or native AA", next != DLAA && next != ur::FSR4_AA && next >= 0 && next < ur::kUpscalers);
                }
    }
    std::puts("upscaler rows: all checks passed");
}
