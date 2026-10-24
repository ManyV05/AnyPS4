// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <libatrac9.h>

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "core/libraries/ngs2/ngs2.h"
#include "core/libraries/ngs2/ngs2_sampler.h"
#include "core/libraries/ngs2/ngs2_mastering.h"
#include "core/libraries/ngs2/ngs2_custom.h"
#include "core/libraries/ngs2/ngs2_error.h"
#include "core/libraries/ngs2/ngs2_geom.h"
#include "core/libraries/ngs2/ngs2_impl.h"
#include "core/libraries/ngs2/ngs2_pan.h"
#include "core/libraries/ngs2/ngs2_report.h"

namespace Libraries::Ngs2 {

namespace {
// Synthetic handles for racks/voices so games get stable, nonzero values instead of
// reading back uninitialized memory. Racks are spaced far apart so per-voice offsets
// derived from (rack, voiceIndex) never collide.
std::atomic<u32> next_rack_slot{1};

uintptr_t MakeRackHandle() {
    return 0x4E530000'00000000ULL | (static_cast<uintptr_t>(next_rack_slot++) << 20);
}

uintptr_t MakeVoiceHandle(uintptr_t rack_handle, u32 voice_index) {
    return rack_handle + 0x10 + static_cast<uintptr_t>(voice_index) * 8;
}

// Voice state flag values follow the public NGS2 SDK layout.
constexpr u32 NGS2_VOICE_STATE_FLAG_INUSE = 1u << 0;
constexpr u32 NGS2_VOICE_STATE_FLAG_ACTIVE = 1u << 1;
constexpr u32 NGS2_VOICE_STATE_FLAG_PLAYING = 1u << 2;

// Without a real synthesizer voices neither start nor finish, so any game loop that
// polls for either condition would spin forever. Alternate between "playing" and
// "idle" so every wait-for-state poll terminates within two reads.
std::atomic<u32> voice_state_poll{0};

u32 NextVoiceStateFlags() {
    return (voice_state_poll++ & 1)
               ? (NGS2_VOICE_STATE_FLAG_INUSE | NGS2_VOICE_STATE_FLAG_ACTIVE |
                  NGS2_VOICE_STATE_FLAG_PLAYING)
               : 0;
}

// Codec unit layout: bytes/samples per smallest decodable unit.
struct WaveformUnit {
    u32 size;
    u32 samples;
};

// Decode the 4-byte ATRAC9 config blob (big-endian bit fields:
// sync:8, srIndex:4, chConfig:3, valid:1, frameBytes-1:11, superframeShift:2).
bool DecodeAt9Config(u32 config, WaveformUnit& out) {
    const u8 b0 = config & 0xFF;
    const u8 b1 = (config >> 8) & 0xFF;
    const u8 b2 = (config >> 16) & 0xFF;
    const u8 b3 = (config >> 24) & 0xFF;
    if (b0 != 0xFE) {
        return false;
    }
    static constexpr u32 frame_samples_power[16] = {6, 6, 7, 7, 7, 8, 8, 8,
                                                    6, 6, 7, 7, 7, 8, 8, 8};
    const u32 sr_index = b1 >> 4;
    const u32 frame_bytes = ((u32(b2) << 3) | (b3 >> 5)) + 1;
    const u32 superframe_shift = (b3 >> 3) & 0x3;
    out.size = frame_bytes << superframe_shift;
    out.samples = (1u << frame_samples_power[sr_index]) << superframe_shift;
    return out.size != 0 && out.samples != 0;
}

WaveformUnit UnitFromFormat(const OrbisNgs2WaveformFormat* format) {
    WaveformUnit unit{};
    if (format && DecodeAt9Config(format->configData, unit)) {
        return unit;
    }
    if (format && format->numChannels != 0) {
        // Assume 16-bit PCM.
        return {format->numChannels * 2u, 1u};
    }
    return {4u, 1u};
}

// Parse a standard RIFF/WAVE header (PCM or ATRAC9) into OrbisNgs2WaveformInfo.
// AnyPS4: loop points of parsed RIFF waveforms by data pointer. They are not
// reported to the title (reporting them makes it crash while loading a bank);
// the sampler uses them to loop a music track seamlessly instead of restarting
// it from the first sample.
std::mutex loop_mutex;
std::unordered_map<const void*, std::pair<u32, u32>> loop_points;

bool ParseRiffWaveform(const void* data, size_t data_size, OrbisNgs2WaveformInfo* out) {
    const u8* p = static_cast<const u8*>(data);
    if (!p || data_size < 12) {
        return false;
    }
    auto rd32 = [&](size_t off) {
        u32 v;
        std::memcpy(&v, p + off, 4);
        return v;
    };
    auto rd16 = [&](size_t off) {
        u16 v;
        std::memcpy(&v, p + off, 2);
        return v;
    };
    if (std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WAVE", 4) != 0) {
        return false;
    }
    const size_t riff_end = std::min(data_size, size_t(rd32(4)) + 8);

    u16 format_tag = 0, num_channels = 0, block_align = 0, samples_per_block = 0;
    u32 sample_rate = 0, config_data = 0;
    u32 num_samples = 0, delay_samples = 0, data_off = 0, data_bytes = 0;
    u32 loop_begin = 0, loop_end = 0;
    bool has_fact = false;

    size_t off = 12;
    while (off + 8 <= riff_end) {
        const u32 cid = rd32(off);
        const u32 csz = rd32(off + 4);
        const size_t body = off + 8;
        if (body + csz > riff_end) {
            break;
        }
        if (cid == 0x20746d66u && csz >= 16) { // 'fmt '
            format_tag = rd16(body);
            num_channels = rd16(body + 2);
            sample_rate = rd32(body + 4);
            block_align = rd16(body + 12);
            if (format_tag == 0xFFFE && csz >= 52) { // WAVE_FORMAT_EXTENSIBLE (AT9)
                samples_per_block = rd16(body + 18);
                config_data = rd32(body + 44);
            }
        } else if (cid == 0x74636166u && csz >= 4) { // 'fact'
            num_samples = rd32(body);
            has_fact = true;
            if (csz >= 12) {
                delay_samples = rd32(body + 8);
            }
        } else if (cid == 0x61746164u) { // 'data'
            data_off = static_cast<u32>(body);
            data_bytes = csz;
        } else if (cid == 0x6c706d73u && csz >= 60) { // 'smpl' with at least one loop
            const u32 num_loops = rd32(body + 28);
            if (num_loops >= 1) {
                loop_begin = rd32(body + 36 + 8);
                loop_end = rd32(body + 36 + 12);
            }
        }
        off = body + csz + (csz & 1);
    }
    if (sample_rate == 0 || num_channels == 0 || data_bytes == 0) {
        return false;
    }
    {
        std::scoped_lock lk{loop_mutex};
        if (loop_end > loop_begin) {
            loop_points[data] = {loop_begin, loop_end};
        } else {
            loop_points.erase(data);
        }
    }

    WaveformUnit unit{};
    if (!DecodeAt9Config(config_data, unit)) {
        unit.size = block_align ? block_align : num_channels * 2u;
        unit.samples = samples_per_block ? samples_per_block : 1u;
    }
    if (!has_fact) {
        num_samples = static_cast<u32>(u64(data_bytes) / unit.size * unit.samples);
    }

    std::memset(out, 0, sizeof(*out));
    out->format.waveformType = format_tag;
    out->format.numChannels = num_channels;
    out->format.sampleRate = sample_rate;
    out->format.configData = config_data;
    out->dataOffset = data_off;
    out->dataSize = data_bytes;
    out->numSamples = num_samples;
    // Bisection: loop points from the smpl chunk are parsed but temporarily NOT
    // reported — suspected trigger of a load-time crash in the game.
    (void)loop_begin;
    (void)loop_end;
    out->loopBeginPosition = 0;
    out->loopEndPosition = 0;
    out->audioUnitSize = unit.size;
    out->numAudioUnitSamples = unit.samples;
    out->numAudioUnitPerFrame = 1;
    out->audioFrameSize = unit.size;
    out->numAudioFrameSamples = unit.samples;
    out->numDelaySamples = delay_samples;
    out->numBlocks = 1;
    out->aBlock[0].dataOffset = data_off;
    out->aBlock[0].dataSize = data_bytes;
    out->aBlock[0].numSamples = num_samples;
    return true;
}

// ---------------------------------------------------------------------------
// Minimal sampler-voice playback engine: decodes AT9/PCM16 waveform blocks and
// mixes them into the buffers handed to sceNgs2SystemRender. Not a faithful
// NGS2 synthesizer (no envelopes, pitch, effects or routing matrices), but it
// produces real audio for games that use the common sampler-voice pattern.
// ---------------------------------------------------------------------------

// SDK layouts for the sampler params the games send via sceNgs2VoiceControl.
struct SamplerVoiceSetupParam {
    OrbisNgs2VoiceParamHeader header;
    OrbisNgs2WaveformFormat format;
    u32 flags;
    u32 reserved;
};
static_assert(sizeof(SamplerVoiceSetupParam) == 40);

struct SamplerVoiceWaveformBlocksParam {
    OrbisNgs2VoiceParamHeader header;
    const void* data;
    u32 flags;
    u32 numBlocks;
    const OrbisNgs2WaveformBlock* aBlock;
};
static_assert(sizeof(SamplerVoiceWaveformBlocksParam) == 32);

struct VoicePortVolumeParam {
    OrbisNgs2VoiceParamHeader header;
    u32 port;
    float level;
};

enum SamplerParamId : u32 {
    SamplerSetup = 0x10000000,
    SamplerAddWaveformBlocks = 0x10000001,
    SamplerPitch = 0x10000005, // {float ratio, u32 pad} — confirmed from payload dumps
};

enum VoiceParamId : u32 {
    VoiceMatrixLevels = 0x1, // {u32 matrixId, u32 numLevels, const float* aLevel}
    VoicePortVolume = 0x2,   // {u32 port, float level} — confirmed from payload dumps
    VoicePortMatrix = 0x3,   // {u32 port, s32 matrixId}
    VoicePatch = 0x5,        // {u32 port, u32 destInputId, OrbisNgs2Handle destHandle}
    VoiceEvent = 0x6,
};

enum VoiceEventId : u32 {
    EventPlay = 0,
    EventStop = 1,
    EventStopImm = 2,
    EventKill = 3,
    EventPause = 4,
    EventResume = 5,
};

struct VoiceBlock {
    const u8* data;
    u32 size;
    u32 num_repeats;
    u32 num_skip_samples;
    uintptr_t user_data{};
    const void* base{};
};

struct Voice {
    bool known{}; // received a sampler Setup or waveform data at least once
    bool playing{};
    bool paused{};
    bool has_data{};
    bool pending_play{};   // Play event arrived before waveform data
    bool stop_requested{}; // the game asked to stop a music voice (suppressed)
    bool is_music{};       // large waveform (>1MB): long music track
    u32 add_calls{};       // AddWaveformBlocks invocations (>1 means streaming)
    OrbisNgs2WaveformFormat format{};
    std::vector<VoiceBlock> blocks; // kept across Play/Stop so sounds can replay
    float volume{1.0f};
    float matrix_gain{1.0f};
    float peak_volume{};   // highest port volume since the last Play (what a held track plays at)
    float pitch{1.0f};
    uintptr_t patch_dest{}; // mastering voice this voice is routed into
    u64 decoded_samples{};
    u32 starved_renders{}; // consecutive render calls with no data (streaming underrun)

    // Playback cursor
    u32 block_index{};
    u32 repeats_left{};
    u32 byte_pos{};       // read position in current block
    u32 skip_remaining{}; // decoded samples to discard (encoder delay)
    u32 loop_start{};     // AnyPS4: loop points in source samples, from the file's smpl chunk
    u32 loop_end{};
    u64 src_pos{};        // AnyPS4: source samples decoded since the last rewind

    // Decoder state
    void* at9_handle{};
    Atrac9CodecInfo at9_info{};
    bool at9_ready{};
    u32 sf_frames{};       // frames decoded in the current superframe
    u32 sf_bytes_remain{}; // bytes remaining in the current superframe
    u32 at9_errors{};      // consecutive decode failures (for resync give-up)
    enum class PcmKind { Unknown, S16, F32, Vag } pcm_kind{PcmKind::Unknown};
    float vag_hist[2][4]{}; // HEVAG decode history per channel
    u32 vag_rate{};         // sample rate from the VAG header
    u8 vag_channels{1};     // channel count from the VAG header

    // One decoded frame of interleaved f32 PCM and the read cursor into it.
    std::vector<float> frame_pcm;
    u32 frame_samples{}; // valid samples (per channel) in frame_pcm
    u32 frame_pos{};     // consumed samples (per channel)
    float frac{};        // fractional cursor for resampling

    // The game renders the same system into several output buffers per audio
    // grain (Main and BGM ports are both 8-channel). Cache the produced grain and
    // pace production against wall-clock time so the voice advances at exactly
    // 48kHz regardless of how many buffers are rendered per cycle.
    std::vector<float> grain_cache; // stereo L/R pairs
    u32 grain_len{};
    bool clock_valid{};
    std::chrono::steady_clock::time_point clock_start{};
    u64 clock_samples{}; // output samples produced since clock_start

