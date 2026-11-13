#pragma once

// Controllers as a PS2 Guitar Hero guitar. Each controller SDL opens gets a
// layout: a known guitar's (by USB vendor and product), a guitar's by SDL's
// type, or, for any other gamepad, the DualShock 2's buttons where SDL puts
// them. Adapted from GH Classic's src/host/profiles.cpp (GPL-3.0), which
// lists the guitars for GH2; the PS2 side here is the RedOctane guitar's, as
// PCSX2's PadGuitar reports it: green R2, red circle, yellow triangle, blue
// cross, orange square, strum up/down on the d-pad, star power on select,
// tilt on L2, whammy on the left stick's Y from 0x7f (rest) to 0x00.

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pcsx2gs::guitars
{
    enum Action
    {
        kGreen,
        kRed,
        kYellow,
        kBlue,
        kOrange,
        kStrumUp,
        kStrumDown,
        kStart,
        kStarPower,
        kTilt,
        kWhammy,
        kActionCount
    };

    struct Source
    {
        enum Kind
        {
            kButton,    // raw joystick button
            kAxis,      // raw joystick axis, -1..1
            kHat,       // raw joystick hat direction
            kPadButton, // SDL gamepad button
            kPadAxis,   // SDL gamepad axis, -1..1 (triggers 0..1)
            kPadAccel,  // gamepad accelerometer, m/s^2
        };
        Kind kind;
        int index;
        int hat = 0;
        float rest = 0.0f, full = 1.0f; // analog: 0 at rest, 1 at full
    };

    using Layout = std::array<std::vector<Source>, kActionCount>;

    struct Device
    {
        SDL_JoystickID id = 0;
        SDL_Joystick *joystick = nullptr;
        SDL_Gamepad *gamepad = nullptr;
        std::string label;
        bool guitar = false; // false: a gamepad with the DualShock 2's buttons
        Layout layout;
        // Kept by the host for a gamepad: whether it has shown a button
        // down yet, frames since it did, frames D-Left has been held (a PS2
        // guitar on a PS2-to-USB adapter holds it always) and then not held,
        // whether it is known not to be such a guitar, and the last frame
        // any of its buttons was down.
        bool heard = false;
        bool dpadOnStick = false; // a PS2 guitar whose adapter gives its d-pad as the left stick
        std::string rawLast;      // the host's log of what a gamepad sends, as it changes
        uint32_t rawLines = 0;
        uint32_t age = 0;
        uint32_t leftHeld = 0;
        uint32_t leftFree = 0;
        bool notGuitar = false;
        uint64_t lastPress = 0;
    };

    // Opens a controller SDL announced; false when it has no layout here.
    bool open(SDL_JoystickID id, Device &device);
    void close(Device &device);

    // What a guitar device holds now: PS2 pad bits (active high, the
    // DualShock 2 order) and the whammy, 0 at rest to 1 fully down.
    struct State
    {
        uint16_t buttons = 0;
        float whammy = 0.0f;
    };
    State read(const Device &device);
}
