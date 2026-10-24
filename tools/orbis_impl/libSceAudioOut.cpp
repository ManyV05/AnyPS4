#include <common/OrbisHle.hpp>
#include <windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s16 = std::int16_t;
using s32 = std::int32_t;

constexpr s32 kErrorNotOpened = static_cast<s32>(0x80260001);
constexpr s32 kErrorBusy = static_cast<s32>(0x80260002);
constexpr s32 kErrorInvalidPort = static_cast<s32>(0x80260003);
constexpr s32 kErrorInvalidPointer = static_cast<s32>(0x80260004);
constexpr s32 kErrorInvalidSize = static_cast<s32>(0x80260006);
constexpr s32 kErrorInvalidFormat = static_cast<s32>(0x80260007);
constexpr s32 kErrorInvalidSampleFreq = static_cast<s32>(0x80260008);
constexpr s32 kErrorPortFull = static_cast<s32>(0x8026000A);
constexpr s32 kPortTypeMain = 0;
constexpr s32 kPortTypeBgm = 1;
constexpr s32 kPortTypePadSpk = 4;
constexpr std::size_t kMaxPorts = 16;
constexpr u32 kMixFrequency = 48000;
constexpr u32 kMixFrames = 960;
constexpr std::size_t kRingBuffers = 12;
constexpr u32 kQueueAheadChunks = 8;
constexpr u32 kStarvedChunks = 1;
constexpr float kFrontGain = 0.5f;
constexpr float kCenterGain = 0.70710678f;
constexpr float kLfeGain = 0.15f;
constexpr float kBackNearGain = 0.25f;
constexpr float kBackFarGain = 0.15f;
constexpr float kSideNearGain = 0.27f;
constexpr float kSideFarGain = 0.12f;

float EnvGain(const char* name, float fallback) {
    const char* value = std::getenv(name);
    return value != nullptr ? static_cast<float>(std::atof(value)) : fallback;
}

const float kBgmGain = EnvGain("ORBIS_AUDIO_BGM_GAIN", 0.7f);
const float kMainGain = EnvGain("ORBIS_AUDIO_MAIN_GAIN", 1.0f);

struct OutputParam {
    s32 handle;
    u32 reserved;
    void* data;
};

struct PortState {
    u16 output;
    u8 channel;
    u8 reserved8;
    s16 volume;
    u16 rerouteCounter;
    u64 flag;
    u64 reserved64[2];
};

struct Port {
    bool used = false;
    s32 type = 0;
    u32 samples = 0;
    u32 freq = 0;
    u32 format = 0;
    u32 sourceChannels = 0;
    bool isFloat = false;
    bool standardOrder = false;
    bool mixed = false;
    std::vector<float> queue;
    double resamplePosition = 0.0;
    std::array<s32, 8> volume{};
    std::chrono::steady_clock::time_point virtualClock{};
    u64 outputs = 0;
    u64 underruns = 0;
    u64 waits = 0;
    u64 paced = 0;
    u64 resyncs = 0;
    float peak = 0.0f;
    std::chrono::steady_clock::time_point lastReport{};
    std::chrono::steady_clock::time_point nextSlot{};
    bool slotValid = false;
    FILE* dump = nullptr;
    u64 dumpFrames = 0;
};

struct Mixer {
    HWAVEOUT device = nullptr;
    HANDLE event = nullptr;
    std::array<WAVEHDR, kRingBuffers> headers{};
    std::array<std::vector<s16>, kRingBuffers> buffers{};
    std::size_t next = 0;
    bool running = false;
    bool failed = false;
    u64 blocks = 0;
    u64 clipped = 0;
    float peak = 0.0f;
    FILE* dump = nullptr;
    u64 dumpFrames = 0;
};

struct Dump {
    const char* directory = std::getenv("ORBIS_AUDIO_DUMP");
    double startSeconds = std::getenv("ORBIS_AUDIO_DUMP_START") != nullptr ? std::atof(std::getenv("ORBIS_AUDIO_DUMP_START")) : 0.0;
    double lengthSeconds = std::getenv("ORBIS_AUDIO_DUMP_SECONDS") != nullptr ? std::atof(std::getenv("ORBIS_AUDIO_DUMP_SECONDS")) : 20.0;
    std::chrono::steady_clock::time_point firstOutput{};
    bool Active() const {
        if (directory == nullptr || firstOutput.time_since_epoch().count() == 0) return false;
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - firstOutput).count();
        return elapsed >= startSeconds;
    }
};

