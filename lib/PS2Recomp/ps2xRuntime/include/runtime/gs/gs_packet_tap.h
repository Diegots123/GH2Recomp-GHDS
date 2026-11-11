#pragma once

// A tap on the GS's input, for an external GS (a renderer outside this
// runtime). It sees every GIF packet the GS receives, as the raw qwords the
// GIF delivered and before the runtime's GS frontend interprets them, with the
// path it came from (1-3). While a tap is set, the runtime's shortcuts that
// skip raw packets (native image upload and packed-chain fast paths) are off.
//
// Process-wide (the runtime has one GS) and declared outside GS, so GS's
// layout, which generated code was compiled against, does not change.

#include <cstdint>
#include <functional>

namespace ps2x::gs
{
    using PacketTap = std::function<void(uint32_t path, const uint8_t *data, uint32_t sizeBytes)>;
    void setPacketTap(PacketTap tap);
    bool hasPacketTap();

    // The GIF arbiter names the path of the packet it hands to the GS.
    void setCurrentPath(uint32_t path);

    // Called where the GIF takes data in (PS2Memory::submitGifPacket, the
    // release of masked PATH3 packets): the raw qwords, in the order the
    // guest sent them. A packet the runtime built itself (a GIFtag it added to
    // continue an IMAGE across VIF DIRECTs) is submitted with the tap
    // suppressed, after its raw data has been tapped.
    void tap(uint32_t path, const uint8_t *data, uint32_t sizeBytes);
    void suppressTap(bool suppressed);
}
