#include "keyboard_options.h"
#include "product.h"

#include "font8x8.h"

#include <cstdio>

namespace pcsx2gs::keyboard
{
    namespace
    {
        // In units of the 8x8 font's pixels, drawn kScale times their size.
        constexpr int kScale = 2;
        constexpr float kChar = 8.0f;
        constexpr float kRowHeight = 11.0f;
        constexpr float kRowsTop = 30.0f;
        constexpr float kValueX = 216.0f;
        constexpr int kWidth = 392;
        constexpr int kHeight = 362;

        std::string keyName(int code)
        {
            if (code <= 0)
                return "(nenhuma)";
            const char *n = SDL_GetScancodeName(static_cast<SDL_Scancode>(code));
            return n && *n ? n : "(nenhuma)";
        }

        struct Canvas
        {
            SDL_Surface *surface;

            void fill(float x, float y, float w, float h, Uint8 r, Uint8 g, Uint8 b) const
            {
                const SDL_Rect rect{static_cast<int>(x * kScale), static_cast<int>(y * kScale),
                                    static_cast<int>(w * kScale), static_cast<int>(h * kScale)};
                SDL_FillSurfaceRect(surface, &rect, SDL_MapSurfaceRGB(surface, r, g, b));
            }

            void text(float x, float y, const std::string &s, Uint8 r, Uint8 g, Uint8 b) const
            {
                const Uint32 color = SDL_MapSurfaceRGB(surface, r, g, b);
                for (size_t i = 0; i < s.size(); ++i)
                {
                    const unsigned char c = static_cast<unsigned char>(s[i]);
                    if (c < 0x21 || c > 0x7e)
                        continue;
                    const uint8_t *glyph = kFont8x8[c - 0x21];
                    const int x0 = static_cast<int>((x + static_cast<float>(i) * kChar) * kScale);
                    const int y0 = static_cast<int>(y * kScale);
                    for (int gy = 0; gy < 8; ++gy)
                        for (int gx = 0; gx < 8; ++gx)
                            if (glyph[gy] & (1u << gx))
                            {
                                const SDL_Rect px{x0 + gx * kScale, y0 + gy * kScale, kScale, kScale};
                                SDL_FillSurfaceRect(surface, &px, color);
                            }
                }
            }
        };
    }

    void OptionsWindow::toggle(SDL_Window *parent)
    {
        if (isOpen())
        {
            close();
            return;
        }
        m_window = SDL_CreateWindow(GHR_TITLE " - Teclado (F1)", kWidth * kScale, kHeight * kScale, 0);
        if (!m_window)
        {
            std::fprintf(stderr, "[host] options window: %s\n", SDL_GetError());
            return;
        }
        if (parent)
        {
            SDL_SetWindowParent(m_window, parent);
            SDL_SetWindowModal(m_window, true);
        }
        SDL_RaiseWindow(m_window);
        m_dirty = true;
        m_capturing = false;
        m_message.clear();
        rebuild();
        m_selected = 0;
        move(0);
    }

    void OptionsWindow::close()
    {
        if (m_window)
            SDL_DestroyWindow(m_window);
        m_window = nullptr;
        m_capturing = false;
    }

    void OptionsWindow::rebuild()
    {
        m_rows.clear();
        m_rows.push_back({RowKind::Enabled});
        m_rows.push_back({RowKind::Mode});
        m_rows.push_back({RowKind::Blank});
        auto addLayer = [this](LayerId id)
        {
            const Layer &L = layer(id);
            m_rows.push_back({RowKind::Heading, id, 0, L.title});
            for (size_t i = 0; i < L.count; ++i)
                m_rows.push_back({RowKind::Key, id, static_cast<int>(i), L.actions[i].label});
        };
        addLayer(m_config.noStrum ? kLayerPad : kLayerStrum);
        m_rows.push_back({RowKind::Blank});
        addLayer(kLayerMenu);
        m_rows.push_back({RowKind::Blank});
        m_rows.push_back({RowKind::Defaults, kLayerMenu, 0, "Restaurar as teclas padrao"});
        m_rows.push_back({RowKind::Close, kLayerMenu, 0, "Fechar"});
        if (m_selected >= static_cast<int>(m_rows.size()))
            m_selected = 0;
    }

    void OptionsWindow::move(int step)
    {
        const int n = static_cast<int>(m_rows.size());
        int at = m_selected;
        for (int tries = 0; tries < n; ++tries)
        {
            if (tries > 0 || step != 0)
                at = (at + (step >= 0 ? 1 : -1) + n) % n;
            const RowKind k = m_rows[at].kind;
            if (k != RowKind::Heading && k != RowKind::Blank)
            {
                m_selected = at;
                return;
            }
        }
    }

    void OptionsWindow::changed()
    {
        if (!save(m_iniPath, m_config))
            m_message = "Nao consegui gravar o " GHR_INI;
    }

    void OptionsWindow::activate()
    {
        const Row &row = m_rows[m_selected];
        switch (row.kind)
        {
        case RowKind::Enabled:
            m_config.on = !m_config.on;
            m_message.clear();
            changed();
            break;
        case RowKind::Mode:
            m_config.noStrum = !m_config.noStrum;
            m_message.clear();
            rebuild();
            changed();
            break;
        case RowKind::Key:
            m_capturing = true;
            m_message = "Aperte a nova tecla (Esc cancela)";
            break;
        case RowKind::Defaults:
            setDefaults(m_config, kLayerStrum);
            setDefaults(m_config, kLayerPad);
            setDefaults(m_config, kLayerMenu);
            changed();
            break;
        case RowKind::Close:
            close();
            break;
        default:
            break;
        }
    }

