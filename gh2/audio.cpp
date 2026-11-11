// Real-time pull of the synth module, the IOP's SPU2 clock.
//
// The module ticks every 5 ms of rendered audio, and the game's song clock
// is held to the stream positions it replies with, so it must be pulled at
// real speed whether or not anything is heard. With a sound device (SDL3,
// when the host is built with it) the device pulls: its clock is the SPU2's.
// Muted, or with no device, a host thread pulls in 5 ms blocks paced by the
// steady clock and the samples are discarded, which keeps timing exact on a
// machine with no sound card.

#include "audio.h"

#if defined(GH1_PCSX2GS)
#include <SDL3/SDL.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace ghr
{
    namespace
    {
        constexpr size_t kBlockFrames = kAudioRate / 200u; // 5 ms
        std::atomic<AudioSource> s_source{nullptr};

#if defined(GH1_PCSX2GS)
        // SDL asks for bytes as the device drains, and gets exactly that many.
        void pull(void *, SDL_AudioStream *stream, int additional, int)
        {
            static std::vector<float> buffer;
            const size_t frames = static_cast<size_t>(additional) / (2u * sizeof(float));
            if (frames == 0u)
                return;
            buffer.assign(frames * 2u, 0.0f);
            if (AudioSource src = s_source.load())
                src(buffer.data(), frames);
            SDL_PutAudioStreamData(stream, buffer.data(), static_cast<int>(frames * 2u * sizeof(float)));
        }

        bool openDevice()
        {
            // 256 frames (5.3 ms) a period, about the module's own tick.
            SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "256");
            if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
                return false;
            const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, static_cast<int>(kAudioRate)};
            SDL_AudioStream *stream =
                SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, pull, nullptr);
            if (!stream)
            {
                std::fprintf(stderr, "[audio] no sound device (%s); playing silently\n", SDL_GetError());
                return false;
            }
            SDL_ResumeAudioStreamDevice(stream);
            return true;
        }
#endif

        void startTimer()
        {
            std::thread([] {
                std::vector<float> block(kBlockFrames * 2u);
                // GH1_AUDIO_DUMP=<file>: the muted output as raw f32 stereo, to check levels.
                const char *dumpPath = std::getenv("GH1_AUDIO_DUMP");
                std::FILE *dump = dumpPath ? std::fopen(dumpPath, "wb") : nullptr;
                auto next = std::chrono::steady_clock::now();
                const auto period = std::chrono::microseconds(1000000ull * kBlockFrames / kAudioRate);
                for (;;)
                {
                    std::fill(block.begin(), block.end(), 0.0f); // the source mixes into it
                    if (AudioSource src = s_source.load())
                        src(block.data(), kBlockFrames);
                    if (dump)
                    {
                        std::fwrite(block.data(), sizeof(float), block.size(), dump);
                        std::fflush(dump);
                    }
                    next += period;
                    const auto now = std::chrono::steady_clock::now();
                    if (next < now - std::chrono::milliseconds(100))
                        next = now; // a stall (debugger, suspend): resume, do not burst
                    std::this_thread::sleep_until(next);
                }
            }).detach();
        }
    }

    void startAudio(AudioSource source, bool mute)
    {
        if (s_source.exchange(source) != nullptr)
            return; // already running; the new source takes over
#if defined(GH1_PCSX2GS)
        if (!mute && openDevice())
            return;
#else
        (void)mute;
#endif
        startTimer();
    }
}