struct State {
    std::mutex lock;
    std::condition_variable drained;
    std::array<Port, kMaxPorts> ports{};
    Mixer mixer;
    Dump dump;
};

void WriteWavHeader(FILE* file, u32 freq, u32 channels, bool isFloat, u64 dataBytes) {
    const u16 tag = isFloat ? 3 : 1;
    const u16 bits = isFloat ? 32 : 16;
    const u32 byteRate = freq * channels * (bits / 8u);
    const u16 align = static_cast<u16>(channels * (bits / 8u));
    u8 h[44];
    auto w32 = [&](int o, u32 v) { std::memcpy(h + o, &v, 4); };
    auto w16 = [&](int o, u16 v) { std::memcpy(h + o, &v, 2); };
    std::memcpy(h, "RIFF", 4);
    w32(4, static_cast<u32>(36 + dataBytes));
    std::memcpy(h + 8, "WAVEfmt ", 8);
    w32(16, 16);
    w16(20, tag);
    w16(22, static_cast<u16>(channels));
    w32(24, freq);
    w32(28, byteRate);
    w16(32, align);
    w16(34, bits);
    std::memcpy(h + 36, "data", 4);
    w32(40, static_cast<u32>(dataBytes));
    std::fseek(file, 0, SEEK_SET);
    std::fwrite(h, 1, sizeof h, file);
}

FILE* OpenDump(const Dump& dump, const char* name, u32 freq, u32 channels, bool isFloat) {
    char path[512];
    std::snprintf(path, sizeof path, "%s/%s", dump.directory, name);
    FILE* file = std::fopen(path, "wb");
    char text[600];
    std::snprintf(text, sizeof text, "%s %s (%u Hz, %u ch, %s)",
                  file != nullptr ? "dumping to" : "cannot open", path, freq, channels,
                  isFloat ? "float32" : "int16");
    std::fprintf(stderr, "[AudioOut] %s\n", text);
    std::fflush(stderr);
    if (file != nullptr) {
        WriteWavHeader(file, freq, channels, isFloat, 0);
    }
    return file;
}

void DumpPortChunk(State& state, Port& port, s32 handle, const void* data) {
    if (!state.dump.Active()) return;
    const u64 cap = static_cast<u64>(state.dump.lengthSeconds * port.freq);
    const u64 sampleBytes = port.isFloat ? 4 : 2;
    if (port.dumpFrames >= cap) {
        if (port.dump != nullptr) {
            WriteWavHeader(port.dump, port.freq, port.sourceChannels, port.isFloat,
                           port.dumpFrames * port.sourceChannels * sampleBytes);
            std::fclose(port.dump);
            port.dump = nullptr;
        }
        return;
    }
    if (port.dump == nullptr) {
        char name[96];
        std::snprintf(name, sizeof name, "port%d-type%d-%uch-%s-%uhz.wav", handle, port.type, port.sourceChannels, port.isFloat ? "f32" : "s16", port.freq);
        port.dump = OpenDump(state.dump, name, port.freq, port.sourceChannels, port.isFloat);
        if (port.dump == nullptr) {
            port.dumpFrames = cap;
            return;
        }
    }
    std::fseek(port.dump, 0, SEEK_END);
    std::fwrite(data, sampleBytes, static_cast<std::size_t>(port.samples) * port.sourceChannels, port.dump);
    port.dumpFrames += port.samples;
    if ((port.dumpFrames / port.samples) % 64 == 0) {
        WriteWavHeader(port.dump, port.freq, port.sourceChannels, port.isFloat,
                       port.dumpFrames * port.sourceChannels * sampleBytes);
    }
}

void DumpMixBlock(State& state, const std::vector<s16>& buffer) {
    Mixer& mixer = state.mixer;
    if (!state.dump.Active()) return;
    const u64 cap = static_cast<u64>(state.dump.lengthSeconds * kMixFrequency);
    if (mixer.dumpFrames >= cap) {
        if (mixer.dump != nullptr) {
            WriteWavHeader(mixer.dump, kMixFrequency, 2, false, mixer.dumpFrames * 2 * sizeof(s16));
            std::fclose(mixer.dump);
            mixer.dump = nullptr;
        }
        return;
    }
    if (mixer.dump == nullptr) {
        mixer.dump = OpenDump(state.dump, "mix-2ch-s16-48000hz.wav", kMixFrequency, 2, false);
        if (mixer.dump == nullptr) {
            mixer.dumpFrames = cap;
            return;
        }
    }
    std::fseek(mixer.dump, 0, SEEK_END);
    std::fwrite(buffer.data(), sizeof(s16), buffer.size(), mixer.dump);
    mixer.dumpFrames += kMixFrames;
    if ((mixer.dumpFrames / kMixFrames) % 64 == 0) {
        WriteWavHeader(mixer.dump, kMixFrequency, 2, false, mixer.dumpFrames * 2 * sizeof(s16));
    }
}

