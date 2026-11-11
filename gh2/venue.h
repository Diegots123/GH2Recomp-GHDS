#pragma once

#include "ps2_runtime.h"

namespace ghr
{
    // show = false: during a song nothing is drawn behind the track (venue = off).
    void installVenue(PS2Runtime &runtime, bool show);
}
