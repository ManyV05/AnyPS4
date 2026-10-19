#include <common/OrbisHle.hpp>
#include <windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
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
constexpr std::size_t kRingBuffers = 4;

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
    u32 outputChannels = 2;
    HWAVEOUT device = nullptr;
    HANDLE event = nullptr;
    std::array<WAVEHDR, kRingBuffers> headers{};
    std::array<std::vector<s16>, kRingBuffers> frames{};
    std::size_t next = 0;
    std::chrono::steady_clock::time_point virtualClock{};
    u64 outputs = 0;
    float peak = 0.0f;
    std::chrono::steady_clock::time_point lastReport{};
    std::array<s32, 8> volume{};
};

struct State {
    std::mutex lock;
    std::array<Port, kMaxPorts> ports{};
};

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

bool DescribeFormat(u32 format, u32& channels, bool& isFloat) {
    switch (format & 0xFF) {
    case 0: channels = 1; isFloat = false; return true;
    case 1: channels = 2; isFloat = false; return true;
    case 2: channels = 8; isFloat = false; return true;
    case 3: channels = 1; isFloat = true; return true;
    case 4: channels = 2; isFloat = true; return true;
    case 5: channels = 8; isFloat = true; return true;
    case 6: channels = 8; isFloat = false; return true;
    case 7: channels = 8; isFloat = true; return true;
    default: return false;
    }
}

bool OpenDevice(Port& port) {
    port.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = static_cast<WORD>(port.outputChannels);
    format.nSamplesPerSec = port.freq;
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(port.outputChannels * 2);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    const MMRESULT result = waveOutOpen(&port.device, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(port.event), 0, CALLBACK_EVENT);
    if (result != MMSYSERR_NOERROR) {
        char text[120];
        std::snprintf(text, sizeof text, "waveOutOpen failed (%u); the port becomes a paced sink", static_cast<unsigned>(result));
        Log(text);
        port.device = nullptr;
        return false;
    }
    for (std::size_t k = 0; k < kRingBuffers; ++k) {
        port.frames[k].assign(static_cast<std::size_t>(port.samples) * port.outputChannels, 0);
        WAVEHDR& header = port.headers[k];
        std::memset(&header, 0, sizeof header);
        header.lpData = reinterpret_cast<LPSTR>(port.frames[k].data());
        header.dwBufferLength = static_cast<DWORD>(port.frames[k].size() * sizeof(s16));
        waveOutPrepareHeader(port.device, &header, sizeof header);
        header.dwFlags |= WHDR_DONE;
    }
    return true;
}

void ConvertFrame(Port& port, const void* data, std::vector<s16>& out) {
    const u32 samples = port.samples;
    const u32 inChannels = port.sourceChannels;
    float peak = 0.0f;
    for (u32 sample = 0; sample < samples; ++sample) {
        float left = 0.0f;
        float right = 0.0f;
        if (port.isFloat) {
            const auto* source = static_cast<const float*>(data) + static_cast<std::size_t>(sample) * inChannels;
            if (inChannels == 1) {
                left = right = source[0];
            } else if (inChannels == 2) {
                left = source[0];
                right = source[1];
            } else {
                left = source[0] + 0.7f * source[2] + 0.7f * source[4] + 0.7f * source[6];
                right = source[1] + 0.7f * source[2] + 0.7f * source[5] + 0.7f * source[7];
            }
        } else {
            const auto* source = static_cast<const s16*>(data) + static_cast<std::size_t>(sample) * inChannels;
            if (inChannels == 1) {
                left = right = source[0] / 32768.0f;
            } else if (inChannels == 2) {
                left = source[0] / 32768.0f;
                right = source[1] / 32768.0f;
            } else {
                left = (source[0] + 0.7f * source[2] + 0.7f * source[4] + 0.7f * source[6]) / 32768.0f;
                right = (source[1] + 0.7f * source[2] + 0.7f * source[5] + 0.7f * source[7]) / 32768.0f;
            }
        }
        peak = std::max(peak, std::max(std::fabs(left), std::fabs(right)));
        out[static_cast<std::size_t>(sample) * 2] = static_cast<s16>(std::clamp(left, -1.0f, 1.0f) * 32767.0f);
        out[static_cast<std::size_t>(sample) * 2 + 1] = static_cast<s16>(std::clamp(right, -1.0f, 1.0f) * 32767.0f);
    }
    port.peak = std::max(port.peak, peak);
}

