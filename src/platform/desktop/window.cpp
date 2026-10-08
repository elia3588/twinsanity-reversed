#include "window.h"

#if defined(DESKTOP_SDL2)
#include <cstdio>
#include "debug.h"

// The program's own main, not SDL's
#define SDL_MAIN_HANDLED
#include <SDL.h>

namespace
{
// A TV's 4:3 at 720 lines to start with
constexpr s32 WindowHeight = 720;
constexpr s32 WindowWidth = WindowHeight * 4 / 3;

bool g_Started = false;
SDL_Window* g_Window = nullptr;
SDL_Renderer* g_Renderer = nullptr;
}

// Without a display (or SDL's video) the game goes on without a window
void DesktopWindow::Open()
{
    if (g_Started)
    {
        return;
    }

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        std::fprintf(stderr, "No window: %s\n", SDL_GetError());
        return;
    }

    g_Started = true;
    g_Window = SDL_CreateWindow("Crash Twinsanity", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WindowWidth, WindowHeight,
                                SDL_WINDOW_RESIZABLE);
    if (g_Window == nullptr)
    {
        std::fprintf(stderr, "No window: %s\n", SDL_GetError());
        return;
    }

    // The vertical blank is the game's to wait for (Platform::Graphics::WaitVSync), presenting doesn't
    g_Renderer = SDL_CreateRenderer(g_Window, -1, 0);
    if (g_Renderer == nullptr)
    {
        std::fprintf(stderr, "Nothing shown in the window: %s\n", SDL_GetError());
    }
}

bool DesktopWindow::HandleEvents()
{
    bool closed = false;
    SDL_Event event;
    while (SDL_PollEvent(&event) != 0)
    {
        if (event.type == SDL_QUIT)
        {
            closed = true;
        }
    }

    return closed;
}

void DesktopWindow::Show(u8 red, u8 green, u8 blue)
{
    if (g_Renderer == nullptr)
    {
        return;
    }

    SDL_SetRenderDrawColor(g_Renderer, red, green, blue, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(g_Renderer);
    SDL_RenderPresent(g_Renderer);
}

namespace DesktopWindow
{
SDL_Renderer* GetRenderer()
{
    return g_Renderer;
}

void Present()
{
    if (g_Renderer != nullptr)
    {
        SDL_RenderPresent(g_Renderer);
    }
}
}

void DesktopWindow::Close()
{
    if (!g_Started)
    {
        return;
    }

    if (g_Renderer != nullptr)
    {
        SDL_DestroyRenderer(g_Renderer);
        g_Renderer = nullptr;
    }

    if (g_Window != nullptr)
    {
        SDL_DestroyWindow(g_Window);
        g_Window = nullptr;
    }

    SDL_Quit();
    g_Started = false;
}
#else
void DesktopWindow::Open()
{
    if (g_Window != nullptr)
    {
        SDL_RaiseWindow(g_Window);
        SDL_SetWindowInputFocus(g_Window);
    }
}

bool DesktopWindow::HandleEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        if (event.type == SDL_QUIT)
        {
            return true;
        }
        if (event.type == SDL_KEYDOWN)
        {
            switch (event.key.keysym.sym)
            {
            case SDLK_RETURN:
                break;
            case SDLK_SPACE:
            case SDLK_z:
                break;
            case SDLK_x:
                // (1u << 1); // Circle
                break;
            case SDLK_c:
                // (1u << 2); // Square
                break;
            case SDLK_v:
                // (1u << 3); // Triangle
                break;
            case SDLK_ESCAPE:
                // (1u << 1);
                break;
            case SDLK_w:
            case SDLK_UP:
                // (1u << 6);
                break;
            case SDLK_s:
            case SDLK_DOWN:
                // (1u << 7);
                break;
            case SDLK_a:
            case SDLK_LEFT:
                // (1u << 8);
                break;
            case SDLK_d:
            case SDLK_RIGHT:
                // (1u << 9);
                break;
            case SDLK_1:
                // (1u << 10);
                break;
            case SDLK_2:
                // (1u << 11);
                break;
            case SDLK_3:
                // (1u << 12);
                break;
            case SDLK_4:
                // (1u << 13);
                break;
            default:
                break;
            }
        }
        if (event.type == SDL_KEYUP)
        {
            switch (event.key.keysym.sym)
            {
            case SDLK_RETURN:
                // ~(1u << 4);
                break;
            case SDLK_SPACE:
            case SDLK_z:
                // ~(1u << 0);
                break;
            case SDLK_x:
                // ~(1u << 1);
                break;
            case SDLK_c:
                // ~(1u << 2);
                break;
            case SDLK_v:
                // ~(1u << 3);
                break;
            case SDLK_ESCAPE:
                // ~(1u << 1);
                break;
            case SDLK_w:
            case SDLK_UP:
                // ~(1u << 6);
                break;
            case SDLK_s:
            case SDLK_DOWN:
                // ~(1u << 7);
                break;
            case SDLK_a:
            case SDLK_LEFT:
                // ~(1u << 8);
                break;
            case SDLK_d:
            case SDLK_RIGHT:
                // ~(1u << 9);
                break;
            case SDLK_1:
                // ~(1u << 10);
                break;
            case SDLK_2:
                // ~(1u << 11);
                break;
            case SDLK_3:
                // ~(1u << 12);
                break;
            case SDLK_4:
                // ~(1u << 13);
                break;
            default:
                break;
            }
        }
    }
    return false;
}

void DesktopWindow::Show(u8, u8, u8)
{
}

void DesktopWindow::Close()
{
}

namespace DesktopWindow
{
::SDL_Renderer* GetRenderer()
{
    return nullptr;
}

void Present()
{
    // no-op stub
}
}
#endif
