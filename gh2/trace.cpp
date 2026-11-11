// Development traces, off unless GH1_TRACE is set. Each only reads
// arguments on entry to a GH1 function and changes nothing.
//
//   GH1_TRACE=files   ArkFile::ArkFile(name, mode) and DataReadFile(name)
//   GH1_TRACE=modal   DebugModal / PsModal(bool &, char *msg): the message
//   GH1_TRACE=ui      UIScreen::Enter / UIManager::PushScreen, PopScreen, and
//                     which screen draws (every 120th UIScreen::Draw)
//   GH1_TRACE=draw    calls per 120 frames to PsMesh::DrawShowing, DrawFaces,
//                     PsMultiMesh::DrawShowing and RndText::DrawShowing
//   GH1_TRACE=stack   every 120th PsRnd::BeginDrawing: return addresses on
//                     the guest stack, for tools to name offline
//   GH1_TRACE=padmode port 1's mode byte and left stick Y from scePadRead,
//                     scePadInfoMode, scePadSetMainMode, scePadPortOpen
//   GH1_TRACE=threads every 10 s of host time: each guest thread's state,
//                     what it waits on, its PC and RA, and the semaphores

#include "trace.h"

#include "guest.h"
#include "hook.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <thread>

namespace ghr
{
    namespace
    {
        std::string guestString(uint8_t *rdram, uint32_t address)
        {
            std::string out;
            for (uint32_t i = 0; address != 0u && i < 256u; ++i)
            {
                const char c = static_cast<char>(gh2::load<uint8_t>(rdram, address + i));
                if (c == '\0')
                    break;
                out.push_back(c);
            }
            return out;
        }

        struct ArkFileTag;
        struct PadDownTag;
        struct JoyPollTag;
        struct SysPollTag;
        struct CompactTag;
        struct MoveBlockTag;
        struct CopyQwTag;
        struct JoyClientPollTag;
        struct PadUpTag;
        struct DataReadTag;
        struct DebugModalTag;
        struct DebugModal2Tag;
        struct PsModalTag;
        struct BeginTag;
        struct EnterTag;
        struct PushTag;
        struct PopTag;
        struct ScreenDrawTag;
        struct GotoTag;
        struct MeshShowTag;
        struct MeshFacesTag;
        struct MultiShowTag;
        struct TextShowTag;
        struct FrameTag;
        uint32_t s_draws[8] = {};
        struct MovStartTag;
        struct MovEndTag;
        struct MovFrameTag;
        struct MovChunkTag;
        void onMovStart(uint8_t *, R5900Context *, PS2Runtime *) { ++s_draws[4]; }
        void onMovEnd(uint8_t *, R5900Context *, PS2Runtime *) { ++s_draws[5]; }
        void onMovFrame(uint8_t *, R5900Context *, PS2Runtime *) { ++s_draws[6]; }
        void onMovChunk(uint8_t *, R5900Context *, PS2Runtime *) { ++s_draws[7]; }
        void onMeshShow(uint8_t *, R5900Context *, PS2Runtime *) { ++s_draws[0]; }
        void onMeshFaces(uint8_t *, R5900Context *, PS2Runtime *) { ++s_draws[1]; }
        void onMultiShow(uint8_t *, R5900Context *, PS2Runtime *) { ++s_draws[2]; }
        void onTextShow(uint8_t *, R5900Context *, PS2Runtime *) { ++s_draws[3]; }
        void onFrame(uint8_t *, R5900Context *, PS2Runtime *)
        {
            static uint32_t frames = 0;
            if (++frames % 120u == 0u)
            {
                std::cerr << "[gh2 draw] frame " << frames << " PsMesh::DrawShowing=" << s_draws[0]
                          << " DrawFaces=" << s_draws[1] << " PsMultiMesh=" << s_draws[2]
                          << " RndText=" << s_draws[3] << " | PsMovie StartDecode=" << s_draws[4]
                          << " EndDecode=" << s_draws[5] << " SetFrame=" << s_draws[6] << " StreamChunk=" << s_draws[7]
                          << std::endl;
                std::fill(std::begin(s_draws), std::end(s_draws), 0u);
            }
        }
        struct UiPollTag;
        struct PanelsLoadedTag;
        std::string objectName(uint8_t *rdram, uint32_t object);

