// Port 1: a RedOctane guitar, or a DualShock 2.
//
// GH1 tells a guitar from a DualShock the way GH2 does (GH Classic,
// src/host/pad.cpp): an analog pad with no motors that holds D-Left is a
// guitar; a DualShock has motors, takes GH1's own joypad layout (frets L2 L1
// R1 R2 X, no strumming, X and triangle in menus) and plays it with GH1's
// joypad rules. The host decides which port 1 is (a guitar when one is
// connected, a DualShock when a gamepad is, else the keyboard's mode) and
// holds D-Left for a guitar; here scePadInfoAct answers no motors for a
// guitar and the runtime's DualShock answer otherwise. When port 1 changes
// kind, the host unplugs it for a moment (scePadGetState answers
// "disconnected"), so GH1 looks at the new controller as it would on a PS2.
//
// The keyboard plays with its game keys while a song plays and with its menu
// keys elsewhere (the pause screen included), and port 1 changes kind only on
// the main menu: UIScreen::Enter tells the host which screen the game is
// going to ("main_screen", "game_screen", ...).

#include "pad.h"

#include "guest.h"
#include "hook.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <string>

#if defined(GH1_PCSX2GS)
#include "pcsx2gs_sdl_host.h"
#endif

namespace ps2_stubs
{
    void scePadInfoAct(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadGetState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void padReplugged(int port, int slot);
}

namespace ghr
{
    namespace
    {
        struct PortOne
        {
            bool plugged = true;
            bool guitar = true;
        };

        PortOne portOne()
        {
#if defined(GH1_PCSX2GS)
            const pcsx2gs::PortOne p = pcsx2gs::portOne();
            return {p.plugged, p.guitar};
#else
            return {};
#endif
        }

        const Addresses *s_addresses = nullptr;

        struct ScreenEnterTag;
        void onScreenEnter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            std::string name;
            const uint32_t screen = GPR_U32(ctx, 4);
            const uint32_t text = screen ? gh2::load<uint32_t>(rdram, screen + s_addresses->screenNameOffset) : 0u;
            for (uint32_t i = 0; text > 0x100000u && text < 0x2000000u && i < 32u; ++i)
            {
                const char c = static_cast<char>(gh2::load<uint8_t>(rdram, text + i));
                if (c == '\0')
                    break;
                name.push_back(c);
            }
#if defined(GH1_PCSX2GS)
            // GH2's screens: "game_screen" while a song plays, "main_screen"
            // for the main menu (GH1_TRACE=ui).
            pcsx2gs::setScreen(name == "game_screen"   ? pcsx2gs::Screen::Song
                               : name == "main_screen" ? pcsx2gs::Screen::MainMenu
                                                       : pcsx2gs::Screen::Other);
#endif
        }

        // scePadInfoAct(port, slot, actno, term).
        void padInfoAct(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            if (getRegU32(ctx, 4) == 0u && portOne().guitar)
                setReturnS32(ctx, 0); // a guitar has no actuators
            else
                ps2_stubs::scePadInfoAct(rdram, ctx, runtime);
            ctx->pc = returnTo;
        }

        // scePadGetState(port, slot).
        void padGetState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            if (getRegU32(ctx, 4) != 0u)
            {
                // Port 2: nothing there, as on a PS2 with one controller. (The
                // runtime answers "stable" for any port the game opens, and
                // GH2 then offered Multiplayer with one controller.)
                setReturnS32(ctx, 0); // scePadStateDiscon
            }
            else if (!portOne().plugged)
            {
                // Unplugged for the change of kind: the controller that comes
                // back starts digital, as a real one does, and GH1 sets it up
                // (and tells a guitar from a DualShock) all over again.
                ps2_stubs::padReplugged(0, 0);
                setReturnS32(ctx, 0); // scePadStateDiscon
            }
            else
                ps2_stubs::scePadGetState(rdram, ctx, runtime);
            ctx->pc = returnTo;
        }
    }

    void installPad(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        gh2::EntryHook<ScreenEnterTag>::install(runtime, addresses.uiScreenEnter, onScreenEnter);
        runtime.replaceFunction(addresses.scePadInfoAct, padInfoAct);
        runtime.replaceFunction(addresses.scePadGetState, padGetState);
    }
}
