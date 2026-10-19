#pragma once

#include "common.h"

// The desktop's sound memory shared by the streams' bank reads (stream.cpp) and the mixer (audio.cpp), the PS2's sound
// processor's memory: every sound's samples land in their own block tagged by the sound's number
namespace DesktopSound
{
// A bank's bytes go into a block of its own made here: the buffer returned (nullptr couldn't be made) is filled by the stream,
// the sound's samples stay playable until its voice is done even when the sound is read again
void* BankBuffer(u32 sound, u32 size);
}
