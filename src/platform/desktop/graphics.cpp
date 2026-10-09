#include "platform/graphics.h"

#include "clock.h"
#include "window.h"
#include "graphics_internal.h"
#include "frustum.h"
#include "gs_texture.h"
#include "sdl_gfx.h"

#include "game/context.h"
#include "game/controllers.h"
#include "game/font.h"
#include "game/particles.h"
#include "game/place.h"
#include "game/renderer.h"
#include "game/resources.h"
#include "game/view.h"
#include "game/graphicstables.h"
#include "game/memory.h"
#include "game/scenery.h"
#include "game/shaderanimation.h"
#include "game/stream.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

// The desktop's side of Platform::Graphics: the window (window.cpp) with the frame's 2D drawn in it through SDL (sdl_gfx.cpp),
// the game's own textures decoded from the disc (gs_texture.cpp). The 3D scene isn't drawn yet
namespace
{
u8 g_ClearColor[3] = {};
// The screen the game draws in, its pixel size (the shapes' places are pixels of it)
s32 g_ScreenWidthPx = 512;
s32 g_ScreenHeightPx = 512;
// The frame's corner in the GS's own pixels (the screen's middle at 2048), which SetUpRenderTarget works out; the 3D draw takes
// the projection's off it to reach the game's screen
s32 g_TargetX = 0;
s32 g_TargetY = 0;

// The frame's memory (AllocFrameMemory): a bump arena of chunks that live until the frame is drawn, then are let go (the PS2's
// own is the frame buckets' DMA chain). A chunk never moves once made, so the pointers handed out stay valid
constexpr size_t FrameMemoryChunk = 1u << 20;

std::vector<std::unique_ptr<u8[]>> g_FrameMemory;
size_t g_FrameMemoryChunk = 0;
size_t g_FrameMemoryUsed = 0;

void ResetFrameMemory()
{
    g_FrameMemory.clear();
    g_FrameMemoryChunk = 0;
    g_FrameMemoryUsed = 0;
}

u8* AllocFrameMemoryArena(size_t bytes)
{
    // The DMA tags the PS2's allocation must start a quadword into: keep the desktop's quadword aligned too
    bytes = (bytes + 0xF) & ~static_cast<size_t>(0xF);
    if (bytes == 0)
    {
        bytes = 0x10;
    }

    if (g_FrameMemory.empty() || g_FrameMemoryUsed + bytes > FrameMemoryChunk)
    {
        size_t chunk = bytes > FrameMemoryChunk ? bytes : FrameMemoryChunk;
        g_FrameMemory.push_back(std::unique_ptr<u8[]>(new u8[chunk]));
        g_FrameMemoryChunk = g_FrameMemory.size() - 1;
        g_FrameMemoryUsed = 0;
    }

    u8* start = g_FrameMemory[g_FrameMemoryChunk].get() + g_FrameMemoryUsed;
    g_FrameMemoryUsed += bytes;
    return start;
}

// The graphics' trace of what it's given, while $TWINSANITY_GFX_LOG is set (to stderr, which the run's own does)
void Log(const char* format, ...)
{
    static bool enabled = std::getenv("TWINSANITY_GFX_LOG") != nullptr;
    if (!enabled)
    {
        return;
    }

    std::va_list arguments;
    va_start(arguments, format);
    std::vfprintf(stderr, format, arguments);
    va_end(arguments);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

// A game colour's byte made the window's: the game's red, green and blue are full at 192, its alpha at 128, SDL's channels at 255
u8 ScaleChannel(u8 channel, u32 full)
{
    u32 scaled = static_cast<u32>(channel) * 255 / full;
    return static_cast<u8>(scaled > 255 ? 255 : scaled);
}

// The desktop's texture a material's first shader draws, none when it's untextured
DesktopGraphics::Texture* MaterialTexture(const DesktopGraphics::Material* material)
{
    if (material == nullptr || material->shaderCount == 0 || material->shaders[0] == nullptr ||
        material->shaders[0]->texture == nullptr)
    {
        return nullptr;
    }

    return &material->shaders[0]->texture->texture;
}

// The material's first shader's settings put to the 3D drawer's own state (SdlGfx::Material3D): its blending and its preset,
// its alpha test and its depth test and write, so an opaque shader writes its colour as it is, a blended one mixes it and an
// alpha-tested one drops its samples (the PS2's GS reads the same settings)
void SetMaterialState(const DesktopGraphics::Material* material)
{
    SdlGfx::Material3D state = {};
    state.depthTest = 2;   // gequal, the usual opaque test (the GS's own number, the host's lequal)
    if (material != nullptr && material->shaderCount != 0 && material->shaders[0] != nullptr)
    {
        const u8* settings = material->shaders[0]->settings;
        state.blends = settings[0];
        state.preset = settings[1];
        state.alphaTest = settings[2];
        state.alphaMethod = settings[3];
        state.alphaReference = settings[4];
        state.alphaFail = settings[5];
        state.depthTest = settings[8];
        state.ownAlpha = settings[17];
        state.alphaA = settings[18];
        state.alphaB = settings[19];
        state.alphaC = settings[20];
        state.alphaD = settings[21];
        state.alphaFix = settings[22];
        state.noDepthWrites = settings[27];
    }

    SdlGfx::SetMaterial3D(state);
}

// The UI's flat material (untextured, white) and its one shader, made once
DesktopGraphics::Material g_FlatMaterial;
DesktopGraphics::Shader g_FlatShader;
bool g_FlatMaterialMade = false;

void ConstructDesktopMaterial(DesktopGraphics::Material* material)
{
    material->shaderCount = 0;
    material->bucket = 0;
    material->activatedShaders = 0;
    material->call = 0;
    material->listed = 0;
    material->unused65 = 0;
    material->drawnDirectly = 0;
    for (u32 shader = 0; shader < 4; shader++)
    {
        material->shaders[shader] = nullptr;
    }
}

void DestroyDesktopShader(DesktopGraphics::Shader* shader)
{
    if (shader == nullptr)
    {
        return;
    }

    if (shader->animation != nullptr)
    {
        MemoryDeallocate2_(shader->animation);
    }

    MemoryDeallocate2_(shader);
}

// An array of count items of the game's heap, or none for no items
template <typename T>
T* AllocateItems(u32 count)
{
    return count != 0 ? static_cast<T*>(MemoryAllocate(count * sizeof(T))) : nullptr;
}

// The decode's own data (DesktopGraphics::DecodedSubModel and its arrays): a host allocation, not the game's heap (the game's
// arena is the PS2's size and the decode is the desktop's alone, which would otherwise crowd the level's own allocations out)
template <typename T>
T* AllocateDecoded(u32 count)
{
    return count != 0 ? static_cast<T*>(std::malloc(static_cast<size_t>(count) * sizeof(T))) : nullptr;
}

// A packet of a model's or skin's submodel kept as its file's bytes, in a block of its own: the decode reads it when the model
// is drawn. None for an empty packet (the stream reads nothing)
u8* ReadPacketBytes(Stream* stream, u32 size)
{
    if (size == 0)
    {
        return nullptr;
    }

    auto* packet = static_cast<u8*>(MemoryAllocate(size));
    if (packet != nullptr)
    {
        stream->Read(packet, size, 1);
    }

    return packet;
}

// The PS2 VU1 model packets (the DMA chains the PS2 renderer's WriteSubModels makes) decoded into host geometry. A packet is the
// submodel's size word and its zero, then per strip: a GIF tag and its registers (VU1 addresses 0 and 1, kept for VU1 alone, not
// read here), the strip's texture coordinates, normals, positions and colours (VU1 addresses 4, 5, 3 and 6, the order materials.cpp
// writes them in), closed by an MSCAL starting the programme that draws the strip. A strip's vertexes are a triangle strip (its
// GIF tag's PRIM), so the decode hands the draws runs of the strip.
f32 AsFloat(u32 bits)
{
    f32 value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// The components one UNPACK kind's element takes (V4Bytes, one packed word, is a single component)
u32 UnpackComponents(u32 command)
{
    if (command == 0x6E)
    {
        return 1;
    }

    return ((command >> 2) & 3) + 1;
}

u32 UnpackElementSize(u32 command, u32 unpackComponents)
{
    // V4Bytes unpacks one packed word an element; the S, V2, V3 and V4 kinds one word a component, or (their low bit set)
    // one halfword a component (the skins' own positions and coordinates)
    if (command == 0x6E)
    {
        return 1;
    }

    return (command & 1) != 0 ? (unpackComponents * 2 + 3) / 4 : unpackComponents;
}

void FreeDecodedSubModel(DesktopGraphics::DecodedSubModel* decoded)
{
    if (decoded == nullptr)
    {
        return;
    }

    if (decoded->stripStarts != nullptr)
    {
        std::free(decoded->stripStarts);
    }
    if (decoded->stripCounts != nullptr)
    {
        std::free(decoded->stripCounts);
    }
    if (decoded->positions != nullptr)
    {
        std::free(decoded->positions);
    }
    if (decoded->normals != nullptr)
    {
        std::free(decoded->normals);
    }
    if (decoded->textureCoordinates != nullptr)
    {
        std::free(decoded->textureCoordinates);
    }
    if (decoded->colours != nullptr)
    {
        std::free(decoded->colours);
    }

    std::free(decoded);
}

// One submodel's packet decoded, null when it isn't the shape expected (the submodel isn't drawn)
DesktopGraphics::DecodedSubModel* DecodeSubModel(const u8* packet, u32 size, u32 vertexCount)
{
    if (packet == nullptr || size < 16 || vertexCount == 0)
    {
        return nullptr;
    }

    auto* decoded = static_cast<DesktopGraphics::DecodedSubModel*>(std::malloc(sizeof(DesktopGraphics::DecodedSubModel)));
    if (decoded == nullptr)
    {
        return nullptr;
    }

    std::memset(decoded, 0, sizeof(*decoded));
    decoded->positions = AllocateDecoded<Vector4>(vertexCount);
    decoded->normals = AllocateDecoded<Vector4>(vertexCount);
    decoded->textureCoordinates = AllocateDecoded<Vector2>(vertexCount);
    decoded->colours = AllocateDecoded<u32>(vertexCount);
    decoded->stripStarts = AllocateDecoded<u32>(vertexCount);
    decoded->stripCounts = AllocateDecoded<u32>(vertexCount);
    if (decoded->positions == nullptr || decoded->normals == nullptr || decoded->textureCoordinates == nullptr ||
        decoded->colours == nullptr || decoded->stripStarts == nullptr || decoded->stripCounts == nullptr)
    {
        FreeDecodedSubModel(decoded);
        return nullptr;
    }

    // A submodel without a colour UNPACK (the level's meshes: the material's own colour tints them) keeps white, so the
    // host's allocation's leftovers never tint or hide it
    for (u32 index = 0; index < vertexCount; index++)
    {
        decoded->colours[index] = 0xFFFFFFFF;
    }

    const u32* words = reinterpret_cast<const u32*>(packet);
    const u32 wordCount = size / 4;
    u32 word = 2;      // the size word and its zero
    u32 vertex = 0;    // where the next unpacked vertex goes
    u32 strip = 0;
    u32 stripStart = 0;
    bool ok = true;

    while (word < wordCount && ok)
    {
        const u32 command = (words[word] >> 24) & 0xFF;
        if (command >= 0x60 && command <= 0x6F)
        {
            const u32 components = UnpackComponents(command);
            const u32 elementSize = UnpackElementSize(command, components);
            const u32 count = (words[word] >> 16) & 0xFF;
            const u32 address = words[word] & 0x3FFF;
            if (word + 1 + elementSize * count > wordCount)
            {
                ok = false;     // the packet ends before this unpack's data does
                break;
            }
            const u32* data = &words[word + 1];
            if (address == 3 || address == 4 || address == 5 || address == 6)
            {
                // Every attribute of a strip is indexed from the strip's first vertex (the positions' cursor has already
                // advanced past the strip by the time the texture, normal and colour unpacks arrive)
                if (stripStart + count > vertexCount)
                {
                    ok = false;
                    break;
                }

                for (u32 index = 0; index < count; index++)
                {
                    const u32 base = index * elementSize;
                    const u32 at = stripStart + index;
                    if (address == 3)
                    {
                        decoded->positions[at] = Vector4{AsFloat(data[base]), AsFloat(data[base + 1]), AsFloat(data[base + 2]), 1.0f};
                    }
                    else if (address == 4)
                    {
                        decoded->textureCoordinates[at] = Vector2{AsFloat(data[base]), AsFloat(data[base + 1])};
                    }
                    else if (address == 5)
                    {
                        decoded->normals[at] = Vector4{AsFloat(data[base]), AsFloat(data[base + 1]), AsFloat(data[base + 2]), 0.0f};
                    }
                    else
                    {
                        decoded->colours[at] = data[base];
                    }
                }

                if (address == 3)
                {
                    vertex = stripStart + count;
                }
            }

            word += 1 + elementSize * count;
            continue;
        }

        if (command == 0x14)    // MSCAL: the strip's end
        {
            if (vertex > stripStart)
            {
                decoded->stripStarts[strip] = stripStart;
                decoded->stripCounts[strip] = vertex - stripStart;
                strip++;
            }
            stripStart = vertex;
            word += 2;          // the MSCAL and the programme's address
            continue;
        }

        if (command == 0x00 || command == 0x01 || command == 0x07)   // NOP, STCYCL, MARK
        {
            word += 1;
            continue;
        }

        ok = false;             // an unexpected code: stop, the packet isn't as expected
    }

    if (!ok || vertex == 0)
    {
        FreeDecodedSubModel(decoded);
        return nullptr;
    }

    decoded->vertexCount = vertex;
    decoded->stripCount = strip;
    return decoded;
}

// The submodel's decode, made once and kept on the model (graphics_internal.h)
DesktopGraphics::DecodedSubModel* DecodedSubModelOf(DesktopGraphics::Model* model, u32 subModel)
{
    if (model->decoded == nullptr)
    {
        model->decoded = AllocateDecoded<DesktopGraphics::DecodedSubModel*>(model->subModelCount);
        if (model->decoded == nullptr)
        {
            return nullptr;
        }

        for (u32 index = 0; index < model->subModelCount; index++)
        {
            model->decoded[index] = nullptr;
        }
    }

    if (model->decoded[subModel] == nullptr && model->packets != nullptr && model->sizes != nullptr)
    {
        model->decoded[subModel] = DecodeSubModel(model->packets[subModel], model->sizes[subModel], model->vertexCounts[subModel]);
    }

    return model->decoded[subModel];
}

// A 16 bit component of a skin's packed position or coordinate, sign extended the way VIF unpacks it
s32 SignExtend16(u32 value)
{
    return static_cast<s32>(static_cast<s16>(value & 0xFFFF));
}

void FreeDecodedSkinSubModel(DesktopGraphics::DecodedSkinSubModel* decoded)
{
    if (decoded == nullptr)
    {
        return;
    }

    std::free(decoded->stripStarts);
    std::free(decoded->stripCounts);
    std::free(decoded->positions);
    std::free(decoded->textureCoordinates);
    std::free(decoded->colours);
    std::free(decoded->jointIndices);
    std::free(decoded->jointWeights);
    std::free(decoded->jointAmounts);
    std::free(decoded->jointNoDraw);
    std::free(decoded);
}

// One skin submodel's packet decoded (the VU1 skinning programme's vertexes): the VIF batches the game's tools packed, each strip's
// positions and coordinates 16 bit, scaled by the batch's own scale (VU1 address 2) and offset by its row registers (offset mode's
// STROW), the joints and weights the skinning programme blends the vertexes by (VU1 address 5), and the colours (address 6). The
// positions hold the normal's X in their W, the coordinates the normal's Y and Z in their Z and W. Null when the packet isn't the
// shape expected (the submodel isn't drawn)
DesktopGraphics::DecodedSkinSubModel* DecodeSkinSubModel(const u8* packet, u32 size, u32 vertexCount)
{
    if (packet == nullptr || size < 16 || vertexCount == 0)
    {
        return nullptr;
    }

    auto* decoded = static_cast<DesktopGraphics::DecodedSkinSubModel*>(std::malloc(sizeof(DesktopGraphics::DecodedSkinSubModel)));
    if (decoded == nullptr)
    {
        return nullptr;
    }

    std::memset(decoded, 0, sizeof(*decoded));
    decoded->positions = AllocateDecoded<Vector4>(vertexCount);
    decoded->textureCoordinates = AllocateDecoded<Vector4>(vertexCount);
    decoded->colours = AllocateDecoded<u32>(vertexCount);
    decoded->jointIndices = AllocateDecoded<s32>(vertexCount * 3);
    decoded->jointWeights = AllocateDecoded<f32>(vertexCount * 3);
    decoded->jointAmounts = AllocateDecoded<u8>(vertexCount);
    decoded->jointNoDraw = AllocateDecoded<u8>(vertexCount);
    decoded->stripStarts = AllocateDecoded<u32>(vertexCount);
    decoded->stripCounts = AllocateDecoded<u32>(vertexCount);
    if (decoded->positions == nullptr || decoded->textureCoordinates == nullptr || decoded->colours == nullptr ||
        decoded->jointIndices == nullptr || decoded->jointWeights == nullptr || decoded->jointAmounts == nullptr ||
        decoded->jointNoDraw == nullptr || decoded->stripStarts == nullptr || decoded->stripCounts == nullptr)
    {
        FreeDecodedSkinSubModel(decoded);
        return nullptr;
    }

    // A submodel without a colour UNPACK keeps white, and a vertex without a joint keeps the first (the skins the tools packed all
    // carry the four addresses, so these are only the safety of an unexpected packet)
    for (u32 index = 0; index < vertexCount; index++)
    {
        decoded->colours[index] = 0xFFFFFFFF;
        decoded->textureCoordinates[index] = Vector4{0.0f, 0.0f, 0.0f, 1.0f};
        decoded->jointIndices[index * 3 + 0] = 0;
        decoded->jointIndices[index * 3 + 1] = 0;
        decoded->jointIndices[index * 3 + 2] = 0;
        decoded->jointWeights[index * 3 + 0] = 1.0f;
        decoded->jointWeights[index * 3 + 1] = 0.0f;
        decoded->jointWeights[index * 3 + 2] = 0.0f;
        decoded->jointAmounts[index] = 1;
        decoded->jointNoDraw[index] = 0;
    }

    const u32* words = reinterpret_cast<const u32*>(packet);
    const u32 wordCount = size / 4;
    u32 word = 2;       // the DMA tag (the size word and its zero), the codes follow
    u32 vertex = 0;
    u32 strip = 0;
    u32 stripStart = 0;
    u32 row[4] = {0, 0, 0, 0};
    f32 positionScale = 1.0f / 16384.0f;
    f32 uvScale = 1.0f / 2048.0f;
    bool offsetMode = false;
    bool started = false;
    bool ok = true;

    while (word < wordCount && ok)
    {
        const u32 code = words[word];
        const u32 command = (code >> 24) & 0xFF;
        if (command >= 0x60 && command <= 0x6F)
        {
            const u32 components = UnpackComponents(command);
            const u32 elementSize = UnpackElementSize(command, components);
            const u32 count = (code >> 16) & 0xFF;
            const u32 address = code & 0x3FFF;
            if (word + 1 + elementSize * count > wordCount)
            {
                ok = false;
                break;
            }

            const u32* data = &words[word + 1];
            if (address == 0)
            {
                // An unpack to address 0 (the GIF tag) starts the next strip: the one before it is done
                if (started && vertex > stripStart)
                {
                    decoded->stripStarts[strip] = stripStart;
                    decoded->stripCounts[strip] = vertex - stripStart;
                    strip++;
                }
                stripStart = vertex;
                started = true;
            }

            if (stripStart + count > vertexCount)
            {
                ok = false;
                break;
            }

            for (u32 index = 0; index < count; index++)
            {
                const u32 base = index * elementSize;
                const u32 at = stripStart + index;
                if (address == 2)
                {
                    positionScale = AsFloat(data[0]);
                    uvScale = AsFloat(data[1]);
                }
                else if (address == 3)
                {
                    s32 x = SignExtend16(data[base]);
                    s32 y = SignExtend16(data[base] >> 16);
                    s32 z = SignExtend16(data[base + 1]);
                    s32 normalX = SignExtend16(data[base + 1] >> 16);
                    if (offsetMode)
                    {
                        x += static_cast<s32>(row[0]);
                        y += static_cast<s32>(row[1]);
                        z += static_cast<s32>(row[2]);
                        normalX += static_cast<s32>(row[3]);
                    }
                    decoded->positions[at] = Vector4{x * positionScale, y * positionScale, z * positionScale, normalX * positionScale};
                }
                else if (address == 4)
                {
                    s32 u = SignExtend16(data[base]);
                    s32 v = SignExtend16(data[base] >> 16);
                    s32 normalY = SignExtend16(data[base + 1]);
                    s32 normalZ = SignExtend16(data[base + 1] >> 16);
                    if (offsetMode)
                    {
                        u += static_cast<s32>(row[0]);
                        v += static_cast<s32>(row[1]);
                        normalY += static_cast<s32>(row[2]);
                        normalZ += static_cast<s32>(row[3]);
                    }
                    decoded->textureCoordinates[at] = Vector4{u * uvScale, v * uvScale, normalY * uvScale, normalZ * uvScale};
                }
                else if (address == 5)
                {
                    // The weights are floats with the joint index times 4 in their lowest 9 bits, the fourth word holds how many
                    const u32 joint0 = data[base + 0];
                    const u32 joint1 = data[base + 1];
                    const u32 joint2 = data[base + 2];
                    u32 amount = data[base + 3] & 0xFF;
                    if (amount == 0)
                    {
                        amount = 1;
                    }
                    else if (amount > 3)
                    {
                        amount = 3;
                    }

                    decoded->jointIndices[at * 3 + 0] = static_cast<s32>((joint0 & 0x1FF) / 4);
                    decoded->jointIndices[at * 3 + 1] = static_cast<s32>((joint1 & 0x1FF) / 4);
                    decoded->jointIndices[at * 3 + 2] = static_cast<s32>((joint2 & 0x1FF) / 4);
                    decoded->jointWeights[at * 3 + 0] = AsFloat(joint0 & ~0x1FFu);
                    decoded->jointWeights[at * 3 + 1] = AsFloat(joint1 & ~0x1FFu);
                    decoded->jointWeights[at * 3 + 2] = AsFloat(joint2 & ~0x1FFu);
                    decoded->jointAmounts[at] = static_cast<u8>(amount);
                    decoded->jointNoDraw[at] = (data[base + 3] & 0x8000) != 0 ? 1 : 0;
                }
                else if (address == 6)
                {
                    decoded->colours[at] = data[base];
                }
            }

            if (address == 3)
            {
                vertex = stripStart + count;
            }

            word += 1 + elementSize * count;
            continue;
        }

        switch (command)
        {
        case 0x00:  // NOP
        case 0x01:  // STCYCL
        case 0x04:  // ITOP
        case 0x07:  // MARK
        case 0x10:  // FLUSHA
        case 0x11:  // FLUSH
        case 0x13:  // FLUSHE
        case 0x14:  // MSCAL
        case 0x15:  // MSCALF
        case 0x17:  // MSCNT
            word += 1;
            break;
        case 0x05:  // STMOD: the row registers get added to the packed components in offset mode
            offsetMode = (code & 3) == 1;
            word += 1;
            break;
        case 0x30:  // STROW: the four row registers
            if (word + 5 > wordCount)
            {
                ok = false;
                break;
            }
            row[0] = words[word + 1];
            row[1] = words[word + 2];
            row[2] = words[word + 3];
            row[3] = words[word + 4];
            word += 5;
            break;
        case 0x31:  // STCOL
            word += 5;
            break;
        case 0x20:  // STMASK
            word += 2;
            break;
        default:
            ok = false;
            break;
        }
    }

    if (ok && vertex > stripStart)
    {
        decoded->stripStarts[strip] = stripStart;
        decoded->stripCounts[strip] = vertex - stripStart;
        strip++;
    }

    if (!ok || vertex == 0)
    {
        FreeDecodedSkinSubModel(decoded);
        return nullptr;
    }

    decoded->vertexCount = vertex;
    decoded->stripCount = strip;
    return decoded;
}

// The skin submodel's decode, made once and kept on the skin (graphics_internal.h)
DesktopGraphics::DecodedSkinSubModel* DecodedSkinSubModelOf(DesktopGraphics::Skin* skin, u32 subModel)
{
    if (skin->decoded == nullptr)
    {
        skin->decoded = AllocateDecoded<DesktopGraphics::DecodedSkinSubModel*>(skin->count);
        if (skin->decoded == nullptr)
        {
            return nullptr;
        }

        for (u32 index = 0; index < skin->count; index++)
        {
            skin->decoded[index] = nullptr;
        }
    }

    if (subModel >= skin->count)
    {
        return nullptr;
    }

    if (skin->decoded[subModel] == nullptr && skin->packets != nullptr && skin->sizes != nullptr && skin->vertexCounts != nullptr)
    {
        skin->decoded[subModel] = DecodeSkinSubModel(skin->packets[subModel], skin->sizes[subModel], skin->vertexCounts[subModel]);
    }

    return skin->decoded[subModel];
}

// A blend part's decode (its packet the same skin packet a skin submodel's is), made once and kept on the part (graphics_internal.h)
DesktopGraphics::DecodedSkinSubModel* DecodedBlendPartOf(DesktopGraphics::BlendPart* part)
{
    if (part->decoded == nullptr && part->packet != nullptr && part->vertexCount != 0)
    {
        part->decoded = DecodeSkinSubModel(part->packet, part->packetSize, part->vertexCount);
    }

    return part->decoded;
}

// A material's shaders' animations started again (a model's materials are the game's own, graphics_internal.h's)
void RestartMaterialAnimations(DesktopGraphics::MaterialResource* resource)
{
    if (resource == nullptr || resource->material == nullptr)
    {
        return;
    }

    DesktopGraphics::Material* material = resource->material;
    for (u32 index = 0; index < material->shaderCount; index++)
    {
        DesktopGraphics::Shader* shader = material->shaders[index];
        if (shader != nullptr && shader->animation != nullptr)
        {
            RestartShaderAnimation(shader->animation);
        }
    }
}

// A rigid model's or mesh's data after its version: the materials it names, the model it's of and (the desktop drawing each
// frame's placements itself) no block of instances
void ReadRigidModelData(DesktopGraphics::RigidModel* model, Stream* stream)
{
    s32 count = 0;
    stream->ReadS32(&count);
    model->count = static_cast<u32>(count);
    model->materials = AllocateItems<DesktopGraphics::MaterialResource*>(model->count);
    for (u32 index = 0; index < model->count; index++)
    {
        u32 id = 0;
        stream->ReadS32(reinterpret_cast<s32*>(&id));
        model->materials[index] = reinterpret_cast<DesktopGraphics::MaterialResource*>(g_MaterialTable.Acquire(&id, nullptr));
    }

    s32 modelId = 0;
    stream->ReadS32(&modelId);
    model->modelId = static_cast<u32>(modelId);
    model->model = reinterpret_cast<DesktopGraphics::Model*>(g_ModelTable.Acquire(&model->modelId, nullptr));
    model->instances.block = nullptr;
    model->instances.needsEye = 0;
    model->instances.billboard = 0;
}

// A rigid model or mesh let go of: its materials' and its model's references, and its own memory
void DestroyRigidModel(DesktopGraphics::RigidModel* model)
{
    if (model == nullptr)
    {
        return;
    }

    for (u32 index = 0; index < model->count; index++)
    {
        if (model->materials != nullptr && model->materials[index] != nullptr)
        {
            g_MaterialTable.ReleaseItem(reinterpret_cast<MaterialResource*>(model->materials[index]));
        }
    }

    if (model->model != nullptr)
    {
        g_ModelTable.ReleaseItem(reinterpret_cast<RigidModelData*>(model->model));
    }

    if (model->materials != nullptr)
    {
        MemoryDeallocate2_(model->materials);
    }

    MemoryDeallocate2_(model);
}

// The meshes a LOD or sky names, taken from the mesh table
void ReadMeshIds(DesktopGraphics::RigidModel** meshes, u32 count, Stream* stream)
{
    for (u32 index = 0; index < count; index++)
    {
        u32 id = 0;
        stream->ReadS32(reinterpret_cast<s32*>(&id));
        meshes[index] = reinterpret_cast<DesktopGraphics::RigidModel*>(g_MeshTable.Acquire(&id, nullptr));
    }
}

// A blend part: its packet of vertexes, the factors VU1 gets with each shape's weight, and its shapes' vertex data (their sizes
// in quadwords the files have)
void ReadBlendPart(DesktopGraphics::BlendPart* part, Stream* stream, u32 shapeCount)
{
    std::memset(part, 0, sizeof(DesktopGraphics::BlendPart));

    s32 packetSize = 0;
    s32 vertexCount = 0;
    stream->ReadS32(&packetSize);
    stream->ReadS32(&vertexCount);
    part->packetSize = static_cast<u32>(packetSize);
    part->vertexCount = static_cast<u32>(vertexCount);
    part->packet = ReadPacketBytes(stream, part->packetSize);

    stream->Read(part->shapeFactors, sizeof(part->shapeFactors), 1);
    part->shapeCount = static_cast<u8>(shapeCount);
    part->shapeAddresses = AllocateItems<u8*>(part->shapeCount);
    part->shapeSizes = AllocateItems<u32>(part->shapeCount);
    part->shapeCounts = AllocateItems<u32>(part->shapeCount);
    for (u32 shape = 0; shape < part->shapeCount; shape++)
    {
        s32 size = 0;
        s32 count = 0;
        stream->ReadS32(&size);
        stream->ReadS32(&count);
        part->shapeSizes[shape] = static_cast<u32>(size);
        part->shapeCounts[shape] = static_cast<u32>(count);
        part->shapeAddresses[shape] = ReadPacketBytes(stream, part->shapeSizes[shape] << 4);
    }
}

// A blend submodel's parts, made and read
void ReadBlendParts(DesktopGraphics::BlendSubModel* subModel, Stream* stream, u32 shapeCount)
{
    subModel->parts = AllocateItems<DesktopGraphics::BlendPart*>(subModel->count);
    for (u32 index = 0; index < subModel->count; index++)
    {
        auto* part = static_cast<DesktopGraphics::BlendPart*>(MemoryAllocate(sizeof(DesktopGraphics::BlendPart)));
        subModel->parts[index] = part;
        if (part != nullptr)
        {
            ReadBlendPart(part, stream, shapeCount);
        }
    }
}

// A blend submodel destroyed: its parts' packets, shapes and memory
void DestroyBlendSubModel(DesktopGraphics::BlendSubModel* subModel)
{
    if (subModel == nullptr)
    {
        return;
    }

    for (u32 index = 0; index < subModel->count; index++)
    {
        DesktopGraphics::BlendPart* part = subModel->parts != nullptr ? subModel->parts[index] : nullptr;
        if (part == nullptr)
        {
            continue;
        }

        if (part->packet != nullptr)
        {
            MemoryDeallocate2_(part->packet);
        }

        for (u32 shape = 0; shape < part->shapeCount; shape++)
        {
            if (part->shapeAddresses != nullptr && part->shapeAddresses[shape] != nullptr)
            {
                MemoryDeallocate2_(part->shapeAddresses[shape]);
            }
        }

        if (part->shapeAddresses != nullptr)
        {
            MemoryDeallocate2_(part->shapeAddresses);
        }
        if (part->shapeSizes != nullptr)
        {
            MemoryDeallocate2_(part->shapeSizes);
        }
        if (part->shapeCounts != nullptr)
        {
            MemoryDeallocate2_(part->shapeCounts);
        }

        FreeDecodedSkinSubModel(part->decoded);

        MemoryDeallocate2_(part);
    }

    if (subModel->parts != nullptr)
    {
        MemoryDeallocate2_(subModel->parts);
    }

    MemoryDeallocate2_(subModel);
}
} // namespace

namespace Platform::Graphics
{
void ResetDevices()
{
}

void ResetPath()
{
}

// The next of the display's 60 refreshes a second (IsPalDisplay's NTSC), counted from the last one waited for, and then the
// window's events: closing it ends Main's loop after the frame
void WaitVSync()
{
    constexpr u64 Refresh = 1000000000 / 60;
    static u64 next = DesktopClock::Now();
    next = std::max(next + Refresh, DesktopClock::Now());
    DesktopClock::SleepUntil(next);
    if (DesktopWindow::HandleEvents())
    {
        g_GameState = GameStateQuitting;
    }
}

bool IsPalDisplay()
{
    return false;
}

void WaitIdle()
{
}

void StartRenderer(s32 height, bool pal)
{
    DesktopWindow::Open();
}

void FinishRendererStart()
{
}

void MoveDisplay(const Vector2* offset)
{
}

void StepAnimations(const TimeClock* clock)
{
}

// The matrix from clip space to the GS's own coordinates (16ths of a pixel, the screen's middle at 2048), which the game's view
// composes into the matrix a model is drawn through (the PS2's renderer/frame.cpp does the same)
void SetUpRenderTarget(RenderTargetDescription* target)
{
    if (target == nullptr)
    {
        return;
    }

    constexpr f32 Middle = 2048.0f;
    constexpr f32 Subpixels = 16.0f;
    constexpr f32 HalfSubpixels = 8.0f;
    constexpr f32 DepthRange = 8388607.0f;

    f32 left = Middle - static_cast<f32>(target->displayWidth) * 0.5f + static_cast<f32>(target->offsetX);
    f32 top = Middle - static_cast<f32>(target->displayHeight) * 0.5f + static_cast<f32>(target->offsetY);
    f32 width = static_cast<f32>(target->width);
    f32 height = static_cast<f32>(target->height);

    Matrix4x4* matrix = &target->clipToScreen;
    InitIdentityMatrix(matrix);
    matrix->m[3][0] = (left + width * 0.5f) * Subpixels;
    target->x = static_cast<s32>(left);
    matrix->m[1][1] = -height * HalfSubpixels;
    target->y = static_cast<s32>(top);
    matrix->m[0][0] = width * HalfSubpixels;
    matrix->m[3][2] = DepthRange;
    matrix->m[2][2] = -DepthRange;
    matrix->m[3][1] = (top + height * 0.5f) * Subpixels;

    g_TargetX = target->x;
    g_TargetY = target->y;
}

// The frame's end: the window's next refresh waited for, then what was drawn shown. The frame was cleared at its start
void Present(const void* commands)
{
    WaitVSync();
    DesktopWindow::Present();
}

void PresentFromInterrupt(const void* commands)
{
}

void WaitSent()
{
}

void ResetBuckets(bool movie)
{
    // Nothing's still reading last frame's memory at a restart
    (void)movie;
    ResetFrameMemory();
}

void SubmitBuckets(bool fromInterrupt)
{
    // The frame is drawn: its memory starts over for the next
    (void)fromInterrupt;
    ResetFrameMemory();
}

void* AllocFrameMemory(u32 count, u32 size)
{
    return AllocFrameMemoryArena(static_cast<size_t>(count) * size);
}

void FinishFrame()
{
}

void FinishScene(bool effects)
{
}

// The frame's start: the color it's cleared to (once a frame, before anything of it is drawn)
void StartFrame(const FrameStart& frame)
{
    if (frame.screenWidth > 0 && frame.screenHeight > 0)
    {
        g_ScreenWidthPx = frame.screenWidth;
        g_ScreenHeightPx = frame.screenHeight;
    }

    if (frame.clearColor)
    {
        g_ClearColor[0] = frame.red;
        g_ClearColor[1] = frame.green;
        g_ClearColor[2] = frame.blue;
    }

    SdlGfx::Clear(g_ClearColor[0], g_ClearColor[1], g_ClearColor[2]);
}

void UseHelperPrograms(u32 set, bool wait)
{
}

Material* MakeFlatMaterial()
{
    if (!g_FlatMaterialMade)
    {
        ConstructDesktopMaterial(&g_FlatMaterial);
        std::memset(&g_FlatShader, 0, sizeof(g_FlatShader));
        for (u32 channel = 0; channel < 4; channel++)
        {
            g_FlatShader.shaderColour[channel] = 1.0f;
        }

        g_FlatMaterial.shaderCount = 1;
        g_FlatMaterial.shaders[0] = &g_FlatShader;
        g_FlatMaterialMade = true;
    }

    return reinterpret_cast<Material*>(&g_FlatMaterial);
}

Material* FlatMaterial()
{
    return reinterpret_cast<Material*>(&g_FlatMaterial);
}

void ConstructMaterial(Material* material)
{
    ConstructDesktopMaterial(reinterpret_cast<DesktopGraphics::Material*>(material));
}

void DestroyMaterial(Material* material)
{
    auto* desktop = reinterpret_cast<DesktopGraphics::Material*>(material);
    if (desktop == nullptr)
    {
        return;
    }

    for (u32 shader = 0; shader < desktop->shaderCount && shader < 4; shader++)
    {
        DestroyDesktopShader(desktop->shaders[shader]);
        desktop->shaders[shader] = nullptr;
    }

    desktop->shaderCount = 0;
}

void* MakeParticleMaterial(Material* material)
{
    auto* desktop = reinterpret_cast<DesktopGraphics::Material*>(material);
    auto* shader = static_cast<DesktopGraphics::Shader*>(MemoryAllocate(sizeof(DesktopGraphics::Shader)));
    if (shader == nullptr)
    {
        return nullptr;
    }

    std::memset(shader, 0, sizeof(DesktopGraphics::Shader));
    for (u32 channel = 0; channel < 4; channel++)
    {
        shader->shaderColour[channel] = 1.0f;
    }

    desktop->shaderCount = 1;
    desktop->shaders[0] = shader;
    return shader;
}

void MakeDistortionMaterial(Material* material)
{
    auto* desktop = reinterpret_cast<DesktopGraphics::Material*>(material);
    ConstructDesktopMaterial(desktop);
}

void ChainParticleBlocks(u8* const* blocks, s32 count)
{
}

void EndParticleBlock(u8* block)
{
}

void InitParticleBlock(u8* block, bool hexagons)
{
}

void InitParticleRenderTable(u8* table)
{
}

void InitParticleGraphics()
{
}

void WriteParticleRenderTable(u8* table, const ParticleLook& look)
{
}

void DrawParticleBlock(u8* block, u8* table, const Matrix4x4* matrix, Material* material, f32 time)
{
}

void DrawDistortionBlock(u8* block, u8* table, const Matrix4x4* matrix, Material* material, f32 time, f32 distortionX, f32 distortionY)
{
}

void DrawDecals(DecalData* decals)
{
}

// A rigid model's submodels drawn with a model-to-screen matrix (defined with the rigid model's draw). The depth isn't written
// when writeDepth is clear (a background)
void DrawRigidModelToScreen(RigidModel* model, const Matrix4x4* toScreen, bool writeDepth = true);

void DrawPlacedModel(RigidModel* model, ChunkView* view, const Matrix4x4* world)
{
    if (view == nullptr)
    {
        return;
    }

    // The view's toScreen is the model's to the screen: the game's culling set it for this model (ChunkView::DrawnModelResult,
    // what the PS2's instance gets)
    DrawRigidModelToScreen(model, &view->toScreen);
    (void)world;
}

RigidModel* LodModelAt(Lod* lod, u32 distance)
{
    auto* desktop = reinterpret_cast<DesktopGraphics::Lod*>(lod);
    if (desktop == nullptr || desktop->meshes == nullptr)
    {
        return nullptr;
    }

    if (distance < desktop->nearest || desktop->farthest < distance)
    {
        return nullptr;
    }

    RigidModel* mesh = reinterpret_cast<RigidModel*>(desktop->meshes[0]);
    s32 last = static_cast<s32>(desktop->count) - 1;
    if (last <= 0 || distance < desktop->switchDistances[0])
    {
        return mesh;
    }

    mesh = reinterpret_cast<RigidModel*>(desktop->meshes[1]);
    for (s32 index = 1; index < last && distance >= desktop->switchDistances[index]; index++)
    {
        mesh = reinterpret_cast<RigidModel*>(desktop->meshes[index + 1]);
    }

    return mesh;
}

void SetFontPage(Material* material, u32 page)
{
    auto* desktop = reinterpret_cast<DesktopGraphics::Material*>(material);
    if (desktop != nullptr && desktop->shaderCount != 0 && desktop->shaders[0] != nullptr)
    {
        desktop->shaders[0]->fontPage = static_cast<u8>(page);
    }
}

// The UI's sprites and shapes, whose places and texture areas are fractions of the screen and of the texture
void DrawSprite(Material* material, u32 colour, const Rectangle& place, const Rectangle& texturePlace)
{
    s32 windowWidth = 0;
    s32 windowHeight = 0;
    SdlGfx::GetSize(&windowWidth, &windowHeight);
    if (windowWidth <= 0 || windowHeight <= 0)
    {
        return;
    }

    // The screen the game draws in: place is normalized 0..1 fractions of the window
    f32 across = static_cast<f32>(windowWidth);
    f32 down = static_cast<f32>(windowHeight);

    Rectangle p = place;
    if (p.width < 0.0f)
    {
        p.x += p.width;
        p.width = -p.width;
    }
    if (p.height < 0.0f)
    {
        p.y += p.height;
        p.height = -p.height;
    }

    s32 screenX = static_cast<s32>(std::floor(p.x * across));
    s32 screenY = static_cast<s32>(std::floor(p.y * down));
    s32 screenWidth = static_cast<s32>(std::ceil((p.x + p.width) * across)) - screenX;
    s32 screenHeight = static_cast<s32>(std::ceil((p.y + p.height) * down)) - screenY;

    if (screenWidth < 0)
    {
        screenWidth = -screenWidth;
        screenX -= screenWidth;
    }
    if (screenHeight < 0)
    {
        screenHeight = -screenHeight;
        screenY -= screenHeight;
    }

    u8 red = ScaleChannel(static_cast<u8>(colour & 0xFF), 192);
    u8 green = ScaleChannel(static_cast<u8>((colour >> 8) & 0xFF), 192);
    u8 blue = ScaleChannel(static_cast<u8>((colour >> 16) & 0xFF), 192);
    u8 alpha = ScaleChannel(static_cast<u8>((colour >> 24) & 0xFF), 128);

    auto* desktopMat = reinterpret_cast<DesktopGraphics::Material*>(material);
    DesktopGraphics::Texture* tex = MaterialTexture(desktopMat);

    if (tex != nullptr && tex->sdlTexture != nullptr)
    {
        // The texture's rows run bottom-up (the decoded image's row 0 is its bottom), so the area's rows are turned about
        // within their rectangle to read top-down on the screen, as the font's glyphs do
        SdlGfx::DrawTextureFlipped(tex->sdlTexture, screenX, screenY, screenWidth, screenHeight,
                                   static_cast<s32>(texturePlace.x * static_cast<f32>(tex->width)),
                                   static_cast<s32>(texturePlace.y * static_cast<f32>(tex->height)),
                                   static_cast<s32>(texturePlace.width * static_cast<f32>(tex->width)),
                                   static_cast<s32>(texturePlace.height * static_cast<f32>(tex->height)),
                                   red, green, blue, alpha);
    }
    else
    {
        SdlGfx::FillRect(screenX, screenY, screenWidth, screenHeight, red, green, blue, alpha);
    }
}

void DrawTurnedSprite(Material* material, u32 colour, const Vector2* corners, const Rectangle& texture)
{
    if (corners == nullptr)
    {
        return;
    }

    f32 left = corners[0].x;
    f32 top = corners[0].y;
    f32 right = corners[0].x;
    f32 bottom = corners[0].y;
    for (u32 corner = 1; corner < 4; corner++)
    {
        left = std::min(left, corners[corner].x);
        top = std::min(top, corners[corner].y);
        right = std::max(right, corners[corner].x);
        bottom = std::max(bottom, corners[corner].y);
    }

    Rectangle place;
    place.x = left;
    place.y = top;
    place.width = right - left;
    place.height = bottom - top;

    DrawSprite(material, colour, place, texture);
}

void DrawStrip(Material* material, u32 count, const Vector2* places, const u32* colours)
{
    if (count < 3 || places == nullptr || colours == nullptr)
    {
        return;
    }

    s32 windowWidth = 0;
    s32 windowHeight = 0;
    SdlGfx::GetSize(&windowWidth, &windowHeight);
    if (windowWidth <= 0 || windowHeight <= 0)
    {
        return;
    }

    // The places are pixels of the screen the game draws in (the PS2's draw2d.cpp takes them as pixels, where the sprites'
    // places are fractions of it), scaled to the window
    f32 across = static_cast<f32>(windowWidth) / static_cast<f32>(g_ScreenWidthPx);
    f32 down = static_cast<f32>(windowHeight) / static_cast<f32>(g_ScreenHeightPx);
    auto* vertices = static_cast<SdlGfx::Vertex*>(MemoryAllocate(sizeof(SdlGfx::Vertex) * count));
    if (vertices == nullptr)
    {
        return;
    }

    for (u32 vertex = 0; vertex < count; vertex++)
    {
        u32 colour = colours[vertex];
        vertices[vertex].x = places[vertex].x * across;
        vertices[vertex].y = places[vertex].y * down;
        vertices[vertex].red = ScaleChannel(static_cast<u8>(colour & 0xFF), 192);
        vertices[vertex].green = ScaleChannel(static_cast<u8>((colour >> 8) & 0xFF), 192);
        vertices[vertex].blue = ScaleChannel(static_cast<u8>((colour >> 16) & 0xFF), 192);
        vertices[vertex].alpha = ScaleChannel(static_cast<u8>((colour >> 24) & 0xFF), 128);
        vertices[vertex].u = 0.0f;
        vertices[vertex].v = 0.0f;
    }

    auto* desktop = reinterpret_cast<DesktopGraphics::Material*>(material);
    DesktopGraphics::Texture* tex = MaterialTexture(desktop);
    SdlGfx::DrawTriangleStrip(tex != nullptr ? tex->sdlTexture : nullptr, vertices, count);
    MemoryDeallocate2_(vertices);
}

namespace
{
// A strip's vertex in the clip space the model's to-screen matrix makes (its w is the view's depth), with what the draw needs
// of it (the packet's own texture coordinates and colour)
struct ClipVertex
{
    f32 x;
    f32 y;
    f32 z;
    f32 w;
    f32 u;
    f32 v;
    u32 colour;
};

// A packed colour's channels blended along the edge from a to b (t 0 at a, 1 at b)
u32 BlendColours(u32 a, u32 b, f32 t)
{
    u32 colour = 0;
    for (u32 shift = 0; shift < 32; shift += 8)
    {
        f32 from = static_cast<f32>((a >> shift) & 0xFF);
        f32 to = static_cast<f32>((b >> shift) & 0xFF);
        f32 value = from + (to - from) * t;
        u32 channel = static_cast<u32>(value + 0.5f);
        colour |= (channel > 255 ? 255 : channel) << shift;
    }

    return colour;
}

ClipVertex BlendClip(const ClipVertex& a, const ClipVertex& b, f32 t)
{
    ClipVertex out;
    out.x = a.x + (b.x - a.x) * t;
    out.y = a.y + (b.y - a.y) * t;
    out.z = a.z + (b.z - a.z) * t;
    out.w = a.w + (b.w - a.w) * t;
    out.u = a.u + (b.u - a.u) * t;
    out.v = a.v + (b.v - a.v) * t;
    out.colour = BlendColours(a.colour, b.colour, t);
    return out;
}

// A triangle clipped to the camera's own plane (w at or below zero is behind it): what's in front, three or four vertexes (none
// when the whole triangle is behind). The near plane itself is left to the depth test (the clipped edges' depths are the same
// fractions of the way along their edges as their places are)
u32 ClipTriangleToCamera(const ClipVertex* triangle, ClipVertex* out)
{
    constexpr f32 CameraPlane = 0.0001f;
    u32 count = 0;
    for (u32 index = 0; index < 3; index++)
    {
        const ClipVertex& a = triangle[index];
        const ClipVertex& b = triangle[(index + 1) % 3];
        bool aInside = a.w >= CameraPlane;
        bool bInside = b.w >= CameraPlane;
        if (aInside)
        {
            out[count++] = a;
        }
        if (aInside != bInside)
        {
            out[count++] = BlendClip(a, b, (CameraPlane - a.w) / (b.w - a.w));
        }
    }

    return count;
}

// A clip vertex to the window's pixels and depth (0 nearest, 1 farthest)
void ClipToScreen(const ClipVertex& clip, f32 across, f32 down, SdlGfx::Vertex3D* out)
{
    constexpr f32 Subpixels = 16.0f;
    constexpr f32 DepthRange = 8388607.0f;
    f32 inverse = 1.0f / clip.w;
    f32 gameX = clip.x * inverse / Subpixels - static_cast<f32>(g_TargetX);
    f32 gameY = clip.y * inverse / Subpixels - static_cast<f32>(g_TargetY);
    out->x = gameX * across;
    out->y = gameY * down;
    out->z = 1.0f - clip.z * inverse / (2.0f * DepthRange);
    out->red = ScaleChannel(static_cast<u8>(clip.colour & 0xFF), 192);
    out->green = ScaleChannel(static_cast<u8>((clip.colour >> 8) & 0xFF), 192);
    out->blue = ScaleChannel(static_cast<u8>((clip.colour >> 16) & 0xFF), 192);
    out->alpha = ScaleChannel(static_cast<u8>((clip.colour >> 24) & 0xFF), 128);
    out->u = clip.u;
    // The game texture's row 0 (v 0) is the decoded image's row 0, which the SDL texture keeps as its own row 0 (its t 0): the
    // model's v samples the row the game means as it is (the 2D drawers' "turned" path serves a top-down source area, not this)
    out->v = clip.v;
}
}

// One decoded submodel's strips drawn: each strip's vertexes are transformed by the model's matrix to the GS's coordinates, then
// taken to the window's pixels and the game screen's, and drawn depth-tested. A strip wholly in front of the camera is drawn in
// one go; one crossing the camera's own plane is clipped to it triangle by triangle, so the level's big faces aren't dropped whole
void DrawDecodedSubModel(DesktopGraphics::DecodedSubModel* decoded, void* texture, const Matrix4x4* matrix, bool writeDepth = true)
{
    if (decoded == nullptr || matrix == nullptr || decoded->vertexCount == 0)
    {
        return;
    }

    s32 windowWidth = 0;
    s32 windowHeight = 0;
    SdlGfx::GetSize(&windowWidth, &windowHeight);
    if (windowWidth <= 0 || windowHeight <= 0 || g_ScreenWidthPx <= 0 || g_ScreenHeightPx <= 0)
    {
        return;
    }

    f32 across = static_cast<f32>(windowWidth) / static_cast<f32>(g_ScreenWidthPx);
    f32 down = static_cast<f32>(windowHeight) / static_cast<f32>(g_ScreenHeightPx);

    for (u32 strip = 0; strip < decoded->stripCount; strip++)
    {
        u32 count = decoded->stripCounts[strip];
        if (count < 3)
        {
            continue;
        }

        auto* clips = static_cast<ClipVertex*>(std::malloc(sizeof(ClipVertex) * count));
        auto* screen = static_cast<SdlGfx::Vertex3D*>(std::malloc(sizeof(SdlGfx::Vertex3D) * (count + 1)));
        if (clips == nullptr || screen == nullptr)
        {
            std::free(clips);
            std::free(screen);
            return;
        }

        bool behind = false;
        for (u32 index = 0; index < count; index++)
        {
            u32 vertex = decoded->stripStarts[strip] + index;
            Vector4 transformed;
            VuTransformPoint(matrix, &decoded->positions[vertex], &transformed);
            ClipVertex& clip = clips[index];
            clip.x = transformed.x;
            clip.y = transformed.y;
            clip.z = transformed.z;
            clip.w = transformed.w;
            clip.u = decoded->textureCoordinates[vertex].x;
            clip.v = decoded->textureCoordinates[vertex].y;
            clip.colour = decoded->colours[vertex];
            if (transformed.w < 0.0001f)
            {
                behind = true;
            }
        }

        if (!behind)
        {
            for (u32 index = 0; index < count; index++)
            {
                ClipToScreen(clips[index], across, down, &screen[index]);
            }

            SdlGfx::DrawTriangleStrip3D(texture, screen, count, writeDepth);
        }
        else
        {
            // The strip is a run of triangles (i, i + 1, i + 2): each one crossing the camera's plane is clipped and drawn on
            // its own (the vertexes it keeps are shared with neither neighbour, which the depth test then sorts out)
            for (u32 index = 0; index + 2 < count; index++)
            {
                ClipVertex triangle[3] = {clips[index], clips[index + 1], clips[index + 2]};
                ClipVertex polygon[4];
                u32 vertices = ClipTriangleToCamera(triangle, polygon);
                if (vertices < 3)
                {
                    continue;
                }

                for (u32 vertex = 0; vertex < vertices; vertex++)
                {
                    ClipToScreen(polygon[vertex], across, down, &screen[vertex]);
                }

                SdlGfx::DrawTriangleStrip3D(texture, screen, vertices, writeDepth);
            }
        }

        std::free(clips);
        std::free(screen);
    }
}

// A blend skin's own deformations of a part's vertexes: the offsets one of the part's shapes holds for each of them (four signed
// bytes a vertex: its X, Y and Z in blend shape units), and the shapes the OGI's animation weighs this frame
struct BlendShapeState
{
    const u32* counts;       // per shape: how many vertexes its offsets hold
    u8* const* addresses;    // per shape: the offsets, four signed bytes a vertex
    const f32* factors;      // the part's blend shape scale, a component a word (the offsets are its units)
    const f32* weights;      // how much of each active shape this frame
    const s32* shapes;       // which shapes are active
    s32 shapeCount;          // how many are active
    u32 shapeTotal;          // how many the part has
};

// One decoded skin submodel's strips drawn: each vertex is blended by its joints (the VU1 skinning programme's own work) into the
// model's space, then taken to the GS's coordinates and the window's pixels and the game screen's, and drawn depth-tested. A strip
// wholly in front of the camera is drawn in one go; one crossing the camera's own plane is clipped to it triangle by triangle
void DrawDecodedSkinSubModel(DesktopGraphics::DecodedSkinSubModel* decoded, void* texture, const Matrix4x4* toScreen,
                             const Matrix4x4* joints, u32 jointCount, bool writeDepth = true, const BlendShapeState* blend = nullptr)
{
    if (decoded == nullptr || toScreen == nullptr || decoded->vertexCount == 0)
    {
        return;
    }

    s32 windowWidth = 0;
    s32 windowHeight = 0;
    SdlGfx::GetSize(&windowWidth, &windowHeight);
    if (windowWidth <= 0 || windowHeight <= 0 || g_ScreenWidthPx <= 0 || g_ScreenHeightPx <= 0)
    {
        return;
    }

    f32 across = static_cast<f32>(windowWidth) / static_cast<f32>(g_ScreenWidthPx);
    f32 down = static_cast<f32>(windowHeight) / static_cast<f32>(g_ScreenHeightPx);

    for (u32 strip = 0; strip < decoded->stripCount; strip++)
    {
        u32 count = decoded->stripCounts[strip];
        u32 stripStart = decoded->stripStarts[strip];
        if (count < 3)
        {
            continue;
        }

        auto* clips = static_cast<ClipVertex*>(std::malloc(sizeof(ClipVertex) * count));
        auto* screen = static_cast<SdlGfx::Vertex3D*>(std::malloc(sizeof(SdlGfx::Vertex3D) * (count + 1)));
        if (clips == nullptr || screen == nullptr)
        {
            std::free(clips);
            std::free(screen);
            return;
        }

        bool behind = false;
        for (u32 index = 0; index < count; index++)
        {
            u32 vertex = stripStart + index;
            Vector4 base = decoded->positions[vertex];
            if (blend != nullptr)
            {
                // The shape offsets are added to the bind pose before the joints blend it (the VU1's own order)
                for (s32 shape = 0; shape < blend->shapeCount; shape++)
                {
                    s32 shapeIndex = blend->shapes[shape];
                    if (shapeIndex < 0 || static_cast<u32>(shapeIndex) >= blend->shapeTotal ||
                        blend->addresses[shapeIndex] == nullptr || vertex >= blend->counts[shapeIndex])
                    {
                        continue;
                    }

                    const s8* offset = reinterpret_cast<const s8*>(blend->addresses[shapeIndex]) + vertex * 4;
                    f32 weight = blend->weights[shape];
                    base.x += weight * offset[0] * blend->factors[0];
                    base.y += weight * offset[1] * blend->factors[1];
                    base.z += weight * offset[2] * blend->factors[2];
                }
            }

            u8 amount = decoded->jointAmounts[vertex];
            Vector4 posed = base;
            if (joints != nullptr)
            {
                Vector4 blended{0.0f, 0.0f, 0.0f, 0.0f};
                f32 applied = 0.0f;
                for (u32 joint = 0; joint < amount; joint++)
                {
                    s32 jointIndex = decoded->jointIndices[vertex * 3 + joint];
                    if (jointIndex < 0 || static_cast<u32>(jointIndex) >= jointCount)
                    {
                        continue;
                    }

                    Vector4 transformed;
                    VuTransformPoint(&joints[jointIndex], &base, &transformed);
                    f32 weight = decoded->jointWeights[vertex * 3 + joint];
                    blended.x += weight * transformed.x;
                    blended.y += weight * transformed.y;
                    blended.z += weight * transformed.z;
                    blended.w += weight * transformed.w;
                    applied += weight;
                }

                if (applied != 0.0f)
                {
                    posed = blended;
                }
            }

            Vector4 screenPos;
            VuTransformPoint(toScreen, &posed, &screenPos);
            ClipVertex& clip = clips[index];
            clip.x = screenPos.x;
            clip.y = screenPos.y;
            clip.z = screenPos.z;
            clip.w = screenPos.w;
            clip.u = decoded->textureCoordinates[vertex].x;
            clip.v = decoded->textureCoordinates[vertex].y;
            clip.colour = decoded->colours[vertex];
            if (screenPos.w < 0.0001f)
            {
                behind = true;
            }
        }

        if (!behind)
        {
            for (u32 index = 0; index < count; index++)
            {
                ClipToScreen(clips[index], across, down, &screen[index]);
            }

            SdlGfx::DrawTriangleStrip3D(texture, screen, count, writeDepth, decoded->jointNoDraw + stripStart);
        }
        else
        {
            for (u32 index = 0; index + 2 < count; index++)
            {
                if (decoded->jointNoDraw[stripStart + index + 2] != 0)
                {
                    continue;   // the triangle ending at a no-draw vertex is one the GS draws not either
                }

                ClipVertex triangle[3] = {clips[index], clips[index + 1], clips[index + 2]};
                ClipVertex polygon[4];
                u32 vertices = ClipTriangleToCamera(triangle, polygon);
                if (vertices < 3)
                {
                    continue;
                }

                for (u32 vertex = 0; vertex < vertices; vertex++)
                {
                    ClipToScreen(polygon[vertex], across, down, &screen[vertex]);
                }

                SdlGfx::DrawTriangleStrip3D(texture, screen, vertices, writeDepth);
            }
        }

        std::free(clips);
        std::free(screen);
    }
}

// A rigid model's submodels drawn with the model-to-screen matrix it's given (the model's own materials)
void DrawRigidModelToScreen(RigidModel* model, const Matrix4x4* toScreen, bool writeDepth)
{
    auto* desktop = reinterpret_cast<DesktopGraphics::RigidModel*>(model);
    if (desktop == nullptr || desktop->model == nullptr || toScreen == nullptr)
    {
        return;
    }

    DesktopGraphics::Model* decodedModel = desktop->model;
    u32 count = desktop->count;
    if (decodedModel->subModelCount < count)
    {
        count = decodedModel->subModelCount;
    }

    for (u32 subModel = 0; subModel < count; subModel++)
    {
        DesktopGraphics::Material* material = nullptr;
        if (desktop->materials != nullptr && desktop->materials[subModel] != nullptr)
        {
            material = desktop->materials[subModel]->material;
        }

        DesktopGraphics::Texture* texture = MaterialTexture(material);
        DesktopGraphics::DecodedSubModel* decoded = DecodedSubModelOf(decodedModel, subModel);
        SetMaterialState(material);
        DrawDecodedSubModel(decoded, texture != nullptr ? texture->sdlTexture : nullptr, toScreen, writeDepth);
    }
}

void DrawRigidModel(RigidModel* model, const Matrix4x4* matrix, const ModelLights& lights, u32 mode)
{
    if (matrix == nullptr)
    {
        return;
    }

    // The matrix is the model's to the world; the view's to the screen makes the model's to the screen (what the PS2's VU1
    // gets in an instance's toScreen)
    Matrix4x4 toScreen;
    VuMultiplyMatrices(matrix, DesktopGraphics::ViewToScreen(), &toScreen);
    DrawRigidModelToScreen(model, &toScreen);

    // The lighting is VU1's for now; the colours the packets carry are drawn as they are
    (void)lights;
    (void)mode;
}

void DrawSkin(Skin* skin, const Matrix4x4* joints, u32 jointCount, const Matrix4x4* matrix, const ModelLights& lights, u32 mode)
{
    auto* desktop = reinterpret_cast<DesktopGraphics::Skin*>(skin);
    if (desktop == nullptr || matrix == nullptr)
    {
        return;
    }

    // The matrix is the model's to the world; the view's to the screen makes the model's to the screen, the joints' own (the
    // model's space) blending each vertex before it (what the PS2's VU1 gets in a skin's toScreen)
    Matrix4x4 toScreen;
    VuMultiplyMatrices(matrix, DesktopGraphics::ViewToScreen(), &toScreen);

    for (u32 subModel = 0; subModel < desktop->count; subModel++)
    {
        DesktopGraphics::Material* material = nullptr;
        if (desktop->materials != nullptr && desktop->materials[subModel] != nullptr)
        {
            material = desktop->materials[subModel]->material;
        }

        DesktopGraphics::Texture* texture = MaterialTexture(material);
        DesktopGraphics::DecodedSkinSubModel* decoded = DecodedSkinSubModelOf(desktop, subModel);
        SetMaterialState(material);
        DrawDecodedSkinSubModel(decoded, texture != nullptr ? texture->sdlTexture : nullptr, &toScreen, joints, jointCount);
    }

    // The lighting is VU1's for now; the colours the packets carry are drawn as they are
    (void)lights;
    (void)mode;
}

void DrawBlendSkin(BlendSkin* skin, const Matrix4x4* joints, u32 jointCount, const Matrix4x4* matrix, const ModelLights& lights, const f32* weights, const s32* shapes, s32 shapeCount, u32 mode)
{
    auto* desktop = reinterpret_cast<DesktopGraphics::BlendSkin*>(skin);
    if (desktop == nullptr || matrix == nullptr)
    {
        return;
    }

    // The matrix is the model's to the world; the view's to the screen makes the model's to the screen, the joints' own (the
    // model's space) blending each vertex after the shape offsets have deformed it (what the PS2's VU1 gets in a blend skin's)
    Matrix4x4 toScreen;
    VuMultiplyMatrices(matrix, DesktopGraphics::ViewToScreen(), &toScreen);

    for (u32 subModel = 0; subModel < desktop->count; subModel++)
    {
        DesktopGraphics::BlendSubModel* sub = desktop->subModels != nullptr ? desktop->subModels[subModel] : nullptr;
        if (sub == nullptr)
        {
            continue;
        }

        DesktopGraphics::Material* material = nullptr;
        if (desktop->materials != nullptr && desktop->materials[subModel] != nullptr)
        {
            material = desktop->materials[subModel]->material;
        }

        DesktopGraphics::Texture* texture = MaterialTexture(material);
        void* sdlTexture = texture != nullptr ? texture->sdlTexture : nullptr;
        SetMaterialState(material);

        for (u32 partIndex = 0; partIndex < sub->count; partIndex++)
        {
            DesktopGraphics::BlendPart* part = sub->parts != nullptr ? sub->parts[partIndex] : nullptr;
            if (part == nullptr)
            {
                continue;
            }

            // The parts of a submodel share their blend shape scale, the first one's (what the PS2 writes to VU1)
            const f32* factors = sub->parts[0] != nullptr ? sub->parts[0]->shapeFactors : part->shapeFactors;
            BlendShapeState blend;
            blend.counts = part->shapeCounts;
            blend.addresses = part->shapeAddresses;
            blend.factors = factors;
            blend.weights = weights;
            blend.shapes = shapes;
            blend.shapeCount = shapeCount;
            blend.shapeTotal = part->shapeCount;

            DrawDecodedSkinSubModel(DecodedBlendPartOf(part), sdlTexture, &toScreen, joints, jointCount, true, &blend);
        }
    }

    // The lighting is VU1's for now; the colours the packets carry are drawn as they are
    (void)lights;
    (void)mode;
}

void SetUpShadowPass()
{
}

void BeginShadows()
{
}

void EndShadows()
{
}

void DrawShadowMesh(RigidModel* mesh, const Matrix4x4* toScreen, const Matrix4x4* toCamera, const Matrix4x4* world)
{
}

void BeginScreenModel()
{
}

void SetScreenModelMaterial(Material* material)
{
}

void SetScreenModelColour(u32 colour)
{
}

void AddScreenModelVertex(const Vector4* place)
{
}

ScreenModel* EndScreenModel()
{
    return nullptr;
}

void DeleteScreenModel(ScreenModel* model)
{
}

void DrawScreenModel(ScreenModel* model, const Matrix4x4* toScreen, const Matrix4x4* toClip, const Vector4* clip)
{
}

Material* MakeSkidMaterial(bool subtracting)
{
    return nullptr;
}

void RestartAnimations(RigidModel* model)
{
    auto* desktop = reinterpret_cast<DesktopGraphics::RigidModel*>(model);
    if (desktop == nullptr || desktop->materials == nullptr || desktop->model == nullptr)
    {
        return;
    }

    u32 count = desktop->count;
    if (desktop->model->subModelCount < count)
    {
        count = desktop->model->subModelCount;
    }

    for (u32 index = 0; index < count; index++)
    {
        RestartMaterialAnimations(desktop->materials[index]);
    }
}

void RestartAnimations(Skin* skin)
{
    auto* desktop = reinterpret_cast<DesktopGraphics::Skin*>(skin);
    if (desktop == nullptr)
    {
        return;
    }

    for (u32 index = 0; index < desktop->count; index++)
    {
        RestartMaterialAnimations(desktop->materials != nullptr ? desktop->materials[index] : nullptr);
    }
}

void RestartAnimations(BlendSkin* skin)
{
    auto* desktop = reinterpret_cast<DesktopGraphics::BlendSkin*>(skin);
    if (desktop == nullptr)
    {
        return;
    }

    for (u32 index = 0; index < desktop->count; index++)
    {
        RestartMaterialAnimations(desktop->materials != nullptr ? desktop->materials[index] : nullptr);
    }
}

MaterialResource* FirstMaterial(const RigidModel* model)
{
    auto* desktop = reinterpret_cast<const DesktopGraphics::RigidModel*>(model);
    if (desktop == nullptr || desktop->materials == nullptr || desktop->count == 0)
    {
        return nullptr;
    }

    return reinterpret_cast<MaterialResource*>(desktop->materials[0]);
}

GameTexture* NewTexture(u32 id)
{
    auto* texture = static_cast<DesktopGraphics::GameTexture*>(MemoryAllocate(sizeof(DesktopGraphics::GameTexture)));
    if (texture == nullptr)
    {
        return nullptr;
    }

    ConstructResourceHeader(texture, id);
    texture->size = 0;
    std::memset(&texture->texture, 0, sizeof(texture->texture));
    return reinterpret_cast<GameTexture*>(texture);
}

void DeleteTexture(GameTexture* texture)
{
    auto* desktop = reinterpret_cast<DesktopGraphics::GameTexture*>(texture);
    if (desktop == nullptr)
    {
        return;
    }

    if (desktop->texture.sdlTexture != nullptr)
    {
        SdlGfx::DestroyTexture(desktop->texture.sdlTexture);
        desktop->texture.sdlTexture = nullptr;
    }

    MemoryDeallocate2_(desktop);
}

// A texture's file: its size, its 0x60 byte GS header, 0x20 bytes of the tools' memory and the transfer packet, which is decoded
// to the window's texture
void ReadTexture(GameTexture* texture, Stream* stream)
{
    auto* desktop = reinterpret_cast<DesktopGraphics::GameTexture*>(texture);
    stream->ReadU32(&desktop->size);

    u8 header[0x60];
    stream->Read(header, sizeof(header), 1);

    u8 leftover[0x20];
    stream->Read(leftover, sizeof(leftover), 1);

    u32 packetSize = desktop->size > sizeof(header) + sizeof(leftover) ? desktop->size - (sizeof(header) + sizeof(leftover)) : 0;
    u8* packet = packetSize != 0 ? static_cast<u8*>(MemoryAllocate(packetSize)) : nullptr;
    if (packet != nullptr)
    {
        stream->Read(packet, packetSize, 1);
    }

    u32 width = GSTexture::Width(header);
    u32 height = GSTexture::Height(header);
    desktop->texture.widthPower = static_cast<u16>(header[4] | (header[5] << 8));
    desktop->texture.heightPower = static_cast<u16>(header[6] | (header[7] << 8));
    desktop->texture.width = width;
    desktop->texture.height = height;
    desktop->texture.hasAlpha = header[0x0B] != 0;

    u8* rgba = width != 0 && height != 0 ? static_cast<u8*>(MemoryAllocate(width * height * 4)) : nullptr;
    if (rgba != nullptr)
    {
        bool decoded = GSTexture::Decode(header, packet, packetSize, rgba);
        if (!decoded)
        {
            for (u32 pixel = 0; pixel < width * height; pixel++)
            {
                rgba[pixel * 4 + 0] = 128;
                rgba[pixel * 4 + 1] = 128;
                rgba[pixel * 4 + 2] = 128;
                rgba[pixel * 4 + 3] = 255;
            }
        }

        desktop->texture.sdlTexture = SdlGfx::CreateTexture(rgba, static_cast<s32>(width), static_cast<s32>(height));
        Log("ReadTexture %p %ux%u power %u,%u alpha %d decode %d sdl %p", desktop, width, height,
            desktop->texture.widthPower, desktop->texture.heightPower, desktop->texture.hasAlpha ? 1 : 0, decoded ? 1 : 0,
            desktop->texture.sdlTexture);
        if (std::getenv("TWINSANITY_GFX_DUMP") != nullptr)
        {
            static u32 dumped = 0;
            char name[64];
            std::snprintf(name, sizeof(name), "texdump%03u_%ux%u.rgba", dumped++, width, height);
            if (std::FILE* file = std::fopen(name, "wb"))
            {
                std::fwrite(rgba, 1, width * height * 4, file);
                std::fclose(file);
            }
        }

        MemoryDeallocate2_(rgba);
    }

    if (packet != nullptr)
    {
        MemoryDeallocate2_(packet);
    }
}

MaterialResource* NewMaterial(u32 id)
{
    auto* resource = static_cast<DesktopGraphics::MaterialResource*>(MemoryAllocate(sizeof(DesktopGraphics::MaterialResource)));
    if (resource == nullptr)
    {
        return nullptr;
    }

    ConstructResourceHeader(resource, id);
    resource->material = nullptr;
    resource->unused0C = 0;
    return reinterpret_cast<MaterialResource*>(resource);
}

void DeleteMaterial(MaterialResource* material)
{
    auto* desktop = reinterpret_cast<DesktopGraphics::MaterialResource*>(material);
    if (desktop == nullptr)
    {
        return;
    }

    if (desktop->material != nullptr)
    {
        DestroyMaterial(reinterpret_cast<Material*>(desktop->material));
        MemoryDeallocate2_(desktop->material);
        desktop->material = nullptr;
    }

    MemoryDeallocate2_(desktop);
}

// A material's shaders, byte for byte as the PS2 reads them (shaderclasses.cpp), the textures they name taken from the game's
// table (which the sprites' reading filled)
void ReadMaterial(MaterialResource* material, Stream* stream)
{
    auto* resource = reinterpret_cast<DesktopGraphics::MaterialResource*>(material);
    auto* desktop = static_cast<DesktopGraphics::Material*>(MemoryAllocate(sizeof(DesktopGraphics::Material)));
    if (desktop == nullptr)
    {
        return;
    }

    ConstructDesktopMaterial(desktop);

    s64 activatedShaders;
    stream->ReadS64(&activatedShaders);
    desktop->activatedShaders = static_cast<u64>(activatedShaders);
    s32 bucket;
    stream->ReadS32(&bucket);
    desktop->bucket = static_cast<u32>(bucket);

    s32 nameLength;
    stream->ReadS32(&nameLength);
    if (nameLength > 0)
    {
        stream->Skip(nameLength);
    }

    s32 shaderCount;
    stream->ReadS32(&shaderCount);
    if (shaderCount > 4)
    {
        shaderCount = 4;
    }

    desktop->shaderCount = static_cast<u32>(shaderCount);
    for (s32 index = 0; index < shaderCount; index++)
    {
        s32 type;
        stream->ReadS32(&type);

        auto* shader = static_cast<DesktopGraphics::Shader*>(MemoryAllocate(sizeof(DesktopGraphics::Shader)));
        if (shader == nullptr)
        {
            continue;
        }
        std::memset(shader, 0, sizeof(DesktopGraphics::Shader));

        // The fields a shader's kind has before the settings every one has (shaderclasses.cpp's readers)
        switch (type)
        {
        case 0x10:
        case 0x11:
        case 0x18:
        {
            f32 value;
            stream->ReadF32(&value);
            break;
        }
        case 0x17:
        {
            s32 mode;
            f32 speed;
            f32 amplitude;
            stream->ReadS32(&mode);
            stream->ReadF32(&speed);
            stream->ReadF32(&amplitude);
            break;
        }
        case 0x1A:
        {
            s32 mode;
            f32 speed;
            f32 amplitudes[3];
            stream->ReadS32(&mode);
            stream->ReadF32(&speed);
            stream->ReadF32(&amplitudes[0]);
            stream->ReadF32(&amplitudes[1]);
            stream->ReadF32(&amplitudes[2]);
            break;
        }
        default:
            break;
        }

        stream->Read(shader->settings, sizeof(shader->settings), 1);
        stream->ReadU16(reinterpret_cast<u16*>(&shader->lodK));
        stream->ReadU16(reinterpret_cast<u16*>(&shader->lodL));
        stream->Read(shader->leftover, sizeof(shader->leftover), 1);
        stream->Read(shader->shaderColour, sizeof(shader->shaderColour), 1);
        stream->ReadF32(&shader->scrollPhases[0]);
        stream->ReadF32(&shader->scrollPhases[1]);
        stream->ReadF32(&shader->scrollSpeeds[0]);
        stream->ReadF32(&shader->scrollSpeeds[1]);
        stream->ReadU32(&shader->textureId);
        stream->ReadU32(&shader->type);

        // The tools' last settings byte says an animation follows
        if (shader->settings[29] != 0)
        {
            auto* animation = static_cast<ShaderAnimation*>(MemoryAllocate(sizeof(ShaderAnimation)));
            if (animation != nullptr)
            {
                ConstructShaderAnimation(animation);
                ReadShaderAnimation(animation, stream);
                shader->animation = animation;
            }
        }

        u32 textureId = shader->textureId;
        if (textureId != 0)
        {
            shader->texture = reinterpret_cast<DesktopGraphics::GameTexture*>(g_TextureTable.Acquire(&textureId, nullptr));
        }

        desktop->shaders[index] = shader;
    }

    Log("ReadMaterial %p shaders=%u bucket=%u", desktop, desktop->shaderCount, desktop->bucket);
    for (u32 index = 0; index < desktop->shaderCount; index++)
    {
        DesktopGraphics::Shader* shader = desktop->shaders[index];
        Log("  shader[%u] type=0x%X texId=%08X tex=%p anim=%u", index, shader != nullptr ? shader->type : 0,
            shader != nullptr ? shader->textureId : 0, shader != nullptr ? shader->texture : nullptr,
            shader != nullptr && shader->animation != nullptr ? 1 : 0);
    }

    resource->material = desktop;
    resource->unused0C = 0;
}

// The models, meshes, skins, blend skins, levels of detail and skies: the game's resources (they keep their place in the game's
// tables), whose contents the desktop holds in its own types (graphics_internal.h). A model's and skin's submodels keep their
// packets as the files have them; the decode is the desktop's when it draws them
RigidModelData* NewModel(u32 id)
{
    auto* model = static_cast<DesktopGraphics::Model*>(MemoryAllocate(sizeof(DesktopGraphics::Model)));
    if (model == nullptr)
    {
        return nullptr;
    }

    std::memset(model, 0, sizeof(DesktopGraphics::Model));
    ConstructResourceHeader(model, id);
    return reinterpret_cast<RigidModelData*>(model);
}

void DeleteModel(RigidModelData* model)
{
    auto* desktop = reinterpret_cast<DesktopGraphics::Model*>(model);
    if (desktop == nullptr)
    {
        return;
    }

    for (u32 index = 0; index < desktop->subModelCount; index++)
    {
        if (desktop->packets != nullptr && desktop->packets[index] != nullptr)
        {
            MemoryDeallocate2_(desktop->packets[index]);
        }
        if (desktop->decoded != nullptr && desktop->decoded[index] != nullptr)
        {
            FreeDecodedSubModel(desktop->decoded[index]);
        }
    }

    if (desktop->decoded != nullptr)
    {
        std::free(desktop->decoded);
    }
    if (desktop->vertexCounts != nullptr)
    {
        MemoryDeallocate2_(desktop->vertexCounts);
    }
    if (desktop->sizes != nullptr)
    {
        MemoryDeallocate2_(desktop->sizes);
    }
    if (desktop->extraSizes != nullptr)
    {
        MemoryDeallocate2_(desktop->extraSizes);
    }
    if (desktop->packets != nullptr)
    {
        MemoryDeallocate2_(desktop->packets);
    }

    MemoryDeallocate2_(desktop);
}

// Each submodel: its vertex count, its packet's size in bytes, its packet, and the size of the data that follows it in the file
// (read here and dropped: VU1's microcode's own, which the desktop doesn't run)
void ReadModel(RigidModelData* model, Stream* stream)
{
    auto* desktop = reinterpret_cast<DesktopGraphics::Model*>(model);
    s32 subModelCount = 0;
    stream->ReadS32(&subModelCount);
    desktop->subModelCount = static_cast<u32>(subModelCount);

    desktop->vertexCounts = AllocateItems<u32>(desktop->subModelCount);
    desktop->sizes = AllocateItems<u32>(desktop->subModelCount);
    desktop->extraSizes = AllocateItems<u32>(desktop->subModelCount);
    desktop->packets = AllocateItems<u8*>(desktop->subModelCount);
    for (u32 index = 0; index < desktop->subModelCount; index++)
    {
        s32 vertexCount = 0;
        s32 size = 0;
        stream->ReadS32(&vertexCount);
        stream->ReadS32(&size);
        desktop->vertexCounts[index] = static_cast<u32>(vertexCount);
        desktop->sizes[index] = static_cast<u32>(size);
        desktop->packets[index] = ReadPacketBytes(stream, desktop->sizes[index]);

        s32 extraSize = 0;
        stream->ReadS32(&extraSize);
        desktop->extraSizes[index] = static_cast<u32>(extraSize);
        if (extraSize > 0)
        {
            stream->Skip(extraSize);
        }
    }

    Log("ReadModel %p submodels=%u", desktop, desktop->subModelCount);
    for (u32 index = 0; index < desktop->subModelCount; index++)
    {
        Log("  submodel[%u] vertexes=%u bytes=%u extra=%u packet=%p", index, desktop->vertexCounts[index], desktop->sizes[index],
            desktop->extraSizes[index], desktop->packets[index]);
    }
}

RigidModel* NewRigidModel(u32 id)
{
    auto* model = static_cast<DesktopGraphics::RigidModel*>(MemoryAllocate(sizeof(DesktopGraphics::RigidModel)));
    if (model == nullptr)
    {
        return nullptr;
    }

    std::memset(model, 0, sizeof(DesktopGraphics::RigidModel));
    ConstructResourceHeader(model, id);
    return reinterpret_cast<RigidModel*>(model);
}

void DeleteRigidModel(RigidModel* model)
{
    DestroyRigidModel(reinterpret_cast<DesktopGraphics::RigidModel*>(model));
}

// The version, then the materials it names and the model it's of
void ReadRigidModel(RigidModel* model, Stream* stream)
{
    auto* desktop = reinterpret_cast<DesktopGraphics::RigidModel*>(model);
    s32 version = 0;
    stream->ReadS32(&version);
    ReadRigidModelData(desktop, stream);

    Log("ReadRigidModel %p version=%d submodels=%u modelId=%08X", desktop, version, desktop->count, desktop->modelId);
}

// A skin's submodels: each one's material, its packet's size and vertex count, and its packet (the vertexes all of them have added
// up; the constructor cleared the count)
Skin* NewSkin(u32 id)
{
    auto* skin = static_cast<DesktopGraphics::Skin*>(MemoryAllocate(sizeof(DesktopGraphics::Skin)));
    if (skin == nullptr)
    {
        return nullptr;
    }

    std::memset(skin, 0, sizeof(DesktopGraphics::Skin));
    ConstructResourceHeader(skin, id);
    return reinterpret_cast<Skin*>(skin);
}

void DeleteSkin(Skin* skin)
{
    auto* desktop = reinterpret_cast<DesktopGraphics::Skin*>(skin);
    if (desktop == nullptr)
    {
        return;
    }

    for (u32 index = 0; index < desktop->count; index++)
    {
        if (desktop->materials != nullptr && desktop->materials[index] != nullptr)
        {
            g_MaterialTable.ReleaseItem(reinterpret_cast<MaterialResource*>(desktop->materials[index]));
        }

        if (desktop->packets != nullptr && desktop->packets[index] != nullptr)
        {
            MemoryDeallocate2_(desktop->packets[index]);
        }
    }

    if (desktop->materials != nullptr)
    {
        MemoryDeallocate2_(desktop->materials);
    }
    if (desktop->vertexCounts != nullptr)
    {
        MemoryDeallocate2_(desktop->vertexCounts);
    }
    if (desktop->packets != nullptr)
    {
        MemoryDeallocate2_(desktop->packets);
    }
    if (desktop->sizes != nullptr)
    {
        MemoryDeallocate2_(desktop->sizes);
    }

    if (desktop->decoded != nullptr)
    {
        for (u32 index = 0; index < desktop->count; index++)
        {
            FreeDecodedSkinSubModel(desktop->decoded[index]);
        }
        std::free(desktop->decoded);
    }

    MemoryDeallocate2_(desktop);
}

void ReadSkin(Skin* skin, Stream* stream)
{
    auto* desktop = reinterpret_cast<DesktopGraphics::Skin*>(skin);
    s32 count = 0;
    stream->ReadS32(&count);
    desktop->count = static_cast<u32>(count);

    desktop->materials = AllocateItems<DesktopGraphics::MaterialResource*>(desktop->count);
    desktop->packets = AllocateItems<u8*>(desktop->count);
    desktop->sizes = AllocateItems<u32>(desktop->count);
    desktop->vertexCounts = AllocateItems<u32>(desktop->count);
    for (u32 index = 0; index < desktop->count; index++)
    {
        u32 id = 0;
        stream->ReadS32(reinterpret_cast<s32*>(&id));
        desktop->materials[index] = reinterpret_cast<DesktopGraphics::MaterialResource*>(g_MaterialTable.Acquire(&id, nullptr));

        s32 size = 0;
        s32 vertexCount = 0;
        stream->ReadS32(&size);
        stream->ReadS32(&vertexCount);
        desktop->sizes[index] = static_cast<u32>(size);
        desktop->vertexCounts[index] = static_cast<u32>(vertexCount);
        desktop->vertexTotal += desktop->vertexCounts[index];
        desktop->packets[index] = ReadPacketBytes(stream, desktop->sizes[index]);
    }

    Log("ReadSkin %p submodels=%u vertexes=%u", desktop, desktop->count, desktop->vertexTotal);
}

// A blend skin's submodels: each one's material and its parts (their shapes' vertex data), and how many shapes they blend
BlendSkin* NewBlendSkin(u32 id)
{
    auto* skin = static_cast<DesktopGraphics::BlendSkin*>(MemoryAllocate(sizeof(DesktopGraphics::BlendSkin)));
    if (skin == nullptr)
    {
        return nullptr;
    }

    std::memset(skin, 0, sizeof(DesktopGraphics::BlendSkin));
    ConstructResourceHeader(skin, id);
    return reinterpret_cast<BlendSkin*>(skin);
}

void DeleteBlendSkin(BlendSkin* skin)
{
    auto* desktop = reinterpret_cast<DesktopGraphics::BlendSkin*>(skin);
    if (desktop == nullptr)
    {
        return;
    }

    for (u32 index = 0; index < desktop->count; index++)
    {
        if (desktop->materials != nullptr && desktop->materials[index] != nullptr)
        {
            g_MaterialTable.ReleaseItem(reinterpret_cast<MaterialResource*>(desktop->materials[index]));
        }

        if (desktop->subModels != nullptr)
        {
            DestroyBlendSubModel(desktop->subModels[index]);
        }
    }

    if (desktop->materials != nullptr)
    {
        MemoryDeallocate2_(desktop->materials);
    }
    if (desktop->subModels != nullptr)
    {
        MemoryDeallocate2_(desktop->subModels);
    }

    MemoryDeallocate2_(desktop);
}

void ReadBlendSkin(BlendSkin* skin, Stream* stream)
{
    auto* desktop = reinterpret_cast<DesktopGraphics::BlendSkin*>(skin);
    s32 count = 0;
    stream->ReadS32(&count);
    desktop->count = static_cast<u32>(count);
    s32 shapeCount = 0;
    stream->ReadS32(&shapeCount);
    desktop->shapeCount = static_cast<u32>(shapeCount);

    desktop->materials = AllocateItems<DesktopGraphics::MaterialResource*>(desktop->count);
    desktop->subModels = AllocateItems<DesktopGraphics::BlendSubModel*>(desktop->count);
    for (u32 index = 0; index < desktop->count; index++)
    {
        s32 partCount = 0;
        stream->ReadS32(&partCount);
        u32 id = 0;
        stream->ReadS32(reinterpret_cast<s32*>(&id));
        desktop->materials[index] = reinterpret_cast<DesktopGraphics::MaterialResource*>(g_MaterialTable.Acquire(&id, nullptr));

        auto* subModel = static_cast<DesktopGraphics::BlendSubModel*>(MemoryAllocate(sizeof(DesktopGraphics::BlendSubModel)));
        if (subModel != nullptr)
        {
            subModel->parts = nullptr;
            subModel->count = static_cast<u32>(partCount);
            ReadBlendParts(subModel, stream, desktop->shapeCount);
        }

        desktop->subModels[index] = subModel;
    }

    Log("ReadBlendSkin %p submodels=%u shapes=%u", desktop, desktop->count, desktop->shapeCount);
}

// The meshes of the scenery (a rigid model too, read the same way as the OGIs' models)
RigidModel* NewMesh(u32 id)
{
    auto* mesh = static_cast<DesktopGraphics::RigidModel*>(MemoryAllocate(sizeof(DesktopGraphics::RigidModel)));
    if (mesh == nullptr)
    {
        return nullptr;
    }

    std::memset(mesh, 0, sizeof(DesktopGraphics::RigidModel));
    ConstructResourceHeader(mesh, id);
    return reinterpret_cast<RigidModel*>(mesh);
}

void DeleteMesh(RigidModel* mesh)
{
    DestroyRigidModel(reinterpret_cast<DesktopGraphics::RigidModel*>(mesh));
}

void ReadMesh(RigidModel* mesh, Stream* stream)
{
    auto* desktop = reinterpret_cast<DesktopGraphics::RigidModel*>(mesh);
    s32 version = 0;
    stream->ReadS32(&version);
    ReadRigidModelData(desktop, stream);

    Log("ReadMesh %p version=%d submodels=%u modelId=%08X", desktop, version, desktop->count, desktop->modelId);
}

// The levels of detail and the skies: the meshes they name, and the squares of the LOD's distances (its file's version says
// whether the distances are the squares already, 0x1002, or the plain distances, 0x1001)
Lod* NewLod(u32 id)
{
    auto* lod = static_cast<DesktopGraphics::Lod*>(MemoryAllocate(sizeof(DesktopGraphics::Lod)));
    if (lod == nullptr)
    {
        return nullptr;
    }

    std::memset(lod, 0, sizeof(DesktopGraphics::Lod));
    ConstructResourceHeader(lod, id);
    return reinterpret_cast<Lod*>(lod);
}

void DeleteLod(Lod* lod)
{
    auto* desktop = reinterpret_cast<DesktopGraphics::Lod*>(lod);
    if (desktop == nullptr)
    {
        return;
    }

    for (u32 index = 0; index < desktop->count; index++)
    {
        if (desktop->meshes != nullptr && desktop->meshes[index] != nullptr)
        {
            g_MeshTable.ReleaseItem(reinterpret_cast<RigidModel*>(desktop->meshes[index]));
        }
    }

    if (desktop->meshes != nullptr)
    {
        MemoryDeallocate2_(desktop->meshes);
    }

    MemoryDeallocate2_(desktop);
}

void ReadLod(Lod* lod, Stream* stream)
{
    auto* desktop = reinterpret_cast<DesktopGraphics::Lod*>(lod);
    constexpr s32 DistancesVersion = 0x1001;
    constexpr s32 SquaresVersion = 0x1002;

    s32 version = 0;
    stream->ReadS32(&version);
    if (version == DistancesVersion)
    {
        u32 count = 0;
        stream->ReadU32(&count);
        desktop->count = static_cast<u8>(count);
        desktop->meshes = AllocateItems<DesktopGraphics::RigidModel*>(desktop->count);

        stream->ReadS32(reinterpret_cast<s32*>(&desktop->nearest));
        desktop->nearest *= desktop->nearest;
        stream->ReadS32(reinterpret_cast<s32*>(&desktop->farthest));
        if (desktop->farthest != 0xFFFFFFFF)
        {
            desktop->farthest *= desktop->farthest;
        }

        for (u32 distance = 0; distance < DesktopGraphics::Lod::MostSwitches; distance++)
        {
            stream->ReadS32(reinterpret_cast<s32*>(&desktop->switchDistances[distance]));
            desktop->switchDistances[distance] *= desktop->switchDistances[distance];
        }

        s32 unused = 0;
        stream->ReadS32(&unused);
        ReadMeshIds(desktop->meshes, desktop->count, stream);
    }
    else if (version == SquaresVersion)
    {
        s8 count = 0;
        stream->ReadS8(&count);
        desktop->count = static_cast<u8>(count);
        desktop->meshes = AllocateItems<DesktopGraphics::RigidModel*>(desktop->count);

        stream->ReadS32(reinterpret_cast<s32*>(&desktop->nearest));
        stream->ReadS32(reinterpret_cast<s32*>(&desktop->farthest));
        stream->Read(desktop->switchDistances, sizeof(desktop->switchDistances), 1);
        ReadMeshIds(desktop->meshes, desktop->count, stream);
    }

    Log("ReadLod %p version=%d meshes=%u nearest=%u farthest=%u", desktop, version, desktop->count, desktop->nearest,
        desktop->farthest);
}

Sky* NewSky(u32 id)
{
    auto* sky = static_cast<DesktopGraphics::Sky*>(MemoryAllocate(sizeof(DesktopGraphics::Sky)));
    if (sky == nullptr)
    {
        return nullptr;
    }

    std::memset(sky, 0, sizeof(DesktopGraphics::Sky));
    ConstructResourceHeader(sky, id);
    return reinterpret_cast<Sky*>(sky);
}

void DeleteSky(Sky* sky)
{
    auto* desktop = reinterpret_cast<DesktopGraphics::Sky*>(sky);
    if (desktop == nullptr)
    {
        return;
    }

    for (u32 index = 0; index < desktop->count; index++)
    {
        if (desktop->models != nullptr && desktop->models[index] != nullptr)
        {
            g_MeshTable.ReleaseItem(reinterpret_cast<RigidModel*>(desktop->models[index]));
        }
    }

    if (desktop->models != nullptr)
    {
        MemoryDeallocate2_(desktop->models);
    }

    MemoryDeallocate2_(desktop);
}

void ReadSky(Sky* sky, Stream* stream)
{
    auto* desktop = reinterpret_cast<DesktopGraphics::Sky*>(sky);
    s32 unused = 0;
    stream->ReadS32(&unused);
    s32 count = 0;
    stream->ReadS32(&count);
    desktop->count = static_cast<u32>(count);
    desktop->models = AllocateItems<DesktopGraphics::RigidModel*>(desktop->count);
    ReadMeshIds(desktop->models, desktop->count, stream);

    Log("ReadSky %p meshes=%u", desktop, desktop->count);
}

void DrawChunkSky(Sky* sky, RenderView* view)
{
    // A chunk with a sky doesn't set its frame up through SetUpFrame (on the PS2 the sky's own half-size buffer replaces that
    // setup, renderer/sky.cpp): this is where the frame is cleared. Without it nothing clears between frames and each one smears
    // over the last
    SdlGfx::Clear(g_ClearColor[0], g_ClearColor[1], g_ClearColor[2]);

    auto* desktop = reinterpret_cast<DesktopGraphics::Sky*>(sky);
    if (desktop == nullptr || desktop->models == nullptr || view == nullptr)
    {
        return;
    }

    // The sky turns with the camera but doesn't move with it: its meshes are drawn as though the camera sat at the world's origin,
    // so the camera's place goes in front of the view's own matrix to the screen (the PS2's DrawSky composes it the same way)
    Matrix4x4 translation;
    InitIdentityMatrix(&translation);
    Reference* reference = view->cameraObject;
    ObjectPlace* place = reference != nullptr && reference->object != nullptr ? reference->object->place : nullptr;
    if (place != nullptr)
    {
        translation.m[3][0] = place->matrix.m[3][0];
        translation.m[3][1] = place->matrix.m[3][1];
        translation.m[3][2] = place->matrix.m[3][2];
        translation.m[3][3] = place->matrix.m[3][3];
    }

    Matrix4x4 toScreen;
    VuMultiplyMatrices(&translation, &view->toScreen, &toScreen);

    for (u32 index = 0; index < desktop->count; index++)
    {
        if (desktop->models[index] != nullptr)
        {
            // The background never writes depth: the scene drawn after it comes over the top
            DrawRigidModelToScreen(reinterpret_cast<RigidModel*>(desktop->models[index]), &toScreen, false);
        }
    }
}

void LoadParticlePage(ParticlePage* page, const char* path, bool decals)
{
}

void ReadParticlePage(ParticlePage* page, Stream* stream, bool decals)
{
}

}

// A font's glyphs drawn (its vtable's slots, the PS2 side's renderer/text.cpp): each glyph a quad of its page's texture over the
// text's colour, laid out as the PS2's WriteTextPackets does (the layout's extents sixteenths of the screen's pixels, the text's
// place fractions of it, its glyphs from the page's texture flipped)
namespace
{
// How many texels one of a glyph's units is (its coordinates and sizes are sixteenths)
constexpr f32 kGlyphTexelScale = 0.0625f;

// The character's glyph (as font.cpp's GlyphIndex), the font's first when the character isn't one of its
u32 FontGlyphIndex(const Font* font, u32 character)
{
    s32 index = static_cast<s32>(character & 0xFF) - font->firstCharacter;
    if (index < 0 || index >= font->glyphCount)
    {
        return 0;
    }

    return static_cast<u32>(index);
}

// A glyph's page: its index in the font's pages, in the low bits of its first byte (0 or 1 the first page, 2 the second...)
u8 GlyphPageIndexFromMask(u8 mask)
{
    if (mask == 0 || mask == 1)
    {
        return 0;
    }

    return static_cast<u8>(mask - 1);
}

// A font page's texture: its material's, falling back to the texture the font read for it
DesktopGraphics::Texture* FontPageTexture(const Font* font, s32 page)
{
    if (page < 0 || page >= static_cast<s32>(Font::MaxPages))
    {
        return nullptr;
    }

    MaterialResource* material = font->materials[page];
    if (material != nullptr && material->material != nullptr)
    {
        DesktopGraphics::Texture* texture =
            MaterialTexture(reinterpret_cast<DesktopGraphics::Material*>(material->material));
        if (texture != nullptr && texture->sdlTexture != nullptr)
        {
            return texture;
        }
    }

    GameTexture* texture = font->textures[page];
    return texture != nullptr ? &reinterpret_cast<DesktopGraphics::GameTexture*>(texture)->texture : nullptr;
}
} // namespace

void Font::Begin()
{
    // The PS2 batches the frame's texts into packets; the desktop draws each as it's given
}

void Font::Draw(const TextItem* item, TextPackets* /*packets*/)
{
    if (item == nullptr || item->text == nullptr || glyphs == nullptr || glyphCount <= 0 || pageCount <= 0)
    {
        return;
    }

    Vector2 scale;
    FontScale(this, &scale);
    Vector2 size;
    size.x = item->scale.x * scale.x;
    size.y = item->scale.y * scale.y;
    TextLayout layout;
    std::memset(&layout, 0, sizeof(layout));
    RenderText(this, item->text, &size, &layout);

    s32 windowWidth = 0;
    s32 windowHeight = 0;
    SdlGfx::GetSize(&windowWidth, &windowHeight);
    if (windowWidth <= 0 || windowHeight <= 0)
    {
        return;
    }

    // The layout's units are sixteenths of the renderer's pixels (as the PS2's text.cpp takes its places)
    const f32 rw = static_cast<f32>(g_RendererWidth != 0 ? g_RendererWidth : 512) * 16.0f;
    const f32 rh = static_cast<f32>(g_RendererHeight != 0 ? g_RendererHeight : 512) * 16.0f;

    // The text's place (fractions of the screen), the alignment about it
    Vector2 cursor = item->position;
    TextAlignment alignment = item->alignment;
    if (alignment.top == 0)
    {
        cursor.y -= alignment.bottom != 0 ? layout.height / rh : layout.height / rh * 0.5f;
    }

    u8 red = ScaleChannel(static_cast<u8>(item->colour & 0xFF), 192);
    u8 green = ScaleChannel(static_cast<u8>((item->colour >> 8) & 0xFF), 192);
    u8 blue = ScaleChannel(static_cast<u8>((item->colour >> 16) & 0xFF), 192);
    u8 alpha = ScaleChannel(static_cast<u8>((item->colour >> 24) & 0xFF), 128);

    s32 first = 0;
    for (s32 line = 0; line < layout.lineCount; line++)
    {
        f32 extentX = layout.lines[line].x / rw;
        f32 extentY = layout.lines[line].y / rh;
        f32 start = cursor.x;
        if (alignment.left == 0)
        {
            start -= alignment.right != 0 ? extentX : extentX * 0.5f;
        }

        f32 pen = start;
        for (s32 at = 0; at < layout.counts[line]; at++)
        {
            const Vector4& glyph = glyphs[layout.glyphs[first + at]];
            f32 advance = glyph.z * size.x / rw;
            u8 mask = *reinterpret_cast<const u8*>(&glyph.x) & 3u;
            u32 page = GlyphPageIndexFromMask(mask);
            if (page >= static_cast<u32>(pageCount))
            {
                page = 0;
            }

            DesktopGraphics::Texture* tex = FontPageTexture(this, static_cast<s32>(page));
            if (tex == nullptr || tex->sdlTexture == nullptr)
            {
                pen += advance;
                continue;
            }

            // The glyph's page: its left texel and bottom row (its page bits cleared), its size in texels
            u32 bits = *reinterpret_cast<const u32*>(&glyph.x) & ~3u;
            f32 texelLeft = *reinterpret_cast<const f32*>(&bits) * kGlyphTexelScale;
            f32 texelBottom = glyph.y * kGlyphTexelScale;
            f32 texelWidth = glyph.z * kGlyphTexelScale;
            f32 texelHeight = glyph.w * kGlyphTexelScale;

            f32 height = glyph.w * size.y / rh;
            s32 srcX = static_cast<s32>(std::floor(texelLeft));
            s32 srcY = static_cast<s32>(std::floor(texelBottom - texelHeight));
            s32 srcRight = static_cast<s32>(std::ceil(texelLeft + texelWidth));
            s32 srcBottom = static_cast<s32>(std::ceil(texelBottom));
            s32 left = static_cast<s32>(std::floor(pen * static_cast<f32>(windowWidth)));
            s32 top = static_cast<s32>(std::floor(cursor.y * static_cast<f32>(windowHeight)));
            s32 right = static_cast<s32>(std::ceil((pen + advance) * static_cast<f32>(windowWidth)));
            s32 bottom = static_cast<s32>(std::ceil((cursor.y + height) * static_cast<f32>(windowHeight)));
            if (right > left && bottom > top && srcRight > srcX && srcBottom > srcY)
            {
                SdlGfx::DrawTextureFlipped(tex->sdlTexture, left, top, right - left, bottom - top, srcX, srcY, srcRight - srcX,
                                           srcBottom - srcY, red, green, blue, alpha);
            }

            pen += advance;
        }

        first += layout.counts[line];
        cursor.y += extentY;
    }
}

void Font::End(TextPackets* /*packets*/)
{
    // The PS2 sends its packets for the other pages here; the desktop drew each text's glyphs directly
}

// The shadows' models and meshes, which the shadow code makes and reads itself
extern "C"
{
    RigidModelData* ModelConstruct(void* memory, u32 id) RETAIL(InitGameModel);
    void ModelRead(RigidModelData* model, Stream* stream) RETAIL(ReadGameModel);
    RigidModel* MeshConstruct(void* memory, u32 id) RETAIL(FUN_001c1e70);
    void MeshRead(RigidModel* mesh, Stream* stream) RETAIL(ReadRigidModel2);
}

RigidModelData* ModelConstruct(void* memory, u32 id)
{
    ConstructResourceHeader(memory, id);
    return static_cast<RigidModelData*>(memory);
}

void ModelRead(RigidModelData*, Stream*)
{
}

RigidModel* MeshConstruct(void* memory, u32 id)
{
    ConstructResourceHeader(memory, id);
    return static_cast<RigidModel*>(memory);
}

void MeshRead(RigidModel*, Stream*)
{
}

// The PS2 renderer's modules' static constructors, which the retail list of them (G_UnkFunTableSize's) names: the renderer's
// makes the game's statics it holds (no particle section's file, the screens' offsets none, the models' update rate), the VU
// programs' have nothing to make here
extern "C"
{
    void RendererStaticInit() RETAIL(FUN_001a6678);
    void InitVuProgramsModule() RETAIL(FUN_001dd4d0);
    void ConstructVu0ProgramsModule() RETAIL(FUN_002b2230);
}

void RendererStaticInit()
{
    g_ParticleSectionFile = -1;
    g_PalScreenOffset.x = 0.0f;
    g_PalScreenOffset.y = 0.0f;
    g_NtscScreenOffset.x = 0.0f;
    g_NtscScreenOffset.y = 0.0f;
    // The model nodes are never left without updates
    g_ModelUpdateRate.cutoff = UpdateRate::NoCutoff;
    g_ModelUpdateRate.slope = 1.0f;
    g_ModelUpdateRate.grace = 0;
}

void InitVuProgramsModule()
{
}

void ConstructVu0ProgramsModule()
{
}