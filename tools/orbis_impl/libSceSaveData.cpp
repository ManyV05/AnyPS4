#include <common/OrbisHle.hpp>
#include "../../3rdparty/shadps4-ngs2/shims/AnyPS4LiveSave.hpp"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <mutex>
#include <string>
#include <vector>

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
namespace fs = std::filesystem;

constexpr s32 kErrorParameter = static_cast<s32>(0x809F0000);
constexpr s32 kErrorNotInitialized = static_cast<s32>(0x809F0001);
constexpr s32 kErrorBusy = static_cast<s32>(0x809F0003);
constexpr s32 kErrorNotMounted = static_cast<s32>(0x809F0004);
constexpr s32 kErrorExists = static_cast<s32>(0x809F0007);
constexpr s32 kErrorNotFound = static_cast<s32>(0x809F0008);
constexpr s32 kErrorInternal = static_cast<s32>(0x809F000B);
constexpr s32 kErrorMountFull = static_cast<s32>(0x809F000C);

constexpr u32 kModeReadOnly = 1;
constexpr u32 kModeReadWrite = 2;
constexpr u32 kModeCreate = 4;
constexpr u32 kModeCreate2 = 32;
constexpr u32 kParamAll = 0;
constexpr u32 kParamTitle = 1;
constexpr u32 kParamSubTitle = 2;
constexpr u32 kParamDetail = 3;
constexpr u32 kParamUserParam = 4;
constexpr u32 kParamMtime = 5;
constexpr u32 kSortDirName = 0;
constexpr u32 kSortUserParam = 1;
constexpr u32 kSortBlocks = 2;
constexpr u32 kSortMtime = 3;
constexpr u32 kSortFreeBlocks = 5;
constexpr u64 kBlockSize = 32768;
constexpr u64 kDefaultBlocks = 96;
constexpr std::size_t kSlots = 16;
constexpr int kSaveSlotCount = 5;

struct TitleId {
    char data[10];
    char pad[6];
};

struct DirName {
    char data[32];
};

struct MountPoint {
    char data[16];
};

struct Mount {
    s32 userId;
    s32 pad;
    const TitleId* titleId;
    const DirName* dirName;
    const void* fingerprint;
    u64 blocks;
    u32 mountMode;
    u8 reserved[32];
};

struct MountResult {
    MountPoint mountPoint;
    u64 requiredBlocks;
    u32 unused;
    u32 mountStatus;
    u8 reserved[28];
    s32 pad;
};

struct Param {
    char title[128];
    char subTitle[128];
    char detail[1024];
    u32 userParam;
    u32 pad;
    std::int64_t mtime;
    u8 reserved[32];
};

struct SearchInfo {
    u64 blocks;
    u64 freeBlocks;
    u8 reserved[32];
};

struct SearchCond {
    s32 userId;
    s32 pad;
    const TitleId* titleId;
    const DirName* dirName;
    u32 key;
    u32 order;
    u8 reserved[32];
};

struct SearchResult {
    u32 hitNum;
    u32 pad;
    DirName* dirNames;
    u32 dirNamesNum;
    u32 setNum;
    Param* params;
    SearchInfo* infos;
    u8 reserved[12];
    s32 pad2;
};

struct Delete {
    s32 userId;
    s32 pad;
    const TitleId* titleId;
    const DirName* dirName;
    u32 unused;
    u8 reserved[32];
    s32 pad2;
};

struct Icon {
    void* buf;
    std::size_t bufSize;
    std::size_t dataSize;
    u8 reserved[32];
};

static_assert(sizeof(Mount) == 80);
static_assert(sizeof(MountResult) == 64);
static_assert(sizeof(Param) == 1328);
static_assert(sizeof(SearchCond) == 64);
static_assert(sizeof(SearchResult) == 56);
static_assert(sizeof(Delete) == 64);
static_assert(sizeof(Icon) == 56);

struct Slot {
    bool used = false;
    std::string mountPoint;
    std::string dirName;
    fs::path hostPath;
    bool readOnly = false;
};

using AliasFunction = void (*)(const char*, const char*);
using UnaliasFunction = void (*)(const char*);

struct State {
    std::mutex lock;
    bool initialized = false;
    fs::path root;
    std::string titleId;
    int saveSlot = 0;
    int launchSlot = 0;
    fs::path saveData;
    anyps4::LiveSaveBlock* live = nullptr;
    std::array<Slot, kSlots> slots{};
    AliasFunction addAlias = nullptr;
    UnaliasFunction removeAlias = nullptr;
};

