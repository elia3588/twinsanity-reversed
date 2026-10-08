#pragma once

#include "common.h"

// The PlayStation 2's graphics synthesizer textures decoded for the window (gs_texture.cpp): the swizzled, palettized image of
// a texture's transfer packet turned into the plain RGBA bytes SDL takes. The algorithm is TT-Lab's (TwinTech/Libraries/
// EzSwizzle.cs and Common/Color.cs), which the retail textures were made with
namespace GSTexture
{
// The image's size in pixels, from the 0x60 byte GS header a texture's file starts with (its width and height's powers of two)
u32 Width(const u8* header);
u32 Height(const u8* header);

// The packet's image decoded into rgba (Width * Height * 4 bytes, red first, then green, blue and alpha), halved channels scaled
// back up. False when the header names a pixel format this doesn't know (the caller keeps whatever it made)
bool Decode(const u8* header, const u8* packet, u32 packetSize, u8* rgba);
}