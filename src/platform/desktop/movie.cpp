#include "platform/movie.h"

#include "movie/mpeg.h"

#include "game/renderer.h"
#include "platform/files.h"
#include "platform/graphics.h"
#include "sdl_gfx.h"
#include "sound_internal.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// The desktop's movies: the PS2's PSS files. The file is read whole through Platform::Files, libmpeg (movie/) demultiplexes it
// into the video's elementary stream and the sound's PCM, the pictures are decoded by the software IPU (movie/ipu.cpp) and drawn
// through SDL, and the sound is mixed at the movie's own volume (sound_internal.h)
//
// The PS2 streams the file off the disc into rings as the IPU and the sound processor consume them (movie/decoder.cpp and
// movie/player.cpp). The desktop reads it all at once (the files are a few megabytes), decodes a picture a frame and hands the
// sound over whole, which keeps the parser's own ring and DMA bookkeeping out of the way

namespace Platform::Movie
{
namespace
{


// sceMpegCreate's state (its buffers, callbacks and the sequence's own) before the pictures' own memory is added to it
constexpr u32 MpegStateWorkSize = 0x2800;
// The sound the mixer plays (audio.cpp): 16 bit stereo at this rate, which the player resamples the file's PCM to
constexpr u32 MixRate = 48000;
// A picture macroblock's own 32 bit pixels (16 x 16)
constexpr u32 Rgb32MacroblockBytes = Rgb32MacroblockQuadwords << 4;
// The read buffer's own chunk of the file
constexpr s32 ReadChunk = 0x4000;

// The player: one at a time on the desktop (the game shows one movie at a time). The game's own Player memory is left to it: the
// pointer is only the handle the interface's calls are made with
struct PlayerState
{
    // The decoder's state and the memory it lives in
    Mpeg mpeg;
    bool mpegMade;
    u8* work;
    // The video's elementary stream and the sound's PCM as the demultiplexer gave them
    std::vector<u8> video;
    std::vector<u8> sound;
    MovieAudioHeader audioHeader;
    u32 audioHeaderBytes;
    // The samples the mixer plays
    std::vector<s16> samples;
    // The decoded pictures (the IPU's 32 bit pixels): the one decoded last and the one before it
    u8* pictures[2];
    u32 pictureBytes;
    s32 current;
    s32 frames;
    // When the picture schedule started (the sound's opening, microseconds of the steady clock), the pictures are paced at the
    // sound's own rate (48000 divided by the samples per frame: 25 for the retail's movies)
    s64 startMicros;
    s32 rate;
    // The picture's own size in pixels (macroblock aligned, known once the first one is decoded)
    u32 width;
    u32 height;
    u32 macroblocks;
    // The picture linearised for the texture (its macroblock columns made rows)
    u8* linear;
    u32 linearBytes;
    bool ended;
    bool open;
    void (*present)();
};

PlayerState g_Player = {};

// A buffer of bytes whose address is a multiple of alignment (the decoder's state and a picture buffer must be), or nullptr
u8* AllocateAligned(size_t bytes, size_t alignment)
{
    u8* raw = static_cast<u8*>(std::malloc(bytes + alignment + sizeof(void*)));
    if (raw == nullptr)
    {
        return nullptr;
    }

    uintptr_t start = reinterpret_cast<uintptr_t>(raw) + sizeof(void*);
    uintptr_t aligned = (start + alignment - 1) & ~(static_cast<uintptr_t>(alignment) - 1);
    reinterpret_cast<uintptr_t*>(aligned)[-1] = reinterpret_cast<uintptr_t>(raw);
    return reinterpret_cast<u8*>(aligned);
}

void FreeAligned(u8* address)
{
    if (address != nullptr)
    {
        std::free(reinterpret_cast<void*>(reinterpret_cast<uintptr_t*>(address)[-1]));
    }
}

// The file's whole contents through Platform::Files. An empty vector when it couldn't be opened
std::vector<u8> ReadWholeFile(const char* file)
{
    // The game's disc names carry the ISO 9660 version (";1"), which the desktop's file system's own names don't
    char path[0x100];
    std::strncpy(path, file, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    char* version = std::strrchr(path, ';');
    if (version != nullptr)
    {
        *version = '\0';
    }

    s32 handle = Platform::Files::Open(path, Platform::Files::OpenRead);
    if (handle < 0)
    {
        return {};
    }

    std::vector<u8> data;
    u8 buffer[ReadChunk];
    while (true)
    {
        s32 got = Platform::Files::Read(handle, buffer, ReadChunk);
        if (got <= 0)
        {
            break;
        }

        data.insert(data.end(), buffer, buffer + got);
    }

    Platform::Files::Close(handle);
    return data;
}

// libmpeg's callback for the video: its elementary stream's bytes, in order (nothing of a sub-stream's: MPEG-2 video has none)
s32 StoreVideo(Mpeg*, void* streamData, void*)
{
    auto* packet = static_cast<MpegStreamData*>(streamData);
    g_Player.video.insert(g_Player.video.end(), packet->data, packet->data + packet->length);
    return 1;
}

// libmpeg's callback for the sound: the file's own header ("SShd"/"SSbd") first, then the PCM's samples
s32 StoreAudio(Mpeg*, void* streamData, void*)
{
    auto* packet = static_cast<MpegStreamData*>(streamData);
    if (packet->length < SubStreamBytes)
    {
        return 1;
    }

    packet->data += SubStreamBytes;
    packet->length -= SubStreamBytes;
    if (g_Player.audioHeaderBytes < sizeof(MovieAudioHeader))
    {
        u8* header = reinterpret_cast<u8*>(&g_Player.audioHeader);
        u32 missing = static_cast<u32>(sizeof(MovieAudioHeader)) - g_Player.audioHeaderBytes;
        u32 take = packet->length < missing ? packet->length : missing;
        std::memcpy(header + g_Player.audioHeaderBytes, packet->data, take);
        g_Player.audioHeaderBytes += take;
        packet->data += take;
        packet->length -= take;
    }

    if (packet->length != 0)
    {
        g_Player.sound.insert(g_Player.sound.end(), packet->data, packet->data + packet->length);
    }

    return 1;
}

// The picture size the video's sequence header gives (its horizontal and vertical sizes after its start code)
void SequenceSizesFromVideo(u32* width, u32* height)
{
    *width = 0;
    *height = 0;
    const std::vector<u8>& video = g_Player.video;
    for (size_t i = 0; i + 8 < video.size(); i++)
    {
        if (video[i] != 0 || video[i + 1] != 0 || video[i + 2] != 1 || video[i + 3] != 0xB3)
        {
            continue;
        }

        *width = (static_cast<u32>(video[i + 4]) << 4) | (static_cast<u32>(video[i + 5]) >> 4);
        *height = (static_cast<u32>(video[i + 5] & 0xF) << 8) | static_cast<u32>(video[i + 6]);
        return;
    }
}

// The PSS file's PCM made the mixer's own frames (16 bit stereo at MixRate): resampled when the file's rate differs, and a mono
// stream's sample given to both
void BuildSamples()
{
    u32 channels = g_Player.audioHeader.channels > 0 ? static_cast<u32>(g_Player.audioHeader.channels) : 2u;
    u32 rate = g_Player.audioHeader.rate > 0 ? g_Player.audioHeader.rate : MixRate;
    // The SS sound's channels arrive in blocks: the header's interleave is the bytes of one channel's block, the blocks take
    // turns (the left's block, then the right's, both the next of the sound's own times after). The player's samples want the
    // frames themselves, so the blocks are drawn apart first (the retail's sound processor keeps the channels' rings apart the
    // same way)
    u32 blockValues = 0;
    if (g_Player.audioHeader.interleave > 0 && channels > 1)
    {
        blockValues = static_cast<u32>(g_Player.audioHeader.interleave) / 2u;
    }

    size_t bytes = g_Player.sound.size() - (g_Player.sound.size() % (2 * channels));
    u32 inputFrames = 0;
    std::vector<s16> frames;
    const s16* input = nullptr;
    if (blockValues != 0 && bytes >= blockValues * channels)
    {
        u32 blocks = static_cast<u32>(bytes / (blockValues * channels));
        inputFrames = blocks * blockValues;
        frames.resize(inputFrames * channels);
        const s16* source = reinterpret_cast<const s16*>(g_Player.sound.data());
        for (u32 block = 0; block < blocks; block++)
        {
            u32 channel = block % channels;
            u32 start = block / channels * blockValues;
            const s16* from = source + block * blockValues;
            for (u32 i = 0; i < blockValues; i++)
            {
                u32 frame = start + i;
                frames[frame * channels + channel] = from[i];
            }
        }

        input = frames.data();
    }
    else
    {
        inputFrames = static_cast<u32>(bytes / (2 * channels));
        input = reinterpret_cast<const s16*>(g_Player.sound.data());
    }

    g_Player.samples.clear();
    if (inputFrames == 0)
    {
        return;
    }

    double step = static_cast<double>(rate) / MixRate;
    u32 outputFrames = static_cast<u32>(static_cast<double>(inputFrames) / step);
    g_Player.samples.resize(static_cast<size_t>(outputFrames) * 2);
    u32 right = channels > 1 ? 1 : 0;
    for (u32 i = 0; i < outputFrames; i++)
    {
        double position = static_cast<double>(i) * step;
        u32 index = static_cast<u32>(position);
        if (index >= inputFrames)
        {
            index = inputFrames - 1;
        }

        u32 next = index + 1 < inputFrames ? index + 1 : index;
        f32 fraction = static_cast<f32>(position - index);
        s16 left0 = input[index * channels];
        s16 right0 = input[index * channels + right];
        s16 left1 = input[next * channels];
        s16 right1 = input[next * channels + right];
        g_Player.samples[i * 2] = static_cast<s16>(left0 + static_cast<f32>(left1 - left0) * fraction);
        g_Player.samples[i * 2 + 1] = static_cast<s16>(right0 + static_cast<f32>(right1 - right0) * fraction);
    }
}

void AddStreamCallbacks()
{
    sceMpegAddStrCallback(&g_Player.mpeg, MpegVideoStream, 0, StoreVideo, nullptr);
    sceMpegAddStrCallback(&g_Player.mpeg, MpegPcmStream, 0, StoreAudio, nullptr);
}

// One more picture decoded into the other of the two buffers. False when there was none
bool DecodeNext()
{
    if (!g_Player.mpegMade)
    {
        return false;
    }

    u8* target = g_Player.pictures[1 - g_Player.current];
    s32 result = sceMpegGetPicture(&g_Player.mpeg, target, static_cast<s32>(g_Player.macroblocks));
    if (result < 0)
    {
        return false;
    }

    // The sequence and picture headers set the picture's own size (the first decode knows it)
    if (g_Player.mpeg.sys->pictureOutput != 0)
    {
        g_Player.current = 1 - g_Player.current;
        g_Player.frames++;
        g_Player.width = static_cast<u32>(g_Player.mpeg.width);
        g_Player.height = static_cast<u32>(g_Player.mpeg.height);
        return true;
    }

    return false;
}

// Everything the player holds let go of
void Release()
{
    DesktopSound::MovieSoundClose();
    FreeAligned(g_Player.pictures[0]);
    FreeAligned(g_Player.pictures[1]);
    FreeAligned(g_Player.work);
    FreeAligned(g_Player.linear);
    g_Player.linear = nullptr;
    g_Player.linearBytes = 0;
    g_Player.pictures[0] = nullptr;
    g_Player.pictures[1] = nullptr;
    g_Player.work = nullptr;
    g_Player.video.clear();
    g_Player.sound.clear();
    g_Player.samples.clear();
    g_Player.mpegMade = false;
    g_Player.open = false;
    g_Player.ended = false;
    g_Player.frames = 0;
    g_Player.current = 0;
    g_Player.width = 0;
    g_Player.height = 0;
    g_Player.audioHeaderBytes = 0;
}
} // namespace

void Construct(Player*)
{
    Release();
    g_Player.present = nullptr;
}

void BeginPresenting(Player*, void (*present)())
{
    g_Player.present = present;
}

bool Open(Player*, const char* file, u32, s32, void*)
{
    Release();
    // The movie's own schedule starts at its first step (a previous movie's would have it a long way along already)
    g_Player.startMicros = 0;
    g_Player.rate = 0;
    std::vector<u8> contents = ReadWholeFile(file != nullptr ? file : "");
    if (contents.empty())
    {
        return false;
    }

    sceMpegInit();
    // The first create needs only the state: the demultiplexing reads the packs into the callbacks' video and sound, and the
    // pictures' own memory comes once the sequence header's size is known
    u8* state = AllocateAligned(MpegStateWorkSize, 0x40);
    if (state == nullptr)
    {
        return false;
    }

    if (sceMpegCreate(&g_Player.mpeg, state, static_cast<s32>(MpegStateWorkSize)) == 0)
    {
        FreeAligned(state);
        return false;
    }

    g_Player.mpegMade = true;
    AddStreamCallbacks();
    s32 offset = 0;
    s32 size = static_cast<s32>(contents.size());
    while (offset < size)
    {
        s32 used = sceMpegDemuxPss(&g_Player.mpeg, contents.data() + offset, size - offset);
        if (used <= 0)
        {
            break;
        }

        offset += used;
    }

    contents.clear();
    contents.shrink_to_fit();

    u32 width = 0;
    u32 height = 0;
    SequenceSizesFromVideo(&width, &height);
    if (width == 0 || height == 0 || g_Player.video.empty())
    {
        FreeAligned(state);
        Release();
        return false;
    }

    // The macroblock memory, sized for the taller of a frame's own height and an interlaced sequence's (the header's vertical
    // size is the smaller of the two readings, and the sequence extension settles which the stream is)
    u32 widthMacroblocks = (width + 15) >> 4;
    u32 heightMacroblocks = ((height + 31) >> 5) << 1;
    g_Player.macroblocks = widthMacroblocks * heightMacroblocks;
    g_Player.pictureBytes = g_Player.macroblocks * Rgb32MacroblockBytes;
    u32 workBytes = MpegStateWorkSize + widthMacroblocks * heightMacroblocks * MacroblockBytes * 3 + 0x1000;
    u8* work = AllocateAligned(workBytes, 0x40);
    if (work == nullptr)
    {
        FreeAligned(state);
        Release();
        return false;
    }

    // The state again, this time with the pictures' own memory behind it
    if (sceMpegCreate(&g_Player.mpeg, work, static_cast<s32>(workBytes)) == 0)
    {
        FreeAligned(state);
        FreeAligned(work);
        Release();
        return false;
    }

    FreeAligned(state);
    g_Player.work = work;
    g_Player.mpegMade = true;
    AddStreamCallbacks();
    g_Player.pictures[0] = AllocateAligned(g_Player.pictureBytes, 0x40);
    g_Player.pictures[1] = AllocateAligned(g_Player.pictureBytes, 0x40);
    // The picture linearised for the texture, which the pictures' own 32 bit macroblocks always have the room for
    g_Player.linearBytes = g_Player.pictureBytes;
    g_Player.linear = AllocateAligned(g_Player.linearBytes, 0x40);
    if (g_Player.pictures[0] == nullptr || g_Player.pictures[1] == nullptr || g_Player.linear == nullptr)
    {
        Release();
        return false;
    }

    // The IPU reads the video's whole elementary stream (its commands' own bit reader, movie/mpegbits.cpp)
    IpuSetBitstream(g_Player.video.data(), static_cast<s32>(g_Player.video.size()));
    BuildSamples();
    g_Player.current = 0;
    g_Player.frames = 0;
    g_Player.ended = false;
    g_Player.open = true;
    DecodeNext();
    return true;
}

void StartSound(Player*, f32 volume)
{
    if (g_Player.samples.empty())
    {
        return;
    }

    DesktopSound::MovieSoundOpen(g_Player.samples.data(), static_cast<u32>(g_Player.samples.size() / 2), volume);
}

bool Step(Player*)
{
    if (!g_Player.open || g_Player.ended)
    {
        return false;
    }

    if (sceMpegIsEnd(&g_Player.mpeg) != 0)
    {
        g_Player.ended = true;
        return false;
    }

    // The pictures are paced by the wall clock at the movie's own rate (the retail waits for the VBlank the PS2's fixed years
    // give it): keep the decode until the schedule's frame is made, a few at most so a slow stretch doesn't stall the loop
    s64 now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
                  .count();
    if (g_Player.startMicros == 0)
    {
        g_Player.startMicros = now;
        // The retail's PSS sound: 48000 Hz stereo 16 bit PCM, 1920 samples to the movie's frame (40 ms, PAL's 25 a second)
        g_Player.rate = 48000 / 1920;
    }

    // The frame the schedule has reached is decoded (the first picture came with the file): the game steps once a frame, so
    // decoding more often than the schedule would run the movie fast
    s32 due = static_cast<s32>((now - g_Player.startMicros) * g_Player.rate / 1000000);
    s32 guard = 4;
    while (g_Player.frames <= due && guard > 0 && sceMpegIsEnd(&g_Player.mpeg) == 0)
    {
        DecodeNext();
        guard--;
    }

    if (sceMpegIsEnd(&g_Player.mpeg) != 0)
    {
        g_Player.ended = true;
    }

    return true;
}

void WaitFrame(Player*)
{
    // The PS2 waits for the vertical blank, which presents the frame and then queues the next picture. The desktop presents it
    // here: the game's own present callback (its frame's bookkeeping) and then the window's flip
    if (g_Player.present != nullptr)
    {
        g_Player.present();
    }

    Platform::Graphics::Present(nullptr);
}

void QueuePicture(Player*)
{
    // Nothing to send: the desktop draws the decoded picture itself from memory (Draw)
}

void Draw(Player*, const Area& area, s32, s32, u32 skippedRows)
{
    if (!g_Player.open || g_Player.frames == 0 || g_Player.width == 0 || g_Player.height == 0)
    {
        return;
    }

    // The picture's macroblocks are columns of the picture (the IPU's own order): each made a run of the linear image
    u32 macroblocksWide = g_Player.width >> 4;
    u32 macroblocksHigh = g_Player.height >> 4;
    size_t needed = static_cast<size_t>(g_Player.width) * g_Player.height * 4;
    if (g_Player.linear == nullptr || g_Player.linearBytes < needed)
    {
        return;
    }

    const u8* source = g_Player.pictures[g_Player.current];
    u8* out = g_Player.linear;
    for (u32 column = 0; column < macroblocksWide; column++)
    {
        for (u32 row = 0; row < macroblocksHigh; row++)
        {
            const u8* macroblock = source + (static_cast<size_t>(column) * macroblocksHigh + row) * Rgb32MacroblockBytes;
            for (u32 y = 0; y < 16; y++)
            {
                u8* line = out + (static_cast<size_t>(row * 16 + y) * g_Player.width + column * 16) * 4;
                std::memcpy(line, macroblock + y * 16 * 4, 16 * 4);
            }
        }
    }


    // The game gives the area in the display's own pixels (the PS2's frame): the window's are its own size
    s32 windowWidth = 0;
    s32 windowHeight = 0;
    SdlGfx::GetSize(&windowWidth, &windowHeight);
    u32 displayWidth = g_DisplayWidth != 0 ? g_DisplayWidth : g_Player.width;
    u32 displayHeight = g_DisplayHeight != 0 ? g_DisplayHeight : g_Player.height;
    if (windowWidth <= 0 || windowHeight <= 0)
    {
        return;
    }

    f32 across = static_cast<f32>(windowWidth) / static_cast<f32>(displayWidth);
    f32 down = static_cast<f32>(windowHeight) / static_cast<f32>(displayHeight);
    s32 dstX = static_cast<s32>(static_cast<f32>(area.x) * across);
    s32 dstY = static_cast<s32>(static_cast<f32>(area.y) * down);
    s32 dstWidth = static_cast<s32>(static_cast<f32>(area.width) * across);
    s32 dstHeight = static_cast<s32>(static_cast<f32>(area.height) * down);

    // A picture higher than the screen loses as many rows at its top as at its bottom
    u32 skip = skippedRows;
    if (g_Player.height <= skip * 2)
    {
        skip = 0;
    }

    void* texture = SdlGfx::CreateTexture(out, static_cast<s32>(g_Player.width), static_cast<s32>(g_Player.height));
    if (texture != nullptr)
    {
        SdlGfx::DrawTexture(texture, dstX, dstY, dstWidth, dstHeight, 0, static_cast<s32>(skip),
                            static_cast<s32>(g_Player.width), static_cast<s32>(g_Player.height - skip * 2), 255, 255, 255, 255);
        SdlGfx::DestroyTexture(texture);
    }
}

void Close(Player*)
{
    Release();
    g_Player.present = nullptr;
}
} // namespace Platform::Movie
