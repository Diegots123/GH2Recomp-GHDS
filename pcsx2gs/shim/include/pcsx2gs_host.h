#pragma once

// What the embedding program (the PS2Recomp runtime) tells the standalone GS:
// the game it runs and the window to draw into.

#include "common/WindowInfo.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace pcsx2gs
{
    struct Host
    {
        std::string serial;
        std::string title;
        uint32_t crc = 0;
        std::function<std::optional<WindowInfo>(bool recreate)> acquireWindow;
        std::function<void()> releaseWindow;
    };

    inline Host &host()
    {
        static Host instance;
        return instance;
    }
}

// 64 KiB of read-write memory in the same reservation as the recompilers'
// code, so code can reach it with 32-bit displacements (microVU addresses
// VU memory that way, as PCSX2 places it). Null if none could be reserved.
uint8_t *pcsx2NearCodeData();
