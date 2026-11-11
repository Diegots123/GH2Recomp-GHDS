// Between vushim.cpp and the VU1 bridge: where XGKICK packets go, and the
// INTC lines microVU raised.
#pragma once

#include "common/Pcsx2Types.h"

namespace pcsx2vu_shim
{
    using KickFn = void (*)(const u8 *packet, u32 size, void *user);
    void setKick(KickFn kick, void *user);

    // INTC lines raised since the last call (bit n for hwIntcIrq(n)).
    u32 takeInterrupts();
}