State& Global() {
    static auto* state = new State();
    return *state;
}

void Trace(const char* function, const char* nid) {
    static std::uint64_t calls = 0;
    OrbisHleTrace("libSceSaveData.prx", function, nid, ++calls, __builtin_return_address(0));
}

void Log(const std::string& text) {
    std::fprintf(stderr, "[SaveData] %s\n", text.c_str());
    std::fflush(stderr);
}

fs::path ExecutableDirectory() {
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return fs::path(std::wstring(buffer, length)).parent_path();
}

std::string ReadTitleId(const fs::path& sfo) {
    std::ifstream file(sfo, std::ios::binary);
    std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.size() < 20 || std::memcmp(data.data(), "\0PSF", 4) != 0) return {};
    auto u32At = [&](std::size_t offset) { u32 value = 0; std::memcpy(&value, data.data() + offset, 4); return value; };
    auto u16At = [&](std::size_t offset) { std::uint16_t value = 0; std::memcpy(&value, data.data() + offset, 2); return value; };
    const u32 keyTable = u32At(8);
    const u32 dataTable = u32At(12);
    const u32 entries = u32At(16);
    for (u32 k = 0; k < entries; ++k) {
        const std::size_t entry = 20 + static_cast<std::size_t>(k) * 16;
        if (entry + 16 > data.size()) break;
        const std::size_t keyOffset = keyTable + u16At(entry);
        const u32 dataLength = u32At(entry + 4);
        const std::size_t dataOffset = dataTable + u32At(entry + 12);
        if (keyOffset >= data.size() || dataOffset + dataLength > data.size()) continue;
        if (std::strncmp(data.data() + keyOffset, "TITLE_ID", data.size() - keyOffset) == 0) {
            std::string value(data.data() + dataOffset, dataLength);
            value.erase(std::find(value.begin(), value.end(), '\0'), value.end());
            return value;
        }
    }
    return {};
}

std::string CString(const char* data, std::size_t capacity) {
    const char* end = static_cast<const char*>(std::memchr(data, '\0', capacity));
    return std::string(data, end != nullptr ? static_cast<std::size_t>(end - data) : capacity);
}

void CopyCString(char* target, std::size_t capacity, const std::string& text) {
    const std::size_t length = std::min(text.size(), capacity - 1);
    std::memcpy(target, text.data(), length);
    std::memset(target + length, 0, capacity - length);
}

bool ValidDirName(const std::string& name) {
    if (name.empty() || name.size() > 31) return false;
    for (const char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') return false;
    }
    return true;
}

fs::path ParamFile(const fs::path& dir) {
    return dir / "sce_sys" / "anyps4.param";
}

fs::path BlocksFile(const fs::path& dir) {
    return dir / "sce_sys" / "anyps4.blocks";
}

bool LoadParam(const fs::path& dir, Param& param) {
    std::ifstream file(ParamFile(dir), std::ios::binary);
    std::memset(&param, 0, sizeof param);
    return file.good() && file.read(reinterpret_cast<char*>(&param), sizeof param).gcount() == static_cast<std::streamsize>(sizeof param);
}

bool SaveParam(const fs::path& dir, const Param& param) {
    std::error_code error;
    fs::create_directories(dir / "sce_sys", error);
    std::ofstream file(ParamFile(dir), std::ios::binary | std::ios::trunc);
    return file.good() && file.write(reinterpret_cast<const char*>(&param), sizeof param).good();
}

u64 LoadBlocks(const fs::path& dir) {
    std::ifstream file(BlocksFile(dir));
    u64 blocks = 0;
    return file >> blocks ? blocks : kDefaultBlocks;
}

u64 UsedBlocks(const fs::path& dir) {
    u64 bytes = 0;
    std::error_code error;
    for (const auto& entry : fs::recursive_directory_iterator(dir, error)) {
        if (entry.is_regular_file(error)) bytes += entry.file_size(error);
    }
    return (bytes + kBlockSize - 1) / kBlockSize;
}

