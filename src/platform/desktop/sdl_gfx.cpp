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
    // The depth runs the same way as the 3D's own (0 nearest, 1 farthest): a vertex z of 0 lands on the frame's depth 0 and one
    // of 1 on its 1, so the depth test keeps the nearest surface. With the usual (-1, 1) range the two would be the other way
    // about and the far side of everything would be drawn (the scene looks inside out and see through)
    glOrtho(0.0, static_cast<double>(g_Width), static_cast<double>(g_Height), 0.0, 0.0, -1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    // The 2D always draws with every sample: a 3D draw's material may have left its alpha test on (the 3D drawers set their own
    // again after Begin)
    glDisable(GL_ALPHA_TEST);
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

void BindTexture(const Texture* texture, bool repeat = false)
{
    if (texture != nullptr)
    {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, texture->id);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        const GLint wrap = repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    }
    else
    {
        glDisable(GL_TEXTURE_2D);
    }
}

// The material the 3D drawers last set (SetMaterial3D), the plain state until one does
Material3D g_Material3D = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0};

// The GS's ATST as the host's alpha function
GLenum AlphaFunc3D(u32 method)
{
    switch (method)
    {
    case 0:
        return GL_NEVER;
    case 1:
        return GL_ALWAYS;
    case 2:
        return GL_LESS;
    case 3:
        return GL_LEQUAL;
    case 4:
        return GL_EQUAL;
    case 5:
        return GL_GEQUAL;
    case 6:
        return GL_GREATER;
    case 7:
        return GL_NOTEQUAL;
    }

    return GL_ALWAYS;
}

// The GS's ZTST as the host's: the GS's depth runs the other way about (its 0 is the far plane, the window's 0 the near one).
// GS depth test methods (reverse depth): 0=NEVER, 1=ALWAYS, 2=LESS (farther), 3=LEQUAL (farther or equal),
// 4=EQUAL, 5=NOTEQUAL, 6=GREATER (closer), 7=GEQUAL (closer or equal).
// The game uses method 2 (GS LESS = farther) for opaque objects but expects standard LESS (closer) behavior.
// Map pragmatically for what the game actually uses.
GLenum DepthFunc3D(u32 method)
{
    switch (method)
    {
    case 0:
        return GL_NEVER;
    case 1:
        return GL_ALWAYS;
    case 2:   // GS LESS (farther in reverse) but game uses for opaque -> standard LESS (closer)
        return GL_LESS;
    case 3:   // GS LEQUAL (farther or equal) -> standard LEQUAL
        return GL_LEQUAL;
    case 4:
        return GL_EQUAL;
    case 5:
        return GL_NOTEQUAL;
    case 6:   // GS GREATER (closer in reverse) -> standard GREATER (farther)
        return GL_GREATER;
    case 7:   // GS GEQUAL (closer or equal in reverse) -> standard GEQUAL
        return GL_GEQUAL;
    }

    return GL_LEQUAL;
}