State& Global() {
    static auto* state = new State();
    return *state;
}

void Trace(const char* function, const char* nid) {
    static std::uint64_t calls = 0;
    OrbisHleTrace("libSceAudioOut.prx", function, nid, ++calls, __builtin_return_address(0));
}

void Log(const char* text) {
    std::fprintf(stderr, "[AudioOut] %s\n", text);
    std::fflush(stderr);
}

Port* GetPort(s32 handle) {
    if (handle < 1 || static_cast<std::size_t>(handle) > kMaxPorts) return nullptr;
    Port& port = Global().ports[static_cast<std::size_t>(handle - 1)];
    return port.used ? &port : nullptr;
}

bool DescribeFormat(u32 format, u32& channels, bool& isFloat, bool& standardOrder) {
    standardOrder = false;
    switch (format & 0xFF) {
    case 0: channels = 1; isFloat = false; return true;
    case 1: channels = 2; isFloat = false; return true;
    case 2: channels = 8; isFloat = false; return true;
    case 3: channels = 1; isFloat = true; return true;
    case 4: channels = 2; isFloat = true; return true;
    case 5: channels = 8; isFloat = true; return true;
    case 6: channels = 8; isFloat = false; standardOrder = true; return true;
    case 7: channels = 8; isFloat = true; standardOrder = true; return true;
    default: return false;
    }
}

void MixerThread();

void PreciseSleepUntil(std::chrono::steady_clock::time_point slot) {
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= slot) return;
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(slot - now);
        if (remaining > std::chrono::milliseconds(2)) {
            Sleep(static_cast<DWORD>(remaining.count() / 1000 - 1));
        } else {
            SwitchToThread();
        }
    }
}

bool StartMixer(State& state) {
    Mixer& mixer = state.mixer;
    static const bool timerResolution = timeBeginPeriod(1) == TIMERR_NOERROR;
    (void)timerResolution;
    if (mixer.running) return true;
    if (mixer.failed) return false;
    mixer.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = kMixFrequency;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    const MMRESULT result = waveOutOpen(&mixer.device, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(mixer.event), 0, CALLBACK_EVENT);
    if (result != MMSYSERR_NOERROR) {
        char text[160];
        std::snprintf(text, sizeof text, "waveOutOpen failed (%u); audio ports become paced sinks", static_cast<unsigned>(result));
        Log(text);
        mixer.device = nullptr;
        mixer.failed = true;
        return false;
    }
    for (std::size_t k = 0; k < kRingBuffers; ++k) {
        mixer.buffers[k].assign(static_cast<std::size_t>(kMixFrames) * 2, 0);
        WAVEHDR& header = mixer.headers[k];
        std::memset(&header, 0, sizeof header);
        header.lpData = reinterpret_cast<LPSTR>(mixer.buffers[k].data());
        header.dwBufferLength = static_cast<DWORD>(mixer.buffers[k].size() * sizeof(s16));
        waveOutPrepareHeader(mixer.device, &header, sizeof header);
        header.dwFlags |= WHDR_DONE;
    }
    mixer.running = true;
    std::thread(MixerThread).detach();
    char text[200];
    std::snprintf(text, sizeof text, "mixer started: waveOut stereo s16 at %u Hz, %zu buffers of %u frames (%u ms), ports queue up to %u chunks ahead",
                  kMixFrequency, kRingBuffers, kMixFrames, kMixFrames * 1000 / kMixFrequency, kQueueAheadChunks);
    Log(text);
    return true;
}