void Report(Port& port, s32 handle) {
    const auto now = std::chrono::steady_clock::now();
    if (port.lastReport.time_since_epoch().count() != 0 && now - port.lastReport < std::chrono::seconds(10)) return;
    port.lastReport = now;
    char text[200];
    std::snprintf(text, sizeof text, "port %d (type %d): %u Hz, %u ch %s -> %s, %llu outputs of %u samples, peak %.3f over the last interval", handle, port.type,
                  port.freq, port.sourceChannels, port.isFloat ? "float" : "s16", port.device != nullptr ? "waveOut stereo s16" : "paced sink",
                  static_cast<unsigned long long>(port.outputs), port.samples, port.peak);
    Log(text);
    port.peak = 0.0f;
}

void PaceVirtual(Port& port) {
    const auto period = std::chrono::microseconds(static_cast<long long>(port.samples) * 1000000LL / std::max(1u, port.freq));
    const auto now = std::chrono::steady_clock::now();
    if (port.virtualClock.time_since_epoch().count() == 0 || port.virtualClock < now - period * 4) port.virtualClock = now;
    port.virtualClock += period;
    std::this_thread::sleep_until(port.virtualClock);
}

s32 Output(s32 handle, const void* data) {
    auto& state = Global();
    std::unique_lock guard(state.lock);
    Port* port = GetPort(handle);
    if (port == nullptr) return kErrorInvalidPort;
    if (port->device == nullptr) {
        guard.unlock();
        PaceVirtual(*port);
        guard.lock();
        if (data != nullptr) {
            ++port->outputs;
            Report(*port, handle);
        }
        return static_cast<s32>(port->samples);
    }
    if (data == nullptr) {
        for (auto& header : port->headers) {
            while (!(header.dwFlags & WHDR_DONE)) {
                guard.unlock();
                WaitForSingleObject(port->event, 20);
                guard.lock();
            }
        }
        return static_cast<s32>(port->samples);
    }
    WAVEHDR& header = port->headers[port->next];
    while (!(header.dwFlags & WHDR_DONE)) {
        guard.unlock();
        WaitForSingleObject(port->event, 20);
        guard.lock();
    }
    ConvertFrame(*port, data, port->frames[port->next]);
    header.dwFlags &= ~WHDR_DONE;
    waveOutWrite(port->device, &header, sizeof header);
    port->next = (port->next + 1) % kRingBuffers;
    ++port->outputs;
    Report(*port, handle);
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
    if (!DescribeFormat(format, channels, isFloat)) return kErrorInvalidFormat;
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
        port.volume.fill(32768);
        const bool real = type == kPortTypeMain || type == kPortTypeBgm;
        const bool device = real && OpenDevice(port);
        const s32 handle = static_cast<s32>(k + 1);
        char text[200];
        std::snprintf(text, sizeof text, "sceAudioOutOpen(user %d, type %d, index %d, %u samples, %u Hz, format %#x) -> handle %d (%s)", userId, type, index,
                      samples, freq, format, handle, device ? "waveOut device" : "paced sink");
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
    if (port->device != nullptr) {
        waveOutReset(port->device);
        for (auto& header : port->headers) waveOutUnprepareHeader(port->device, &header, sizeof header);
        waveOutClose(port->device);
    }
    if (port->event != nullptr) CloseHandle(port->event);
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
