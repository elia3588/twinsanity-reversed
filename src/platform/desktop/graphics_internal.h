#pragma once

#include "platform/graphics.h"
#include "game/resources.h"
#include "game/graphicstables.h"
#include "game/shaderanimation.h"

// What the desktop's Platform::Graphics holds of the game's graphics (graphics.cpp): the resources are the game's own objects,
// but their contents (a texture's decoded image, a material's shaders) are this platform's, the game only ever holding the
// pointers. The layouts are the platform's own; the game reads none of them (it goes through the platform functions)
namespace DesktopGraphics
{
struct GameTexture;
struct Shader;

// A texture of the game's as the desktop sees it: its GS header's sizes, the decoded image's size and the window's texture made
// from it (opaque, sdl_gfx.h)
struct Texture
{
    u16 widthPower;
    u16 heightPower;
    u32 width;
    u32 height;
    void* sdlTexture;
    bool hasAlpha;
};

// A material's shader: the settings the reading's the file's bytes, the fields the desktop's 2D drawing needs, and the texture
// the shader's ID names (the game's own, its decoded image)
struct Shader
{
    u8 settings[30];
    u8 fontPage;
    u8 colour[4];
    s16 lodK;
    s16 lodL;
    f32 leftover[4];
    f32 shaderColour[4];
    f32 scrollPhases[2];
    f32 scrollSpeeds[2];
    f32 scroll[2];
    u32 textureId;
    u32 type;
    ShaderAnimation* animation;
    GameTexture* texture;
};

// A material: its shaders, the render bucket it's drawn in and the keys the PS2's drawing uses (kept for the game's own
// constructions, which set them)
struct Material
{
    u32 shaderCount;
    u32 bucket;
    u64 activatedShaders;
    u32 call;
    u8 listed;
    u8 unused65;
    u8 drawnDirectly;
    Shader* shaders[4];
};

// A game texture resource: its header, its file's size and the desktop's texture
struct GameTexture
{
    ResourceHeader header;
    u32 size;
    Texture texture;
};

// A material resource: its header and the material it holds
struct MaterialResource
{
    ResourceHeader header;
    Material* material;
    u32 unused0C;
};
}