#include "platform/saves.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

// The desktop's memory card: a directory of saves on the host's own filesystem (./saves), one directory a save like the PS2's
// (region, product and name) and its files within. One operation runs at a time, finished the Update after it was asked for
// (the host's calls are synchronous). Nothing is a real block device: the card is always there, always formatted and with room
namespace
{
using namespace Platform::Saves;

constexpr u32 ClusterSize = 0x400;
constexpr u32 ClusterShift = 10;
constexpr s32 DirectoryClusters = 2;
constexpr s32 MaxEntries = 20;
// Room the host's card reports (a PS2 card's kilobytes), enough for the game's saves
constexpr s32 CardKilobytes = 0x1e00;
constexpr size_t PathLimit = 256;
constexpr size_t NameLimit = 64;

#if defined(_WIN32)
#define MakeDirectory(path) _mkdir(path)
#else
#define MakeDirectory(path) mkdir(path, 0755)
#endif

constexpr char Root[] = "./saves";

bool g_Initialised;
char g_Region[8];
char g_Product[16];
Operation g_Operation = Operation::None;
Result g_Results[12];
StorageInfo g_Info = {-1, -1, -1};

s32 g_Port;
s32 g_Slot;
char g_Save[NameLimit];
char g_File[NameLimit];
void* g_Data;
s32 g_DataSize;
s32* g_Kilobytes;
s32* g_Size;
FileEntry* g_Entry;

void ForgetResults()
{
    for (Result& result : g_Results)
    {
        result = {-1, -1, 0};
    }
}

// Temporary diagnostics
void SaveLog(const char* what)
{
    if (std::getenv("TWINSANITY_SAVE_LOG") != nullptr)
    {
        std::fprintf(stderr, "SAVES %s op=%d info=(%d,%d,%d)\n", what, static_cast<s32>(g_Operation), g_Info.type,
                     g_Info.freeKilobytes, g_Info.formatted);
        std::fflush(stderr);
    }
}

void Finish(s32 succeeded, s32 step, s32 error)
{
    SaveLog("finish");
    g_Results[static_cast<s32>(g_Operation)] = {succeeded, step, error};
    g_Operation = Operation::None;
}

void Fail(s32 error)
{
    Finish(0, 0, error);
}

// The save's name as it may be a directory on the host (the region, product and name the PS2 would, with anything the host
// can't take as one character)
void SaveDirectory(char* path)
{
    char name[NameLimit];
    size_t i = 0;
    const char* parts[3] = {g_Region, g_Product, g_Save};
    for (const char* part : parts)
    {
        for (; *part != 0 && i + 1 < sizeof(name); part++)
        {
            char c = *part;
            bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
            name[i++] = plain ? c : '_';
        }
    }

    name[i] = 0;
    std::snprintf(path, PathLimit, "%s/%s", Root, name);
}

void FilePath(char* path)
{
    char directory[PathLimit];
    SaveDirectory(directory);
    char name[NameLimit];
    size_t i = 0;
    for (; g_File[i] != 0 && i + 1 < sizeof(name); i++)
    {
        char c = g_File[i];
        bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        name[i] = plain ? c : '_';
    }

    name[i] = 0;
    std::snprintf(path, PathLimit, "%s/%s", directory, name);
}

bool SaveDirectoryExists()
{
    char path[PathLimit];
    SaveDirectory(path);
    struct stat status;
    return stat(path, &status) == 0 && (status.st_mode & S_IFDIR) != 0;
}

#if defined(_WIN32)
void FileTimeToEntry(FileEntry& entry, const FILETIME& time)
{
    SYSTEMTIME systemTime;
    FileTimeToSystemTime(&time, &systemTime);
    entry.modified[1] = static_cast<u8>(systemTime.wSecond);
    entry.modified[2] = static_cast<u8>(systemTime.wMinute);
    entry.modified[3] = static_cast<u8>(systemTime.wHour);
    entry.modified[4] = static_cast<u8>(systemTime.wDay);
    entry.modified[5] = static_cast<u8>(systemTime.wMonth);
    entry.modified[6] = static_cast<u8>(systemTime.wYear & 0xff);
    entry.modified[7] = static_cast<u8>(systemTime.wYear >> 8);
}

// The save's files, their names and sizes (. and .. left out). -1 files when the save isn't there
s32 ListSave(FileEntry* entries)
{
    char path[PathLimit];
    SaveDirectory(path);
    char pattern[PathLimit];
    std::snprintf(pattern, sizeof(pattern), "%s/*", path);

    WIN32_FIND_DATAA data;
    HANDLE find = FindFirstFileA(pattern, &data);
    if (find == INVALID_HANDLE_VALUE)
    {
        return -1;
    }

    s32 count = 0;
    do
    {
        if (count >= MaxEntries)
        {
            break;
        }

        if (std::strcmp(data.cFileName, ".") == 0 || std::strcmp(data.cFileName, "..") == 0)
        {
            continue;
        }

        FileEntry& entry = entries[count];
        std::memset(&entry, 0, sizeof(entry));
        std::snprintf(entry.name, sizeof(entry.name), "%s", data.cFileName);
        entry.size = data.nFileSizeLow;
        FileTimeToEntry(entry, data.ftLastWriteTime);
        count++;
    } while (FindNextFileA(find, &data) != 0);

    FindClose(find);
    return count;
}

void ClearDirectory(const char* path);
void ClearDirectory(const char* path)
{
    char pattern[PathLimit];
    std::snprintf(pattern, sizeof(pattern), "%s/*", path);
    WIN32_FIND_DATAA data;
    HANDLE find = FindFirstFileA(pattern, &data);
    if (find == INVALID_HANDLE_VALUE)
    {
        return;
    }

    do
    {
        if (std::strcmp(data.cFileName, ".") == 0 || std::strcmp(data.cFileName, "..") == 0)
        {
            continue;
        }

        char child[PathLimit];
        std::snprintf(child, sizeof(child), "%s/%s", path, data.cFileName);
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            ClearDirectory(child);
            RemoveDirectoryA(child);
        }
        else
        {
            DeleteFileA(child);
        }
    } while (FindNextFileA(find, &data) != 0);

