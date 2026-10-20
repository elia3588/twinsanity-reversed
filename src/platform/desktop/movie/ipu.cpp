#include "mpeg.h"

#include <cmath>
#include <cstring>
#include <vector>

// The default matrices and the VQ colour table the IPU starts with (the game's data)
extern "C"
{
    extern const u8 g_IpuDefaultMatrices[0x50] RETAIL(D_002E8310);
    extern const u8 g_IpuVqClut[0x20] RETAIL(D_002E8360);
}

// The IPU as software: the commands the movie decoder sends it are decoded here instead of by the EE's hardware, so a PSS file's
// MPEG-2 video is decoded on the desktop. The bit-reading commands read the video's elementary stream, which the decoder hands
// over once (IpuSetBitstream); the matrix, the VQ table and the colour conversion's macroblocks come through the toIPU channel
// (D4), which the software keeps in an auxiliary buffer. The DMA channels complete at once: what the decoder sets up before an
// IPU command has already arrived when the command runs.

// The DMA controller's registers, by channel (hardware.h declares them)
u32 g_DmaMadr[16];
u32 g_DmaQwc[16];
u32 g_DmaTadr[16];
u32 g_DmaChcr[16];
u32 g_DmaEnableWrite;
u32 g_DmaEnableRead;
u32 g_DmaStat;

namespace
{
// The scratchpad's 16 KiB (only the RAW8 output path copies through it)
u8 g_Scratchpad[0x4000];

// The video's elementary stream and the bit the IPU reads next
const u8* g_Stream;
u64 g_StreamBits;
u64 g_ReadBit;

// What the toIPU channel sends: quantiser matrices, the VQ table, the colour conversion's macroblocks
std::vector<u8> g_Aux;
u64 g_AuxBit;

// IPU_CTRL, the last command's result (never busy); IPU_BP and IPU_TOP are the registers the decoder reads (hardware.h's
// g_IpuBitPosition and g_IpuTop, which UpdatePosition keeps up to date)
u32 g_Control;
u64 g_Result;

// The quantiser matrices as the stream gives them (zig-zag order)
u8 g_IntraMatrix[64];
u8 g_NonIntraMatrix[64];

// The last macroblock type (MacroblockType's flags) and its coded block pattern
s32 g_MacroblockType;
s32 g_CodedBlockPattern;

// The intra DC predictors: luminance, Cb and Cr
// The DC prediction's own middle: a block with no coefficients is the flat grey of the samples' middle (128), which the IDCT's
// scale makes the intra coefficients' 128 * 8
constexpr s32 DcMiddle = 128 << 3;
s32 g_DcPrediction[3];

void* Pointer(u32 address)
{
    // The PS2's DMA addresses carry a segment in their top bits (the uncached and accelerated segments, the scratchpad's too),
    // which the desktop's own addresses don't have and must not lose: a desktop pointer's own bits reach past those segments',
    // and the copies here add no segment of their own (the scratchpad mode isn't used, the buffers being the state's own). The
    // scratchpad's DMA encoding (its address's own bit) comes off, so the scratchpad transfers' fetched bytes' address is theirs
    if ((address & DmaScratchpad) != 0)
    {
        address &= ~DmaScratchpad;
    }

    return reinterpret_cast<void*>(static_cast<uintptr_t>(address));
}

// A command's input through the toIPU channel, delivered before the command runs
void PullToIpu()
{
    if ((g_DmaChcr[IpuToChannel] & ChcrStart) != 0 && g_DmaQwc[IpuToChannel] != 0)
    {
        u32 bytes = g_DmaQwc[IpuToChannel] * QuadwordBytes;
        const u8* source = static_cast<const u8*>(Pointer(g_DmaMadr[IpuToChannel]));
        g_Aux.insert(g_Aux.end(), source, source + bytes);
        g_DmaQwc[IpuToChannel] = 0;
        g_DmaChcr[IpuToChannel] = 0;
    }
}

u32 ReadBit()
{
    u64 bit = g_ReadBit++;
    if (bit >= g_StreamBits)
    {
        return 0;
    }

    return (g_Stream[bit >> 3] >> (7 - (bit & 7))) & 1;
}

u32 ReadBits(s32 count)
{
    u32 value = 0;
    for (s32 i = 0; i < count; i++)
    {
        value = (value << 1) | ReadBit();
    }

    return value;
}

u32 BitsAt(u64 position, s32 count)
{
    u32 value = 0;
    for (s32 i = 0; i < count; i++)
    {
        u64 bit = position + i;
        u32 one = bit < g_StreamBits ? (g_Stream[bit >> 3] >> (7 - (bit & 7))) & 1 : 0;
        value = (value << 1) | one;
    }

    return value;
}

// A variable length code: its bits from the stream, its value
struct Vlc
{
    struct Node
    {
        s16 child[2];
        s16 value;
    };

    Node nodes[4096];
    s32 used;

