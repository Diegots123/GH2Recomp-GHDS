// What PCSX2's microVU expects from the rest of the emulator, for running its
// VU1 recompiler on its own as GH1 Recomp's VU1. PCSX2's sources are used
// unmodified; this file stands in for the parts of PCSX2 that are not built:
//
//  - VU and EE state microVU reads and writes (vuRegs, VPU_STAT/FBRST, VIF1's
//    TOP/ITOP in eeHw), owned here and kept in step with the runtime by the
//    bridge (vubridge/pcsx2vu_bridge.cpp);
//  - the GIF unit's PATH1 (see include/Gif_Unit.h): XGKICK packets go to the
//    runtime's GIF arbiter through the bridge;
//  - the MTVU thread, which stays off (PCSX2's default);
//  - the EE recompiler, which microVU calls into only to compile COP2 macro
//    instructions for VU0. GH1 Recomp's EE is recompiled ahead of time and
//    never asks for that, so those entry points abort if ever reached.
//
// Licence: GPL-3.0-or-later, as the code it links with.

#include "Common.h"
#include "VU.h"
#include "VUmicro.h"
#include "Gif_Unit.h"
#include "common/emitter/x86emitter.h"
#include "x86/iCore.h"
#include "x86/iR5900.h"
#include "R5900OpcodeTables.h"
#include "vtlb.h"
#include "MTVU.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "pcsx2vu_shim.h"

// --- state microVU works on -----------------------------------------------------

alignas(16) VURegs vuRegs[2];
BaseVUmicroCPU *CpuVU0 = nullptr; // VU0 stays the runtime's
BaseVUmicroCPU *CpuVU1 = &CpuMicroVU1;
alignas(__pagealignsize) u8 eeHw[Ps2MemSize::Hardware];
alignas(16) cpuRegistersPack _cpuRegistersPack;

// --- the GIF unit's PATH1 -------------------------------------------------------

Gif_Unit_Vu1 gifUnit;

namespace
{
    std::vector<u8> s_packet;
    pcsx2vu_shim::KickFn s_kick = nullptr;
    void *s_kickUser = nullptr;
    u32 s_interrupts = 0;
}

namespace pcsx2vu_shim
{
    void setKick(KickFn kick, void *user)
    {
        s_kick = kick;
        s_kickUser = user;
    }

    u32 takeInterrupts()
    {
        const u32 raised = s_interrupts;
        s_interrupts = 0;
        return raised;
    }
}

void Gif_Path_Vu1::CopyGSPacketData(u8 *pMem, u32 size, bool)
{
    s_packet.insert(s_packet.end(), pMem, pMem + size);
}

u32 Gif_Unit_Vu1::GetGSPacketSize(GIF_PATH pathIdx, u8 *pMem, u32 offset, u32 size, bool)
{
    // PCSX2's loop with microVU1's settings (REC_VU1, no XGKick gamefix):
    // it returns at the tag with EOP.
    const u32 memMask = pathIdx ? ~0u : 0x3fffu;
    u32 curSize = 0;
    for (;;)
    {
        u64 lo;
        std::memcpy(&lo, &pMem[offset & memMask], sizeof(lo));
        const u32 nloop = static_cast<u32>(lo & 0x7fffu);
        const bool eop = ((lo >> 15) & 1u) != 0;
        const u32 flg = static_cast<u32>((lo >> 58) & 3u);
        const u32 nreg = ((static_cast<u32>(lo >> 60) - 1u) & 0xfu) + 1u;
        u32 len;
        if (flg == 0) // PACKED
            len = nreg * nloop * 16u;
        else if (flg == 1) // REGLIST
            len = ((nreg * nloop + 1u) >> 1) * 16u;
        else // IMAGE
            len = nloop * 16u;
        offset += 16u + len;
        curSize += 16u + len;
        if (pathIdx == GIF_PATH_1 && curSize >= 0x4000)
        {
            std::fprintf(stderr, "[pcsx2 vu] GS packet size exceeded VU memory size\n");
            return 0;
        }
        if (curSize >= size)
            return size;
        if (eop)
            return curSize;
    }
}

u32 Gif_Unit_Vu1::TransferGSPacketData(GIF_TRANSFER_TYPE, u8 *pMem, u32 size, bool)
{
    s_packet.insert(s_packet.end(), pMem, pMem + size);
    if (s_kick && !s_packet.empty())
        s_kick(s_packet.data(), static_cast<u32>(s_packet.size()), s_kickUser);
    s_packet.clear();
    return size;
}

// --- interrupts -------------------------------------------------------------------

// microVU raises INTC 6/7 when a program stops on a D or T bit; the bridge
// hands them back to the runtime.
void hwIntcIrq(int n)
{
    s_interrupts |= 1u << n;
}

void CPU_INT(EE_EventType, s32) {}

// --- the MTVU thread (off) ----------------------------------------------------------

VU_Thread vu1Thread;
VU_Thread::VU_Thread() = default;
VU_Thread::~VU_Thread() = default;
void VU_Thread::Open() {}
void VU_Thread::WaitVU() {}
void VU_Thread::Get_MTVUChanges() {}

