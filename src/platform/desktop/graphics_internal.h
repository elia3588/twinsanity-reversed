#pragma once

#include "platform/graphics.h"
#include "game/resources.h"
#include "game/graphicstables.h"
#include "game/math.h"
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

// A submodel's packet decoded into host geometry (the VU1 programmes' vertexes): the strips it's cut into (each a triangle strip,
// a run of the arrays below) and the arrays every strip shares. The positions and normals are in the model's space; the texture
// coordinates and colours are what the programme unpacks into VU1's addresses 4 and 6
struct DecodedSubModel
{
    u32 vertexCount;
    u32 stripCount;
    u32* stripStarts;
    u32* stripCounts;
    Vector4* positions;
    Vector4* normals;
    Vector2* textureCoordinates;
    u32* colours;
};

// A skin submodel's packet decoded into host geometry (the VU1 skinning programme's vertexes): the strips it's cut into (each a
// triangle strip, a run of the arrays below), and the arrays every strip shares. The positions are the bind pose in 16 bit
// components packed with the batch's scale and offset (the W holds the X of the vertex's normal); the texture coordinates hold
// the UVs in X and Y and the normal's Y and Z in Z and W. Every vertex carries the joints and weights the programme blends it by
struct DecodedSkinSubModel
{
    u32 vertexCount;
    u32 stripCount;
    u32* stripStarts;
    u32* stripCounts;
    Vector4* positions;
    Vector4* textureCoordinates;
    u32* colours;
    s32* jointIndices;      // three a vertex
    f32* jointWeights;      // three a vertex
    u8* jointAmounts;       // one a vertex
};

// A model (a rigid model's submodels): how many, each one's vertex count, the size of its packet, the size of the data the file
// has after its packet (read and dropped alone) and the packets' bytes, kept as the file has them (the decode happens when the
// model is drawn, cached here)
struct Model
{
    ResourceHeader header;
    u32 subModelCount;
    u32* vertexCounts;
    u32* sizes;
    u32* extraSizes;
    u8** packets;
    DecodedSubModel** decoded;
};

// A block of a model's instances: the desktop draws each frame's placements itself, so it keeps none (needsEye and billboard are
// the shaders' and stay clear until the shaders are the desktop's own)
struct InstanceBlockOwner
{
    void* block;
    u8 needsEye;
    u8 billboard;
};

// A rigid model as an OGI or the scenery (a mesh) draws it: its submodels' materials, the model, the block of its instances, how
// many submodels (materials) it has and the model's ID
struct RigidModel
{
    ResourceHeader header;
    MaterialResource** materials;
    Model* model;
    InstanceBlockOwner instances;
    u32 count;
    u32 modelId;
};

// A skin as an OGI draws it: its submodels' materials, vertex counts and packets, how many, their packets' sizes, and the
// vertexes all of them have (the decode happens when the skin is drawn, cached here)
struct Skin
{
    ResourceHeader header;
    MaterialResource** materials;
    u32* vertexCounts;
    u8** packets;
    u32 count;
    u32* sizes;
    u32 vertexTotal;
    DecodedSkinSubModel** decoded;
};

// A part of a blend skin's submodel: each of its shapes' vertex data (a heap block each, its size in quadwords and how many
// vertexes VU1 unpacks), the packet of the part's vertexes, how many shapes, and the factors VU1 gets with each shape's weight
struct BlendPart
{
    u8** shapeAddresses;
    u32* shapeSizes;
    u32* shapeCounts;
    u8* packet;
    u8 shapeCount;
    f32 shapeFactors[3];
    u32 packetSize;
    u32 vertexCount;
    DecodedSkinSubModel* decoded;
};

struct BlendSubModel
{
    BlendPart** parts;
    u32 count;
};

// A blend skin as an OGI draws it: its submodels' materials, the submodels, how many, and how many shapes they have
struct BlendSkin
{
    ResourceHeader header;
    MaterialResource** materials;
    BlendSubModel** subModels;
    u32 count;
    u32 shapeCount;
};

// A level of detail of the scenery's: its meshes, how many, and the squares of its distances (the nearest and farthest it's
// drawn at, and where each mesh gives way to the next)
struct Lod
{
    static constexpr u32 MostSwitches = 3;

    ResourceHeader header;
    RigidModel** meshes;
    u8 count;
    u32 nearest;
    u32 farthest;
    u32 switchDistances[MostSwitches];
};

// A chunk's sky: its meshes, and the matrix of the half-size buffer's screen this frame
struct Sky
{
    ResourceHeader header;
    u32 count;
    RigidModel** models;
    Matrix4x4 toScreen;
};
}