    void Build(const u16* codes, const u8* lengths, const s16* values, s32 count)
    {
        used = 1;
        nodes[0].child[0] = -1;
        nodes[0].child[1] = -1;
        nodes[0].value = -1;
        for (s32 i = 0; i < count; i++)
        {
            s32 node = 0;
            for (s32 bit = lengths[i] - 1; bit >= 0; bit--)
            {
                s32 next = (codes[i] >> bit) & 1;
                if (nodes[node].child[next] < 0)
                {
                    nodes[used].child[0] = -1;
                    nodes[used].child[1] = -1;
                    nodes[used].value = -1;
                    nodes[node].child[next] = static_cast<s16>(used++);
                }

                node = nodes[node].child[next];
            }

            nodes[node].value = values[i];
        }
    }

    s32 Decode() const
    {
        s32 node = 0;
        while (nodes[node].value < 0)
        {
            s32 next = nodes[node].child[ReadBit()];
            if (next < 0)
            {
                return 0;
            }

            node = next;
        }

        return nodes[node].value;
    }
};

// The zig-zag scan's raster positions
const u8 g_Zigzag[64] =
{
    0, 1, 8, 16, 9, 2, 3, 10,
    17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

// The zig-zag position of each raster position: quantiser matrices are transmitted in zig-zag order whatever the
// scan in use, so their weights are looked up through this
u8 g_RasterToZigzag[64];

// The alternate scan's raster positions
const u8 g_AlternateScan[64] =
{
    0, 8, 16, 24, 1, 9, 2, 10,
    17, 25, 32, 40, 48, 56, 57, 49,
    41, 33, 26, 18, 3, 11, 4, 12,
    19, 27, 34, 42, 50, 58, 35, 43,
    51, 59, 20, 28, 5, 13, 6, 14,
    21, 29, 36, 44, 52, 60, 37, 45,
    53, 61, 22, 30, 7, 15, 23, 31,
    38, 46, 54, 62, 39, 47, 55, 63
};

// The DCT coefficient tables' (run, level) symbols: 40 with run 0, 18 with run 1, and fewer as the runs grow
s16 g_Run[111];
s16 g_Level[111];

// The tables the CPU decodes: the address increment, the macroblock types, the motion code, the DM vector, the coded block
// pattern, the two DCT coefficient tables and the DC sizes
Vlc g_AddressIncrement;
Vlc g_MacroblockTypeI;
Vlc g_MacroblockTypeP;
Vlc g_MacroblockTypeB;
Vlc g_MotionCode;
Vlc g_DmVector;
Vlc g_BlockPattern;
Vlc g_DctZero;
Vlc g_DctOne;
Vlc g_DcLuminance;
Vlc g_DcChrominance;

void BuildTable(Vlc& table, const u16* codes, const u8* lengths, s32 count)
{
    std::vector<s16> values(count);
    for (s32 i = 0; i < count; i++)
    {
        values[i] = static_cast<s16>(i);
    }

    table.Build(codes, lengths, values.data(), count);
}

void BuildTables()
{
    for (s32 i = 0; i < 64; i++)
    {
        g_RasterToZigzag[g_Zigzag[i]] = static_cast<u8>(i);
    }

    // The (run, level) symbols, in the tables' order
    s32 symbol = 0;
    for (s32 level = 1; level <= 40; level++)
    {
        g_Run[symbol] = 0;
        g_Level[symbol++] = static_cast<s16>(level);
    }

    for (s32 level = 1; level <= 18; level++)
    {
        g_Run[symbol] = 1;
        g_Level[symbol++] = static_cast<s16>(level);
    }

    for (s32 level = 1; level <= 5; level++)
    {
        g_Run[symbol] = 2;
        g_Level[symbol++] = static_cast<s16>(level);
    }

    for (s32 level = 1; level <= 4; level++)
    {
        g_Run[symbol] = 3;
        g_Level[symbol++] = static_cast<s16>(level);
    }

    for (s32 level = 1; level <= 3; level++)
    {
        g_Run[symbol] = 4;
        g_Level[symbol++] = static_cast<s16>(level);
    }

    for (s32 level = 1; level <= 3; level++)
    {
        g_Run[symbol] = 5;
        g_Level[symbol++] = static_cast<s16>(level);
    }

    for (s32 level = 1; level <= 3; level++)
    {
        g_Run[symbol] = 6;
        g_Level[symbol++] = static_cast<s16>(level);
    }

    for (s32 run = 7; run <= 16; run++)
    {
        for (s32 level = 1; level <= 2; level++)
        {
            g_Run[symbol] = static_cast<s16>(run);
            g_Level[symbol++] = static_cast<s16>(level);
        }
    }

    for (s32 run = 17; run <= 31; run++)
    {
        g_Run[symbol] = static_cast<s16>(run);
        g_Level[symbol++] = 1;
    }

    static const u16 addressCodes[36] =
    {
        0x1, 0x3, 0x2, 0x3, 0x2, 0x3, 0x2, 0x7, 0x6, 0xB, 0xA, 0x9, 0x8, 0x7, 0x6, 0x17, 0x16, 0x15,
        0x14, 0x13, 0x12, 0x23, 0x22, 0x21, 0x20, 0x1F, 0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x8, 0xF, 0x0
    };
    static const u8 addressLengths[36] =
    {
        1, 3, 3, 4, 4, 5, 5, 7, 7, 8, 8, 8, 8, 8, 8, 10, 10, 10,
        10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 8
    };
    s16 addressValues[36];
    for (s32 i = 0; i < 36; i++)
    {
        addressValues[i] = static_cast<s16>(i < 33 ? i + 1 : (i == 33 ? 35 : (i == 34 ? 34 : 0)));
    }

    g_AddressIncrement.Build(addressCodes, addressLengths, addressValues, 36);

    static const u16 typeICodes[2] = {0x1, 0x1};
    static const u8 typeILengths[2] = {1, 2};
    static const s16 typeIValues[2] = {1, 17};
    g_MacroblockTypeI.Build(typeICodes, typeILengths, typeIValues, 2);

    static const u16 typePCodes[7] = {0x3, 0x1, 0x1, 0x1, 0x1, 0x1, 0x2};
    static const u8 typePLengths[7] = {5, 2, 3, 1, 6, 5, 5};
    static const s16 typePValues[7] = {1, 2, 8, 10, 17, 18, 26};
    g_MacroblockTypeP.Build(typePCodes, typePLengths, typePValues, 7);

    static const u16 typeBCodes[11] = {0x3, 0x2, 0x3, 0x2, 0x3, 0x2, 0x3, 0x1, 0x2, 0x3, 0x2};
    static const u8 typeBLengths[11] = {5, 3, 3, 4, 4, 2, 2, 6, 6, 6, 5};
    static const s16 typeBValues[11] = {1, 4, 6, 8, 10, 12, 14, 17, 22, 26, 30};
    g_MacroblockTypeB.Build(typeBCodes, typeBLengths, typeBValues, 11);

    static const u16 motionCodes[17] =
    {
        0x1, 0x1, 0x1, 0x1, 0x3, 0x5, 0x4, 0x3, 0xB, 0xA, 0x9, 0x11, 0x10, 0xF, 0xE, 0xD, 0xC
    };
    static const u8 motionLengths[17] = {1, 2, 3, 4, 6, 7, 7, 7, 9, 9, 9, 10, 10, 10, 10, 10, 10};
    BuildTable(g_MotionCode, motionCodes, motionLengths, 17);

    static const u16 dmCodes[3] = {0x0, 0x2, 0x3};
    static const u8 dmLengths[3] = {1, 2, 2};
    static const s16 dmValues[3] = {0, 1, -1};
    g_DmVector.Build(dmCodes, dmLengths, dmValues, 3);

    static const u16 patternCodes[64] =
    {
        0x1, 0xB, 0x9, 0xD, 0xD, 0x17, 0x13, 0x1F, 0xC, 0x16, 0x12, 0x1E, 0x13, 0x1B, 0x17, 0x13,
        0xB, 0x15, 0x11, 0x1D, 0x11, 0x19, 0x15, 0x11, 0xF, 0xF, 0xD, 0x3, 0xF, 0xB, 0x7, 0x7,
        0xA, 0x14, 0x10, 0x1C, 0xE, 0xE, 0xC, 0x2, 0x10, 0x18, 0x14, 0x10, 0xE, 0xA, 0x6, 0x6,
        0x12, 0x1A, 0x16, 0x12, 0xD, 0x9, 0x5, 0x5, 0xC, 0x8, 0x4, 0x4, 0x7, 0xA, 0x8, 0xC
    };
    static const u8 patternLengths[64] =
    {
        9, 5, 5, 6, 4, 7, 7, 8, 4, 7, 7, 8, 5, 8, 8, 8,
        4, 7, 7, 8, 5, 8, 8, 8, 6, 8, 8, 9, 5, 8, 8, 9,
        4, 7, 7, 8, 6, 8, 8, 9, 5, 8, 8, 8, 5, 8, 8, 9,
        5, 8, 8, 8, 5, 8, 8, 9, 5, 8, 8, 9, 3, 5, 5, 6
    };
    BuildTable(g_BlockPattern, patternCodes, patternLengths, 64);

    static const u16 dctZeroCodes[113] =
    {
        0x3, 0x4, 0x5, 0x6, 0x26, 0x21, 0xA, 0x1D, 0x18, 0x13, 0x10, 0x1A, 0x19, 0x18, 0x17, 0x1F,
        0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x18,
        0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x3, 0x6, 0x25, 0xC, 0x1B, 0x16, 0x15, 0x1F,
        0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x13, 0x12, 0x11, 0x10, 0x5, 0x4, 0xB, 0x14, 0x14, 0x7,
        0x24, 0x1C, 0x13, 0x6, 0xF, 0x12, 0x7, 0x9, 0x12, 0x5, 0x1E, 0x14, 0x4, 0x15, 0x7, 0x11,
        0x5, 0x11, 0x27, 0x10, 0x23, 0x1A, 0x22, 0x19, 0x20, 0x18, 0xE, 0x17, 0xD, 0x16, 0x8, 0x15,
        0x1F, 0x1A, 0x19, 0x17, 0x16, 0x1F, 0x1E, 0x1D, 0x1C, 0x1B, 0x1F, 0x1E, 0x1D, 0x1C, 0x1B,
        0x1, 0x2
    };
    static const u8 dctZeroLengths[113] =
    {
        2, 4, 5, 7, 8, 8, 10, 12, 12, 12, 12, 13, 13, 13, 13, 14,
        14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 15,
        15, 15, 15, 15, 15, 15, 15, 15, 3, 6, 8, 10, 12, 13, 13, 15,
        15, 15, 15, 15, 15, 15, 16, 16, 16, 16, 4, 7, 10, 12, 13, 5,
        8, 12, 13, 5, 10, 12, 6, 10, 13, 6, 12, 16, 6, 12, 7, 12,
        7, 13, 8, 13, 8, 16, 8, 16, 8, 16, 10, 16, 10, 16, 10, 16,
        12, 12, 12, 12, 12, 13, 13, 13, 13, 13, 16, 16, 16, 16, 16, 6, 2
    };
    static const u16 dctOneCodes[113] =
    {
        0x02, 0x06, 0x07, 0x1C, 0x1D, 0x05, 0x04, 0x7B, 0x7C, 0x23, 0x22, 0xFA, 0xFB, 0xFE, 0xFF, 0x1F,
        0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x18,
        0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x02, 0x06, 0x79, 0x27, 0x20, 0x16, 0x15, 0x1F,
        0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x13, 0x12, 0x11, 0x10, 0x05, 0x07, 0xFC, 0x0C, 0x14, 0x07,
        0x26, 0x1C, 0x13, 0x06, 0xFD, 0x12, 0x07, 0x04, 0x12, 0x06, 0x1E, 0x14, 0x04, 0x15, 0x05, 0x11,
        0x78, 0x11, 0x7A, 0x10, 0x21, 0x1A, 0x25, 0x19, 0x24, 0x18, 0x05, 0x17, 0x07, 0x16, 0x0D, 0x15,
        0x1F, 0x1A, 0x19, 0x17, 0x16, 0x1F, 0x1E, 0x1D, 0x1C, 0x1B, 0x1F, 0x1E, 0x1D, 0x1C, 0x1B,
        0x01, 0x06
    };
    static const u8 dctOneLengths[113] =
    {
        2, 3, 4, 5, 5, 6, 6, 7, 7, 8, 8, 8, 8, 8, 8, 14,
        14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 15,
        15, 15, 15, 15, 15, 15, 15, 15, 3, 5, 7, 8, 8, 13, 13, 15,
        15, 15, 15, 15, 15, 15, 16, 16, 16, 16, 5, 7, 8, 10, 13, 5,
        8, 12, 13, 6, 8, 12, 6, 9, 13, 7, 12, 16, 7, 12, 7, 12,
        7, 13, 7, 13, 8, 16, 8, 16, 8, 16, 9, 16, 9, 16, 10, 16,
        12, 12, 12, 12, 12, 13, 13, 13, 13, 13, 16, 16, 16, 16, 16, 6, 4
    };

    // The last two entries are the escape and the end of block, which the block decoder handles itself
    struct DctValues
    {
        s16 values[113];
    };
    static DctValues zeroValues;
    static DctValues oneValues;
    for (s32 i = 0; i < 111; i++)
    {
        zeroValues.values[i] = static_cast<s16>(i);
        oneValues.values[i] = static_cast<s16>(i);
    }

    zeroValues.values[111] = 111;
    zeroValues.values[112] = 112;
    oneValues.values[111] = 111;
    oneValues.values[112] = 112;
    g_DctZero.Build(dctZeroCodes, dctZeroLengths, zeroValues.values, 113);
    g_DctOne.Build(dctOneCodes, dctOneLengths, oneValues.values, 113);

    static const u16 dcLumCodes[12] = {0x4, 0x0, 0x1, 0x5, 0x6, 0xE, 0x1E, 0x3E, 0x7E, 0xFE, 0x1FE, 0x1FF};
    static const u8 dcLumLengths[12] = {3, 2, 2, 3, 3, 4, 5, 6, 7, 8, 9, 9};
    BuildTable(g_DcLuminance, dcLumCodes, dcLumLengths, 12);

    static const u16 dcChromaCodes[12] = {0x0, 0x1, 0x2, 0x6, 0xE, 0x1E, 0x3E, 0x7E, 0xFE, 0x1FE, 0x3FE, 0x3FF};
    static const u8 dcChromaLengths[12] = {2, 2, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10};
    BuildTable(g_DcChrominance, dcChromaCodes, dcChromaLengths, 12);
}

void UpdatePosition()
{
    g_IpuTop = BitsAt(g_ReadBit, 32);
    g_IpuBitPosition = static_cast<u32>(g_ReadBit & 0x7F) | (15u << 8);
}

// The inverse DCT (the standard one, not the IPU's own rounding): a block of coefficients to the samples they stand for
void Idct(const s16* coefficients, s16* samples)
{
    static double cosine[8][8];
    static bool built = false;
    if (!built)
    {
        for (s32 x = 0; x < 8; x++)
        {
            for (s32 u = 0; u < 8; u++)
            {
                cosine[x][u] = std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0);
            }
        }

        built = true;
    }

    double temp[64];
    for (s32 y = 0; y < 8; y++)
    {
        for (s32 x = 0; x < 8; x++)
        {
            double sum = 0;
            for (s32 u = 0; u < 8; u++)
            {
                double scale = u == 0 ? 0.70710678118654752440 : 1.0;
                sum += scale * coefficients[y * 8 + u] * cosine[x][u];
            }

            temp[y * 8 + x] = sum * 0.5;
        }
    }

    for (s32 y = 0; y < 8; y++)
    {
        for (s32 x = 0; x < 8; x++)
        {
            double sum = 0;
            for (s32 v = 0; v < 8; v++)
            {
                double scale = v == 0 ? 0.70710678118654752440 : 1.0;
                sum += scale * temp[v * 8 + x] * cosine[y][v];
            }

            sum *= 0.5;
            s32 value = static_cast<s32>(std::floor(sum + 0.5));
            if (value < -32768)
            {
                value = -32768;
            }
            else if (value > 32767)
            {
                value = 32767;
            }

            samples[y * 8 + x] = static_cast<s16>(value);
        }
    }
}

// A block of a macroblock: its coefficients decoded, dequantised, inverse transformed
void DecodeBlock(s16* block, s32 index, s32 intra, s32 quantiserScale, s32 precision, s32 mpeg1, s32 intraVlc,
                 s32 alternateScan)
{
    const u8* scan = alternateScan ? g_AlternateScan : g_Zigzag;
    const u8* matrix = intra ? g_IntraMatrix : g_NonIntraMatrix;
    s32 component = index < 4 ? 0 : index - 3;

    std::memset(block, 0, 64 * sizeof(s16));

    s32 position;
    s32 mismatch = 1;
    if (intra)
    {
        const Vlc& dc = index < 4 ? g_DcLuminance : g_DcChrominance;
        s32 size = dc.Decode();
        s32 difference = 0;
        if (size > 0)
        {
            s32 value = static_cast<s32>(ReadBits(size));
            if (value < (1 << (size - 1)))
            {
                value -= (1 << size) - 1;
            }

            difference = value;
        }

        g_DcPrediction[component] += difference;

        s32 dcValue = g_DcPrediction[component] << (mpeg1 ? 3 : (3 - precision));
        block[0] = static_cast<s16>(dcValue);
        mismatch = dcValue ^ 1;
        // The DC is at the first scan position, so the first alternate coefficient lands past it
        position = 0;
    }
    else
    {
        // A non intra block's first coefficient can take the first scan position itself
        position = -1;

        // A coefficient of level one in the first scan position is coded as a one bit code and its sign bit, the run level
        // table's code for it being longer: a coded block always has coefficients, so this cannot be an end of block code
        if (BitsAt(g_ReadBit, 1) != 0)
        {
            s32 level = BitsAt(g_ReadBit + 1, 1) != 0 ? -1 : 1;
            ReadBits(2);

            s32 magnitude = level < 0 ? -level : level;
            s32 coefficient = ((2 * magnitude + 1) * quantiserScale * matrix[0]) >> 5;
            if (level < 0)
            {
                coefficient = -coefficient;
            }

            block[0] = static_cast<s16>(coefficient);
            mismatch ^= coefficient;
            position = 0;

            // The block is only this coefficient when an end of block code follows it
            if (BitsAt(g_ReadBit, 2) == 2)
            {
                ReadBits(2);
                position = 64;
            }
        }
    }

    const Vlc& ac = (intra && intraVlc) ? g_DctOne : g_DctZero;
    while (position < 64)
    {
        s32 symbol = ac.Decode();
        if (symbol == 112)
        {
            break;
        }

        s32 run;
        s32 level;
        if (symbol == 111)
        {
            run = static_cast<s32>(ReadBits(6)) + 1;
            if (mpeg1)
            {
                level = static_cast<s8>(ReadBits(8));
                if (level == -128)
                {
                    level = static_cast<s32>(ReadBits(8)) - 256;
                }
                else if (level == 0)
                {
                    level = static_cast<s32>(ReadBits(8));
                }
            }
            else
            {
                level = static_cast<s32>(ReadBits(12));
                if ((level & 0x800) != 0)
                {
                    level |= ~0x7FF;
                }
            }
        }
        else if (symbol >= 0 && symbol < 111)
        {
            run = g_Run[symbol] + 1;
            level = g_Level[symbol];
        }
        else
        {
            break;
        }

        position += run;
        if (position >= 64)
        {
            break;
        }

        // A coefficient's sign bit is only in the stream when the coefficient itself is
        s32 normal = symbol >= 0 && symbol < 111;
        if (normal && level != 0 && ReadBit() != 0)
        {
            level = -level;
        }

        s32 raster = scan[position];
        s32 coefficient;

        // The matrix comes in zig-zag order too, so a coefficient's weight is the one its raster position calls for
        s32 weight = matrix[g_RasterToZigzag[raster]];
        if (intra)
        {
            coefficient = (level * quantiserScale * weight) >> 4;
        }
        else
        {
            s32 magnitude = level < 0 ? -level : level;
            coefficient = ((2 * magnitude + 1) * quantiserScale * weight) >> 5;
            if (level < 0)
            {
                coefficient = -coefficient;
            }
        }

        block[raster] = static_cast<s16>(coefficient);
        mismatch ^= coefficient;
    }

    block[63] ^= static_cast<s16>(mismatch & 1);

    Idct(block, block);
}

void DecodeVariable(s32 table)
{
    u64 start = g_ReadBit;
    s32 value = 0;
    switch (table)
    {
    case 0:
        value = g_AddressIncrement.Decode();
        break;
    case 1:
    {
        s32 picture = static_cast<s32>((g_Control >> IpuControlPictureTypeShift) & 7);
        if (picture == MpegPictureI)
        {
            value = g_MacroblockTypeI.Decode();
        }
        else if (picture == MpegPictureP)
        {
            value = g_MacroblockTypeP.Decode();
        }
        else
        {
            value = g_MacroblockTypeB.Decode();
        }

        g_MacroblockType = value;
        break;
    }
    case 2:
    {
        value = g_MotionCode.Decode();
        if (value > 0 && ReadBit() != 0)
        {
            value = -value;
        }
        break;
    }
    case 3:
        value = g_DmVector.Decode();
        break;
    default:
        break;
    }

    u32 length = static_cast<u32>(g_ReadBit - start);
    g_Result = static_cast<u32>(static_cast<u16>(value)) | (length << 16);
    UpdatePosition();
}

void DecodeMacroblock(u32 command)
{
    s32 quantiserScale = static_cast<s32>((command >> IpuBlockQuantiserShift) & 0x1F);
    s32 dctType = static_cast<s32>((command >> IpuBlockDctTypeShift) & 1);
    s32 dcReset = static_cast<s32>((command >> IpuBlockDcResetShift) & 1);
    s32 intra = static_cast<s32>((command >> IpuBlockIntraShift) & 1);
    s32 mpeg1 = static_cast<s32>((g_Control >> 23) & 1);
    s32 precision = static_cast<s32>((g_Control >> IpuControlDcPrecisionShift) & 3);
    s32 quantiserType = static_cast<s32>((g_Control >> IpuControlQuantiserTypeShift) & 1);
    s32 intraVlc = static_cast<s32>((g_Control >> IpuControlIntraVlcShift) & 1);
    s32 alternateScan = static_cast<s32>((g_Control >> IpuControlAlternateScanShift) & 1);

    if (quantiserType != 0)
    {
        static const s32 nonLinear[32] =
        {
            0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 18, 20, 22,
            24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112
        };
        quantiserScale = nonLinear[quantiserScale & 31];
    }

    if (dcReset != 0)
    {
        s32 middle = mpeg1 != 0 ? DcMiddle : 128 << precision;
        g_DcPrediction[0] = middle;
        g_DcPrediction[1] = middle;
        g_DcPrediction[2] = middle;
    }

    // A macroblock's coded block pattern comes after its modes and motion vectors, so it is read here and not with the
    // macroblock type
    if (intra == 0 && (g_MacroblockType & 2) != 0)
    {
        g_CodedBlockPattern = g_BlockPattern.Decode();
        g_Control = (g_Control & ~0x3F00u) | (static_cast<u32>(g_CodedBlockPattern) << 8);
    }

    s32 pattern = (g_MacroblockType & 2) != 0 ? g_CodedBlockPattern : (intra != 0 ? 0x3F : 0);
    s16 macroblock[6 * 64];
    std::memset(macroblock, 0, sizeof(macroblock));
    for (s32 block = 0; block < 6; block++)
    {
        if ((pattern & (1 << (5 - block))) != 0)
        {
            DecodeBlock(macroblock + block * 64, block, intra, quantiserScale, precision, mpeg1, intraVlc,
                        alternateScan);
        }
    }

    // A macroblock's pixels: the luma 16 rows of 16, the chroma's Cb and Cr 8 rows of 8 after them. The blocks were decoded as
    // quadrants in the order the DCT types give them: a frame's are the macroblock's own, a field's the two fields' (its even
    // rows of one, its odd of the other)
    const bool fieldPicture = dctType != 0;
    {
        s16 luma[256];
        for (s32 y = 0; y < 16; y++)
        {
            for (s32 x = 0; x < 16; x++)
            {
                s32 side = x >= 8 ? 1 : 0;
                s32 block;
                s32 row;
                if (!fieldPicture)
                {
                    block = (y >= 8 ? 2 : 0) + side;
                    row = y & 7;
                }
                else
                {
                    block = ((y & 1) != 0 ? 2 : 0) + side;
                    row = y >> 1;
                }

                luma[y * 16 + x] = macroblock[block * 64 + row * 8 + (x & 7)];
            }
        }

        std::memcpy(macroblock, luma, sizeof(luma));
    }

    std::memcpy(Pointer(g_DmaMadr[IpuFromChannel]), macroblock, sizeof(macroblock));
    g_DmaQwc[IpuFromChannel] = 0;
    g_DmaChcr[IpuFromChannel] = 0;
    UpdatePosition();
}

// The IPU's colour conversion: the auxiliary buffer's 8 bit macroblocks to 32 bit pixels (BT.601)
void ConvertMacroblocks(s32 macroblocks)
{
    const u8* source = g_Aux.data() + (g_AuxBit >> 3);
    u8* output = static_cast<u8*>(Pointer(g_DmaMadr[IpuFromChannel]));
    for (s32 macroblock = 0; macroblock < macroblocks; macroblock++)
    {
        const u8* input = source + macroblock * MacroblockBytes;
        u8* pixels = output + macroblock * (Rgb32MacroblockQuadwords << 4);
        for (s32 y = 0; y < 16; y++)
        {
            for (s32 x = 0; x < 16; x++)
            {
                // A macroblock is its luma's 16 rows of 16, then Cb's 8 rows of 8 and Cr's
                s32 luma = input[y * 16 + x];
                s32 cb = input[256 + (y >> 1) * 8 + (x >> 1)] - 128;
                s32 cr = input[320 + (y >> 1) * 8 + (x >> 1)] - 128;
                s32 l = luma - 16;
                s32 red = (298 * l + 409 * cr + 128) >> 8;
                s32 green = (298 * l - 100 * cb - 208 * cr + 128) >> 8;
                s32 blue = (298 * l + 516 * cb + 128) >> 8;
                red = red < 0 ? 0 : (red > 255 ? 255 : red);
                green = green < 0 ? 0 : (green > 255 ? 255 : green);
                blue = blue < 0 ? 0 : (blue > 255 ? 255 : blue);
                u8* pixel = pixels + (y * 16 + x) * 4;
                pixel[0] = static_cast<u8>(red);
                pixel[1] = static_cast<u8>(green);
                pixel[2] = static_cast<u8>(blue);
                pixel[3] = 0xFF;
            }
        }
    }

    g_DmaQwc[IpuFromChannel] = 0;
    g_DmaChcr[IpuFromChannel] = 0;
}

void SetMatrix(bool intra)
{
    const u8* source = g_Aux.data() + (g_AuxBit >> 3);
    std::memcpy(intra ? g_IntraMatrix : g_NonIntraMatrix, source, 64);
    g_AuxBit += 64 * 8;
}

void RunCommand(u32 command)
{
    PullToIpu();
    switch (command & IpuCodeMask)
    {
    case IpuClearInput:
        g_Aux.clear();
        g_AuxBit = 0;
        break;
    case IpuDecodeFixed:
    {
        s32 count = static_cast<s32>(command & 0x3F);
        g_ReadBit += count;
        g_Result = BitsAt(g_ReadBit, 32);
        UpdatePosition();
        break;
    }
    case IpuDecodeVariable:
        DecodeVariable(static_cast<s32>((command >> 26) & 3));
        break;
    case IpuDecodeBlock:
        DecodeMacroblock(command);
        break;
    case IpuSetIntraMatrix:
    case IpuSetNonIntraMatrix:
        // SETIQ's bit 27 is the non-intra matrix's (both codes are above the mask the switch sees)
        SetMatrix((command & 0x08000000) == 0);
        break;
    case IpuSetVqClut:
        g_AuxBit += 32 * 8;
        break;
    case IpuSetThresholds:
        break;
    case IpuConvert:
        ConvertMacroblocks(static_cast<s32>(command & 0xFFFF));
        break;
    default:
        break;
    }
}
}

