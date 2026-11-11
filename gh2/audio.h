#pragma once

#include <cstddef>

namespace ghr
{
    constexpr unsigned kAudioRate = 48000u;
    using AudioSource = void (*)(float *out, size_t frames);

    // Pulls the source in real time (the synth module only advances as it is
    // rendered), to the host's audio device unless muted or none opens.
    void startAudio(AudioSource source, bool mute);
}
