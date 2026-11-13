#pragma once

// The PS2Recomp runtime's host side on SDL3 with PCSX2's GS: an SDL window
// the GS draws into (OpenGL), keyboard and gamepad input, and the GS fed on
// the host thread from a queue the EE side fills (like PCSX2's MTGS).

#include "keyboard.h"
#include "pcsx2gs_bridge.h"
#include "ps2_runtime.h"

#include <array>
#include <memory>
#include <string>

namespace pcsx2gs
{
    enum class GamepadAs
    {
        Auto,
        Guitar,
        DualShock,
    };

    struct HostConfig
    {
        float upscale = 1.0f;     // internal resolution, multiples of native
        Api api = Api::OpenGL;    // what the hardware renderer draws with
        bool software = false;    // PCSX2's software renderer
        bool null = false;        // no drawing (GH1_RENDERER=null, for measuring)
        bool vsync = false;       // present on the host's vertical blank
        uint32_t windowWidth = 1280;
        uint32_t windowHeight = 960;
        bool fullscreen = false;  // borderless on the desktop; F11 or Alt+Enter toggles
        bool venue = true;        // false: nothing drawn behind the track (venue = off, for weak GPUs)
        int audioOffsetMs = 0;    // audio_offset: the songs' audio against the notes, in ms (+ later, - earlier)
        int gsHalfPixelOffset = 0; // the game's GS fixes from PCSX2's game database (set by the game layer)
        int gsNativeScaling = 0;
        int vu1Thread = -1;       // VIF1/VU1 on a thread of its own: 1 on, 0 off, -1 when 2+ CPU threads are free
        bool raisePriority = true; // Windows: the process above normal priority (priority = above_normal|normal)
        bool stats = false;       // print presented frames per second
        bool guitar = true;       // false: port 1 is always a DualShock 2 (GH1_JOYPAD=1)
        // gamepad = auto|guitar|dualshock: what a gamepad that is no known
        // guitar plays as (a PS2-to-USB adapter): auto, a PS2 guitar when it
        // holds D-Left (as they all do), else a DualShock 2.
        GamepadAs gamepadAs = GamepadAs::Auto;
        keyboard::Config keyboard; // on/off, mode and keys (F1 changes them)
        bool keyboardForced = false; // on/off set by GH1_KEYBOARD, not by what is connected at start
        std::string iniPath;       // GH1Recomp.ini beside the exe
    };

    // Port 1 as the host has it now: plugged (false for a moment when it
    // changes kind) and a guitar or a DualShock 2.
    struct PortOne
    {
        bool plugged;
        bool guitar;
    };
    PortOne portOne();

    // A local-to-host read (the transfer's registers already sent to the
    // GS) answered at once from local memory as the GS has it, a frame or
    // so old; the read itself is queued so that the GS brings local memory
    // up to date for the next one. False when no SDL host runs (nothing was
    // read): the caller reads the usual way.
    bool readLocalLate(uint8_t *dst, uint32_t bytes, uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg);

    // The screen GH1 shows, as GH1's side reports it. In a song (not paused)
    // the keyboard plays with its game keys, elsewhere with its menu keys.
    // Port 1 changes kind (and F1 opens) only on the main menu.
    enum class Screen
    {
        Other,
        MainMenu,
        Song,
    };
    void setScreen(Screen screen);

    // Reads HostConfig from GH1Recomp.ini beside the exe (key = value lines:
    // upscale, renderer = auto|opengl|d3d11|d3d12|sw, vsync, window = <w>x<h>, fullscreen, venue = on|off,
    // audio_offset = <ms>,
    // vu1_thread = auto|on|off, priority = above_normal|normal, gamepad = auto|guitar|dualshock, and
    // the keyboard's, see keyboard.h), then
    // from the environment, which wins: GH1_UPSCALE, GH1_RENDERER (the same names, or null),
    // GH1_VSYNC=1, GH1_FULLSCREEN=1, GH1_VENUE=off, GH1_AUDIO_OFFSET=<ms>, GH1_VU1_THREAD=0|1, GH1_PRIORITY=normal, GH1_STATS=1, GH1_GAMEPAD=auto|guitar|dualshock, GH1_JOYPAD=1 (port 1 a
    // DualShock, not a guitar), GH1_WINDOW=<w>x<h>.
    HostConfig hostConfigFromEnvironment();

    std::unique_ptr<PS2Runtime::HostFrontend> makeSdlHost(const HostConfig &config);
}
