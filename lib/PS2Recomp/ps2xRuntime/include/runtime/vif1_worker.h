#pragma once

// VIF1 on a thread of its own, as PCSX2's MTVU puts VU1 on one: the EE hands
// each VIF1 transfer over (its data copied, the DMA channel done at once, as
// the transfers' data is already copied today) and goes on, while this
// thread unpacks it into VU1 memory, runs the VU1 programs it starts and
// sends what they kick to the GS. Everything bound for the GS from anywhere
// else (other GIF paths, a host's VBlank and read records) is posted behind
// the VIF1 data queued before it, so the GS sees it all in the EE's order.
// Reads that need the GS up to date wait for the queue (drain).
//
// Not in the precompiled header on purpose: ps2_memory.cpp, the GS frontend
// and the host include it.

#include <cstdint>
#include <functional>
#include <vector>

class PS2Memory;

namespace ps2x::vif1
{
    // Starts the thread; from then on VIF1 data goes to it. Once only.
    void start(PS2Memory &memory);
    // Finishes what is queued and stops the thread (VIF1 runs inline again).
    void stop();
    [[nodiscard]] bool active() noexcept;
    // True on the VIF1 thread itself.
    [[nodiscard]] bool onWorker() noexcept;
    // VIF1 data, in the order the EE sent it.
    void submit(std::vector<uint8_t> data);
    // Work that must come after the VIF1 data queued so far.
    void post(std::function<void()> job);
    // Waits until everything queued so far is done (no-op on the thread itself).
    void drain();
    // Time the thread spent on its work since the last call, in nanoseconds.
    [[nodiscard]] uint64_t takeBusyNs() noexcept;
    // Time other threads spent in drain() waiting for the queue, since the last call.
    [[nodiscard]] uint64_t takeDrainWaitNs() noexcept;
}
