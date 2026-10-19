#include "platform/audio.h"

#include "sound_internal.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "platform/stream.h"

#if defined(DESKTOP_SDL2)
#include <SDL.h>
// The Windows include's PlaySound to PlaySoundA macro takes our own sound function's name
#include <windows.h>
#undef PlaySound

// The desktop's sound processor: the SPU2's 48 voices, each playing a sound's samples of SPU2 ADPCM (blocks of 16 bytes
// decoding to 28 samples) mixed into the host's own audio device. A sound's samples land in their own block of sound memory
// (DesktopSound::BankBuffer below, filled by the streams' bank reads in stream.cpp) tagged by the sound's number; a play keys
// a voice on its sound's latest block at the volumes and pitch given (0x1000 the sample's own rate), with the group's scale
// (a word of 4.12 fixed point, 0x1000 full) multiplying the volumes, and the sample is let go by its own end block or a key
// off to fall at its release rate. The groups' volumes (1 the effects', 2 the second effects', 3 the music's, 4 the movies')
// come in as their number left by GroupShift

namespace Platform::Audio
{
namespace
{
// The device's own rate: the pitches are fractions of it (0x1000 a sample's own rate)
constexpr s32 MixRate = 48000;
// How many samples a mix is asked for at a time (about a frame's 21 ms)
constexpr u32 MixSamples = 1024;
// The envelope's own times: the attack a few thousandths over, the release an exponential fall whose halving is about the
// game's own release rate (0xD) scaled by the rate's pace (every four rates twice as long)
constexpr f32 AttackSeconds = 0.003f;
constexpr f32 ReleaseHalfSeconds = 0.008f;
// The play's own release (the game lets its sounds go at it)
constexpr u32 PlayReleaseRate = 0xD;
// The rate the release's pace is about (above twice as quick, twice as quick below, every four rates)
constexpr u32 ReleasePaceRate = 0xD;

// The SPU2's ADPCM prediction: a block's first byte's high four pick the coefficients (in 64ths over the two past samples)
constexpr s32 PredictCoeffs[5][2] = {
    {0, 0},
    {60, 0},
    {115, -52},
    {98, -55},
    {122, -60},
};

// A bank of sound memory: a sound's samples as the stream read them. A voice plays from its own bank, and so a bank still
// heard holds while the sound is read again (the map keeps the latest for the next play)
struct Bank
{
    std::vector<u8> samples;
};

// The sound memory's blocks by their sounds' numbers (the reads' banks land here, and DesktopSound::BankBuffer fills them)
std::unordered_map<u32, std::shared_ptr<const Bank>> g_Sounds;

// The sound memory's own addresses, the game's music buffers keep (sound.cpp keeps the values and nothing of ours does)
u32 g_MemoryAddress = 0x100000;

// A music's stream: the game's stream system's channel it runs by, its track's data the voices play (the track's whole data
// read here when it gets ready; on the PS2 the module streams the .mb's blocks itself). A mono track one voice, an interleaved
// track its sides' own voices, the parent reading and the child (the one of AddMusicChannel) playing the after half of it
struct MusicStream
{
    bool taken = false;
    bool prepared = false;
    bool playing = false;
    bool interleaved = false;
    u32 file = 0;
    u32 voice = 0;
    u32 childVoice = 0xFFFFFFFF;
    u32 group = 1;
    u32 pitch = 0x1000;
    bool once = false;
    u32 blockSize = 0x10000;
    u32 end = 0;
    bool hasRange = false;
    std::shared_ptr<const Bank> left;
    std::shared_ptr<const Bank> right;
};

constexpr u32 MusicStreamCount = 16;
MusicStream g_MusicStreams[MusicStreamCount];

// The music's read ranges (ReadMusic's file, offset and size of the tracks to come) by the streams' own files
std::map<u32, std::pair<u32, u32>> g_MusicRanges;

MusicStream* MusicStreamAt(s32 channel)
{
    return channel >= 0 && channel < static_cast<s32>(MusicStreamCount) ? &g_MusicStreams[channel] : nullptr;
}

// A voice's envelope: risen at its attack, held while its sample sounds, fallen when it is let go of, and off
enum Envelope : u8
{
    EnvelopeOff = 0,
    EnvelopeAttack,
    EnvelopeHolding,
    EnvelopeRelease,
};

// A voice: the sound's samples it plays and where in them (the place by whole samples and the pitch's fraction of one, the
// decode's own two past samples and the block's decoded window of 28), its volumes, group and envelope. The voice's own bits
// (whose number, its use, the music's taking of it) are the game's SoundVoice's
struct Voice
{
    bool taken = false;
    std::shared_ptr<const Bank> bank = nullptr;
    u32 group = 1;
    s32 left = 0;
    s32 right = 0;
    u32 pitch = 0x1000;
    f32 releaseFactor = 1.0f;
    f32 releaseScale = 1.0f;
    // The plays' count left (0 until it's let go of)
    u32 loopsLeft = 1;
    u32 block = 0;
    u32 loopBlock = 0;
    u32 position = 0;
    u32 fraction = 0;
    s32 old1 = 0;
    s32 old2 = 0;
    s16 window[28];
    s16 windowNext[28];
    u32 windowBlock = 0xFFFFFFFF;
    u32 windowNextBlock = 0xFFFFFFFF;
    f32 level = 0.0f;
    Envelope envelope = EnvelopeOff;
    // The music's own voice (its stream's), held still while the game's paused
    bool stream = false;
};

static_assert(Platform::Audio::Voices == 48, "the mixer's voices are the sound processor's own");
Voice g_VoiceTable[Voices];

// The groups' scales by word of 4.12 fixed point (0x1000 full); index 0 without a group of its own
constexpr f32 GroupFull = static_cast<f32>(FullGroupVolume);
f32 g_GroupScales[5][2];

// The movies', the menus' and the speech's own group (the game's MovieGroup as SoundGroupOf makes it): the pause's ramp leaves
// it sounding while the effects and music go silent
constexpr u32 MovieGroupNumber = 4;

u32 g_Device = 0;
bool g_DeviceMade = false;

// The game's own pause (MsPauseAll to MsResumeAll): the mix's effects and music go silent by a short ramp over and are held
// still, while the movie's own sounds (the movies', the menus' and the speech) keep sounding. The movie's end is the module's
// own resume: the game pauses the sound at a movie's start and lets it go again at the reclaim, the pairing done here
bool g_Paused = false;
// The ramp's own place: 1 sounding, 0 silent, stepped a few milliseconds over
f32 g_PauseGain = 1.0f;

// The mix's own dump (TWINSANITY_AUDIO_DUMP a path): the mixed frames into a raw stereo 16-bit file, for looking at the mix's
// contents without an ear
FILE* g_MixDump = nullptr;

// TEMP trace: the mixed frames run whole, and a voice's own last frame's sound, for telling a click's own voice and event
u64 g_MixFrames = 0;
f32 g_PrevVoiceOut[Voices][2];
f32 g_VoiceAvgJump[Voices];

// The sample's whole length in samples (blocks of 28)
u32 TotalSamplesOf(const Voice& voice)
{
    return voice.bank->samples.size() / 16 * 28;
}

// The voice's decode as its own start (a fresh play, or back at the loop's point)
void ResetDecode(Voice& voice)
{
    voice.position = 0;
    voice.fraction = 0;
    voice.block = 0;
    voice.loopBlock = 0;
    voice.windowBlock = 0xFFFFFFFF;
    voice.windowNextBlock = 0xFFFFFFFF;
    voice.old1 = 0;
    voice.old2 = 0;
}

// One of the sample's blocks into a window of 28: its first byte's low four the shift, its high four the prediction's
// filter over the voice's own two past samples, and its flags the loop's point and the history's start
void DecodeBlockInto(Voice& voice, u32 blockIndex, s16* window)
{
    if (voice.bank == nullptr)
    {
        for (u32 i = 0; i < 28; i++)
        {
            window[i] = 0;
        }

        return;
    }

    const std::vector<u8>& samples = voice.bank->samples;
    u32 at = blockIndex * 16;
    if (at + 16 > samples.size())
    {
        // A place past the sample's own end (a play of a short or empty bank) silent of its own
        for (u32 i = 0; i < 28; i++)
        {
            window[i] = 0;
        }

        return;
    }

    u32 shift = samples[at] & 0xF;
    u32 filter = (samples[at] >> 4) & 0xF;
    if (filter > 4)
    {
        filter = 0;
    }

    const s32* coeffs = PredictCoeffs[filter];
    u32 flags = samples[at + 1] & 0xF;
    if ((flags & 0x3) == 0x1)
    {
        // The loop's own point: the sample's plays come back here (the end's blocks carry the bit as well, not a loop's start)
        voice.loopBlock = blockIndex;
    }

    if ((flags & 0x4) != 0)
    {
        // A start of the sample's own: the history's past left out of it
        voice.old1 = 0;
        voice.old2 = 0;
    }

    for (u32 i = 0; i < 28; i++)
    {
        u8 byte = samples[at + 2 + i / 2];
        s32 nibble = (i & 1) != 0 ? static_cast<s32>(byte >> 4) : static_cast<s32>(byte & 0xF);
        if (nibble >= 8)
        {
            nibble -= 16;
        }

        s32 sample = (nibble << 12) >> shift;
        sample += (voice.old1 * coeffs[0] + voice.old2 * coeffs[1]) >> 6;
        if (sample > 0x7FFF)
        {
            sample = 0x7FFF;
        }
        else if (sample < -0x8000)
        {
            sample = -0x8000;
        }

        voice.old2 = voice.old1;
        voice.old1 = sample;
        window[i] = static_cast<s16>(sample);
    }
}

// The voice's two windows hold a block and the one after it (a place interpolated between the pair's); the windows are made
// as the places reach them, the history walking the sample's blocks once in their order
void EnsureWindows(Voice& voice, u32 blockIndex)
{
    u32 blocks = voice.bank->samples.size() / 16;
    if (voice.windowBlock == blockIndex)
    {
        return;
    }

    if (voice.windowNextBlock == blockIndex)
    {
        // Moving on to the after of the pair: its window becomes the voice's own, a new one made after it
        for (u32 i = 0; i < 28; i++)
        {
            voice.window[i] = voice.windowNext[i];
        }

        voice.windowBlock = blockIndex;
        if (blockIndex + 1 < blocks)
        {
            DecodeBlockInto(voice, blockIndex + 1, voice.windowNext);
            voice.windowNextBlock = blockIndex + 1;
        }
        else
        {
            voice.windowNextBlock = 0xFFFFFFFF;
        }

        return;
    }

    // A place of its own (a fresh play, a loop's jump): the pair made whole
    DecodeBlockInto(voice, blockIndex, voice.window);
    voice.windowBlock = blockIndex;
    if (blockIndex + 1 < blocks)
    {
        DecodeBlockInto(voice, blockIndex + 1, voice.windowNext);
        voice.windowNextBlock = blockIndex + 1;
    }
    else
    {
        voice.windowNextBlock = 0xFFFFFFFF;
    }
}

// The sample where the voice's place is
s16 SampleAt(Voice& voice)
{
    EnsureWindows(voice, voice.position / 28);
    return voice.window[voice.position % 28];
}

// The sample a place on after it (the next block's first over the pair's windows)
s16 SampleAhead(Voice& voice)
{
    EnsureWindows(voice, voice.position / 28);
    if (voice.position % 28 == 27)
    {
        return voice.windowNextBlock != 0xFFFFFFFF ? voice.windowNext[0] : 0;
    }

    return voice.window[voice.position % 28 + 1];
}

// The release's pace of a release rate (the rates below 0x20, 0xD the game's own common)
void SetReleaseRate(Voice& voice, u32 rate)
{
    voice.releaseScale = rate < 0x20 ? std::powf(2.0f, (static_cast<f32>(ReleasePaceRate) - static_cast<f32>(rate)) / 4.0f)
                                     : 1.0f;
}

// The voice let go of: its level falls exponentially, rate by rate
void StartRelease(Voice& voice)
{
    if (voice.envelope == EnvelopeOff || !voice.taken)
    {
        return;
    }

    voice.releaseFactor = std::powf(0.5f, 1.0f / (ReleaseHalfSeconds * voice.releaseScale * MixRate));
    if (voice.envelope != EnvelopeRelease)
    {
        // TEMP trace
        std::fprintf(stderr, "rel f=%.3f v=%u g=%u lvl=%.3f\n", static_cast<double>(g_MixFrames) / MixRate,
                     static_cast<u32>(&voice - g_VoiceTable), voice.group, voice.level);
        std::fflush(stderr);
    }

    voice.envelope = EnvelopeRelease;
}

// What a volume's code holds: 0 to 0x3FFF the size in the own phase, 0x4000 and up the other phase (the base less the size)
void ApplyVolume(s32 code, f32& sign, f32& size)
{
    if (code < 0)
    {
        // The other phase by less than the base (the game's own Phase of a volume's being under nothing)
        code = InvertedVolumeBase + code;
        if (code < 0)
        {
            code = 0;
        }
    }
    else if (code > InvertedVolumeBase)
    {
        code = InvertedVolumeBase;
    }

    if (code >= 0x4000)
    {
        sign = -1.0f;
        size = static_cast<f32>(InvertedVolumeBase - code);
    }
    else
    {
        sign = 1.0f;
        size = static_cast<f32>(code > MaxVolume ? MaxVolume : code);
    }
}

// The mix of every voice into the device's own frames (SDL's thread: the sound's bits are turned while the device is locked,
// and nothing here touches them otherwise, so their read stays whole)
void Mix(Uint8* stream, int bytes)
{
    s16* frame = reinterpret_cast<s16*>(stream);
    u32 frames = static_cast<u32>(bytes) / 4;
    const f32 attackStep = 1.0f / (AttackSeconds * MixRate);
    // The pause's own ramp: a few milliseconds to silence or back (a step of one frame)
    const f32 pauseStep = 1.0f / (0.005f * MixRate);
    for (u32 frameIndex = 0; frameIndex < frames; frameIndex++)
    {
        // The pause's own place stepped to what the game last asked for
        f32 pauseTarget = g_Paused ? 0.0f : 1.0f;
        if (g_PauseGain < pauseTarget)
        {
            g_PauseGain += pauseStep;
            if (g_PauseGain > pauseTarget)
            {
                g_PauseGain = pauseTarget;
            }
        }
        else if (g_PauseGain > pauseTarget)
        {
            g_PauseGain -= pauseStep;
            if (g_PauseGain < pauseTarget)
            {
                g_PauseGain = pauseTarget;
            }
        }

        f32 left = 0.0f;
        f32 right = 0.0f;
        for (u32 index = 0; index < Voices; index++)
        {
            Voice& voice = g_VoiceTable[index];
            if (!voice.taken || !voice.bank || voice.envelope == EnvelopeOff)
            {
                g_PrevVoiceOut[index][0] = 0.0f;
                g_PrevVoiceOut[index][1] = 0.0f;
                g_VoiceAvgJump[index] = 0.0f;
                continue;
            }

            f32 leftSign = 1.0f;
            f32 leftSize = 0.0f;
            ApplyVolume(voice.left, leftSign, leftSize);
            f32 rightSign = 1.0f;
            f32 rightSize = 0.0f;
            ApplyVolume(voice.right, rightSign, rightSize);
            f32 leftGain = leftSign * leftSize / MaxVolume * voice.level * g_GroupScales[voice.group][0];
            f32 rightGain = rightSign * rightSize / MaxVolume * voice.level * g_GroupScales[voice.group][1];
            // The pause's own ramp over the effects and music (group 4 the movies', the menus' and the speech, which stay)
            if (voice.group != MovieGroupNumber)
            {
                leftGain *= g_PauseGain;
                rightGain *= g_PauseGain;
            }

            // The place moved on by the pitch (a pitch of 0x1000 a whole sample on). The music's own voice is held still while
            // the game's paused, so the track takes up where it left off
            if (!(g_Paused && voice.stream))
            {
                voice.fraction += voice.pitch;
                voice.position += voice.fraction >> 12;
                voice.fraction &= 0xFFF;
                u32 total = TotalSamplesOf(voice);
                if (voice.position >= total)
                {
                    // The sample's whole length through: again at the loop's point by its plays left (a play's count of 0 until
                    // it's let go of), else let go of at its release (the end block's key off)
                    if (voice.loopBlock < total / 28 && (voice.loopsLeft == 0 || voice.loopsLeft > 1))
                    {
                        if (voice.loopsLeft != 0)
                        {
                            voice.loopsLeft--;
                        }

                        voice.position = voice.loopBlock * 28;
                        voice.fraction = 0;
                        voice.windowBlock = 0xFFFFFFFF;
                        voice.windowNextBlock = 0xFFFFFFFF;
                        voice.old1 = 0;
                        voice.old2 = 0;
                        // TEMP trace
                        std::fprintf(stderr, "loop f=%.3f v=%u g=%u to=%u\n", static_cast<double>(g_MixFrames) / MixRate,
                                     index, voice.group, voice.loopBlock * 28);
                        std::fflush(stderr);
                    }
                    else
                    {
                        ResetDecode(voice);
                        StartRelease(voice);
                    }
                }
            }

            s32 sample = SampleAt(voice);
            if (voice.fraction != 0)
            {
                // The place between the samples interpolated (the pitch's fraction of the step over the one after)
                s32 next = SampleAhead(voice);
                sample += static_cast<s32>(static_cast<f32>(next - sample) * static_cast<f32>(voice.fraction) * (1.0f / 4096.0f));
            }

            left += static_cast<f32>(sample) * leftGain;
            right += static_cast<f32>(sample) * rightGain;

            // TEMP trace: a voice's own sound jumping far over its own recent walk (a click), with where and in what state
            {
                f32 nowLeft = static_cast<f32>(sample) * leftGain;
                f32 nowRight = static_cast<f32>(sample) * rightGain;
                f32 dLeft = std::fabs(nowLeft - g_PrevVoiceOut[index][0]);
                f32 dRight = std::fabs(nowRight - g_PrevVoiceOut[index][1]);
                f32 jump = dLeft > dRight ? dLeft : dRight;
                if (jump > 3000.0f && jump > 10.0f * g_VoiceAvgJump[index])
                {
                    std::fprintf(stderr, "click f=%.3f v=%u g=%u pos=%u inblk=%u env=%u lvl=%.3f prev=%.0f now=%.0f avg=%.0f\n",
                                 static_cast<double>(g_MixFrames) / MixRate, index, voice.group, voice.position,
                                 voice.position % 28, static_cast<u32>(voice.envelope), voice.level,
                                 static_cast<double>(g_PrevVoiceOut[index][0]), static_cast<double>(nowLeft),
                                 static_cast<double>(g_VoiceAvgJump[index]));
                    std::fflush(stderr);
                }

                g_VoiceAvgJump[index] = g_VoiceAvgJump[index] * 0.999f + jump * 0.001f;
                g_PrevVoiceOut[index][0] = nowLeft;
                g_PrevVoiceOut[index][1] = nowRight;
            }

            // The envelope's rise and fall (the sample sounding while it falls)
            switch (voice.envelope)
            {
            case EnvelopeAttack:
                voice.level += attackStep;
                if (voice.level >= 1.0f)
                {
                    voice.level = 1.0f;
                    voice.envelope = EnvelopeHolding;
                }
                break;
            case EnvelopeRelease:
                voice.level *= voice.releaseFactor;
                if (voice.level < 0.001f)
                {
                    voice.level = 0.0f;
                    voice.taken = false;
                    voice.bank = nullptr;
                    voice.envelope = EnvelopeOff;
                    // TEMP trace
                    std::fprintf(stderr, "free f=%.3f v=%u g=%u\n", static_cast<double>(g_MixFrames) / MixRate, index,
                                 voice.group);
                    std::fflush(stderr);
                }
                break;
            default:
                break;
            }
        }

        s32 outLeft = static_cast<s32>(left);
        if (outLeft > 0x7FFF)
        {
            outLeft = 0x7FFF;
        }
        else if (outLeft < -0x8000)
        {
            outLeft = -0x8000;
        }

        s32 outRight = static_cast<s32>(right);
        if (outRight > 0x7FFF)
        {
            outRight = 0x7FFF;
        }
        else if (outRight < -0x8000)
        {
            outRight = -0x8000;
        }

        frame[frameIndex * 2] = static_cast<s16>(outLeft);
        frame[frameIndex * 2 + 1] = static_cast<s16>(outRight);
        g_MixFrames++;
    }

    if (g_MixDump != nullptr && frames != 0)
    {
        std::fwrite(stream, 1, static_cast<size_t>(frames) * 4, g_MixDump);
    }

    // TEMP trace: the mix's own heartbeat and who holds voices (the callback running shows as the seconds counting up)
    static u32 acc = 0;
    static u32 secs = 0;
    acc += frames;
    if (acc >= MixRate)
    {
        acc -= MixRate;
        secs++;
        if (secs <= 60)
        {
            std::fprintf(stderr, "mix %u:", secs);
            for (u32 v = 0; v < Voices; v++)
            {
                if (!g_VoiceTable[v].taken)
                {
                    continue;
                }

                std::fprintf(stderr, " v%u{p%u/28 e%u l%.2f L%d g%u}", v, g_VoiceTable[v].position,
                             u32(g_VoiceTable[v].envelope), g_VoiceTable[v].level, g_VoiceTable[v].left,
                             g_VoiceTable[v].group);
            }

            std::fprintf(stderr, "\n");
            std::fflush(stderr);
        }
    }
}

void Callback(void*, Uint8* stream, int bytes)
{
    Mix(stream, bytes);
}

// The host's own device opened once (stereo 16-bit at the sound processor's rate, the mix's samples its buffer)
void MakeDevice()
{
    if (g_DeviceMade)
    {
        return;
    }

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
    {
        return;
    }

    g_DeviceMade = true;
    SDL_AudioSpec spec;
    std::memset(&spec, 0, sizeof(spec));
    spec.freq = MixRate;
    spec.format = AUDIO_S16SYS;
    spec.channels = 2;
    spec.samples = MixSamples;
    spec.callback = Callback;
    g_Device = SDL_OpenAudioDevice(nullptr, 0, &spec, nullptr, 0);
    if (g_Device != 0)
    {
        SDL_PauseAudioDevice(g_Device, 0);
    }

    const char* dump = std::getenv("TWINSANITY_AUDIO_DUMP");
    if (dump != nullptr && *dump != 0)
    {
        g_MixDump = std::fopen(dump, "wb");
    }
}
} // namespace
} // namespace Platform::Audio

