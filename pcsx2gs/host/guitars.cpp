#include "guitars.h"

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <string>

namespace pcsx2gs::guitars
{
    namespace
    {
        // The PS2 guitar's bits for each action (DualShock 2 order).
        constexpr uint16_t kBits[kActionCount] = {
            0x0200, // green: R2
            0x2000, // red: circle
            0x1000, // yellow: triangle
            0x4000, // blue: cross
            0x8000, // orange: square
            0x0010, // strum up: d-pad up
            0x0040, // strum down: d-pad down
            0x0008, // start
            0x0001, // star power: select
            0x0100, // tilt: L2
            0,      // whammy: an axis
        };

        Source button(int i) { return {Source::kButton, i}; }
        Source axis(int i, float rest, float full) { return {Source::kAxis, i, 0, rest, full}; }
        Source hat(int i, int dir) { return {Source::kHat, i, dir}; }
        Source padButton(SDL_GamepadButton b) { return {Source::kPadButton, b}; }
        Source padAxis(SDL_GamepadAxis a, float rest, float full) { return {Source::kPadAxis, a, 0, rest, full}; }
        Source padAccel(int i, float rest, float full) { return {Source::kPadAccel, i, 0, rest, full}; }

        Layout make(std::initializer_list<std::pair<Action, std::vector<Source>>> bindings)
        {
            Layout l;
            for (const auto &[action, sources] : bindings)
                l[action] = sources;
            return l;
        }

        // A Wii guitar through a raphnet WUSBMote v2.2 (no tilt: that is the Wiimote's).
        Layout raphnetWiiGuitar()
        {
            return make({{kGreen, {button(0)}}, {kRed, {button(1)}}, {kYellow, {button(2)}},
                         {kBlue, {button(3)}}, {kOrange, {button(4)}}, {kStrumUp, {button(8)}},
                         {kStrumDown, {button(5)}}, {kStart, {button(6)}}, {kStarPower, {button(7)}},
                         {kWhammy, {axis(2, -0.11f, 0.70f)}}});
        }

        // Xbox 360 guitars through XInput: frets A B Y X LB, star power on
        // Back, whammy on RIGHTX from -1, tilt on RIGHTY either way past half
        // (SDL's sign for it varies) or the star power pedal (RB).
        Layout xinputGuitar()
        {
            return make({{kGreen, {padButton(SDL_GAMEPAD_BUTTON_SOUTH)}},
                         {kRed, {padButton(SDL_GAMEPAD_BUTTON_EAST)}},
                         {kYellow, {padButton(SDL_GAMEPAD_BUTTON_NORTH)}},
                         {kBlue, {padButton(SDL_GAMEPAD_BUTTON_WEST)}},
                         {kOrange, {padButton(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)}},
                         {kStrumUp, {padButton(SDL_GAMEPAD_BUTTON_DPAD_UP)}},
                         {kStrumDown, {padButton(SDL_GAMEPAD_BUTTON_DPAD_DOWN)}},
                         {kStart, {padButton(SDL_GAMEPAD_BUTTON_START)}},
                         {kStarPower, {padButton(SDL_GAMEPAD_BUTTON_BACK)}},
                         {kTilt,
                          {padButton(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), padAxis(SDL_GAMEPAD_AXIS_RIGHTY, 0.0f, -1.0f),
                           padAxis(SDL_GAMEPAD_AXIS_RIGHTY, 0.0f, 1.0f)}},
                         {kWhammy, {padAxis(SDL_GAMEPAD_AXIS_RIGHTX, -1.0f, 1.0f)}}});
        }

