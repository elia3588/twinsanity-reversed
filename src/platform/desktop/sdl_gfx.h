#pragma once
#include "common.h"

// The deskop's 2D drawing through SDL2 (sdl_gfx.cpp, built for the host's own layouts like the window), kept behind plain
// functions so the game's graphics (graphics.cpp, built as the game's own layouts are) uses no SDL2 headers
namespace SdlGfx
{
bool Available();
void GetSize(s32* width, s32* height);
void Clear(u8 red, u8 green, u8 blue);
// A texture the decoded image's RGBA bytes make (one pixel's red, green, blue and alpha in that order), the handle nullptr when
// there's no window or the texture couldn't be made
void* CreateTexture(const void* rgba, s32 width, s32 height);
void DestroyTexture(void* texture);
// The rectangle filled with the colour (SDL's alpha blends it)
void FillRect(s32 x, s32 y, s32 width, s32 height, u8 red, u8 green, u8 blue, u8 alpha);
// One vertex of a filled shape: where it is in the window's pixels, and the colour it's made of
struct Vertex
{
    f32 x;
    f32 y;
    u8 red;
    u8 green;
    u8 blue;
    u8 alpha;
    f32 u;
    f32 v;
};
// The vertexes' connected triangles (each the next three of them), coloured by the vertexes, the texture's pixels over those
// colours when it isn't nullptr
void DrawTriangleStrip(void* texture, const Vertex* vertices, u32 count);
// One vertex of the 3D scene: where it is in the window's pixels and its depth (0 nearest, 1 farthest, tested against what's
// already drawn), with the same colour and texture coordinates as Vertex
struct Vertex3D
{
    f32 x;
    f32 y;
    f32 z;
    u8 red;
    u8 green;
    u8 blue;
    u8 alpha;
    f32 u;
    f32 v;
};
// The same strip as DrawTriangleStrip, over triangles that depth-test against each other and against the frame's depth.
// The depth isn't written when writeDepth is clear: a background (a sky) tests against the frame's depth without ever hiding
// what's drawn after it. When noDraw isn't nullptr it holds one flag a vertex: the PS2's GS draws no triangle for a vertex whose
// flag is set, so the triangle ending at it (its previous two vertexes and it) is left out (the game's packed strips, quad
// lists widened into strips, mark the connecting vertexes so)
void DrawTriangleStrip3D(void* texture, const Vertex3D* vertices, u32 count, bool writeDepth = true,
                         const u8* noDraw = nullptr);
// The texture drawn in the window: the destination's and the source's rectangles in pixels, and the tint its pixels are
// modulated by
void DrawTexture(void* texture, s32 dstX, s32 dstY, s32 dstWidth, s32 dstHeight, s32 srcX, s32 srcY, s32 srcWidth, s32 srcHeight,
                 u8 red, u8 green, u8 blue, u8 alpha);
// The same, the source's rows taken the other way up (a font page stores its glyphs flipped)
void DrawTextureFlipped(void* texture, s32 dstX, s32 dstY, s32 dstWidth, s32 dstHeight, s32 srcX, s32 srcY, s32 srcWidth,
                        s32 srcHeight, u8 red, u8 green, u8 blue, u8 alpha);
}