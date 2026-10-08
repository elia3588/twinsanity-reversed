#include "sdl_gfx.h"
#include "window.h"

#if defined(DESKTOP_SDL2)
#include <SDL.h>
#include <cstdio>
#include <cstdlib>

namespace SdlGfx
{
bool Available()
{
    return DesktopWindow::GetRenderer() != nullptr;
}

void GetSize(int* width, int* height)
{
    SDL_Renderer* renderer = DesktopWindow::GetRenderer();
    if (renderer == nullptr || width == nullptr || height == nullptr)
    {
        if (width != nullptr) *width = 0;
        if (height != nullptr) *height = 0;
        return;
    }

    SDL_GetRendererOutputSize(renderer, width, height);
}

void Clear(u8 r, u8 g, u8 b)
{
    SDL_Renderer* renderer = DesktopWindow::GetRenderer();
    if (renderer == nullptr)
    {
        return;
    }

    SDL_SetRenderDrawColor(renderer, r, g, b, 255);
    SDL_RenderClear(renderer);
}

void* CreateTexture(const void* rgba, int width, int height)
{
    SDL_Renderer* renderer = DesktopWindow::GetRenderer();
    if (renderer == nullptr || rgba == nullptr || width <= 0 || height <= 0)
    {
        return nullptr;
    }

    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, width, height);
    if (texture == nullptr)
    {
        return nullptr;
    }

    SDL_UpdateTexture(texture, nullptr, rgba, width * 4);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    return texture;
}

void DestroyTexture(void* texture)
{
    if (texture != nullptr)
    {
        SDL_DestroyTexture(static_cast<SDL_Texture*>(texture));
    }
}

void FillRect(int x, int y, int width, int height, u8 r, u8 g, u8 b, u8 a)
{
    SDL_Renderer* renderer = DesktopWindow::GetRenderer();
    if (renderer == nullptr)
    {
        return;
    }

    SDL_Rect rectangle = {x, y, width, height};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, r, g, b, a);
    SDL_RenderFillRect(renderer, &rectangle);
}

void DrawTriangleStrip(void* texture, const Vertex* vertices, u32 count)
{
    SDL_Renderer* renderer = DesktopWindow::GetRenderer();
    if (renderer == nullptr || vertices == nullptr || count < 3)
    {
        return;
    }

    auto* points = static_cast<SDL_Vertex*>(std::malloc(sizeof(SDL_Vertex) * count));
    auto* order = static_cast<int*>(std::malloc(sizeof(int) * (count - 2) * 3));
    if (points == nullptr || order == nullptr)
    {
        std::free(points);
        std::free(order);
        return;
    }

    for (u32 vertex = 0; vertex < count; vertex++)
    {
        points[vertex].position.x = vertices[vertex].x;
        points[vertex].position.y = vertices[vertex].y;
        points[vertex].color.r = vertices[vertex].red;
        points[vertex].color.g = vertices[vertex].green;
        points[vertex].color.b = vertices[vertex].blue;
        points[vertex].color.a = vertices[vertex].alpha;
        points[vertex].tex_coord.x = vertices[vertex].u;
        points[vertex].tex_coord.y = vertices[vertex].v;
    }

    // Each of the strip's triangles the next three vertexes, the odd ones about faced as a strip turns them
    int triangles = 0;
    for (u32 vertex = 0; vertex + 2 < count; vertex++)
    {
        int* triangle = order + triangles * 3;
        triangle[0] = static_cast<int>(vertex) + ((triangles & 1) != 0 ? 1 : 0);
        triangle[1] = static_cast<int>(vertex) + ((triangles & 1) != 0 ? 0 : 1);
        triangle[2] = static_cast<int>(vertex) + 2;
        triangles++;
    }

    SDL_RenderGeometry(renderer, static_cast<SDL_Texture*>(texture), points, static_cast<int>(count), order, triangles * 3);
    std::free(points);
    std::free(order);
}

void DrawTexture(void* texture, int dstX, int dstY, int dstW, int dstH, int srcX, int srcY, int srcW, int srcH, u8 r, u8 g, u8 b,
                 u8 a)
{    SDL_Renderer* renderer = DesktopWindow::GetRenderer();
    if (renderer == nullptr || texture == nullptr)
    {
        return;
    }

    SDL_Rect destination = {dstX, dstY, dstW, dstH};
    SDL_Rect source = {srcX, srcY, srcW, srcH};
    SDL_SetTextureColorMod(static_cast<SDL_Texture*>(texture), r, g, b);
    SDL_SetTextureAlphaMod(static_cast<SDL_Texture*>(texture), a);
    if (SDL_RenderCopy(renderer, static_cast<SDL_Texture*>(texture), &source, &destination) != 0)
    {
        std::fprintf(stderr, "RenderCopy failed: %s\n", SDL_GetError());
        std::fflush(stderr);
    }
}

void DrawTextureFlipped(void* texture, int dstX, int dstY, int dstW, int dstH, int srcX, int srcY, int srcW, int srcH, u8 r, u8 g,
                        u8 b, u8 a)
{
    SDL_Renderer* renderer = DesktopWindow::GetRenderer();
    if (renderer == nullptr || texture == nullptr)
    {
        return;
    }

    SDL_Rect destination = {dstX, dstY, dstW, dstH};
    SDL_Rect source = {srcX, srcY, srcW, srcH};
    SDL_SetTextureColorMod(static_cast<SDL_Texture*>(texture), r, g, b);
    SDL_SetTextureAlphaMod(static_cast<SDL_Texture*>(texture), a);
    if (SDL_RenderCopyEx(renderer, static_cast<SDL_Texture*>(texture), &source, &destination, 0.0, nullptr, SDL_FLIP_VERTICAL) != 0)
    {
        std::fprintf(stderr, "RenderCopyEx failed: %s\n", SDL_GetError());
        std::fflush(stderr);
    }
}
}
#else
namespace SdlGfx
{
bool Available() { return false; }
void GetSize(int* width, int* height) { if (width) *width = 0; if (height) *height = 0; }
void Clear(u8, u8, u8) {}
void* CreateTexture(const void*, int, int) { return nullptr; }
void DestroyTexture(void*) {}
void FillRect(int, int, int, int, u8, u8, u8, u8) {}
void DrawTriangleStrip(void*, const Vertex*, u32) {}
void DrawTexture(void*, int, int, int, int, int, int, int, int, u8, u8, u8, u8) {}
void DrawTextureFlipped(void*, int, int, int, int, int, int, int, int, u8, u8, u8, u8) {}
}
#endif