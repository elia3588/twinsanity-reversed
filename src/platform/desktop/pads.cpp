#include "platform/pads.h"

#include "debug.h"

#include "retail/libc.h"

// Minimal desktop pad stub: a single stable pad at port 0, slot 0
namespace Platform::Pads
{
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
    RetailLibc::MemorySet(data, 0, 21);
    u32 btn = 0;
    // g_TestPadButtons is defined in debug.cpp; not exposed as extern here
    // avoid pulling it in to prevent link issues
    u8 low = 0xFF;
    u8 high = 0xFF;
    // Map basic buttons (approximate DS2 digital layout)
    if (btn & (1u << 0)) low &= ~0x40; // Cross
    if (btn & (1u << 1)) low &= ~0x20; // Circle
    if (btn & (1u << 2)) low &= ~0x80; // Square
    if (btn & (1u << 3)) low &= ~0x10; // Triangle
    if (btn & (1u << 4)) high &= ~0x08; // Start
    if (btn & (1u << 5)) high &= ~0x01; // Select
    if (btn & (1u << 6)) high &= ~0x10; // Up
    if (btn & (1u << 7)) high &= ~0x40; // Down
    if (btn & (1u << 8)) high &= ~0x80; // Left
    if (btn & (1u << 9)) high &= ~0x20; // Right
    if (btn & (1u << 10)) low &= ~0x04; // L1
    if (btn & (1u << 11)) low &= ~0x08; // L2
    if (btn & (1u << 12)) low &= ~0x02; // R1
    if (btn & (1u << 13)) low &= ~0x01; // R2
    data[2] = low;
    data[3] = high;
    data[6] = 0x80;
    data[7] = 0x80;
    data[8] = 0x80;
    data[9] = 0x80;
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
        // DualShock 2 ID
        return 0x5A;
    }
    if (info == InfoCurrentExtendedId)
    {
        return 0x03; // analog pad extended
    }
    if (info == InfoModeTable)
    {
        // Return 1 to indicate success for mode table read
        return 1;
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
