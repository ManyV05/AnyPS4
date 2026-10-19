"""Generate the Orbis (PS4) runtime libraries for a relinked executable.

Inputs: the <output>.registry.json written by `relinker --registry`, optionally the
original eboot.bin (plain ELF or fake SELF) to recover the full DT_NEEDED list and the
library/module ids behind every NID, and a shadPS4 checkout to name the functions.

Every module named by the executable becomes a <module>.prx Windows library under
core/libs/orbis. Modules listed in --real are built from the Prospero implementation in
core/libs/prx (PS4 and PS5 derive NIDs from function names with the same hash, so the
shared functions resolve as they are) plus a complement file for the NIDs that
implementation lacks; every other module is a .def plus logging stubs that return 0.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

BASE64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"
NID_SALT = bytes([0x51, 0x8D, 0x64, 0xA6, 0x35, 0xDE, 0xD8, 0xC1, 0xE6, 0xB0, 0x39, 0xB1, 0xC3, 0xE5, 0x52, 0x30])
SELF_MAGIC = 0x1D3D154F
PT_DYNAMIC = 2
PT_SCE_DYNLIBDATA = 0x61000000
DT_NULL = 0
DT_NEEDED = 1
DT_SCE_NEEDED_MODULE = 0x6100000F
DT_SCE_IMPORT_LIB = 0x61000015
DT_SCE_STRTAB = 0x61000035
DT_SCE_STRSZ = 0x61000037
DT_SCE_SYMTAB = 0x61000039
DT_SCE_SYMTABSZ = 0x6100003F
IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
LIB_FUNCTION = re.compile(
    r'LIB_FUNCTION\(\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*(\d+)\s*,\s*"([^"]+)"\s*,\s*&?\s*([A-Za-z_][\w:]*)\s*\)')
LIB_OBJ = re.compile(
    r'LIB_OBJ\(\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*(\d+)\s*,\s*"([^"]+)"\s*,\s*&?\s*([A-Za-z_][\w:]*)\s*\)')
STUB = re.compile(r'STUB\(\s*"([^"]+)"\s*,\s*([^\s)]+)\s*\)')
VABI_FUNCTION = re.compile(r"APS5_VABI\s+(\w+)\s*\(")
POSTFIX_FUNCTION = re.compile(r"^[A-Za-z_\[][^=;{}]*?([A-Za-z_][A-Za-z0-9_]*_nid_postfix)\s*\(", re.MULTILINE)
ASM_FUNCTION = re.compile(r'APS5_ASM_FUNCTION\(\s*"(\w+)"\s*\)')
EXPLICIT_EXPORT = re.compile(r'APS5_EXPORT\(\s*"([^"]+)"\s*,\s*(\w+)\s*\)')
DATA_DEFINITION = re.compile(r"^[A-Za-z_][\w:<>\*&\s]*?\b(\w+(?:_nid_postfix)?)\s*(?:\[[^\]]*\])?\s*=", re.MULTILINE)
CMAKE_SOURCE = re.compile(r"\$\{(CMAKE_CURRENT_SOURCE_DIR|LIBS_INCLUDE_DIR)\}/([\w./]+\.cpp)")
def OBJECT_DEFINITION(name: str) -> re.Pattern[str]:
    return re.compile(r"^(?!\s*extern\b)[^\n]*?\b" + re.escape(name) + r"(?:_nid_postfix)?\s*(?:\{|=|;|\[)", re.MULTILINE)


KNOWN_OBJECTS = {"g_stack_chk_guard", "__stack_chk_guard", "Need_sceLibc", "_Stdout", "_Stdin", "_Stderr", "_ZSt7nothrow",
                 "kernel_environ", "g_progname"}


def compute_nid(name: str) -> str:
    value = int.from_bytes(hashlib.sha1(name.encode("utf-8") + NID_SALT).digest()[:8], "little")
    return "".join(BASE64[(value >> (58 - 6 * index)) & 0x3F] for index in range(10)) + BASE64[(value & 0xF) * 4]


def encode_id(value: int) -> str:
    if value < 0x40:
        return BASE64[value]
    if value < 0x1000:
        return BASE64[(value >> 6) & 0x3F] + BASE64[value & 0x3F]
    return BASE64[(value >> 12) & 0x3F] + BASE64[(value >> 6) & 0x3F] + BASE64[value & 0x3F]


def split_nid(symbol: str) -> tuple[str, str | None, str | None]:
    parts = symbol.split("#")
    if len(parts) == 3:
        return parts[0], parts[1], parts[2]
    return parts[0], None, None


class Eboot:
    """Reads DT_NEEDED, module ids, library ids and undefined symbols from an eboot."""

    def __init__(self, path: Path) -> None:
        data = path.read_bytes()
        self.needed: list[str] = []
        self.modules: dict[str, str] = {}
        self.libraries: dict[str, str] = {}
        self.imports: list[str] = []
        segments: list[tuple[int, int, int, int]] = []
        elf_offset = 0
        if struct.unpack_from("<I", data, 0)[0] == SELF_MAGIC:
            count = struct.unpack_from("<H", data, 0x18)[0]
            segments = [struct.unpack_from("<QQQQ", data, 0x20 + i * 0x20) for i in range(count)]
            elf_offset = 0x20 + count * 0x20
        if data[elf_offset:elf_offset + 4] != b"\x7fELF":
            raise SystemExit(f"{path}: not an ELF or fake SELF image")
        phoff = struct.unpack_from("<Q", data, elf_offset + 0x20)[0]
        phnum = struct.unpack_from("<H", data, elf_offset + 0x38)[0]
        headers = [struct.unpack_from("<IIQQQQQQ", data, elf_offset + phoff + i * 56) for i in range(phnum)]

        def read(offset: int, size: int) -> bytes:
            if not segments:
                return data[offset:offset + size]
            for flags, file_offset, _, _ in segments:
                if not flags & 0x800:
                    continue
                header = headers[(flags >> 20) & 0xFFF]
                if header[2] <= offset < header[2] + header[5]:
                    start = file_offset + (offset - header[2])
                    return data[start:start + size]
            return b""

        dynamic = next((h for h in headers if h[0] == PT_DYNAMIC), None)
        dynlib = next((h for h in headers if h[0] == PT_SCE_DYNLIBDATA), None)
        if dynamic is None or dynlib is None:
            raise SystemExit(f"{path}: PT_DYNAMIC or PT_SCE_DYNLIBDATA is missing")
        tags: list[tuple[int, int]] = []
        table = read(dynamic[2], dynamic[5])
        for offset in range(0, len(table), 16):
            tag, value = struct.unpack_from("<qQ", table, offset)
            if tag == DT_NULL:
                break
            tags.append((tag, value))
        values = dict(tags)
        blob = read(dynlib[2], dynlib[5])
        strings = blob[values[DT_SCE_STRTAB]:values[DT_SCE_STRTAB] + values[DT_SCE_STRSZ]]

        def cstr(offset: int) -> str:
            return strings[offset:strings.index(b"\0", offset)].decode()

        for tag, value in tags:
            if tag == DT_NEEDED:
                self.needed.append(cstr(value))
            elif tag == DT_SCE_NEEDED_MODULE:
                self.modules[encode_id(value >> 48)] = cstr(value & 0xFFFFFFFF)
            elif tag == DT_SCE_IMPORT_LIB:
                self.libraries[encode_id(value >> 48)] = cstr(value & 0xFFFFFFFF)
        symbols = blob[values[DT_SCE_SYMTAB]:values[DT_SCE_SYMTAB] + values[DT_SCE_SYMTABSZ]]
        for offset in range(24, len(symbols), 24):
            name, info, _, section, value, _ = struct.unpack_from("<IBBHQQ", symbols, offset)
            if section == 0 and value == 0 and (info & 0xF) in (0, 1, 2) and (info >> 4) in (1, 2):
                self.imports.append(cstr(name))

    def module_file(self, module_id: str) -> str | None:
        module = self.modules.get(module_id)
        if module is None:
            return None
        stems = {needed: needed[:needed.rfind(".")] if "." in needed else needed for needed in self.needed}
        for needed, stem in stems.items():
            if stem == module:
                return needed
        candidates = [needed for needed, stem in stems.items() if stem.startswith(module) or module.startswith(stem)]
        if candidates:
            return max(candidates, key=lambda needed: len(stems[needed]))
        return None


class ShadPs4Names:
    """NID to function name mapping harvested from a shadPS4 checkout."""

    def __init__(self, root: Path | None) -> None:
        self.implemented: dict[str, tuple[str, str, str]] = {}
        self.objects: dict[str, tuple[str, str, str]] = {}
        self.names: dict[str, str] = {}
        if root is None:
            return
        core = root / "src" / "core" if (root / "src" / "core").is_dir() else root
        for source in sorted((core / "libraries").rglob("*.cpp")):
            text = source.read_text(encoding="utf-8", errors="replace")
            for nid, library, _, module, function in LIB_FUNCTION.findall(text):
                self.implemented.setdefault(nid, (function.split("::")[-1], library, module))
            for nid, library, _, module, symbol in LIB_OBJ.findall(text):
                self.objects.setdefault(nid, (symbol.split("::")[-1], library, module))
        table = core / "aerolib" / "aerolib.inl"
        if table.is_file():
            for nid, name in STUB.findall(table.read_text(encoding="utf-8", errors="replace")):
                self.names.setdefault(nid, name)


class ProsperoLibrary:
    """A core/libs/prx library: its source list and the NIDs its exports will carry after nid_patcher."""

    def __init__(self, libs_root: Path, stem: str) -> None:
        self.stem = stem
        self.directory = libs_root / "prx" / stem
        cmake = (self.directory / "CMakeLists.txt").read_text(encoding="utf-8")
        self.sources: list[str] = []
        for base, relative in CMAKE_SOURCE.findall(cmake):
            path = ("prx/" + stem + "/" + relative) if base == "CMAKE_CURRENT_SOURCE_DIR" else relative
            if "tests/" not in path and path not in self.sources:
                self.sources.append(path)
        self.provided: dict[str, str] = {}
        for source in self.directory.rglob("*.cpp"):
            if "tests" in source.parts:
                continue
            text = source.read_text(encoding="utf-8", errors="replace")
            for name in VABI_FUNCTION.findall(text) + ASM_FUNCTION.findall(text) + POSTFIX_FUNCTION.findall(text):
                self._provide(name)
            for name in DATA_DEFINITION.findall(text):
                if name.endswith("_nid_postfix"):
                    self._provide(name)
            for base in KNOWN_OBJECTS:
                if OBJECT_DEFINITION(base).search(text):
                    self._provide(base)
            for nid, function in EXPLICIT_EXPORT.findall(text):
                self.provided[nid] = function

    def _provide(self, name: str) -> None:
        if name.endswith("_nid_no_patch") or name.endswith("_nid_no_patch_cut"):
            return
        base = name[:-len("_nid_postfix")] if name.endswith("_nid_postfix") else name
        base = re.sub(r"_nid_disambig\d+$", "", base)
        self.provided.setdefault(compute_nid(base), name)


class VendoredLibrary:
    """A module built from vendored shadPS4 sources: the NIDs its PS4_SYSV_ABI functions export after nid_patcher."""

    PS4_FUNCTION = re.compile("PS4_SYSV_ABI[ ]+([A-Za-z_][A-Za-z0-9_]*)[ ]*[(]")

    DECLARATION = re.compile(r"([A-Za-z_][\w:<>\*&\s]*?)\s+PS4_SYSV_ABI\s+([A-Za-z_]\w*)\s*\(([^;{]*?)\)\s*;")
    DEFINITION = re.compile(r"^([A-Za-z_][\w:<>\*&\s]*?)\s+PS4_SYSV_ABI\s+([A-Za-z_]\w*)\s*\(([^;{]*?)\)\s*(?:try\s*)?\{", re.MULTILINE)
    NAMESPACE = re.compile(r"namespace\s+([\w:]+)\s*\{")

    def __init__(self, repo_root: Path, directory: str, module: str) -> None:
        self.directory = directory
        self.module = module
        self.provided: dict[str, str] = {}
        self.declarations: dict[str, tuple[str, str, str, str]] = {}
        source_root = repo_root / directory / "src"
        self.definitions: dict[str, tuple[str, str, str, tuple[str, ...]]] = {}
        for source in sorted(source_root.rglob("*.cpp")):
            text = source.read_text(encoding="utf-8", errors="replace")
            for name in self.PS4_FUNCTION.findall(text):
                self.provided.setdefault(compute_nid(name), name)
            siblings = tuple(f"<{name}>" if bracket == "<" else f'"{name}"' for bracket, name in re.findall(r'^#include\s+([<"])([^>"]+)[>"]', text, re.MULTILINE))
            for match in self.DEFINITION.finditer(text):
                namespaces = [m.group(1) for m in self.NAMESPACE.finditer(text[:match.start()])]
                namespace = namespaces[-1] if namespaces else ""
                self.definitions.setdefault(match.group(2), (" ".join(match.group(1).split()), " ".join(match.group(3).split()), namespace, siblings))
        for header in sorted(source_root.rglob("*.h")):
            text = header.read_text(encoding="utf-8", errors="replace")
            relative = header.relative_to(source_root).as_posix()
            for match in self.DECLARATION.finditer(text):
                namespaces = [m.group(1) for m in self.NAMESPACE.finditer(text[:match.start()])]
                namespace = namespaces[-1] if namespaces else ""
                self.declarations.setdefault(match.group(2), (" ".join(match.group(1).split()), " ".join(match.group(3).split()), namespace, relative))

    def forwarder(self, name: str, nid: str = "") -> tuple[str, str, tuple[str, ...]] | None:
        declaration = self.declarations.get(name)
        forward = False
        if declaration is None:
            definition = self.definitions.get(name)
            if definition is None:
                return None
            result, params, namespace, headers = definition
            forward = True
        else:
            result, params, namespace, header = declaration
            headers = (f'"{header}"',)
        arguments: list[str] = []
        declared: list[str] = []
        type_words = {"void", "char", "short", "int", "long", "float", "double", "bool", "unsigned", "signed", "size_t", "uintptr_t", "intptr_t",
                      "u8", "u16", "u32", "u64", "s8", "s16", "s32", "s64", "f32", "f64", "VAddr", "PAddr", "const", "volatile"}
        if params and params != "void":
            for index, param in enumerate(params.split(",")):
                token = param.strip()
                suffix = ""
                if token.endswith("[]"):
                    token, suffix = token[:-2].strip(), "[]"
                words = re.findall("[A-Za-z_][A-Za-z0-9_]*", token)
                unnamed = len(words) == 1 or token.endswith("*") or token.endswith("&") or words[-1] in type_words
                if unnamed:
                    argument = f"p{index}"
                    declared.append(f"{token} {argument}{suffix}")
                else:
                    argument = words[-1]
                    declared.append(param.strip())
                arguments.append(argument)
        params = ", ".join(declared)
        qualified = (namespace + "::" if namespace else "") + name
        symbol = name + "_nid_postfix"
        text = ""
        if forward:
            text = (f"namespace {namespace} {{ {result} PS4_SYSV_ABI {name}({params}); }}" if namespace
                    else f"{result} PS4_SYSV_ABI {name}({params});")
        text += FORWARDER_SPLIT
        text += (f'extern "C" {result} PS4_SYSV_ABI {symbol}({params}) {{ static std::uint64_t calls = 0; '
                 f'OrbisHleTrace("{self.module}", "{name}", "{nid}", ++calls, __builtin_return_address(0)); '
                 f'return {qualified}({", ".join(arguments)}); }}')
        return symbol, text, headers


FORWARDER_SPLIT = "\n//FORWARDER//\n"


def sanitize(name: str) -> str | None:
    return name if IDENTIFIER.match(name) and len(name) <= 200 else None


def nid_identifier(nid: str) -> str:
    return "nid_" + "".join(c if c.isalnum() else ("p" if c == "+" else "m") for c in nid)


def load_registry(path: Path) -> list[dict]:
    entries = json.loads(path.read_text(encoding="utf-8"))
    if isinstance(entries, dict):
        entries = entries.get("entries") or entries.get("Entries") or []
    return entries


def collect(registry: list[dict], eboot: Eboot | None) -> tuple[list[str], dict[str, dict[str, dict]], list[str]]:
    requested: dict[str, str | None] = {}
    for entry in registry:
        symbol = entry.get("nid") or entry.get("Nid") or ""
        library = entry.get("library") or entry.get("Library") or None
        if symbol:
            requested.setdefault(symbol, library)
    if eboot is not None:
        for symbol in eboot.imports:
            requested.setdefault(symbol, None)
    files: list[str] = list(eboot.needed) if eboot is not None else []
    for library in requested.values():
        if library and library not in files:
            files.append(library)
    modules: dict[str, dict[str, dict]] = collections.OrderedDict((name, collections.OrderedDict()) for name in files)
    unresolved: list[str] = []
    for symbol, library in requested.items():
        nid, library_id, module_id = split_nid(symbol)
        target = library
        if target is None and eboot is not None and module_id is not None:
            target = eboot.module_file(module_id)
        if target is None:
            unresolved.append(symbol)
            target = "libkernel.prx" if "libkernel.prx" in modules else files[0]
        if target not in modules:
            modules[target] = collections.OrderedDict()
            files.append(target)
        record = modules[target].setdefault(nid, {"nid": nid, "symbols": [], "library": None})
        record["symbols"].append(symbol)
        if eboot is not None and library_id is not None:
            record["library"] = eboot.libraries.get(library_id)
    return files, modules, unresolved


def assign_names(nids: dict[str, dict], names: ShadPs4Names, prefix: str) -> None:
    used: set[str] = set()
    for nid, record in nids.items():
        if nid in names.objects:
            source, raw, kind = "shadps4-object", names.objects[nid][0], "object"
        elif nid in names.implemented:
            source, raw, kind = "shadps4-function", names.implemented[nid][0], "function"
        elif nid in names.names:
            source, raw, kind = "aerolib-name", names.names[nid], "function"
        else:
            source, raw, kind = "unknown", nid_identifier(nid), "function"
        if raw in KNOWN_OBJECTS or raw.startswith("_ZTI") or raw.startswith("_ZTV") or raw.startswith("_ZTS"):
            kind = "object"
        identifier = sanitize(raw) or nid_identifier(nid)
        candidate = prefix + identifier
        if candidate in used:
            candidate = prefix + identifier + "_" + nid_identifier(nid)
        used.add(candidate)
        record["name"] = candidate
        record["source"] = source
        record["display"] = raw
        record["kind"] = kind


def module_target(file_name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]", "_", file_name[:file_name.rfind(".")] if "." in file_name else file_name)


COMPLEMENT_IMPLEMENTATIONS: dict[str, str] = {
    "_FSin": "float ORBIS_SYSV_ABI _FSin_nid_postfix(float x, unsigned int quadrant) {\n"
             "    switch (quadrant & 3u) {\n        case 0: return std::sin(x);\n        case 1: return std::cos(x);\n"
             "        case 2: return -std::sin(x);\n        default: return -std::cos(x);\n    }\n}",
    "_FLog": "float ORBIS_SYSV_ABI _FLog_nid_postfix(float x, int base) {\n"
             "    return base == 0 ? std::log(x) : base == 1 ? std::log10(x) : std::log2(x);\n}",
    "_Log": "double ORBIS_SYSV_ABI _Log_nid_postfix(double x, int base) {\n"
            "    return base == 0 ? std::log(x) : base == 1 ? std::log10(x) : std::log2(x);\n}",
    "_Stoll": "long long ORBIS_SYSV_ABI _Stoll_nid_postfix(const char* text, char** end, int base) {\n"
              "    return std::strtoll(text, end, base);\n}",
    "_Stod": "double ORBIS_SYSV_ABI _Stod_nid_postfix(const char* text, char** end, long exponent) {\n"
             "    return std::strtod(text, end) * std::pow(10.0, static_cast<double>(exponent));\n}",
    "_Atomic_store_1": "void ORBIS_SYSV_ABI _Atomic_store_1_nid_postfix(volatile std::uint8_t* target, std::uint8_t value, int) {\n"
                       "    __atomic_store_n(target, value, __ATOMIC_SEQ_CST);\n}",
    "_Atomic_load_1": "std::uint8_t ORBIS_SYSV_ABI _Atomic_load_1_nid_postfix(volatile std::uint8_t* target, int) {\n"
                      "    return __atomic_load_n(target, __ATOMIC_SEQ_CST);\n}",
    "_Atomic_store_4": "void ORBIS_SYSV_ABI _Atomic_store_4_nid_postfix(volatile std::uint32_t* target, std::uint32_t value, int) {\n"
                       "    __atomic_store_n(target, value, __ATOMIC_SEQ_CST);\n}",
    "_Atomic_fetch_or_4": "std::uint32_t ORBIS_SYSV_ABI _Atomic_fetch_or_4_nid_postfix(volatile std::uint32_t* target, std::uint32_t value, int) {\n"
                          "    return __atomic_fetch_or(target, value, __ATOMIC_SEQ_CST);\n}",
    "_ZSt13_Syserror_mapi": "const char* ORBIS_SYSV_ABI orbis_Syserror_map(int) {\n    return \"unknown error\";\n}",
    "__cxa_call_unexpected": "void ORBIS_SYSV_ABI __cxa_call_unexpected_nid_postfix(void*) {\n"
                             "    std::fputs(\"[HLE] __cxa_call_unexpected\\n\", stderr);\n    std::abort();\n}",
    "abort": "void ORBIS_SYSV_ABI abort_nid_postfix() {\n    std::fputs(\"[HLE] guest abort()\\n\", stderr);\n    std::fflush(stderr);\n    std::abort();\n}",
    "sinhf": "float ORBIS_SYSV_ABI sinhf_nid_postfix(float x) {\n    return std::sinh(x);\n}",
    "_ZNSt8ios_base4InitC1Ev": "void ORBIS_SYSV_ABI orbis_ios_base_Init_ctor(void*) {\n}",
    "_ZNSt8ios_base4InitD1Ev": "void ORBIS_SYSV_ABI orbis_ios_base_Init_dtor(void*) {\n}",
    "_ZNSt6_WinitC1Ev": "void ORBIS_SYSV_ABI orbis_Winit_ctor(void*) {\n}",
    "_ZNSt6_WinitD1Ev": "void ORBIS_SYSV_ABI orbis_Winit_dtor(void*) {\n}",
    "__stack_chk_fail": "void ORBIS_SYSV_ABI __stack_chk_fail_nid_postfix() {\n"
                        "    std::fputs(\"[HLE] stack smashing detected\\n\", stderr);\n    std::fflush(stderr);\n    std::abort();\n}",
    "stack_chk_fail": "void ORBIS_SYSV_ABI __stack_chk_fail_nid_postfix() {\n"
                      "    std::fputs(\"[HLE] stack smashing detected\\n\", stderr);\n    std::fflush(stderr);\n    std::abort();\n}",
    "g_stack_chk_guard": "std::uintptr_t __stack_chk_guard_nid_postfix = 0x595e9fbd94fda766ull;",
    "__stack_chk_guard": "std::uintptr_t __stack_chk_guard_nid_postfix = 0x595e9fbd94fda766ull;",
    "_ZTISt9bad_alloc": "void* orbis_typeinfo_bad_alloc[4] __asm__(\"_ZTISt9bad_alloc\") __attribute__((weak)) = {};",
    "_ZTVSt12bad_weak_ptr": "void* orbis_vtable_bad_weak_ptr[8] __asm__(\"_ZTVSt12bad_weak_ptr\") __attribute__((weak)) = {};",
    "_ZSt7nothrow": "std::uint8_t orbis_nothrow[8] __asm__(\"_ZSt7nothrow\") __attribute__((weak)) = {};",
}


def complement_symbol(record: dict) -> str:
    display = record["display"]
    return display if display.startswith("_Z") else display + "_nid_postfix"


def write_complement(out: Path, file_name: str, missing: dict[str, dict], forwarders: list[tuple[str, str, str]] | None = None) -> list[str]:
    stem = module_target(file_name)
    directory = out / stem
    directory.mkdir(parents=True, exist_ok=True)
    lines = ["#include <common/OrbisHle.hpp>", "#include <cmath>", "#include <cstdio>", "#include <cstdlib>"]
    forwarders = forwarders or []
    seen: list[str] = []
    for _, _, headers in forwarders:
        for header in headers:
            if header not in seen:
                seen.append(header)
    lines += [f"#include {header}" for header in seen]
    if forwarders:
        lines.append("")
    lines += [text.split(FORWARDER_SPLIT)[0] for _, text, _ in forwarders if text.split(FORWARDER_SPLIT)[0]]
    for namespace in sorted({text.split("return ")[1].rsplit("::", 1)[0] for _, text, _ in forwarders if "::" in text.split("return ")[1]}):
        lines.append(f"using namespace {namespace};")
    lines += ["", 'extern "C" {', ""]
    for _, text, _ in forwarders:
        lines.append(text.split(FORWARDER_SPLIT)[-1])
    if forwarders:
        lines.append("")
    explicit: list[str] = []
    providers: list[str] = []
    for nid, record in missing.items():
        text = COMPLEMENT_IMPLEMENTATIONS.get(record["display"])
        if text is not None:
            head = re.split("[(=]", text.split(chr(10))[0], maxsplit=1)[0]
            symbol = re.findall("[A-Za-z_][A-Za-z0-9_]*", head.split("[")[0])[-1]
            lines.append(text)
            providers.append("complement-implementation")
        else:
            symbol = record["name"] if record["source"] == "unknown" else complement_symbol(record)
            if record["kind"] == "object":
                lines.append(f"std::uint64_t {symbol}[8] = {{}};")
            else:
                lines.append(f'ORBIS_HLE_STUB("{file_name}", {symbol}, "{record["display"]}", "{nid}")')
            providers.append("complement-stub")
        natural = symbol[:-len("_nid_postfix")] if symbol.endswith("_nid_postfix") else symbol
        if compute_nid(natural) != nid and "__asm__(" not in (text or ""):
            explicit.append(f'APS5_EXPORT_NID("{nid}", {symbol});')
        lines.append("")
    lines += explicit + ["", "}", ""]
    (directory / f"{stem}_orbis.cpp").write_text("\n".join(lines), encoding="utf-8", newline="\n")
    return providers


STUB_IMPLEMENTATIONS: dict[str, tuple[str, str, str]] = {
    "sceUserServiceGetInitialUser": ("std::int32_t", "std::int32_t* userId", "if (userId != nullptr) *userId = 1; return 0;"),
    "sceUserServiceGetEvent": ("std::int32_t", "std::uint32_t* event",
                               "static bool delivered = false; if (event == nullptr) return static_cast<std::int32_t>(0x80960002); if (delivered) return static_cast<std::int32_t>(0x80960007); delivered = true; event[0] = 0; event[1] = 1; return 0;"),
    "sceUserServiceGetLoginUserIdList": ("std::int32_t", "std::int32_t* list", "if (list != nullptr) { list[0] = 1; list[1] = -1; list[2] = -1; list[3] = -1; } return 0;"),
    "sceUserServiceGetUserName": ("std::int32_t", "std::int32_t, char* name, std::size_t size",
                                  "if (name != nullptr && size != 0) { const char* text = \"Player\"; std::size_t index = 0; while (text[index] != 0 && index + 1 < size) { name[index] = text[index]; ++index; } name[index] = 0; } return 0;"),
    "scePlayGoOpen": ("std::int32_t", "std::int32_t* handle, const void*", "if (handle != nullptr) *handle = 1; return 0;"),
    "sceNpTrophyCreateContext": ("std::int32_t", "std::int32_t* context, std::int32_t, std::uint32_t, std::uint64_t", "if (context != nullptr) *context = 1; return 0;"),
    "sceNpTrophyCreateHandle": ("std::int32_t", "std::int32_t* handle", "if (handle != nullptr) *handle = 1; return 0;"),
    "sceNpTrophyRegisterContext": ("std::int32_t", "std::int32_t, std::int32_t, std::uint64_t", "return 0;"),
    "sceNpTrophyDestroyContext": ("std::int32_t", "std::int32_t", "return 0;"),
    "sceNpTrophyDestroyHandle": ("std::int32_t", "std::int32_t", "return 0;"),
    "sceNpTrophyAbortHandle": ("std::int32_t", "std::int32_t", "return 0;"),
    "sceNpTrophyGetGameInfo": ("std::int32_t", "std::int32_t, std::int32_t, std::uint8_t* details, std::uint8_t* data",
                               "if (details != nullptr) { const std::size_t size = *reinterpret_cast<const std::size_t*>(details); if (size != 0 && size <= 4096) { std::memset(details + 8, 0, size - 8); } } if (data != nullptr) { const std::size_t size = *reinterpret_cast<const std::size_t*>(data); if (size != 0 && size <= 4096) { std::memset(data + 8, 0, size - 8); } } return 0;"),
    "sceNpTrophyGetTrophyUnlockState": ("std::int32_t", "std::int32_t, std::int32_t, std::uint8_t* flags, std::uint32_t* count", "if (flags != nullptr) std::memset(flags, 0, 16); if (count != nullptr) *count = 0; return 0;"),
    "sceNpTrophyUnlockTrophy": ("std::int32_t", "std::int32_t, std::int32_t, std::int32_t, std::int32_t* platinumId", "if (platinumId != nullptr) *platinumId = -1; return 0;"),
    "sceSaveDataDirNameSearch": ("std::int32_t", "const void*, std::uint32_t* result", "if (result != nullptr) { result[0] = 0; result[5] = 0; } return 0;"),
    "sceSystemServiceParamGetInt": ("std::int32_t", "std::int32_t paramId, std::int32_t* value", "if (value == nullptr) return static_cast<std::int32_t>(0x80A10003); switch (paramId) { case 1: *value = 1; break; case 2: *value = 2; break; case 3: *value = 0; break; case 4: *value = -360; break; case 5: *value = 0; break; case 6: *value = 0; break; case 7: *value = 0; break; case 1000: *value = 1; break; default: *value = 0; break; } return 0;"),
    "sceAppContentAppParamGetInt": ("std::int32_t", "std::uint32_t paramId, std::int32_t* value", "if (value == nullptr) return static_cast<std::int32_t>(0x80D90002); *value = paramId == 0 ? 3 : 0; return 0;"),
    "sceAppContentGetAddcontInfoList": ("std::int32_t", "std::uint32_t, void*, std::uint32_t, std::uint32_t* hitNum", "if (hitNum != nullptr) *hitNum = 0; return 0;"),
    "scePlayGoGetLocus": ("std::int32_t", "std::int32_t, const std::uint16_t*, std::uint32_t count, std::int8_t* loci",
                          "if (loci == nullptr) return static_cast<std::int32_t>(0x80B20002); for (std::uint32_t k = 0; k < count; ++k) loci[k] = 3; return 0;"),
    "scePlayGoGetProgress": ("std::int32_t", "std::int32_t, const std::uint16_t*, std::uint32_t, std::uint64_t* progress",
                             "if (progress != nullptr) { progress[0] = 1; progress[1] = 1; } return 0;"),
    "scePlayGoGetToDoList": ("std::int32_t", "std::int32_t, void*, std::uint32_t, std::uint32_t* entries", "if (entries != nullptr) *entries = 0; return 0;"),
}


def implemented_stub(file_name: str, record: dict, nid: str) -> str:
    result, params, body = STUB_IMPLEMENTATIONS[record["display"]]
    return (f'ORBIS_SYSV_ABI {result} {record["name"]}({params}) {{ static std::uint64_t calls = 0; '
            f'OrbisHleTrace("{file_name}", "{record["display"]}", "{nid}", ++calls, __builtin_return_address(0)); {body} }}')


STUB_EXTERNAL: dict[str, str] = {
    "sceNgs2ParseWaveformData": "orbis_sceNgs2ParseWaveformData",
    "sceNgs2SystemCreateWithAllocator": "orbis_sceNgs2SystemCreateWithAllocator",
    "sceNgs2RackCreateWithAllocator": "orbis_sceNgs2RackCreateWithAllocator",
    "sceNgs2RackGetVoiceHandle": "orbis_sceNgs2RackGetVoiceHandle",
    "sceNgs2VoiceGetState": "orbis_sceNgs2VoiceGetState",
    "sceNgs2VoiceGetStateFlags": "orbis_sceNgs2VoiceGetStateFlags",
    "sceVideoOutOpen": "orbis_sceVideoOutOpen",
    "sceVideoOutClose": "orbis_sceVideoOutClose",
    "sceVideoOutSetBufferAttribute": "orbis_sceVideoOutSetBufferAttribute",
    "sceVideoOutRegisterBuffers": "orbis_sceVideoOutRegisterBuffers",
    "sceVideoOutRegisterStereoBuffers": "orbis_sceVideoOutRegisterStereoBuffers",
    "sceVideoOutUnregisterBuffers": "orbis_sceVideoOutUnregisterBuffers",
    "sceVideoOutSetFlipRate": "orbis_sceVideoOutSetFlipRate",
    "sceVideoOutGetFlipStatus": "orbis_sceVideoOutGetFlipStatus",
    "sceVideoOutIsFlipPending": "orbis_sceVideoOutIsFlipPending",
    "sceVideoOutSubmitFlip": "orbis_sceVideoOutSubmitFlip",
    "sceVideoOutAddFlipEvent": "orbis_sceVideoOutAddFlipEvent",
    "sceVideoOutGetResolutionStatus": "orbis_sceVideoOutGetResolutionStatus",
    "sceVideoOutGetDeviceCapabilityInfo": "orbis_sceVideoOutGetDeviceCapabilityInfo",
    "sceVideoOutConfigureOutputMode_": "orbis_sceVideoOutConfigureOutputMode_",
    "sceVideoOutModeSetAny_": "orbis_sceVideoOutModeSetAny_",
    "sceVideoOutGetVblankStatus": "orbis_sceVideoOutGetVblankStatus",
    "sceAudioOutInit": "orbis_sceAudioOutInit",
    "sceAudioOutOpen": "orbis_sceAudioOutOpen",
    "sceAudioOutClose": "orbis_sceAudioOutClose",
    "sceAudioOutOutput": "orbis_sceAudioOutOutput",
    "sceAudioOutOutputs": "orbis_sceAudioOutOutputs",
    "sceAudioOutSetVolume": "orbis_sceAudioOutSetVolume",
    "sceAudioOutGetPortState": "orbis_sceAudioOutGetPortState",
    "sceAudioOutSetMixLevelPadSpk": "orbis_sceAudioOutSetMixLevelPadSpk",
    "sceSaveDataDialogInitialize": "orbis_sceSaveDataDialogInitialize",
    "sceSaveDataDialogTerminate": "orbis_sceSaveDataDialogTerminate",
    "sceSaveDataDialogOpen": "orbis_sceSaveDataDialogOpen",
    "sceSaveDataDialogUpdateStatus": "orbis_sceSaveDataDialogUpdateStatus",
    "sceSaveDataDialogGetStatus": "orbis_sceSaveDataDialogGetStatus",
    "sceSaveDataDialogIsReadyToDisplay": "orbis_sceSaveDataDialogIsReadyToDisplay",
    "sceSaveDataDialogGetResult": "orbis_sceSaveDataDialogGetResult",
    "sceSaveDataDialogClose": "orbis_sceSaveDataDialogClose",
    "sceSaveDataDialogProgressBarSetValue": "orbis_sceSaveDataDialogProgressBarSetValue",
    "scePadInit": "orbis_scePadInit",
    "scePadOpen": "orbis_scePadOpen",
    "scePadClose": "orbis_scePadClose",
    "scePadReadState": "orbis_scePadReadState",
    "scePadGetControllerInformation": "orbis_scePadGetControllerInformation",
    "scePadSetLightBar": "orbis_scePadSetLightBar",
    "scePadSetMotionSensorState": "orbis_scePadSetMotionSensorState",
    "scePadSetVibration": "orbis_scePadSetVibration",
    "scePadResetOrientation": "orbis_scePadResetOrientation",
}

EXTERNAL_LINK_LIBRARIES: dict[str, list[str]] = {
    "libSceVideoOut": ["user32", "gdi32"],
    "libSceAudioOut": ["winmm"],
    "libScePad": ["user32"],
}

IMPLEMENTATION_DIR = Path(__file__).resolve().parent / "orbis_impl"

STUB_RETURNS: dict[str, str] = {
    "sceSystemServiceReceiveEvent": "0x80A10004",
}


def write_stub_module(out: Path, file_name: str, nids: dict[str, dict]) -> None:
    stem = module_target(file_name)
    directory = out / stem
    directory.mkdir(parents=True, exist_ok=True)
    definitions = [f"LIBRARY {file_name}", "EXPORTS"]
    lines = ["#include <common/OrbisHle.hpp>", "#include <cstddef>", "#include <cstring>", "", 'extern "C" {', ""]
    for nid, record in nids.items():
        if record["kind"] == "object":
            lines.append(f'ORBIS_HLE_OBJECT({record["name"]})')
            definitions.append(f'    "{nid}" = {record["name"]} DATA')
        elif record["display"] in STUB_IMPLEMENTATIONS:
            lines.append(implemented_stub(file_name, record, nid))
            definitions.append(f'    "{nid}" = {record["name"]}')
        elif record["display"] in STUB_EXTERNAL and (IMPLEMENTATION_DIR / f"{stem}.cpp").exists():
            definitions.append(f'    "{nid}" = {STUB_EXTERNAL[record["display"]]}')
        elif record["display"] in STUB_RETURNS:
            lines.append(f'ORBIS_HLE_STUB_RETURNING("{file_name}", {record["name"]}, "{record["display"]}", "{nid}", {STUB_RETURNS[record["display"]]})')
            definitions.append(f'    "{nid}" = {record["name"]}')
        else:
            lines.append(f'ORBIS_HLE_STUB("{file_name}", {record["name"]}, "{record["display"]}", "{nid}")')
            definitions.append(f'    "{nid}" = {record["name"]}')
    lines += ["", "}", ""]
    (directory / f"{stem}.cpp").write_text("\n".join(lines), encoding="utf-8", newline="\n")
    (directory / f"{stem}.def").write_text("\n".join(definitions) + "\n", encoding="utf-8", newline="\n")


COMMON_HEADER = """#ifndef CORE_LIBS_ORBIS_COMMON_ORBISHLE_HPP
#define CORE_LIBS_ORBIS_COMMON_ORBISHLE_HPP

