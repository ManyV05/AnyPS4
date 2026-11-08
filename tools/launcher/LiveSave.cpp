#include "LiveSave.hpp"

#include <windows.h>

#include "AnyPS4LiveSave.hpp"

namespace launcher {

namespace {

struct Connection {
    unsigned long processId = 0;
    HANDLE mapping = nullptr;
    anyps4::LiveSaveBlock* block = nullptr;
};

Connection g_connection;

void Disconnect() {
    if (g_connection.block != nullptr) UnmapViewOfFile(g_connection.block);
    if (g_connection.mapping != nullptr) CloseHandle(g_connection.mapping);
    g_connection = Connection{};
}

anyps4::LiveSaveBlock* Connect(unsigned long processId) {
    if (processId == 0) return nullptr;
    if (g_connection.processId == processId && g_connection.block != nullptr) return g_connection.block;
    Disconnect();
    wchar_t name[64];
    anyps4::LiveSaveName(processId, name);
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    if (mapping == nullptr) return nullptr;
    void* view = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(anyps4::LiveSaveBlock));
    if (view == nullptr) {
        CloseHandle(mapping);
        return nullptr;
    }
    g_connection.processId = processId;
    g_connection.mapping = mapping;
    g_connection.block = static_cast<anyps4::LiveSaveBlock*>(view);
    return g_connection.block;
}

}  // namespace

bool PushLiveSaveSlot(unsigned long processId, int slot) {
    anyps4::LiveSaveBlock* block = Connect(processId);
    if (block == nullptr || block->state.load() != 2) return false;
    block->requestedSlot.store(slot);
    return true;
}

bool ReadLiveSave(unsigned long processId, SaveActivity& activity) {
    anyps4::LiveSaveBlock* block = Connect(processId);
    if (block == nullptr || block->state.load() != 2) return false;
    activity.savesDone = block->savesDone.load();
    activity.lastSavedSlot = block->lastSavedSlot.load();
    activity.activeSlot = block->activeSlot.load();
    return true;
}

}  // namespace launcher
