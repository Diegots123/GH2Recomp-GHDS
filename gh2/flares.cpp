// GH2's lens flares (the lights over the stage) and the Z buffer: the same
// code as GH1's (PsRnd::DoPointTests at 0x19a7f0 here), handled the same way.
//
// Each frame PsRnd::DoPointTests (0x19ae48) asks the GS, for every light,
// whether something was drawn in front of it: it reads the one Z buffer
// value under the light back to the EE (sceGsSetDefStoreImage, then SPSM set
// to PSMZ16 in the packet, then sceGsExecStoreImage) and shows the flare
// only when that value is below the light's own depth. Read back wrong (no
// value at all, as the runtime once did), every flare shows at once: a white
// glare over the venue.
//
// A read back to the EE stops it until the GS has drawn everything sent
// before the read, on the GPU too, and the EE then reads the answer: with
// PCSX2's hardware renderers, some milliseconds a frame in a song, in which
// the EE does nothing. These reads are answered instead from the Z buffer as
// the GS last brought it back, a frame or so earlier (the GS brings it back
// again with each of these reads, after the EE has gone on): what a light's
// flare shows a frame late, too little to see. The game decides as ever.

#include "flares.h"

#include "guest.h"
#include "hook.h"
#include "ps2_runtime_macros.h"

#if defined(GH1_PCSX2GS)
#include "pcsx2gs_sdl_host.h"
#endif

#include <cstdlib>
#include <cstring>

namespace ghr
{
    namespace
    {
        constexpr uint32_t kExecStoreImage = 0x2f0688u; // sceGsExecStoreImage(sp, dst)
        constexpr uint32_t kFromPointTests = 0x19a8dcu; // its return address in DoPointTests (0x19a7f0)

        // libgraph's sceGsStoreImage packet: BITBLTBUF, TRXPOS and TRXREG.
        constexpr uint32_t kBitbltbuf = 0x20u;
        constexpr uint32_t kTrxpos = 0x30u;
        constexpr uint32_t kTrxreg = 0x40u;

        uint32_t transferBits(uint32_t psm)
        {
            switch (psm & 0x3Fu)
            {
            case 0x01u: // PSMCT24
            case 0x31u: // PSMZ24
                return 24u;
            case 0x02u: // PSMCT16
            case 0x0Au: // PSMCT16S
            case 0x32u: // PSMZ16
            case 0x3Au: // PSMZ16S
                return 16u;
            default:
                return 32u;
            }
        }

        struct ExecStoreTag;
        bool lateRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
#if defined(GH1_PCSX2GS)
            if (GPR_U32(ctx, 31) != kFromPointTests)
                return false;
            const uint32_t sp = GPR_U32(ctx, 4);
            const uint32_t dst = GPR_U32(ctx, 5);
            const uint64_t bitbltbuf = gh2::load<uint64_t>(rdram, sp + kBitbltbuf);
            const uint64_t trxpos = gh2::load<uint64_t>(rdram, sp + kTrxpos);
            const uint64_t trxreg = gh2::load<uint64_t>(rdram, sp + kTrxreg);
            const uint32_t width = static_cast<uint32_t>(trxreg) & 0xFFFu;
            const uint32_t height = static_cast<uint32_t>(trxreg >> 32) & 0xFFFu;
            const uint32_t bytes = (width * height * transferBits(static_cast<uint32_t>(bitbltbuf >> 24)) + 7u) / 8u;
            // Only the one-value reads this is for; anything bigger is read as ever.
            if (bytes == 0u || bytes > 16u)
                return false;

            // The transfer's registers to the GS through the GIF, as the
            // runtime's sceGsExecStoreImage sends them.
            const uint32_t pkt = runtime->guestMalloc(80u, 16u);
            if (!pkt)
                return false;
            const uint64_t q[10] = {
                4ull | (1ull << 15) | (1ull << 60), 0xEull, // GIF tag: 4 A+D, EOP
                bitbltbuf, 0x50ull, trxpos, 0x51ull, trxreg, 0x52ull,
                1ull, 0x53ull, // TRXDIR: local to host
            };
            std::memcpy(getMemPtr(rdram, pkt), q, sizeof(q));
            PS2Memory &mem = runtime->memory();
            mem.writeIORegister(0x1000A010u, pkt); // GIF MADR
            mem.writeIORegister(0x1000A020u, 5u);  // QWC
            mem.writeIORegister(0x1000A000u, 0x101u); // CHCR: start, normal mode
            mem.processPendingTransfers();
            runtime->guestFree(pkt);

            uint8_t value[16];
            if (!pcsx2gs::readLocalLate(value, bytes, bitbltbuf, trxpos, trxreg))
                return false; // no SDL host: the runtime's own way (registers sent twice, harmless)
            std::memcpy(getMemPtr(rdram, dst), value, bytes);
            SET_GPR_S32(ctx, 2, 0);
            gh2::leave(ctx);
            return true;
#else
            (void)rdram;
            (void)ctx;
            (void)runtime;
            return false;
#endif
        }
    }

    void installFlares(PS2Runtime &runtime)
    {
        // GH1_FLARES=sync: the flares' reads wait for the GS, as other reads do.
        const char *mode = std::getenv("GH1_FLARES");
        if (mode && std::strcmp(mode, "sync") == 0)
            return;
        gh2::Override<ExecStoreTag>::install(runtime, kExecStoreImage, &lateRead);
    }
}
