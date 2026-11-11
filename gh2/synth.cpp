// SYNTH_S (HMX_synth_s), GH2's IOP synth module: the EE's side of the link.
// Adapted from GH Classic's src/synth/synth.cpp (GPL-3.0), which serves the
// GH2 build of the same module; the module itself is GH Classic's
// synth::Module.
//
// SynthEE batches its commands into one NOWAIT RPC per poll on SID
// 0x75433178: {cmd, len, data} records, the data padded to 4 bytes. The IOP
// answers on the EE's own server (SID 0x75433179), whose RpcDispatch hands
// each record to the handler CtlServerInit stored. That server's
// sceSifRpcLoop never runs here, so replies go to the handler at the next
// CtlClientPoll instead, as GH Classic does.
//
// Deliberately NOT ported, for fidelity: GH Classic's lockClock, which pulls
// StreamEE's song clock toward the audio position. GH1 keeps its own rule
// (StreamEE::Poll snaps the clock only past 100 ms of drift).

#include "synth.h"

#include "audio.h"
#include "movie_audio.h"
#include "guest.h"
#include "hook.h"
#include "ps2_runtime_macros.h"
#include "ps2x/iop/iop_subsystem.h"
#include "synth/module.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <iostream>
#include <memory>
#include <set>
#include <vector>

namespace ghr
{
    namespace
    {
        using gh2::load;

        constexpr uint32_t kSynthSid = 0x75433178u;

        const Addresses *s_addresses = nullptr;
        uint8_t *s_rdram = nullptr;
        gh2::synth::Module s_module;
        int s_audioOffsetMs = 0;
        std::atomic<uint64_t> s_framesRendered{0}; // for the trace: where in the output a position was read
        std::vector<gh2::synth::Reply> s_held;
        std::set<uint32_t> s_seenCommands;

        class Synth final : public ps2x::iop::IopService
        {
        public:
            [[nodiscard]] std::string_view name() const override
            {
                return "gh2 synth";
            }

            [[nodiscard]] std::span<const uint32_t> sids() const override
            {
                return kSids;
            }

            // Dormant until the game loads SYNTH_S.IRX, as the real module is.
            [[nodiscard]] std::span<const std::string_view> moduleAliases() const override
            {
                return kModuleAliases;
            }

            void reset() override
            {
                s_module.reset();
                s_module.setSamplesAfterStreamHeap(false);
                s_module.setStreamShiftMs(s_audioOffsetMs);
            }

            [[nodiscard]] ps2x::iop::RpcResult handleRpc(const ps2x::iop::RpcRequest &request) override
            {
                ps2x::iop::RpcResult result;
                result.handled = request.sid == kSynthSid;
                result.resultAddress = request.receive.address;
                if (!result.handled || !s_rdram)
                    return result;
                uint32_t at = request.send.address;
                const uint32_t end = request.send.address + request.send.size;
                while (at + 8u <= end)
                {
                    const uint32_t cmd = load<uint32_t>(s_rdram, at);
                    const int32_t len = load<int32_t>(s_rdram, at + 4u);
                    const uint32_t data = at + 8u;
                    if (len < 0 || data + static_cast<uint32_t>(len) > end)
                        break;
                    // GH1's command set is checked against GH2's as it shows up.
                    if (s_seenCommands.insert(cmd).second)
                        std::cerr << "[gh2 synth] first command " << cmd << " len " << len << std::endl;
                    // GH1_TRACE=synthcmd: every command but the per-tick
                    // stream feed (408/409), with its first words.
                    static const bool traceCmds = std::getenv("GH1_TRACE") && std::strstr(std::getenv("GH1_TRACE"), "synthcmd");
                    if (traceCmds && cmd != 409u)
                    {
                        std::cerr << "[gh2 synthcmd] " << cmd << " len " << len;
                        for (int32_t w = 0; w + 4 <= len && w < 16; w += 4)
                            std::cerr << ' ' << load<int32_t>(s_rdram, data + static_cast<uint32_t>(w));
                        std::cerr << std::endl;
                    }
                    s_module.command(cmd, getMemPtr(s_rdram, data), static_cast<uint32_t>(len));
                    at = (data + static_cast<uint32_t>(len) + 3u) & ~3u;
                }
                return result;
            }

        private:
            inline static constexpr std::array<uint32_t, 1> kSids{kSynthSid};
            inline static constexpr std::array<std::string_view, 1> kModuleAliases{"synth_s"};
        };

