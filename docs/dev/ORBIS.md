# Orbis (PS4) input

How a decrypted PS4 executable differs from the Prospero (PS5) input the relinker was written for, and what the relinker does about it. Facts below were measured on two retail `eboot.bin` dumps (CUSA02799, CUSA00900) and cross-checked against the shadPS4 loader (`src/core/loader/elf.cpp`, `src/core/module.cpp`), which maps the same images natively.

## Container

| | Prospero input | Orbis dump |
|---|---|---|
| File magic | `7F 45 4C 46` (plain ELF) | `4F 15 3D 1D` (fake SELF: cleartext segments) |
| SELF header | none | 0x20 bytes: version 0, mode 1, little-endian, attributes 0x12, category 1, program type 1, `segment_count` at 0x18 |
| SELF segment table | none | `segment_count` entries of 0x20 bytes at 0x20: `flags`, `file_offset`, `file_size`, `memory_size` |
| Embedded ELF header | at 0 | at `0x20 + segment_count * 0x20`; `e_phoff` is relative to it |
| Segment to data mapping | `p_offset` indexes the file | a SELF segment with flag `0x800` (blocked) carries the data of program header `(flags >> 20) & 0xFFF`; flag `0x2` means encrypted, `0x8` compressed, both unsupported; unblocked segments are digests and are ignored |
| `PT_DYNAMIC` data | at its `p_offset` | its `p_offset` falls inside the `PT_SCE_DYNLIBDATA` file range, so it is served by that segment's SELF block |

`Relinker::UnwrapFakeSelf` ([OrbisInput.cpp](../../core/relinker/relinker/src/parsing/OrbisInput.cpp)) rebuilds the flat ELF: it copies the ELF header and program header table, then places each blocked segment at the `p_offset` of the program header it names, leaving gaps zero. The result is what the original linker wrote, so `ElfReader` and every later stage read it unchanged.

## ELF header

| Field | Prospero input | Orbis dump |
|---|---|---|
| `EI_OSABI` | 9 (FreeBSD) | 9 (FreeBSD) |
| `e_type` | `3`, `0xFE10` or `0xFE18` | `0xFE10` (`ET_SCE_DYNEXEC`, relocatable executable) or `0xFE18` (`ET_SCE_DYNAMIC`, module); `0xFE00` (`ET_SCE_EXEC`) is a fixed-address image and stays unsupported |
| `e_entry` | image-relative | image-relative |
| `e_phentsize` | 56 | 56 |
| `e_shnum` | 0 | 0 |

## Program headers

| Type | Value | Prospero input | Orbis dump |
|---|---|---|---|
| `PT_LOAD` | 1 | code and data | code and data; the data segment also covers `PT_SCE_PROCPARAM` and `PT_TLS` |
| `PT_SCE_RELRO` | `0x61000010` | overlaps a `PT_LOAD` or is absent | may be a standalone segment (CUSA02799: `vaddr 0x2CC4000`, `memsz 0x1F8000`, `p_flags = R`) that no `PT_LOAD` covers and that holds the GOT named by `DT_SCE_PLTGOT`; shadPS4 maps it like a `PT_LOAD` |
| `PT_SCE_DYNLIBDATA` | `0x61000000` | string, symbol, relocation tables; `DT_*` offsets are relative to its `p_offset` | identical |
| `PT_SCE_PROCPARAM` | `0x61000001` | process parameters | 0x40 bytes, magic `ORBI` at +8 |
| `PT_DYNAMIC` | 2 | `vaddr 0` | `vaddr 0`, file range inside `PT_SCE_DYNLIBDATA` |
| `PT_TLS` | 7 | TLS template | TLS template, often `filesz 0` |
| `PT_GNU_EH_FRAME` | `0x6474E550` | unwind table | unwind table; the PLT follows the last FDE as on Prospero |
| `PT_SCE_COMMENT`, `PT_SCE_LIBVERSION` | `0x6FFFFF00`, `0x6FFFFF01` | metadata, `memsz 0` | metadata, `memsz 0` |

