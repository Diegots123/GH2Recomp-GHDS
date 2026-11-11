// VU1: PCSX2's microVU recompiler in place of the runtime's VU1 interpreter,
// with PCSX2's settings for GH2 (its game database gives SLUS-21447 VU1
// rounding to nearest, for the score meter's Z; everything else is PCSX2's
// default).
// GH1_VU1=interp keeps the runtime's interpreter, for comparisons.

#include "vu1.h"

#include "ps2_runtime.h"
#include "runtime/host_hooks.h"

#if defined(GH1_PCSX2VU)
#include "pcsx2vu_bridge.h"
#endif

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace ghr
{
    void installVu1(PS2Runtime &runtime)
    {
#if defined(GH1_PCSX2VU)
        const char *choice = std::getenv("GH1_VU1");
        if (choice && std::strcmp(choice, "interp") == 0)
        {
            std::cerr << "[gh2] VU1: the runtime's interpreter" << std::endl;
            return;
        }
        PS2Memory *memory = &runtime.memory();
        pcsx2vu::Settings settings;
        settings.roundMode = 0; // GameIndex.yaml, SLUS-21447: vu1RoundMode 0
        pcsx2vu::configure(settings, [memory](const uint8_t *packet, uint32_t size) {
            memory->submitGifPacket(GifPathId::Path1, packet, size);
        });
        // VU1 memory beside microVU's code (before the runtime allocates it).
        if (pcsx2vu::memory(false))
            ps2x::host::setVu1MemorySource([](bool code) { return pcsx2vu::memory(code); });
        ps2x::host::setVu1Backend([](const ps2x::host::Vu1Run &run) {
            pcsx2vu::Run r;
            r.code = run.code;
            r.data = run.data;
            r.codeGeneration = run.codeGeneration;
            r.startPC = run.startPC;
            r.resume = run.resume;
            r.top = run.top;
            r.itop = run.itop;
            r.fbrst = run.fbrst;
            return pcsx2vu::run(r).vpuStat;
        });
        std::cerr << "[gh2] VU1: PCSX2's microVU" << std::endl;
#else
        (void)runtime;
#endif
    }
}
