#pragma once

// What the BIOS's SetGsCrt programs into the GS's CRTC registers, for the
// HLE of SetGsCrt and of sceGsResetGraph (which calls it).

#include "runtime/ps2_memory.h"

#include <cstdint>

namespace ps2x::gs
{
    // interlaced: SMODE2.INT; videoMode: 2 NTSC, 3 PAL (others leave the
    // timing as it is); frameMode: SMODE2.FFMD.
    //
    // NTSC and PAL timing as the BIOS sets it (the values NetBSD's
    // playstation2 gs.c also uses): SMODE1's CMOD names the standard, and
    // SYNCV's front porch is odd when interlaced. A GS that reads them
    // (PCSX2's) takes the video mode, the picture's position and whether to
    // deinterlace from them.
    inline void programCrtc(GSRegisters &gs, uint32_t interlaced, uint32_t videoMode, uint32_t frameMode)
    {
        gs.smode2 = (interlaced & 0x1ull) | ((frameMode & 0x1ull) << 1);
        if (videoMode != 2u && videoMode != 3u)
            return;
        const bool pal = videoMode == 3u;
        const bool inter = (interlaced & 1u) != 0u;
        gs.smode1 = pal ? 0x740826504ull : 0x740824504ull;
        gs.synch1 = pal ? 0x7f5c21fc83030ull : 0x7f5b61f06f040ull;
        gs.synch2 = pal ? 0x3484bcull : 0x33a4d8ull;
        gs.syncv = pal ? (inter ? 0xa9000502101405ull : 0xa9000502101404ull)
                       : (inter ? 0xc7800601a01801ull : 0xc7800601a01802ull);
        gs.srfsh = 8u;
    }
}
