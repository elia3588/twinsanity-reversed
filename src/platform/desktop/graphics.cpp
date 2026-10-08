#include "platform/graphics.h"

#include "clock.h"
#include "window.h"
#include "graphics_internal.h"
#include "gs_texture.h"
#include "sdl_gfx.h"

#include "game/context.h"
#include "game/font.h"
#include "game/particles.h"
#include "game/renderer.h"
#include "game/resources.h"
#include "game/graphicstables.h"
#include "game/memory.h"
#include "game/shaderanimation.h"
#include "game/stream.h"

#include <algorithm>
#include <chrono>
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
u32 g_DrawsInFrame = 0;
bool g_LogDraws = false;
u32 g_DrawLogLines = 0;
bool g_LogStripVertexes = false;
// The screen the game draws in, its pixel size (the shapes' places are pixels of it)
s32 g_ScreenWidthPx = 512;
s32 g_ScreenHeightPx = 512;

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
}

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

void SetUpRenderTarget(RenderTargetDescription* target)
{
}

// The frame's end: the window's next refresh waited for, then what was drawn shown. The frame was cleared at its start
void Present(const void* commands)
{
    static u32 presented = 0;
    presented++;
    g_LogDraws = presented <= 3 || (presented >= 47 && presented <= 50) || (presented >= 100 && presented <= 103) ||
                 (presented >= 180 && presented <= 183);
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    g_LogStripVertexes = presented == 180;
    if (presented <= 3000)
    {
        Log("Present %u t=%lld draws=%u", presented, static_cast<long long>(milliseconds), g_DrawsInFrame);
    }

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
    static u32 frames = 0;
    if (frames++ < 5)
    {
        Log("StartFrame %u clear=%d rgb=%u,%u,%u at %d,%d %dx%d of %dx%d", frames, frame.clearColor != 0, frame.red, frame.green,
            frame.blue, frame.x, frame.y, frame.width, frame.height, frame.screenWidth, frame.screenHeight);
    }

    g_DrawsInFrame = 0;
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

void DrawPlacedModel(RigidModel* model, ChunkView* view, const Matrix4x4* world)
{
}

RigidModel* LodModelAt(Lod* lod, u32 distance)
{
    return nullptr;
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
    g_DrawsInFrame++;
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

    if (g_LogDraws && g_DrawLogLines < 700)
    {
        g_DrawLogLines++;
        Log("draw f=%u mat=%p tex=%p %ux%u place %.4f,%.4f,%.4f,%.4f -> %d,%d %dx%d src %.4f,%.4f,%.4f,%.4f colour %08X",
            g_DrawsInFrame, material, tex, tex != nullptr ? tex->width : 0, tex != nullptr ? tex->height : 0, p.x, p.y,
            p.width, p.height, screenX, screenY, screenWidth, screenHeight, texturePlace.x, texturePlace.y,
            texturePlace.width, texturePlace.height, colour);
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
    static u32 turned = 0;
    if (turned++ < 100)
    {
        Log("turned#%u corners %.4f,%.4f %.4f,%.4f %.4f,%.4f %.4f,%.4f place %.4f,%.4f,%.4f,%.4f", turned, corners[0].x,
            corners[0].y, corners[1].x, corners[1].y, corners[2].x, corners[2].y, corners[3].x, corners[3].y, place.x, place.y,
            place.width, place.height);
    }

    DrawSprite(material, colour, place, texture);
}

void DrawStrip(Material* material, u32 count, const Vector2* places, const u32* colours)
{
    g_DrawsInFrame++;
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

    if (g_LogStripVertexes && g_DrawLogLines < 700)
    {
        g_DrawLogLines++;
        Log("strip n=%u places %.2f,%.2f..%.2f,%.2f -> %.1f,%.1f..%.1f,%.1f first colour %08X", count, places[0].x, places[0].y,
            places[count - 1].x, places[count - 1].y, vertices[0].x, vertices[0].y, vertices[count - 1].x, vertices[count - 1].y,
            colours[0]);
    }

    auto* desktop = reinterpret_cast<DesktopGraphics::Material*>(material);
    DesktopGraphics::Texture* tex = MaterialTexture(desktop);
    SdlGfx::DrawTriangleStrip(tex != nullptr ? tex->sdlTexture : nullptr, vertices, count);
    MemoryDeallocate2_(vertices);
}

void DrawRigidModel(RigidModel* model, const Matrix4x4* matrix, const ModelLights& lights, u32 mode)
{
}

void DrawSkin(Skin* skin, const Matrix4x4* joints, u32 jointCount, const Matrix4x4* matrix, const ModelLights& lights, u32 mode)
{
}

void DrawBlendSkin(BlendSkin* skin, const Matrix4x4* joints, u32 jointCount, const Matrix4x4* matrix, const ModelLights& lights, const f32* weights, const s32* shapes, s32 shapeCount, u32 mode)
{
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
}

void RestartAnimations(Skin* skin)
{
}

void RestartAnimations(BlendSkin* skin)
{
}

MaterialResource* FirstMaterial(const RigidModel* model)
{
    return nullptr;
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

// The models and meshes the desktop doesn't draw yet (its 3D) still need their place in the game's tables: their kinds' Item
// types are complete only in the platform's renderer, so their bytes are the PS2's (renderer.h), their headers at the front and
// the rest zeroed until the file's bytes are read
RigidModelData* NewModel(u32 id)
{
    void* memory = MemoryAllocate(0x1C);
    if (memory == nullptr)
    {
        return nullptr;
    }

    std::memset(memory, 0, 0x1C);
    ConstructResourceHeader(memory, id);
    return static_cast<RigidModelData*>(memory);
}

void DeleteModel(RigidModelData* model)
{
    if (model != nullptr)
    {
        MemoryDeallocate2_(model);
    }
}

void ReadModel(RigidModelData* model, Stream* stream)
{
}

RigidModel* NewRigidModel(u32 id)
{
    void* memory = MemoryAllocate(0x20);
    if (memory == nullptr)
    {
        return nullptr;
    }

    std::memset(memory, 0, 0x20);
    ConstructResourceHeader(memory, id);
    return static_cast<RigidModel*>(memory);
}

void DeleteRigidModel(RigidModel* model)
{
    if (model != nullptr)
    {
        MemoryDeallocate2_(model);
    }
}

void ReadRigidModel(RigidModel* model, Stream* stream)
{
}

Skin* NewSkin(u32 id)
{
    // The skins' are the platform's too (their kind's Item type is complete only there), sized as the PS2's (renderer.h's 0x24)
    void* memory = MemoryAllocate(0x24);
    if (memory == nullptr)
    {
        return nullptr;
    }

    std::memset(memory, 0, 0x24);
    ConstructResourceHeader(memory, id);
    return static_cast<Skin*>(memory);
}

void DeleteSkin(Skin* skin)
{
    if (skin != nullptr)
    {
        MemoryDeallocate2_(skin);
    }
}

void ReadSkin(Skin* skin, Stream* stream)
{
}

BlendSkin* NewBlendSkin(u32 id)
{
    void* memory = MemoryAllocate(0x18);
    if (memory == nullptr)
    {
        return nullptr;
    }

    std::memset(memory, 0, 0x18);
    ConstructResourceHeader(memory, id);
    return static_cast<BlendSkin*>(memory);
}

void DeleteBlendSkin(BlendSkin* skin)
{
    if (skin != nullptr)
    {
        MemoryDeallocate2_(skin);
    }
}

void ReadBlendSkin(BlendSkin* skin, Stream* stream)
{
}

RigidModel* NewMesh(u32 id)
{
    void* memory = MemoryAllocate(0x20);
    if (memory == nullptr)
    {
        return nullptr;
    }

    std::memset(memory, 0, 0x20);
    ConstructResourceHeader(memory, id);
    return static_cast<RigidModel*>(memory);
}

void DeleteMesh(RigidModel* mesh)
{
    if (mesh != nullptr)
    {
        MemoryDeallocate2_(mesh);
    }
}

void ReadMesh(RigidModel* mesh, Stream* stream)
{
}

// The levels of detail and the skies are the platform's (their kinds' Item types are complete only there), and their sizes the
// PS2's (renderer.h's 0x24 and 0x50 bytes): their headers at the front, the rest zeroed
constexpr u32 LodBytes = 0x24;
constexpr u32 SkyBytes = 0x50;

Lod* NewLod(u32 id)
{
    void* memory = MemoryAllocate(LodBytes);
    if (memory == nullptr)
    {
        return nullptr;
    }

    std::memset(memory, 0, LodBytes);
    ConstructResourceHeader(memory, id);
    return static_cast<Lod*>(memory);
}

void DeleteLod(Lod* lod)
{
    if (lod != nullptr)
    {
        MemoryDeallocate2_(lod);
    }
}

void ReadLod(Lod* lod, Stream* stream)
{
}

Sky* NewSky(u32 id)
{
    void* memory = MemoryAllocate(SkyBytes);
    if (memory == nullptr)
    {
        return nullptr;
    }

    std::memset(memory, 0, SkyBytes);
    ConstructResourceHeader(memory, id);
    return static_cast<Sky*>(memory);
}

void DeleteSky(Sky* sky)
{
    if (sky != nullptr)
    {
        MemoryDeallocate2_(sky);
    }
}

void ReadSky(Sky* sky, Stream* stream)
{
}

void DrawChunkSky(Sky* sky, RenderView* view)
{
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