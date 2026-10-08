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
    std::fprintf(stderr, "CRASH code %08lX base %p eip %p addr %p\n", info->ExceptionRecord->ExceptionCode,
                 GetModuleHandle(nullptr), info->ContextRecord->Eip, info->ExceptionRecord->ExceptionAddress);
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