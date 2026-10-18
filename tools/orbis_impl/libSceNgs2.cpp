#include <common/OrbisHle.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using OrbisNgs2Handle = std::uintptr_t;

constexpr s32 kErrorInvalidOutAddress = static_cast<s32>(0x804A0053);
constexpr s32 kErrorInvalidVoiceHandle = static_cast<s32>(0x804A0302);
constexpr int kMaxBlocks = 4;

struct OrbisNgs2WaveformFormat {
    u32 waveformType;
    u32 numChannels;
    u32 sampleRate;
    u32 configData;
    u32 frameOffset;
    u32 frameMargin;
};

struct OrbisNgs2WaveformBlock {
    u32 dataOffset;
    u32 dataSize;
    u32 numRepeats;
    u32 numSkipSamples;
    u32 numSamples;
    u32 reserved;
    std::uintptr_t userData;
};

struct OrbisNgs2WaveformInfo {
    OrbisNgs2WaveformFormat format;
    u32 dataOffset;
    u32 dataSize;
    u32 loopBeginPosition;
    u32 loopEndPosition;
    u32 numSamples;
    u32 audioUnitSize;
    u32 numAudioUnitSamples;
    u32 numAudioUnitPerFrame;
    u32 audioFrameSize;
    u32 numAudioFrameSamples;
    u32 numDelaySamples;
    u32 numBlocks;
    OrbisNgs2WaveformBlock aBlock[kMaxBlocks];
};

struct WaveformUnit {
    u32 size;
    u32 samples;
};

bool DecodeAt9Config(const u32 config, WaveformUnit& out) {
    const u8 b0 = config & 0xFF;
    const u8 b1 = (config >> 8) & 0xFF;
    const u8 b2 = (config >> 16) & 0xFF;
    const u8 b3 = (config >> 24) & 0xFF;
    if (b0 != 0xFE) return false;
    static constexpr u32 frameSamplesPower[16] = {6, 6, 7, 7, 7, 8, 8, 8, 6, 6, 7, 7, 7, 8, 8, 8};
    const u32 srIndex = b1 >> 4;
    const u32 frameBytes = ((static_cast<u32>(b2) << 3) | (b3 >> 5)) + 1;
    const u32 superframeShift = (b3 >> 3) & 0x3;
    out.size = frameBytes << superframeShift;
    out.samples = (1u << frameSamplesPower[srIndex]) << superframeShift;
    return out.size != 0 && out.samples != 0;
}

