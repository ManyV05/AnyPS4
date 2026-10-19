#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <common/OrbisHle.hpp>
#undef ORBIS_SYSV_ABI
#define ORBIS_SYSV_ABI
#include "../orbis_impl/libSceSaveData.cpp"

namespace {

int failures = 0;

void Check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) ++failures;
}

}  // namespace

int RunTests() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "anyps4-savedata-test";
    std::filesystem::remove_all(root);
    _putenv_s("ORBIS_SAVEDATA_DIR", root.string().c_str());
    _putenv_s("ORBIS_TITLE_ID", "TEST00001");

    Check(orbis_sceSaveDataMount(nullptr, nullptr) == kErrorNotInitialized, "mount before initialize is rejected");
    Check(orbis_sceSaveDataInitialize(nullptr) == 0, "initialize");

    DirName name{};
    std::strcpy(name.data, "SYSSAVE");
    Mount mount{};
    mount.userId = 1;
    mount.dirName = &name;
    mount.blocks = 256;
    mount.mountMode = kModeReadWrite;
    MountResult result{};
    Check(orbis_sceSaveDataMount(&mount, &result) == kErrorNotFound, "open of a missing directory reports NOT_FOUND");

    mount.mountMode = kModeCreate | kModeReadWrite;
    Check(orbis_sceSaveDataMount(&mount, &result) == 0, "create mount");
    Check(std::string(result.mountPoint.data) == "/savedata0", "mount point is /savedata0");
    Check(result.mountStatus == 1, "mount status reports creation");
    Check(std::filesystem::exists(root / "SYSSAVE" / "sce_sys" / "anyps4.param"), "param file created");
    Check(LoadBlocks(root / "SYSSAVE") == 256, "requested blocks recorded");
    Check(orbis_sceSaveDataMount(&mount, &result) == kErrorBusy, "mounting the same directory twice is BUSY");

    const char title[] = "Chapter 1";
    Check(orbis_sceSaveDataSetParam(&result.mountPoint, kParamTitle, title, sizeof title) == 0, "set title");
    const u32 userParam = 42;
    Check(orbis_sceSaveDataSetParam(&result.mountPoint, kParamUserParam, &userParam, sizeof userParam) == 0, "set user param");
    unsigned char png[] = {0x89, 'P', 'N', 'G', 0, 1, 2, 3};
    Icon icon{png, sizeof png, sizeof png, {}};
    Check(orbis_sceSaveDataSaveIcon(&result.mountPoint, &icon) == 0, "save icon");
    std::error_code sizeError;
    Check(std::filesystem::file_size(root / "SYSSAVE" / "sce_sys" / "icon0.png", sizeError) == sizeof png, "icon written with its size");
    {
        std::ofstream file(root / "SYSSAVE" / "data.bin", std::ios::binary);
        file << std::string(70000, 'x');
    }
    Check(orbis_sceSaveDataUmount(&result.mountPoint) == 0, "umount");
    Check(orbis_sceSaveDataUmount(&result.mountPoint) == kErrorNotMounted, "second umount is NOT_MOUNTED");

    mount.mountMode = kModeCreate | kModeReadWrite;
    Check(orbis_sceSaveDataMount(&mount, &result) == kErrorExists, "create of an existing directory reports EXISTS");
    mount.mountMode = kModeReadOnly;
    Check(orbis_sceSaveDataMount(&mount, &result) == 0 && result.mountStatus == 0, "read-only mount of the existing directory");
    Check(orbis_sceSaveDataUmount(&result.mountPoint) == 0, "umount read-only");

    std::strcpy(name.data, "SLOT01");
    mount.mountMode = kModeCreate2 | kModeReadWrite;
    Check(orbis_sceSaveDataMount(&mount, &result) == 0 && result.mountStatus == 1, "CREATE2 creates a missing directory");
    Check(orbis_sceSaveDataUmount(&result.mountPoint) == 0, "umount slot");

    SearchCond cond{};
    cond.userId = 1;
    cond.key = kSortDirName;
    static DirName names[4];
    static Param params[4];
    static SearchInfo infos[4];
    SearchResult search{};
    search.dirNames = names;
    search.dirNamesNum = 4;
    search.params = params;
    search.infos = infos;
    Check(orbis_sceSaveDataDirNameSearch(&cond, &search) == 0, "search");
    Check(search.hitNum == 2 && search.setNum == 2, "search finds both directories");
    Check(std::string(names[0].data) == "SLOT01" && std::string(names[1].data) == "SYSSAVE", "sorted by name");
    Check(std::string(params[1].title) == "Chapter 1" && params[1].userParam == 42, "params returned for SYSSAVE");
    Check(infos[1].blocks == 256 && infos[1].freeBlocks == 256 - 3, "blocks and free blocks (70000 bytes = 3 blocks)");
    DirName pattern{};
    std::strcpy(pattern.data, "SLOT*");
    cond.dirName = &pattern;
    cond.order = 1;
    Check(orbis_sceSaveDataDirNameSearch(&cond, &search) == 0 && search.hitNum == 1 && std::string(names[0].data) == "SLOT01", "wildcard filter");

    Delete del{};
    del.userId = 1;
    del.dirName = &pattern;
    Check(orbis_sceSaveDataDelete(&del) == kErrorParameter, "delete rejects a wildcard name");
    std::strcpy(pattern.data, "SLOT01");
    Check(orbis_sceSaveDataDelete(&del) == 0 && !std::filesystem::exists(root / "SLOT01"), "delete removes the directory");
    Check(orbis_sceSaveDataDelete(&del) == kErrorNotFound, "deleting again is NOT_FOUND");
    Check(orbis_sceSaveDataTerminate() == 0, "terminate");
    std::filesystem::remove_all(root);
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}

int main() {
    return RunTests();
}