// A scratchpad channel's transfer: the bytes move now and the channel is done
void DmaTransfer(u32 channel, u32)
{
    u32 bytes = g_DmaQwc[channel] * QuadwordBytes;
    if (channel == IpuToChannel)
    {
        const u8* source = static_cast<const u8*>(Pointer(g_DmaMadr[channel]));
        g_Aux.insert(g_Aux.end(), source, source + bytes);
    }
    else if (channel == ScratchpadFromChannel)
    {
        std::memcpy(g_Scratchpad + (g_DmaTadr[channel] & 0x3FFF), Pointer(g_DmaMadr[channel]), bytes);
    }
    else if (channel == ScratchpadToChannel)
    {
        std::memcpy(Pointer(g_DmaMadr[channel]), g_Scratchpad + (g_DmaTadr[channel] & 0x3FFF), bytes);
    }

    g_DmaQwc[channel] = 0;
    g_DmaChcr[channel] = 0;
}

// The IPU's registers
IpuCommandPort g_IpuCommandPort;
IpuControlPort g_IpuControlPort;
u32 g_IpuBitPosition;
u64 g_IpuTop;

IpuCommandPort& IpuCommandPort::operator=(u32 command)
{
    RunCommand(command);
    return *this;
}

IpuCommandPort::operator u64() const
{
    return g_Result;
}

