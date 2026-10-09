#pragma once
#include "common.h"

// The deskop's 2D drawing through SDL2 (sdl_gfx.cpp, built for the host's own layouts like the window), kept behind plain
// functions so the game's graphics (graphics.cpp, built as the game's own layouts are) uses no SDL2 headers
namespace SdlGfx
{
bool Available();
void GetSize(s32* width, s32* height);
void Clear(u8 red, u8 green, u8 blue, bool clearDepth = true);
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
// A 3D draw's material: the shader's own GS setting bytes (its blending and alpha preset, or its own alpha formula, its alpha
// test and its depth test). The game's 3D drawers set it from each submodel's material before its strips are drawn; every
// DrawTriangleStrip3D applies the last one it was given
struct Material3D
{
    u8 blends;           // the GS's ABE: whether the source blends with the frame at all
    u8 preset;           // the alpha preset (the GS ALPHA formula) when it blends
    u8 ownAlpha;         // the shader's own alpha formula rather than the preset
    u8 alphaA;
    u8 alphaB;
    u8 alphaC;
    u8 alphaD;
    u8 alphaFix;
    u8 alphaTest;        // the GS's ATE
    u8 alphaMethod;      // the GS's ATST
    u8 alphaReference;   // the GS's AREF
    u8 alphaFail;        // the GS's AFAIL (the desktop drops the fragment whatever it is)
    u8 depthTest;        // the GS's ZTST (0 never, 1 always, 2 gequal, 3 greater)
    u8 noDepthWrites;    // the GS's ZMSK: the depth is kept even when the draw would write it
};
// The material the following DrawTriangleStrip3D calls draw with. The game's own draw calls it per submodel; without a call the
// plain state (opaque, no alpha test, the usual nearest depth test) holds
void SetMaterial3D(const Material3D& material);
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