        // Guitars SDL lines up with the PS3's: PS3/Wii Rock Band, PS3 Guitar
        // Hero, Rock Band 4 on PS4/PS5/Xbox One. Tilt on RIGHT_SHOULDER.
        Layout ps3WayGuitar(float whammyRest, bool yellowBlueSwapped)
        {
            return make({{kGreen, {padButton(SDL_GAMEPAD_BUTTON_SOUTH)}},
                         {kRed, {padButton(SDL_GAMEPAD_BUTTON_EAST)}},
                         {kYellow, {padButton(yellowBlueSwapped ? SDL_GAMEPAD_BUTTON_WEST : SDL_GAMEPAD_BUTTON_NORTH)}},
                         {kBlue, {padButton(yellowBlueSwapped ? SDL_GAMEPAD_BUTTON_NORTH : SDL_GAMEPAD_BUTTON_WEST)}},
                         {kOrange, {padButton(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)}},
                         {kStrumUp, {padButton(SDL_GAMEPAD_BUTTON_DPAD_UP)}},
                         {kStrumDown, {padButton(SDL_GAMEPAD_BUTTON_DPAD_DOWN)}},
                         {kStart, {padButton(SDL_GAMEPAD_BUTTON_START)}},
                         {kStarPower, {padButton(SDL_GAMEPAD_BUTTON_BACK)}},
                         {kTilt, {padButton(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)}},
                         {kWhammy, {padAxis(SDL_GAMEPAD_AXIS_RIGHTX, whammyRest, 1.0f)}}});
        }

        // The PC/Mac Guitar Hero World Tour guitar, a plain HID joystick to SDL.
        Layout pcWorldTourGuitar()
        {
            return make({{kGreen, {button(1)}}, {kRed, {button(2)}}, {kYellow, {button(0)}},
                         {kBlue, {button(3)}}, {kOrange, {button(4)}}, {kStrumUp, {hat(0, SDL_HAT_UP)}},
                         {kStrumDown, {hat(0, SDL_HAT_DOWN)}}, {kStart, {button(9)}},
                         {kStarPower, {button(8), button(5)}}, {kWhammy, {axis(2, 0.0f, 1.0f)}}});
        }

        // Its tilt is the accelerometer's X only: past about 35 degrees either way.
        Layout ps3GuitarHero()
        {
            Layout l = ps3WayGuitar(0.0f, true);
            l[kTilt].push_back(padAccel(0, 0.0f, -12.0f));
            l[kTilt].push_back(padAccel(0, 0.0f, 12.0f));
            return l;
        }

        Layout rockBandDrifting() { return ps3WayGuitar(0.0f, false); }
        Layout rockBand4() { return ps3WayGuitar(-1.0f, false); }

        // Star power also on the d-pad's left.
        Layout riffmaster()
        {
            Layout l = rockBand4();
            l[kStarPower].push_back(padButton(SDL_GAMEPAD_BUTTON_DPAD_LEFT));
            return l;
        }

        struct Known
        {
            uint16_t vendor, product;
            const char *label;
            Layout (*layout)();
            bool sensor;
        };

