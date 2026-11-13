// HeyListen native engine: the graphics front end's boundary with the game's graphics module (hl/bridge/GfxBridge.h).
#include "hl/bridge/GfxBridge.h"

namespace hl::bridge {

uint32_t GfxBridge::mode(uint64_t args) const { return static_cast<const guest::PassArgs*>(runtime_.ptr(args))->mode; }

void GfxBridge::run_phase(gfx::PassMode phase, uint64_t listener, uint64_t args) {
    // (the translated listener calls either phase with its own two arguments and nothing else in registers: both phases
    // read x0 and x1 only)
    runtime_.call(runtime_.current(), phase == gfx::PassMode::Scene ? kScenePass : kSecondPass, listener, args);
}

}   // namespace hl::bridge