    // One key per action: a key already on another action of the same
    // layer trades places with this action's key.
    void OptionsWindow::assign(int code)
    {
        if (reserved(code))
            return; // F1 and F11 are taken; keep waiting for a key
        const Row &row = m_rows[m_selected];
        m_capturing = false;
        m_message.clear();
        int *keys = m_config.keys(row.layer);
        const Layer &L = layer(row.layer);
        const int old = keys[row.index];
        for (size_t i = 0; i < L.count; ++i)
            if (static_cast<int>(i) != row.index && keys[i] == code)
                keys[i] = old;
        keys[row.index] = code;
        changed();
    }

    int OptionsWindow::rowAt(float y) const
    {
        const float logical = y / static_cast<float>(kScale) - kRowsTop;
        if (logical < 0.0f)
            return -1;
        const int i = static_cast<int>(logical / kRowHeight);
        return i < static_cast<int>(m_rows.size()) ? i : -1;
    }

    bool OptionsWindow::handle(const SDL_Event &e)
    {
        const bool used = take(e);
        m_dirty |= used;
        return used;
    }

    bool OptionsWindow::take(const SDL_Event &e)
    {
        if (!m_window)
            return false;
        const SDL_WindowID mine = SDL_GetWindowID(m_window);
        switch (e.type)
        {
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
        case SDL_EVENT_WINDOW_SHOWN:
        case SDL_EVENT_WINDOW_MOUSE_ENTER:
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        case SDL_EVENT_WINDOW_MOVED:
            return e.window.windowID == mine;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (e.window.windowID != mine)
                return false;
            close();
            return true;
        case SDL_EVENT_KEY_DOWN:
        {
            if (e.key.windowID != mine)
                return false;
            const int code = static_cast<int>(e.key.scancode);
            if (m_capturing)
            {
                if (code == SDL_SCANCODE_ESCAPE)
                {
                    m_capturing = false;
                    m_message.clear();
                }
                else if (!e.key.repeat)
                {
                    assign(code);
                }
                return true;
            }
            switch (code)
            {
            case SDL_SCANCODE_UP:
                move(-1);
                break;
            case SDL_SCANCODE_DOWN:
                move(1);
                break;
            case SDL_SCANCODE_LEFT:
            case SDL_SCANCODE_RIGHT:
                if (m_rows[m_selected].kind == RowKind::Enabled || m_rows[m_selected].kind == RowKind::Mode)
                    activate();
                break;
            case SDL_SCANCODE_RETURN:
            case SDL_SCANCODE_KP_ENTER:
            case SDL_SCANCODE_SPACE:
                if (!e.key.repeat)
                    activate();
                break;
            case SDL_SCANCODE_ESCAPE:
            case SDL_SCANCODE_F1:
                close();
                break;
            default:
                break;
            }
            return true;
        }
        case SDL_EVENT_KEY_UP:
            return e.key.windowID == mine;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        {
            if (e.button.windowID != mine)
                return false;
            const int i = rowAt(e.button.y);
            if (i >= 0 && m_rows[i].kind != RowKind::Heading && m_rows[i].kind != RowKind::Blank && !m_capturing)
            {
                m_selected = i;
                activate();
            }
            return true;
        }
        default:
            return false;
        }
    }

    void OptionsWindow::draw()
    {
        if (!m_window || !m_dirty)
            return;
        SDL_Surface *surface = SDL_GetWindowSurface(m_window);
        if (!surface)
            return;
        m_dirty = false;
        const Canvas c{surface};
        SDL_FillSurfaceRect(surface, nullptr, SDL_MapSurfaceRGB(surface, 18, 18, 26));
        c.text(12, 10, GHR_TITLE " - Teclado", 255, 210, 60);

        for (size_t i = 0; i < m_rows.size(); ++i)
        {
            const Row &row = m_rows[i];
            const float y = kRowsTop + static_cast<float>(i) * kRowHeight;
            if (static_cast<int>(i) == m_selected)
                c.fill(6.0f, y - 2.0f, static_cast<float>(kWidth) - 12.0f, kRowHeight - 1.0f, 52, 56, 96);
            std::string label, value;
            switch (row.kind)
            {
            case RowKind::Heading:
                c.text(12, y, row.text, 140, 200, 255);
                continue;
            case RowKind::Blank:
                continue;
            case RowKind::Enabled:
                label = "Teclado";
                value = m_config.on ? "LIGADO" : "DESLIGADO";
                break;
            case RowKind::Mode:
                label = "Modo";
                value = m_config.noStrum ? "SEM PALHETAR" : "PALHETANDO";
                break;
            case RowKind::Key:
                label = row.text;
                value = m_capturing && static_cast<int>(i) == m_selected
                            ? "..."
                            : keyName(m_config.keys(row.layer)[row.index]);
                break;
            default:
                label = row.text;
                break;
            }
            c.text(20, y, label, 235, 235, 235);
            if (!value.empty())
                c.text(kValueX, y, value, 255, 230, 120);
        }

        const float hintY = static_cast<float>(kHeight) - 30.0f;
        if (!m_message.empty())
            c.text(12, hintY, m_message, 255, 170, 90);
        c.text(12, hintY + 13.0f, "Setas: escolher  Enter: mudar  Esc/F1: fechar", 150, 150, 160);
        SDL_UpdateWindowSurface(m_window);
    }
}
