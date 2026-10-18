#include "platform/audio.h"

// The desktop's side of Platform::Audio: no sound yet
namespace Platform::Audio
{
void Reset()
{
}

void PauseAll()
{
}

void ResumeAll()
{
}

s32 LendToMovie()
{
    return 0;
}

s32 ReclaimFromMovie()
{
    return 0;
}

void CompactSoundMemory()
{
}

void SetGroupVolume(s32 group, u32 left, u32 right)
{
}

s32 SetVoiceVolume(s32 voice, s16 left, s16 right)
{
    return 0;
}

s32 SetVoicePitch(s32 voice, u16 pitch)
{
    return 0;
}

void SetVoiceReverb(s32 voice, bool on)
{
}

void ReleaseVoice(s32 voice, u32 rate)
{
}

s32 IsVoiceFree(s32 voice)
{
    return 0;
}

void SetReverb(s32 core, s32 mode, u16 depthLeft, u16 depthRight, u16 delay, u16 feedback)
{
}

void ClearReverb(s32 core)
{
}

void SetReverbVolume(s32 core, s16 left, s16 right)
{
}

u32 ReserveSoundMemory(u32 size)
{
    return 0;
}

void SoundBankLoaded(u16 bank)
{
}

void ReserveSounds(u16 count)
{
}

void ReleaseSound(u32 sound)
{
}

s32 PlaySound(u32 sound, u32 voiceAndGroup, s16 left, s16 right, u16 pitch, u32 attack, u32 release, u32 loops)
{
    return 0;
}

s32 PitchOfRate(s32 rate)
{
    // The pitch for a sample's own rate: the PS2's SPU2 plays 0x1000 as the sample's rate, so return that (the game scales
    // it by its own g_PitchScale)
    (void)rate;
    return 0x1000;
}

u32 ChannelBufferAddress(s32 channel)
{
    (void)channel;
    return 0;
}

void ReadMusic(s32 file, u32 offset, u32 size)
{
    (void)file;
    (void)offset;
    (void)size;
}

s32 StreamMusic(s32 file, s32 channel, u32 voiceAndGroup, u16 pitch, bool once)
{
    // The music "streams" instantly: the channel is ready and playing (silently)
    (void)file;
    (void)channel;
    (void)voiceAndGroup;
    (void)pitch;
    (void)once;
    return channel;
}

void InterleaveMusic(s32 channel, u32 blockSize)
{
    (void)channel;
    (void)blockSize;
}

void AddMusicChannel(s32 child, s32 parent, u32 voiceAndGroup, u32 soundAddress)
{
    (void)child;
    (void)parent;
    (void)voiceAndGroup;
    (void)soundAddress;
}

void SetMusicEnd(s32 channel, u32 end)
{
    (void)channel;
    (void)end;
}

void PrepareMusic(s32 channel)
{
    (void)channel;
}

bool IsMusicReady(s32 channel)
{
    // The music is ready at once (no decoding/streaming delay on the stub)
    (void)channel;
    return true;
}

s32 PlayMusic(s32 channel)
{
    return channel;
}

bool IsMusicPlaying(s32 channel)
{
    // The music keeps "playing" (silently) so the game never thinks it has ended on its own (a real decoder would report its
    // end; the stub stays playing until StopMusic is called)
    (void)channel;
    return true;
}

void StopMusic(s32 channel)
{
    (void)channel;
}

}