IpuControlPort& IpuControlPort::operator=(u32 control)
{
    g_Control = control;
    if ((control & IpuControlReset) != 0)
    {
        g_Control = 0;
        g_DcPrediction[0] = DcMiddle;
        g_DcPrediction[1] = DcMiddle;
        g_DcPrediction[2] = DcMiddle;
        g_MacroblockType = 0;
        g_CodedBlockPattern = 0;
    }

    return *this;
}

IpuControlPort::operator u32() const
{
    return g_Control;
}

// The video's elementary stream, which the bit-reading commands read
void IpuSetBitstream(const u8* data, s32 size)
{
    BuildTables();
    g_Stream = data;
    g_StreamBits = static_cast<u64>(size) * 8;
    g_ReadBit = 0;
    g_Aux.clear();
    g_AuxBit = 0;
    g_Control = 0;
    g_Result = 0;
    g_MacroblockType = 0;
    g_CodedBlockPattern = 0;
    g_DcPrediction[0] = DcMiddle;
    g_DcPrediction[1] = DcMiddle;
    g_DcPrediction[2] = DcMiddle;
    UpdatePosition();
}

namespace Libipu
{
void SetFromIpuChcr(u32 chcr)
{
    g_DmaChcr[IpuFromChannel] = chcr;
}

void SetToIpuChcr(u32 chcr)
{
    g_DmaChcr[IpuToChannel] = chcr;
}

void SetToIpuChcrForInit(u32 chcr)
{
    g_DmaChcr[IpuToChannel] = chcr;
}
}

