#include "common.h"
#include "platform/system.h"

// The desktop's entry: the game's Main with the program's arguments
extern "C" int Main(u32 argc, char** argv);
// The crash handler's installer (crash.cpp, host ABI)
extern "C" void InstallCrashHandler();

int main(int argc, char** argv)
{
    InstallCrashHandler();
    Platform::System::Exit(Main(static_cast<u32>(argc), argv));
}