void ConvertChunk(Port& port, const void* data) {
    const u32 samples = port.samples;
    const u32 inChannels = port.sourceChannels;
    float peak = 0.0f;
    const std::size_t base = port.queue.size();
    port.queue.resize(base + static_cast<std::size_t>(samples) * 2);
    for (u32 sample = 0; sample < samples; ++sample) {
        float channels[8]{};
        if (port.isFloat) {
            const auto* source = static_cast<const float*>(data) + static_cast<std::size_t>(sample) * inChannels;
            for (u32 c = 0; c < inChannels; ++c) channels[c] = source[c];
        } else {
            const auto* source = static_cast<const s16*>(data) + static_cast<std::size_t>(sample) * inChannels;
            for (u32 c = 0; c < inChannels; ++c) channels[c] = source[c] / 32768.0f;
        }
        float left = 0.0f;
        float right = 0.0f;
        if (inChannels == 1) {
            left = right = channels[0];
        } else if (inChannels == 2) {
            left = channels[0];
            right = channels[1];
        } else {
            left = kFrontGain * channels[0] + kCenterGain * channels[2] + kLfeGain * channels[3] + kBackNearGain * channels[4] + kBackFarGain * channels[5] +
                   kSideNearGain * channels[6] + kSideFarGain * channels[7];
            right = kFrontGain * channels[1] + kCenterGain * channels[2] + kLfeGain * channels[3] + kBackFarGain * channels[4] + kBackNearGain * channels[5] +
                    kSideFarGain * channels[6] + kSideNearGain * channels[7];
        }
        peak = std::max(peak, std::max(std::fabs(left), std::fabs(right)));
        port.queue[base + static_cast<std::size_t>(sample) * 2] = left;
        port.queue[base + static_cast<std::size_t>(sample) * 2 + 1] = right;
    }
    port.peak = std::max(port.peak, peak);
}

void MixBlock(State& state, std::vector<float>& mix) {
    std::fill(mix.begin(), mix.end(), 0.0f);
    for (Port& port : state.ports) {
        if (!port.used || !port.mixed) continue;
        const float typeGain = port.type == kPortTypeBgm ? kBgmGain : kMainGain;
        const float gainLeft = typeGain * std::clamp(port.volume[0], 0, 32768) / 32768.0f;
        const float gainRight = typeGain * std::clamp(port.volume[1], 0, 32768) / 32768.0f;
        const double ratio = static_cast<double>(port.freq) / kMixFrequency;
        const bool nativeRate = port.freq == kMixFrequency;
        const std::size_t available = port.queue.size() / 2;
        double position = nativeRate ? 0.0 : port.resamplePosition;
        u32 produced = 0;
        for (; produced < kMixFrames; ++produced) {
            const auto index = static_cast<std::size_t>(position);
            if (index >= available) break;
            float left = port.queue[index * 2];
            float right = port.queue[index * 2 + 1];
            if (!nativeRate) {
                const std::size_t next = std::min(index + 1, available - 1);
                const float t = static_cast<float>(position - static_cast<double>(index));
                left = left * (1.0f - t) + port.queue[next * 2] * t;
                right = right * (1.0f - t) + port.queue[next * 2 + 1] * t;
            }
            mix[static_cast<std::size_t>(produced) * 2] += left * gainLeft;
            mix[static_cast<std::size_t>(produced) * 2 + 1] += right * gainRight;
            position += nativeRate ? 1.0 : ratio;
        }
        if (produced < kMixFrames && port.outputs > 0) ++port.underruns;
        const auto consumed = std::min(static_cast<std::size_t>(position), available);
        port.queue.erase(port.queue.begin(), port.queue.begin() + static_cast<std::ptrdiff_t>(consumed * 2));
        port.resamplePosition = position - static_cast<double>(consumed);
    }
}

void MixerThread() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    State& state = Global();
    Mixer& mixer = state.mixer;
    std::vector<float> mix(static_cast<std::size_t>(kMixFrames) * 2, 0.0f);
    for (;;) {
        WAVEHDR& header = mixer.headers[mixer.next];
        while (!(header.dwFlags & WHDR_DONE)) WaitForSingleObject(mixer.event, 20);
        {
            std::lock_guard guard(state.lock);
            MixBlock(state, mix);
        }
        state.drained.notify_all();
        std::vector<s16>& buffer = mixer.buffers[mixer.next];
        float peak = 0.0f;
        u64 clipped = 0;
        for (std::size_t k = 0; k < mix.size(); ++k) {
            const float value = mix[k];
            peak = std::max(peak, std::fabs(value));
            if (value > 1.0f || value < -1.0f) ++clipped;
            buffer[k] = static_cast<s16>(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
        }
        header.dwFlags &= ~WHDR_DONE;
        waveOutWrite(mixer.device, &header, sizeof header);
        mixer.next = (mixer.next + 1) % kRingBuffers;
        {
            std::lock_guard guard(state.lock);
            ++mixer.blocks;
            mixer.clipped += clipped;
            mixer.peak = std::max(mixer.peak, peak);
            DumpMixBlock(state, buffer);
        }
    }
}

