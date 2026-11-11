#pragma once

// The keyboard as port 1. Off, it plays only while no controller is
// connected; on, it plays before any controller (which are then ignored).
// It starts on when nothing is connected as the game opens, off otherwise.
// It plays as a guitar (hold the fret and strum) or as a DualShock 2 (no
// strumming: GH1's own joypad play), and has one key per action in three
// layers: the game with each of the two, and the menus (which GH1 reads
// differently from a guitar and from a DualShock: the menu keys press what
// each of them selects and goes back with). A key is in a layer at most
// once, and an action has one key.

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>

namespace pcsx2gs::keyboard
{
    enum StrumKey
    {
        kStrumGreen,
        kStrumRed,
        kStrumYellow,
        kStrumBlue,
        kStrumOrange,
        kStrumUp,
        kStrumDown,
        kStrumStart,
        kStrumStarPower,
        kStrumWhammy,
        kStrumKeyCount
    };

    enum PadKey
    {
        kPadGreen, // L2
        kPadRed,   // L1
        kPadYellow, // R1
        kPadBlue,  // R2
        kPadOrange, // X
        kPadStart,
        kPadStarPower,
        kPadWhammy,
        kPadKeyCount
    };

    enum MenuKey
    {
        kMenuUp,
        kMenuDown,
        kMenuLeft,
        kMenuRight,
        kMenuConfirm,
        kMenuBack,
        kMenuStart, // some screens ask for Start (the band's name)
        kMenuKeyCount
    };

    enum LayerId
    {
        kLayerStrum,
        kLayerPad,
        kLayerMenu,
        kLayerCount
    };

    struct Action
    {
        const char *ini;   // name in GH1Recomp.ini
        const char *label; // name on the options screen
        const char *key;   // default key (SDL's name)
    };

    struct Layer
    {
        const char *section; // GH1Recomp.ini section
        const char *title;   // options screen heading
        const Action *actions;
        size_t count;
    };
    const Layer &layer(LayerId id);

    struct Config
    {
        bool on = false;      // plays before any controller (set at start by what is connected)
        bool noStrum = false; // as a DualShock 2 (true) or a guitar
        std::array<int, kStrumKeyCount> strum{}; // SDL scancodes, 0: none
        std::array<int, kPadKeyCount> pad{};
        std::array<int, kMenuKeyCount> menu{};

        int *keys(LayerId id);
        const int *keys(LayerId id) const;
    };

    // Every layer on its default keys.
    void setDefaults(Config &c, LayerId only = kLayerCount);

    // Keys the keyboard cannot take: the options screen (F1), fullscreen
    // (F11) and Escape (cancels on the options screen).
    bool reserved(int scancode);

    // GH1Recomp.ini's keyboard lines into c: "keyboard_mode =
    // strum|nostrum" and the [keys.strum], [keys.nostrum]
    // and [keys.menu] sections (the sections of earlier versions are not
    // read). A layer with an unknown key or action, a
    // reserved key or a key named twice keeps its defaults, and problems
    // says why.
    void read(const std::string &iniText, Config &c, std::string &problems);

    // Writes c into the ini at path: its other lines stay as they are.
    bool save(const std::string &path, const Config &c);

    // A UTF-8 path (SDL's) as a path the standard streams open right on
    // Windows too, whatever letters the folder's name has.
    std::filesystem::path utf8Path(const std::string &path);
}
