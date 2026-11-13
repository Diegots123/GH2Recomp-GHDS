#include "keyboard.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

namespace pcsx2gs::keyboard
{
    namespace
    {
        constexpr Action kStrumActions[kStrumKeyCount] = {
            {"green", "Verde", "1"},
            {"red", "Vermelho", "2"},
            {"yellow", "Amarelo", "3"},
            {"blue", "Azul", "4"},
            {"orange", "Laranja", "5"},
            {"strum_up", "Palhetada para cima", "Up"},
            {"strum_down", "Palhetada para baixo", "Down"},
            {"start", "Start (pausa)", "Return"},
            {"star_power", "Star Power", "Backspace"},
            {"whammy", "Whammy (segure)", "Right Shift"},
        };
        constexpr Action kPadActions[kPadKeyCount] = {
            {"green", "Verde (L2)", "1"},
            {"red", "Vermelho (L1)", "2"},
            {"yellow", "Amarelo (R1)", "3"},
            {"blue", "Azul (R2)", "4"},
            {"orange", "Laranja (X)", "5"},
            {"start", "Start (pausa)", "Return"},
            {"star_power", "Star Power (Select)", "Backspace"},
            {"whammy", "Whammy (segure)", "Right Shift"},
        };
        constexpr Action kMenuActions[kMenuKeyCount] = {
            {"up", "Cima", "Up"},
            {"down", "Baixo", "Down"},
            {"left", "Esquerda", "Left"},
            {"right", "Direita", "Right"},
            {"confirm", "Confirmar", "Return"},
            {"back", "Voltar", "Backspace"},
            {"start", "Start", "Space"},
        };
        const Layer kLayers[kLayerCount] = {
            {"keys.strum", "NO JOGO (palhetando)", kStrumActions, kStrumKeyCount},
            {"keys.nostrum", "NO JOGO (sem palhetar)", kPadActions, kPadKeyCount},
            {"keys.menu", "NOS MENUS", kMenuActions, kMenuKeyCount},
        };

        std::string trim(const std::string &v)
        {
            const size_t a = v.find_first_not_of(" \t\r\n");
            const size_t b = v.find_last_not_of(" \t\r\n");
            return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
        }

        std::string lower(std::string v)
        {
            for (char &ch : v)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return v;
        }

        int scancode(const std::string &name)
        {
            if (name.empty() || lower(name) == "none")
                return 0;
            return static_cast<int>(SDL_GetScancodeFromName(name.c_str()));
        }

        std::string keyName(int code)
        {
            if (code <= 0)
                return "none";
            const char *n = SDL_GetScancodeName(static_cast<SDL_Scancode>(code));
            return n && *n ? n : "none";
        }

        // A line of the ini without its comment.
        std::string uncommented(const std::string &line)
        {
            const size_t hash = line.find('#');
            return trim(hash == std::string::npos ? line : line.substr(0, hash));
        }

        bool sectionName(const std::string &text, std::string &name)
        {
            if (text.size() < 2 || text.front() != '[' || text.back() != ']')
                return false;
            name = lower(trim(text.substr(1, text.size() - 2)));
            return true;
        }

        // Keyboard sections, the current and those of earlier versions
        // ([keys.guitar], [keys.dualshock]: dropped when the ini is saved).
        bool keySection(const std::string &name) { return name.rfind("keys.", 0) == 0; }

        // The first line of the keyboard block GH1Recomp.ini ends with.
        constexpr const char *kMarker = "# ---------------- Teclado ----------------";

    }

    const Layer &layer(LayerId id) { return kLayers[id]; }

