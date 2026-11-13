// HeyListen native engine: where the graphics front end (hl::gfx) meets the game's graphics module, still run from the
// user's local translation (docs/native-engine/gfx-record-job.md has the reading behind every address here).
//
// Nothing here is game code or data: guest addresses of the one supported build and the layout of the few fields read.
#pragma once

#include <cstddef>
#include <cstdint>

#include "hl/bridge/JobBridge.h"
#include "hl/gfx/RecordPass.h"

namespace hl::bridge {

namespace guest {

/// The argument block the recording job's listener walk hands every listener.
struct PassArgs {
    uint8_t _0[28];
    uint32_t mode;   ///< hl::gfx::PassMode
};
static_assert(offsetof(PassArgs, mode) == 28);

}   // namespace guest

/// The pass's listener: reached only through a pointer (the listener list's walk at 0x80076cd0 jumps to it through the
/// thunk 0x80076e60, under the global lock at 0x84eb5440), so it is answered with no change to the lifted code.
inline constexpr GuestAddr kRecordPass = 0x806602e0;
inline constexpr GuestAddr kScenePass = 0x80660320;    ///< mode 0: the scene's recording (called)
inline constexpr GuestAddr kSecondPass = 0x8065fe30;   ///< mode 1 (called)

/// The pass's phases, run through the guest runtime on the calling thread's own guest context.
class GfxBridge final : public gfx::PassHost {
public:
    explicit GfxBridge(GuestRuntime& runtime) : runtime_(runtime) {}
    uint32_t mode(uint64_t args) const override;
    void run_phase(gfx::PassMode phase, uint64_t listener, uint64_t args) override;

private:
    GuestRuntime& runtime_;
};

}   // namespace hl::bridge
