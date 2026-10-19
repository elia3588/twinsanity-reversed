#include "platform/pads.h"

#include "debug.h"
#include "window.h"

#include "retail/libc.h"

// TEMP debug: an input script for runs the window's own keyboard can't drive ($TWINSANITY_PAD_SEQ "ms:buttons", a step's
// buttons held for its ms), applied as pad 1's own
#include <chrono>
#include <cstdlib>
#include <vector>

// Minimal desktop pad stub: a single stable pad at port 0, slot 0. It reports the buttons the desktop's own input holds
// (DesktopWindow::PadButtons), plus the test runs' g_TestPadButtons
namespace Platform::Pads
{
namespace
{
// TEMP debug: the input script's steps and where it started
struct PadStep
{
    u32 ms;
    u32 buttons;
};

std::vector<PadStep> g_PadScript;
bool g_PadScriptRead = false;
std::chrono::steady_clock::time_point g_PadScriptStart;

u32 ScriptedPad()
{
    if (!g_PadScriptRead)
    {
        g_PadScriptRead = true;
        g_PadScriptStart = std::chrono::steady_clock::now();
        const char* sequence = std::getenv("TWINSANITY_PAD_SEQ");
        if (sequence != nullptr)
        {
            const char* at = sequence;
            while (*at != 0)
            {
                char* end = nullptr;
                unsigned long ms = std::strtoul(at, &end, 0);
                if (end == at || *end != ':')
                {
                    break;
                }

                at = end + 1;
                unsigned long buttons = std::strtoul(at, &end, 0);
                if (end == at)
                {
                    break;
                }

                g_PadScript.push_back({static_cast<u32>(ms), static_cast<u32>(buttons)});
                if (*end == 0)
                {
                    break;
                }

                at = *end == ',' ? end + 1 : end;
            }
        }
    }

    if (g_PadScript.empty())
    {
        return 0;
    }

    u32 elapsed = static_cast<u32>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                                         g_PadScriptStart)
                                       .count());
    u32 at = 0;
    for (const PadStep& step : g_PadScript)
    {
        at += step.ms;
        if (elapsed < at)
        {
            return step.buttons;
        }
    }

    return g_PadScript.back().buttons;
}

// The report's pressure bytes of the low byte's buttons and of the directions (bits 12-15), as the PS2's pad layer
constexpr u32 PressureButtons = 8;
constexpr u8 ButtonPressures[PressureButtons] = {PressureL2,       PressureR2,     PressureL1,    PressureR1,
                                                 PressureTriangle, PressureCircle, PressureCross, PressureSquare};
constexpr u32 FirstDirectionBit = 12;
constexpr u32 Directions = 4;
constexpr u8 DirectionPressures[Directions] = {PressureUp, PressureRight, PressureDown, PressureLeft};
constexpr u8 FullPressure = 0xFF;
constexpr u8 StickMiddle = 0x80;

// The report a held button word (g_TestPadButtons: its bits) makes: its bytes, its button word (its high byte first), the
// buttons' pressures and the sticks centered
void FillReport(u8* data, u32 held)
{
    RetailLibc::MemorySet(data, 0, 21);
    data[ReportButtonsHigh] = static_cast<u8>(~(held >> 8));
    data[ReportButtonsLow] = static_cast<u8>(~held);
    for (u32 bit = 0; bit < PressureButtons; bit++)
    {
        if ((held & (1u << bit)) != 0)
        {
            data[ButtonPressures[bit]] = FullPressure;
        }
    }

    for (u32 direction = 0; direction < Directions; direction++)
    {
        if ((held & (1u << FirstDirectionBit) << direction) != 0)
        {
            data[DirectionPressures[direction]] = FullPressure;
        }
    }

    data[StickRightX] = StickMiddle;
    data[StickRightY] = StickMiddle;
    data[StickLeftX] = StickMiddle;
    data[StickLeftY] = StickMiddle;
    // The desktop's own sticks over the centred ones (the controller's, or the mouse's movement over the right stick)
    DesktopWindow::Sticks(&data[StickLeftX], &data[StickLeftY], &data[StickRightX], &data[StickRightY]);
}
}

s32 Initialise()
{
    return 0;
}

s32 Open(s32 port, s32 slot, void* buffer)
{
    if (port == 0 && slot == 0)
    {
        return 1;
    }
    return 0;
}

s32 Close(s32 port, s32 slot)
{
    return 0;
}

s32 Read(s32 port, s32 slot, u8* data)
{
    if (port != 0 || slot != 0 || data == nullptr)
    {
        return 0;
    }

    FillReport(data, g_TestPadButtons | DesktopWindow::PadButtons() | ScriptedPad());
    return 1;
}

State GetState(s32 port, s32 slot)
{
    if (port == 0 && slot == 0)
    {
        return StateStable;
    }
    return StateDisconnected;
}

RequestState GetRequestState(s32 port, s32 slot)
{
    return {};
}

s32 InfoMode(s32 port, s32 slot, ModeInfo info, s32 index)
{
    if (port != 0 || slot != 0)
    {
        return 0;
    }
    if (info == InfoCurrentId)
    {
        // An analog DualShock (the game's analog setup path, which reads the pad)
        return ModeIdAnalog;
    }
    if (info == InfoCurrentExtendedId)
    {
        // No extended mode: none that would override the current one
        return 0;
    }
    return 0;
}

s32 SetMainMode(s32 port, s32 slot, MainMode mode, ModeLock lock)
{
    if (port == 0 && slot == 0)
    {
        return 1; // success
    }
    return 0;
}

s32 InfoActuator(s32 port, s32 slot, s32 actuator, s32 term)
{
    if (port == 0 && slot == 0)
    {
        return 1; // has actuators
    }
    return 0;
}

s32 SetActuatorAlign(s32 port, s32 slot, const u8 align[6])
{
    return (port == 0 && slot == 0) ? 1 : 0;
}

s32 SetActuatorDirect(s32 port, s32 slot, const u8 values[6])
{
    return (port == 0 && slot == 0) ? 1 : 0;
}

s32 InfoPressureMode(s32 port, s32 slot)
{
    if (port == 0 && slot == 0)
    {
        return 1; // has pressure mode
    }
    return 0;
}

s32 EnterPressureMode(s32 port, s32 slot)
{
    return (port == 0 && slot == 0) ? 1 : 0;
}

}

volatile u32 g_TestPadButtons = 0;
