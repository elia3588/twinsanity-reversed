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

// The PS2 pad's button word (g_TestPadButtons: its bits, pressed 1), which the desktop's keyboard and controller set
constexpr u32 PadL2 = 0x0001;
constexpr u32 PadR2 = 0x0002;
constexpr u32 PadL1 = 0x0004;
constexpr u32 PadR1 = 0x0008;
constexpr u32 PadTriangle = 0x0010;
constexpr u32 PadCircle = 0x0020;
constexpr u32 PadCross = 0x0040;
constexpr u32 PadSquare = 0x0080;
constexpr u32 PadSelect = 0x0100;
constexpr u32 PadL3 = 0x0200;
constexpr u32 PadR3 = 0x0400;
constexpr u32 PadStart = 0x0800;
constexpr u32 PadUp = 0x1000;
constexpr u32 PadRight = 0x2000;
constexpr u32 PadDown = 0x4000;
constexpr u32 PadLeft = 0x8000;
// How far a stick has to be pushed for a direction to count as held
constexpr s16 StickThreshold = 0x4000;

bool g_Started = false;
SDL_Window* g_Window = nullptr;
SDL_Renderer* g_Renderer = nullptr;
SDL_GameController* g_Controller = nullptr;
u32 g_PadButtons = 0;

// The keyboard's and the controller's buttons held, as the PS2 pad's button word
u32 PollPad()
{
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    u32 held = 0;
    if (keys[SDL_SCANCODE_Z] || keys[SDL_SCANCODE_SPACE])
    {
        held |= PadCross;
    }
    if (keys[SDL_SCANCODE_X])
    {
        held |= PadCircle;
    }
    if (keys[SDL_SCANCODE_C])
    {
        held |= PadSquare;
    }
    if (keys[SDL_SCANCODE_V])
    {
        held |= PadTriangle;
    }
    if (keys[SDL_SCANCODE_Q] || keys[SDL_SCANCODE_1])
    {
        held |= PadL1;
    }
    if (keys[SDL_SCANCODE_2])
    {
        held |= PadL2;
    }
    if (keys[SDL_SCANCODE_E] || keys[SDL_SCANCODE_3])
    {
        held |= PadR1;
    }
    if (keys[SDL_SCANCODE_4])
    {
        held |= PadR2;
    }
    if (keys[SDL_SCANCODE_BACKSPACE] || keys[SDL_SCANCODE_ESCAPE])
    {
        held |= PadSelect;
    }
    if (keys[SDL_SCANCODE_RETURN])
    {
        held |= PadStart;
    }
    if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP])
    {
        held |= PadUp;
    }
    if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN])
    {
        held |= PadDown;
    }
    if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT])
    {
        held |= PadLeft;
    }
    if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT])
    {
        held |= PadRight;
    }

    if (g_Controller != nullptr)
    {
        if (SDL_GameControllerGetButton(g_Controller, SDL_CONTROLLER_BUTTON_A))
        {
            held |= PadCross;
        }
        if (SDL_GameControllerGetButton(g_Controller, SDL_CONTROLLER_BUTTON_B))
        {
            held |= PadCircle;
        }
        if (SDL_GameControllerGetButton(g_Controller, SDL_CONTROLLER_BUTTON_X))
        {
            held |= PadSquare;
        }
        if (SDL_GameControllerGetButton(g_Controller, SDL_CONTROLLER_BUTTON_Y))
        {
            held |= PadTriangle;
        }
        if (SDL_GameControllerGetButton(g_Controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))
        {
            held |= PadL1;
        }
        if (SDL_GameControllerGetButton(g_Controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER))
        {
            held |= PadR1;
        }
        if (SDL_GameControllerGetAxis(g_Controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > StickThreshold)
        {
            held |= PadL2;
        }
        if (SDL_GameControllerGetAxis(g_Controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > StickThreshold)
        {
            held |= PadR2;
        }
        if (SDL_GameControllerGetButton(g_Controller, SDL_CONTROLLER_BUTTON_BACK))
        {
            held |= PadSelect;
        }
        if (SDL_GameControllerGetButton(g_Controller, SDL_CONTROLLER_BUTTON_START))
        {
            held |= PadStart;
        }
        if (SDL_GameControllerGetButton(g_Controller, SDL_CONTROLLER_BUTTON_LEFTSTICK))
        {
            held |= PadL3;
        }
        if (SDL_GameControllerGetButton(g_Controller, SDL_CONTROLLER_BUTTON_RIGHTSTICK))
        {
            held |= PadR3;
        }

        if (SDL_GameControllerGetButton(g_Controller, SDL_CONTROLLER_BUTTON_DPAD_UP) ||
            SDL_GameControllerGetAxis(g_Controller, SDL_CONTROLLER_AXIS_LEFTY) < -StickThreshold)
        {
            held |= PadUp;
        }
        if (SDL_GameControllerGetButton(g_Controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN) ||
            SDL_GameControllerGetAxis(g_Controller, SDL_CONTROLLER_AXIS_LEFTY) > StickThreshold)
        {
            held |= PadDown;
        }
        if (SDL_GameControllerGetButton(g_Controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT) ||
            SDL_GameControllerGetAxis(g_Controller, SDL_CONTROLLER_AXIS_LEFTX) < -StickThreshold)
        {
            held |= PadLeft;
        }
        if (SDL_GameControllerGetButton(g_Controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ||
            SDL_GameControllerGetAxis(g_Controller, SDL_CONTROLLER_AXIS_LEFTX) > StickThreshold)
        {
            held |= PadRight;
        }
    }

    return held;
}
}

// Without a display (or SDL's video) the game goes on without a window
void DesktopWindow::Open()
{
    if (g_Started)
    {
        return;
    }

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0)
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

    // A controller already plugged in (ones plugged in later come through the device events)
    for (s32 device = 0; device < SDL_NumJoysticks(); device++)
    {
        if (SDL_IsGameController(device))
        {
            g_Controller = SDL_GameControllerOpen(device);
            if (g_Controller != nullptr)
            {
                break;
            }
        }
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
        else if (event.type == SDL_CONTROLLERDEVICEADDED)
        {
            if (g_Controller == nullptr)
            {
                g_Controller = SDL_GameControllerOpen(event.cdevice.which);
            }
        }
        else if (event.type == SDL_CONTROLLERDEVICEREMOVED && g_Controller != nullptr &&
                 event.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_Controller)))
        {
            SDL_GameControllerClose(g_Controller);
            g_Controller = nullptr;
        }
    }

    g_PadButtons = PollPad();
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

// The buttons the desktop's own input held at the last HandleEvents
u32 PadButtons()
{
    return g_PadButtons;
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

    if (g_Controller != nullptr)
    {
        SDL_GameControllerClose(g_Controller);
        g_Controller = nullptr;
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

u32 PadButtons()
{
    return 0;
}
}
#endif