void Report(State& state, Port& port, s32 handle) {
    const auto now = std::chrono::steady_clock::now();
    if (port.lastReport.time_since_epoch().count() != 0 && now - port.lastReport < std::chrono::seconds(10)) return;
    port.lastReport = now;
    char text[320];
    std::snprintf(text, sizeof text,
                  "port %d (type %d): %u Hz, %u ch %s%s -> %s, %llu outputs of %u samples, %llu paced, %llu resyncs, %llu waits, %llu underruns, queue %zu frames, gain %.2f/%.2f x%.2f, peak %.3f; mixer %llu blocks, %llu clipped samples, peak %.3f",
                  handle, port.type, port.freq, port.sourceChannels, port.isFloat ? "float" : "s16", port.standardOrder ? " std" : "",
                  port.mixed ? "mixer" : "paced sink", static_cast<unsigned long long>(port.outputs), port.samples,
                  static_cast<unsigned long long>(port.paced), static_cast<unsigned long long>(port.resyncs),
                  static_cast<unsigned long long>(port.waits), static_cast<unsigned long long>(port.underruns), port.queue.size() / 2,
                  port.volume[0] / 32768.0f, port.volume[1] / 32768.0f, port.type == kPortTypeBgm ? kBgmGain : kMainGain, port.peak,
                  static_cast<unsigned long long>(state.mixer.blocks), static_cast<unsigned long long>(state.mixer.clipped), state.mixer.peak);
    Log(text);
    port.peak = 0.0f;
    state.mixer.peak = 0.0f;
}

void PaceVirtual(Port& port) {
    const auto period = std::chrono::microseconds(static_cast<long long>(port.samples) * 1000000LL / std::max(1u, port.freq));
    const auto now = std::chrono::steady_clock::now();
    if (port.virtualClock.time_since_epoch().count() == 0 || port.virtualClock < now - period * 4) port.virtualClock = now;
    port.virtualClock += period;
    PreciseSleepUntil(port.virtualClock);
}

s32 Output(s32 handle, const void* data) {
    auto& state = Global();
    std::unique_lock guard(state.lock);
    Port* port = GetPort(handle);
    if (port == nullptr) return kErrorInvalidPort;
    if (!port->mixed) {
        guard.unlock();
        PaceVirtual(*port);
        guard.lock();
        if (data != nullptr) {
            ++port->outputs;
            Report(state, *port, handle);
        }
        return static_cast<s32>(port->samples);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    if (data == nullptr) {
        while (!port->queue.empty() && std::chrono::steady_clock::now() < deadline) state.drained.wait_for(guard, std::chrono::milliseconds(20));
        return static_cast<s32>(port->samples);
    }
    const auto period = std::chrono::microseconds(static_cast<long long>(port->samples) * 1000000LL / std::max(1u, port->freq));
    const auto now = std::chrono::steady_clock::now();
    if (!port->slotValid || now > port->nextSlot + period * 4 || port->queue.size() < static_cast<std::size_t>(port->samples) * kStarvedChunks * 2) {
        if (port->slotValid) ++port->resyncs;
        port->nextSlot = now;
        port->slotValid = true;
    } else if (now < port->nextSlot) {
        const auto slot = port->nextSlot;
        guard.unlock();
        PreciseSleepUntil(slot);
        guard.lock();
        port = GetPort(handle);
        if (port == nullptr) return kErrorInvalidPort;
        ++port->paced;
    }
    port->nextSlot += period;
    const std::size_t limit = static_cast<std::size_t>(port->samples) * kQueueAheadChunks * 2;
    bool waited = false;
    while (port->queue.size() >= limit && std::chrono::steady_clock::now() < deadline) {
        waited = true;
        state.drained.wait_for(guard, std::chrono::milliseconds(20));
    }
    if (waited) ++port->waits;
    if (port->queue.size() >= limit) port->queue.erase(port->queue.begin(), port->queue.begin() + static_cast<std::ptrdiff_t>(port->samples * 2));
    if (state.dump.firstOutput.time_since_epoch().count() == 0) state.dump.firstOutput = std::chrono::steady_clock::now();
    DumpPortChunk(state, *port, handle, data);
    ConvertChunk(*port, data);
    ++port->outputs;
    Report(state, *port, handle);
    return static_cast<s32>(port->samples);
}

}  // namespace