// The streams' side of the sound memory (stream.cpp reads the banks' samples into its blocks)
namespace DesktopSound
{
void* BankBuffer(u32 sound, u32 size)
{
    if (size == 0)
    {
        return nullptr;
    }

    auto* bank = new Platform::Audio::Bank();
    bank->samples.resize(size);
    Platform::Audio::g_Sounds[sound] = std::shared_ptr<const Platform::Audio::Bank>(bank);
    return bank->samples.data();
}
} // namespace DesktopSound

namespace Platform::Audio
{
void Reset()
{
    if (!g_DeviceMade)
    {
        MakeDevice();
    }

    if (g_Device != 0)
    {
        SDL_LockAudioDevice(g_Device);
    }

    for (u32 i = 0; i < Voices; i++)
    {
        Voice& voice = g_VoiceTable[i];
        voice.taken = false;
        voice.bank = nullptr;
        voice.group = 1;
        voice.left = 0;
        voice.right = 0;
        voice.pitch = 0x1000;
        voice.releaseFactor = 1.0f;
        voice.releaseScale = 1.0f;
        voice.loopsLeft = 1;
        voice.position = 0;
        voice.fraction = 0;
        voice.block = 0;
        voice.loopBlock = 0;
        voice.windowBlock = 0xFFFFFFFF;
        voice.windowNextBlock = 0xFFFFFFFF;
        voice.old1 = 0;
        voice.old2 = 0;
        voice.level = 0.0f;
        voice.envelope = EnvelopeOff;
        voice.stream = false;
    }

    for (u32 group = 0; group < 5; group++)
    {
        g_GroupScales[group][0] = 1.0f;
        g_GroupScales[group][1] = 1.0f;
    }

    // The sound processor started over: unpaused and sounding
    g_Paused = false;
    g_PauseGain = 1.0f;

    if (g_Device != 0)
    {
        SDL_UnlockAudioDevice(g_Device);
    }

    // The sound processor started over: its sounds' memory of banks as well
    g_Sounds.clear();
    g_MusicRanges.clear();
    for (u32 i = 0; i < MusicStreamCount; i++)
    {
        g_MusicStreams[i] = MusicStream();
    }
}

void PauseAll()
{
    // The game's own pause (MsPauseAll): the effects and music ramp to silence and hold, the movies', the menus' and the
    // speech's own sounds (group 4) keep sounding. The movie's end is the module's own resume (ReclaimFromMovie below)
    g_Paused = true;
    // TEMP trace
    std::fprintf(stderr, "pause\n");
    std::fflush(stderr);
}

void ResumeAll()
{
    g_Paused = false;
    // TEMP trace
    std::fprintf(stderr, "resume\n");
    std::fflush(stderr);
}

s32 LendToMovie()
{
    // The movie's own sound the platform lends to (the music's voices kept); none made of it yet
    return 0;
}

s32 ReclaimFromMovie()
{
    // The movie's end: the module's own resume. The game pauses the sound at a movie's start (game/movie.cpp's StateStarting)
    // and never pairs it with a ResumeSound of its own, so the letting go has to be here or the pause would leak on over the
    // front end's menus and silence them
    g_Paused = false;
    // TEMP trace
    std::fprintf(stderr, "reclaim-resume\n");
    std::fflush(stderr);
    return 0;
}

void CompactSoundMemory()
{
    // A step of the sound memory's sounds moved together (the game's own): the desktop's memory is the banks' own map, whose
    // sounds released are dropped with them
}

void SetGroupVolume(s32 group, u32 left, u32 right)
{
    // The group's own number the word's top half carries (1 to 4)
    group = static_cast<s32>((static_cast<u32>(group) >> GroupShift) & 0xFFFF);
    if (group < 0 || group >= 5)
    {
        return;
    }

    f32 scaleLeft = static_cast<f32>(left > 2u * FullGroupVolume ? 2u * FullGroupVolume : left) / GroupFull;
    f32 scaleRight = static_cast<f32>(right > 2u * FullGroupVolume ? 2u * FullGroupVolume : right) / GroupFull;
    if (g_Device != 0)
    {
        SDL_LockAudioDevice(g_Device);
        g_GroupScales[group][0] = scaleLeft;
        g_GroupScales[group][1] = scaleRight;
        SDL_UnlockAudioDevice(g_Device);
    }
}

s32 SetVoiceVolume(s32 voice, s16 left, s16 right)
{
    if (voice < 0 || voice >= Voices)
    {
        return -1;
    }

    if (g_Device != 0)
    {
        SDL_LockAudioDevice(g_Device);
        g_VoiceTable[voice].left = left;
        g_VoiceTable[voice].right = right;
        SDL_UnlockAudioDevice(g_Device);
    }

    return 0;
}

s32 SetVoicePitch(s32 voice, u16 pitch)
{
    if (voice < 0 || voice >= Voices)
    {
        return -1;
    }

    if (g_Device != 0)
    {
        SDL_LockAudioDevice(g_Device);
        g_VoiceTable[voice].pitch = pitch;
        SDL_UnlockAudioDevice(g_Device);
    }

    return 0;
}

void SetVoiceReverb(s32 voice, bool on)
{
    // The voice's send to its core's reverb (the reverb's own step to come)
    (void)voice;
    (void)on;
}

void ReleaseVoice(s32 voice, u32 rate)
{
    if (voice < 0 || voice >= Voices || g_Device == 0)
    {
        return;
    }

    SDL_LockAudioDevice(g_Device);
    Voice& item = g_VoiceTable[voice];
    if (item.taken)
    {
        SetReleaseRate(item, rate);
        StartRelease(item);
    }

    SDL_UnlockAudioDevice(g_Device);
}

s32 IsVoiceFree(s32 voice)
{
    if (voice < 0 || voice >= Voices)
    {
        return -1;
    }

    if (g_Device == 0)
    {
        return 1;
    }

    SDL_LockAudioDevice(g_Device);
    s32 isFree = g_VoiceTable[voice].taken ? 0 : 1;
    SDL_UnlockAudioDevice(g_Device);
    return isFree;
}

void SetReverb(s32 core, s32 mode, u16 depthLeft, u16 depthRight, u16 delay, u16 feedback)
{
    // The core's reverb's mode and its depths, delays and give-back (the reverb's own step to come)
    (void)core;
    (void)mode;
    (void)depthLeft;
    (void)depthRight;
    (void)delay;
    (void)feedback;
}

void ClearReverb(s32 core)
{
    (void)core;
}

void SetReverbVolume(s32 core, s16 left, s16 right)
{
    (void)core;
    (void)left;
    (void)right;
}

u32 ReserveSoundMemory(u32 size)
{
    // A place of the sound memory its own (the game's music buffers keep the values returned)
    u32 place = g_MemoryAddress;
    g_MemoryAddress = place + size;
    return place;
}

void SoundBankLoaded(u16 bank)
{
    // The bank's samples all in the sound memory (the stream's read put them there)
    (void)bank;
}

void ReserveSounds(u16 count)
{
    // The sound processor's memory's sounds the table keeps (the desktop's banks are the table, made as they are read)
    (void)count;
}

void ReleaseSound(u32 sound)
{
    // The sound's samples' memory let go of: the voices playing from it hold it while they do (their own banks)
    g_Sounds.erase(sound);
}

// The voice keyed onto a sound's samples: everything the play and the music's streams set their voices by. The volumes kept
// (a bringVoice of false) for the music's, whose volumes the game sets over its voices before the play itself
void KeyOnVoice(Voice& itemVoice, const std::shared_ptr<const Bank>& bank, u32 group, s32 left, s32 right, u32 pitch,
                u32 releaseRate, u32 loops, bool ownVolumes)
{
    itemVoice.taken = true;
    itemVoice.bank = bank;
    itemVoice.group = group < 5 ? group : 0;
    // The music's own voice streams (its volumes the game's over it), so the pause holds it still; a play's own does not
    itemVoice.stream = !ownVolumes;
    if (ownVolumes)
    {
        itemVoice.left = left;
        itemVoice.right = right;
    }

    itemVoice.pitch = pitch;
    SetReleaseRate(itemVoice, releaseRate);
    // A play's count of its own (so many plays, or 0 of them until the voice is let go of)
    itemVoice.loopsLeft = loops;
    // TEMP trace
    {
        u32 was = static_cast<u32>(itemVoice.envelope);
        ResetDecode(itemVoice);
        itemVoice.level = 0.0f;
        itemVoice.envelope = EnvelopeAttack;
        std::fprintf(stderr, "ko f=%.3f v=%u g=%u pitch=0x%X loops=%u was=%u\n", static_cast<double>(g_MixFrames) / MixRate,
                     static_cast<u32>(&itemVoice - g_VoiceTable), group, pitch, loops, was);
        std::fflush(stderr);
    }
}

void TerminateVoice(Voice& itemVoice)
{
    SetReleaseRate(itemVoice, PlayReleaseRate);
    StartRelease(itemVoice);
}

// TEMP: a sound's own samples decoded whole into a WAV (TWINSANITY_SFX_DUMP a folder), the mixer's own decode with the pitch's
// step left out, so a click's own place in the sample's blocks can be heard and counted
void DumpSoundWav(const char* folder, u32 sound, const std::shared_ptr<const Bank>& bank)
{
    char path[512];
    std::snprintf(path, sizeof(path), "%s/sfx_%u.wav", folder, sound);
    FILE* file = std::fopen(path, "wb");
    if (file == nullptr)
    {
        return;
    }

    u32 blocks = static_cast<u32>(bank->samples.size()) / 16;
    std::vector<s16> pcm;
    pcm.reserve(static_cast<size_t>(blocks) * 28);
    Voice voice;
    voice.bank = bank;
    s16 window[28];
    for (u32 block = 0; block < blocks; block++)
    {
        DecodeBlockInto(voice, block, window);
        for (u32 i = 0; i < 28; i++)
        {
            pcm.push_back(window[i]);
        }
    }

    u32 dataBytes = static_cast<u32>(pcm.size()) * 2;
    u8 header[44] = {0};
    u32 riff = 36 + dataBytes;
    u32 fmtSize = 16;
    u16 audioFormat = 1;
    u16 channels = 1;
    u32 rate = static_cast<u32>(MixRate);
    u32 byteRate = static_cast<u32>(MixRate) * 2;
    u16 blockAlign = 2;
    u16 bits = 16;
    std::memcpy(header, "RIFF", 4);
    std::memcpy(header + 4, &riff, 4);
    std::memcpy(header + 8, "WAVEfmt ", 8);
    std::memcpy(header + 16, &fmtSize, 4);
    std::memcpy(header + 20, &audioFormat, 2);
    std::memcpy(header + 22, &channels, 2);
    std::memcpy(header + 24, &rate, 4);
    std::memcpy(header + 28, &byteRate, 4);
    std::memcpy(header + 32, &blockAlign, 2);
    std::memcpy(header + 34, &bits, 2);
    std::memcpy(header + 36, "data", 4);
    std::memcpy(header + 40, &dataBytes, 4);
    std::fwrite(header, 1, sizeof(header), file);
    std::fwrite(pcm.data(), 2, pcm.size(), file);
    std::fclose(file);
}

s32 PlaySound(u32 sound, u32 voiceAndGroup, s16 left, s16 right, u16 pitch, u32 attack, u32 release, u32 loops)
{
    if (loops > 0xFFFF)
    {
        return -2;
    }

    s32 voice = static_cast<s32>(voiceAndGroup & 0xFFFF);
    if (voice < 0 || voice >= Voices)
    {
        return -1;
    }

    auto item = g_Sounds.find(sound);
    if (item == g_Sounds.end())
    {
        return -1;
    }

    if (g_Device == 0)
    {
        return -1;
    }

    // TEMP: the sound's own samples into a WAV the first time it plays, for hearing a click's own place
    {
        static const char* dumpFolder = std::getenv("TWINSANITY_SFX_DUMP");
        static std::unordered_map<u32, bool> dumped;
        if (dumpFolder != nullptr && *dumpFolder != 0 && dumped.find(sound) == dumped.end())
        {
            dumped[sound] = true;
            DumpSoundWav(dumpFolder, sound, item->second);
        }
    }

    SDL_LockAudioDevice(g_Device);
    KeyOnVoice(g_VoiceTable[voice], item->second, (voiceAndGroup >> GroupShift) & 0xFFFF, left, right, pitch,
               PlayReleaseRate, loops, true);
    SDL_UnlockAudioDevice(g_Device);
    // TEMP trace: the front end's and the movie's sounds (the ids 0x8000 and up) with what they were keyed by
    if (sound >= 0x8000)
    {
        static u32 plays = 0;
        if (plays < 120)
        {
            plays++;
            std::fprintf(stderr, "ps s=%u v=%d g=%u L=%d R=%d pitch=0x%X loops=%u bytes=%u paused=%u\n", sound, voice,
                         (voiceAndGroup >> GroupShift) & 0xFFFF, left, right, pitch, loops,
                         static_cast<u32>(item->second->samples.size()), g_Paused ? 1u : 0u);
            std::fflush(stderr);
        }
    }

    (void)attack;
    (void)release;
    return 0;
}

// The pitch for a sample's own rate (a rate of 48000 the own, a pitch of 0x1000)
s32 PitchOfRate(s32 rate)
{
    return static_cast<s32>((static_cast<s64>(rate) << 12) / MixRate);
}

u32 ChannelBufferAddress(s32 channel)
{
    // The channel's buffer in the sound memory (the music only keeps the value in its player)
    (void)channel;
    return g_MemoryAddress - 0x10000;
}

void ReadMusic(s32 file, u32 offset, u32 size)
{
    // Where the track's data is in the file (the streams of it pick it up at their own start)
    g_MusicRanges[static_cast<u32>(file)] = {static_cast<u32>(offset), static_cast<u32>(size)};
}

// The track's data read whole into the sides' own banks, the interleaved's split by its halves and trimmed by the end the
// game set in the last pair (the file the music's bank opened read by the streams' own channel)
void LoadMusicStream(MusicStream& stream)
{
    if (!stream.hasRange)
    {
        return;
    }

    auto range = g_MusicRanges.find(stream.file);
    if (range == g_MusicRanges.end())
    {
        return;
    }

    u32 offset = range->second.first;
    u32 whole = range->second.second;
    std::vector<u8> wholeData(whole);
    Platform::Stream::Read(0, static_cast<s32>(stream.file), offset, whole, wholeData.data());

    if (stream.interleaved && stream.blockSize >= 32 && whole >= 2 * stream.blockSize)
    {
        // The sides' own halves of the pairs (left first, the halves' own size the game gave). The final pair is valid
        // through the end the game told (as many bytes of both the sides of it), the rest of the file's range zeros after it
        u32 half = stream.blockSize;
        u32 valid = stream.end != 0 && stream.end < half ? stream.end : half;
        u32 pairs = whole / (2 * half);
        u32 side = (pairs - 1) * half + valid;
        side -= side % 16;
        auto* leftBank = new Bank();
        auto* rightBank = new Bank();
        leftBank->samples.resize(side);
        rightBank->samples.resize(side);
        for (u32 i = 0; i < side; i++)
        {
            leftBank->samples[i] = wholeData[i / half * 2 * half + i % half];
        }

        for (u32 i = 0; i < side; i++)
        {
            rightBank->samples[i] = wholeData[i / half * 2 * half + half + i % half];
        }

        stream.left = std::shared_ptr<const Bank>(leftBank);
        stream.right = std::shared_ptr<const Bank>(rightBank);
    }
    else
    {
        auto* bank = new Bank();
        bank->samples.swap(wholeData);
        stream.left = std::shared_ptr<const Bank>(bank);
        stream.right = nullptr;
    }

    stream.prepared = true;
}

s32 StreamMusic(s32 file, s32 channel, u32 voiceAndGroup, u16 pitch, bool once)
{
    // The stream of the game's own channel number told its voice, group, pitch and whether it plays once or loops
    MusicStream* stream = MusicStreamAt(channel);
    if (stream == nullptr)
    {
        return channel;
    }

    stream->taken = true;
    stream->prepared = false;
    stream->playing = false;
    stream->file = static_cast<u32>(file);
    stream->voice = static_cast<u32>(voiceAndGroup & 0xFFFF);
    stream->group = (voiceAndGroup >> GroupShift) & 0xFFFF;
    stream->pitch = pitch;
    stream->once = once;
    stream->hasRange = g_MusicRanges.find(stream->file) != g_MusicRanges.end();
    // The stream's shape starts over (a channel reused by the next track of its own shape)
    stream->interleaved = false;
    stream->blockSize = 0x10000;
    stream->end = 0;
    stream->childVoice = 0xFFFFFFFF;
    stream->left = nullptr;
    stream->right = nullptr;
    return channel;
}

void InterleaveMusic(s32 channel, u32 blockSize)
{
    MusicStream* stream = MusicStreamAt(channel);
    if (stream != nullptr)
    {
        stream->interleaved = true;
        stream->blockSize = blockSize;
    }
}

void AddMusicChannel(s32 child, s32 parent, u32 voiceAndGroup, u32 soundAddress)
{
    // The child's voice plays the parent's track's after halves (the data made into both the voices' banks when ready)
    MusicStream* stream = MusicStreamAt(parent);
    if (stream != nullptr)
    {
        stream->childVoice = static_cast<u32>(voiceAndGroup & 0xFFFF);
    }

    MusicStream* childStream = MusicStreamAt(child);
    if (childStream != nullptr)
    {
        *childStream = MusicStream();
        childStream->taken = true;
    }

    (void)soundAddress;
}

void SetMusicEnd(s32 channel, u32 end)
{
    MusicStream* stream = MusicStreamAt(channel);
    if (stream != nullptr)
    {
        stream->end = end;
    }
}

void PrepareMusic(s32 channel)
{
    // The stream's data read and ready to key on (a track without its data comes ready silent, so the player moves on)
    MusicStream* stream = MusicStreamAt(channel);
    if (stream != nullptr && !stream->prepared)
    {
        LoadMusicStream(*stream);
        stream->prepared = true;
    }
}

bool IsMusicReady(s32 channel)
{
    MusicStream* stream = MusicStreamAt(channel);
    return stream == nullptr ? true : stream->prepared;
}

s32 PlayMusic(s32 channel)
{
    MusicStream* stream = MusicStreamAt(channel);
    if (stream == nullptr)
    {
        return channel;
    }

    if (g_Device != 0)
    {
        SDL_LockAudioDevice(g_Device);
        if (stream->left != nullptr && stream->voice < Voices)
        {
            KeyOnVoice(g_VoiceTable[stream->voice], stream->left, stream->group, 0, 0, stream->pitch, PlayReleaseRate,
                       stream->once ? 1u : 0u, false);
        }

        if (stream->interleaved && stream->right != nullptr && stream->childVoice < Voices)
        {
            KeyOnVoice(g_VoiceTable[stream->childVoice], stream->right, stream->group, 0, 0, stream->pitch,
                       PlayReleaseRate, stream->once ? 1u : 0u, false);
        }

        SDL_UnlockAudioDevice(g_Device);
    }

    stream->playing = true;
    // TEMP trace
    static u32 played = 0;
    if (played < 20)
    {
        played++;
        std::fprintf(stderr, "music play ch=%d file=%u voice=%u child=%u group=%u once=%d left=%u right=%u\n", channel,
                     stream->file, stream->voice, stream->childVoice, stream->group, stream->once ? 1 : 0,
                     stream->left != nullptr ? static_cast<u32>(stream->left->samples.size()) : 0,
                     stream->right != nullptr ? static_cast<u32>(stream->right->samples.size()) : 0);
        std::fflush(stderr);
    }

    return channel;
}

bool IsMusicPlaying(s32 channel)
{
    // The music sounds while a voice of its own still does (a track once through ends of its own, a looping one not)
    MusicStream* stream = MusicStreamAt(channel);
    if (stream == nullptr || !stream->playing)
    {
        return false;
    }

    if (g_Device == 0)
    {
        return true;
    }

    SDL_LockAudioDevice(g_Device);
    bool sounding = (stream->left != nullptr && stream->voice < Voices && g_VoiceTable[stream->voice].taken) ||
                    (stream->interleaved && stream->right != nullptr && stream->childVoice < Voices &&
                     g_VoiceTable[stream->childVoice].taken);
    SDL_UnlockAudioDevice(g_Device);
    if (!sounding)
    {
        // The track through: the stream stopped with it
        stream->playing = false;
    }

    return sounding;
}

void StopMusic(s32 channel)
{
    MusicStream* stream = MusicStreamAt(channel);
    if (stream == nullptr)
    {
        return;
    }

    stream->playing = false;
    if (g_Device != 0)
    {
        SDL_LockAudioDevice(g_Device);
        if (stream->voice < Voices && g_VoiceTable[stream->voice].taken)
        {
            TerminateVoice(g_VoiceTable[stream->voice]);
        }

        if (stream->interleaved && stream->childVoice < Voices && g_VoiceTable[stream->childVoice].taken)
        {
            TerminateVoice(g_VoiceTable[stream->childVoice]);
        }

        SDL_UnlockAudioDevice(g_Device);
    }

    *stream = MusicStream();
}
} // namespace Platform::Audio
#else
// Without the host's audio device (no SDL2's audio), no sound out but the game's calls kept
namespace DesktopSound
{
void* BankBuffer(u32, u32)
{
    return nullptr;
}
} // namespace DesktopSound