// The blend the material's preset asks for: the GS ALPHA formulas InitAlphaPresets writes, as the host's source and destination
// factors (its own formula is rare and falls back to the mix)
void ApplyBlend3D()
{
    if (g_Material3D.ownAlpha != 0)
    {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        return;
    }

    switch (g_Material3D.preset)
    {
    case 1:   // add: source·sourceAlpha + destination
    case 3:   // brighten (as close as the host's factors reach)
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        return;
    case 4:   // darken: destination·(1 - sourceAlpha)
        glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
        return;
    case 5:   // scale: destination·sourceAlpha
        glBlendFunc(GL_ZERO, GL_SRC_ALPHA);
        return;
    }

    // The mix, and everything else (the presets past the table's own: the source as it is)
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

// The material's own state over the 2D one Begin leaves: whether it blends and how, its alpha test, and its depth test and write
void ApplyMaterial3D(bool writeDepth)
{
    if (g_Material3D.blends != 0)
    {
        glEnable(GL_BLEND);
        ApplyBlend3D();
    }
    else
    {
        glDisable(GL_BLEND);
    }

    if (g_Material3D.alphaTest != 0)
    {
        // The GS's alpha and its reference are the game's convention, full at 128 (the window's samples full at 1)
        GLclampf reference = static_cast<GLclampf>(g_Material3D.alphaReference) / 128.0f;
        if (reference > 1.0f)
        {
            reference = 1.0f;
        }

        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(AlphaFunc3D(g_Material3D.alphaMethod), reference);
    }
    else
    {
        glDisable(GL_ALPHA_TEST);
    }

    // Depth test: method 1 (GS_ZBUFF_ALWAYS) means no depth test at all
    if (g_Material3D.depthTest == 1)
    {
        glDisable(GL_DEPTH_TEST);
    }
    else
    {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(DepthFunc3D(g_Material3D.depthTest));
    }
    glDepthMask(writeDepth && g_Material3D.noDepthWrites == 0 ? GL_TRUE : GL_FALSE);
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

void Clear(u8 r, u8 g, u8 b, bool clearDepth)
{
    if (!Ready())
    {
        return;
    }

    Begin();
    glDisable(GL_TEXTURE_2D);
    glClearColor(static_cast<GLclampf>(r) / 255.0f, static_cast<GLclampf>(g) / 255.0f, static_cast<GLclampf>(b) / 255.0f, 1.0f);
    GLbitfield mask = GL_COLOR_BUFFER_BIT;
    if (clearDepth)
    {
        mask |= GL_DEPTH_BUFFER_BIT;
    }
    glClear(mask);
}

// Clear depth buffer to far plane (1.0) - used after sky to fill gaps where sky meshes don't cover
void ClearDepthToFar()
{
    if (!Ready())
    {
        return;
    }

    Begin();
    glDisable(GL_TEXTURE_2D);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glClearDepth(1.0f);
    glClear(GL_DEPTH_BUFFER_BIT);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
}

// Draw a full-screen quad at the far plane with the given color (for sky horizon fill)
void DrawFullScreenQuad(u8 r, u8 g, u8 b)
{
    if (!Ready())
    {
        return;
    }

    Begin();
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glColor4ub(r, g, b, 255);

    // Full-screen quad in NDC space (z=1.0 = far plane)
    glBegin(GL_QUADS);
    glVertex3f(-1.0f, -1.0f, 1.0f);
    glVertex3f( 1.0f, -1.0f, 1.0f);
    glVertex3f( 1.0f,  1.0f, 1.0f);
    glVertex3f(-1.0f,  1.0f, 1.0f);
    glEnd();

    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
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

void SetMaterial3D(const Material3D& material)
{
    g_Material3D = material;
}

void DrawTriangleStrip3D(void* handle, const Vertex3D* vertices, u32 count, bool writeDepth, const u8* noDraw)
{
    if (!Ready() || vertices == nullptr || count < 3)
    {
        return;
    }

    Begin();
    // The 2D's ortho projection keeps the window's pixels; the material's own blending, alpha test and depth test then go over
    // the 2D state Begin leaves (z 0 nearest, 1 farthest)
    ApplyMaterial3D(writeDepth);
    BindTexture(static_cast<const Texture*>(handle), true);
    // A vertex whose no-draw flag is set takes no triangle: the triangle it would have closed with the two before it is left out,
    // the strip's own winding kept exactly (GL_TRIANGLES emits each kept triangle with the same vertex order)
    if (noDraw != nullptr)
    {
        glBegin(GL_TRIANGLES);
        for (u32 index = 2; index < count; index++)
        {
            if (noDraw[index] != 0)
            {
                continue;
            }

            for (u32 corner = 0; corner < 3; corner++)
            {
                const Vertex3D& vertex = vertices[index - 2 + corner];
                glColor4ub(vertex.red, vertex.green, vertex.blue, vertex.alpha);
                glTexCoord2f(vertex.u, vertex.v);
                glVertex3f(vertex.x, vertex.y, vertex.z);
            }
        }
        glEnd();
    }
    else
    {
        // The PS2 GS discards degenerate triangles (where two vertices coincide). GL renders them,
        // so we skip vertices that would form a degenerate triangle with the previous two.
        glBegin(GL_TRIANGLE_STRIP);
        f32 prevX = 0.0f, prevY = 0.0f, prevZ = 0.0f;
        f32 prev2X = 0.0f, prev2Y = 0.0f, prev2Z = 0.0f;
        u32 emitted = 0;
        constexpr f32 DegenerateEps = 0.01f;   // sub-pixel tolerance
        for (u32 vertex = 0; vertex < count; vertex++)
        {
            const Vertex3D& v = vertices[vertex];
            bool degenerate = false;
            if (emitted >= 2)
            {
                // Check if this vertex forms a degenerate triangle with the previous two
                f32 dx1 = v.x - prevX;
                f32 dy1 = v.y - prevY;
                f32 dz1 = v.z - prevZ;
                f32 dx2 = prevX - prev2X;
                f32 dy2 = prevY - prev2Y;
                f32 dz2 = prevZ - prev2Z;
                // Degenerate if any edge is near-zero (two vertices coincide)
                if ((dx1 * dx1 + dy1 * dy1 + dz1 * dz1) < DegenerateEps ||
                    (dx2 * dx2 + dy2 * dy2 + dz2 * dz2) < DegenerateEps ||
                    (v.x - prev2X) * (v.x - prev2X) + (v.y - prev2Y) * (v.y - prev2Y) + (v.z - prev2Z) * (v.z - prev2Z) < DegenerateEps)
                {
                    degenerate = true;
                }
            }

            if (!degenerate)
            {
                glColor4ub(v.red, v.green, v.blue, v.alpha);
                glTexCoord2f(v.u, v.v);
                glVertex3f(v.x, v.y, v.z);
                prev2X = prevX; prev2Y = prevY; prev2Z = prevZ;
                prevX = v.x; prevY = v.y; prevZ = v.z;
                emitted++;
            }
        }
        glEnd();
    }
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
void DrawTriangleStrip3D(void*, const Vertex3D*, u32, bool, const u8*) {}
void DrawTexture(void*, int, int, int, int, int, int, int, int, u8, u8, u8, u8) {}
void DrawTextureFlipped(void*, int, int, int, int, int, int, int, int, u8, u8, u8, u8) {}
}
#endif