// '%' is the wildcard of sceSaveDataDirNameSearch patterns (the title searches "WITCHANDNIGHT%"); '*' and '?' are accepted too.
bool Glob(const std::string& pattern, const std::string& text) {
    std::size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || std::tolower(pattern[p]) == std::tolower(text[t]))) {
            ++p;
            ++t;
        } else if (p < pattern.size() && (pattern[p] == '*' || pattern[p] == '%')) {
            star = p++;
            mark = t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && (pattern[p] == '*' || pattern[p] == '%')) ++p;
    return p == pattern.size();
}

int ParseSaveSlot(const char* text) {
    if (text == nullptr || *text == '\0') return 1;
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (*end != '\0' || value < 1 || value > kSaveSlotCount) {
        Log(std::string("ORBIS_SAVE_SLOT=") + text + " is not a slot from 1 to " + std::to_string(kSaveSlotCount) + ": using slot 1");
        return 1;
    }
    return static_cast<int>(value);
}

fs::path ResolveSaveRoot(const fs::path& saveData, const std::string& titleId, int slot) {
    const fs::path legacy = saveData / titleId;
    const fs::path root = saveData / ("slot_" + std::to_string(slot)) / titleId;
    std::error_code error;
    if (slot == 1 && fs::is_directory(legacy, error) && !fs::exists(root, error)) {
        fs::create_directories(root.parent_path(), error);
        fs::rename(legacy, root, error);
        if (error) {
            Log("could not move the saves of " + legacy.string() + " into slot 1 (" + error.message() + "): using them where they are");
            return legacy;
        }
        Log("moved the existing saves of " + legacy.string() + " into slot 1");
    }
    return root;
}

void ApplyRequestedSaveSlot(State& state) {
    if (state.live == nullptr) return;
    for (const Slot& slot : state.slots) {
        if (slot.used) return;
    }
    const int requested = state.live->requestedSlot.load();
    if (requested < 1 || requested > kSaveSlotCount || requested == state.saveSlot) return;
    state.root = ResolveSaveRoot(state.saveData, state.titleId, requested);
    std::error_code error;
    fs::create_directories(state.root, error);
    state.saveSlot = requested;
    state.live->activeSlot.store(requested);
    Log("save slot changed to " + std::to_string(requested) + ", save directory " + state.root.string());
}

void EnsureInitialized(State& state) {
    if (!state.root.empty()) {
        ApplyRequestedSaveSlot(state);
        return;
    }
    const fs::path exeDir = ExecutableDirectory();
    if (const char* id = std::getenv("ORBIS_TITLE_ID")) state.titleId = id;
    if (state.titleId.empty()) state.titleId = ReadTitleId(exeDir / "app0" / "sce_sys" / "param.sfo");
    if (state.titleId.empty()) state.titleId = "UNKNOWN";
    if (const char* dir = std::getenv("ORBIS_SAVEDATA_DIR")) {
        state.root = fs::path(dir);
    } else {
        state.saveSlot = ParseSaveSlot(std::getenv("ORBIS_SAVE_SLOT"));
        state.saveData = exeDir / "savedata";
        state.launchSlot = state.saveSlot;
        state.root = ResolveSaveRoot(state.saveData, state.titleId, state.saveSlot);
        state.live = anyps4::LiveSaveBlockOfThisProcess(state.saveSlot);
    }
    std::error_code error;
    fs::create_directories(state.root, error);
    const HMODULE libc = GetModuleHandleW(L"libc.prx");
    if (libc != nullptr) {
        state.addAlias = reinterpret_cast<AliasFunction>(reinterpret_cast<void*>(GetProcAddress(libc, "AddPathAlias_nid_no_patch")));
        state.removeAlias = reinterpret_cast<UnaliasFunction>(reinterpret_cast<void*>(GetProcAddress(libc, "RemovePathAlias_nid_no_patch")));
    }
    Log("title " + state.titleId + (state.saveSlot != 0 ? ", save slot " + std::to_string(state.saveSlot) : std::string()) + ", save directory " + state.root.string() + (state.addAlias != nullptr ? "" : " (libc path aliases unavailable: mounts will not resolve)"));
}

Slot* FindSlot(State& state, const MountPoint* mountPoint) {
    if (mountPoint == nullptr) return nullptr;
    const std::string name = CString(mountPoint->data, sizeof mountPoint->data);
    for (Slot& slot : state.slots) {
        if (slot.used && slot.mountPoint == name) return &slot;
    }
    return nullptr;
}

