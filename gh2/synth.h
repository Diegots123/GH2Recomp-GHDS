#pragma once

#include "addresses.h"
#include "ps2_runtime.h"

namespace ghr
{
    // HMX_synth_s, the IOP synth module GH1 loads as SYNTH_S.IRX, served on
    // the host by GH Classic's implementation of the same module.
    // audioOffsetMs (audio_offset in GH2Recomp.ini): the streams' audio that
    // much later (+) or earlier (-) against the notes, as discs with the
    // songs' audio recorded again do it; the disc itself stays the original.
    void installSynth(PS2Runtime &runtime, const Addresses &addresses, int audioOffsetMs = 0);
}