extern "C"
{
void sceIpuStopDMA(IpuDmaEnvironment* environment)
{
    Libipu::SetToIpuChcr(ChcrFromMemory);
    environment->toMadr = g_DmaMadr[IpuToChannel];
    environment->toTadr = g_DmaTadr[IpuToChannel];
    environment->toQwc = g_DmaQwc[IpuToChannel];
    environment->toChcr = g_DmaChcr[IpuToChannel];
    Libipu::SetFromIpuChcr(0);
    environment->fromMadr = g_DmaMadr[IpuFromChannel];
    environment->fromQwc = g_DmaQwc[IpuFromChannel];
    environment->fromChcr = g_DmaChcr[IpuFromChannel];
    environment->bitPosition.value = g_IpuBitPosition;
    environment->control.value = g_Control;
}

void sceIpuRestartDMA(IpuDmaEnvironment* environment)
{
    // The PS2 continues the input DMA where it left off, counting the quadwords its FIFO still holds as unread and sending them
    // again (the software IPU has no FIFO: the video's stream is one buffer it reads by bit position, movie/mpegbits.cpp, so the
    // saved toIPU state is resumed exactly as it was)
    if (environment->fromMadr != 0 && environment->fromQwc != 0)
    {
        g_DmaMadr[IpuFromChannel] = environment->fromMadr;
        g_DmaQwc[IpuFromChannel] = environment->fromQwc;
        Libipu::SetFromIpuChcr(environment->fromChcr | ChcrStart);
    }

    g_Aux.clear();
    g_AuxBit = 0;
    if (environment->toMadr != 0 && environment->toQwc != 0)
    {
        g_DmaMadr[IpuToChannel] = environment->toMadr;
        g_DmaTadr[IpuToChannel] = environment->toTadr;
        g_DmaQwc[IpuToChannel] = environment->toQwc;
        Libipu::SetToIpuChcr(environment->toChcr | ChcrStart);
    }
}

s32 sceIpuSync(s32 mode, u16)
{
    if (mode == IpuSyncPoll)
    {
        return 0;
    }

    return 0;
}

void sceIpuInit()
{
    Libipu::SetToIpuChcrForInit(ChcrFromMemory);
    g_Control = 0;
    g_Aux.clear();
    g_AuxBit = 0;
    // The intra matrix, then the non-intra one (its own default: the one row of sixteens four times)
    g_Aux.insert(g_Aux.end(), g_IpuDefaultMatrices, g_IpuDefaultMatrices + 64);
    g_Aux.insert(g_Aux.end(), 64, static_cast<u8>(0x10));
    g_Aux.insert(g_Aux.end(), g_IpuVqClut, g_IpuVqClut + 32);
    RunCommand(IpuSetIntraMatrix);
    RunCommand(IpuSetNonIntraMatrix);
    RunCommand(IpuSetVqClut);
    g_Control = 0;
    g_Aux.clear();
    g_AuxBit = 0;
}
}