bool ParseRiffWaveform(const void* data, const std::size_t dataSize, OrbisNgs2WaveformInfo* out) {
    const auto* p = static_cast<const u8*>(data);
    if (p == nullptr || dataSize < 12) return false;
    const auto rd32 = [&](std::size_t off) { u32 v; std::memcpy(&v, p + off, 4); return v; };
    const auto rd16 = [&](std::size_t off) { u16 v; std::memcpy(&v, p + off, 2); return v; };
    if (std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WAVE", 4) != 0) return false;
    const std::size_t riffEnd = std::min(dataSize, static_cast<std::size_t>(rd32(4)) + 8);

    u16 formatTag = 0, numChannels = 0, blockAlign = 0, samplesPerBlock = 0;
    u32 sampleRate = 0, configData = 0, numSamples = 0, delaySamples = 0, dataOff = 0, dataBytes = 0;
    bool hasFact = false;
    std::size_t off = 12;
    while (off + 8 <= riffEnd) {
        const u32 cid = rd32(off);
        const u32 csz = rd32(off + 4);
        const std::size_t body = off + 8;
        if (body + csz > riffEnd) break;
        if (cid == 0x20746d66u && csz >= 16) {
            formatTag = rd16(body);
            numChannels = rd16(body + 2);
            sampleRate = rd32(body + 4);
            blockAlign = rd16(body + 12);
            if (formatTag == 0xFFFE && csz >= 52) {
                samplesPerBlock = rd16(body + 18);
                configData = rd32(body + 44);
            }
        } else if (cid == 0x74636166u && csz >= 4) {
            numSamples = rd32(body);
            hasFact = true;
            if (csz >= 12) delaySamples = rd32(body + 8);
        } else if (cid == 0x61746164u) {
            dataOff = static_cast<u32>(body);
            dataBytes = csz;
        }
        off = body + csz + (csz & 1);
    }
    if (sampleRate == 0 || numChannels == 0 || dataBytes == 0) return false;

    WaveformUnit unit{};
    if (!DecodeAt9Config(configData, unit)) {
        unit.size = blockAlign != 0 ? blockAlign : numChannels * 2u;
        unit.samples = samplesPerBlock != 0 ? samplesPerBlock : 1u;
    }
    if (!hasFact) numSamples = static_cast<u32>(static_cast<u64>(dataBytes) / unit.size * unit.samples);

    std::memset(out, 0, sizeof(*out));
    out->format.waveformType = formatTag;
    out->format.numChannels = numChannels;
    out->format.sampleRate = sampleRate;
    out->format.configData = configData;
    out->dataOffset = dataOff;
    out->dataSize = dataBytes;
    out->numSamples = numSamples;
    out->audioUnitSize = unit.size;
    out->numAudioUnitSamples = unit.samples;
    out->numAudioUnitPerFrame = 1;
    out->audioFrameSize = unit.size;
    out->numAudioFrameSamples = unit.samples;
    out->numDelaySamples = delaySamples;
    out->numBlocks = 1;
    out->aBlock[0].dataOffset = dataOff;
    out->aBlock[0].dataSize = dataBytes;
    out->aBlock[0].numSamples = numSamples;
    return true;
}

void FillDefaultWaveform(const std::size_t dataSize, OrbisNgs2WaveformInfo* out) {
    std::memset(out, 0, sizeof(*out));
    out->format.numChannels = 2;
    out->format.sampleRate = 48000;
    out->dataSize = static_cast<u32>(dataSize);
    out->numSamples = static_cast<u32>(dataSize / 4);
    out->audioUnitSize = 4;
    out->numAudioUnitSamples = 1;
    out->numAudioUnitPerFrame = 1;
    out->audioFrameSize = 4;
    out->numAudioFrameSamples = 1;
    out->numBlocks = 1;
    out->aBlock[0].dataSize = out->dataSize;
    out->aBlock[0].numSamples = out->numSamples;
}

void Trace(const char* function, const char* nid) {
    static std::uint64_t calls = 0;
    OrbisHleTrace("libSceNgs2.prx", function, nid, ++calls, __builtin_return_address(0));
}

}  // namespace

extern "C" {

ORBIS_SYSV_ABI s32 orbis_sceNgs2ParseWaveformData(const void* data, std::size_t dataSize, OrbisNgs2WaveformInfo* outInfo) {
    Trace("sceNgs2ParseWaveformData", "hyVLT2VlOYk");
    if (outInfo == nullptr) return kErrorInvalidOutAddress;
    if (!ParseRiffWaveform(data, dataSize, outInfo)) FillDefaultWaveform(dataSize, outInfo);
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceNgs2SystemCreateWithAllocator(const void*, const void*, OrbisNgs2Handle* outHandle) {
    Trace("sceNgs2SystemCreateWithAllocator", "mPYgU4oYpuY");
    if (outHandle == nullptr) return kErrorInvalidOutAddress;
    *outHandle = 0x1000;
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceNgs2RackCreateWithAllocator(OrbisNgs2Handle, u32 rackId, const void*, const void*, OrbisNgs2Handle* outHandle) {
    Trace("sceNgs2RackCreateWithAllocator", "U546k6orxQo");
    if (outHandle == nullptr) return kErrorInvalidOutAddress;
    static u32 racks = 0;
    *outHandle = 0x2000 + (static_cast<OrbisNgs2Handle>(++racks) << 16) + (rackId & 0xFFFF);
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceNgs2RackGetVoiceHandle(OrbisNgs2Handle rackHandle, u32 voiceIndex, OrbisNgs2Handle* outHandle) {
    Trace("sceNgs2RackGetVoiceHandle", "MwmHz8pAdAo");
    if (outHandle == nullptr) return kErrorInvalidOutAddress;
    if (rackHandle == 0) return kErrorInvalidVoiceHandle;
    *outHandle = (rackHandle << 12) + voiceIndex + 1;
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceNgs2VoiceGetState(OrbisNgs2Handle, void* outState, std::size_t stateSize) {
    Trace("sceNgs2VoiceGetState", "-TOuuAQ-buE");
    if (outState == nullptr) return kErrorInvalidOutAddress;
    std::memset(outState, 0, stateSize);
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceNgs2VoiceGetStateFlags(OrbisNgs2Handle, u32* outStateFlags) {
    Trace("sceNgs2VoiceGetStateFlags", "rEh728kXk3w");
    if (outStateFlags == nullptr) return kErrorInvalidOutAddress;
    *outStateFlags = 0;
    return 0;
}

}