    FindClose(find);
}
#else
s32 ListSave(FileEntry* entries)
{
    char path[PathLimit];
    SaveDirectory(path);
    DIR* directory = opendir(path);
    if (directory == nullptr)
    {
        return -1;
    }

    s32 count = 0;
    struct dirent* item;
    while ((item = readdir(directory)) != nullptr && count < MaxEntries)
    {
        if (std::strcmp(item->d_name, ".") == 0 || std::strcmp(item->d_name, "..") == 0)
        {
            continue;
        }

        char filePath[PathLimit];
        std::snprintf(filePath, sizeof(filePath), "%s/%s", path, item->d_name);
        struct stat status;
        if (stat(filePath, &status) != 0)
        {
            continue;
        }

        FileEntry& entry = entries[count];
        std::memset(&entry, 0, sizeof(entry));
        std::snprintf(entry.name, sizeof(entry.name), "%s", item->d_name);
        entry.size = static_cast<u32>(status.st_size);
        count++;
    }

    closedir(directory);
    return count;
}

void ClearDirectory(const char* path)
{
    DIR* directory = opendir(path);
    if (directory == nullptr)
    {
        return;
    }

    struct dirent* item;
    while ((item = readdir(directory)) != nullptr)
    {
        if (std::strcmp(item->d_name, ".") == 0 || std::strcmp(item->d_name, "..") == 0)
        {
            continue;
        }

        char child[PathLimit];
        std::snprintf(child, sizeof(child), "%s/%s", path, item->d_name);
        struct stat status;
        if (stat(child, &status) != 0)
        {
            continue;
        }

        if ((status.st_mode & S_IFDIR) != 0)
        {
            ClearDirectory(child);
            rmdir(child);
        }
        else
        {
            remove(child);
        }
    }

    closedir(directory);
}
#endif