        // As GH Classic lists them (PlasticBand's documentation and SDL's
        // drivers); tested there: the raphnet Wii guitar.
        const Known kKnown[] = {
            {0x12ba, 0x0100, "PS3 Guitar Hero guitar", ps3GuitarHero, true},
            {0x12ba, 0x0200, "PS3 Rock Band guitar", rockBandDrifting, false},
            {0x1bad, 0x0004, "Wii Rock Band guitar", rockBandDrifting, false},
            {0x1bad, 0x3010, "Wii Rock Band 2 guitar", rockBandDrifting, false},
            {0x1430, 0x474c, "PC/Mac Guitar Hero World Tour guitar", pcWorldTourGuitar, false},
            {0x289b, 0x0028, "Wii guitar (raphnet)", raphnetWiiGuitar, false},
            {0x289b, 0x002b, "Wii guitar (raphnet)", raphnetWiiGuitar, false},
            {0x289b, 0x0080, "Wii guitar (raphnet)", raphnetWiiGuitar, false},
            {0x1430, 0x4748, "Xbox 360 Guitar Hero guitar", xinputGuitar, false},
            {0x1430, 0x0705, "Xbox 360 Guitar Hero guitar", xinputGuitar, false},
            {0x1430, 0x4734, "Xbox 360 Guitar Hero guitar", xinputGuitar, false},
            {0x1430, 0x0706, "Xbox 360 Guitar Hero guitar", xinputGuitar, false},
            {0x1bad, 0x0002, "Xbox 360 Rock Band guitar", xinputGuitar, false},
            {0x0738, 0x9806, "Xbox 360 Rock Band guitar", xinputGuitar, false},
            {0x0e6f, 0x0248, "Riffmaster", riffmaster, false},
            {0x0e6f, 0x024a, "Riffmaster", riffmaster, false},
            {0x0e6f, 0x0249, "Riffmaster", riffmaster, false},
            {0x0738, 0x4161, "Rock Band 4 guitar", rockBand4, false},
            {0x0e6f, 0x0170, "Rock Band 4 guitar", rockBand4, false},
            {0x3651, 0x4161, "Rock Band 4 guitar", rockBand4, false},
            {0x3958, 0x4161, "Rock Band 4 guitar", rockBand4, false},
            {0x0738, 0x8261, "Rock Band 4 guitar", rockBand4, false},
            {0x0e6f, 0x0173, "Rock Band 4 guitar", rockBand4, false},
            {0x3651, 0x1500, "Rock Band 4 guitar", rockBand4, false},
            {0x3651, 0x5500, "Rock Band 4 guitar", rockBand4, false},
            {0x3958, 0x5500, "Rock Band 4 guitar", rockBand4, false},
            {0x3651, 0x1600, "Rock Band 4 guitar", rockBand4, false},
            {0x3651, 0x5600, "Rock Band 4 guitar", rockBand4, false},
            {0x3958, 0x5600, "Rock Band 4 guitar", rockBand4, false},
        };

        // Six-fret Guitar Hero Live guitars have no five-fret layout.
        bool isGuitarHeroLive(uint16_t vendor, uint16_t product)
        {
            return (vendor == 0x12ba && product == 0x074b) || (vendor == 0x1430 && product == 0x070b) ||
                   (vendor == 0x1430 && product == 0x07bb) || (vendor == 0x1430 && product == 0x079b);
        }

        float value(const Device &d, const Source &s)
        {
            auto span = [&s](float v) {
                const float range = s.full - s.rest;
                return range == 0.0f ? 0.0f : std::clamp((v - s.rest) / range, 0.0f, 1.0f);
            };
            switch (s.kind)
            {
            case Source::kButton:
                return d.joystick && SDL_GetJoystickButton(d.joystick, s.index) ? 1.0f : 0.0f;
            case Source::kAxis:
                return d.joystick ? span(SDL_GetJoystickAxis(d.joystick, s.index) / 32767.0f) : 0.0f;
            case Source::kHat:
                return d.joystick && (SDL_GetJoystickHat(d.joystick, s.index) & s.hat) == s.hat ? 1.0f : 0.0f;
            case Source::kPadButton:
                return d.gamepad && SDL_GetGamepadButton(d.gamepad, static_cast<SDL_GamepadButton>(s.index)) ? 1.0f
                                                                                                           : 0.0f;
            case Source::kPadAxis:
                return d.gamepad ? span(SDL_GetGamepadAxis(d.gamepad, static_cast<SDL_GamepadAxis>(s.index)) / 32767.0f)
                                 : 0.0f;
            case Source::kPadAccel:
            {
                float accel[3] = {};
                if (!d.gamepad || !SDL_GetGamepadSensorData(d.gamepad, SDL_SENSOR_ACCEL, accel, 3))
                    return 0.0f;
                return span(accel[s.index]);
            }
            }
            return 0.0f;
        }
    }