    ~Voice() {
        if (at9_handle) {
            Atrac9ReleaseHandle(at9_handle);
        }
    }
};

std::mutex engine_mutex;
// AnyPS4: ORBIS_NGS2_TRACE=1 logs every voice event, volume, matrix and setup
// without the caps below, to reconstruct what a title asks of its voices.
const bool trace_all = std::getenv("ORBIS_NGS2_TRACE") != nullptr;
// AnyPS4: music hold mode (ORBIS_NGS2_MUSIC_HOLD). "fork", the default: Stop
// and Kill on a music voice are suppressed and the next music Play retires it,
// which is the behaviour of the user's shadPS4 fork, the reference this title
// is known to play correctly with. "scoped": only a track the title fades
// within its first three seconds while nothing else plays is held. "off" (or
// ORBIS_NGS2_MUSIC_FAITHFUL=1): every Stop and Kill ends the track.
const std::string music_hold_mode = [] {
    const char* v = std::getenv("ORBIS_NGS2_MUSIC_HOLD");
    return v == nullptr || std::string(v) == "1" ? std::string("fork") : std::string(v);
}();
const bool music_faithful = music_hold_mode == "off" || std::getenv("ORBIS_NGS2_MUSIC_FAITHFUL") != nullptr;
const bool music_hold = !music_faithful && music_hold_mode != "scoped";
// AnyPS4: the mastering (bus) voices report PLAYING with real peak meters only
// with ORBIS_NGS2_MASTERING_METERS=1; by default they report like the fork
// (never PLAYING, meters at zero), the state this title's music sequencing and
// dialogue ducking are known to work with.
const bool mastering_meters = std::getenv("ORBIS_NGS2_MASTERING_METERS") != nullptr;
// AnyPS4: the extended sampler state (bytes consumed, block user data, waveform
// pointer) is filled only with ORBIS_NGS2_SAMPLER_EXT=1; the fork leaves it zero.
const bool sampler_ext_state = std::getenv("ORBIS_NGS2_SAMPLER_EXT") != nullptr;
// AnyPS4: raw VAGp files are parsed by sceNgs2ParseWaveformData only with
// ORBIS_NGS2_VAG_PARSE=1. By default they get the fork's generic report (stereo,
// 48 kHz, size/4 samples), the behaviour this title's effect voices are known to
// play with; the decoder takes rate and channels from the VAGp header either way.
const bool vag_parse = [] {
    const char* v = std::getenv("ORBIS_NGS2_VAG_PARSE");
    return v != nullptr && *v == '1';
}();
// AnyPS4: a voice reuses its cached grain while it runs ahead of the wall
// clock by more than this window. The fork's 8 ms is the default again now that
// sceAudioOutOutput paces the title per chunk; ORBIS_NGS2_GRAIN_WINDOW_MS overrides.
const double grain_window = [] {
    const char* v = std::getenv("ORBIS_NGS2_GRAIN_WINDOW_MS");
    return v != nullptr ? std::atof(v) / 1000.0 : 0.008;
}();
std::unordered_map<uintptr_t, Voice> voice_registry;         // voice handle -> voice
std::unordered_map<uintptr_t, uintptr_t> rack_system;        // rack handle -> system handle
// AnyPS4: rack ids by rack handle, and per-bus peak meters (left, right) of
// what was mixed into each mastering voice on the last render. The title polls
// the mastering voices' state every grain and drives its music fades and
// dialogue ducking from the PLAYING flag and these meters.
constexpr u32 kMasteringRackId = 12288;
std::unordered_map<uintptr_t, u32> rack_ids;
std::unordered_map<uintptr_t, std::array<float, 2>> bus_peak;

// The title starts a track, fades it out within its first second and stops it
// even when nothing replaces it (the title screen would be silent). Hold such a
// track at the volume it reached, unless another music voice is already
// playing or it had played for a while: then the Stop is a real track change.
bool ShouldHoldMusic(uintptr_t voice_handle, const struct Voice& voice);

bool IsMasteringVoice(uintptr_t voice_handle) {
    const auto it = rack_ids.find(voice_handle & ~0xFFFFFull);
    return it != rack_ids.end() && it->second == kMasteringRackId;
}
u64 render_seq{};                                            // SystemRender call counter

uintptr_t RackOfVoice(uintptr_t voice_handle) {
    return voice_handle & ~0xFFFFFull;
}

bool ShouldHoldMusic(uintptr_t voice_handle, const Voice& voice) {
    if (music_faithful || voice.decoded_samples > 48000ull * 3) {
        return false;
    }
    for (const auto& [other_handle, other] : voice_registry) {
        if (other_handle != voice_handle && other.is_music && other.playing && other.has_data && !other.stop_requested &&
            other.volume > 0.01f) {
            return false;
        }
    }
    return voice.peak_volume > 0.01f;
}

// True for handles we minted in MakeRackHandle/MakeVoiceHandle. A synthetic
// handle unknown to the registry is a real-but-never-configured voice slot and
// must report as empty — phantom "playing" reports here trip the game's music
// ducking permanently.
bool IsSyntheticHandle(uintptr_t handle) {
    return (handle >> 32) == 0x4E530000u;
}

std::atomic<u64> empty_state_queries{};

void ResetVoiceDecoder(Voice& v) {
    if (v.at9_handle) {
        Atrac9ReleaseHandle(v.at9_handle);
        v.at9_handle = nullptr;
    }
    v.at9_ready = false;
    v.sf_frames = 0;
    v.sf_bytes_remain = 0;
    v.frame_samples = 0;
    v.frame_pos = 0;
    v.decoded_samples = 0;
}

// Decode one 16-byte HEVAG/PS-ADPCM block into 28 mono float samples. HEVAG
// (the PS4/Vita "VAGp") extends classic VAG with 128 4-tap filters; the first
// five rows equal classic VAG's filters, so both decode correctly.
void DecodeVagBlock(const u8* b, float* out, float* hist /* 4 floats */) {
    static constexpr float kHevagCoefs[128][4] = {
        {0.0f, 0.0f, 0.0f, 0.0f},
        {0.9375f, 0.0f, 0.0f, 0.0f},
        {1.796875f, -0.8125f, 0.0f, 0.0f},
        {1.53125f, -0.859375f, 0.0f, 0.0f},
        {1.90625f, -0.9375f, 0.0f, 0.0f},
        {1.7982178f, -0.86169434f, 0.0f, 0.0f},
        {1.770874f, -0.89916992f, 0.0f, 0.0f},
        {1.6992188f, -0.91821289f, 0.0f, 0.0f},
        {1.6031494f, -0.9375f, 0.0f, 0.0f},
        {1.4682617f, -0.9375f, 0.0f, 0.0f},
        {1.3139648f, -0.9375f, 0.0f, 0.0f},
        {1.1424561f, -0.9375f, 0.0f, 0.0f},
        {0.95605469f, -0.9375f, 0.0f, 0.0f},
        {0.75695801f, -0.9375f, 0.0f, 0.0f},
        {0.54785156f, -0.9375f, 0.0f, 0.0f},
        {0.33166504f, -0.9375f, 0.0f, 0.0f},
        {0.11108398f, -0.9375f, 0.0f, 0.0f},
        {-0.11108398f, -0.9375f, 0.0f, 0.0f},
        {-0.33166504f, -0.9375f, 0.0f, 0.0f},
        {-0.54785156f, -0.9375f, 0.0f, 0.0f},
        {-0.75695801f, -0.9375f, 0.0f, 0.0f},
        {-0.95605469f, -0.9375f, 0.0f, 0.0f},
        {-1.1424561f, -0.9375f, 0.0f, 0.0f},
        {-1.3139648f, -0.9375f, 0.0f, 0.0f},
        {-1.4682617f, -0.9375f, 0.0f, 0.0f},
        {-1.6031494f, -0.9375f, 0.0f, 0.0f},
        {-1.6992188f, -0.91821289f, 0.0f, 0.0f},
        {-1.770874f, -0.89916992f, 0.0f, 0.0f},
        {-1.7982178f, -0.86169434f, 0.0f, 0.0f},
        {0.65625f, -1.125f, 0.40625f, -0.375f},
        {-0.78125f, -0.875f, -0.40625f, -0.28125f},
        {-1.28125f, -0.90625f, -0.4375f, -0.125f},
        {-0.020385742f, -0.33227539f, -0.060302734f, -0.066040039f},
        {-0.90698242f, -0.27111816f, -0.28051758f, 0.051757812f},
        {-0.97668457f, -0.38647461f, -0.34350586f, 0.03527832f},
        {0.73461914f, -0.57983398f, 0.32336426f, -0.15844727f},
        {0.46362305f, -0.84790039f, 0.47302246f, -0.1484375f},
        {-1.0054932f, -0.31689453f, -0.25280762f, 0.027709961f},
        {1.1229248f, 0.24194336f, -0.16870117f, -0.28271484f},
        {1.5894775f, -0.37158203f, -0.46289062f, 0.15466309f},
        {1.6005859f, -0.54772949f, -0.2746582f, 0.20324707f},
        {-0.20361328f, -0.45703125f, -0.78808594f, 0.10253906f},
        {0.95446777f, -0.52832031f, 0.25769043f, -0.061767578f},
        {1.168335f, -0.16308594f, -0.092407227f, 0.059448242f},
        {1.2246094f, -0.31274414f, 0.036621094f, 0.024291992f},
        {-0.57922363f, -0.50317383f, -0.66967773f, -0.18225098f},
        {-0.71972656f, 0.2902832f, -0.58435059f, -0.84802246f},
        {-0.14562988f, -1.112915f, -0.15100098f, -0.38012695f},
        {0.33972168f, -0.86767578f, -0.19226074f, -0.17663574f},
        {-0.89526367f, -0.25170898f, -0.27001953f, 0.054443359f},
        {0.7479248f, -0.3145752f, -0.038452148f, -0.0021972656f},
        {1.1544189f, -0.22680664f, 0.012451172f, 0.031494141f},
        {0.96142578f, -0.54724121f, 0.25952148f, -0.065673828f},
        {-0.87548828f, -0.21911621f, -0.25256348f, 0.058837891f},
        {-0.89819336f, -0.2565918f, -0.27258301f, 0.053710938f},
        {-1.1193848f, -0.42834473f, -0.32641602f, -0.047729492f},
        {-0.32202148f, -0.32312012f, -0.23547363f, -0.1998291f},
        {0.2286377f, 1.1209717f, 0.22705078f, -0.70141602f},
        {1.1247559f, 0.22692871f, -0.13720703f, -0.29626465f},
        {1.6118164f, -0.36767578f, -0.50524902f, 0.16723633f},
        {1.5181885f, -0.58496094f, -0.03125f, 0.075927734f},
        {-0.32385254f, -0.13964844f, -0.38842773f, -0.83959961f},
        {1.1390381f, -0.12792969f, -0.10107422f, 0.061889648f},
        {0.20043945f, -0.075683594f, -0.11547852f, -0.51623535f},
        {0.51831055f, -0.92590332f, -0.065063477f, -0.27575684f},
        {-1.097168f, -0.47497559f, -0.34265137f, 0.0053710938f},
        {-0.31274414f, -0.3338623f, -0.21118164f, -0.23181152f},
        {0.38842773f, -0.058959961f, -0.087158203f, -0.17346191f},
        {0.96887207f, -0.46923828f, 0.34436035f, -0.12438965f},
        {1.229126f, -0.31848145f, 0.038330078f, 0.023803711f},
        {1.0253906f, -0.40246582f, 0.18933105f, -0.018920898f},
        {-1.0411377f, -0.33874512f, -0.296875f, -0.041015625f},
        {1.1568604f, -0.22973633f, 0.013183594f, 0.03125f},
        {0.0091552734f, -0.27355957f, -0.036376953f, -0.84680176f},
        {-1.1160889f, -0.5078125f, -0.36169434f, 0.00061035156f},
        {-0.88745117f, -0.23901367f, -0.26318359f, 0.056152344f},
        {-0.33447266f, 0.45715332f, 0.72460938f, -0.13293457f},
        {1.0977783f, 0.23779297f, -0.083374023f, -0.33007812f},
        {1.5992432f, -0.34606934f, -0.47045898f, 0.12878418f},
        {1.164917f, -0.23937988f, 0.015869141f, 0.030517578f},
        {0.64355469f, -0.52124023f, 0.38134766f, -0.38537598f},
        {-0.93945312f, -0.41296387f, -0.3548584f, -0.055664062f},
        {0.89221191f, 0.3079834f, 0.052978516f, -0.30041504f},
        {1.2542725f, -0.34997559f, 0.047729492f, 0.020996094f},
        {1.3354492f, -0.45422363f, 0.081176758f, 0.01184082f},
        {0.0029296875f, -0.037841797f, -0.15405273f, 0.0390625f},
        {-0.99145508f, -0.29431152f, -0.28210449f, -0.033081055f},
        {-1.0389404f, -0.37438965f, -0.28527832f, 0.019897461f},
        {0.039794922f, -0.46948242f, 0.051147461f, -0.1138916f},
        {1.0858154f, 0.26782227f, -0.066040039f, -0.3515625f},
        {1.4737549f, -0.22900391f, -0.24621582f, -0.073364258f},
        {1.0655518f, -0.41784668f, 0.2043457f, -0.020629883f},
        {1.5808105f, -0.46960449f, -0.36706543f, 0.23754883f},
        {1.2253418f, -0.3137207f, 0.036865234f, 0.024169922f},
        {1.1456299f, -0.33654785f, 0.12304688f, 0.0050048828f},
        {-0.57617188f, -0.61108398f, -0.34814453f, -0.14172363f},
        {0.96057129f, -0.52807617f, 0.26062012f, -0.061157227f},
        {0.29907227f, -1.0494385f, 0.15856934f, -0.33935547f},
        {1.2441406f, -0.33728027f, 0.043945312f, 0.022094727f},
        {1.3809814f, -0.51428223f, 0.10168457f, 0.0064697266f},
        {1.239502f, -0.33154297f, 0.042114258f, 0.022583008f},
        {1.1765137f, -0.17297363f, -0.08996582f, 0.058837891f},
        {0.47045898f, -0.5559082f, 0.3470459f, -0.41467285f},
        {0.81774902f, -0.6907959f, 0.27453613f, -0.13110352f},
        {1.3527832f, -0.47705078f, 0.088867188f, 0.009765625f},
        {-0.12524414f, -1.1975098f, -0.098266602f, -0.42260742f},
        {1.269043f, -0.45727539f, 0.16687012f, -0.01171875f},
        {1.2557373f, 0.12060547f, -0.23376465f, -0.17541504f},
        {0.9708252f, 0.47338867f, -0.093261719f, -0.39831543f},
        {1.5489502f, -0.4119873f, -0.40942383f, 0.25378418f},
        {0.81066895f, 0.38647461f, 0.028198242f, -0.25500488f},
        {-0.28662109f, -0.89770508f, -0.23730469f, -0.50317383f},
        {1.1340332f, -0.49304199f, 0.23010254f, -0.030029297f},
        {0.56555176f, -0.78161621f, 0.21337891f, -0.19763184f},
        {1.3729248f, -0.50354004f, 0.097900391f, 0.0074462891f},
        {1.1971436f, -0.27880859f, 0.026733398f, 0.027099609f},
        {1.1884766f, -0.1875f, -0.086181641f, 0.057739258f},
        {1.0302734f, -0.41943359f, 0.19067383f, -0.021484375f},
        {1.1361084f, -0.12463379f, -0.10192871f, 0.062133789f},
        {0.20727539f, -1.1016846f, 0.083984375f, -0.37072754f},
        {1.2468262f, -0.34069824f, 0.044921875f, 0.021850586f},
        {1.0241699f, 0.39648438f, -0.092529297f, -0.36486816f},
        {0.87902832f, 0.40478516f, 0.0056152344f, -0.3190918f},
        {-0.010742188f, -0.95324707f, -0.065673828f, -0.5579834f},
        {0.75598145f, -0.63342285f, 0.33691406f, -0.15197754f},
        {1.5045166f, -0.1574707f, -0.40087891f, 0.030883789f},
        {1.5947266f, -0.49743652f, -0.34472656f, 0.22912598f},
        {0.65100098f, 0.36608887f, 0.094604492f, -0.13818359f},
    };
    const u32 coef_index = std::min<u32>((b[1] & 0xF0u) | (b[0] >> 4), 127u);
    const u32 shift = b[0] & 0x0F;
    const u32 flag = b[1] & 0x0F;
    if (flag >= 0x07) {
        // End-mute/marker blocks (flags 7..15) play as silence on hardware.
        // Decoding them as audio produces a constant static patter ("rain").
        // The end-of-voice click this once masked is now handled by the
        // tail fade in ProduceGrain.
        std::memset(out, 0, 28 * sizeof(float));
        return;
    }
    const float* c = kHevagCoefs[coef_index];
    for (int i = 0; i < 28; ++i) {
        const u8 byte = b[2 + i / 2];
        s32 nibble = (i & 1) ? (byte >> 4) : (byte & 0xF);
        if (nibble > 7) {
            nibble -= 16;
        }
        const float code = static_cast<float>((nibble * 4096) >> shift);
        const float sample =
            code + hist[0] * c[0] + hist[1] * c[1] + hist[2] * c[2] + hist[3] * c[3];
        hist[3] = hist[2];
        hist[2] = hist[1];
        hist[1] = hist[0];
        hist[0] = sample;
        out[i] = std::clamp(sample / 32768.0f, -1.0f, 1.0f);
    }
}

// Distinguish float32 PCM from s16 PCM by sampling the data: real audio floats
// are finite and within a sane amplitude range, while s16 bytes read as floats
// are mostly huge, denormal, or NaN.
bool LooksLikeFloatPcm(const u8* data, u32 size) {
    const u32 n = std::min<u32>(size / 4, 64);
    if (n < 8) {
        return false;
    }
    for (u32 i = 0; i < n; ++i) {
        float f;
        std::memcpy(&f, data + i * 4, 4);
        if (!std::isfinite(f) || std::fabs(f) > 4.0f) {
            return false;
        }
    }
    return true;
}

// Rewind the playback cursor to the beginning of the waveform without
// discarding the loaded blocks, so the voice can be replayed.
void RewindVoice(Voice& v) {
    ResetVoiceDecoder(v);
    v.block_index = 0;
    v.byte_pos = 0;
    v.repeats_left = v.blocks.empty() ? 0 : v.blocks.front().num_repeats;
    v.skip_remaining = v.blocks.empty() ? 0 : v.blocks.front().num_skip_samples;
    v.frac = 0.0f;
    v.src_pos = 0;
}

bool IsAt9Format(const OrbisNgs2WaveformFormat& fmt) {
    return (fmt.configData & 0xFF) == 0xFE;
}

u32 VoiceChannels(const Voice& v) {
    return std::max(1u, v.format.numChannels);
}

// Advance the cursor past exhausted blocks (honoring per-block repeats).
// Returns false when playback reached the end of all blocks.
bool AdvancePastExhaustedBlock(Voice& v, u32 min_remaining) {
    while (v.block_index < v.blocks.size()) {
        const VoiceBlock& block = v.blocks[v.block_index];
        if (v.byte_pos + min_remaining <= block.size) {
            return true;
        }
        if (v.repeats_left > 0) {
            --v.repeats_left;
            v.byte_pos = 0;
            continue;
        }
        ++v.block_index;
        v.byte_pos = 0;
        if (v.block_index < v.blocks.size()) {
            v.repeats_left = v.blocks[v.block_index].num_repeats;
            v.skip_remaining = v.blocks[v.block_index].num_skip_samples;
        }
    }
    return false;
}

// Decode the next frame of the current block into v.frame_pcm. Returns false
// when the voice ran out of data.
bool DecodeNextFrame(Voice& v) {
    if (IsAt9Format(v.format)) {
        if (!v.at9_ready) {
            v.at9_handle = Atrac9GetHandle();
            u8 config[ATRAC9_CONFIG_DATA_SIZE];
            std::memcpy(config, &v.format.configData, sizeof(config));
            if (Atrac9InitDecoder(v.at9_handle, config) != 0 ||
                Atrac9GetCodecInfo(v.at9_handle, &v.at9_info) != 0) {
                LOG_ERROR(Lib_Ngs2, "Failed to init AT9 decoder, dropping voice data");
                return false;
            }
            v.at9_ready = true;
            v.sf_frames = 0;
            v.sf_bytes_remain = static_cast<u32>(v.at9_info.superframeSize);
        }
        while (AdvancePastExhaustedBlock(v, 1)) {
            const VoiceBlock& block = v.blocks[v.block_index];
            const u32 channels = std::max(1, v.at9_info.channels);
            v.frame_pcm.resize(size_t(v.at9_info.frameSamples) * channels);
            int bytes_used = 0;
            const int ret =
                Atrac9DecodeF32(v.at9_handle, block.data + v.byte_pos,
                                static_cast<int>(block.size - v.byte_pos), v.frame_pcm.data(),
                                &bytes_used, 0);
            if (ret != 0 || bytes_used <= 0) {
                LOG_WARNING(Lib_Ngs2, "AT9 decode error {} at offset {}, resyncing", ret,
                            v.byte_pos);
                if (++v.at9_errors > 16) {
                    // Repeated failures: give up on this block.
                    v.byte_pos = block.size;
                    v.at9_errors = 0;
                    continue;
                }
                // Skip to the next superframe boundary and retry.
                v.byte_pos += v.sf_frames != 0 ? v.sf_bytes_remain
                                               : static_cast<u32>(v.at9_info.superframeSize);
                v.sf_bytes_remain = static_cast<u32>(v.at9_info.superframeSize);
                v.sf_frames = 0;
                continue;
            }
            v.at9_errors = 0;
            v.byte_pos += static_cast<u32>(bytes_used);
            v.sf_bytes_remain -= static_cast<u32>(bytes_used);
            // After the last frame of a superframe, skip its padding bytes.
            if (++v.sf_frames >= static_cast<u32>(v.at9_info.framesInSuperframe)) {
                v.byte_pos += v.sf_bytes_remain;
                v.sf_bytes_remain = static_cast<u32>(v.at9_info.superframeSize);
                v.sf_frames = 0;
            }
            v.frame_samples = static_cast<u32>(v.at9_info.frameSamples);
            v.frame_pos = 0;
            return true;
        }
        return false;
    }
    // PCM path (s16 or f32, auto-detected)
    const u32 channels = VoiceChannels(v);
    if (v.pcm_kind == Voice::PcmKind::Unknown) {
        if (!AdvancePastExhaustedBlock(v, 4)) {
            return false;
        }
        const VoiceBlock& first = v.blocks[v.block_index];
        const u8* head = first.data + v.byte_pos;
        if (first.size - v.byte_pos >= 48 && std::memcmp(head, "VAGp", 4) == 0) {
            v.pcm_kind = Voice::PcmKind::Vag;
        } else if (LooksLikeFloatPcm(head, first.size - v.byte_pos)) {
            v.pcm_kind = Voice::PcmKind::F32;
        } else {
            v.pcm_kind = Voice::PcmKind::S16;
        }
    }
    if (v.pcm_kind == Voice::PcmKind::Vag) {
        // Trust the VAGp header's channel count over the voice format: the game
        // frequently declares stereo voices around mono VAG data.
        const bool stereo = v.vag_channels >= 2;
        const u32 need = stereo ? 32 : 16;
        // Every streamed chunk starts with its own 48-byte VAGp header.
        while (true) {
            if (!AdvancePastExhaustedBlock(v, need)) {
                return false;
            }
            const VoiceBlock& block = v.blocks[v.block_index];
            if (v.byte_pos == 0 && block.size >= 48 &&
                std::memcmp(block.data, "VAGp", 4) == 0) {
                const u8* r = block.data + 16; // big-endian sample rate
                v.vag_rate = (u32(r[0]) << 24) | (u32(r[1]) << 16) | (u32(r[2]) << 8) | u32(r[3]);
                v.vag_channels = std::max<u8>(block.data[0x1E], 1);
                v.byte_pos = 48;
                continue;
            }
            break;
        }
        const VoiceBlock& block = v.blocks[v.block_index];
        if (stereo) {
            // Stereo streams interleave 16-byte ADPCM blocks L,R with separate
            // predictor histories per channel.
            float left[28], right[28];
            DecodeVagBlock(block.data + v.byte_pos, left, v.vag_hist[0]);
            DecodeVagBlock(block.data + v.byte_pos + 16, right, v.vag_hist[1]);
            v.frame_pcm.resize(28 * 2);
            for (int i = 0; i < 28; ++i) {
                v.frame_pcm[i * 2 + 0] = left[i];
                v.frame_pcm[i * 2 + 1] = right[i];
            }
            v.byte_pos += 32;
        } else {
            v.frame_pcm.resize(28);
            DecodeVagBlock(block.data + v.byte_pos, v.frame_pcm.data(), v.vag_hist[0]);
            v.byte_pos += 16;
        }
        v.frame_samples = 28;
        v.frame_pos = 0;
        return true;
    }
    const u32 sample_bytes = v.pcm_kind == Voice::PcmKind::F32 ? 4 : 2;
    const u32 bytes_per_sample = channels * sample_bytes;
    if (!AdvancePastExhaustedBlock(v, bytes_per_sample)) {
        return false;
    }
    const VoiceBlock& block = v.blocks[v.block_index];
    const u32 avail = (block.size - v.byte_pos) / bytes_per_sample;
    const u32 todo = std::min(avail, 256u);
    v.frame_pcm.resize(size_t(todo) * channels);
    const u8* src = block.data + v.byte_pos;
    if (v.pcm_kind == Voice::PcmKind::F32) {
        std::memcpy(v.frame_pcm.data(), src, size_t(todo) * channels * 4);
    } else {
        for (u32 i = 0; i < todo * channels; ++i) {
            s16 s;
            std::memcpy(&s, src + i * 2, 2);
            v.frame_pcm[i] = static_cast<float>(s) / 32768.0f;
        }
    }
    v.byte_pos += todo * bytes_per_sample;
    v.frame_samples = todo;
    v.frame_pos = 0;
    return true;
}

// Produce one grain of `num_samples` output samples (48kHz stereo pairs) into
// v.grain_cache, resampling by the source rate and pitch ratio, advancing the
// voice. Mono sources go to L+R, stereo to L/R.
void ProduceGrain(Voice& v, u32 num_samples, float gain) {
    v.grain_cache.assign(size_t(num_samples) * 2, 0.0f);
    v.grain_len = num_samples;
    u32 src_channels = IsAt9Format(v.format) && v.at9_ready ? std::max(1, v.at9_info.channels)
                                                            : VoiceChannels(v);
    u32 src_rate = v.format.sampleRate;
    if (IsAt9Format(v.format) && v.at9_ready && v.at9_info.samplingRate > 0) {
        src_rate = static_cast<u32>(v.at9_info.samplingRate);
    }
    if (v.pcm_kind == Voice::PcmKind::Vag) {
        src_channels = std::min<u32>(v.vag_channels, 2u);
        if (v.vag_rate != 0) {
            src_rate = v.vag_rate;
        }
    }
    if (src_rate == 0) {
        src_rate = 48000;
    }
    const float step = (static_cast<float>(src_rate) / 48000.0f) * v.pitch;
    for (u32 i = 0; i < num_samples; ++i) {
        // Ensure the cursor points at a valid source sample, skipping any
        // encoder-delay samples at block starts.
        while (true) {
            if (v.frame_pos >= v.frame_samples) {
                bool decoded = v.loop_end == 0 || v.src_pos < v.loop_end ? DecodeNextFrame(v) : false;
                if (decoded) {
                    if (v.loop_end != 0 && v.src_pos + v.frame_samples > v.loop_end) {
                        v.frame_samples = static_cast<u32>(std::max<u64>(v.loop_end - v.src_pos, 1));
                    }
                    v.src_pos += v.frame_samples;
                }
                if (!decoded) {
                    u64 total = 0;
                    for (const auto& b : v.blocks) {
                        total += b.size;
                    }
                    if (total > 1'000'000) {
                        // Music track reached its end (or its loop end): go back
                        // to the loop start, decoding and discarding up to it.
                        RewindVoice(v);
                        v.skip_remaining += v.loop_start;
                        decoded = DecodeNextFrame(v);
                        if (decoded) {
                            v.src_pos += v.frame_samples;
                        }
                    }
                }
                if (!decoded) {
                    if (v.add_calls > 1) {
                        // Streaming voice: more blocks arrive shortly, survive a
                        // short underrun before declaring it done.
                        if (++v.starved_renders > 30) {
                            v.playing = false;
                        }
                    } else {
                        // One-shot: end immediately so the game sees the voice free
                        // and can retrigger it (footsteps, rapid effects). Fade the
                        // tail we produced to avoid a discontinuity click.
                        v.playing = false;
                        const u32 fade = std::min(i, 128u);
                        for (u32 k = 0; k < fade; ++k) {
                            const float w = static_cast<float>(k) / fade;
                            v.grain_cache[size_t(i - 1 - k) * 2 + 0] *= w;
                            v.grain_cache[size_t(i - 1 - k) * 2 + 1] *= w;
                        }
                    }
                    return;
                }
            }
            if (v.skip_remaining > 0) {
                const u32 skip = std::min(v.skip_remaining, v.frame_samples - v.frame_pos);
                v.frame_pos += skip;
                v.skip_remaining -= skip;
                continue;
            }
            break;
        }
        v.starved_renders = 0;
        const float* s0 = v.frame_pcm.data() + size_t(v.frame_pos) * src_channels;
        const float* s1 = v.frame_pos + 1 < v.frame_samples ? s0 + src_channels : s0;
        const float t = v.frac;
        float* dst = v.grain_cache.data() + size_t(i) * 2;
        const float l = s0[0] + (s1[0] - s0[0]) * t;
        const float r = src_channels > 1 ? s0[1] + (s1[1] - s0[1]) * t : l;
        dst[0] = l * gain;
        dst[1] = r * gain;
        ++v.decoded_samples;
        v.frac += step;
        while (v.frac >= 1.0f) {
            v.frac -= 1.0f;
            ++v.frame_pos;
        }
    }
}
} // namespace

// Ngs2

s32 PS4_SYSV_ABI sceNgs2CalcWaveformBlock(const OrbisNgs2WaveformFormat* format, u32 samplePos,
                                          u32 numSamples, OrbisNgs2WaveformBlock* outBlock) try {
    LOG_DEBUG(Lib_Ngs2, "samplePos = {}, numSamples = {}", samplePos, numSamples);
    if (outBlock) {
        const WaveformUnit unit = UnitFromFormat(format);
        const u32 frame_offset = format ? format->frameOffset : 0;
        std::memset(outBlock, 0, sizeof(*outBlock));
        outBlock->dataOffset =
            frame_offset + static_cast<u32>(u64(samplePos) / unit.samples * unit.size);
        outBlock->dataSize = static_cast<u32>(
            (u64(numSamples) + unit.samples - 1) / unit.samples * unit.size);
        outBlock->numSkipSamples = samplePos % unit.samples;
        outBlock->numSamples = numSamples;
    }
    return ORBIS_OK;
} catch (const std::exception& e) {
    LOG_CRITICAL(Lib_Ngs2, "exception in sceNgs2CalcWaveformBlock: {}", e.what());
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2GetWaveformFrameInfo(const OrbisNgs2WaveformFormat* format,
                                             u32* outFrameSize, u32* outNumFrameSamples,
                                             u32* outUnitsPerFrame, u32* outNumDelaySamples) {
    LOG_DEBUG(Lib_Ngs2, "called");
    const WaveformUnit unit = UnitFromFormat(format);
    if (outFrameSize) {
        *outFrameSize = unit.size;
    }
    if (outNumFrameSamples) {
        *outNumFrameSamples = unit.samples;
    }
    if (outUnitsPerFrame) {
        *outUnitsPerFrame = 1;
    }
    if (outNumDelaySamples) {
        *outNumDelaySamples = 0;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2ParseWaveformData(const void* data, size_t dataSize,
                                          OrbisNgs2WaveformInfo* outInfo) try {
    if (!outInfo) {
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    if (ParseRiffWaveform(data, dataSize, outInfo)) {
        LOG_INFO(Lib_Ngs2,
                 "parsed WAV: tag = {:#x}, channels = {}, rate = {}, samples = {}, dataSize = {}",
                 outInfo->format.waveformType, outInfo->format.numChannels,
                 outInfo->format.sampleRate, outInfo->numSamples, outInfo->dataSize);
        return ORBIS_OK;
    }
    // AnyPS4: raw "VAGp" (HEVAG/PS-ADPCM) files, which this title uses for every
    // sound effect. Big-endian data size at 0x0C and sample rate at 0x10, the
    // channel count at 0x1E; 16-byte units of 28 samples. The data offset stays
    // 0 so the decoder still sees the header and takes its rate and channels.
    if (vag_parse && data != nullptr && dataSize >= 64 && std::memcmp(data, "VAGp", 4) == 0) {
        const auto* bytes = static_cast<const u8*>(data);
        auto be32 = [&](size_t at) { return (u32(bytes[at]) << 24) | (u32(bytes[at + 1]) << 16) | (u32(bytes[at + 2]) << 8) | u32(bytes[at + 3]); };
        const u32 channels = std::max<u32>(bytes[0x1E], 1u);
        const u32 rate = be32(0x10) != 0 ? be32(0x10) : 44100u;
        const u32 body = std::min<u32>(be32(0x0C), static_cast<u32>(dataSize - 48));
        const u32 units = body / (16u * channels);
        std::memset(outInfo, 0, sizeof(OrbisNgs2WaveformInfo));
        outInfo->format.waveformType = 0;
        outInfo->format.numChannels = channels;
        outInfo->format.sampleRate = rate;
        outInfo->dataOffset = 0;
        outInfo->dataSize = static_cast<u32>(dataSize);
        outInfo->numSamples = units * 28u;
        outInfo->audioUnitSize = 16u * channels;
        outInfo->numAudioUnitSamples = 28;
        outInfo->numAudioUnitPerFrame = 1;
        outInfo->audioFrameSize = 16u * channels;
        outInfo->numAudioFrameSamples = 28;
        outInfo->numBlocks = 1;
        outInfo->aBlock[0].dataOffset = 0;
        outInfo->aBlock[0].dataSize = static_cast<u32>(dataSize);
        outInfo->aBlock[0].numSamples = outInfo->numSamples;
        static std::atomic<int> vag_logs{0};
        if (vag_logs++ < 20) {
            LOG_INFO(Lib_Ngs2, "parsed VAG: channels = {}, rate = {}, samples = {}, dataSize = {}, name = {:.16}", channels, rate,
                     outInfo->numSamples, dataSize, reinterpret_cast<const char*>(bytes + 0x20));
        }
        return ORBIS_OK;
    }
    // Unknown container: report a plausible non-zero waveform so duration math
    // in games never divides by zero.
    LOG_WARNING(Lib_Ngs2, "unrecognized waveform data (size = {}), using defaults", dataSize);
    if (trace_all) {
        static std::atomic<int> dumps{0};
        if (dumps++ < 40 && data != nullptr) {
            std::string hex, text;
            const auto* bytes = static_cast<const u8*>(data);
            for (size_t b = 0; b < std::min<size_t>(dataSize, 48); ++b) {
                hex += fmt::format("{:02x} ", bytes[b]);
                text += (bytes[b] >= 0x20 && bytes[b] < 0x7f) ? static_cast<char>(bytes[b]) : '.';
            }
            LOG_INFO(Lib_Ngs2, "unrecognized waveform head: {} |{}|", hex, text);
        }
    }
    std::memset(outInfo, 0, sizeof(OrbisNgs2WaveformInfo));
    outInfo->format.numChannels = 2;
    outInfo->format.sampleRate = 48000;
    outInfo->dataSize = static_cast<u32>(dataSize);
    outInfo->numSamples = static_cast<u32>(dataSize / 4);
    outInfo->audioUnitSize = 4;
    outInfo->numAudioUnitSamples = 1;
    outInfo->numAudioUnitPerFrame = 1;
    outInfo->audioFrameSize = 4;
    outInfo->numAudioFrameSamples = 1;
    outInfo->numBlocks = 1;
    outInfo->aBlock[0].dataSize = outInfo->dataSize;
    outInfo->aBlock[0].numSamples = outInfo->numSamples;
    return ORBIS_OK;
} catch (const std::exception& e) {
    LOG_CRITICAL(Lib_Ngs2, "exception in sceNgs2ParseWaveformData: {}", e.what());
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2ParseWaveformFile(const char* path, u64 offset,
                                          OrbisNgs2WaveformInfo* outInfo) {
    LOG_ERROR(Lib_Ngs2, "path = {}, offset = {}", path, offset);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2ParseWaveformUser(OrbisNgs2ParseReadHandler handler, uintptr_t userData,
                                          OrbisNgs2WaveformInfo* outInfo) {
    LOG_ERROR(Lib_Ngs2, "userData = {}", userData);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackCreate(OrbisNgs2Handle systemHandle, u32 rackId,
                                   const OrbisNgs2RackOption* option,
                                   const OrbisNgs2ContextBufferInfo* bufferInfo,
                                   OrbisNgs2Handle* outHandle) {
    LOG_ERROR(Lib_Ngs2, "rackId = {}", rackId);
    if (!systemHandle) {
        LOG_ERROR(Lib_Ngs2, "systemHandle is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    if (outHandle) {
        *outHandle = MakeRackHandle();
        std::scoped_lock lk{engine_mutex};
        rack_system[*outHandle] = systemHandle;
        rack_ids[*outHandle] = rackId;
        LOG_INFO(Lib_Ngs2, "rack {:#x} (id {}) on system {:#x}", *outHandle, rackId, systemHandle);
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackCreateWithAllocator(OrbisNgs2Handle systemHandle, u32 rackId,
                                                const OrbisNgs2RackOption* option,
                                                const OrbisNgs2BufferAllocator* allocator,
                                                OrbisNgs2Handle* outHandle) {
    LOG_ERROR(Lib_Ngs2, "rackId = {}", rackId);
    if (!systemHandle) {
        LOG_ERROR(Lib_Ngs2, "systemHandle is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    if (outHandle) {
        *outHandle = MakeRackHandle();
        std::scoped_lock lk{engine_mutex};
        rack_system[*outHandle] = systemHandle;
        rack_ids[*outHandle] = rackId;
        LOG_INFO(Lib_Ngs2, "rack {:#x} (id {}) on system {:#x}", *outHandle, rackId, systemHandle);
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackDestroy(OrbisNgs2Handle rackHandle,
                                    OrbisNgs2ContextBufferInfo* outBufferInfo) {
    LOG_ERROR(Lib_Ngs2, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackGetInfo(OrbisNgs2Handle rackHandle, OrbisNgs2RackInfo* outInfo,
                                    size_t infoSize) {
    LOG_ERROR(Lib_Ngs2, "infoSize = {}", infoSize);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackGetUserData(OrbisNgs2Handle rackHandle, uintptr_t* outUserData) {
    LOG_ERROR(Lib_Ngs2, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackGetVoiceHandle(OrbisNgs2Handle rackHandle, u32 voiceIndex,
                                           OrbisNgs2Handle* outHandle) {
    LOG_DEBUG(Lib_Ngs2, "(STUBBED) voiceIndex = {}", voiceIndex);
    if (!rackHandle) {
        return ORBIS_NGS2_ERROR_INVALID_RACK_HANDLE;
    }
    if (outHandle) {
        // Deterministic per (rack, index) so repeated queries return the same handle.
        *outHandle = MakeVoiceHandle(rackHandle, voiceIndex);
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackLock(OrbisNgs2Handle rackHandle) {
    LOG_ERROR(Lib_Ngs2, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackQueryBufferSize(u32 rackId, const OrbisNgs2RackOption* option,
                                            OrbisNgs2ContextBufferInfo* outBufferInfo) {
    LOG_ERROR(Lib_Ngs2, "rackId = {}", rackId);
    return RackQueryBufferSize(option, outBufferInfo);
}

s32 PS4_SYSV_ABI sceNgs2RackSetUserData(OrbisNgs2Handle rackHandle, uintptr_t userData) {
    LOG_ERROR(Lib_Ngs2, "userData = {}", userData);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2RackUnlock(OrbisNgs2Handle rackHandle) {
    LOG_ERROR(Lib_Ngs2, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemCreate(const OrbisNgs2SystemOption* option,
                                     const OrbisNgs2ContextBufferInfo* bufferInfo,
                                     OrbisNgs2Handle* outHandle) {
    s32 result;
    OrbisNgs2ContextBufferInfo localInfo;
    if (!bufferInfo || !outHandle) {
        if (!bufferInfo) {
            result = ORBIS_NGS2_ERROR_INVALID_BUFFER_INFO;
            LOG_ERROR(Lib_Ngs2, "Invalid system buffer info {}", (void*)bufferInfo);
        } else {
            result = ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
            LOG_ERROR(Lib_Ngs2, "Invalid system handle address {}", (void*)outHandle);
        }

        // TODO: Report errors?
    } else {
        // Make bufferInfo copy
        localInfo.hostBuffer = bufferInfo->hostBuffer;
        localInfo.hostBufferSize = bufferInfo->hostBufferSize;
        for (int i = 0; i < 5; i++) {
            localInfo.reserved[i] = bufferInfo->reserved[i];
        }
        localInfo.userData = bufferInfo->userData;

        result = SystemSetup(option, &localInfo, 0, outHandle);
    }

    // TODO: API reporting?

    LOG_INFO(Lib_Ngs2, "called");
    return result;
}

s32 PS4_SYSV_ABI sceNgs2SystemCreateWithAllocator(const OrbisNgs2SystemOption* option,
                                                  const OrbisNgs2BufferAllocator* allocator,
                                                  OrbisNgs2Handle* outHandle) {
    s32 result;
    if (allocator && allocator->allocHandler != 0) {
        OrbisNgs2BufferAllocHandler hostAlloc = allocator->allocHandler;
        if (outHandle) {
            OrbisNgs2BufferFreeHandler hostFree = allocator->freeHandler;
            OrbisNgs2ContextBufferInfo bufferInfo;
            result = SystemSetup(option, &bufferInfo, 0, 0);
            if (result >= 0) {
                uintptr_t sysUserData = allocator->userData;
                result = hostAlloc(&bufferInfo);
                if (result >= 0) {
                    OrbisNgs2Handle* handleCopy = outHandle;
                    result = SystemSetup(option, &bufferInfo, hostFree, handleCopy);
                    if (result < 0) {
                        if (hostFree) {
                            hostFree(&bufferInfo);
                        }
                    }
                }
            }
        } else {
            result = ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
            LOG_ERROR(Lib_Ngs2, "Invalid system handle address {}", (void*)outHandle);
        }
    } else {
        result = ORBIS_NGS2_ERROR_INVALID_BUFFER_ALLOCATOR;
        LOG_ERROR(Lib_Ngs2, "Invalid system buffer allocator {}", (void*)allocator);
    }
    LOG_INFO(Lib_Ngs2, "called");
    return result;
}

s32 PS4_SYSV_ABI sceNgs2SystemDestroy(OrbisNgs2Handle systemHandle,
                                      OrbisNgs2ContextBufferInfo* outBufferInfo) {
    if (!systemHandle) {
        LOG_ERROR(Lib_Ngs2, "systemHandle is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    LOG_INFO(Lib_Ngs2, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemEnumHandles(OrbisNgs2Handle* aOutHandle, u32 maxHandles) {
    LOG_ERROR(Lib_Ngs2, "maxHandles = {}", maxHandles);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemEnumRackHandles(OrbisNgs2Handle systemHandle,
                                              OrbisNgs2Handle* aOutHandle, u32 maxHandles) {
    LOG_ERROR(Lib_Ngs2, "maxHandles = {}", maxHandles);
    if (!systemHandle) {
        LOG_ERROR(Lib_Ngs2, "systemHandle is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemGetInfo(OrbisNgs2Handle rackHandle, OrbisNgs2SystemInfo* outInfo,
                                      size_t infoSize) {
    LOG_ERROR(Lib_Ngs2, "infoSize = {}", infoSize);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemGetUserData(OrbisNgs2Handle systemHandle, uintptr_t* outUserData) {
    if (!systemHandle) {
        LOG_ERROR(Lib_Ngs2, "systemHandle is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    LOG_ERROR(Lib_Ngs2, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemLock(OrbisNgs2Handle systemHandle) {
    if (!systemHandle) {
        LOG_ERROR(Lib_Ngs2, "systemHandle is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    LOG_ERROR(Lib_Ngs2, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemQueryBufferSize(const OrbisNgs2SystemOption* option,
                                              OrbisNgs2ContextBufferInfo* outBufferInfo) {
    s32 result;
    if (outBufferInfo) {
        result = SystemSetup(option, outBufferInfo, 0, 0);
        LOG_INFO(Lib_Ngs2, "called");
    } else {
        result = ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
        LOG_ERROR(Lib_Ngs2, "Invalid system buffer info {}", (void*)outBufferInfo);
    }

    return result;
}

s32 PS4_SYSV_ABI sceNgs2SystemRender(OrbisNgs2Handle systemHandle,
                                     const OrbisNgs2RenderBufferInfo* aBufferInfo,
                                     u32 numBufferInfo) try {
    LOG_DEBUG(Lib_Ngs2, "(STUBBED) numBufferInfo = {}", numBufferInfo);
    if (!systemHandle) {
        LOG_ERROR(Lib_Ngs2, "systemHandle is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    if (!aBufferInfo) {
        return ORBIS_OK;
    }
    // Zero the output buffers, then mix every playing voice that belongs to this
    // system into them (float PCM output is assumed; the games we target create
    // float AudioOut ports).
    for (u32 i = 0; i < numBufferInfo; ++i) {
        if (aBufferInfo[i].buffer && aBufferInfo[i].bufferSize > 0) {
            std::memset(aBufferInfo[i].buffer, 0, aBufferInfo[i].bufferSize);
        }
    }
    std::scoped_lock lk{engine_mutex};
    const u64 seq = ++render_seq;
    if (trace_all && seq % 2000 < 3) {
        std::string buffers;
        for (u32 i = 0; i < numBufferInfo; ++i) {
            buffers += fmt::format("{}[{}ch {}B] ", i, aBufferInfo[i].numChannels, aBufferInfo[i].bufferSize);
        }
        u32 playing = 0, pending = 0, other_system = 0;
        for (auto& [handle, voice] : voice_registry) {
            const auto rack_it = rack_system.find(RackOfVoice(handle));
            if (rack_it != rack_system.end() && rack_it->second != systemHandle) {
                if (voice.playing) ++other_system;
                continue;
            }
            if (voice.playing) ++playing;
            if (voice.pending_play) ++pending;
        }
        LOG_INFO(Lib_Ngs2, "render #{} system {:#x}: buffers {}; voices playing {} (other systems {}), pending data {}",
                 seq, systemHandle, buffers, playing, other_system, pending);
        for (auto& [handle, voice] : voice_registry) {
            if (!voice.playing || !voice.has_data) {
                continue;
            }
            float peak = 0.0f;
            for (float sample : voice.grain_cache) {
                peak = std::max(peak, std::fabs(sample));
            }
            LOG_INFO(Lib_Ngs2, "  voice {:#x}: music {}, vol {:.3f}, matrix {:.3f}, pitch {:.2f}, grain peak {:.3f}, pos {:.1f}s, starved {}, paused {}, blocks {}",
                     handle, voice.is_music, voice.volume, voice.matrix_gain, voice.pitch, peak,
                     double(voice.decoded_samples) / 48000.0, voice.starved_renders, voice.paused, voice.blocks.size());
        }
    }
    for (u32 i = 0; i < numBufferInfo; ++i) {
        auto& info = aBufferInfo[i];
        const u32 channels = std::max(1u, info.numChannels);
        const u32 num_samples = static_cast<u32>(info.bufferSize / (sizeof(float) * channels));
        if (!info.buffer || num_samples == 0) {
            continue;
        }
        {
            // One-shot diagnostic per (system, channels): which outputs exist?
            static std::unordered_set<u64> seen_outputs;
            if (seen_outputs.insert((u64(systemHandle) << 8) | channels).second) {
                LOG_INFO(Lib_Ngs2, "render output: system = {:#x}, channels = {}, size = {}",
                         systemHandle, channels, info.bufferSize);
            }
        }
        // The game renders the same system into several buffers per cycle (main
        // 7.1, stereo fallback, pad speaker). Mix only into the multi-channel
        // main buffer; mixing into each would advance voices multiple times per
        // grain (audible as sped-up, chopped audio).
        if (channels < 8) {
            continue;
        }
        auto* out = static_cast<float*>(info.buffer);
        for (auto& [bus, peak] : bus_peak) {
            peak = {0.0f, 0.0f};
        }
        for (auto& [handle, voice] : voice_registry) {
            if (!voice.playing || voice.paused || !voice.has_data) {
                continue;
            }
            const auto rack_it = rack_system.find(RackOfVoice(handle));
            if (rack_it != rack_system.end() && rack_it->second != systemHandle) {
                continue;
            }
            // Pace grain production by wall-clock time so the voice advances at
            // 48kHz no matter how many output buffers are rendered per cycle;
            // extra renders in the same cycle reuse the cached grain. AnyPS4:
            // sceAudioOutOutput paces the title per chunk, so a render may run
            // up to 64 ms ahead of the clock before a grain is reused; the old
            // 8 ms window repeated one chunk in four when outputs came in bursts.
            const auto now = std::chrono::steady_clock::now();
            if (!voice.clock_valid) {
                voice.clock_valid = true;
                voice.clock_start = now;
                voice.clock_samples = 0;
            }
            const double elapsed =
                std::chrono::duration<double>(now - voice.clock_start).count();
            const double ahead = double(voice.clock_samples) / 48000.0 - elapsed;
            if (ahead < -0.5) {
                // Fell far behind (pause, lag spike): resync instead of bursting.
                voice.clock_start =
                    now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(double(voice.clock_samples) /
                                                            48000.0));
            }
            if (ahead < grain_window || voice.grain_len != num_samples) {
                // Gain: port volume; for music also the matrix peak (the game's
                // fade engine ramps music in/out through matrix levels — without
                // it, crossfade successor tracks play at their initial silence).
                float gain = voice.stop_requested && !music_hold ? voice.peak_volume : voice.volume;
                if (voice.is_music) {
                    gain *= voice.matrix_gain;
                    // Workaround: the game demands near-silence from music tracks
                    // it started (root cause of its decision still unidentified).
                    // Floor the music volume so the soundtrack stays audible; real
                    // track changes still swap via voice retirement.
                    static const float music_floor = [] {
                        const char* value = std::getenv("ORBIS_NGS2_MUSIC_FLOOR");
                        return value != nullptr ? static_cast<float>(std::atof(value)) : 0.6f;
                    }();
                    if (music_hold && gain < 0.25f) {
                        gain = music_floor;
                    }
                }
                ProduceGrain(voice, num_samples, std::clamp(gain, 0.0f, 4.0f));
                voice.clock_samples += num_samples;
            }
            float peak_left = 0.0f, peak_right = 0.0f;
            for (u32 s = 0; s < num_samples; ++s) {
                const float* src = voice.grain_cache.data() + size_t(s) * 2;
                float* dst = out + size_t(s) * channels;
                dst[0] += src[0];
                if (channels > 1) {
                    dst[1] += src[1];
                }
                peak_left = std::max(peak_left, std::fabs(src[0]));
                peak_right = std::max(peak_right, std::fabs(src[1]));
            }
            if (!voice.stop_requested) {
                auto& meter = bus_peak[voice.patch_dest];
                meter[0] = std::max(meter[0], peak_left);
                meter[1] = std::max(meter[1], peak_right);
            }
        }
        {
            // ~1 Hz probe: how many streamed voices play simultaneously? Overlapping
            // VAG chunks indicate broken chunk-chain sequencing.
            static u64 last_probe = 0;
            if (seq - last_probe > 500) {
                last_probe = seq;
                u32 vag_active = 0, at9_active = 0, other_active = 0;
                for (auto& [h, v] : voice_registry) {
                    if (!v.playing) {
                        continue;
                    }
                    if (v.pcm_kind == Voice::PcmKind::Vag) {
                        ++vag_active;
                    } else if (IsAt9Format(v.format)) {
                        ++at9_active;
                    } else {
                        ++other_active;
                    }
                }
                LOG_INFO(Lib_Ngs2,
                         "active voices: vag = {}, at9 = {}, other = {}, empty queries = {}",
                         vag_active, at9_active, other_active, empty_state_queries.load());
                for (auto& [h, v] : voice_registry) {
                    if (v.is_music && (v.playing || v.stop_requested)) {
                        LOG_INFO(Lib_Ngs2,
                                 "music {:#x}: playing = {}, vol = {:.3f}, matrix = {:.3f}, "
                                 "pos = {:.1f}s, starved = {}",
                                 h, v.playing, v.volume, v.matrix_gain,
                                 double(v.decoded_samples) / 48000.0, v.starved_renders);
                    }
                }
            }
        }
        // Guard against clipping from overlapping voices.
        const u32 total_floats = num_samples * channels;
        for (u32 s = 0; s < total_floats; ++s) {
            out[s] = std::clamp(out[s], -1.0f, 1.0f);
        }
        // Only mix into the primary buffer; auxiliary buffers stay silent to
        // avoid double audio until proper routing is implemented.
        break;
    }
    return ORBIS_OK;
} catch (const std::exception& e) {
    LOG_CRITICAL(Lib_Ngs2, "exception in sceNgs2SystemRender: {}", e.what());
    return ORBIS_OK;
}

static s32 PS4_SYSV_ABI sceNgs2SystemResetOption(OrbisNgs2SystemOption* outOption) {
    static const OrbisNgs2SystemOption option = {
        sizeof(OrbisNgs2SystemOption), "", 0, 512, 256, 48000, {0}};

    if (!outOption) {
        LOG_ERROR(Lib_Ngs2, "Invalid system option address {}", (void*)outOption);
        return ORBIS_NGS2_ERROR_INVALID_OPTION_ADDRESS;
    }
    *outOption = option;

    LOG_INFO(Lib_Ngs2, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemSetGrainSamples(OrbisNgs2Handle systemHandle, u32 numSamples) {
    LOG_ERROR(Lib_Ngs2, "numSamples = {}", numSamples);
    if (!systemHandle) {
        LOG_ERROR(Lib_Ngs2, "systemHandle is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemSetSampleRate(OrbisNgs2Handle systemHandle, u32 sampleRate) {
    LOG_ERROR(Lib_Ngs2, "sampleRate = {}", sampleRate);
    if (!systemHandle) {
        LOG_ERROR(Lib_Ngs2, "systemHandle is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemSetUserData(OrbisNgs2Handle systemHandle, uintptr_t userData) {
    LOG_ERROR(Lib_Ngs2, "userData = {}", userData);
    if (!systemHandle) {
        LOG_ERROR(Lib_Ngs2, "systemHandle is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2SystemUnlock(OrbisNgs2Handle systemHandle) {
    if (!systemHandle) {
        LOG_ERROR(Lib_Ngs2, "systemHandle is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    LOG_ERROR(Lib_Ngs2, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2VoiceControl(OrbisNgs2Handle voiceHandle,
                                     const OrbisNgs2VoiceParamHeader* paramList) try {
    LOG_DEBUG(Lib_Ngs2, "called");
    std::scoped_lock lk{engine_mutex};
    Voice& voice = voice_registry[voiceHandle];
    const auto* hdr = paramList;
    for (int i = 0; hdr && i < 64; ++i) {
        switch (hdr->id) {
        case SamplerSetup: {
            if (hdr->size >= sizeof(SamplerVoiceSetupParam)) {
                const auto* p = reinterpret_cast<const SamplerVoiceSetupParam*>(hdr);
                voice.format = p->format;
                if (trace_all) {
                    LOG_INFO(Lib_Ngs2, "setup voice {:#x} rack {:#x}: type {:#x}, {} ch, {} Hz, config {:#x}, flags {:#x}",
                             voiceHandle, RackOfVoice(voiceHandle), static_cast<u32>(p->format.waveformType),
                             static_cast<u32>(p->format.numChannels), static_cast<u32>(p->format.sampleRate),
                             static_cast<u32>(p->format.configData), static_cast<u32>(p->flags));
                }
                voice.blocks.clear();
                voice.known = true;
                voice.playing = false;
                voice.paused = false;
                voice.has_data = false;
                voice.add_calls = 0;
                ResetVoiceDecoder(voice);
            }
            break;
        }
        case SamplerAddWaveformBlocks: {
            if (hdr->size >= sizeof(SamplerVoiceWaveformBlocksParam)) {
                const auto* p = reinterpret_cast<const SamplerVoiceWaveformBlocksParam*>(hdr);
                if (p->data && p->aBlock) {
                    ++voice.add_calls;
                    const bool was_empty = voice.blocks.empty();
                    for (u32 b = 0; b < std::min(p->numBlocks, 64u); ++b) {
                        const auto& blk = p->aBlock[b];
                        if (blk.dataSize == 0) {
                            continue;
                        }
                        voice.blocks.push_back({static_cast<const u8*>(p->data) + blk.dataOffset,
                                                blk.dataSize, blk.numRepeats,
                                                blk.numSkipSamples, blk.userData, p->data});
                        voice.has_data = true;
                        voice.known = true;
                    }
                    u64 total_now = 0;
                    for (const auto& b : voice.blocks) {
                        total_now += b.size;
                    }
                    voice.is_music = total_now > 1'000'000;
                    {
                        std::scoped_lock loop_lk{loop_mutex};
                        const auto loop = loop_points.find(p->data);
                        if (loop != loop_points.end() && voice.is_music) {
                            voice.loop_start = loop->second.first;
                            voice.loop_end = loop->second.second;
                            LOG_INFO(Lib_Ngs2, "music voice {:#x} loops from sample {} to {}", voiceHandle, voice.loop_start,
                                     voice.loop_end);
                        } else {
                            voice.loop_start = 0;
                            voice.loop_end = 0;
                        }
                    }
                    if (was_empty && !voice.blocks.empty()) {
                        voice.repeats_left = voice.blocks.front().num_repeats;
                        voice.skip_remaining = voice.blocks.front().num_skip_samples;
                        if (voice.pending_play) {
                            voice.pending_play = false;
                            voice.playing = true;
                            voice.paused = false;
                        }
                    } else if (!was_empty && voice.playing) {
                        // Stream top-up while playing: part of the lifecycle trace.
                        static std::atomic<int> appends{0};
                        if (appends++ < 100) {
                            LOG_INFO(Lib_Ngs2, "stream append to playing voice {:#x} ({} blocks)",
                                     voiceHandle, voice.blocks.size());
                        }
                    }
                }
            }
            break;
        }
        case VoicePortVolume: {
            if (hdr->size >= sizeof(VoicePortVolumeParam)) {
                const auto* p = reinterpret_cast<const VoicePortVolumeParam*>(hdr);
                if (trace_all) {
                    LOG_INFO(Lib_Ngs2, "volume voice {:#x} port {} = {:.4f} (music {}, playing {})", voiceHandle, p->port,
                             p->level, voice.is_music, voice.playing);
                }
                if (p->port == 0 && std::isfinite(p->level)) {
                    voice.volume = std::clamp(p->level, 0.0f, 4.0f);
                    if (!voice.stop_requested) {
                        voice.peak_volume = std::max(voice.peak_volume, voice.volume);
                    }
                    if (voice.is_music) {
                        static std::atomic<int> vol_trace{0};
                        if (vol_trace++ < 400) {
                            LOG_INFO(Lib_Ngs2, "music {:#x} port{} volume = {:.4f}",
                                     voiceHandle, p->port, p->level);
                        }
                    }
                }
            }
            break;
        }
        case VoicePortMatrix:
            // Routing matrix selection; irrelevant to the simple stereo mix.
            break;
        case VoiceMatrixLevels: {
            // {header, u32 matrixId, u32 numLevels, const float* aLevel}: the game
            // fades voices by ramping these. Use the front-left level as gain.
            if (hdr->size >= sizeof(OrbisNgs2VoiceMatrixLevelsParam)) {
                const auto* p = reinterpret_cast<const OrbisNgs2VoiceMatrixLevelsParam*>(hdr);
                if (p->aLevel && p->numLevels >= 1) {
                    // Use the loudest channel so panning does not read as volume.
                    float peak = 0.0f;
                    for (u32 l = 0; l < std::min(p->numLevels, 16u); ++l) {
                        if (std::isfinite(p->aLevel[l])) {
                            peak = std::max(peak, std::fabs(p->aLevel[l]));
                        }
                    }
                    if (trace_all && std::fabs(peak - voice.matrix_gain) > 0.01f) {
                        LOG_INFO(Lib_Ngs2, "matrix voice {:#x} id {} levels {}: peak {:.3f} (music {}, playing {})",
                                 voiceHandle, p->matrixId, p->numLevels, peak, voice.is_music, voice.playing);
                    }
                    if (voice.is_music && std::fabs(peak - voice.matrix_gain) > 0.05f) {
                        LOG_INFO(Lib_Ngs2,
                                 "music {:#x} matrix update: peak = {:.3f} (levels {:.2f} {:.2f} "
                                 "{:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f})",
                                 voiceHandle, peak, p->numLevels > 0 ? p->aLevel[0] : 0.0f,
                                 p->numLevels > 1 ? p->aLevel[1] : 0.0f,
                                 p->numLevels > 2 ? p->aLevel[2] : 0.0f,
                                 p->numLevels > 3 ? p->aLevel[3] : 0.0f,
                                 p->numLevels > 4 ? p->aLevel[4] : 0.0f,
                                 p->numLevels > 5 ? p->aLevel[5] : 0.0f,
                                 p->numLevels > 6 ? p->aLevel[6] : 0.0f,
                                 p->numLevels > 7 ? p->aLevel[7] : 0.0f);
                    }
                    voice.matrix_gain = std::clamp(peak, 0.0f, 4.0f);
                }
            }
            break;
        }
        case VoicePatch: {
            if (hdr->size >= sizeof(OrbisNgs2VoicePatchParam)) {
                const auto* p = reinterpret_cast<const OrbisNgs2VoicePatchParam*>(hdr);
                voice.patch_dest = p->destHandle;
            }
            break;
        }
        case SamplerPitch: {
            if (hdr->size >= 12) {
                float ratio;
                std::memcpy(&ratio, reinterpret_cast<const u8*>(hdr) + sizeof(*hdr),
                            sizeof(float));
                if (std::isfinite(ratio) && ratio > 0.0f) {
                    if (ratio != 1.0f) {
                        static std::atomic<bool> warned{false};
                        if (!warned.exchange(true)) {
                            LOG_WARNING(Lib_Ngs2,
                                        "Game uses non-unity sampler pitch ({}); resampling not "
                                        "implemented yet",
                                        ratio);
                        }
                    }
                    voice.pitch = ratio;
                }
            }
            break;
        }
        case VoiceEvent: {
            const auto event = reinterpret_cast<const OrbisNgs2VoiceEventParam*>(hdr)->eventId;
            // Capped lifecycle trace to reconstruct streaming/music behavior.
            static std::atomic<int> trace_count{0};
            if (trace_all || trace_count++ < 400) {
                u64 total = 0;
                for (const auto& b : voice.blocks) {
                    total += b.size;
                }
                LOG_INFO(Lib_Ngs2, "evt {} voice {:#x} rack {:#x} (blocks = {}, bytes = {}, at9 = {}, vol {:.3f}, matrix {:.3f}, patch {:#x}, pos {:.1f}s)",
                         event, voiceHandle, RackOfVoice(voiceHandle), voice.blocks.size(), total,
                         IsAt9Format(voice.format), voice.volume, voice.matrix_gain, voice.patch_dest,
                         double(voice.decoded_samples) / 48000.0);
            }
            switch (event) {
            case EventPlay: {
                if (voice.has_data) {
                    u64 own_bytes = 0;
                    for (const auto& b : voice.blocks) {
                        own_bytes += b.size;
                    }
                    if (own_bytes > 1'000'000) {
                        // A new music track starts: retire any music voice whose
                        // Stop we previously suppressed (track change).
                        for (auto& [other_handle, other] : voice_registry) {
                            if (other_handle != voiceHandle && other.stop_requested) {
                                other.playing = false;
                                other.stop_requested = false;
                            }
                        }
                    }
                    RewindVoice(voice);
                } else {
                    // Control/mastering voices play without data; report them as
                    // steadily playing so the game trusts its audio bus. If data
                    // arrives later, playback starts then.
                    voice.pending_play = true;
                    voice.known = true;
                }
                voice.playing = true;
                voice.paused = false;
                voice.stop_requested = false;
                voice.peak_volume = voice.volume;
                static std::unordered_set<uintptr_t> logged_handles;
                if (logged_handles.insert(voiceHandle).second) {
                    u64 total_bytes = 0;
                    for (const auto& b : voice.blocks) {
                        total_bytes += b.size;
                    }
                    LOG_INFO(Lib_Ngs2,
                             "Play voice {:#x}: blocks = {}, bytes = {}, rate = {}, ch = {}, "
                             "type = {}, config = {:#x}, has_data = {}",
                             voiceHandle, voice.blocks.size(), total_bytes,
                             voice.format.sampleRate, voice.format.numChannels,
                             voice.format.waveformType, voice.format.configData,
                             voice.has_data);
                }
                break;
            }
            case EventPause:
                voice.paused = true;
                break;
            case EventResume:
                voice.paused = false;
                break;
            case EventKill:
                // The game also aborts music via Kill; suppress it the same way
                // as Stop (the next music track's Play retires this voice).
                if (voice.playing && voice.is_music && (music_hold || ShouldHoldMusic(voiceHandle, voice))) {
                    voice.stop_requested = true;
                    LOG_INFO(Lib_Ngs2, "holding music voice {:#x} through Kill at {:.1f}s (volume {:.3f})", voiceHandle,
                             double(voice.decoded_samples) / 48000.0, voice.peak_volume);
                    break;
                }
                // Kill frees the voice entirely.
                voice.playing = false;
                voice.pending_play = false;
                voice.blocks.clear();
                voice.has_data = false;
                voice.is_music = false;
                ResetVoiceDecoder(voice);
                voice.block_index = 0;
                voice.byte_pos = 0;
                break;
            case EventStop:
            case EventStopImm:
            default: {
                // The game aborts long music tracks shortly after starting them
                // (its bookkeeping rejects our partial state emulation). Keep them
                // playing but remember the request: the next music track's Play
                // retires them, so track changes still work.
                u64 total_bytes = 0;
                for (const auto& b : voice.blocks) {
                    total_bytes += b.size;
                }
                if (voice.playing && total_bytes > 1'000'000 && (music_hold || ShouldHoldMusic(voiceHandle, voice))) {
                    voice.stop_requested = true;
                    LOG_INFO(Lib_Ngs2, "holding music voice {:#x} through Stop at {:.1f}s (volume {:.3f})", voiceHandle,
                             double(voice.decoded_samples) / 48000.0, voice.peak_volume);
                    break;
                }
                // Stop halts playback but keeps the loaded waveform so the
                // game can Play the same sound again.
                voice.playing = false;
                voice.pending_play = false;
                RewindVoice(voice);
                break;
            }
            }
            break;
        }
        default: {
            // Log unknown params once each, with payload, to map remaining features
            // (pitch, envelopes, filters).
            static std::mutex seen_mutex;
            static std::unordered_set<u32> seen_ids;
            std::scoped_lock seen_lk{seen_mutex};
            if (seen_ids.insert(hdr->id).second) {
                const auto* bytes = reinterpret_cast<const u8*>(hdr) + sizeof(*hdr);
                const u32 payload =
                    hdr->size > sizeof(*hdr) ? std::min<u32>(hdr->size - sizeof(*hdr), 16) : 0;
                std::string hex;
                for (u32 b = 0; b < payload; ++b) {
                    hex += fmt::format("{:02x} ", bytes[b]);
                }
                LOG_INFO(Lib_Ngs2, "Unhandled voice param id = {:#x} size = {} payload = {}",
                         hdr->id, hdr->size, hex);
            }
            break;
        }
        }
        if (hdr->next <= 0) {
            break;
        }
        hdr = reinterpret_cast<const OrbisNgs2VoiceParamHeader*>(
            reinterpret_cast<const u8*>(hdr) + hdr->next);
    }
    return ORBIS_OK;
} catch (const std::exception& e) {
    LOG_CRITICAL(Lib_Ngs2, "exception in sceNgs2VoiceControl: {}", e.what());
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2VoiceGetMatrixInfo(OrbisNgs2Handle voiceHandle, u32 matrixId,
                                           OrbisNgs2VoiceMatrixInfo* outInfo, size_t outInfoSize) {
    LOG_ERROR(Lib_Ngs2, "matrixId = {}, outInfoSize = {}", matrixId, outInfoSize);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2VoiceGetOwner(OrbisNgs2Handle voiceHandle, OrbisNgs2Handle* outRackHandle,
                                      u32* outVoiceId) {
    LOG_ERROR(Lib_Ngs2, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2VoiceGetPortInfo(OrbisNgs2Handle voiceHandle, u32 port,
                                         OrbisNgs2VoicePortInfo* outInfo, size_t outInfoSize) {
    LOG_ERROR(Lib_Ngs2, "port = {}, outInfoSize = {}", port, outInfoSize);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2VoiceGetState(OrbisNgs2Handle voiceHandle, OrbisNgs2VoiceState* outState,
                                      size_t stateSize) {
    LOG_DEBUG(Lib_Ngs2, "stateSize = {}", stateSize);
    if (outState && stateSize >= sizeof(OrbisNgs2VoiceState)) {
        std::memset(outState, 0, stateSize);
        std::scoped_lock lk{engine_mutex};
        const auto it = voice_registry.find(voiceHandle);
        if (it != voice_registry.end() && it->second.known) {
            // Real voice state from the playback engine. Layout of the extended
            // sampler state: stateFlags, envelopeHeight, peakHeight, reserved,
            // then u64 numDecodedSamples.
            const Voice& v = it->second;
            // A mastering voice is a bus: once the title played it, it stays
            // PLAYING and its meters show what was mixed into it (zero when
            // silent). The fork reported it idle with meters stuck at one,
            // which made the title fade its music out after a second and, in
            // the other variant, duck it permanently.
            if (mastering_meters && IsMasteringVoice(voiceHandle)) {
                outState->stateFlags = NGS2_VOICE_STATE_FLAG_INUSE | NGS2_VOICE_STATE_FLAG_ACTIVE |
                                       (v.playing ? NGS2_VOICE_STATE_FLAG_PLAYING : 0);
                if (stateSize >= sizeof(OrbisNgs2MasteringVoiceState)) {
                    auto* state = reinterpret_cast<OrbisNgs2MasteringVoiceState*>(outState);
                    const auto meter = bus_peak.find(voiceHandle);
                    const float left = meter != bus_peak.end() ? std::min(meter->second[0], 1.0f) : 0.0f;
                    const float right = meter != bus_peak.end() ? std::min(meter->second[1], 1.0f) : 0.0f;
                    state->limiterPeakLevel = std::max(left, right);
                    state->limiterPressLevel = 0.0f;
                    state->aInputPeakHeight[0] = left;
                    state->aInputPeakHeight[1] = right;
                    state->aOutputPeakHeight[0] = left;
                    state->aOutputPeakHeight[1] = right;
                }
                if (trace_all) {
                    static std::atomic<int> bus_logs{0};
                    if (bus_logs++ < 24) {
                        LOG_INFO(Lib_Ngs2, "state mastering {:#x} size {}: flags {:#x}", voiceHandle, stateSize, outState->stateFlags);
                    }
                }
                return ORBIS_OK;
            }
            const bool playing_now = v.playing && v.has_data && v.starved_renders < 2;
            outState->stateFlags =
                NGS2_VOICE_STATE_FLAG_INUSE | NGS2_VOICE_STATE_FLAG_ACTIVE |
                (playing_now ? NGS2_VOICE_STATE_FLAG_PLAYING : 0);
            u32* words = reinterpret_cast<u32*>(outState);
            if (stateSize >= 24) {
                const float height = playing_now ? 1.0f : 0.0f;
                std::memcpy(&words[1], &height, sizeof(float));
                std::memcpy(&words[2], &height, sizeof(float));
                std::memcpy(&words[4], &v.decoded_samples, sizeof(u64));
            }
            // AnyPS4: the sampler state also reports the bytes consumed, the
            // user data of the block being played and the waveform base
            // pointer; titles drive their streaming and fades from them.
            if (sampler_ext_state && stateSize >= sizeof(OrbisNgs2SamplerVoiceState)) {
                u64 consumed = 0;
                uintptr_t user_data = 0;
                const void* base = nullptr;
                for (size_t b = 0; b < v.blocks.size(); ++b) {
                    if (b < v.block_index) {
                        consumed += v.blocks[b].size;
                    } else if (b == v.block_index) {
                        consumed += std::min<u64>(v.byte_pos, v.blocks[b].size);
                        user_data = v.blocks[b].user_data;
                        base = v.blocks[b].base;
                    }
                }
                if (!v.blocks.empty() && v.block_index >= v.blocks.size()) {
                    user_data = v.blocks.back().user_data;
                    base = v.blocks.back().base;
                }
                auto* state = reinterpret_cast<OrbisNgs2SamplerVoiceState*>(outState);
                state->decodedDataSize = consumed;
                state->userData = user_data;
                state->waveformData = base;
            }
            if (trace_all) {
                static std::atomic<int> state_logs{0};
                if (state_logs++ < 40) {
                    LOG_INFO(Lib_Ngs2, "state voice {:#x} size {}: flags {:#x}, playing {}, decoded {} samples, block {}/{}, byte {}",
                             voiceHandle, stateSize, outState->stateFlags, playing_now, v.decoded_samples, v.block_index,
                             v.blocks.size(), v.byte_pos);
                }
            }
        } else if (IsSyntheticHandle(voiceHandle)) {
            // A never-configured voice slot: honestly empty (buffer already zeroed).
            ++empty_state_queries;
        } else {
            // Foreign/garbage handle: alternate playing/idle plus "finished"
            // progress values so no polling pattern can spin forever.
            outState->stateFlags = NextVoiceStateFlags();
            u32* words = reinterpret_cast<u32*>(outState);
            const size_t num_words = std::min<size_t>(stateSize / sizeof(u32), 8);
            for (size_t i = 1; i < num_words; ++i) {
                words[i] = 0x3F800000;
            }
        }
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2VoiceGetStateFlags(OrbisNgs2Handle voiceHandle, u32* outStateFlags) {
    LOG_DEBUG(Lib_Ngs2, "called");
    if (outStateFlags) {
        std::scoped_lock lk{engine_mutex};
        const auto it = voice_registry.find(voiceHandle);
        if (it != voice_registry.end() && it->second.known) {
            const bool playing_now = mastering_meters && IsMasteringVoice(voiceHandle)
                                         ? it->second.playing
                                         : it->second.playing && it->second.has_data && it->second.starved_renders < 2;
            *outStateFlags = NGS2_VOICE_STATE_FLAG_INUSE | NGS2_VOICE_STATE_FLAG_ACTIVE |
                             (playing_now ? NGS2_VOICE_STATE_FLAG_PLAYING : 0);
        } else if (IsSyntheticHandle(voiceHandle)) {
            *outStateFlags = 0;
            ++empty_state_queries;
        } else {
            *outStateFlags = NextVoiceStateFlags();
        }
    }
    return ORBIS_OK;
}

// Ngs2Custom

s32 PS4_SYSV_ABI sceNgs2CustomRackGetModuleInfo(OrbisNgs2Handle rackHandle, u32 moduleIndex,
                                                OrbisNgs2CustomModuleInfo* outInfo,
                                                size_t infoSize) {
    LOG_ERROR(Lib_Ngs2, "moduleIndex = {}, infoSize = {}", moduleIndex, infoSize);
    return ORBIS_OK;
}

// Ngs2Geom

s32 PS4_SYSV_ABI sceNgs2GeomResetListenerParam(OrbisNgs2GeomListenerParam* outListenerParam) {
    LOG_ERROR(Lib_Ngs2, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2GeomResetSourceParam(OrbisNgs2GeomSourceParam* outSourceParam) {
    LOG_ERROR(Lib_Ngs2, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2GeomCalcListener(const OrbisNgs2GeomListenerParam* param,
                                         OrbisNgs2GeomListenerWork* outWork, u32 flags) {
    LOG_ERROR(Lib_Ngs2, "flags = {}", flags);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2GeomApply(const OrbisNgs2GeomListenerWork* listener,
                                  const OrbisNgs2GeomSourceParam* source,
                                  OrbisNgs2GeomAttribute* outAttrib, u32 flags) {
    LOG_ERROR(Lib_Ngs2, "flags = {}", flags);
    return ORBIS_OK;
}

// Ngs2Pan

s32 PS4_SYSV_ABI sceNgs2PanInit(OrbisNgs2PanWork* work, const float* aSpeakerAngle, float unitAngle,
                                u32 numSpeakers) {
    LOG_INFO(Lib_Ngs2, "unitAngle = {}, numSpeakers = {}", unitAngle, numSpeakers);
    if (!work) {
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    work->numSpeakers = std::min<u32>(numSpeakers, ORBIS_NGS2_MAX_VOICE_CHANNELS);
    work->unitAngle = unitAngle != 0.0f ? unitAngle : 6.2831853f;
    for (u32 i = 0; i < ORBIS_NGS2_MAX_VOICE_CHANNELS; ++i) {
        work->aSpeakerAngle[i] =
            (aSpeakerAngle && i < work->numSpeakers) ? aSpeakerAngle[i] : 0.0f;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2PanGetVolumeMatrix(OrbisNgs2PanWork* work, const OrbisNgs2PanParam* aParam,
                                           u32 numParams, u32 matrixFormat,
                                           float* outVolumeMatrix) {
    LOG_DEBUG(Lib_Ngs2, "numParams = {}, matrixFormat = {}", numParams, matrixFormat);
    if (!work || !outVolumeMatrix) {
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    const u32 speakers = std::max(1u, work->numSpeakers);
    const float span = std::fabs(work->unitAngle) > 1e-6f ? work->unitAngle : 6.2831853f;
    constexpr float kTwoPi = 6.2831853f;
    for (u32 p = 0; p < numParams; ++p) {
        const OrbisNgs2PanParam& param = aParam ? aParam[p] : OrbisNgs2PanParam{};
        const float level =
            std::isfinite(param.fbwLevel) ? std::clamp(param.fbwLevel, 0.0f, 4.0f) : 1.0f;
        // Simple distance attenuation: 1 at 0, softening with distance.
        const float dist = std::isfinite(param.distance) ? std::max(param.distance, 0.0f) : 0.0f;
        const float atten = 1.0f / (1.0f + dist);
        float* row = outVolumeMatrix + size_t(p) * speakers;
        float total = 0.0f;
        for (u32 s = 0; s < speakers; ++s) {
            // Angular proximity window between the source angle and each speaker.
            float delta = param.angle - work->aSpeakerAngle[s];
            delta = std::fmod(delta + kTwoPi * 4, kTwoPi);
            if (delta > kTwoPi / 2) {
                delta -= kTwoPi;
            }
            const float w = std::max(0.0f, 1.0f - std::fabs(delta) / (span * 0.5f + 1e-6f));
            row[s] = w;
            total += w;
        }
        if (total <= 1e-6f) {
            // Source between speakers: give everything to the front pair.
            for (u32 s = 0; s < speakers; ++s) {
                row[s] = s < 2 ? 0.7071f : 0.0f;
            }
            total = 1.4142f;
        }
        // Normalize to constant power and apply source level and distance.
        for (u32 s = 0; s < speakers; ++s) {
            row[s] = row[s] / total * level * atten;
        }
    }
    return ORBIS_OK;
}

// Ngs2Report

s32 PS4_SYSV_ABI sceNgs2ReportRegisterHandler(u32 reportType, OrbisNgs2ReportHandler handler,
                                              uintptr_t userData, OrbisNgs2Handle* outHandle) {
    LOG_INFO(Lib_Ngs2, "reportType = {}, userData = {}", reportType, userData);
    if (!handler) {
        LOG_ERROR(Lib_Ngs2, "handler is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_REPORT_HANDLE;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNgs2ReportUnregisterHandler(OrbisNgs2Handle reportHandle) {
    if (!reportHandle) {
        LOG_ERROR(Lib_Ngs2, "reportHandle is nullptr");
        return ORBIS_NGS2_ERROR_INVALID_REPORT_HANDLE;
    }
    LOG_INFO(Lib_Ngs2, "called");
    return ORBIS_OK;
}

// Unknown

int PS4_SYSV_ABI sceNgs2FftInit() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2FftProcess() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2FftQuerySize() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2JobSchedulerResetOption() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2ModuleArrayEnumItems() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2ModuleEnumConfigs() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2ModuleQueueEnumItems() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2RackQueryInfo() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2RackRunCommands() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2SystemQueryInfo() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2SystemRunCommands() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2SystemSetLoudThreshold() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2StreamCreate() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2StreamCreateWithAllocator() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2StreamDestroy() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2StreamQueryBufferSize() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2StreamQueryInfo() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2StreamResetOption() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2StreamRunCommands() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2VoiceQueryInfo() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNgs2VoiceRunCommands() {
    LOG_ERROR(Lib_Ngs2, "(STUBBED) called");
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("3pCNbVM11UA", "libSceNgs2", 1, "libSceNgs2", sceNgs2CalcWaveformBlock);
    LIB_FUNCTION("6qN1zaEZuN0", "libSceNgs2", 1, "libSceNgs2", sceNgs2CustomRackGetModuleInfo);
    LIB_FUNCTION("Kg1MA5j7KFk", "libSceNgs2", 1, "libSceNgs2", sceNgs2FftInit);
    LIB_FUNCTION("D8eCqBxSojA", "libSceNgs2", 1, "libSceNgs2", sceNgs2FftProcess);
    LIB_FUNCTION("-YNfTO6KOMY", "libSceNgs2", 1, "libSceNgs2", sceNgs2FftQuerySize);
    LIB_FUNCTION("eF8yRCC6W64", "libSceNgs2", 1, "libSceNgs2", sceNgs2GeomApply);
    LIB_FUNCTION("1WsleK-MTkE", "libSceNgs2", 1, "libSceNgs2", sceNgs2GeomCalcListener);
    LIB_FUNCTION("7Lcfo8SmpsU", "libSceNgs2", 1, "libSceNgs2", sceNgs2GeomResetListenerParam);
    LIB_FUNCTION("0lbbayqDNoE", "libSceNgs2", 1, "libSceNgs2", sceNgs2GeomResetSourceParam);
    LIB_FUNCTION("ekGJmmoc8j4", "libSceNgs2", 1, "libSceNgs2", sceNgs2GetWaveformFrameInfo);
    LIB_FUNCTION("BcoPfWfpvVI", "libSceNgs2", 1, "libSceNgs2", sceNgs2JobSchedulerResetOption);
    LIB_FUNCTION("EEemGEQCjO8", "libSceNgs2", 1, "libSceNgs2", sceNgs2ModuleArrayEnumItems);
    LIB_FUNCTION("TaoNtmMKkXQ", "libSceNgs2", 1, "libSceNgs2", sceNgs2ModuleEnumConfigs);
    LIB_FUNCTION("ve6bZi+1sYQ", "libSceNgs2", 1, "libSceNgs2", sceNgs2ModuleQueueEnumItems);
    LIB_FUNCTION("gbMKV+8Enuo", "libSceNgs2", 1, "libSceNgs2", sceNgs2PanGetVolumeMatrix);
    LIB_FUNCTION("xa8oL9dmXkM", "libSceNgs2", 1, "libSceNgs2", sceNgs2PanInit);
    LIB_FUNCTION("hyVLT2VlOYk", "libSceNgs2", 1, "libSceNgs2", sceNgs2ParseWaveformData);
    LIB_FUNCTION("iprCTXPVWMI", "libSceNgs2", 1, "libSceNgs2", sceNgs2ParseWaveformFile);
    LIB_FUNCTION("t9T0QM17Kvo", "libSceNgs2", 1, "libSceNgs2", sceNgs2ParseWaveformUser);
    LIB_FUNCTION("cLV4aiT9JpA", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackCreate);
    LIB_FUNCTION("U546k6orxQo", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackCreateWithAllocator);
    LIB_FUNCTION("lCqD7oycmIM", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackDestroy);
    LIB_FUNCTION("M4LYATRhRUE", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackGetInfo);
    LIB_FUNCTION("Mn4XNDg03XY", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackGetUserData);
    LIB_FUNCTION("MwmHz8pAdAo", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackGetVoiceHandle);
    LIB_FUNCTION("MzTa7VLjogY", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackLock);
    LIB_FUNCTION("0eFLVCfWVds", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackQueryBufferSize);
    LIB_FUNCTION("TZqb8E-j3dY", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackQueryInfo);
    LIB_FUNCTION("MI2VmBx2RbM", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackRunCommands);
    LIB_FUNCTION("JNTMIaBIbV4", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackSetUserData);
    LIB_FUNCTION("++YZ7P9e87U", "libSceNgs2", 1, "libSceNgs2", sceNgs2RackUnlock);
    LIB_FUNCTION("uBIN24Tv2MI", "libSceNgs2", 1, "libSceNgs2", sceNgs2ReportRegisterHandler);
    LIB_FUNCTION("nPzb7Ly-VjE", "libSceNgs2", 1, "libSceNgs2", sceNgs2ReportUnregisterHandler);
    LIB_FUNCTION("koBbCMvOKWw", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemCreate);
    LIB_FUNCTION("mPYgU4oYpuY", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemCreateWithAllocator);
    LIB_FUNCTION("u-WrYDaJA3k", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemDestroy);
    LIB_FUNCTION("vubFP0T6MP0", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemEnumHandles);
    LIB_FUNCTION("U-+7HsswcIs", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemEnumRackHandles);
    LIB_FUNCTION("vU7TQ62pItw", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemGetInfo);
    LIB_FUNCTION("4lFaRxd-aLs", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemGetUserData);
    LIB_FUNCTION("gThZqM5PYlQ", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemLock);
    LIB_FUNCTION("pgFAiLR5qT4", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemQueryBufferSize);
    LIB_FUNCTION("3oIK7y7O4k0", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemQueryInfo);
    LIB_FUNCTION("i0VnXM-C9fc", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemRender);
    LIB_FUNCTION("AQkj7C0f3PY", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemResetOption);
    LIB_FUNCTION("gXiormHoZZ4", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemRunCommands);
    LIB_FUNCTION("l4Q2dWEH6UM", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemSetGrainSamples);
    LIB_FUNCTION("Wdlx0ZFTV9s", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemSetLoudThreshold);
    LIB_FUNCTION("-tbc2SxQD60", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemSetSampleRate);
    LIB_FUNCTION("GZB2v0XnG0k", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemSetUserData);
    LIB_FUNCTION("JXRC5n0RQls", "libSceNgs2", 1, "libSceNgs2", sceNgs2SystemUnlock);
    LIB_FUNCTION("sU2St3agdjg", "libSceNgs2", 1, "libSceNgs2", sceNgs2StreamCreate);
    LIB_FUNCTION("I+RLwaauggA", "libSceNgs2", 1, "libSceNgs2", sceNgs2StreamCreateWithAllocator);
    LIB_FUNCTION("bfoMXnTRtwE", "libSceNgs2", 1, "libSceNgs2", sceNgs2StreamDestroy);
    LIB_FUNCTION("dxulc33msHM", "libSceNgs2", 1, "libSceNgs2", sceNgs2StreamQueryBufferSize);
    LIB_FUNCTION("rfw6ufRsmow", "libSceNgs2", 1, "libSceNgs2", sceNgs2StreamQueryInfo);
    LIB_FUNCTION("q+2W8YdK0F8", "libSceNgs2", 1, "libSceNgs2", sceNgs2StreamResetOption);
    LIB_FUNCTION("qQHCi9pjDps", "libSceNgs2", 1, "libSceNgs2", sceNgs2StreamRunCommands);
    LIB_FUNCTION("uu94irFOGpA", "libSceNgs2", 1, "libSceNgs2", sceNgs2VoiceControl);
    LIB_FUNCTION("jjBVvPN9964", "libSceNgs2", 1, "libSceNgs2", sceNgs2VoiceGetMatrixInfo);
    LIB_FUNCTION("W-Z8wWMBnhk", "libSceNgs2", 1, "libSceNgs2", sceNgs2VoiceGetOwner);
    LIB_FUNCTION("WCayTgob7-o", "libSceNgs2", 1, "libSceNgs2", sceNgs2VoiceGetPortInfo);
    LIB_FUNCTION("-TOuuAQ-buE", "libSceNgs2", 1, "libSceNgs2", sceNgs2VoiceGetState);
    LIB_FUNCTION("rEh728kXk3w", "libSceNgs2", 1, "libSceNgs2", sceNgs2VoiceGetStateFlags);
    LIB_FUNCTION("9eic4AmjGVI", "libSceNgs2", 1, "libSceNgs2", sceNgs2VoiceQueryInfo);
    LIB_FUNCTION("AbYvTOZ8Pts", "libSceNgs2", 1, "libSceNgs2", sceNgs2VoiceRunCommands);
};

} // namespace Libraries::Ngs2