void FormatOp()
{
    ClearDirectory(Root);
    g_Info = {StorageMemoryCard, CardKilobytes, 1};
    Finish(1, 1, 0);
}

void CreateSaveOp()
{
    if (g_Info.freeKilobytes < DirectoryClusters)
    {
        Fail(ErrorNotEnoughSpace);
        return;
    }

    char path[PathLimit];
    SaveDirectory(path);
    if (MakeDirectory(path) != 0 && !SaveDirectoryExists())
    {
        Fail(ErrorNotEnoughSpace);
        return;
    }

    Finish(1, 1, 0);
}

void MeasureSaveOp()
{
    FileEntry entries[MaxEntries];
    s32 count = ListSave(entries);
    if (count < 0)
    {
        *g_Kilobytes = -1;
        Finish(1, 1, 0);
        return;
    }

    s32 kilobytes = DirectoryClusters;
    for (s32 i = 0; i < count; i++)
    {
        kilobytes += (static_cast<s32>(entries[i].size) + ClusterSize - 1) >> ClusterShift;
    }

    // Half a kilobyte an entry, the directory's own "." and ".." among them
    *g_Kilobytes = kilobytes + (count + 1) / 2;
    Finish(1, 1, 0);
}

void FindFileOp()
{
    if (g_Size != nullptr)
    {
        *g_Size = 0;
    }

    FileEntry entries[MaxEntries];
    s32 count = ListSave(entries);
    bool found = false;
    for (s32 i = 0; i < count && !found; i++)
    {
        found = std::strcmp(g_File, entries[i].name) == 0;
        if (found)
        {
            if (g_Size != nullptr)
            {
                *g_Size = static_cast<s32>(entries[i].size);
            }

            if (g_Entry != nullptr)
            {
                *g_Entry = entries[i];
            }
        }
    }

    if (!found && g_Size != nullptr)
    {
        *g_Size = -1;
    }

    Finish(1, 1, 0);
}

void ReadOp()
{
    if (!SaveDirectoryExists())
    {
        Fail(ErrorNoSave);
        return;
    }

    char path[PathLimit];
    FilePath(path);
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        Fail(ErrorNoSave);
        return;
    }

    size_t read = g_DataSize > 0 ? std::fread(g_Data, 1, static_cast<size_t>(g_DataSize), file) : 0;
    std::fclose(file);
    if (static_cast<s32>(read) != g_DataSize)
    {
        Fail(ErrorNoSave);
        return;
    }

    Finish(1, 1, 0);
}

void WriteOp()
{
    if (!SaveDirectoryExists())
    {
        Fail(ErrorNoSave);
        return;
    }

    char path[PathLimit];
    FilePath(path);
    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr)
    {
        Fail(ErrorNoSave);
        return;
    }

    if (g_DataSize > 0 && g_Data != nullptr &&
        std::fwrite(g_Data, 1, static_cast<size_t>(g_DataSize), file) != static_cast<size_t>(g_DataSize))
    {
        std::fclose(file);
        Fail(ErrorNoSave);
        return;
    }

    std::fclose(file);
    Finish(1, 1, 0);
}

bool Start(Operation operation, s32 port, s32 slot)
{
    ForgetResults();
    if (g_Operation != Operation::None || g_Info.type != StorageMemoryCard)
    {
        std::fprintf(stderr, "SAVES start refused op=%d info=(%d,%d,%d)\n", static_cast<s32>(operation), g_Info.type,
                     g_Info.freeKilobytes, g_Info.formatted);
        g_Operation = Operation::None;
        return false;
    }

    g_Operation = operation;
    g_Port = port;
    g_Slot = slot;
    SaveLog("start");
    return true;
}
}

