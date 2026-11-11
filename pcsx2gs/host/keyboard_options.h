#pragma once

// The keyboard options screen F1 opens: a small window of its own over the
// game's, drawn straight into the window's surface (the game's window keeps
// its OpenGL context to itself). It turns the keyboard on or off, picks its mode
// and rebinds one key per action; every change is used at once and saved to
// GH1Recomp.ini.

#include "keyboard.h"

#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace pcsx2gs::keyboard
{
    class OptionsWindow
    {
    public:
        OptionsWindow(Config &config, std::string iniPath) : m_config(config), m_iniPath(std::move(iniPath)) {}
        ~OptionsWindow() { close(); }

        [[nodiscard]] bool isOpen() const { return m_window != nullptr; }
        void toggle(SDL_Window *parent);
        void close();

        // Takes the event when it is this window's; true when it did.
        bool handle(const SDL_Event &e);
        void draw();

    private:
        enum class RowKind
        {
            Heading,
            Blank,
            Enabled,
            Mode,
            Key,
            Defaults,
            Close,
        };
        struct Row
        {
            RowKind kind;
            LayerId layer = kLayerMenu;
            int index = 0;
            std::string text;
        };

        bool take(const SDL_Event &e);
        void rebuild();
        void move(int step);
        void activate();
        void assign(int scancode);
        void changed();
        int rowAt(float y) const;

        Config &m_config;
        std::string m_iniPath;
        SDL_Window *m_window = nullptr;
        bool m_dirty = true; // to be drawn again
        std::vector<Row> m_rows;
        int m_selected = 0;
        bool m_capturing = false;
        std::string m_message;
    };
}
