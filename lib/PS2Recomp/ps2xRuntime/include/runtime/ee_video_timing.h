#pragma once

#include <cstdint>

namespace ps2x
{
    // EE cycles between vblanks, from the scheduler. EE timers on the H-BLANK
    // clock (a timer's CLKS 3) count 262.5 lines a field from it, as NTSC's
    // video timing does: a game that zeroes one at a vblank reads the same
    // line count at every later one.
    void setVBlankCycles(uint64_t cycles) noexcept;
    // Two fields' EE cycles: 525 H-BLANKs.
    [[nodiscard]] uint64_t hblankFrameCycles() noexcept;
    // Nanoseconds the EE spent waiting for a VBlank's time to come (ahead
    // of the clock, with nothing to do) since the last call: what is left of
    // a frame once the EE's own work is done.
    [[nodiscard]] uint64_t takePacingIdleNs() noexcept;
}
