#pragma once

// Hooks for a host program that replaces parts of the runtime's own host side
// (an external GS, its own window and input). Process-wide, set before run().
// They live outside the runtime's classes so the layout that generated code
// was compiled against does not change.

#include <cstddef>
#include <cstdint>
#include <functional>

namespace ps2x::host
{
    // Called at every VBlank start on the EE side, after the GS CSR FIELD bit
    // flips and before the guest's VBlank handlers are dispatched: the point at
    // which a PS2 GS latches the display registers for the next field.
    using VBlankHook = std::function<void(uint64_t vsyncTick)>;
    void setVBlankHook(VBlankHook hook);
    void vblankStart(uint64_t vsyncTick);

    // Fills a 32-byte pad status (libpad layout) for port/slot. Returning
    // false falls back to the runtime's own input.
    using PadSource = std::function<bool(int port, int slot, uint8_t *data, size_t size)>;
    void setPadSource(PadSource source);
    bool readPad(int port, int slot, uint8_t *data, size_t size);

    // An external VU1 (a recompiler) in place of the runtime's interpreter.
    // Called at VIF1 MSCAL (resume false, startPC in bytes) and MSCNT (resume
    // true) with VIF1's TOP/ITOP and the EE's VU0 FBRST; runs the program to
    // its end, as the interpreter does, and returns VPU_STAT's VU1 stop bits
    // (0x200 D bit, 0x400 T bit). codeGeneration changes whenever VU1 micro
    // memory is written.
    struct Vu1Run
    {
        uint8_t *code;
        uint8_t *data;
        uint64_t codeGeneration;
        uint32_t startPC;
        bool resume;
        uint32_t top;
        uint32_t itop;
        uint32_t fbrst;
    };
    using Vu1Backend = std::function<uint32_t(const Vu1Run &run)>;
    void setVu1Backend(Vu1Backend backend);
    bool hasVu1Backend();
    uint32_t runVu1(const Vu1Run &run);

    // Where VU1's micro (code true) and data memory live, when the VU1
    // backend needs them somewhere of its own (a recompiler that addresses
    // them relative to its code). Each call returns the same 16 KiB, or null
    // for the runtime's own allocation. Set before the runtime initializes.
    using Vu1MemorySource = std::function<uint8_t *(bool code)>;
    void setVu1MemorySource(Vu1MemorySource source);
    uint8_t *vu1Memory(bool code);
}
