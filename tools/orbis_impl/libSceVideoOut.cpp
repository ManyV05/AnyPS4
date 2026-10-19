#include <common/OrbisHle.hpp>
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

constexpr s32 kHandle = 1;
constexpr s32 kErrorInvalidHandle = static_cast<s32>(0x80290001);
constexpr s32 kErrorInvalidAddress = static_cast<s32>(0x80290002);
constexpr s32 kErrorInvalidIndex = static_cast<s32>(0x80290004);
constexpr s32 kErrorInvalidValue = static_cast<s32>(0x80290006);
constexpr s32 kErrorResourceBusy = static_cast<s32>(0x80290010);
constexpr u32 kPixelFormatArgb = 0x80000000u;
constexpr u32 kPixelFormatAbgr = 0x80002200u;
constexpr std::size_t kMaxBuffers = 16;
constexpr std::size_t kMaxGroups = 4;

struct BufferAttribute {
    u32 pixelFormat;
    s32 tilingMode;
    s32 aspectRatio;
    u32 width;
    u32 height;
    u32 pitchInPixel;
    u32 option;
    u32 reserved0;
    u64 reserved1;
};

struct FlipStatus {
    u64 count;
    u64 processTime;
    u64 tsc;
    s64 flipArg;
    u64 submitTsc;
    u64 reserved0;
    s32 gcQueueNum;
    s32 flipPendingNum;
    s32 currentBuffer;
    u32 reserved1;
};

struct ResolutionStatus {
    s32 fullWidth;
    s32 fullHeight;
    s32 paneWidth;
    s32 paneHeight;
    u64 refreshRate;
    float screenSizeInInch;
    u16 flags;
    u16 reserved0;
    u32 reserved1[3];
};

struct VblankStatus {
    u64 count;
    u64 processTime;
    u64 tsc;
    u64 reserved;
    u8 flags;
    u8 pad[7];
};

struct Buffer {
    const void* address = nullptr;
    s32 group = -1;
};

struct State {
    std::mutex lock;
    bool open = false;
    std::array<Buffer, kMaxBuffers> buffers{};
    std::array<BufferAttribute, kMaxGroups> groups{};
    std::array<bool, kMaxGroups> groupUsed{};
    FlipStatus flip{};
    u64 vblanks = 0;
    std::atomic<s32> pending{0};
    u32 flipRate = 0;
    std::chrono::steady_clock::time_point lastPresent{};
    std::chrono::steady_clock::time_point fpsWindow{};
    u32 fpsFrames = 0;
    double fps = 0.0;
    HWND window = nullptr;
    bool windowStarted = false;
    u32 windowWidth = 0;
    u32 windowHeight = 0;
    std::vector<u32> staging;
    u32 stagingWidth = 0;
    u32 stagingHeight = 0;
    bool stagingValid = false;
    bool warnedTiled = false;
    bool directPresentation = false;
};

State& Global() {
    static auto* state = new State();
    return *state;
}

void Trace(const char* function, const char* nid) {
    static std::uint64_t calls = 0;
    OrbisHleTrace("libSceVideoOut.prx", function, nid, ++calls, __builtin_return_address(0));
}

void Log(const char* text) {
    std::fprintf(stderr, "[VideoOut] %s\n", text);
    std::fflush(stderr);
}

u64 ProcessTimeMicroseconds() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void PaintLast(HWND window) {
    auto& state = Global();
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    std::lock_guard guard(state.lock);
    if (state.directPresentation) {
    } else if (state.stagingValid) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(state.stagingWidth);
        info.bmiHeader.biHeight = -static_cast<LONG>(state.stagingHeight);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, HALFTONE);
        StretchDIBits(dc, 0, 0, client.right, client.bottom, 0, 0, static_cast<int>(state.stagingWidth), static_cast<int>(state.stagingHeight),
                      state.staging.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    } else {
        FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    }
    EndPaint(window, &paint);
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT:
        PaintLast(window);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        Log("window closed by the user; exiting");
        ExitProcess(0);
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

