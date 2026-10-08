#include "gs_texture.h"

#include <cstdlib>
#include <cstring>

// The PS2's GS texture formats decoded (see gs_texture.h): the same tables and swizzle functions as TT-Lab's EzSwizzle, in the
// same order, over little endian bytes
namespace
{
// The block a pixel's word or byte is in, and the word and byte within it (GS memory's layouts, TT-Lab's tables)
constexpr u32 Block32[32] = {
    0, 1, 4, 5, 16, 17, 20, 21, 2, 3, 6, 7, 18, 19, 22, 23, 8, 9, 12, 13, 24, 25, 28, 29, 10, 11, 14, 15, 26, 27, 30, 31,
};
constexpr u32 ColumnWord32[16] = {0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15};
constexpr u32 Block8[32] = {
    0, 1, 4, 5, 16, 17, 20, 21, 2, 3, 6, 7, 18, 19, 22, 23, 8, 9, 12, 13, 24, 25, 28, 29, 10, 11, 14, 15, 26, 27, 30, 31,
};
constexpr u32 ColumnWord8[2][64] = {
    {0, 1, 4, 5, 8, 9, 12, 13, 0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15, 2, 3, 6, 7, 10, 11, 14, 15, 8, 9, 12,
     13, 0, 1, 4, 5, 8, 9, 12, 13, 0, 1, 4, 5, 10, 11, 14, 15, 2, 3, 6, 7, 10, 11, 14, 15, 2, 3, 6, 7},
    {8, 9, 12, 13, 0, 1, 4, 5, 8, 9, 12, 13, 0, 1, 4, 5, 10, 11, 14, 15, 2, 3, 6, 7, 10, 11, 14, 15, 2, 3, 6, 7, 0, 1, 4,
     5, 8, 9, 12, 13, 0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15, 2, 3, 6, 7, 10, 11, 14, 15},
};
constexpr u32 ColumnByte8[64] = {
    0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2,
    1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 3, 3, 3, 3, 3, 3, 1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 3, 3, 3, 3, 3, 3,
};

// The PS2's pixel formats this knows (the rest grey)
constexpr u8 FormatPsmct32 = 0x00;
constexpr u8 FormatPsmt8 = 0x13;

u64 ReadU64(const u8* bytes)
{
    u64 value;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

u32 ReadU32(const u8* bytes)
{
    u32 value;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

u32 ReadU16(const u8* bytes)
{
    u16 value;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

// A pixel's word in GS memory (64 word pages of 32 bit blocks)
s32 Map32(s32 x, s32 y, s32 width)
{
    s32 pageX = x / 64;
    s32 pageY = y / 32;
    s32 page = pageX + pageY * width;
    s32 px = x - pageX * 64;
    s32 py = y - pageY * 32;
    s32 blockX = px / 8;
    s32 blockY = py / 8;
    s32 block = static_cast<s32>(Block32[blockX + blockY * 8]);
    s32 bx = px - blockX * 8;
    s32 by = py - blockY * 8;
    s32 column = by / 2;
    s32 cx = bx;
    s32 cy = by - column * 2;
    s32 cw = static_cast<s32>(ColumnWord32[cx + cy * 8]);
    return page * 2048 + block * 64 + column * 16 + cw;
}

// A pixel's byte in GS memory (256 byte pages of 8 bit blocks)
s32 Map8(s32 x, s32 y, s32 width, s32* byte)
{
    s32 pageX = x / 128;
    s32 pageY = y / 64;
    s32 page = pageX + pageY * width;
    s32 px = x - pageX * 128;
    s32 py = y - pageY * 64;
    s32 blockX = px / 16;
    s32 blockY = py / 16;
    s32 block = static_cast<s32>(Block8[blockX + blockY * 8]);
    s32 bx = px - blockX * 16;
    s32 by = py - blockY * 16;
    s32 column = by / 4;
    s32 cx = bx;
    s32 cy = by - column * 4;
    s32 cw = static_cast<s32>(ColumnWord8[column & 1][cx + cy * 16]);
    *byte = static_cast<s32>(ColumnByte8[cx + cy * 16]);
    return page * 2048 + block * 64 + column * 16 + cw;
}

// The packet's image taken as the 32 bit format it was transferred as: each of its words put where the swizzle would hold it
void WritePsmct32(u8* destination, s32 words, const u8* source, u32 sourceBytes, s32 rrw, s32 rrh)
{
    s32 src = 0;
    for (s32 y = 0; y < rrh; y++)
    {
        for (s32 x = 0; x < rrw; x++)
        {
            s32 dst = Map32(x, y, 1);
            if (4 * dst + 3 < words * 4 && static_cast<u32>(src + 3) < sourceBytes)
            {
                std::memcpy(destination + 4 * dst, source + src, 4);
            }
            src += 4;
        }
    }
}

// The bytes of the 8 bit format's image unscattered (dbp is its word offset, dbw its width in 64 pixel units)
void ReadPsmt8(u8* destination, s32 dbp, s32 dbw, s32 rrw, s32 rrh, const u8* source, s32 words)
{
    dbw >>= 1;
    s32 src = 0;
    s32 start = dbp * 64;
    for (s32 y = 0; y < rrh; y++)
    {
        for (s32 x = 0; x < rrw; x++)
        {
            s32 byte;
            s32 dst = start + Map8(x, y, dbw, &byte);
            if (byte >= 0 && static_cast<u64>(4 * dst + byte) < static_cast<u64>(words) * 4)
            {
                destination[src] = source[4 * dst + byte];
            }
            src++;
        }
    }
}

// The words of the 32 bit format's image unscattered
void ReadPsmct32(u8* destination, s32 dbp, s32 dbw, s32 rrw, s32 rrh, const u8* source, s32 words)
{
    s32 src = 0;
    s32 start = dbp * 64;
    for (s32 y = 0; y < rrh; y++)
    {
        for (s32 x = 0; x < rrw; x++)
        {
            s32 dst = start + Map32(x, y, dbw);
            if (4 * dst + 3 < words * 4)
            {
                std::memcpy(destination + src, source + 4 * dst, 4);
            }
            else
            {
                std::memset(destination + src, 0, 4);
            }
            src += 4;
        }
    }
}

// A colour of the file's channels doubled (the tools write them halved)
void ScaleUp(u8* rgba)
{
    rgba[0] = static_cast<u8>(rgba[0] << 1);
    rgba[1] = static_cast<u8>(rgba[1] << 1);
    rgba[2] = static_cast<u8>(rgba[2] << 1);
    rgba[3] = static_cast<u8>(rgba[3] << 1);
}
}

namespace GSTexture
{
u32 Width(const u8* header)
{
    return 1u << ReadU16(header + 4);
}

u32 Height(const u8* header)
{
    return 1u << ReadU16(header + 6);
}

bool Decode(const u8* header, const u8* packet, u32 packetSize, u8* rgba)
{
    if (header == nullptr || packet == nullptr || rgba == nullptr)
    {
        return false;
    }

    u8 format = header[9];
    u32 width = Width(header);
    u32 height = Height(header);
    if (width == 0 || height == 0 || width > 4096 || height > 4096)
    {
        return false;
    }

    // The DMA tag, then the header tag (the registers that follow say the image's size)
    if (packetSize < 32)
    {
        return false;
    }

    u64 headerTag = ReadU64(packet + 16);
    u32 headerLoops = static_cast<u32>(headerTag & 0x7FFF);
    u32 headerRegs = static_cast<u32>((headerTag >> 60) & 0xF);
    if (headerRegs == 0)
    {
        headerRegs = 16;
    }
    u32 headerBytes = headerLoops * headerRegs * 16;
    if (32u + headerBytes + 16u > packetSize)
    {
        return false;
    }
    // The registers' second value holds the image's width and height: each of the header's writes is a 16 byte pair, the first
    // pair being something else, so the second pair's first 64 bits are it (as TT-Lab's reader takes it)
    if (headerBytes < 32)
    {
        return false;
    }
    u64 sizeRegister = ReadU64(packet + 32 + 16);
    u32 rrw = static_cast<u32>(sizeRegister & 0xFFFFFFFF);
    u32 rrh = static_cast<u32>(sizeRegister >> 32);
    if (rrw == 0 || rrh == 0 || rrw > 4096 || rrh > 4096)
    {
        return false;
    }

    // The image tag, then its data
    const u8* imageTag = packet + 32 + headerBytes;
    u32 imageLoops = static_cast<u32>(ReadU64(imageTag) & 0x7FFF);
    const u8* gifData = imageTag + 16;
    u32 gifBytes = imageLoops * 16;
    if (32u + headerBytes + 16u + gifBytes > packetSize)
    {
        return false;
    }

    if (format == FormatPsmct32)
    {
        u32 pixels = width * height;
        u32 copy = gifBytes < pixels * 4 ? gifBytes : pixels * 4;
        std::memcpy(rgba, gifData, copy);
        std::memset(rgba + copy, 0, pixels * 4 - copy);
        for (u32 i = 0; i < pixels; i++)
        {
            ScaleUp(rgba + i * 4);
        }

        return true;
    }

    if (format != FormatPsmt8)
    {
        return false;
    }

    // The transferred image as 32 bit words, then its palettized bytes and palette from that
    s32 words = static_cast<s32>(rrw * rrh);
    u8* raw = static_cast<u8*>(std::malloc(static_cast<size_t>(words) * 4));
    if (raw == nullptr)
    {
        return false;
    }
    std::memset(raw, 0, static_cast<size_t>(words) * 4);
    WritePsmct32(raw, static_cast<s32>(words), gifData, gifBytes, static_cast<s32>(rrw), static_cast<s32>(rrh));

    u32 tbw = ReadU32(header + 0x2C);
    u32 cbp = ReadU32(header + 0x48);

    u8* indices = static_cast<u8*>(std::malloc(width * height));
    if (indices == nullptr)
    {
        std::free(raw);
        return false;
    }

    ReadPsmt8(indices, 0, static_cast<s32>(tbw), static_cast<s32>(width), static_cast<s32>(height), raw,
              static_cast<s32>(rrw * rrh));

    // The palette: its sixteen by sixteen colours the read gives red, green, blue and alpha in order, which the GS keeps in a
    // different order, each channel halved
    u8 colours[256 * 4];
    ReadPsmct32(colours, static_cast<s32>(cbp), 1, 16, 16, raw, static_cast<s32>(rrw * rrh));
    for (s32 i = 0; i < 8; i++)
    {
        for (s32 j = 8; j < 16; j++)
        {
            s32 a = j + i * 32;
            s32 b = a + 8;
            for (s32 c = 0; c < 4; c++)
            {
                u8 swap = colours[a * 4 + c];
                colours[a * 4 + c] = colours[b * 4 + c];
                colours[b * 4 + c] = swap;
            }
        }
    }
    for (s32 i = 0; i < 256; i++)
    {
        ScaleUp(colours + i * 4);
    }

    u32 pixels = width * height;
    for (u32 i = 0; i < pixels; i++)
    {
        std::memcpy(rgba + i * 4, colours + static_cast<u32>(indices[i]) * 4, 4);
    }

    std::free(indices);
    std::free(raw);
    return true;
}
}