`Relinker::PromoteStandaloneRelro` rewrites a `PT_SCE_RELRO` that no `PT_LOAD` covers into a `PT_LOAD` with `PF_R | PF_W`, because `WindowsLoadImage` maps only `PT_LOAD` and the startup stub writes the GOT slots at run time. A RELRO that overlaps a `PT_LOAD` is left as it is.

## Dynamic section

The `DT_SCE_*` table tags have the same values on both consoles (`DT_SCE_STRTAB 0x61000035`, `DT_SCE_SYMTAB 0x61000039`, `DT_SCE_RELA 0x6100002F`, `DT_SCE_JMPREL 0x61000029`, `DT_SCE_PLTGOT 0x61000027`, ...), so the pipeline reads both with the `DT_OS_*` constants. The module and library identity tags differ:

| Purpose | Prospero | Orbis |
|---|---|---|
| Imported module | `0x61000045`: `id << 48 \| name offset` | `DT_SCE_NEEDED_MODULE 0x6100000F`: `id << 48 \| version << 32 \| name offset` |
| Imported library | implicit | `DT_SCE_IMPORT_LIB 0x61000015` and `DT_SCE_IMPORT_LIB_ATTR 0x61000019` |
| Own module | `0x6100000D` or `0x61000043` | `DT_SCE_MODULE_INFO 0x6100000D`, id 0 |
| Own library | implicit | `DT_SCE_EXPORT_LIB 0x61000013` and `DT_SCE_EXPORT_LIB_ATTR 0x61000017` |

Both consoles name symbols `<NID>#<library id>#<module id>` with the ids in the `A-Za-z0-9+-` alphabet; the pipeline strips the suffix and resolves the module id against the imported module table, so it now also accepts `DT_SCE_NEEDED_MODULE`. `DT_NEEDED` holds the `.prx` file names on both.

## Relocations

Measured relocation types: `R_X86_64_RELATIVE`, `R_X86_64_64`, `R_X86_64_GLOB_DAT` and `R_X86_64_JUMP_SLOT`, all already supported. No TLS relocations were present; shadPS4 additionally handles `R_X86_64_DTPMOD64`, which the relinker still rejects. An Orbis executable may apply `R_X86_64_64` or `R_X86_64_GLOB_DAT` to a symbol it defines itself; the pipeline folds those into `R_X86_64_RELATIVE` with `st_value + addend` instead of emitting an import of its own export.

## Code layout

Orbis code differs from Prospero code in two ways that break linear decoding, which the syscall scan, the call-site resolver and the Windows TLS scan all rely on:

- `.rodata` shares the executable `PT_LOAD` with `.text`. `AnalyzeCodeLayout` ([CodeExtent.cpp](../../core/relinker/relinker/src/analysis/CodeExtent.cpp)) trims every executable segment to the end of its PLT, found after the last `PT_GNU_EH_FRAME` function as on Prospero, so later analysis never reads past the code.
- orbis-clang keeps `switch` jump tables inside the code: a table of `int32` displacements relative to its own base, loaded with `lea r64, [rip + table]`, sits at the end of the function that uses it, inside that function's FDE range, and with LTO it may spill past the FDE boundary into the next function's range. Two tables can be adjacent. `AnalyzeCodeLayout` collects every `lea` base, bounds each table by the next base and by the first entry whose target leaves the `lea`'s function, and reports the tables as data ranges with their targets as code roots. The pipeline skips those ranges in the syscall and call-site scans; `CodeInstructionCollector` skips them in its function walk and excludes them from the control flow graph.
- The unwind table may contain zero-length FDEs, which are accepted and ignored.

Measured: 498 tables in CUSA02799, 3895 in CUSA00900 (an LTO build).

## Runtime libraries