std::wstring TitleBase() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring name(path);
    const auto slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) name = name.substr(slash + 1);
    return L"AnyPS4 - " + name;
}

void WindowThread(u32 width, u32 height) {
    auto& state = Global();
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.lpszClassName = L"AnyPS4VideoOut";
    RegisterClassW(&windowClass);
    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    double scale = 1.0;
    while (width * scale > screenWidth * 0.9 || height * scale > screenHeight * 0.85) scale *= 0.5;
    RECT rect{0, 0, static_cast<LONG>(width * scale), static_cast<LONG>(height * scale)};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowExW(0, windowClass.lpszClassName, TitleBase().c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                  rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, windowClass.hInstance, nullptr);
    {
        std::lock_guard guard(state.lock);
        state.window = window;
        state.windowWidth = width;
        state.windowHeight = height;
    }
    char text[160];
    std::snprintf(text, sizeof text, "window created: %ux%u shown at %.0f%% (hwnd %p)", width, height, scale * 100.0, static_cast<void*>(window));
    Log(text);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void EnsureWindow(u32 width, u32 height) {
    auto& state = Global();
    std::lock_guard guard(state.lock);
    if (state.windowStarted) return;
    state.windowStarted = true;
    std::thread(WindowThread, width, height).detach();
}

void Convert(const Buffer& buffer, const BufferAttribute& attribute) {
    auto& state = Global();
    const u32 width = attribute.width;
    const u32 height = attribute.height;
    const u32 pitch = attribute.pitchInPixel != 0 ? attribute.pitchInPixel : width;
    if (width == 0 || height == 0 || buffer.address == nullptr) return;
    if (state.staging.size() != static_cast<std::size_t>(width) * height) state.staging.assign(static_cast<std::size_t>(width) * height, 0);
    state.stagingWidth = width;
    state.stagingHeight = height;
    const auto* source = static_cast<const u32*>(buffer.address);
    const bool swap = attribute.pixelFormat == kPixelFormatAbgr;
    for (u32 y = 0; y < height; ++y) {
        const u32* row = source + static_cast<std::size_t>(y) * pitch;
        u32* out = state.staging.data() + static_cast<std::size_t>(y) * width;
        if (!swap) {
            std::memcpy(out, row, static_cast<std::size_t>(width) * 4);
        } else {
            for (u32 x = 0; x < width; ++x) {
                const u32 pixel = row[x];
                out[x] = (pixel & 0xFF00FF00u) | ((pixel & 0xFFu) << 16) | ((pixel >> 16) & 0xFFu);
            }
        }
    }
    state.stagingValid = true;
    if (attribute.tilingMode == 0 && !state.warnedTiled) {
        state.warnedTiled = true;
        Log("display buffers are tiled; without the renderer they are shown as linear memory");
    }
}

void UpdateTitle() {
    auto& state = Global();
    const auto now = std::chrono::steady_clock::now();
    ++state.fpsFrames;
    if (state.fpsWindow.time_since_epoch().count() == 0) state.fpsWindow = now;
    const auto elapsed = std::chrono::duration<double>(now - state.fpsWindow).count();
    if (elapsed < 1.0) return;
    state.fps = state.fpsFrames / elapsed;
    state.fpsFrames = 0;
    state.fpsWindow = now;
    if (state.window == nullptr) return;
    wchar_t title[256];
    std::swprintf(title, 256, L"%ls | %ux%u | flips %llu | %.1f fps", TitleBase().c_str(), state.windowWidth, state.windowHeight,
                  static_cast<unsigned long long>(state.flip.count), state.fps);
    SetWindowTextW(state.window, title);
}

void Pace() {
    auto& state = Global();
    const u32 divisor = state.flipRate == 1 ? 2 : state.flipRate == 2 ? 3 : 1;
    const auto period = std::chrono::microseconds(16667 * divisor);
    const auto now = std::chrono::steady_clock::now();
    if (state.lastPresent.time_since_epoch().count() != 0) {
        const auto target = state.lastPresent + period;
        if (target > now) std::this_thread::sleep_until(target);
    }
    state.lastPresent = std::chrono::steady_clock::now();
}

struct PixelSource {
    const void* pixels;
    u32 width;
    u32 height;
    u32 stride;
    bool swapRedBlue;
};

void CopyPixels(const PixelSource& source) {
    auto& state = Global();
    if (source.pixels == nullptr || source.width == 0 || source.height == 0) return;
    if (state.staging.size() != static_cast<std::size_t>(source.width) * source.height) state.staging.assign(static_cast<std::size_t>(source.width) * source.height, 0);
    state.stagingWidth = source.width;
    state.stagingHeight = source.height;
    for (u32 y = 0; y < source.height; ++y) {
        const auto* row = reinterpret_cast<const u32*>(static_cast<const u8*>(source.pixels) + static_cast<std::size_t>(y) * source.stride);
        u32* out = state.staging.data() + static_cast<std::size_t>(y) * source.width;
        if (!source.swapRedBlue) {
            std::memcpy(out, row, static_cast<std::size_t>(source.width) * 4);
        } else {
            for (u32 x = 0; x < source.width; ++x) {
                const u32 pixel = row[x];
                out[x] = (pixel & 0xFF00FF00u) | ((pixel & 0xFFu) << 16) | ((pixel >> 16) & 0xFFu);
            }
        }
    }
    state.stagingValid = true;
}

void Present(s32 bufferIndex, s64 flipArg, const PixelSource* source = nullptr) {
    auto& state = Global();
    std::unique_lock guard(state.lock);
    if (bufferIndex < 0 || static_cast<std::size_t>(bufferIndex) >= kMaxBuffers) return;
    const Buffer buffer = state.buffers[static_cast<std::size_t>(bufferIndex)];
    const BufferAttribute attribute = buffer.group >= 0 ? state.groups[static_cast<std::size_t>(buffer.group)] : BufferAttribute{};
    guard.unlock();
    Pace();
    guard.lock();
    if (source != nullptr) CopyPixels(*source); else if (!state.directPresentation) Convert(buffer, attribute);
    ++state.flip.count;
    state.flip.processTime = ProcessTimeMicroseconds();
    state.flip.tsc = __rdtsc();
    state.flip.flipArg = flipArg;
    state.flip.currentBuffer = bufferIndex;
    state.flip.flipPendingNum = std::max(0, state.pending.fetch_sub(1) - 1);
    ++state.vblanks;
    HWND window = state.window;
    UpdateTitle();
    if (window != nullptr && state.stagingValid && !state.directPresentation) {
        HDC dc = GetDC(window);
        if (dc != nullptr) {
            RECT client{};
            GetClientRect(window, &client);
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = static_cast<LONG>(state.stagingWidth);
            info.bmiHeader.biHeight = -static_cast<LONG>(state.stagingHeight);
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, HALFTONE);
            StretchDIBits(dc, 0, 0, client.right, client.bottom, 0, 0, static_cast<int>(state.stagingWidth), static_cast<int>(state.stagingHeight),
                          state.staging.data(), &info, DIB_RGB_COLORS, SRCCOPY);
            ReleaseDC(window, dc);
        }
    }
    if (state.flip.count == 1 || state.flip.count % 300 == 0) {
        char text[200];
        std::snprintf(text, sizeof text, "flip #%llu presented: buffer %d (%ux%u, format %#x, tiling %d), arg %lld, pending %d, %.1f fps",
                      static_cast<unsigned long long>(state.flip.count), bufferIndex, attribute.width, attribute.height, attribute.pixelFormat,
                      attribute.tilingMode, static_cast<long long>(flipArg), state.flip.flipPendingNum, state.fps);
        Log(text);
    }
}

}  // namespace