#include <cstdint>

#define ORBIS_SYSV_ABI __attribute__((sysv_abi))

extern "C" void OrbisHleTrace(const char* module, const char* function, const char* nid, std::uint64_t calls, const void* site);

#define ORBIS_HLE_STUB(module, identifier, display, nid) \\
    ORBIS_SYSV_ABI std::int64_t identifier() { \\
        static std::uint64_t calls = 0; \\
        OrbisHleTrace(module, display, nid, ++calls, __builtin_return_address(0)); \\
        return 0; \\
    }

#define ORBIS_HLE_OBJECT(identifier) \\
    std::uint64_t identifier[8] = {};

#define ORBIS_HLE_STUB_RETURNING(module, identifier, display, nid, value) \\
    ORBIS_SYSV_ABI std::int64_t identifier() { \\
        static std::uint64_t calls = 0; \\
        OrbisHleTrace(module, display, nid, ++calls, __builtin_return_address(0)); \\
        return static_cast<std::int64_t>(value); \\
    }

#define APS5_EXPORT_NID(exportName, funcName) \\
    __asm__(".globl \\"" exportName "_nid_no_patch_cut\\"\\n\\t" \\
    ".set \\"" exportName "_nid_no_patch_cut\\", " #funcName)

#endif
"""

COMMON_SOURCE = """#include <common/OrbisHle.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>

namespace {

bool traceEveryCall() {
    static const bool enabled = [] {
        const char* value = std::getenv("ORBIS_HLE_TRACE");
        return value != nullptr && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

}

extern "C" void OrbisHleTrace(const char* module, const char* function, const char* nid, const std::uint64_t calls, const void* site) {
    if (calls != 1 && !traceEveryCall()) return;
    const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto address = reinterpret_cast<std::uintptr_t>(site);
    const long long offset = image != 0 && address > image ? static_cast<long long>(address - image - 0x10000) : -1;
    std::fprintf(stderr, "[HLE] %s: %s (NID %s) call #%llu thread %lu from 0x%llx\\n", module, function, nid, static_cast<unsigned long long>(calls),
                 static_cast<unsigned long>(GetCurrentThreadId()), static_cast<unsigned long long>(offset));
    std::fflush(stderr);
}
"""

QUOTE = chr(34)

NID_PATCHER_SOURCES = ["Sha1", "NidCompute", "NidResolver", "ExportExclusions", "ElfNidPatcher", "PeNidPatcher", "MachONidPatcher",
                       "BinaryPatcherFactory", "main"]

REAL_LINK = {
    "libc": ["advapi32", "ws2_32", "iphlpapi"],
    "libkernel": ["orbis_libc", "ws2_32", "shell32"],
}
REAL_DEFINITIONS = {
    "libkernel": ["_WIN32_WINNT=0x0602"],
}


def write_cmake(out: Path, files: list[str], real: dict[str, ProsperoLibrary], vendored: dict[str, "VendoredLibrary"] | None = None) -> None:
    vendored = vendored or {}
    lines = [
        "set(ANYPS4_LIBS_DIR \"${CMAKE_BINARY_DIR}/libs\" CACHE PATH \"Directory that receives the generated Orbis .prx libraries\")",
        "set(ORBIS_PROSPERO_LIBS ${CMAKE_CURRENT_SOURCE_DIR}/..)",
        "set(ORBIS_UNPATCHED_DIR ${CMAKE_CURRENT_BINARY_DIR}/unpatched)",
        "",
        "add_library(orbis_hle_common OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/common/OrbisHle.cpp)",
        "target_include_directories(orbis_hle_common PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})",
        "target_compile_features(orbis_hle_common PRIVATE cxx_std_20)",
        "set_target_properties(orbis_hle_common PROPERTIES POSITION_INDEPENDENT_CODE ON)",
        "",
        "set(ORBIS_PRX_OUTPUTS)",
    ]
    if real:
        lines += ["add_executable(orbis_nid_patcher"]
        lines += [f"        ${{ORBIS_PROSPERO_LIBS}}/nid/src/{name}.cpp" for name in NID_PATCHER_SOURCES]
        lines += [
            ")",
            "target_include_directories(orbis_nid_patcher PRIVATE ${ORBIS_PROSPERO_LIBS}/nid/include)",
            "target_compile_features(orbis_nid_patcher PRIVATE cxx_std_20)",
            "if(MINGW)",
            "    target_link_options(orbis_nid_patcher PRIVATE -static -static-libgcc -static-libstdc++)",
            "endif()",
            "",
        ]
        for file_name, library in real.items():
            stem = module_target(file_name)
            output = file_name[:file_name.rfind(".")] if "." in file_name else file_name
            suffix = file_name[file_name.rfind("."):] if "." in file_name else ".prx"
            lines.append(f"set(ORBIS_{stem.upper()}_UNPATCHED ${{ORBIS_UNPATCHED_DIR}}/{output}{suffix})")
            lines.append(f"set(ORBIS_{stem.upper()}_PATCHED ${{ANYPS4_LIBS_DIR}}/{output}{suffix})")
        lines.append("")
    for file_name in files:
        if file_name in vendored:
            continue
        stem = module_target(file_name)
        output = file_name[:file_name.rfind(".")] if "." in file_name else file_name
        suffix = file_name[file_name.rfind("."):] if "." in file_name else ".prx"
        library = real.get(file_name)
        if library is None:
            external = [f"        ${{CMAKE_SOURCE_DIR}}/tools/orbis_impl/{stem}.cpp"] if (IMPLEMENTATION_DIR / f"{stem}.cpp").exists() else []
            lines += [
                f"add_library(orbis_{stem} SHARED",
                f"        ${{CMAKE_CURRENT_SOURCE_DIR}}/{stem}/{stem}.cpp",
                f"        ${{CMAKE_CURRENT_SOURCE_DIR}}/{stem}/{stem}.def",
            ] + external + [
                "        $<TARGET_OBJECTS:orbis_hle_common>)",
                f"target_include_directories(orbis_{stem} PRIVATE ${{CMAKE_CURRENT_SOURCE_DIR}})",
                f"target_compile_features(orbis_{stem} PRIVATE cxx_std_20)",
                f"set_target_properties(orbis_{stem} PROPERTIES",
                "        PREFIX \"\"",
                f"        OUTPUT_NAME {output}",
                f"        SUFFIX {suffix}",
                "        RUNTIME_OUTPUT_DIRECTORY ${ANYPS4_LIBS_DIR}",
                "        LIBRARY_OUTPUT_DIRECTORY ${ANYPS4_LIBS_DIR}",
                "        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/implib)",
                "if(MINGW)",
                f"    target_link_options(orbis_{stem} PRIVATE -static -static-libgcc -static-libstdc++)",
                f"    target_compile_options(orbis_{stem} PRIVATE -fno-exceptions)",
                "endif()",
            ] + ([f"target_link_libraries(orbis_{stem} PRIVATE {' '.join(EXTERNAL_LINK_LIBRARIES[stem])})"] if stem in EXTERNAL_LINK_LIBRARIES else []) + [
                f"list(APPEND ORBIS_PRX_OUTPUTS orbis_{stem})",
                "",
            ]
            continue
        lines += [f"add_library(orbis_{stem} SHARED"]
        lines += [f"        ${{ORBIS_PROSPERO_LIBS}}/{source}" for source in library.sources]
        lines += [
            f"        ${{CMAKE_CURRENT_SOURCE_DIR}}/{stem}/{stem}_orbis.cpp",
            "        $<TARGET_OBJECTS:orbis_hle_common>)",
            f"target_include_directories(orbis_{stem} PRIVATE ${{CMAKE_CURRENT_SOURCE_DIR}} ${{ORBIS_PROSPERO_LIBS}} ${{ORBIS_PROSPERO_LIBS}}/nid/include)",
            f"target_compile_features(orbis_{stem} PRIVATE cxx_std_20)",
        ]
        if REAL_DEFINITIONS.get(library.stem):
            lines.append(f"target_compile_definitions(orbis_{stem} PRIVATE {' '.join(REAL_DEFINITIONS[library.stem])})")
        if REAL_LINK.get(library.stem):
            lines.append(f"target_link_libraries(orbis_{stem} PRIVATE {' '.join(REAL_LINK[library.stem])} ${{CMAKE_DL_LIBS}})")
        lines += [
            f"set_target_properties(orbis_{stem} PROPERTIES",
            "        PREFIX \"\"",
            f"        OUTPUT_NAME {output}",
            f"        SUFFIX {suffix}",
            "        RUNTIME_OUTPUT_DIRECTORY ${ORBIS_UNPATCHED_DIR}",
            "        LIBRARY_OUTPUT_DIRECTORY ${ORBIS_UNPATCHED_DIR}",
            "        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/implib)",
            "if(MINGW)",
            f"    target_compile_options(orbis_{stem} PRIVATE -fno-asynchronous-unwind-tables)",
            f"    add_custom_command(TARGET orbis_{stem} POST_BUILD",
            f"            COMMAND \"${{CMAKE_OBJCOPY}}\" --rename-section .eh_frame=.ehfram \"$<TARGET_FILE:orbis_{stem}>\"",
            "            VERBATIM)",
            "endif()",
            f"add_custom_command(OUTPUT ${{ORBIS_{stem.upper()}_PATCHED}}",
            f"        COMMAND ${{CMAKE_COMMAND}} -E copy ${{ORBIS_{stem.upper()}_UNPATCHED}} ${{ORBIS_{stem.upper()}_PATCHED}}",
            (f"        COMMAND $<TARGET_FILE:orbis_nid_patcher> {library.stem} ${{ORBIS_{stem.upper()}_PATCHED}}" if library.stem == "libc"
             else f"        COMMAND $<TARGET_FILE:orbis_nid_patcher> {library.stem} --preserve-exports ${{ORBIS_LIBC_UNPATCHED}} ${{ORBIS_{stem.upper()}_PATCHED}}"),
            f"        DEPENDS orbis_nid_patcher orbis_{stem} ${{ORBIS_{stem.upper()}_UNPATCHED}}" + ("" if library.stem == "libc" else " ${ORBIS_LIBC_UNPATCHED}"),
            f"        COMMENT \"Patching NIDs for {output}\")",
            f"add_custom_target(orbis_{stem}_patched DEPENDS ${{ORBIS_{stem.upper()}_PATCHED}})",
            f"list(APPEND ORBIS_PRX_OUTPUTS orbis_{stem}_patched)",
            "",
        ]
    for file_name, library in vendored.items():
        stem = module_target(file_name)
        output = file_name[:file_name.rfind(".")] if "." in file_name else file_name
        suffix = file_name[file_name.rfind("."):] if "." in file_name else ".prx"
        alias = library.module + suffix if library.module != output else None
        upper = stem.upper()
        lines += [
            f"add_library(orbis_{stem}_complement OBJECT ${{CMAKE_CURRENT_SOURCE_DIR}}/{stem}/{stem}_orbis.cpp)",
            f"target_include_directories(orbis_{stem}_complement PRIVATE ${{CMAKE_CURRENT_SOURCE_DIR}} ${{CMAKE_SOURCE_DIR}}/{library.directory}/shims ${{CMAKE_SOURCE_DIR}}/{library.directory}/src ${{CMAKE_SOURCE_DIR}}/{library.directory}/externals/fmt/include ${{CMAKE_SOURCE_DIR}}/{library.directory}/externals/magic_enum/include ${{ORBIS_PROSPERO_LIBS}})",
            f"target_compile_definitions(orbis_{stem}_complement PRIVATE FMT_HEADER_ONLY=1 ANYPS4_HEADLESS=1 NOMINMAX WIN32_LEAN_AND_MEAN)",
            f"target_compile_features(orbis_{stem}_complement PRIVATE cxx_std_23)",
            f"if(EXISTS ${{CMAKE_SOURCE_DIR}}/{library.directory}/complement.cmake)",
            f"    include(${{CMAKE_SOURCE_DIR}}/{library.directory}/complement.cmake)",
            "endif()",
            f"set_target_properties(orbis_{stem}_complement PROPERTIES POSITION_INDEPENDENT_CODE ON)",
            f"set(ORBIS_{upper}_COMPLEMENT $<TARGET_OBJECTS:orbis_{stem}_complement>)",
            f"add_subdirectory(${{CMAKE_SOURCE_DIR}}/{library.directory} ${{CMAKE_BINARY_DIR}}/vendored/{stem})",
            f"set(ORBIS_{upper}_UNPATCHED ${{ORBIS_UNPATCHED_DIR}}/{output}{suffix})",
            f"set(ORBIS_{upper}_PATCHED ${{ANYPS4_LIBS_DIR}}/{output}{suffix})",
            f"add_custom_command(OUTPUT ${{ORBIS_{upper}_PATCHED}}" + (f" ${{ANYPS4_LIBS_DIR}}/{alias}" if alias else ""),
            f"        COMMAND ${{CMAKE_COMMAND}} -E copy ${{ORBIS_{upper}_UNPATCHED}} ${{ORBIS_{upper}_PATCHED}}",
            f"        COMMAND $<TARGET_FILE:orbis_nid_patcher> {library.module} --preserve-exports ${{ORBIS_LIBC_UNPATCHED}} ${{ORBIS_{upper}_PATCHED}}",
        ]
        if alias:
            lines.append(f"        COMMAND ${{CMAKE_COMMAND}} -E copy ${{ORBIS_{upper}_PATCHED}} ${{ANYPS4_LIBS_DIR}}/{alias}")
        lines += [
            f"        DEPENDS orbis_nid_patcher orbis_{stem} ${{ORBIS_{upper}_UNPATCHED}} ${{ORBIS_LIBC_UNPATCHED}}",
            f"        COMMENT " + QUOTE + f"Patching NIDs for {output}" + QUOTE + ")",
            f"add_custom_target(orbis_{stem}_patched DEPENDS ${{ORBIS_{upper}_PATCHED}})",
            f"list(APPEND ORBIS_PRX_OUTPUTS orbis_{stem}_patched)",
            "",
        ]
    if real:
        lines += [
            "if(MINGW)",
            "    get_filename_component(ORBIS_RUNTIME_DIR \"${CMAKE_CXX_COMPILER}\" DIRECTORY)",
            "    foreach(runtimeName IN ITEMS libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)",
            "        if(NOT EXISTS \"${ORBIS_RUNTIME_DIR}/${runtimeName}\")",
            "            message(FATAL_ERROR \"Cannot locate MinGW runtime: ${runtimeName}\")",
            "        endif()",
            "        add_custom_command(OUTPUT ${ANYPS4_LIBS_DIR}/${runtimeName}",
            "                COMMAND ${CMAKE_COMMAND} -E copy \"${ORBIS_RUNTIME_DIR}/${runtimeName}\" ${ANYPS4_LIBS_DIR}/${runtimeName}",
            "                DEPENDS \"${ORBIS_RUNTIME_DIR}/${runtimeName}\")",
            "        list(APPEND ORBIS_RUNTIME_OUTPUTS ${ANYPS4_LIBS_DIR}/${runtimeName})",
            "    endforeach()",
            "    add_custom_target(orbis_runtime DEPENDS ${ORBIS_RUNTIME_OUTPUTS})",
            "    list(APPEND ORBIS_PRX_OUTPUTS orbis_runtime)",
            "endif()",
            "",
        ]
    lines += ["add_custom_target(orbis_libs DEPENDS ${ORBIS_PRX_OUTPUTS})", ""]
    (out / "CMakeLists.txt").write_text("\n".join(lines), encoding="utf-8", newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--registry", required=True, type=Path, help="<output>.registry.json written by relinker --registry")
    parser.add_argument("--eboot", type=Path, help="original eboot.bin (plain ELF or fake SELF) for DT_NEEDED and module ids")
    parser.add_argument("--shadps4", type=Path, help="shadPS4 checkout (repository root or src/core) used to name functions")
    parser.add_argument("--real", default="libc.prx,libkernel.prx", help="comma-separated modules built from core/libs/prx (empty for stubs only)")
    parser.add_argument("--vendored", action="append", default=[], metavar="FILE=DIR[:MODULE]",
                        help="module built from a vendored source tree with PS4_SYSV_ABI functions, e.g. libSceGnmDriver_padebug.prx=3rdparty/shadps4-gnm:libSceGnmDriver")
    parser.add_argument("--out", type=Path, default=Path(__file__).resolve().parent.parent / "core" / "libs" / "orbis")
    args = parser.parse_args()

    registry = load_registry(args.registry)
    eboot = Eboot(args.eboot) if args.eboot else None
    names = ShadPs4Names(args.shadps4)
    libs_root = args.out.resolve().parent
    real_names = [name for name in args.real.split(",") if name]
    real: dict[str, ProsperoLibrary] = {}

    files, modules, unresolved = collect(registry, eboot)
    for file_name in files:
        if file_name in real_names:
            stem = file_name[:file_name.rfind(".")]
            if (libs_root / "prx" / stem / "CMakeLists.txt").is_file():
                real[file_name] = ProsperoLibrary(libs_root, stem)
    if "libkernel.prx" in real and "libc.prx" not in real:
        raise SystemExit("libkernel.prx needs libc.prx in --real")
    vendored: dict[str, VendoredLibrary] = {}
    for spec in args.vendored:
        file_name, _, location = spec.partition("=")
        directory, _, module = location.partition(":")
        if not file_name or not directory:
            raise SystemExit("--vendored expects FILE=DIR[:MODULE]: " + spec)
        if file_name not in modules:
            modules[file_name] = collections.OrderedDict()
            files.append(file_name)
        vendored[file_name] = VendoredLibrary(libs_root.parent.parent, directory, module or file_name[:file_name.rfind(".")])
    if vendored and "libc.prx" not in real:
        raise SystemExit("vendored modules need libc.prx in --real")
    for file_name, nids in modules.items():
        assign_names(nids, names, "" if file_name in real or file_name in vendored else "orbis_")

    out = args.out
    (out / "common").mkdir(parents=True, exist_ok=True)
    (out / "common" / "OrbisHle.hpp").write_text(COMMON_HEADER, encoding="utf-8", newline="\n")
    (out / "common" / "OrbisHle.cpp").write_text(COMMON_SOURCE, encoding="utf-8", newline="\n")
    summary: dict[str, dict[str, int]] = {}
    for file_name in files:
        library = real.get(file_name) or vendored.get(file_name)
        if library is None:
            write_stub_module(out, file_name, modules[file_name])
            for record in modules[file_name].values():
                record["provider"] = "stub"
            continue
        origin = "prospero:" if file_name in real else "vendored:"
        missing = collections.OrderedDict((nid, record) for nid, record in modules[file_name].items() if nid not in library.provided)
        for nid, record in modules[file_name].items():
            record["provider"] = origin + library.provided[nid] if nid in library.provided else "complement"
        forwarders: list[tuple[str, str, str]] = []
        if file_name in vendored:
            for nid, record in modules[file_name].items():
                if nid not in library.provided:
                    continue
                forwarder = vendored[file_name].forwarder(library.provided[nid], nid)
                if forwarder is None:
                    raise SystemExit(f"{file_name}: no declaration found for {library.provided[nid]}; cannot export it with C linkage")
                forwarders.append(forwarder)
        providers = write_complement(out, file_name, missing, forwarders)
        for record, provider in zip(missing.values(), providers):
            record["provider"] = provider
        summary[file_name] = {"prospero": len(modules[file_name]) - len(missing), "complement": len(missing)}
    write_cmake(out, files, real, vendored)

    manifest = {
        "registry": str(args.registry),
        "eboot": str(args.eboot) if args.eboot else None,
        "real": sorted(real),
        "vendored": {name: library.directory for name, library in vendored.items()},
        "unresolvedModules": unresolved,
        "modules": {
            file_name: [
                {"nid": record["nid"], "name": record["name"], "display": record["display"], "source": record["source"], "kind": record["kind"],
                 "provider": record["provider"], "library": record["library"], "symbols": record["symbols"]}
                for record in nids.values()
            ]
            for file_name, nids in modules.items()
        },
    }
    (out / "stubs.json").write_text(json.dumps(manifest, indent=1), encoding="utf-8", newline="\n")

    total = sum(len(n) for n in modules.values())
    sources = collections.Counter(record["source"] for nids in modules.values() for record in nids.values())
    print(f"registry entries: {len(registry)}; unique NIDs: {total}; modules: {len(files)}")
    if eboot is not None:
        print(f"eboot DT_NEEDED: {len(eboot.needed)}; undefined symbols: {len(eboot.imports)}; module ids: {len(eboot.modules)}; library ids: {len(eboot.libraries)}")
    if names.implemented or names.names:
        print(f"shadPS4: {len(names.implemented)} LIB_FUNCTION, {len(names.objects)} LIB_OBJ, {len(names.names)} aerolib names")
    print("name sources: " + ", ".join(f"{k}={v}" for k, v in sorted(sources.items())))
    for file_name in files:
        if file_name in summary:
            origin = f"core/libs/prx/{real[file_name].stem} ({len(real[file_name].sources)} sources)" if file_name in real else vendored[file_name].directory
            print(f"  {file_name:32} {len(modules[file_name]):5} imports: {summary[file_name]['prospero']} from {origin}, {summary[file_name]['complement']} complemented")
            for record in modules[file_name].values():
                if record["provider"] != "complement" and not record["provider"].startswith("prospero"):
                    print(f"      {record['display']:36} {record['provider']}")
        else:
            print(f"  {file_name:32} {len(modules[file_name]):5} stub exports")
    if unresolved:
        print(f"symbols whose module could not be matched to a DT_NEEDED file ({len(unresolved)}), exported from the fallback module:")
        for symbol in unresolved:
            print(f"  {symbol}")
    print(f"output: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
