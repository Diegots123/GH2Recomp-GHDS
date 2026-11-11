#pragma once

#include "ps2_runtime.h"

#include <cstddef>

namespace ghr
{
    // The movies' sound (the intro's), played from the IOP as the SPU2
    // would (see movie_audio.cpp).
    void installMovieAudio(PS2Runtime &runtime);
    // Mixes the movie sound due now into 48 kHz stereo (the audio thread).
    void mixMovieAudio(float *out, size_t frames);
}