extern "C" {

__declspec(dllexport) void OrbisVideoOutPresent(s32 bufferIndex, s64 flipArg) {
    Present(bufferIndex, flipArg);
}

__declspec(dllexport) void* OrbisVideoOutWindowHandle() {
    auto& state = Global();
    for (int attempt = 0; attempt < 200; ++attempt) {
        {
            std::lock_guard guard(state.lock);
            if (state.window != nullptr) {
                state.directPresentation = true;
                Log("the GPU presents directly into the window; GDI presentation is off");
                return state.window;
            }
            if (!state.windowStarted) return nullptr;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return nullptr;
}

__declspec(dllexport) void OrbisVideoOutFlipPresented(s32 bufferIndex, s64 flipArg) {
    auto& state = Global();
    std::lock_guard guard(state.lock);
    ++state.flip.count;
    state.flip.processTime = ProcessTimeMicroseconds();
    state.flip.tsc = __rdtsc();
    state.flip.flipArg = flipArg;
    state.flip.currentBuffer = bufferIndex;
    state.flip.flipPendingNum = std::max(0, state.pending.fetch_sub(1) - 1);
    ++state.vblanks;
    state.lastPresent = std::chrono::steady_clock::now();
    UpdateTitle();
    if (state.flip.count == 1 || state.flip.count % 600 == 0) {
        char text[160];
        std::snprintf(text, sizeof text, "flip #%llu presented by the GPU: buffer %d, arg %lld, %.1f fps",
                      static_cast<unsigned long long>(state.flip.count), bufferIndex, static_cast<long long>(flipArg), state.fps);
        Log(text);
    }
}

__declspec(dllexport) void OrbisVideoOutPresentPixels(s32 bufferIndex, s64 flipArg, const void* pixels, u32 width, u32 height, u32 stride, s32 swapRedBlue) {
    const PixelSource source{pixels, width, height, stride, swapRedBlue != 0};
    Present(bufferIndex, flipArg, &source);
}

__declspec(dllexport) s32 OrbisVideoOutGetBuffer(s32 bufferIndex, const void** address, BufferAttribute* attribute) {
    auto& state = Global();
    std::lock_guard guard(state.lock);
    if (bufferIndex < 0 || static_cast<std::size_t>(bufferIndex) >= kMaxBuffers || address == nullptr || attribute == nullptr) return -1;
    const Buffer& buffer = state.buffers[static_cast<std::size_t>(bufferIndex)];
    if (buffer.address == nullptr || buffer.group < 0) return -1;
    *address = buffer.address;
    *attribute = state.groups[static_cast<std::size_t>(buffer.group)];
    return 0;
}

__declspec(dllexport) void OrbisVideoOutFlipRequested(s32, s64) {
    auto& state = Global();
    state.pending.fetch_add(1);
    state.flip.submitTsc = __rdtsc();
}

ORBIS_SYSV_ABI s32 orbis_sceVideoOutOpen(s32 userId, s32 busType, s32 index, const void*) {
    Trace("sceVideoOutOpen", "Up36PTk687E");
    auto& state = Global();
    {
        std::lock_guard guard(state.lock);
        if (state.open) return kErrorResourceBusy;
        state.open = true;
        state.flip = FlipStatus{};
        state.flip.flipArg = -1;
        state.flip.currentBuffer = -1;
    }
    char text[120];
    std::snprintf(text, sizeof text, "sceVideoOutOpen(user %d, bus %d, index %d) -> handle %d", userId, busType, index, kHandle);
    Log(text);
    EnsureWindow(1920, 1080);
    return kHandle;
}

ORBIS_SYSV_ABI s32 orbis_sceVideoOutClose(s32 handle) {
    Trace("sceVideoOutClose", "uquVH4-Du78");
    if (handle != kHandle) return kErrorInvalidHandle;
    auto& state = Global();
    std::lock_guard guard(state.lock);
    state.open = false;
    return 0;
}

ORBIS_SYSV_ABI void orbis_sceVideoOutSetBufferAttribute(BufferAttribute* attribute, u32 pixelFormat, u32 tilingMode, u32 aspectRatio, u32 width, u32 height, u32 pitchInPixel) {
    Trace("sceVideoOutSetBufferAttribute", "i6-sR91Wt-4");
    if (attribute == nullptr) return;
    std::memset(attribute, 0, sizeof *attribute);
    attribute->pixelFormat = pixelFormat;
    attribute->tilingMode = static_cast<s32>(tilingMode);
    attribute->aspectRatio = static_cast<s32>(aspectRatio);
    attribute->width = width;
    attribute->height = height;
    attribute->pitchInPixel = pitchInPixel;
}

ORBIS_SYSV_ABI s32 orbis_sceVideoOutRegisterBuffers(s32 handle, s32 startIndex, const void* const* addresses, s32 bufferNum, const BufferAttribute* attribute) {
    Trace("sceVideoOutRegisterBuffers", "w3BY+tAEiQY");
    if (handle != kHandle) return kErrorInvalidHandle;
    if (addresses == nullptr || attribute == nullptr) return kErrorInvalidAddress;
    if (startIndex < 0 || bufferNum <= 0 || static_cast<std::size_t>(startIndex + bufferNum) > kMaxBuffers) return kErrorInvalidIndex;
    auto& state = Global();
    s32 group = -1;
    {
        std::lock_guard guard(state.lock);
        for (std::size_t k = 0; k < kMaxGroups; ++k) {
            if (!state.groupUsed[k]) {
                group = static_cast<s32>(k);
                break;
            }
        }
        if (group < 0) return kErrorInvalidValue;
        state.groupUsed[static_cast<std::size_t>(group)] = true;
        state.groups[static_cast<std::size_t>(group)] = *attribute;
        for (s32 k = 0; k < bufferNum; ++k) {
            auto& buffer = state.buffers[static_cast<std::size_t>(startIndex + k)];
            buffer.address = addresses[k];
            buffer.group = group;
        }
    }
    char text[200];
    std::snprintf(text, sizeof text, "sceVideoOutRegisterBuffers: %d buffers from index %d, %ux%u pitch %u, format %#x, tiling %d, first at %p -> group %d",
                  bufferNum, startIndex, attribute->width, attribute->height, attribute->pitchInPixel, attribute->pixelFormat, attribute->tilingMode, addresses[0], group);
    Log(text);
    EnsureWindow(attribute->width, attribute->height);
    return group;
}

ORBIS_SYSV_ABI s32 orbis_sceVideoOutRegisterStereoBuffers(s32 handle, s32, const void*, s32, const void*) {
    Trace("sceVideoOutRegisterStereoBuffers", "lCTCOogRbk0");
    return handle == kHandle ? 0 : kErrorInvalidHandle;
}

ORBIS_SYSV_ABI s32 orbis_sceVideoOutUnregisterBuffers(s32 handle, s32 attributeIndex) {
    Trace("sceVideoOutUnregisterBuffers", "N5KDtkIjjJ4");
    if (handle != kHandle) return kErrorInvalidHandle;
    if (attributeIndex < 0 || static_cast<std::size_t>(attributeIndex) >= kMaxGroups) return kErrorInvalidIndex;
    auto& state = Global();
    std::lock_guard guard(state.lock);
    state.groupUsed[static_cast<std::size_t>(attributeIndex)] = false;
    for (auto& buffer : state.buffers) {
        if (buffer.group == attributeIndex) buffer = Buffer{};
    }
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceVideoOutSetFlipRate(s32 handle, s32 rate) {
    Trace("sceVideoOutSetFlipRate", "CBiu4mCE1DA");
    if (handle != kHandle) return kErrorInvalidHandle;
    if (rate < 0 || rate > 2) return kErrorInvalidValue;
    Global().flipRate = static_cast<u32>(rate);
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceVideoOutGetFlipStatus(s32 handle, FlipStatus* status) {
    Trace("sceVideoOutGetFlipStatus", "lZSthJm4ikM");
    if (handle != kHandle) return kErrorInvalidHandle;
    if (status == nullptr) return kErrorInvalidAddress;
    auto& state = Global();
    std::lock_guard guard(state.lock);
    *status = state.flip;
    status->flipPendingNum = state.pending.load();
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceVideoOutIsFlipPending(s32 handle) {
    Trace("sceVideoOutIsFlipPending", "zgXifHT9ErY");
    if (handle != kHandle) return kErrorInvalidHandle;
    return Global().pending.load();
}

ORBIS_SYSV_ABI s32 orbis_sceVideoOutSubmitFlip(s32 handle, s32 bufferIndex, s32 flipMode, s64 flipArg) {
    Trace("sceVideoOutSubmitFlip", "U46NwOiJpys");
    if (handle != kHandle) return kErrorInvalidHandle;
    if (bufferIndex < -1 || static_cast<std::size_t>(bufferIndex) >= kMaxBuffers) return kErrorInvalidIndex;
    (void)flipMode;
    OrbisVideoOutFlipRequested(bufferIndex, flipArg);
    if (bufferIndex >= 0) Present(bufferIndex, flipArg);
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceVideoOutAddFlipEvent(void* equeue, s32 handle, void*) {
    Trace("sceVideoOutAddFlipEvent", "HXzjK9yI30k");
    if (handle != kHandle) return kErrorInvalidHandle;
    if (equeue == nullptr) return kErrorInvalidAddress;
    static bool warned = false;
    if (!warned) {
        warned = true;
        Log("sceVideoOutAddFlipEvent: flip events are not delivered to equeues yet; titles polling sceVideoOutGetFlipStatus work");
    }
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceVideoOutGetResolutionStatus(s32 handle, ResolutionStatus* status) {
    Trace("sceVideoOutGetResolutionStatus", "6kPnj51T62Y");
    if (handle != kHandle) return kErrorInvalidHandle;
    if (status == nullptr) return kErrorInvalidAddress;
    std::memset(status, 0, sizeof *status);
    status->fullWidth = 1920;
    status->fullHeight = 1080;
    status->paneWidth = 1920;
    status->paneHeight = 1080;
    status->refreshRate = 3;
    status->screenSizeInInch = 50.0f;
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceVideoOutGetDeviceCapabilityInfo(s32 handle, u64* info) {
    Trace("sceVideoOutGetDeviceCapabilityInfo", "kGVLc3htQE8");
    if (handle != kHandle) return kErrorInvalidHandle;
    if (info == nullptr) return kErrorInvalidAddress;
    *info = 0;
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceVideoOutConfigureOutputMode_(s32 handle, u32, const void*, const void*, std::size_t) {
    Trace("sceVideoOutConfigureOutputMode_", "N1bEoJ4SRw4");
    return handle == kHandle ? 0 : kErrorInvalidHandle;
}

ORBIS_SYSV_ABI s32 orbis_sceVideoOutModeSetAny_(s32 handle, u32, u32, u32, u32, u64) {
    Trace("sceVideoOutModeSetAny_", "pjkDsgxli6c");
    return handle == kHandle ? 0 : kErrorInvalidHandle;
}

ORBIS_SYSV_ABI s32 orbis_sceVideoOutGetVblankStatus(s32 handle, VblankStatus* status) {
    Trace("sceVideoOutGetVblankStatus", "kGVLc3htQE8");
    if (handle != kHandle) return kErrorInvalidHandle;
    if (status == nullptr) return kErrorInvalidAddress;
    auto& state = Global();
    std::lock_guard guard(state.lock);
    std::memset(status, 0, sizeof *status);
    status->count = state.vblanks;
    status->processTime = ProcessTimeMicroseconds();
    status->tsc = __rdtsc();
    return 0;
}

}