extern "C" {

ORBIS_SYSV_ABI s32 orbis_sceAudioOutInit() {
    Trace("sceAudioOutInit", "JfEPXVxhFqA");
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceAudioOutOpen(s32 userId, s32 type, s32 index, u32 samples, u32 freq, u32 format) {
    Trace("sceAudioOutOpen", "ekNvsT22rsY");
    if (type < 0 || (type > 4 && type != 126 && type != 127)) return kErrorInvalidPort;
    if (samples == 0 || samples > 2048) return kErrorInvalidSize;
    if (freq != 48000 && freq != 44100 && freq != 32000 && freq != 24000 && freq != 16000 && freq != 11025 && freq != 8000) return kErrorInvalidSampleFreq;
    u32 channels = 0;
    bool isFloat = false;
    bool standardOrder = false;
    if (!DescribeFormat(format, channels, isFloat, standardOrder)) return kErrorInvalidFormat;
    auto& state = Global();
    std::lock_guard guard(state.lock);
    for (std::size_t k = 0; k < kMaxPorts; ++k) {
        Port& port = state.ports[k];
        if (port.used) continue;
        port = Port{};
        port.used = true;
        port.type = type;
        port.samples = samples;
        port.freq = freq;
        port.format = format;
        port.sourceChannels = channels;
        port.isFloat = isFloat;
        port.standardOrder = standardOrder;
        port.volume.fill(32768);
        port.queue.reserve(static_cast<std::size_t>(samples) * (kQueueAheadChunks + 1) * 2);
        const bool real = type == kPortTypeMain || type == kPortTypeBgm;
        port.mixed = real && StartMixer(state);
        const s32 handle = static_cast<s32>(k + 1);
        char text[200];
        std::snprintf(text, sizeof text, "sceAudioOutOpen(user %d, type %d, index %d, %u samples, %u Hz, format %#x) -> handle %d (%s)", userId, type, index,
                      samples, freq, format, handle, port.mixed ? "mixed into waveOut" : "paced sink");
        Log(text);
        return handle;
    }
    return kErrorPortFull;
}

ORBIS_SYSV_ABI s32 orbis_sceAudioOutClose(s32 handle) {
    Trace("sceAudioOutClose", "s1--uE9mBFw");
    auto& state = Global();
    std::lock_guard guard(state.lock);
    Port* port = GetPort(handle);
    if (port == nullptr) return kErrorInvalidPort;
    *port = Port{};
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceAudioOutOutput(s32 handle, const void* data) {
    Trace("sceAudioOutOutput", "QOQtbeDqsT4");
    return Output(handle, data);
}

ORBIS_SYSV_ABI s32 orbis_sceAudioOutOutputs(const OutputParam* params, u32 count) {
    Trace("sceAudioOutOutputs", "w3PdaSTSwGE");
    if (params == nullptr) return kErrorInvalidPointer;
    if (count == 0) return kErrorInvalidSize;
    s32 result = 0;
    for (u32 k = 0; k < count; ++k) {
        const s32 value = Output(params[k].handle, params[k].data);
        if (value < 0) return value;
        if (k == 0) result = value;
    }
    return result;
}

ORBIS_SYSV_ABI s32 orbis_sceAudioOutSetVolume(s32 handle, s32 flag, const s32* volume) {
    Trace("sceAudioOutSetVolume", "b+uAV89IlxE");
    if (volume == nullptr) return kErrorInvalidPointer;
    auto& state = Global();
    std::lock_guard guard(state.lock);
    Port* port = GetPort(handle);
    if (port == nullptr) return kErrorInvalidPort;
    for (u32 channel = 0; channel < 8; ++channel) {
        if (flag & (1 << channel)) port->volume[channel] = volume[channel];
    }
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceAudioOutGetPortState(s32 handle, PortState* status) {
    Trace("sceAudioOutGetPortState", "GrQ9s4IrNaQ");
    if (status == nullptr) return kErrorInvalidPointer;
    auto& state = Global();
    std::lock_guard guard(state.lock);
    Port* port = GetPort(handle);
    if (port == nullptr) return kErrorInvalidPort;
    std::memset(status, 0, sizeof *status);
    status->output = 1;
    status->channel = static_cast<u8>(port->sourceChannels == 1 ? 1 : 2);
    status->volume = 127;
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceAudioOutSetMixLevelPadSpk(s32 handle, s32) {
    Trace("sceAudioOutSetMixLevelPadSpk", "wVwPU50pS1c");
    auto& state = Global();
    std::lock_guard guard(state.lock);
    return GetPort(handle) != nullptr ? 0 : kErrorInvalidPort;
}

}
