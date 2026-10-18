// AnyPS4 shim: exports are bound by NID through nid_patcher, so the shadPS4
// symbol registration macro has nothing to do.
#pragma once

namespace Core::Loader {
class SymbolsResolver;
}

#define LIB_FUNCTION(nid, lib, libversion, mod, function)
#define LIB_OBJ(nid, lib, libversion, mod, obj)
