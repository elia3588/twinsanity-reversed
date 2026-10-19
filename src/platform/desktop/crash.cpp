#include <windows.h>
#include <cstdio>

// A debugger's stand-in: on an unhandled exception (an access violation) the faulting addresses are written out, to be given
// to addr2line along with this program. Host ABI, built with the SDL2 files
namespace
{
LONG WINAPI OnCrash(EXCEPTION_POINTERS* info)
{
    void* frames[64];
    USHORT count = CaptureStackBackTrace(0, 64, frames, nullptr);
    std::fprintf(stderr, "CRASH code %08lX base %p eip %p addr %p", info->ExceptionRecord->ExceptionCode,
                 GetModuleHandle(nullptr), info->ContextRecord->Eip, info->ExceptionRecord->ExceptionAddress);
    // An access violation's own place of the access with it
    if (info->ExceptionRecord->ExceptionCode == 0xC0000005 && info->ExceptionRecord->NumberParameters > 1)
    {
        std::fprintf(stderr, " access %p%s", info->ExceptionRecord->ExceptionInformation[1],
                     info->ExceptionRecord->ExceptionInformation[0] != 0 ? " write" : " read");
    }

    std::fprintf(stderr, "\n");
    for (USHORT i = 0; i < count; i++)
    {
        std::fprintf(stderr, "  %p\n", frames[i]);
    }

    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
}

extern "C" void InstallCrashHandler()
{
    SetUnhandledExceptionFilter(OnCrash);
}