#pragma once

// PCSX2's microVU as GH1 Recomp's VU1, behind a plain interface so the
// runtime side never sees PCSX2's headers or compile flags.
//
// A program runs to its end at the VIF1 command that starts it (MSCAL,
// MSCNT), as the runtime's own VU1 interpreter runs it, and PCSX2 runs VU1
// with its default "instant VU1" setting.

#include <cstdint>
#include <functional>

namespace pcsx2vu
{
    struct Settings
    {
        // PCSX2's FPRoundMode for VU1 arithmetic: 0 nearest, 1 toward -inf,
        // 2 toward +inf, 3 toward zero (PCSX2's default). Its game database
        // sets 1 for Guitar Hero (SLUS-21224) and 0 for Guitar Hero II
        // (SLUS-21447).
        int roundMode = 3;
    };

    // Receives each PATH1 packet XGKICK sends, whole.
    using Kick = std::function<void(const uint8_t *packet, uint32_t size)>;

    void configure(const Settings &settings, Kick kick);

    // Where VU1's micro (code true) and data memory must live: beside
    // microVU's code, which addresses them with 32-bit displacements (PCSX2
    // keeps them in one reservation too). 16 KiB each; null if unavailable.
    uint8_t *memory(bool code);

    struct Run
    {
        uint8_t *code = nullptr; // VU1 micro memory (16 KiB)
        uint8_t *data = nullptr; // VU1 data memory (16 KiB)
        uint64_t codeGeneration = 0; // changes whenever micro memory was written
        uint32_t startPC = 0;    // in bytes; ignored when resuming
        bool resume = false;     // MSCNT: continue where the last program stopped
        uint32_t top = 0, itop = 0; // VIF1 TOP/ITOP
        uint32_t fbrst = 0;      // VU0 FBRST (DE1 0x400, TE1 0x800)
    };

    struct Result
    {
        uint32_t vpuStat = 0; // VPU_STAT's VU1 stop bits: 0x200 D bit, 0x400 T bit
        uint64_t cycles = 0;  // VU1 cycles the program took
    };

    Result run(const Run &run);

    // Time VU1 programs took since the last call: all of them, and the
    // longest (microVU compiles a program the first time it meets it in a
    // run, inside that run).
    struct Timing
    {
        uint32_t runs = 0;
        uint64_t totalNs = 0;
        uint64_t longestNs = 0;
        uint32_t slowRuns = 0; // programs that took over a millisecond: microVU compiling new code, mostly
        uint32_t clears = 0;   // new microcode uploaded since the last run (microVU looks the program up again)
        uint64_t compiledBytes = 0; // x86 code microVU compiled
        uint32_t cacheResets = 0;   // times microVU's code cache filled up and started over
    };
    Timing takeTiming();
}