        // GH Classic's module answers in GH2's numbering. GH1's
        // SynthEE::CtlDispatch_impl (0x2d8680) has 14 cases, read from its
        // jump table at 0x343940 against GH2's (0x466540, 15 cases) and
        // compared case by case:
        //   0-8   stream data, end, ready, position, slip offset, and the
        //         MicEE/state cases: the same code at GH1's offsets.
        //   GH2 9 (SampleInstEE::UpdateAddr, playback progress): GH1 has no
        //         such case; dropped.
        //   GH2 11 (SampleInstEE::SetDone, a sound effect finished): GH1's
        //         case 11 calls its own SetDone (0x2194b0, the same vector
        //         push as GH2's 0x22e3a8). Passed as 11. Dropping it (as
        //         this layer first did, thinking GH1 sent no 700 commands)
        //         left every menu sound playing forever to GH1, and the main
        //         menu, which waits for its sounds, took no input.
        //   GH2 13 (SPUSetSendDone): GH1's case 9 clears the upload-in-flight
        //         flag (+0x58 of *0x2f1d14) that SynthEE::Handle (0x21a740)
        //         sets before commands 200/201 and checks before the next
        //         upload. Sent as 13, GH1 never saw its first upload finish
        //         and the boot waited forever.
        //   GH2 14 (terminated, sets +0x4130): GH1's case 13 (sets +0x40d4).
        constexpr uint32_t kDropped = ~0u;
        // GH2's own numbering: the module's replies go as they are.
        [[maybe_unused]] uint32_t gh1Reply(uint32_t gh2)
        {
            switch (gh2)
            {
            case 9u:
                return kDropped;
            case 13u:
                return 9u;
            case 14u:
                return 13u;
            default:
                return gh2;
            }
        }

        // Each reply goes to the handler as RpcDispatch would pass it: cmd,
        // its data (null when empty) and its length, staged in the server's
        // receive buffer.
        void deliver(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            s_module.takeReplies(s_held);
            const uint32_t handler = load<uint32_t>(rdram, s_addresses->synthServerHandler);
            static const bool trace = std::getenv("GH1_TRACE") && std::strstr(std::getenv("GH1_TRACE"), "synth");
            static uint32_t polls = 0;
            if (trace && (polls++ % 60u == 0u))
            {
                std::cerr << "[gh2 synth] poll #" << polls << " handler=" << std::hex << handler << std::dec
                          << " frames=" << s_framesRendered.load(std::memory_order_relaxed) << " starves="
                          << s_module.starves() << " replies";
                for (const auto &r : s_held)
                {
                    std::cerr << ' ' << r.cmd;
                    if (r.data.size() >= 8)
                    {
                        int32_t w0, w1;
                        std::memcpy(&w0, r.data.data(), 4);
                        std::memcpy(&w1, r.data.data() + 4, 4);
                        std::cerr << '(' << w0 << ',' << w1 << ')';
                    }
                    else if (r.data.size() >= 4)
                    {
                        int32_t w0;
                        std::memcpy(&w0, r.data.data(), 4);
                        std::cerr << '(' << w0 << ')';
                    }
                }
                std::cerr << std::endl;
            }
            if (handler == 0u)
                return; // the server is not up yet; they keep for the next poll
            std::vector<gh2::synth::Reply> replies;
            replies.swap(s_held);
            for (const gh2::synth::Reply &reply : replies)
            {
                const uint32_t cmd = reply.cmd;
                if (cmd == kDropped)
                    continue;
                uint32_t data = 0u;
                if (!reply.data.empty())
                {
                    data = s_addresses->synthServerBuffer;
                    std::memcpy(getMemPtr(rdram, data), reply.data.data(), reply.data.size());
                }
                runtime->callGuestFunction(rdram, ctx, handler,
                                           {cmd, data, static_cast<uint32_t>(reply.data.size())});
            }
        }

        struct PollTag;

        void render(float *out, size_t frames)
        {
            s_module.render(out, frames);
            s_framesRendered.fetch_add(frames, std::memory_order_relaxed);
            mixMovieAudio(out, frames); // the SPU2's sound data input: the movies' sound
        }
    }

    void installSynth(PS2Runtime &runtime, const Addresses &addresses, int audioOffsetMs)
    {
        s_addresses = &addresses;
        // GH2's IRX gives upload and sample addresses as they are.
        s_module.setSamplesAfterStreamHeap(false);
        s_audioOffsetMs = audioOffsetMs;
        s_module.setStreamShiftMs(audioOffsetMs);
        if (audioOffsetMs != 0)
            std::cerr << "[gh2] audio_offset = " << audioOffsetMs << " ms: the songs' audio "
                      << (audioOffsetMs > 0 ? "later" : "earlier") << " against the notes" << std::endl;
        s_rdram = runtime.memory().getRDRAM();
        runtime.iop().addService(std::make_unique<Synth>());
        gh2::EntryHook<PollTag>::install(runtime, addresses.ctlClientPoll, deliver);
        startAudio(&render, runtime.hostOptions().mute);
    }
}