PS4 and PS5 derive NIDs with the same function (SHA-1 of the name plus the 16-byte salt, 11 base64 characters), so the Prospero libraries in `core/libs/prx` already export most of what a PS4 title imports once `nid_patcher` has renamed their exports. [tools/stub_generator.py](../../tools/stub_generator.py) builds the PS4 runtime from that:

```sh
python tools/stub_generator.py --registry out/app.registry.json --eboot <title>/eboot.bin --shadps4 <shadPS4 checkout>
cmake -S . -B build -G Ninja -DANYPS5_RELINKER_ONLY=ON -DANYPS4_LIBS_DIR=<dir of app.exe>/libs ...
cmake --build build --target orbis_libs
```

- The registry gives the requested NIDs; the eboot gives the full `DT_NEEDED` list and the library and module ids behind each `NID#lib#mod` symbol, which resolves the imports the pipeline could not assign to a module (`libSceAppContentUtil` in `libSceAppContent.prx`, `libSceGnmDriver` in `libSceGnmDriver_padebug.prx`, `libScePosix` in `libkernel.prx`).
- Modules named by `--real` (default `libc.prx,libkernel.prx`) are compiled from the sources listed in their `core/libs/prx/<module>/CMakeLists.txt`, patched with `nid_patcher` exactly as the Prospero build does, and completed by `<module>/<module>_orbis.cpp`, which defines only the NIDs those sources do not provide: data objects, no-op `std::ios_base::Init` and `_Winit` constructors, the Dinkumware `_FSin`, `_FLog`, `_Log`, `_Stoll`, `_Stod` and `_Atomic_*` helpers, `__stack_chk_guard` and `__stack_chk_fail`. A complement symbol whose name does not hash to the wanted NID gets an `APS5_EXPORT`-style alias; objects that libstdc++ may also emit (`_ZTISt9bad_alloc`, `_ZTVSt12bad_weak_ptr`, `_ZSt7nothrow`) are weak. The generator decides what is provided by scanning the sources for `APS5_VABI`, `APS5_ASM_FUNCTION`, `*_nid_postfix` and `APS5_EXPORT` definitions.
- `--vendored <file>=<dir>[:<module>]` builds a module from a vendored source tree instead of stubs: the generator scans `<dir>/src/**/*.cpp` for `PS4_SYSV_ABI` definitions, generates `extern "C"` forwarders from the matching header declarations into `<module>_orbis.cpp`, and adds `<dir>/CMakeLists.txt` as a subdirectory. `3rdparty/shadps4-gnm` is the first such module; see [GNM_PLAN.md](GNM_PLAN.md) section 4.4.1.
- Hand-written modules live in `tools/orbis_impl/<module>.cpp` and replace the stubs named in `STUB_EXTERNAL`; `EXTERNAL_LINK_LIBRARIES` adds their system libraries (`user32`, `gdi32`, `winmm`). `libSceVideoOut.cpp` (Win32 window, GDI presentation, paced flips), `libSceAudioOut.cpp` (`waveOut`, blocking output) and `libScePad.cpp` (keyboard) exist; see [GNM_PLAN.md](GNM_PLAN.md) section 4.4.2. Stub modules compile with `-fno-exceptions` because they carry no unwind tables.
- A stub whose plain `return 0` misleads the title is overridden in the generator: `STUB_IMPLEMENTATIONS` holds small bodies that fill output parameters (user id, handles, resolution), `STUB_RETURNS` holds error codes for functions that must fail or report "no event", and `STUB_EXTERNAL` maps a function to a symbol defined in a hand-written `tools/orbis_impl/<module>.cpp`, compiled into the module when the file exists (`libSceNgs2.cpp` parses waveforms). All keep the trace line. A stub that returns 0 without writing its output parameters is the first suspect for a crash that appears far from any stub call: the sound loader's uninitialised waveform info corrupted the heap and surfaced as three unrelated render-thread crashes.
- Every other module becomes a `.def` plus logging stubs named from shadPS4 (`LIB_FUNCTION`/`LIB_OBJ` registrations in `src/core/libraries`, then the `STUB` table in `src/core/aerolib/aerolib.inl`), prefixed `orbis_` so they never collide with the host C runtime. The stubs are `sysv_abi`, log `[HLE] <module>: <name> (NID ...)` on their first call (every call with `ORBIS_HLE_TRACE=1`) and return 0.
- The generated `core/libs/orbis/CMakeLists.txt` builds everything into `ANYPS4_LIBS_DIR` and copies `libgcc_s_seh-1.dll`, `libstdc++-6.dll` and `libwinpthread-1.dll` next to the libraries, as [USAGE.md](../user/USAGE.md) requires. The renderer module needs a shadPS4 checkout for its external libraries (`-DANYPS4_SHADPS4_EXTERNALS=<shadPS4>/externals`). The real libraries need the WinLibs GCC 15.2 toolchain from [BUILD.md](BUILD.md): with GCC 14 `-fno-asynchronous-unwind-tables` selects SjLj exceptions and the libc unwinder in `core/libs/prx/libc/src/exception` does not link. The root CMakeLists adds the directory when it exists, also in `ANYPS5_RELINKER_ONLY` builds. `core/libs/orbis/stubs.json` records each NID's name, where it came from and which library provides it.

