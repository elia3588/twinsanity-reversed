#include "platform/stream.h"
#include "platform/files.h"

#include "sound_internal.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// The desktop's side of Platform::Stream: read from disc files on host filesystem
namespace Platform::Stream
{
namespace
{
struct FileInfo
{
    s32 fd;
    u32 size;
};

std::vector<FileInfo> g_OpenFiles;
s32 g_NextFile = 0;
// The size of the file opened last: FileSize is its size until the next file is opened (platform/stream.h)
u32 g_LastFileSize = 0;

s32 GetFreeSlot()
{
    for (size_t i = 0; i < g_OpenFiles.size(); i++)
    {
        if (g_OpenFiles[i].fd < 0)
        {
            return static_cast<s32>(i);
        }
    }
    g_OpenFiles.push_back({-1, 0});
    return static_cast<s32>(g_OpenFiles.size() - 1);
}
}

s32 Initialise(s32 channels, void* workMemory)
{
    (void)channels;
    (void)workMemory;
    g_OpenFiles.clear();
    g_NextFile = 0;
    g_LastFileSize = 0;
    return 0;
}

s32 Update()
{
    return 0;
}

State GetState()
{
    return State::Running;
}

void AttachBuffer(s32 channel, u32 size, u32 soundAddress, u32 soundSize)
{
    (void)channel;
    (void)size;
    (void)soundAddress;
    (void)soundSize;
}

void DetachBuffer(s32 channel)
{
    (void)channel;
}

u32 FreeBufferMemory()
{
    return 0x1000000; // large amount
}

s32 OpenFile(const char* path)
{
    if (path == nullptr)
    {
        return NoFile;
    }

    // The disc path straight to the host's files (Platform::Files matches the case)
    s32 fd = Platform::Files::Open(path, Platform::Files::OpenRead);
    if (fd < 0)
    {
        g_LastFileSize = 0;
        return NoFile;
    }

    // The file's size, the read position left where it was
    s32 old = Platform::Files::Seek(fd, 0, Platform::Files::SeekCurrent);
    s32 end = Platform::Files::Seek(fd, 0, Platform::Files::SeekEnd);
    Platform::Files::Seek(fd, old, Platform::Files::SeekSet);
    if (end < 0)
    {
        end = 0;
    }

    g_LastFileSize = static_cast<u32>(end);
    s32 slot = GetFreeSlot();
    g_OpenFiles[slot] = {fd, g_LastFileSize};
    return slot;
}

u32 FileSize()
{
    return g_LastFileSize;
}

void CloseFile(s32 file)
{
    if (file >= 0 && file < static_cast<s32>(g_OpenFiles.size()))
    {
        if (g_OpenFiles[file].fd >= 0)
        {
            Platform::Files::Close(g_OpenFiles[file].fd);
            g_OpenFiles[file].fd = -1;
            g_OpenFiles[file].size = 0;
        }
    }
}

void Read(s32 channel, s32 file, u32 offset, u32 size, void* destination)
{
    (void)channel;
    if (file < 0 || file >= static_cast<s32>(g_OpenFiles.size())) return;
    if (g_OpenFiles[file].fd < 0) return;
    s32 fd = g_OpenFiles[file].fd;
    Platform::Files::Seek(fd, static_cast<s32>(offset), Platform::Files::SeekSet);
    Platform::Files::Read(fd, destination, static_cast<s32>(size));
}

void ReadSoundBank(s32 channel, u32 bank, s32 file, u32 offset, u32 size)
{
    // The bank's samples land in their own block of the sound processor's memory tagged by the sound's number, and a sound
    // read again gets a block of its own so a voice's playing one stays whole (DesktopSound's sound memory)
    void* buffer = DesktopSound::BankBuffer(bank, size);
    if (buffer != nullptr)
    {
        Read(channel, file, offset, size, buffer);
        // TEMP trace: the bank's own head bytes show whether real ADPCM landed here
        static u32 reads = 0;
        if ((bank >= 0x8000 || reads < 3) && reads < 40)
        {
            reads++;
            const u8* head = static_cast<const u8*>(buffer);
            std::fprintf(stderr, "read s=%u size=%u head=%02X%02X%02X%02X\n", bank, size,
                         size > 0 ? head[0] : 0, size > 1 ? head[1] : 0, size > 2 ? head[2] : 0, size > 3 ? head[3] : 0);
            std::fflush(stderr);
        }
    }
}

bool IsReading(s32 channel)
{
    (void)channel;
    return false; // reads complete synchronously
}

s32 Wait(s32 channel)
{
    (void)channel;
    return 0;
}
}