        // UIManager::Poll, 0x209ec0 (placed by order; the main loop calls it
        // every frame): transition state +0x80, current screen +0x64, next
        // screen +0x74, logged when they change.
        uint32_t s_panelsLoadedCalls = 0;
        void onUiPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            static uint32_t last[3] = {~0u, ~0u, ~0u};
            static uint32_t polls = 0;
            const uint32_t self = GPR_U32(ctx, 4);
            const uint32_t now[3] = {gh2::load<uint32_t>(rdram, self + 0x80u), gh2::load<uint32_t>(rdram, self + 0x64u),
                                     gh2::load<uint32_t>(rdram, self + 0x74u)};
            ++polls;
            if (now[0] != last[0] || now[1] != last[1] || now[2] != last[2] || polls % 600u == 0u)
            {
                std::cerr << std::hex << "[gh2 ui] Poll #" << std::dec << polls << std::hex << " this=" << self
                          << " state=" << now[0] << " current=" << now[1] << " next=" << now[2] << std::dec
                          << " ArePanelsLoaded calls=" << s_panelsLoadedCalls << std::endl;
                std::copy(now, now + 3, last);
            }
        }
        void onPanelsLoaded(uint8_t *, R5900Context *, PS2Runtime *)
        {
            ++s_panelsLoadedCalls;
        }

        // UIManager::GotoScreen(UIScreen *, bool, bool), 0x20a4b0: placed by
        // order (GH2: CancelTransition, GotoScreen, PushScreen, PopScreen).
        void onGoto(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            std::cerr << "[gh2 ui] GotoScreen" << objectName(rdram, GPR_U32(ctx, 5)) << " flags " << GPR_U32(ctx, 6)
                      << "," << GPR_U32(ctx, 7) << std::endl;
        }

        // An Hmx::Object's name: printed as every word of its first 0x40 bytes
        // that points at a string, until GH1's offset is known.
        std::string objectName(uint8_t *rdram, uint32_t object)
        {
            if (object == 0u)
                return "(null)";
            // Every word of the object's first 0x40 bytes that points at a
            // printable string (GH1's name offset is not GH2's +0x14).
            std::string out;
            for (uint32_t off = 0u; off < 0x40u; off += 4u)
            {
                const uint32_t p = gh2::load<uint32_t>(rdram, object + off);
                if (p <= 0x100000u || p >= 0x2000000u)
                    continue;
                std::string s = guestString(rdram, p);
                if (s.size() < 3u || !std::all_of(s.begin(), s.end(), [](char c) { return c >= 0x20 && c < 0x7f; }))
                    continue;
                if (s.size() > 32u)
                    s.resize(32u);
                out += " +" + std::to_string(off) + "='" + s + "'";
            }
            return out;
        }