Four Prospero sources needed changes for Orbis titles:

- `core/libs/prx/libkernel/Socket/src/Socket.cpp`: `HostPoll` had no Windows definition; host descriptors now report the requested events.
- `core/libs/prx/libc/src/ApplicationHeap.cpp`: a title without a libc parameter block uses the default allocator, and the PS4 `SceLibcMallocReplace` table (`size 0x70, version 1`) is accepted next to the PS5 one (`0x78, 2`); the ten allocator entries sit at the same offsets.
- `core/libs/prx/libc/src/Mspace.cpp`: `sceLibcMspaceCreate` accepts the Orbis debug flag bits (2 and 4) besides `THREAD_UNSAFE`.
- `core/libs/prx/libkernel/Pthread/src/Thread.cpp`: `scePthreadJoin` returns `SCE_KERNEL_ERROR_ESRCH` for a thread that no longer exists instead of touching the freed object; CUSA02799 joins a worker twice while tearing down a scene.

Measured on CUSA02799: of 425 imports, 140 of 158 `libc.prx` and 68 of 70 `libkernel.prx` NIDs come from the Prospero sources. The startup stub resolves every import and transfers control; the title's allocator maps 0x78000000 bytes of direct memory, creates its mspace, prints `UserMem:0x200000000-0x278000000`, starts its threads through the real `libkernel`, initializes the user, audio and video subsystems through the stubs and enters its main loop. With the vendored Gnm processor (`libSceGnmDriver_padebug.prx`, phase 3.0) the command buffers are executed headless: EOP labels release the main thread, the title registers its display buffers and flips through `sceGnmSubmitAndFlipCommandBuffers` into a window at 60 fps for as long as it is left running, with the audio thread paced by `waveOut` (details, the resolved frame-40 crash and the remaining loading stall in GNM_PLAN.md). `APS5_NO_WRITE_WATCH=1` halves the loader cost; the write-watch tracking serves the Agc caches only. The title reads its data from `/app0`, so `app0/` next to the executable must point at the title directory.

## Not covered yet

- Bundled `sce_module/*.prx` in fake SELF containers are skipped by `GuestModuleBuilder`; unwrapping them the same way is the next step.
- `libSceGnmDriver` now carries shadPS4's Vulkan renderer (phase 3.3, GNM_PLAN.md 4.4.3): draws and dispatches execute and the window shows the rendered frame through a readback; a swapchain-based presenter is still pending. `libSceVideoOut`, `libSceAudioOut` and `libScePad` are hand-written host backends, not the Prospero modules; `libSceNgs2` is the shadPS4 fork's synthesizer. The remaining stub modules still return 0.
- The generator describes one title; libraries for several titles need the union of their registries.
