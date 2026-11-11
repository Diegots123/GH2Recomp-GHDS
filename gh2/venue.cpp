// venue = off (GH2Recomp.ini): nothing is drawn behind the track, for weak
// GPUs. An extra the player opts into (venue = on is the default), and the
// one place GH2 Recomp changes what the game shows: the stage, the band and
// the crowd are not drawn, on a black background. The game runs exactly as
// ever, its world included (only that panel's drawing is skipped), so song
// timing, input and scoring do not change.

#include "venue.h"

#include "guest.h"
#include "hook.h"
#include "ps2_runtime_macros.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace ghr
{
    namespace
    {
        // GH2's song screen ("game_screen") draws six panels (UIScreen::Draw,
        // 0x213140; GH1_TRACE=venue lists them): GamePanel, a plain UIPanel
        // whose directory is the venue's WorldDir (the stage, the band and
        // the crowd), TrackPanel (the track and the gems), HudPanel (score,
        // rock meter, star power), GHPanel and FadePanel. The world is drawn
        // by WorldDir::DrawShowing (0x26f6b8); with it skipped, the track
        // and everything over it are drawn as ever on a black background.
        constexpr uint32_t kWorldPanelDraw = 0x26f6b8u; // WorldDir::DrawShowing(DrawType)
        constexpr uint32_t kUiScreenDraw = 0x213140u;

        struct WorldDrawTag;
        bool skipWorld(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            gh2::leave(ctx);
            return true;
        }

        // GH1_TRACE=venue: each panel a screen draws, the first time. GH2's
        // UIScreen::Draw (0x213140) walks the panel list at +0x2c (the list
        // itself is the end), skips a panel whose +0x10 is 0 and calls its
        // Draw, the vtable at +0x38 and the entry at +8 ({delta, -, pfn}).
        struct ScreenDrawTag;
        void onScreenDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t self = GPR_U32(ctx, 4);
            const uint32_t end = self + 0x2cu;
            static std::set<uint64_t> seen;
            uint32_t node = gh2::load<uint32_t>(rdram, end);
            for (int n = 0; node != end && node != 0u && n < 64; ++n, node = gh2::load<uint32_t>(rdram, node))
            {
                const uint32_t panel = gh2::load<uint32_t>(rdram, node + 8u);
                const uint32_t vtable = gh2::load<uint32_t>(rdram, panel + 0x38u);
                if (seen.insert((uint64_t(self) << 32) | panel).second)
                    std::fprintf(stderr, "[gh2 venue] screen %x panel %x vtable %x draw %x\n", self, panel, vtable,
                                 gh2::load<uint32_t>(rdram, vtable + 0xcu));
            }
        }
    }

    void installVenue(PS2Runtime &runtime, bool show)
    {
        if (!show && kWorldPanelDraw != 0u)
        {
            gh2::Override<WorldDrawTag>::install(runtime, kWorldPanelDraw, &skipWorld);
            std::fprintf(stderr, "[gh2] venue = off: the stage, band and crowd are not drawn\n");
        }
        const char *trace = std::getenv("GH1_TRACE");
        if (trace && std::strstr(trace, "venue"))
            gh2::EntryHook<ScreenDrawTag>::install(runtime, kUiScreenDraw, &onScreenDraw);
    }
}
