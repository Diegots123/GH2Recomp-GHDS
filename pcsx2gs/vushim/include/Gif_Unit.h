// Stands in for PCSX2's Gif_Unit.h when microVU is built for GH1 Recomp.
//
// microVU's XGKICK (mVU_XGKICK_) hands each PATH1 packet to PCSX2's GIF unit,
// which arbitrates the three paths and queues the packets for its MTGS
// thread. GH1 Recomp's runtime has its own GIF arbiter, so here the GIF unit
// is only the part microVU calls: the packet size walk, and a buffer that
// collects a packet (in two pieces when it wraps VU1 memory) and passes it
// to the runtime whole. Everything else Gif_Unit.h declares is left out.
//
// Licence: GPL-3.0-or-later, as the code it is built with.

#pragma once

#include "Gif.h"
#include "Vif.h"
#include "GS.h"
#include "GS/GSRegs.h"
#include "MTGS.h"
#include "common/AlignedMalloc.h" // what PCSX2's Gif_Unit.h brings in for microVU.h

struct Gif_Path_Vu1
{
	// Adds the first piece of a packet that wraps around VU1 memory.
	void CopyGSPacketData(u8* pMem, u32 size, bool aligned = false);
};

struct Gif_Unit_Vu1
{
	Gif_Path_Vu1 gifPath[3];

	// PCSX2's GetGSPacketSize as microVU1 (no XGKick gamefix) sees it: the
	// bytes from the tag at `offset` through the tag with EOP, wrapping at
	// VU1 memory's 16 KiB; 0 when that runs past VU1 memory's size.
	u32 GetGSPacketSize(GIF_PATH pathIdx, u8* pMem, u32 offset = 0, u32 size = ~0u, bool flush = false);

	// Adds the (last) piece of a PATH1 packet and sends the packet.
	u32 TransferGSPacketData(GIF_TRANSFER_TYPE tranType, u8* pMem, u32 size, bool aligned = false);
};

extern Gif_Unit_Vu1 gifUnit;