    std::filesystem::path utf8Path(const std::string &path)
    {
        return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(path.data()), path.size()));
    }

    int *Config::keys(LayerId id)
    {
        switch (id)
        {
        case kLayerStrum:
            return strum.data();
        case kLayerPad:
            return pad.data();
        default:
            return menu.data();
        }
    }

    const int *Config::keys(LayerId id) const { return const_cast<Config *>(this)->keys(id); }

    void setDefaults(Config &c, LayerId only)
    {
        for (int l = 0; l < kLayerCount; ++l)
        {
            if (only != kLayerCount && only != l)
                continue;
            const Layer &L = kLayers[l];
            int *keys = c.keys(static_cast<LayerId>(l));
            for (size_t i = 0; i < L.count; ++i)
                keys[i] = scancode(L.actions[i].key);
        }
    }

    bool reserved(int code)
    {
        return code == SDL_SCANCODE_F1 || code == SDL_SCANCODE_F11 || code == SDL_SCANCODE_ESCAPE;
    }

    void read(const std::string &iniText, Config &c, std::string &problems)
    {
        setDefaults(c);
        std::map<std::string, std::map<std::string, std::string>> given;
        std::istringstream in(iniText);
        std::string line, section;
        while (std::getline(in, line))
        {
            const std::string text = uncommented(line);
            std::string name;
            if (sectionName(text, name))
            {
                section = name;
                continue;
            }
            const size_t eq = text.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string key = lower(trim(text.substr(0, eq)));
            const std::string value = trim(text.substr(eq + 1));
            if (keySection(section))
            {
                given[section][key] = value;
                continue;
            }
            if (!section.empty())
                continue;
            if (key == "keyboard")
            {
                // Earlier versions: "keyboard = guitar|dualshock" was the mode.
                const std::string v = lower(value);
                // (On or off is decided at start by what is connected.)
                if (v == "guitar")
                    c.noStrum = false;
                else if (v == "dualshock")
                    c.noStrum = true;
            }
            else if (key == "keyboard_mode")
            {
                const std::string v = lower(value);
                c.noStrum = v == "nostrum" || v == "dualshock";
            }
        }

        for (int l = 0; l < kLayerCount; ++l)
        {
            const Layer &L = kLayers[l];
            const auto it = given.find(L.section);
            if (it == given.end())
                continue;
            std::vector<int> chosen(L.count);
            std::string problem;
            for (size_t i = 0; i < L.count; ++i)
            {
                const auto a = it->second.find(L.actions[i].ini);
                const std::string key = a != it->second.end() ? a->second : L.actions[i].key;
                chosen[i] = scancode(key);
                if (chosen[i] == 0 && !key.empty() && lower(key) != "none")
                    problem += "\n  " + std::string(L.actions[i].ini) + " = " + key + ": tecla desconhecida";
                else if (reserved(chosen[i]))
                    problem += "\n  " + std::string(L.actions[i].ini) + " = " + key + ": tecla reservada";
            }
            for (const auto &[name, key] : it->second)
            {
                bool known = false;
                for (size_t i = 0; i < L.count; ++i)
                    known |= name == L.actions[i].ini;
                if (!known)
                    problem += "\n  " + name + ": acao desconhecida";
            }
            for (size_t i = 0; i < L.count; ++i)
                for (size_t j = i + 1; j < L.count; ++j)
                    if (chosen[i] != 0 && chosen[i] == chosen[j])
                        problem += "\n  " + std::string(L.actions[i].ini) + " e " + L.actions[j].ini +
                                   " na mesma tecla (" + keyName(chosen[i]) + ")";
            if (!problem.empty())
            {
                problems += "[" + std::string(L.section) + "]:" + problem + "\n";
                continue; // that layer keeps its defaults
            }
            std::copy(chosen.begin(), chosen.end(), c.keys(static_cast<LayerId>(l)));
        }
    }

    bool save(const std::string &path, const Config &c)
    {
        std::vector<std::string> lines;
        {
            std::ifstream f(utf8Path(path), std::ios::binary);
            std::string line;
            while (std::getline(f, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                lines.push_back(line);
            }
        }

        // Everything from the keyboard block's first line on is written anew;
        // before it, every line but keyboard ones (from an older ini) stays.
        std::vector<std::string> kept;
        std::string section;
        for (const std::string &line : lines)
        {
            if (trim(line) == kMarker)
                break;
            const std::string text = uncommented(line);
            std::string name;
            if (sectionName(text, name))
            {
                section = name;
                if (!keySection(name))
                    kept.push_back(line);
                continue;
            }
            if (keySection(section))
                continue;
            const size_t eq = text.find('=');
            const std::string key = eq == std::string::npos ? std::string() : lower(trim(text.substr(0, eq)));
            if (section.empty() && (key == "keyboard" || key == "keyboard_mode"))
                continue;
            kept.push_back(line);
        }
        while (!kept.empty() && trim(kept.back()).empty())
            kept.pop_back();

        std::ostringstream out;
        for (const std::string &line : kept)
            out << line << "\r\n";
        out << "\r\n" << kMarker << "\r\n"
            << "# (a tela do F1 no menu principal muda tudo daqui para baixo)\r\n"
               "# O teclado comeca ligado se nenhum controle ou guitarra estiver conectado\r\n"
               "# ao abrir o jogo, e desligado se houver um; o F1 liga e desliga depois.\r\n"
               "#   keyboard_mode = strum  : segure o traste e palhete (o jogo ve uma guitarra)\r\n"
               "#   keyboard_mode = nostrum: sem palhetar, como o jogo joga com o DualShock 2\r\n";
        out << "keyboard_mode = " << (c.noStrum ? "nostrum" : "strum") << "\r\n";
        out << "\r\n# Uma tecla por acao, sem tecla repetida na mesma secao. Nomes das teclas\r\n"
               "# como o SDL os escreve: 1, A, F2, Up, Down, Left, Right, Return, Space,\r\n"
               "# Backspace, Tab, Left Shift, Right Shift, Left Ctrl, ... (none: nenhuma).\r\n"
               "# F1, F11 e Escape sao reservadas. Nos menus valem as teclas de [keys.menu];\r\n"
               "# na musica, as do modo escolhido.\r\n"
               "# No modo strum, os trastes de [keys.strum] valem tambem nos menus, como os da\r\n"
               "# guitarra (para os cheats do menu principal), menos uma tecla que ja esteja\r\n"
               "# em [keys.menu].\r\n";
        for (int l = 0; l < kLayerCount; ++l)
        {
            const Layer &L = kLayers[l];
            const int *keys = c.keys(static_cast<LayerId>(l));
            out << "[" << L.section << "]\r\n";
            for (size_t i = 0; i < L.count; ++i)
                out << L.actions[i].ini << " = " << keyName(keys[i]) << "\r\n";
            if (l + 1 < kLayerCount)
                out << "\r\n";
        }

        std::ofstream f(utf8Path(path), std::ios::binary | std::ios::trunc);
        if (!f)
            return false;
        const std::string text = out.str();
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        return static_cast<bool>(f);
    }
}