        // UIScreen::Enter(UIScreen *from), 0x208878: this, from.
        void onEnter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            std::cerr << "[gh2 ui] Enter" << objectName(rdram, GPR_U32(ctx, 4)) << " from"
                      << objectName(rdram, GPR_U32(ctx, 5)) << std::endl;
        }
        void onPush(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            std::cerr << "[gh2 ui] PushScreen" << objectName(rdram, GPR_U32(ctx, 5)) << std::endl;
        }
        void onPop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            std::cerr << "[gh2 ui] PopScreen" << objectName(rdram, GPR_U32(ctx, 5)) << std::endl;
        }
        void onScreenDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            static uint32_t calls = 0;
            if (calls++ % 120u == 0u)
                std::cerr << "[gh2 ui] Draw #" << calls << objectName(rdram, GPR_U32(ctx, 4)) << std::endl;
        }

        // PsRnd::BeginDrawing, 0x19b480. A return address on the stack is a
        // word pointing into .text just past a jal or jalr (and its delay slot).
        void onBegin(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            static uint32_t calls = 0;
            if (calls++ % 120u != 0u)
                return;
            const uint32_t sp = GPR_U32(ctx, 29);
            std::cerr << "[gh2 stack] ra=" << std::hex << GPR_U32(ctx, 31) << " chain";
            int found = 0;
            for (uint32_t off = 0; off < 0x2000u && found < 24; off += 4u)
            {
                const uint32_t v = gh2::load<uint32_t>(rdram, sp + off);
                if (v < 0x100008u || v >= 0x466694u || (v & 3u))
                    continue;
                const uint32_t call = gh2::load<uint32_t>(rdram, v - 8u);
                const bool jal = (call >> 26) == 3u;
                const bool jalr = (call & 0xFC1F07FFu) == 0x00000009u;
                if (jal || jalr)
                {
                    std::cerr << ' ' << v;
                    ++found;
                }
            }
            std::cerr << std::dec << std::endl;
        }

        // DebugModal(bool &, char *) at 0x104678 and 0x21e860, PsModal at
        // 0x199fb8 (all matched exactly): the message is the second argument.
        void onModal(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            static int printed = 0;
            if (printed++ < 20)
                std::cerr << "[gh2 modal] " << guestString(rdram, GPR_U32(ctx, 5)) << std::endl;
        }

        std::map<uint32_t, std::string> s_arkNames; // by ArkFile
        std::map<uint32_t, int> s_pending;           // ReadAsync without ReadDone, by ArkFile

        // ArkFile::ArkFile(const char *, int), 0x227e48: this, name, mode.
        void onArkFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const std::string name = guestString(rdram, GPR_U32(ctx, 5));
            s_arkNames[GPR_U32(ctx, 4)] = name;
            std::cerr << "[gh2 files] ArkFile " << name << " mode " << GPR_U32(ctx, 6) << std::endl;
        }

        std::string arkName(uint32_t self)
        {
            const auto it = s_arkNames.find(self);
            return it == s_arkNames.end() ? std::string("?") : it->second;
        }

        // ArkFile::ReadAsync(void *, int), 0x227fe8, and ArkFile::ReadDone(int &),
        // 0x228278 (both matched exactly).
        struct ReadAsyncTag;
        struct ReadDoneTag;
        void onReadAsync(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t self = GPR_U32(ctx, 4);
            ++s_pending[self];
            static uint64_t calls = 0;
            if (++calls < 3000u || calls % 20000u == 0u)
                std::cerr << "[gh2 files] ReadAsync " << arkName(self) << " bytes " << GPR_U32(ctx, 6) << " tick "
                          << runtime->memory().gs().vsyncTick.load() << std::hex << " ra " << GPR_U32(ctx, 31)
                          << " buf " << GPR_U32(ctx, 5) << std::dec << " #" << calls << std::endl;
        }
        void onReadDone(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            static uint32_t calls = 0;
            const uint32_t self = GPR_U32(ctx, 4);
            if (calls++ < 4000u)
                std::cerr << "[gh2 files] ReadDone? " << arkName(self) << std::endl;
        }

        // JoypadClient::OnMsg(const ButtonDownMsg &) 0x222658 and
        // (const ButtonUpMsg &) 0x222968: the presses the game's input layer
        // turns into messages, by VBlank.
        void onPadDown(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            std::cerr << "[gh2 pad] down tick " << runtime->memory().gs().vsyncTick.load() << " msg "
                      << std::hex << GPR_U32(ctx, 5) << std::dec << std::endl;
            (void)rdram;
        }
        // JoypadClientPoll() 0x2223e8 and JoypadClient::Poll 0x222380, counted.
        void onJoyPoll(uint8_t *, R5900Context *, PS2Runtime *runtime)
        {
            static uint32_t n = 0;
            if (++n % 30u == 0u)
                std::cerr << "[gh2 pad] JoypadClientPoll #" << n << " tick " << runtime->memory().gs().vsyncTick.load() << std::endl;
        }
        // Heap::Compact(int, bool) 0x23b560, its block move 0x23ba28 and its
        // quadword copy 0x23b520: GH1_TRACE=heap.
        void onCompact(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t heap = GPR_U32(ctx, 4);
            static uint32_t n = 0;
            if (n++ % 100u != 0u && gh2::load<uint32_t>(rdram, heap) != 0u)
                return;
            std::cerr << "[gh2 heap] Compact heap " << std::hex << heap << " words";
            for (uint32_t i = 0; i < 8; ++i)
                std::cerr << ' ' << gh2::load<uint32_t>(rdram, heap + i * 4u);
            // The other heaps, for their sizes.
            std::cerr << " | heaps";
            for (uint32_t h = 0; h < 4; ++h)
                std::cerr << " [" << gh2::load<uint32_t>(rdram, 0x421e80u + h * 32u + 4u) << ' '
                          << gh2::load<uint32_t>(rdram, 0x421e80u + h * 32u + 8u) << ']';
            std::cerr << std::dec << " max " << GPR_U32(ctx, 5) << " tick " << runtime->memory().gs().vsyncTick.load() << std::endl;
        }
        void onMoveBlock(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
        {
            static uint32_t n = 0;
            if (n++ < 40u || n % 5000u == 0u)
                std::cerr << "[gh2 heap] move #" << n << " a0 " << std::hex << GPR_U32(ctx, 4) << " a1 " << GPR_U32(ctx, 5)
                          << " a2 " << GPR_U32(ctx, 6) << " a3 " << GPR_U32(ctx, 7) << std::dec << " tick "
                          << runtime->memory().gs().vsyncTick.load() << std::endl;
        }
        void onCopyQw(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
        {
            static uint32_t n = 0;
            if (n++ < 40u || n % 5000u == 0u)
                std::cerr << "[gh2 heap] copy #" << n << " from " << std::hex << GPR_U32(ctx, 4) << " to " << GPR_U32(ctx, 5)
                          << std::dec << " words " << GPR_U32(ctx, 6) << " tick " << runtime->memory().gs().vsyncTick.load() << std::endl;
        }
        void onSysPoll(uint8_t *, R5900Context *, PS2Runtime *runtime)
        {
            static uint32_t n = 0;
            if (++n % 30u == 0u)
                std::cerr << "[gh2 pad] SystemPoll #" << n << " tick " << runtime->memory().gs().vsyncTick.load() << std::endl;
        }
        void onJoyClientPoll(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
        {
            static uint32_t n = 0;
            if (++n % 300u == 0u)
                std::cerr << "[gh2 pad] JoypadClient::Poll #" << n << " this " << std::hex << GPR_U32(ctx, 4) << std::dec
                          << " tick " << runtime->memory().gs().vsyncTick.load() << std::endl;
        }
        void onPadUp(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            std::cerr << "[gh2 pad] up tick " << runtime->memory().gs().vsyncTick.load() << std::endl;
            (void)rdram;
            (void)ctx;
        }

        // libpad calls (all runtime handlers, so they return at once), each
        // logged when its arguments or result change: GH1_TRACE=libpad.
        template <typename Tag>
        struct PadCallTrace
        {
            static void install(PS2Runtime &runtime, uint32_t address, const char *name)
            {
                s_original = runtime.lookupFunction(address);
                s_name = name;
                runtime.replaceFunction(address, &run);
            }

        private:
            static void run(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
            {
                const uint32_t a0 = GPR_U32(ctx, 4), a1 = GPR_U32(ctx, 5), a2 = GPR_U32(ctx, 6);
                s_original(rdram, ctx, runtime);
                const uint32_t ret = GPR_U32(ctx, 2);
                const uint64_t key = (uint64_t(a0) << 48) ^ (uint64_t(a1) << 40) ^ (uint64_t(a2) << 20) ^ ret;
                if (key != s_last)
                {
                    s_last = key;
                    std::cerr << "[gh2 libpad] tick " << runtime->memory().gs().vsyncTick.load() << ' ' << s_name
                              << '(' << a0 << ", " << a1 << ", " << std::hex << a2 << std::dec << ") = " << int32_t(ret)
                              << std::endl;
                }
            }

            inline static PS2Runtime::RecompiledFunction s_original = nullptr;
            inline static const char *s_name = "";
            inline static uint64_t s_last = ~0ull;
        };
        // scePadRead(port, slot, data): the mode byte the game gets (0x73
        // DualShock/analog, 0x41 digital) and the left stick's Y, logged
        // when the mode changes or the stick leaves rest.
        struct PadReadData
        {
            static void install(PS2Runtime &runtime, uint32_t address)
            {
                s_original = runtime.lookupFunction(address);
                runtime.replaceFunction(address, &run);
            }

        private:
            static void run(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
            {
                const uint32_t port = GPR_U32(ctx, 4), data = GPR_U32(ctx, 6);
                s_original(rdram, ctx, runtime);
                if (port != 0u)
                    return;
                const uint8_t mode = gh2::load<uint8_t>(rdram, data + 1u);
                const uint8_t ly = gh2::load<uint8_t>(rdram, data + 7u);
                static uint8_t lastMode = 0, lastLy = 0;
                if (mode != lastMode || (ly != lastLy && (ly < 0x60u || lastLy < 0x60u)))
                    std::cerr << "[gh2 libpad] tick " << runtime->memory().gs().vsyncTick.load() << std::hex
                              << " read mode " << int(mode) << " ly " << int(ly) << std::dec << std::endl;
                lastMode = mode;
                lastLy = ly;
            }
            inline static PS2Runtime::RecompiledFunction s_original = nullptr;
        };

        struct PadReadTag;
        struct PadStateTag;
        struct PadInfoModeTag;
        struct PadMainModeTag;
        struct PadOpenTag;
        struct PadReqTag;

        // DataReadFile(const char *), 0x2307f0 (matched exactly).
        void onDataRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            std::cerr << "[gh2 files] DataReadFile " << guestString(rdram, GPR_U32(ctx, 4)) << std::endl;
        }
    }

    namespace
    {
        // The star power meter's Poll (0x136928, this, time f12, value f13):
        // every 30th call, the time it is given against the host's clock,
        // the meter's shown value (+4), fill speed (+0x24, per time unit) and
        // ready level (+0x28): GH1 lets star power deploy only once the shown
        // value reaches the ready level.
        struct MeterTag;
        void onMeterPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            static uint32_t calls = 0;
            static const auto start = std::chrono::steady_clock::now();
            const uint32_t self = GPR_U32(ctx, 4);
            float shown, speed, ready;
            const uint32_t s = gh2::load<uint32_t>(rdram, self + 4u), sp = gh2::load<uint32_t>(rdram, self + 0x24u),
                           rd = gh2::load<uint32_t>(rdram, self + 0x28u);
            std::memcpy(&shown, &s, 4);
            std::memcpy(&speed, &sp, 4);
            std::memcpy(&ready, &rd, 4);
            static float lastShown = -1.0f;
            if (calls++ % 30u == 0u || shown != lastShown)
            {
                const double host =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                std::cerr << "[gh2 meter] host " << host << " time " << ctx->f[12] << " value " << ctx->f[13]
                          << " shown " << shown << " speed " << speed << " ready " << ready << std::endl;
            }
            lastShown = shown;
        }

        void startThreadDump(PS2Runtime &runtime)
        {
            std::thread([&runtime] {
                for (;;)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(10));
                    const EeKernelSnapshot k = runtime.eeScheduler().snapshot();
                    std::cerr << "[gh2 threads] seq " << k.sequence << " running " << k.runningThreadId << "\n";
                    for (const EeThreadSnapshot &t : k.threads)
                        std::cerr << "  thread " << t.id << " status " << static_cast<int>(t.status) << " wait "
                                  << static_cast<int>(t.waitReason) << ":" << t.waitId << " pc " << std::hex << t.pc
                                  << " ra " << t.ra << " sp " << t.sp << " entry " << t.entry << std::dec << " prio "
                                  << t.currentPriority << " inv " << t.invocationDepth << "\n";
                    for (const EeSemaphoreSnapshot &s : k.semaphores)
                        if (s.waiters != 0u)
                            std::cerr << "  sema " << s.id << " count " << s.count << "/" << s.maxCount << " waiters "
                                      << s.waiters << "\n";
                    for (const EeEventFlagSnapshot &f : k.eventFlags)
                        if (f.waiters != 0u)
                            std::cerr << "  flag " << f.id << " bits " << std::hex << f.bits << std::dec << " waiters "
                                      << f.waiters << "\n";
                    static const uint32_t kRegs[] = {0x10009000u, 0x10009010u, 0x10009020u, 0x10009030u,
                                                     0x1000D000u, 0x1000D010u, 0x1000D020u, 0x1000E000u,
                                                     0x1000E010u, 0x1000E020u, 0x1000E040u, 0x1000E050u,
                                                     0x10003C00u};
                    std::cerr << "  dmac";
                    for (uint32_t r : kRegs)
                        std::cerr << " " << std::hex << r << "=" << runtime.memory().readIORegister(r);
                    std::cerr << std::dec << "\n";
                    const uint32_t tadr = runtime.memory().readIORegister(0x10009030u) & 0x1ffffff0u;
                    uint8_t *rdram = runtime.memory().getRDRAM();
                    for (uint32_t i = 0; i < 4u; ++i)
                        std::cerr << "  ring " << std::hex << tadr + i * 8u << ": "
                                  << gh2::load<uint32_t>(rdram, tadr + i * 8u + 4u) << " "
                                  << gh2::load<uint32_t>(rdram, tadr + i * 8u) << std::dec << "\n";
                    std::cerr << std::flush;
                }
            }).detach();
        }
    }

    // GH1_TRACE=sdr: the movie sound's calls to libsd on the IOP
    // (sceSdRemote: command, arguments, result) and the EE's SIF DMA to the
    // IOP, the first few hundred.
    struct SdRemoteTag;
    bool onSdRemote(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static unsigned n = 0;
        const uint32_t a0 = GPR_U32(ctx, 4), cmd = GPR_U32(ctx, 5), a2 = GPR_U32(ctx, 6), a3 = GPR_U32(ctx, 7);
        const uint32_t t0 = GPR_U32(ctx, 8), t1 = GPR_U32(ctx, 9), t2 = GPR_U32(ctx, 10), ra = GPR_U32(ctx, 31);
        gh2::Override<SdRemoteTag>::original()(rdram, ctx, runtime);
        if (n++ < 400)
            std::fprintf(stderr, "[sdr] %llu ra %x sceSdRemote(%x, cmd %x, %x, %x, %x, %x, %x) -> %x\n",
                         static_cast<unsigned long long>(runtime->memory().gs().vsyncTick.load()), ra, a0, cmd, a2, a3, t0,
                         t1, t2, GPR_U32(ctx, 2));
        return true;
    }
    struct SifDmaTag;
    void onSifDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static unsigned n = 0;
        const uint32_t list = GPR_U32(ctx, 4), count = GPR_U32(ctx, 5);
        // GH1_RINGDUMP=<file> (developers): the movie sound's EE ring as it
        // is when the 6th transfer from its start goes out.
        static unsigned fromStart = 0;
        static const char *ringDump = std::getenv("GH1_RINGDUMP");
        if (ringDump && count > 0u && gh2::load<uint32_t>(rdram, list) == 0x1ad1fc0u && ++fromStart == 6u)
            if (std::FILE *f = std::fopen(ringDump, "wb"))
            {
                std::fwrite(getMemPtr(rdram, 0x1ad1fc0u), 1, 0xc000u, f);
                std::fclose(f);
            }
        for (uint32_t i = 0; i < count && i < 4 && n < 300; ++i, ++n)
        {
            const uint32_t src = gh2::load<uint32_t>(rdram, list + i * 16u);
            std::fprintf(stderr, "[sdr] %llu ra %x SifSetDma src %x dst %x size %x attr %x data %08x %08x %08x %08x\n",
                         static_cast<unsigned long long>(runtime->memory().gs().vsyncTick.load()), GPR_U32(ctx, 31),
                         src, gh2::load<uint32_t>(rdram, list + i * 16u + 4u),
                         gh2::load<uint32_t>(rdram, list + i * 16u + 8u), gh2::load<uint32_t>(rdram, list + i * 16u + 12u),
                         gh2::load<uint32_t>(rdram, src), gh2::load<uint32_t>(rdram, src + 4u),
                         gh2::load<uint32_t>(rdram, src + 8u), gh2::load<uint32_t>(rdram, src + 12u));
        }
    }

    // GH1_TRACE=sdr: each PCM packet libmpeg hands the movie player
    // (StreamPCM(mpeg, sceMpegCbDataStr *, void *), 0x21b980).
    struct StreamPcmTag;
    void onStreamPcm(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static unsigned n = 0;
        if (n++ >= 60)
            return;
        const uint32_t cb = GPR_U32(ctx, 5);
        uint32_t w[6];
        for (uint32_t i = 0; i < 6; ++i)
            w[i] = gh2::load<uint32_t>(rdram, cb + i * 4u);
        const uint32_t data = w[2];
        uint32_t hash = 2166136261u; // FNV-1a of the packet as the callback sees it
        for (uint32_t i = 0; i < w[3] && i < 0x10000u; ++i)
            hash = (hash ^ gh2::load<uint8_t>(rdram, data + i)) * 16777619u;
        std::fprintf(stderr, "[sdr] %llu StreamPCM cb %x: %x %x data %x len %x %x %x bytes %08x %08x %08x fnv %08x\n",
                     static_cast<unsigned long long>(runtime->memory().gs().vsyncTick.load()), cb, w[0], w[1], data, w[3],
                     w[4], w[5], gh2::load<uint32_t>(rdram, data), gh2::load<uint32_t>(rdram, data + 4u),
                     gh2::load<uint32_t>(rdram, data + 8u), hash);
    }

    // GH1_TRACE=sdr: audioDec's state at each EndPut (0x21cfa8, ad, size)
    // and Reset (0x21ceb0).
    struct AdEndPutTag;
    void onAdEndPut(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static unsigned n = 0;
        if (n++ >= 40)
            return;
        const uint32_t ad = GPR_U32(ctx, 4);
        std::fprintf(stderr, "[sdr] %llu audioDecEndPut ad %x size %x words",
                     static_cast<unsigned long long>(runtime->memory().gs().vsyncTick.load()), ad, GPR_U32(ctx, 5));
        for (uint32_t i = 0; i < 24; ++i)
            std::fprintf(stderr, " %x", gh2::load<uint32_t>(rdram, ad + i * 4u));
        std::fprintf(stderr, "\n");
    }
    struct AdResetTag;
    void onAdReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::fprintf(stderr, "[sdr] %llu audioDecReset ad %x ra %x\n",
                     static_cast<unsigned long long>(runtime->memory().gs().vsyncTick.load()), GPR_U32(ctx, 4),
                     GPR_U32(ctx, 31));
    }

    // GH1_TRACE=scores: high score entries set, lists made and added to.
    struct ScoreSetTag;
    void onScoreSet(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static unsigned n = 0;
        if (n++ > 600)
            return;
        const uint32_t name = GPR_U32(ctx, 5);
        char text[32] = {};
        for (int i = 0; i < 31 && name; ++i)
            if (!(text[i] = static_cast<char>(gh2::load<uint8_t>(rdram, name + i))))
                break;
        std::fprintf(stderr, "[scores] %llu Set entry %x name '%s' score %d ra %x\n",
                     static_cast<unsigned long long>(runtime->memory().gs().vsyncTick.load()), GPR_U32(ctx, 4), text,
                     static_cast<int>(GPR_U32(ctx, 6)), GPR_U32(ctx, 31));
    }
    struct SongScoresTag;
    void onSongScores(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static unsigned n = 0;
        if (n++ > 200)
            return;
        // Symbol: a pointer to its name.
        const uint32_t sym = GPR_U32(ctx, 5);
        char text[48] = {};
        for (int i = 0; i < 47 && sym; ++i)
            if (!(text[i] = static_cast<char>(gh2::load<uint8_t>(rdram, sym + i))))
                break;
        std::fprintf(stderr, "[scores] %llu SongHighScores(%x, '%s') ra %x\n",
                     static_cast<unsigned long long>(runtime->memory().gs().vsyncTick.load()), GPR_U32(ctx, 4), text,
                     GPR_U32(ctx, 31));
    }
    struct AddScoreTag;
    void onAddScore(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::fprintf(stderr, "[scores] %llu HighScoreDB::AddHighScore(%x, name %x, %d) ra %x\n",
                     static_cast<unsigned long long>(runtime->memory().gs().vsyncTick.load()), GPR_U32(ctx, 4),
                     GPR_U32(ctx, 5), static_cast<int>(GPR_U32(ctx, 6)), GPR_U32(ctx, 31));
    }
    struct CheckScoreTag;
    void onCheckScore(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::fprintf(stderr, "[scores] %llu HighScoreDB::CheckHighScore(%x, %d) ra %x\n",
                     static_cast<unsigned long long>(runtime->memory().gs().vsyncTick.load()), GPR_U32(ctx, 4),
                     static_cast<int>(GPR_U32(ctx, 5)), GPR_U32(ctx, 31));
    }
    struct CurrentListTag;
    void onCurrentList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static unsigned n = 0;
        if (n++ > 40)
            return;
        std::fprintf(stderr, "[scores] %llu HighScoreDB::CurrentList(%x) ra %x\n",
                     static_cast<unsigned long long>(runtime->memory().gs().vsyncTick.load()), GPR_U32(ctx, 4),
                     GPR_U32(ctx, 31));
    }

    void installTraces(PS2Runtime &runtime)
    {
        const char *which = std::getenv("GH1_TRACE");
        if (!which)
            return;
        if (std::strstr(which, "scores"))
        {
            gh2::EntryHook<ScoreSetTag>::install(runtime, 0x13c498u, onScoreSet);
            gh2::EntryHook<SongScoresTag>::install(runtime, 0x13cc20u, onSongScores);
            gh2::EntryHook<AddScoreTag>::install(runtime, 0x13d198u, onAddScore);
            gh2::EntryHook<CheckScoreTag>::install(runtime, 0x13d140u, onCheckScore);
            gh2::EntryHook<CurrentListTag>::install(runtime, 0x13d0c0u, onCurrentList);
        }
        if (std::strstr(which, "sdr"))
        {
            gh2::Override<SdRemoteTag>::install(runtime, 0x2f6920u, onSdRemote);
            gh2::EntryHook<SifDmaTag>::install(runtime, 0x2f74a0u, onSifDma);
            gh2::EntryHook<StreamPcmTag>::install(runtime, 0x21b980u, onStreamPcm);
            gh2::EntryHook<AdEndPutTag>::install(runtime, 0x21cfa8u, onAdEndPut);
            gh2::EntryHook<AdResetTag>::install(runtime, 0x21ceb0u, onAdReset);
        }
        if (std::strstr(which, "files"))
        {
            gh2::EntryHook<ArkFileTag>::install(runtime, 0x2aca50u, onArkFile);
            gh2::EntryHook<DataReadTag>::install(runtime, 0x2b2928u, onDataRead);
            gh2::EntryHook<ReadAsyncTag>::install(runtime, 0x2acc10u, onReadAsync);
            gh2::EntryHook<ReadDoneTag>::install(runtime, 0x2acea0u, onReadDone);
        }
        if (std::strstr(which, "modal"))
        {
            gh2::EntryHook<DebugModalTag>::install(runtime, 0x105a88u, onModal);
            gh2::EntryHook<DebugModal2Tag>::install(runtime, 0x2a2538u, onModal);
            gh2::EntryHook<PsModalTag>::install(runtime, 0x1997a0u, onModal);
        }
        if (std::strstr(which, "ui"))
        {
            gh2::EntryHook<EnterTag>::install(runtime, 0x212f40u, onEnter);
            gh2::EntryHook<PushTag>::install(runtime, 0x215060u, onPush);
            gh2::EntryHook<PopTag>::install(runtime, 0x215120u, onPop);
            gh2::EntryHook<ScreenDrawTag>::install(runtime, 0x213140u, onScreenDraw);
            gh2::EntryHook<GotoTag>::install(runtime, 0x214d48u, onGoto);
            gh2::EntryHook<UiPollTag>::install(runtime, 0x214510u, onUiPoll);
            gh2::EntryHook<PanelsLoadedTag>::install(runtime, 0x213590u, onPanelsLoaded);
        }
        if (std::strstr(which, "draw"))
        {
            gh2::EntryHook<MeshShowTag>::install(runtime, 0x3d88d8u, onMeshShow);
            gh2::EntryHook<MeshFacesTag>::install(runtime, 0x3d8768u, onMeshFaces);
            gh2::EntryHook<MultiShowTag>::install(runtime, 0x1a2f00u, onMultiShow);
            gh2::EntryHook<TextShowTag>::install(runtime, 0x1dc380u, onTextShow);
            gh2::EntryHook<FrameTag>::install(runtime, 0x19af38u, onFrame);
            gh2::EntryHook<MovStartTag>::install(runtime, 0x19ecb0u, onMovStart);
            gh2::EntryHook<MovEndTag>::install(runtime, 0x19ec20u, onMovEnd);
            gh2::EntryHook<MovFrameTag>::install(runtime, 0x19eeb0u, onMovFrame);
            gh2::EntryHook<MovChunkTag>::install(runtime, 0x19eb08u, onMovChunk);
        }
        if (std::strstr(which, "pad"))
        {
            gh2::EntryHook<PadDownTag>::install(runtime, 0x2a69a8u, onPadDown);
            gh2::EntryHook<PadUpTag>::install(runtime, 0x2a6cc0u, onPadUp);
            gh2::EntryHook<JoyPollTag>::install(runtime, 0x2a6738u, onJoyPoll);
            gh2::EntryHook<SysPollTag>::install(runtime, 0x2a85d0u, onSysPoll);
            gh2::EntryHook<JoyClientPollTag>::install(runtime, 0x2a66d0u, onJoyClientPoll);
        }
        if (std::strstr(which, "libpad") && !std::strstr(which, "padmode"))
        {
            // Addresses from tools/gh1_stub_gate.py (SDK hash matches).
            PadCallTrace<PadReadTag>::install(runtime, 0x2f2c48u, "scePadRead");
            PadCallTrace<PadStateTag>::install(runtime, 0x2f2cc0u, "scePadGetState");
            PadCallTrace<PadInfoModeTag>::install(runtime, 0x2f2f78u, "scePadInfoMode");
            PadCallTrace<PadMainModeTag>::install(runtime, 0x2f30b0u, "scePadSetMainMode");
            PadCallTrace<PadOpenTag>::install(runtime, 0x2f2908u, "scePadPortOpen");
            PadCallTrace<PadReqTag>::install(runtime, 0x2f2df8u, "scePadGetReqState");
        }
        if (std::strstr(which, "padmode"))
        {
            // Port 1's mode as the game sets and reads it.
            PadReadData::install(runtime, 0x2f2c48u);
            PadCallTrace<PadInfoModeTag>::install(runtime, 0x2f2f78u, "scePadInfoMode");
            PadCallTrace<PadMainModeTag>::install(runtime, 0x2f30b0u, "scePadSetMainMode");
            PadCallTrace<PadOpenTag>::install(runtime, 0x2f2908u, "scePadPortOpen");
        }
        if (std::strstr(which, "heap"))
        {
            gh2::EntryHook<CompactTag>::install(runtime, 0x2cef78u, onCompact);
            gh2::EntryHook<MoveBlockTag>::install(runtime, 0x2cf4c8u, onMoveBlock);
            gh2::EntryHook<CopyQwTag>::install(runtime, 0x2cef40u, onCopyQw);
        }
        if (std::strstr(which, "stack"))
            gh2::EntryHook<BeginTag>::install(runtime, 0x19af38u, onBegin);
        if (std::strstr(which, "threads"))
            startThreadDump(runtime);
        if (std::strstr(which, "meter"))
            (void)&onMeterPoll; // GH1-only address; no GH2 counterpart placed yet
    }
}
