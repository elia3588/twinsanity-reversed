#pragma once

#include "common.h"

// The desktop's window (SDL2): opened when the renderer starts, its events handled at every vertical blank (whether it was
// closed) and every frame presented in it through OpenGL, so far as its clear color, and closed when the program exits (before
// the exit: the GL drivers' own exit handlers let go of what would still be current). Built without SDL2 there's none, and these
// do nothing. Its file is built for the host's own layouts (without -malign-double, SDL's structs as the SDL library has them),
// so it uses none of the game's. No SDL type leaves here: the rest of the platform layer, built as the game's own layouts, only
// calls these plain functions
namespace DesktopWindow
{
void Open();
bool HandleEvents();
// The buttons held on the desktop's own input (keyboard and the first game controller), as the PS2 pad's button word
// (g_TestPadButtons: its bits, pressed 1). Read after HandleEvents has polled the events
u32 PadButtons();
void Show(u8 red, u8 green, u8 blue);
void Close();
void Present();
// The window's drawing size in pixels (the drawable the frame is drawn in)
void GetSize(s32* width, s32* height);
}