// --- PCSX2's VU1 control, as its VU1micro.cpp / VU0micro.cpp has it ------------------

void vu1ResetRegs()
{
    VU0.VI[REG_VPU_STAT].UL &= ~0xff00;
    VU0.VI[REG_FBRST].UL &= ~0xff00;
    vif1Regs.stat.VEW = false;
}

void vu0ResetRegs()
{
    VU0.VI[REG_VPU_STAT].UL &= ~0xff;
    VU0.VI[REG_FBRST].UL &= ~0xff;
    vif0Regs.stat.VEW = false;
}

// --- the EE recompiler's side of COP2 macro mode: never reached ----------------------

[[noreturn]] static void unreachable(const char *what)
{
    std::fprintf(stderr, "[pcsx2 vu] %s is part of PCSX2's EE recompiler, which GH1 Recomp does not use\n", what);
    std::abort();
}

#define UNREACHABLE(sig, name) \
    sig { unreachable(name); }

u32 pc;
bool s_nBlockInterlocked;
alignas(16) GPR_reg64 g_cpuConstRegs[32];
u32 g_cpuHasConstReg, g_cpuFlushedConstReg;
EEINST *g_pCurInstInfo;
u16 g_x86AllocCounter;
_x86regs x86regs[iREGCNT_GPR];
_xmmregs xmmregs[iREGCNT_XMM];

UNREACHABLE(int _allocX86reg(int, int, int), "_allocX86reg")
UNREACHABLE(bool _hasX86reg(int, int, int), "_hasX86reg")
UNREACHABLE(void _freeX86reg(const x86Emitter::xRegister32 &), "_freeX86reg")
UNREACHABLE(void _freeX86reg(int), "_freeX86reg")
UNREACHABLE(void _freeX86regWithoutWriteback(int), "_freeX86regWithoutWriteback")
UNREACHABLE(int _allocTempXMMreg(XMMSSEType), "_allocTempXMMreg")
UNREACHABLE(int _allocGPRtoXMMreg(int, int), "_allocGPRtoXMMreg")
UNREACHABLE(void _reallocateXMMreg(int, int, int, int, bool), "_reallocateXMMreg")
UNREACHABLE(int _checkXMMreg(int, int, int), "_checkXMMreg")
UNREACHABLE(void _freeXMMreg(int), "_freeXMMreg")
UNREACHABLE(void _freeXMMregWithoutWriteback(int), "_freeXMMregWithoutWriteback")
UNREACHABLE(int _allocVFtoXMMreg(int, int), "_allocVFtoXMMreg")
UNREACHABLE(int _allocIfUsedGPRtoX86(int, int), "_allocIfUsedGPRtoX86")
UNREACHABLE(int _allocIfUsedVItoX86(int, int), "_allocIfUsedVItoX86")
UNREACHABLE(int _allocIfUsedGPRtoXMM(int, int), "_allocIfUsedGPRtoXMM")
UNREACHABLE(bool TrySwapDelaySlot(u32, u32, u32, bool), "TrySwapDelaySlot")
UNREACHABLE(void iFlushCall(int), "iFlushCall")
UNREACHABLE(void recCall(void (*)()), "recCall")
UNREACHABLE(u32 scaleblockcycles_clear(), "scaleblockcycles_clear")
UNREACHABLE(void _eeMoveGPRtoR(const x86Emitter::xRegister32 &, int, bool), "_eeMoveGPRtoR")
UNREACHABLE(void _eeMoveGPRtoM(uptr, int), "_eeMoveGPRtoM")
UNREACHABLE(void _eeFlushAllDirty(), "_eeFlushAllDirty")
UNREACHABLE(void _deleteEEreg128(int), "_deleteEEreg128")
UNREACHABLE(int vtlb_DynGenReadQuad(u32, int, vtlb_ReadRegAllocCallback), "vtlb_DynGenReadQuad")
UNREACHABLE(int vtlb_DynGenReadQuad_Const(u32, u32, vtlb_ReadRegAllocCallback), "vtlb_DynGenReadQuad_Const")
UNREACHABLE(void vtlb_DynGenWrite(u32, bool, int, int), "vtlb_DynGenWrite")
UNREACHABLE(void vtlb_DynGenWrite_Const(u32, bool, u32, int), "vtlb_DynGenWrite_Const")
UNREACHABLE(void VCALLMS(), "VCALLMS")
UNREACHABLE(void VCALLMSR(), "VCALLMSR")
UNREACHABLE(void _vu0WaitMicro(), "_vu0WaitMicro")
UNREACHABLE(void _vu0FinishMicro(), "_vu0FinishMicro")
UNREACHABLE(void vu1Finish(bool), "vu1Finish")
UNREACHABLE(void vu1ExecMicro(u32), "vu1ExecMicro")
UNREACHABLE(void BaseVUmicroCPU::ExecuteBlockJIT(BaseVUmicroCPU *, bool), "BaseVUmicroCPU::ExecuteBlockJIT")

namespace R5900::Dynarec
{
    UNREACHABLE(void recDoBranchImm(u32, u32 *, bool, bool), "recDoBranchImm")
}
