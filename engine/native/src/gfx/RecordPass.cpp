// HeyListen native engine: the graphics recording pass (hl/gfx/RecordPass.h).
#include "hl/gfx/RecordPass.h"

namespace hl::gfx {

uint32_t RecordPass::run(uint64_t listener, uint64_t args) {
    stats_.passes.fetch_add(1, std::memory_order_relaxed);
    const uint32_t mode = host_.mode(args);
    // (any other mode records nothing; the walk goes on all the same)
    if (mode == (uint32_t)PassMode::Scene) {
        stats_.scene.fetch_add(1, std::memory_order_relaxed);
        host_.run_phase(PassMode::Scene, listener, args);
    } else if (mode == (uint32_t)PassMode::Second) {
        stats_.second.fetch_add(1, std::memory_order_relaxed);
        host_.run_phase(PassMode::Second, listener, args);
    }
    return 1;
}

}   // namespace hl::gfx
