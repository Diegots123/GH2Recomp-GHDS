#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_packet_tap.h"
#include <algorithm>
#include <cstring>

GifArbiter::GifArbiter(ProcessPacketFn processFn)
    : m_processFn(std::move(processFn))
{
}

bool GifArbiter::isImagePacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || sizeBytes < 16u)
        return false;

    uint64_t tagLo = 0;
    std::memcpy(&tagLo, data, sizeof(tagLo));
    const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
    return flg == 2u;
}

namespace
{
    // Packets' buffers kept from one drain to the next: a frame of a game
    // queues thousands of small packets (each VU1 XGKICK), and a buffer
    // apiece from the heap every time is time the EE spends for nothing.
    // Buffers of up to 64 KB, a few thousand of them; drain and submit run
    // on the EE's thread.
    constexpr size_t kPooledBufferMax = 64u * 1024u;
    constexpr size_t kPoolMax = 8192u;
    thread_local std::vector<std::vector<uint8_t>> t_buffers;
}

void GifArbiter::submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl)
{
    if (!data || sizeBytes < 16 || !m_processFn)
        return;

    GifArbiterPacket pkt;
    pkt.pathId = pathId;
    pkt.path2DirectHl = (pathId == GifPathId::Path2) && path2DirectHl;
    pkt.path3Image = (pathId == GifPathId::Path3) && isImagePacket(data, sizeBytes);
    if (!t_buffers.empty())
    {
        pkt.data = std::move(t_buffers.back());
        t_buffers.pop_back();
    }
    pkt.data.assign(data, data + sizeBytes);
    m_queue.push_back(std::move(pkt));
}

void GifArbiter::drain()
{
    if (!m_processFn)
        return;

    std::stable_sort(m_queue.begin(), m_queue.end(),
                     [](const GifArbiterPacket &a, const GifArbiterPacket &b)
                     {
                         // DIRECTHL cannot preempt PATH3 IMAGE transfers.
                         if (a.path2DirectHl != b.path2DirectHl || a.path3Image != b.path3Image)
                         {
                             if (a.path3Image && b.path2DirectHl)
                                 return true;
                             if (a.path2DirectHl && b.path3Image)
                                 return false;
                         }
                         return pathPriority(a.pathId) < pathPriority(b.pathId);
                     });

    for (size_t i = 0; i < m_queue.size(); ++i)
    {
        auto &pkt = m_queue[i];
        if (!pkt.data.empty())
        {
            ps2x::gs::setCurrentPath(static_cast<uint32_t>(pkt.pathId));
            m_processFn(pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
        }
    }
    for (auto &pkt : m_queue)
        if (pkt.data.capacity() <= kPooledBufferMax && t_buffers.size() < kPoolMax)
            t_buffers.push_back(std::move(pkt.data));
    m_queue.clear();
}

uint8_t GifArbiter::pathPriority(GifPathId id)
{
    return static_cast<uint8_t>(id);
}
