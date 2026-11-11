// The movies' sound (the intro's).
//
// GH1's movie player (0x24f710 and on) streams a PSS's PCM to the IOP: SIF
// DMA into a 96 KB ring it allocated there, which libsd's block transfer
// plays through core 0's sound data input (sceSdBlockTrans, in a loop, from
// the EE by sceSdRemote). It reads the transfer's position back
// (sceSdBlockTransStatus) to know where the SPU2 is, and writes behind it.
// The runtime's libsd stubs only made the position up, a kilobyte a call,
// and nothing was heard. Here the ring is played for real at 48 kHz, mixed
// into the synth module's output, and the position the game reads back is
// the one that playback has reached. The data is as the SPU2 takes it:
// blocks of 512 bytes of left samples then 512 of right (16-bit), and the
// input's volume is the one the game sets (BVOL).

#include "movie_audio.h"

#include "hook.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace ghr
{
    namespace
    {
        constexpr uint32_t kSdRemote = 0x2f6920u; // sceSdRemote(1, command, ...)
        // libsd's remote commands and parameters.
        constexpr uint32_t kSetParam = 0x8010u;
        constexpr uint32_t kBlockTrans = 0x80e0u;
        constexpr uint32_t kBlockTransStatus = 0x8100u;
        constexpr uint32_t kBvolL = 0x0f80u; // SD_P_BVOLL | core: the sound data input's volume
        constexpr uint32_t kBvolR = 0x1080u;
        constexpr uint32_t kBlockBytes = 1024u;  // 256 stereo samples
        constexpr uint32_t kBlockFrames = 256u;

        struct Ring
        {
            bool active = false;
            bool loop = false;
            uint32_t base = 0;   // IOP address
            uint32_t size = 0;   // bytes, whole blocks
            uint64_t frame = 0;  // stereo samples played since the start of the ring
        };

        std::mutex s_mutex;
        Ring s_rings[2];
        float s_volume[2][2] = {{1.0f, 1.0f}, {1.0f, 1.0f}};
        PS2Runtime *s_runtime = nullptr;

        uint32_t offsetOf(const Ring &r)
        {
            return static_cast<uint32_t>((r.frame / kBlockFrames) * kBlockBytes % r.size);
        }

        // sceSdBlockTransStatus: the half of the ring being played (bit 24)
        // and the address of the block being played.
        uint32_t status(const Ring &r)
        {
            if (r.size == 0u)
                return 0u;
            const uint32_t offset = offsetOf(r);
            const uint32_t half = (r.loop && offset >= r.size / 2u) ? 1u : 0u;
            return (half << 24) | ((r.base + offset) & 0x00FFFFFFu);
        }

        float volume(uint32_t value)
        {
            // A sweep (bit 15) the input does not get here: its level as is.
            return static_cast<float>(value & 0x7FFFu) / 32767.0f;
        }

        struct SdRemoteTag;
        bool sdRemote(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t command = GPR_U32(ctx, 5);
            uint32_t result = 0u;
            if (command == kBlockTrans)
            {
                const uint32_t core = GPR_U32(ctx, 6) & 1u;
                const uint32_t mode = GPR_U32(ctx, 7);
                std::lock_guard lock(s_mutex);
                Ring &r = s_rings[core];
                if ((mode & 3u) == 2u) // stop
                {
                    result = status(r);
                    r = Ring{};
                }
                else
                {
                    const uint32_t base = GPR_U32(ctx, 8);
                    const uint32_t size = GPR_U32(ctx, 9) & ~(kBlockBytes - 1u);
                    const uint32_t from = GPR_U32(ctx, 10);
                    if (base == 0u || size == 0u)
                        result = ~0u;
                    else
                    {
                        r.active = true;
                        r.loop = (mode & 0x10u) != 0u;
                        r.base = base;
                        r.size = size;
                        // Mode 3 starts at a given address, the others at the ring's start.
                        const uint32_t start = ((mode & 3u) == 3u && from >= base && from - base < size) ? from - base : 0u;
                        r.frame = static_cast<uint64_t>(start / kBlockBytes) * kBlockFrames;
                    }
                }
            }
            else if (command == kBlockTransStatus)
            {
                std::lock_guard lock(s_mutex);
                result = status(s_rings[GPR_U32(ctx, 6) & 1u]);
            }
            else
            {
                if (command == kSetParam)
                {
                    const uint32_t param = GPR_U32(ctx, 6);
                    const uint32_t core = param & 1u;
                    std::lock_guard lock(s_mutex);
                    if ((param & ~1u) == kBvolL)
                        s_volume[core][0] = volume(GPR_U32(ctx, 7));
                    else if ((param & ~1u) == kBvolR)
                        s_volume[core][1] = volume(GPR_U32(ctx, 7));
                }
                return false; // the runtime's own answer
            }
            SET_GPR_U32(ctx, 2, result);
            gh2::leave(ctx);
            return true;
        }
    }

    void installMovieAudio(PS2Runtime &runtime)
    {
        s_runtime = &runtime;
        gh2::Override<SdRemoteTag>::install(runtime, kSdRemote, &sdRemote);
    }

    void mixMovieAudio(float *out, size_t frames)
    {
        // GH1_MOVIE_AUDIO=off (developers): the movies silent, to compare.
        static const bool off = [] {
            const char *v = std::getenv("GH1_MOVIE_AUDIO");
            return v && std::strcmp(v, "off") == 0;
        }();
        std::lock_guard lock(s_mutex);
        if (!s_runtime || off)
            return;
        for (uint32_t core = 0; core < 2u; ++core)
        {
            Ring &r = s_rings[core];
            const float gainL = s_volume[core][0] / 32768.0f;
            const float gainR = s_volume[core][1] / 32768.0f;
            size_t done = 0;
            while (r.active && done < frames)
            {
                if (!r.loop && (r.frame / kBlockFrames) * kBlockBytes >= r.size)
                {
                    r.active = false; // a one-shot transfer has ended
                    break;
                }
                const uint32_t block = r.base + offsetOf(r);
                const uint32_t at = static_cast<uint32_t>(r.frame % kBlockFrames);
                const size_t n = std::min<size_t>(kBlockFrames - at, frames - done);
                int16_t left[kBlockFrames], right[kBlockFrames];
                if (!s_runtime->readIopMemory(block + at * 2u, left, n * 2u) ||
                    !s_runtime->readIopMemory(block + 512u + at * 2u, right, n * 2u))
                {
                    r.active = false;
                    break;
                }
                // GH1_MOVIE_DUMP=<file> (developers): the samples read, L/R int16.
                static std::FILE *dump = [] {
                    const char *v = std::getenv("GH1_MOVIE_DUMP");
                    return v ? std::fopen(v, "wb") : nullptr;
                }();
                if (dump)
                    for (size_t i = 0; i < n; ++i)
                    {
                        std::fwrite(&left[i], 2, 1, dump);
                        std::fwrite(&right[i], 2, 1, dump);
                    }
                for (size_t i = 0; i < n; ++i)
                {
                    out[(done + i) * 2u] += static_cast<float>(left[i]) * gainL;
                    out[(done + i) * 2u + 1u] += static_cast<float>(right[i]) * gainR;
                }
                done += n;
                r.frame += n;
            }
        }
    }
}