void TouchMtime(const fs::path& dir) {
    Param param{};
    if (!LoadParam(dir, param)) std::memset(&param, 0, sizeof param);
    param.mtime = static_cast<std::int64_t>(std::time(nullptr));
    SaveParam(dir, param);
}

s32 Unmount(State& state, Slot& slot) {
    if (!slot.readOnly) {
        TouchMtime(slot.hostPath);
        if (state.live != nullptr) {
            state.live->lastSavedSlot.store(state.saveSlot);
            state.live->savesDone.fetch_add(1);
        }
    }
    if (state.removeAlias != nullptr) state.removeAlias(slot.mountPoint.c_str());
    Log("umount " + slot.mountPoint + " (" + slot.dirName + ")");
    slot = Slot{};
    return 0;
}

}  // namespace

extern "C" {

ORBIS_SYSV_ABI s32 orbis_sceSaveDataInitialize(const void*) {
    Trace("sceSaveDataInitialize", "ZkZhskCPXFw");
    auto& state = Global();
    std::lock_guard guard(state.lock);
    EnsureInitialized(state);
    state.initialized = true;
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceSaveDataTerminate() {
    Trace("sceSaveDataTerminate", "yKDy8S5yLA0");
    auto& state = Global();
    std::lock_guard guard(state.lock);
    for (Slot& slot : state.slots) {
        if (slot.used) Unmount(state, slot);
    }
    state.initialized = false;
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceSaveDataMount(const Mount* mount, MountResult* result) {
    Trace("sceSaveDataMount", "32HQAQdwM2o");
    auto& state = Global();
    std::unique_lock guard(state.lock);
    if (!state.initialized) return kErrorNotInitialized;
    if (mount == nullptr || result == nullptr || mount->dirName == nullptr) return kErrorParameter;
    EnsureInitialized(state);
    const std::string dirName = CString(mount->dirName->data, sizeof mount->dirName->data);
    if (!ValidDirName(dirName)) return kErrorParameter;
    const bool create = (mount->mountMode & kModeCreate) != 0;
    const bool readWrite = (mount->mountMode & kModeReadWrite) != 0;
    const bool switched = state.live != nullptr && state.saveSlot != state.launchSlot;
    const bool createIfMissing = (mount->mountMode & kModeCreate2) != 0 || (switched && readWrite);
    const bool readOnly = (mount->mountMode & kModeReadOnly) != 0 && !readWrite;
    for (const Slot& slot : state.slots) {
        if (slot.used && slot.dirName == dirName) return kErrorBusy;
    }
    const fs::path hostPath = state.root / dirName;
    std::error_code error;
    const bool exists = fs::is_directory(hostPath, error);
    if (!create && !createIfMissing && !exists) {
        Log("mount " + dirName + " mode " + std::to_string(mount->mountMode) + ": not found");
        return kErrorNotFound;
    }
    if (create && exists) {
        Log("mount " + dirName + " mode " + std::to_string(mount->mountMode) + ": already exists");
        return kErrorExists;
    }
    Slot* free = nullptr;
    std::size_t index = 0;
    for (std::size_t k = 0; k < kSlots; ++k) {
        if (!state.slots[k].used) {
            free = &state.slots[k];
            index = k;
            break;
        }
    }
    if (free == nullptr) return kErrorMountFull;
    const bool created = !exists;
    free->used = true;
    free->mountPoint = "/savedata" + std::to_string(index);
    free->dirName = dirName;
    free->hostPath = hostPath;
    free->readOnly = readOnly;
    const std::string mountPoint = free->mountPoint;
    const AliasFunction addAlias = state.addAlias;
    guard.unlock();
    if (created) {
        fs::create_directories(hostPath / "sce_sys", error);
        if (error) {
            guard.lock();
            *free = Slot{};
            return kErrorInternal;
        }
        Param param{};
        param.mtime = static_cast<std::int64_t>(std::time(nullptr));
        if (!SaveParam(hostPath, param)) Log("could not write " + ParamFile(hostPath).string());
        std::ofstream blocks(BlocksFile(hostPath));
        blocks << (mount->blocks != 0 ? mount->blocks : kDefaultBlocks);
    } else if (!fs::exists(ParamFile(hostPath), error)) {
        Param param{};
        param.mtime = static_cast<std::int64_t>(std::time(nullptr));
        if (!SaveParam(hostPath, param)) Log("could not write " + ParamFile(hostPath).string());
        if (!fs::exists(BlocksFile(hostPath), error)) {
            std::ofstream blocks(BlocksFile(hostPath));
            blocks << (mount->blocks != 0 ? mount->blocks : kDefaultBlocks);
        }
        Log("mount " + dirName + ": metadata of an existing save restored");
    }
    if (addAlias != nullptr) addAlias(mountPoint.c_str(), fs::absolute(hostPath, error).string().c_str());
    std::memset(result, 0, sizeof *result);
    CopyCString(result->mountPoint.data, sizeof result->mountPoint.data, mountPoint);
    result->mountStatus = created ? 1 : 0;
    Log("mount " + dirName + " mode " + std::to_string(mount->mountMode) + " -> " + mountPoint + (created ? " (created)" : " (existing)") + " at " +
        hostPath.string());
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceSaveDataUmount(const MountPoint* mountPoint) {
    Trace("sceSaveDataUmount", "BMR4F-Uek3E");
    auto& state = Global();
    std::lock_guard guard(state.lock);
    if (!state.initialized) return kErrorNotInitialized;
    Slot* slot = FindSlot(state, mountPoint);
    if (slot == nullptr) return kErrorNotMounted;
    return Unmount(state, *slot);
}

ORBIS_SYSV_ABI s32 orbis_sceSaveDataSetParam(const MountPoint* mountPoint, u32 type, const void* buffer, std::size_t size) {
    Trace("sceSaveDataSetParam", "85zul--eGXs");
    auto& state = Global();
    std::unique_lock guard(state.lock);
    if (!state.initialized) return kErrorNotInitialized;
    if (buffer == nullptr) return kErrorParameter;
    const Slot* found = FindSlot(state, mountPoint);
    if (found == nullptr) return kErrorNotMounted;
    const Slot slotCopy = *found;
    const Slot* slot = &slotCopy;
    guard.unlock();
    Param param{};
    if (!LoadParam(slot->hostPath, param)) std::memset(&param, 0, sizeof param);
    switch (type) {
    case kParamAll:
        if (size < sizeof(Param)) return kErrorParameter;
        std::memcpy(&param, buffer, sizeof param);
        break;
    case kParamTitle:
        CopyCString(param.title, sizeof param.title, std::string(static_cast<const char*>(buffer), std::min(size, sizeof param.title - 1)));
        break;
    case kParamSubTitle:
        CopyCString(param.subTitle, sizeof param.subTitle, std::string(static_cast<const char*>(buffer), std::min(size, sizeof param.subTitle - 1)));
        break;
    case kParamDetail:
        CopyCString(param.detail, sizeof param.detail, std::string(static_cast<const char*>(buffer), std::min(size, sizeof param.detail - 1)));
        break;
    case kParamUserParam:
        if (size < sizeof(u32)) return kErrorParameter;
        std::memcpy(&param.userParam, buffer, sizeof param.userParam);
        break;
    case kParamMtime:
        if (size < sizeof(std::int64_t)) return kErrorParameter;
        std::memcpy(&param.mtime, buffer, sizeof param.mtime);
        break;
    default:
        return kErrorParameter;
    }
    if (type != kParamMtime) param.mtime = static_cast<std::int64_t>(std::time(nullptr));
    if (!SaveParam(slot->hostPath, param)) return kErrorInternal;
    Log("set param type " + std::to_string(type) + " on " + slot->mountPoint + " (title \"" + CString(param.title, sizeof param.title) + "\", user param " +
        std::to_string(param.userParam) + ")");
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceSaveDataSaveIcon(const MountPoint* mountPoint, const Icon* icon) {
    Trace("sceSaveDataSaveIcon", "c88Yy54Mx0w");
    auto& state = Global();
    std::unique_lock guard(state.lock);
    if (!state.initialized) return kErrorNotInitialized;
    if (icon == nullptr || icon->buf == nullptr) return kErrorParameter;
    const Slot* found = FindSlot(state, mountPoint);
    if (found == nullptr) return kErrorNotMounted;
    const Slot slotCopy = *found;
    const Slot* slot = &slotCopy;
    guard.unlock();
    std::error_code error;
    fs::create_directories(slot->hostPath / "sce_sys", error);
    std::ofstream file(slot->hostPath / "sce_sys" / "icon0.png", std::ios::binary | std::ios::trunc);
    const std::size_t size = std::min(icon->dataSize, icon->bufSize);
    if (!file.write(static_cast<const char*>(icon->buf), static_cast<std::streamsize>(size)).good()) return kErrorInternal;
    Log("icon of " + std::to_string(size) + " bytes saved on " + slot->mountPoint);
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceSaveDataDirNameSearch(const SearchCond* cond, SearchResult* result) {
    Trace("sceSaveDataDirNameSearch", "dyIhnXq-0SM");
    auto& state = Global();
    std::unique_lock guard(state.lock);
    if (!state.initialized) return kErrorNotInitialized;
    if (cond == nullptr || result == nullptr) return kErrorParameter;
    EnsureInitialized(state);
    const fs::path root = state.root;
    guard.unlock();
    result->hitNum = 0;
    result->setNum = 0;
    struct Entry {
        std::string name;
        Param param;
        u64 blocks;
        u64 freeBlocks;
    };
    std::vector<Entry> entries;
    std::error_code error;
    const std::string pattern = cond->dirName != nullptr ? CString(cond->dirName->data, sizeof cond->dirName->data) : std::string();
    for (const auto& item : fs::directory_iterator(root, error)) {
        if (!item.is_directory(error)) continue;
        const std::string name = item.path().filename().string();
        if (name.rfind("sce_", 0) == 0 || !ValidDirName(name)) continue;
        if (!pattern.empty() && !Glob(pattern, name)) continue;
        Entry entry{name, {}, LoadBlocks(item.path()), 0};
        if (!LoadParam(item.path(), entry.param)) entry.param.mtime = static_cast<std::int64_t>(std::time(nullptr));
        const u64 used = UsedBlocks(item.path());
        entry.freeBlocks = entry.blocks > used ? entry.blocks - used : 0;
        entries.push_back(entry);
    }
    auto less = [&](const Entry& a, const Entry& b) {
        switch (cond->key) {
        case kSortUserParam: return a.param.userParam < b.param.userParam;
        case kSortBlocks: return a.blocks < b.blocks;
        case kSortMtime: return a.param.mtime < b.param.mtime;
        case kSortFreeBlocks: return a.freeBlocks < b.freeBlocks;
        default: return a.name < b.name;
        }
    };
    std::stable_sort(entries.begin(), entries.end(), less);
    if (cond->order == 1) std::reverse(entries.begin(), entries.end());
    result->hitNum = static_cast<u32>(entries.size());
    const u32 count = std::min<u32>(result->dirNamesNum, result->hitNum);
    for (u32 k = 0; k < count; ++k) {
        if (result->dirNames != nullptr) CopyCString(result->dirNames[k].data, sizeof result->dirNames[k].data, entries[k].name);
        if (result->params != nullptr) result->params[k] = entries[k].param;
        if (result->infos != nullptr) {
            std::memset(&result->infos[k], 0, sizeof result->infos[k]);
            result->infos[k].blocks = entries[k].blocks;
            result->infos[k].freeBlocks = entries[k].freeBlocks;
        }
    }
    result->setNum = result->dirNames != nullptr ? count : 0;
    std::string names;
    for (const Entry& entry : entries) names += (names.empty() ? "" : ", ") + entry.name;
    Log("search \"" + pattern + "\" key " + std::to_string(cond->key) + ": " + std::to_string(entries.size()) + " hit(s)" + (names.empty() ? "" : " [" + names + "]"));
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceSaveDataDelete(const Delete* request) {
    Trace("sceSaveDataDelete", "S1GkePI17zQ");
    auto& state = Global();
    std::unique_lock guard(state.lock);
    if (!state.initialized) return kErrorNotInitialized;
    if (request == nullptr || request->dirName == nullptr) return kErrorParameter;
    EnsureInitialized(state);
    const std::string dirName = CString(request->dirName->data, sizeof request->dirName->data);
    if (!ValidDirName(dirName)) return kErrorParameter;
    for (const Slot& slot : state.slots) {
        if (slot.used && slot.dirName == dirName) return kErrorBusy;
    }
    std::error_code error;
    const fs::path hostPath = state.root / dirName;
    guard.unlock();
    if (!fs::is_directory(hostPath, error)) return kErrorNotFound;
    fs::remove_all(hostPath, error);
    Log("delete " + dirName + (error ? " failed" : ""));
    return error ? kErrorInternal : 0;
}

}
