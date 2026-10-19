#include "common.h"
#include "gcc2.h"
#include "platform/system.h"

// The desktop's entry: the game's Main with the program's arguments
extern "C" int Main(u32 argc, char** argv);
// The crash handler's installer (crash.cpp, host ABI)
extern "C" void InstallCrashHandler();

// The object nodes' and the projectiles' vtables (their retail names: the game's own node construction reads the first as its
// base and the projectiles' part takes the second)
extern const GccVTableEntry InstanceNodeType0_Methods[];
extern const GccVTableEntry InstanceNodeType8_Methods[];

namespace
{
// The object nodes' slot 26 (a launch) and 27 (a push), and the projectiles' slot 26 (the same slot a launch is called on, but
// the projectiles read it as their own velocity)
constexpr u32 LaunchSlot = 26;
constexpr u32 PushSlot = 27;

// The retail functions as the tables hold them, kept to be called from the adapters
void* g_LaunchFunction = nullptr;
void* g_PushFunction = nullptr;
void* g_SetVelocityFunction = nullptr;

// The object nodes' slot 26: the caller gives (self, gravity, velocity) but LaunchNode takes (gravity, node, velocity)
void LaunchAdapter(void* self, f32 gravity, const void* velocity)
{
    reinterpret_cast<void (*)(f32, void*, const void*)>(g_LaunchFunction)(gravity, self, velocity);
}

// The object nodes' slot 27: the caller gives (self, strength, other) but PushNode takes (strength, node, other)
void PushAdapter(void* self, f32 strength, void* other)
{
    reinterpret_cast<void (*)(f32, void*, void*)>(g_PushFunction)(strength, self, other);
}

// The projectiles' slot 26: a launch called on a projectile node reaches SetVelocity(self, velocity); the caller's gravity is
// what the EABI had said nothing of (SetVelocity takes no float), so it's left out
void SetVelocityAdapter(void* self, f32, const void* velocity)
{
    reinterpret_cast<void (*)(void*, const void*)>(g_SetVelocityFunction)(self, velocity);
}

// One entry of a retail vtable pointed at an adapter (the tables are the retail data's, written here as the game reaches them
// through their own names)
void Replace(GccVTableEntry* table, u32 slot, void* adapter)
{
    table[slot].function = adapter;
}
} // namespace

// The mixed-argument calls through the retail vtables are given the order the host reads (CallVirtual): a node's launch and
// push, and a projectile's velocity, are the entries whose retail declarations don't start with the object, so they take an
// adapter that puts the object where their declarations have it
static void InstallVTableFixes()
{
    g_LaunchFunction = InstanceNodeType0_Methods[LaunchSlot].function;
    g_PushFunction = InstanceNodeType0_Methods[PushSlot].function;
    g_SetVelocityFunction = InstanceNodeType8_Methods[LaunchSlot].function;
    Replace(const_cast<GccVTableEntry*>(InstanceNodeType0_Methods), LaunchSlot, reinterpret_cast<void*>(&LaunchAdapter));
    Replace(const_cast<GccVTableEntry*>(InstanceNodeType0_Methods), PushSlot, reinterpret_cast<void*>(&PushAdapter));
    Replace(const_cast<GccVTableEntry*>(InstanceNodeType8_Methods), LaunchSlot, reinterpret_cast<void*>(&SetVelocityAdapter));
}

int main(int argc, char** argv)
{
    InstallCrashHandler();
    InstallVTableFixes();
    Platform::System::Exit(Main(static_cast<u32>(argc), argv));
}
