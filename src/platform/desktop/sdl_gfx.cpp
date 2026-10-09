#include "sdl_gfx.h"
#include "window.h"

#if defined(DESKTOP_SDL2)
#include <SDL.h>
#include <SDL_opengl.h>
#include <cstdlib>

// Clamp to edge isn't in GL 1.1's header (the SDL one usually has it)
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

// The desktop's 2D drawing through OpenGL (sdl_gfx.h): the window's pixels with its corner at the top left, the game's places
// scaled by DesktopWindow::GetSize. The window itself and the context it draws in are window.cpp's
namespace SdlGfx
{
namespace
{
// A texture the game holds: the GL texture made from the decoded image, and its size (the areas the game gives are in its pixels)
struct Texture
{
    GLuint id;
    s32 width;
    s32 height;
};

// The drawable's size, and whether the window and its context are there
s32 g_Width = 0;
s32 g_Height = 0;

bool Ready()
{
    DesktopWindow::GetSize(&g_Width, &g_Height);
    return g_Width > 0 && g_Height > 0;
}

// The window's pixels laid over the drawing (its corner the top left), textures blended and the depth test off while the 2D is
void Begin()
{
    glViewport(0, 0, g_Width, g_Height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, static_cast<double>(g_Width), static_cast<double>(g_Height), 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

// A rectangle of the window: its corner and size in pixels (the window's corner the top left), the texture's area in its own
// pixels (its corner the top left) and whether that area's rows are taken the other way up within its rectangle. Unbound and
// untextured with no texture
void Quad(s32 x, s32 y, s32 width, s32 height, const Texture* texture, s32 srcX, s32 srcY, s32 srcWidth, s32 srcHeight,
          bool flipped)
{
    f32 left = static_cast<f32>(x);
    f32 top = static_cast<f32>(y);
    f32 right = static_cast<f32>(x + width);
    f32 bottom = static_cast<f32>(y + height);

    // The texture's row 0 (v 0) is its corner at the top (SDL's), the decoded image's row 0 its bottom: the source's top and
    // bottom under the rectangle's top and bottom, their order turned when the rows are
    f32 u0 = 0.0f;
    f32 u1 = 0.0f;
    f32 v0 = 0.0f;
    f32 v1 = 0.0f;
    if (texture != nullptr && texture->width > 0 && texture->height > 0)
    {
        u0 = static_cast<f32>(srcX) / static_cast<f32>(texture->width);
        u1 = static_cast<f32>(srcX + srcWidth) / static_cast<f32>(texture->width);
        f32 sourceTop = static_cast<f32>(srcY) / static_cast<f32>(texture->height);
        f32 sourceBottom = static_cast<f32>(srcY + srcHeight) / static_cast<f32>(texture->height);
        v0 = flipped ? sourceBottom : sourceTop;
        v1 = flipped ? sourceTop : sourceBottom;
    }

    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0);
    glVertex2f(left, top);
    glTexCoord2f(u1, v0);
    glVertex2f(right, top);
    glTexCoord2f(u1, v1);
    glVertex2f(right, bottom);
    glTexCoord2f(u0, v1);
    glVertex2f(left, bottom);
    glEnd();
}

void BindTexture(const Texture* texture)
{
    if (texture != nullptr)
    {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, texture->id);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    }
    else
    {
        glDisable(GL_TEXTURE_2D);
    }
}
}

bool Available()
{
    return Ready();
}

void GetSize(int* width, int* height)
{
    DesktopWindow::GetSize(&g_Width, &g_Height);
    if (width != nullptr)
    {
        *width = g_Width;
    }
    if (height != nullptr)
    {
        *height = g_Height;
    }
}

void Clear(u8 r, u8 g, u8 b)
{
    if (!Ready())
    {
        return;
    }

    Begin();
    glDisable(GL_TEXTURE_2D);
    glClearColor(static_cast<GLclampf>(r) / 255.0f, static_cast<GLclampf>(g) / 255.0f, static_cast<GLclampf>(b) / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void* CreateTexture(const void* rgba, int width, int height)
{
    if (rgba == nullptr || width <= 0 || height <= 0)
    {
        return nullptr;
    }

    GLuint id = 0;
    glGenTextures(1, &id);
    if (id == 0)
    {
        return nullptr;
    }

    glBindTexture(GL_TEXTURE_2D, id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

    auto* texture = static_cast<Texture*>(std::malloc(sizeof(Texture)));
    if (texture == nullptr)
    {
        glDeleteTextures(1, &id);
        return nullptr;
    }

    texture->id = id;
    texture->width = width;
    texture->height = height;
    return texture;
}

void DestroyTexture(void* handle)
{
    auto* texture = static_cast<Texture*>(handle);
    if (texture == nullptr)
    {
        return;
    }

    glDeleteTextures(1, &texture->id);
    std::free(texture);
}

void FillRect(int x, int y, int width, int height, u8 r, u8 g, u8 b, u8 a)
{
    if (!Ready())
    {
        return;
    }

    Begin();
    glDisable(GL_TEXTURE_2D);
    glColor4ub(r, g, b, a);
    Quad(x, y, width, height, nullptr, 0, 0, 0, 0, false);
}

void DrawTriangleStrip(void* handle, const Vertex* vertices, u32 count)
{
    if (!Ready() || vertices == nullptr || count < 3)
    {
        return;
    }

    Begin();
    BindTexture(static_cast<const Texture*>(handle));
    glBegin(GL_TRIANGLE_STRIP);
    for (u32 vertex = 0; vertex < count; vertex++)
    {
        glColor4ub(vertices[vertex].red, vertices[vertex].green, vertices[vertex].blue, vertices[vertex].alpha);
        glTexCoord2f(vertices[vertex].u, vertices[vertex].v);
        glVertex2f(vertices[vertex].x, vertices[vertex].y);
    }
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

void DrawTriangleStrip3D(void* handle, const Vertex3D* vertices, u32 count, bool writeDepth)
{
    if (!Ready() || vertices == nullptr || count < 3)
    {
        return;
    }

    Begin();
    // The 2D's ortho projection keeps the window's pixels; the depth test is what the 3D needs (z 0 nearest, 1 farthest)
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(writeDepth ? GL_TRUE : GL_FALSE);
    BindTexture(static_cast<const Texture*>(handle));
    glBegin(GL_TRIANGLE_STRIP);
    for (u32 vertex = 0; vertex < count; vertex++)
    {
        glColor4ub(vertices[vertex].red, vertices[vertex].green, vertices[vertex].blue, vertices[vertex].alpha);
        glTexCoord2f(vertices[vertex].u, vertices[vertex].v);
        glVertex3f(vertices[vertex].x, vertices[vertex].y, vertices[vertex].z);
    }
    glEnd();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
}

void DrawTexture(void* handle, int dstX, int dstY, int dstW, int dstH, int srcX, int srcY, int srcW, int srcH, u8 r, u8 g, u8 b,
                 u8 a)
{
    if (!Ready() || handle == nullptr)
    {
        return;
    }

    Begin();
    const Texture* texture = static_cast<const Texture*>(handle);
    BindTexture(texture);
    glColor4ub(r, g, b, a);
    Quad(dstX, dstY, dstW, dstH, texture, srcX, srcY, srcW, srcH, false);
    glDisable(GL_TEXTURE_2D);
}

void DrawTextureFlipped(void* handle, int dstX, int dstY, int dstW, int dstH, int srcX, int srcY, int srcW, int srcH, u8 r, u8 g,
                        u8 b, u8 a)
{
    if (!Ready() || handle == nullptr)
    {
        return;
    }

    Begin();
    const Texture* texture = static_cast<const Texture*>(handle);
    BindTexture(texture);
    glColor4ub(r, g, b, a);
    Quad(dstX, dstY, dstW, dstH, texture, srcX, srcY, srcW, srcH, true);
    glDisable(GL_TEXTURE_2D);
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
void DrawTriangleStrip3D(void*, const Vertex3D*, u32) {}
void DrawTexture(void*, int, int, int, int, int, int, int, int, u8, u8, u8, u8) {}
void DrawTextureFlipped(void*, int, int, int, int, int, int, int, int, u8, u8, u8, u8) {}
}
#endif
