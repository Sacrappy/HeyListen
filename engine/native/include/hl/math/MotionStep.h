// HeyListen native engine: one motion's step in Havok's last solver substep (guest 0x8182dc20, subIntegrateLastStep,
// docs/native-engine/physics-loops.md section 9.3, steps 1 to 9 and the position of step 11), operation for operation
// (hl/math/Exact.h). It works on copies: the caller gives the motion's 128 bytes and the values the function reads, and
// gets back the motion as the function leaves it and what the rest of the function needs (the records to append, the
// moving flag, the deactivation test, the cell position). It declines (returns false) where the exactness rules do not
// cover a value, and on the integration modifier's path (a virtual call that may change the velocities).
#ifndef HL_MATH_MOTION_STEP_H
#define HL_MATH_MOTION_STEP_H

#include <cstdint>

namespace hl::math {

// What the step reads beside the motion: the two solver velocity entries A and B (32 bytes each: V in floats 0, 1, 2,
// W in floats 4, 5, 3; then A's 64-bit word at +24, B's two packed maxima at +24, +26 and its flags word at +28), the
// solver step data D and the physics module's globals G (their fields by offset).
struct MotionStepIn {
    uint8_t motion[128];
    float a[6], b[6];
    uint64_t a24;
    uint16_t max_w, max_v;   // B +24, +26: a float's bits shifted right by 15
    uint32_t b28;            // bits 0-4 n, bit 5, bit 6; the motion id above bit 8
    float d32, d96[2], d104[2], d136, d144[2], q0[4], q1[4];
    float g50[2], c[4], h[2], c1[2], c2[2], c3[2];
};

enum class MotionDecline : uint8_t { none, values, modifier, count };

// How the step went (a histogram's classes) and what it leaves for the rest of the function.
struct MotionStepOut {
    uint8_t motion[128];    // as the step leaves it
    bool clamp_v, clamp_w;  // a clamp record of kind 0 / kind 1 is appended (in that order)
    uint8_t n_path;         // 0 no n; 1 within the threshold; 2 W scaled; 3 W zeroed (V kept); 4 W and V zeroed
    bool moving;
    uint16_t body_flag;     // 0x1000 when B's bit 5 chose the smaller velocities, else 0
    bool still_record;      // the deactivation's zero case: previous W and dW all zero
    float cell_pos[4];      // the position the cell lookup gets (the centre of mass in binary32, w 0)
    int32_t acc[4];         // the accumulators' lanes before the weight: FCVTZS of the position times 128, then of C.x
};

bool motion_step(const MotionStepIn& in, MotionStepOut& out, MotionDecline& why);

}   // namespace hl::math

#endif
