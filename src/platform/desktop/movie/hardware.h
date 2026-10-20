#pragma once

#include "common.h"

// The PS2 hardware the movie decoder drives, as software on the desktop. The decoder (libmpeg and libipu, the copies in this
// directory) drives the IPU through its registers (A_EE_IPU_*) and the DMA controller's channels (R_EE_D3_*, the IPU's own, and
// R_EE_D8_*/R_EE_D9_* for the scratchpad): here the registers are ordinary variables and the transfers are memory copies, done
// where the hardware would do them (the IPU itself for its own channels, DmaTransfer for the scratchpad's). The names are the
// PS2's, so the copies read as they did.

// The EE's addresses: the scratchpad's, the mask a DMA address keeps (the top bits pick the segment), the uncached segments, and
// the scratchpad's encoding in a DMA address. The desktop's pointers are already physical (and 32 bit), so the mask keeps them.
// Unlike the PS2's (whose addresses top out at 0x1FFFFFFF), the desktop's own addresses reach past 0x10000000, so the masks keep
// a pointer's own bits: the only segment a desktop decoder copy adds is the scratchpad's (and the scratchpad mode isn't used,
// the desktop's buffers being MpegBuffersOwn), which Pointer restores too
constexpr u32 Scratchpad = 0x70000000;
constexpr u32 PhysicalMask = 0xFFFFFFFF;
constexpr u32 UncachedSegment = 0x20000000;
constexpr u32 UncachedAcceleratedSegment = 0x30000000;
constexpr u32 SegmentShift = 28;
constexpr u32 DmaScratchpad = 0x80000000;

// The DMA channels the movie uses: the IPU's from- and to- channel, and the scratchpad's two
constexpr u32 IpuFromChannel = 3;
constexpr u32 IpuToChannel = 4;
constexpr u32 ScratchpadFromChannel = 9;
constexpr u32 ScratchpadToChannel = 8;

// The DMA controller's registers, by channel: its memory address, its transfer's size in quadwords, its chain's tag and its
// control (a transfer starts when a channel's CHCR has ChcrStart, and is done when the controller clears it)
extern u32 g_DmaMadr[16];
extern u32 g_DmaQwc[16];
extern u32 g_DmaTadr[16];
extern u32 g_DmaChcr[16];
// D_ENABLEW/R and D_STAT
extern u32 g_DmaEnableWrite;
extern u32 g_DmaEnableRead;
extern u32 g_DmaStat;

#define R_EE_D3_MADR (g_DmaMadr + IpuFromChannel)
#define R_EE_D3_QWC (g_DmaQwc + IpuFromChannel)
#define R_EE_D3_TADR (g_DmaTadr + IpuFromChannel)
#define R_EE_D3_CHCR (g_DmaChcr + IpuFromChannel)
#define R_EE_D4_MADR (g_DmaMadr + IpuToChannel)
#define R_EE_D4_QWC (g_DmaQwc + IpuToChannel)
#define R_EE_D4_TADR (g_DmaTadr + IpuToChannel)
#define R_EE_D4_CHCR (g_DmaChcr + IpuToChannel)
#define R_EE_D8_MADR (g_DmaMadr + ScratchpadToChannel)
#define R_EE_D8_QWC (g_DmaQwc + ScratchpadToChannel)
#define R_EE_D8_SADR (g_DmaTadr + ScratchpadToChannel)
#define R_EE_D8_CHCR (g_DmaChcr + ScratchpadToChannel)
#define R_EE_D9_MADR (g_DmaMadr + ScratchpadFromChannel)
#define R_EE_D9_QWC (g_DmaQwc + ScratchpadFromChannel)
#define R_EE_D9_SADR (g_DmaTadr + ScratchpadFromChannel)
#define R_EE_D9_TADR (g_DmaTadr + ScratchpadFromChannel)
#define R_EE_D9_CHCR (g_DmaChcr + ScratchpadFromChannel)
#define R_EE_D_ENABLEW (&g_DmaEnableWrite)
#define R_EE_D_ENABLER (&g_DmaEnableRead)
#define R_EE_D_STAT (&g_DmaStat)

// Interrupts: nothing to mask on the desktop (the code only uses it to hold the DMA controller still while it writes a channel)
inline s32 DIntr()
{
    return 0;
}

inline void EIntr()
{
}

// A scratchpad channel's transfer (D8/D9): the bytes move now, and the channel is done (its CHCR's start bit cleared and its QWC
// zero), as the controller's would be. chcrStart is the ChcrStart the hardware is started with (mpeg.h's constant, passed in)
void DmaTransfer(u32 channel, u32 chcrStart);

// The IPU's registers. IPU_CMD is a port: writing it runs the command at once (the software IPU decodes synchronously) and reading
// it gives the command's result; IPU_CTRL is a port too, so a reset write clears the IPU's state. IPU_BP and IPU_TOP are the bit
// reader's state, which every command keeps current.
struct IpuCommandPort
{
    IpuCommandPort& operator=(u32 command);
    operator u64() const;
};

struct IpuControlPort
{
    IpuControlPort& operator=(u32 control);
    operator u32() const;
};

extern IpuCommandPort g_IpuCommandPort;
extern IpuControlPort g_IpuControlPort;
extern u32 g_IpuBitPosition;
extern u64 g_IpuTop;

// The IPU's registers the copies name directly (the same objects as the pointers mpeg.h makes)
#define R_EE_IPU_CMD (&g_IpuCommandPort)
#define R_EE_IPU_CTRL (&g_IpuControlPort)
#define R_EE_IPU_BP (&g_IpuBitPosition)
#define R_EE_IPU_TOP (&g_IpuTop)

// The kernel's DMAC and cache calls: the software completes each transfer where it's set up, so there is no interrupt handler to
// run and nothing to flush
inline s32 AddDmacHandler2(u32, s32 (*)(s32, void*, void*), s32, void*)
{
    return 0;
}

inline void RemoveDmacHandler(u32, s32)
{
}

inline void EnableDmac(u32)
{
}

inline void DisableDmac(u32)
{
}

inline void FlushCache(s32)
{
}

inline void ExitHandler()
{
}

// The video's elementary stream, which the decoder feeds once (the software IPU reads the bit-reading commands' bits from it, so
// the DMA path of a real IPU's input FIFO isn't needed). The bits are read MSB first.
void IpuSetBitstream(const u8* data, s32 size);