    bool open(SDL_JoystickID id, Device &d)
    {
        d = Device{};
        d.id = id;
        if (SDL_IsGamepad(id))
        {
            d.gamepad = SDL_OpenGamepad(id);
            d.joystick = d.gamepad ? SDL_GetGamepadJoystick(d.gamepad) : nullptr;
        }
        else
        {
            d.joystick = SDL_OpenJoystick(id);
        }
        if (!d.joystick)
            return false;

        const uint16_t vendor = SDL_GetJoystickVendor(d.joystick);
        const uint16_t product = SDL_GetJoystickProduct(d.joystick);
        for (const Known &k : kKnown)
        {
            if (k.vendor == vendor && k.product == product)
            {
                d.label = k.label;
                d.layout = k.layout();
                d.guitar = true;
                if (k.sensor && d.gamepad)
                    SDL_SetGamepadSensorEnabled(d.gamepad, SDL_SENSOR_ACCEL, true);
            }
        }
        if (!d.guitar && !isGuitarHeroLive(vendor, product) && SDL_GetJoystickType(d.joystick) == SDL_JOYSTICK_TYPE_GUITAR)
        {
            // A guitar SDL knows but this list does not (Santroller builds among them).
            const bool xinput = d.gamepad && SDL_GetGamepadType(d.gamepad) == SDL_GAMEPAD_TYPE_XBOX360;
            d.label = xinput ? "Xbox 360 guitar" : "guitar";
            d.layout = xinput ? xinputGuitar() : ps3WayGuitar(-1.0f, false);
            d.guitar = true;
        }
        if (!d.guitar && !d.gamepad && SDL_GetNumJoystickButtons(d.joystick) >= 10)
        {
            // A controller no database knows, with a DualShock's worth of
            // buttons: most PS2-to-USB adapters number them alike (triangle,
            // circle, cross, square, L2, R2, L1, R1, select, start, L3, R3;
            // the d-pad on the hat; left stick on axes 0 and 1, the right on
            // 3 and 2). It is opened as a gamepad with that layout.
            char guid[64];
            SDL_GUIDToString(SDL_GetJoystickGUID(d.joystick), guid, sizeof(guid));
            const std::string mapping =
                std::string(guid) +
                ",PS2 adapter,a:b2,b:b1,x:b3,y:b0,lefttrigger:b4,righttrigger:b5,leftshoulder:b6,"
                "rightshoulder:b7,back:b8,start:b9,leftstick:b10,rightstick:b11,dpup:h0.1,dpright:h0.2,"
                "dpdown:h0.4,dpleft:h0.8,leftx:a0,lefty:a1,rightx:a3,righty:a2,";
            SDL_CloseJoystick(d.joystick);
            d.joystick = nullptr;
            if (SDL_AddGamepadMapping(mapping.c_str()) >= 0 && SDL_IsGamepad(id))
            {
                d.gamepad = SDL_OpenGamepad(id);
                d.joystick = d.gamepad ? SDL_GetGamepadJoystick(d.gamepad) : nullptr;
            }
            if (!d.joystick)
                return false;
            d.label = "gamepad (PS2 adapter layout)";
        }
        if (!d.guitar && !d.gamepad)
        {
            close(d);
            return false;
        }
        if (!d.guitar && d.label.empty())
            d.label = "gamepad";
        const char *name = SDL_GetJoystickName(d.joystick);
        std::fprintf(stderr, "[host] controller: %s (%04x:%04x) as %s\n", name ? name : "?", vendor, product,
                     d.label.c_str());
        return true;
    }

    void close(Device &d)
    {
        if (d.gamepad)
            SDL_CloseGamepad(d.gamepad);
        else if (d.joystick)
            SDL_CloseJoystick(d.joystick);
        d = Device{};
    }

    State read(const Device &d)
    {
        State st;
        for (int a = 0; a < kActionCount; ++a)
        {
            float v = 0.0f;
            for (const Source &s : d.layout[a])
                v = std::max(v, value(d, s));
            if (a == kWhammy)
                st.whammy = v;
            else if (v > 0.5f)
                st.buttons |= kBits[a];
        }
        return st;
    }
}
