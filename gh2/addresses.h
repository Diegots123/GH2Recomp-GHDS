#pragma once

// Addresses in GH2 retail (SLUS-21447, boot ELF SHA-1 aea7bbb0...) that the
// game layer hooks or reads: GH2's own names (config/gh2-retail.symbols) and
// GH Classic's retail table (src/addresses.h, kSlus21447).

#include <cstdint>

namespace ghr
{
    struct Addresses
    {
        // CtlClientPoll__Fv: where the synth module's replies are delivered.
        uint32_t ctlClientPoll;

        // CtlServerInit (0x22dda8) stores the reply handler here and
        // registers its RPC server with this receive buffer (GH Classic's table).
        uint32_t synthServerHandler;
        uint32_t synthServerBuffer;

        // scePadInfoAct and scePadGetState (libpad).
        uint32_t scePadInfoAct;
        uint32_t scePadGetState;

        // UIScreen::Enter(UIScreen *from) (0x212f40): `this` is the screen
        // the game goes to, its name a const char * at +0x14 ("game_screen"
        // while a song plays, "main_screen" for the main menu; seen with
        // GH1_TRACE=ui).
        uint32_t uiScreenEnter;
        uint32_t screenNameOffset;
    };

    inline constexpr Addresses kRetail{
        .ctlClientPoll = 0x22dc68u,
        .synthServerHandler = 0x3de454u,
        .synthServerBuffer = 0x484340u,
        .scePadInfoAct = 0x2f2e58u,
        .scePadGetState = 0x2f2cc0u,
        .uiScreenEnter = 0x212f40u,
        .screenNameOffset = 0x14u,
    };
}
