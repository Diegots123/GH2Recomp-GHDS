#pragma once

#include "ps2_runtime.h"

namespace ghr
{
    // The lens flares' Z buffer reads answered without stopping the EE
    // (see flares.cpp). GH1_FLARES=sync leaves them to wait as other reads do.
    void installFlares(PS2Runtime &runtime);
}
