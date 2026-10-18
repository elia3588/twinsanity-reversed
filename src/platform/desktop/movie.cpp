#include "platform/movie.h"

#include "window.h"

// The desktop's side of Platform::Movie: movies (FMVs) are simulated, playing a fixed number of frames so the game's
// transitions through them without hanging. A real decoder (MPEG-2) can replace this later
namespace Platform::Movie
{
namespace
{
// How many frames a simulated movie lasts (about two seconds at 30fps) — long enough for the game to transition
constexpr u32 SimulatedFrames = 60;
// A player's own state: whether it is open and how many frames it has stepped
struct PlayerState
{
    bool open;
    u32 frame;
};
// The players are given by the game in its own memory; their state is kept here by pointer (the game's Player is opaque)
PlayerState g_States[4] = {};
}

void Construct(Player* player)
{
    for (u32 i = 0; i < 4; i++)
    {
        g_States[i] = {false, 0};
    }
}

void BeginPresenting(Player* player, void (*present)())
{
}

bool Open(Player* player, const char* file, u32 audioChannel, s32 width, void* lentMemory)
{
    // Every movie "opens" so the game plays through it (a real decoder would fail on a missing file)
    g_States[0].open = true;
    g_States[0].frame = 0;
    return true;
}

void StartSound(Player* player, f32 volume)
{
}

bool Step(Player* player)
{
    PlayerState* state = &g_States[0];
    if (!state->open)
    {
        return false;
    }

    state->frame++;
    return state->frame < SimulatedFrames;
}

void WaitFrame(Player* player)
{
    // No vertical blank to wait for: the simulated movie steps as fast as it can
}

void QueuePicture(Player* player)
{
}

void Draw(Player* player, const Area& area, s32 width, s32 height, u32 skippedRows)
{
    // No picture to draw: the game's own clear (or the next scene) covers the screen
}

void Close(Player* player)
{
    g_States[0].open = false;
    g_States[0].frame = 0;
}
}