namespace Platform::Audio
{
void Reset() {}
void PauseAll() {}
void ResumeAll() {}
s32 LendToMovie() { return 0; }
s32 ReclaimFromMovie() { return 0; }
void CompactSoundMemory() {}
void SetGroupVolume(s32, u32, u32) {}
s32 SetVoiceVolume(s32, s16, s16) { return 0; }
s32 SetVoicePitch(s32, u16) { return 0; }
void SetVoiceReverb(s32, bool) {}
void ReleaseVoice(s32, u32) {}
s32 IsVoiceFree(s32) { return -1; }
void SetReverb(s32, s32, u16, u16, u16, u16) {}
void ClearReverb(s32) {}
void SetReverbVolume(s32, s16, s16) {}
u32 ReserveSoundMemory(u32) { return 0; }
void SoundBankLoaded(u16) {}
void ReserveSounds(u16) {}
void ReleaseSound(u32) {}
s32 PlaySound(u32, u32, s16, s16, u16, u32, u32, u32) { return 0; }
s32 PitchOfRate(s32 rate) { return rate; }
u32 ChannelBufferAddress(s32) { return 0; }
void ReadMusic(s32, u32, u32) {}
s32 StreamMusic(s32 file, s32 channel, u32, u16, bool) { (void)file; return channel; }
void InterleaveMusic(s32, u32) {}
void AddMusicChannel(s32, s32, u32, u32) {}
void SetMusicEnd(s32, u32) {}
void PrepareMusic(s32) {}
bool IsMusicReady(s32) { return true; }
s32 PlayMusic(s32 channel) { return channel; }
bool IsMusicPlaying(s32) { return true; }
void StopMusic(s32) {}
} // namespace Platform::Audio
#endif
