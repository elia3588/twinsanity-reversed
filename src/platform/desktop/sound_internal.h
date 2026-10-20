#pragma once

#include "common.h"

// The desktop's sound memory shared by the streams' bank reads (stream.cpp) and the mixer (audio.cpp), the PS2's sound
// processor's memory: every sound's samples land in their own block tagged by the sound's number
namespace DesktopSound
{
// A bank's bytes go into a block of its own made here: the buffer returned (nullptr couldn't be made) is filled by the stream,
// the sound's samples stay playable until its voice is done even when the sound is read again
void* BankBuffer(u32 sound, u32 size);
// The bank read whole: its samples' own end worked out (the block carrying the end flag), the read's padding after it not played
void BankLoaded(u32 sound, u32 size);

// The movie's own sound (Platform::Movie's player): 16 bit stereo frames at the mixer's own rate (the player resamples the PSS
// file's PCM to them) played over the voices at the movie's group volume. The mixer (audio.cpp's Mix) reads them; the player sets
// and clears them through these, which hold the device's lock meanwhile
void MovieSoundOpen(const s16* frames, u32 count, f32 volume);
void MovieSoundClose();
// The mixer's own state, which MovieSoundOpen sets and Mix reads
extern const s16* MovieFrames;
extern u32 MovieFrameCount;
extern u32 MovieFrameRead;
extern f32 MovieGain;
}
