#include "runtime/host_hooks.h"

#include <utility>

namespace ps2x::host
{
    namespace
    {
        VBlankHook s_vblank;
        PadSource s_pad;
        Vu1Backend s_vu1;
        Vu1MemorySource s_vu1Memory;
    }

    void setVBlankHook(VBlankHook hook) { s_vblank = std::move(hook); }

    void vblankStart(uint64_t vsyncTick)
    {
        if (s_vblank)
            s_vblank(vsyncTick);
    }

    void setPadSource(PadSource source) { s_pad = std::move(source); }

    bool readPad(int port, int slot, uint8_t *data, size_t size)
    {
        return s_pad && s_pad(port, slot, data, size);
    }

    void setVu1Backend(Vu1Backend backend) { s_vu1 = std::move(backend); }

    bool hasVu1Backend() { return static_cast<bool>(s_vu1); }

    uint32_t runVu1(const Vu1Run &run) { return s_vu1 ? s_vu1(run) : 0u; }

    void setVu1MemorySource(Vu1MemorySource source) { s_vu1Memory = std::move(source); }

    uint8_t *vu1Memory(bool code) { return s_vu1Memory ? s_vu1Memory(code) : nullptr; }
}
