// AnyPS4 adapter: starts the command processor when libSceGnmDriver.prx loads.
#include "core/libraries/gnmdriver/gnmdriver.h"
#include "common/logging/log.h"
#include <windows.h>

namespace Frontend {
class WindowSDL;
}

Frontend::WindowSDL* g_window = nullptr;

namespace {

struct GnmModule {
    GnmModule() {
        Libraries::GnmDriver::RegisterLib(nullptr);
        LOG_INFO(Lib_GnmDriver, "headless Gnm command processor started");
    }
};

GnmModule gnm_module;

} // namespace