bool Platform::Saves::Initialise(const char* region, const char* product)
{
    if (g_Initialised)
    {
        return true;
    }

    std::snprintf(g_Region, sizeof(g_Region), "%s", region);
    std::snprintf(g_Product, sizeof(g_Product), "%s", product);
    MakeDirectory(Root);
    g_Info = {StorageMemoryCard, CardKilobytes, 1};
    g_Initialised = true;
    ForgetResults();
    return true;
}

Platform::Saves::Operation Platform::Saves::Update(s32 port, s32 slot, StorageInfo* info)
{
    static u32 updatesTotal = 0;
    if (std::getenv("TWINSANITY_SAVE_LOG") != nullptr && (updatesTotal++ % 180) == 0)
    {
        std::fprintf(stderr, "SAVES update #%u op=%d info=(%d,%d,%d)\n", updatesTotal, static_cast<s32>(g_Operation), g_Info.type,
                     g_Info.freeKilobytes, g_Info.formatted);
    }

    if (g_Operation == Operation::None)
    {
        // The card is the saves' own directory: always there, always formatted with room
        g_Info = {StorageMemoryCard, CardKilobytes, 1};
    }
    else
    {
        g_Port = port;
        g_Slot = slot;
        switch (g_Operation)
        {
        case Operation::Format:
            FormatOp();
            break;
        case Operation::CreateSave:
            CreateSaveOp();
            break;
        case Operation::MeasureSave:
            MeasureSaveOp();
            break;
        case Operation::FindFile:
            FindFileOp();
            break;
        case Operation::Read:
            ReadOp();
            break;
        case Operation::Write:
            WriteOp();
            break;
        default:
            Fail(0);
            break;
        }
    }

    if (info != nullptr)
    {
        *info = g_Info;
    }

    return g_Operation;
}

Platform::Saves::Result Platform::Saves::GetResult(Operation operation)
{
    return g_Results[static_cast<s32>(operation)];
}

bool Platform::Saves::Format(s32 port, s32 slot)
{
    return Start(Operation::Format, port, slot);
}

bool Platform::Saves::CreateSave(s32 port, s32 slot, const char* save)
{
    bool started = Start(Operation::CreateSave, port, slot);
    std::snprintf(g_Save, sizeof(g_Save), "%s", save);
    return started;
}

bool Platform::Saves::MeasureSave(s32 port, s32 slot, const char* save, s32* kilobytes)
{
    bool started = Start(Operation::MeasureSave, port, slot);
    g_Kilobytes = kilobytes;
    std::snprintf(g_Save, sizeof(g_Save), "%s", save);
    return started;
}

bool Platform::Saves::FindFile(s32 port, s32 slot, const char* file, const char* save, s32* size, FileEntry* entry)
{
    bool started = Start(Operation::FindFile, port, slot);
    g_Entry = entry;
    g_Size = size;
    std::snprintf(g_Save, sizeof(g_Save), "%s", save);
    std::snprintf(g_File, sizeof(g_File), "%s", file);
    return started;
}

bool Platform::Saves::Write(s32 port, s32 slot, const char* save, const char* file, const void* data, s32 size)
{
    bool started = Start(Operation::Write, port, slot);
    g_Data = const_cast<void*>(data);
    g_DataSize = size;
    std::snprintf(g_Save, sizeof(g_Save), "%s", save);
    std::snprintf(g_File, sizeof(g_File), "%s", file);
    return started;
}

bool Platform::Saves::Read(s32 port, s32 slot, const char* save, const char* file, void* buffer, s32 size)
{
    bool started = Start(Operation::Read, port, slot);
    g_Data = buffer;
    g_DataSize = size;
    std::snprintf(g_Save, sizeof(g_Save), "%s", save);
    std::snprintf(g_File, sizeof(g_File), "%s", file);
    return started;
}

u32 Platform::Saves::FileKilobytes(u32 size)
{
    return (size + ClusterSize - 1) >> ClusterShift;
}

u32 Platform::Saves::SaveBytes(u32 kilobytes, u32 files)
{
    return (kilobytes + (files + 1) / 2 + DirectoryClusters) << ClusterShift;
}