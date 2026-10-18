# Gnm on the Prospero Vulkan stack: integration plan

How the PS4 graphics path (`libSceGnmDriver`, `libSceVideoOut`) gets a real implementation inside AnyPS5. Measured on CUSA02799 (The Witch and the Hundred Knight) with the runtime built by [tools/stub_generator.py](../../tools/stub_generator.py); the port source is the shadPS4 checkout, which already turns Gnm command buffers into Vulkan.

## 1. Where the title stands

With real `libc.prx` and `libkernel.prx` and stubs for everything else, the title runs its allocator, starts its threads and submits its first command buffer. Then the main thread stops: in a 25 second trace with every call logged, `sceGnmSubmitCommandBuffers` runs once, `sceVideoOutRegisterBuffers` and `sceVideoOutSubmitFlip` never run, and only the audio thread keeps spinning (128,000 `sceNgs2SystemRender` calls). The first submission writes a completion label through an `EVENT_WRITE_EOP`/`RELEASE_MEM` packet and the CPU polls that label (the shadPS4 fix for this title, a `WaitRegMem` on a per-frame counter, is the same mechanism). A stub that returns 0 never writes it.

So the first deliverable is not pixels. It is a command processor that executes the PM4 stream far enough to write labels, raise end-of-pipe events and honor flips. Rendering comes after.

## 2. Graphics imports of the title

`core/libs/orbis/stubs.json` lists 57 `libSceGnmDriver_padebug.prx` NIDs and 14 `libSceVideoOut.prx` NIDs. Every one of the 57 Gnm functions has an implementation in shadPS4's `src/core/libraries/gnmdriver/gnmdriver.cpp`; 13 of the 14 VideoOut functions are implemented in `src/core/libraries/videoout`.

| Group | Functions |
|---|---|
| Submission | `sceGnmSubmitCommandBuffers`, `sceGnmSubmitAndFlipCommandBuffers`, `sceGnmSubmitDone`, `sceGnmAreSubmitsAllowed`, `sceGnmDingDong`, `sceGnmFlushGarlic`, `sceGnmRequestFlipAndSubmitDone`, `sceGnmInsertWaitFlipDone` |
| Hardware state | `sceGnmDrawInitDefaultHardwareState200`, `sceGnmDispatchInitDefaultHardwareState`, `sceGnmSetGsRingSizes`, `sceGnmGetTheTessellationFactorRingBufferBaseAddress` |
| Shader binding | `sceGnmSetPsShader`, `sceGnmUpdatePsShader`, `sceGnmSetVsShader`, `sceGnmUpdateVsShader`, `sceGnmSetEsShader`, `sceGnmSetHsShader`, `sceGnmUpdateHsShader`, `sceGnmSetLsShader`, `sceGnmSetCsShader`, `sceGnmSetGsShader`, `sceGnmUpdateGsShader`, `sceGnmSetEmbeddedPsShader`, `sceGnmSetEmbeddedVsShader` |
| Events and queues | `sceGnmAddEqEvent`, `sceGnmDeleteEqEvent`, `sceGnmGetEqEventType`, `sceGnmGetEqTimeStamp`, `sceGnmMapComputeQueue`, `sceGnmMapComputeQueueWithPriority`, `sceGnmUnmapComputeQueue` |
| Markers, CU masks, validation, Razor, mip stats | `sceGnmInsertSetMarker`, `sceGnmInsertPushMarker`, `sceGnmInsertPopMarker`, `sceGnmInsertSetColorMarker`, `sceGnmInsertPushColorMarker`, `sceGnmLogicalCuIndexToPhysicalCuIndex`, `sceGnmLogicalCuMaskToPhysicalCuMask`, `sceGnmIsUserPaEnabled`, `sceGnmDebugHardwareStatus`, `sceGnmValidate*` (5), `sceRazor*` (5), `sceGnmDriver*Capture*`/`TraceInProgress` (3), `sceGnmSetupMipStatsReport`, `sceGnmRequestMipStatsReportAndReset`, `sceGnmDisableMipStatsReport` |
| VideoOut | `sceVideoOutOpen`, `sceVideoOutClose`, `sceVideoOutSetBufferAttribute`, `sceVideoOutRegisterBuffers`, `sceVideoOutRegisterStereoBuffers`, `sceVideoOutUnregisterBuffers`, `sceVideoOutSubmitFlip`, `sceVideoOutIsFlipPending`, `sceVideoOutSetFlipRate`, `sceVideoOutAddFlipEvent`, `sceVideoOutGetResolutionStatus`, `sceVideoOutGetDeviceCapabilityInfo`, `sceVideoOutModeSetAny_`, `sceVideoOutConfigureOutputMode_` |

Note the module file name: the title's `DT_NEEDED` is `libSceGnmDriver_padebug.prx` (the PA-debug build of the SDK library), while the module id is `libSceGnmDriver`. The build must emit the same DLL under both names.

## 3. VideoOut: reuse the Prospero module, inject the Orbis entry points

`core/libs/prx/libSceVideoOut` (2,266 lines) already exports 8 of the 14 functions under the NIDs the title wants: `Open`, `Close`, `IsFlipPending`, `SetFlipRate`, `SubmitFlip`, `UnregisterBuffers`, `AddFlipEvent`, `GetResolutionStatus`. Their signatures are the same on both consoles, and `sceVideoOutAddFlipEvent` takes the `KernelEqueue` of the same `libkernel.prx` the title already uses, so flip waits stay consistent.

The six missing functions are the Orbis-only shape of buffer registration plus trivia:

| Orbis function | Prospero counterpart | Divergence |
|---|---|---|
| `sceVideoOutSetBufferAttribute(attr, pixelFormat, tilingMode, aspectRatio, width, height, pitch)` | `sceVideoOutSetBufferAttribute2(attr2, pixelFormat, tilingMode, width, height, option, dccControl, dccClearColor)` | Orbis `BufferAttribute` is 0x28 bytes: `u32 pixel_format, u32 tiling_mode, s32 aspect_ratio, u32 width, u32 height, u32 pitch_in_pixel, u32 option, u32 reserved0, u64 reserved1`. Prospero `VideoOutBufferAttribute2` is 0x50 bytes with `pixel_format` as a `u64` at 0x20, `option` as `u64` at 0x18, plus DCC fields. Pixel format values differ: Orbis `A8R8G8B8_SRGB = 0x80000000`, `A2R10G10B10 = 0x88060000`, `A16R16G16B16_FLOAT = 0xC1060000`, `A8B8G8R8_SRGB = 0x80002200`, Prospero uses 64-bit codes. A conversion table is required. |
| `sceVideoOutRegisterBuffers(handle, startIndex, void* const* addresses, num, const BufferAttribute*)` | `sceVideoOutRegisterBuffers2(handle, setIndex, start, const VideoOutBuffers*, num, const VideoOutBufferAttribute2*, category, option)` | Orbis passes bare addresses; Prospero passes `{data, metadata, reserved[2]}` records and a set index. The adapter builds the records with null metadata (no DCC on Orbis display buffers) and set index 0. |
| `sceVideoOutRegisterStereoBuffers` | none | Return `SCE_VIDEO_OUT_ERROR_INVALID_VALUE`; the title probes it. |
| `sceVideoOutGetDeviceCapabilityInfo(handle, info*)` | none | Fill `{capability = 0}` (no HDR, no VRR). |
| `sceVideoOutModeSetAny_(mode*, size)` | none | Zero the structure, as shadPS4 does. |
| `sceVideoOutConfigureOutputMode_(handle, reserved, mode*, options, size)` | `sceVideoOutConfigureOutput` | Accept and return 0. |

The module depends on `libSceAgcDriver` for the window and presentation: `DisplayWindow.cpp` creates the SDL2 Vulkan window, `Buffer.cpp` validates guest memory through `AgcDriverCheckGuestMemory`, and `VideoOutDriver.cpp` presents through `AgcDriverPresentBuffer(PresentationWindow, DisplayBuffer, gpuReady, context)`. The `DisplayBuffer` carries `{address, pixel_format, width, height, tiling_mode, pitch, metadata, clear color}`, which is enough to describe an Orbis display buffer once the format code is converted.

Decision: do not copy shadPS4's `videoout` module. Build `libSceVideoOut.prx` the way `libc.prx` is built today: the Prospero sources plus a hand-written `libSceVideoOut_orbis.cpp` complement with the six adapters above. The generator needs one addition for this: a per-module override directory (`core/libs/orbis/overrides/<module>.cpp`) whose definitions count as provided, so the generated complement never duplicates them.

Prerequisite: `libSceVideoOut` links `libSceAgcDriver` and `SDL2-static`, so this step needs the full build (`git submodule update --init --recursive`, SDL2, Vulkan-Headers, the shader recompiler) with the WinLibs GCC 15.2 toolchain, not the relinker-only configuration used so far.

## 4. Gnm: port shadPS4's translator as a module, not function by function

Agc is not a translation target. The Prospero driver (`core/libs/prx/libSceAgcDriver`, 27,000 lines of graphics plus 9,000 of execution) decodes RDNA2 registers and recompiles RDNA2 shaders (`core/shader/recompiler`, 57,000 lines, `RdnaDecoder`). Liverpool is GCN2: different register file (`regs_color.h`, `regs_depth.h`, `regs_shader.h` in shadPS4 are the Sea Islands layout), different PM4 opcodes in places, and a GCN shader ISA the Prospero recompiler cannot read. shadPS4 has all of that working and tested against this very title. Porting its translator is the shortest path; translating Gnm to Agc would mean writing a GCN front end that AnyPS5 does not have.

What is reusable from AnyPS5 for the Gnm module is the glue, not the renderer: the SDL2 window and `PresentationWindow` abstraction (`{context, extensions, createSurface(context, VkInstance), getDrawableSize}`), the `libkernel` event queue (`EqueueAddEvent`, `EqueueTriggerEvent`), the direct memory allocator, and the guest write tracking in `libc` (`GuestWriteWatch`). The port therefore owns its own `VkInstance`/`VkDevice` (shadPS4's `vk_instance.cpp`) and presents into the window the VideoOut module creates. Sharing AnyPS5's `VulkanDevice` across two independently scheduled renderers is possible later but is not needed to get frames.

### 4.1 Source inventory (shadPS4, `src/`)

| Component | Files | Lines | Role |
|---|---|---|---|
| `core/libraries/gnmdriver/` | `gnmdriver.cpp`, `gnmdriver.h`, `gnmdriver_init.h`, `gnm_error.h` | 4,186 | The 57 HLE entry points, the hardware init PM4 sequences (`InitSequence200` for `sceGnmDrawInitDefaultHardwareState200`), compute queue mapping, EQ events, flip patching |
| `video_core/amdgpu/` | `liverpool.cpp/.h`, `pm4_cmds.h`, `pm4_opcodes.h`, `regs*.h`, `resource.h`, `pixel_format.cpp/.h`, `tiling.cpp/.h`, `types.h` | 6,489 | PM4 parser and GPU thread (`Liverpool::Process`, `ProcessGraphics`, `ProcessCompute`), register state, `WaitRegMem`, `EVENT_WRITE_EOP`/`RELEASE_MEM` label writes, resource descriptors, tiling modes |
| `video_core/renderer_vulkan/` | 34 files | 10,665 | `vk_instance` (device, extensions), `vk_scheduler`, `vk_rasterizer` (draws, dispatches, render targets), `vk_pipeline_cache`, `vk_graphics_pipeline`, `vk_compute_pipeline`, `liverpool_to_vk` (format and state tables), `vk_presenter`, `vk_swapchain`, `vk_platform`, `vk_shader_hle` |
| `video_core/renderer_vulkan/host_passes/` + `host_shaders/` | | 839 + 3,857 | Blit, FSR and post-processing shaders compiled to SPIR-V at build time with glslang |
| `video_core/texture_cache/` | | 5,024 | Image registry, tile detiling compute shaders, depth/color aliasing |
| `video_core/buffer_cache/` | | 2,576 | Vertex and storage buffers, GDS, uploads |
| `video_core/` root | `page_manager.cpp/.h`, `multi_level_page_table.h`, `host_shaders` glue | 1,073 | Write tracking (Linux `userfaultfd`, Windows access-violation handler) |
| `shader_recompiler/` | `frontend/` (GCN decode and translate, 10,624 + 5,816), `ir/` and `ir/passes/` (6,001 + 5,005), `backend/spirv/` (6,452), root (1,136) | 35,052 | GCN ISA to IR to SPIR-V |
| `common/` subset | `types.h`, `assert.h`, `logging/`, `bit_field.h`, `enum.h`, `alignment.h`, `div_ceil.h`, `slot_vector.h`, `object_pool.h`, `thread.h`, `spin_lock.h`, `adaptive_mutex.h`, `hash.h`, `unique_function.h`, `scope_exit.h`, `polyfill_thread.h`, `lru_cache.h`, `func_traits.h`, `uint128.h`, `io_file.h`, `path_util.h`, `elf_info.h`, `signal_context.h`, `singleton.h` | about 3,500 of 9,704 | Utilities the graphics code includes |

Total to vendor: about 60,000 lines. External libraries the code includes: Boost headers (41 includes, `small_vector`, `icl`, `container`), fmt (14), magic_enum (13), sirit (SPIR-V emitter, 4), tsl robin_map (4), xbyak (2, shader HLE), half (1), VulkanMemoryAllocator, Vulkan-Headers, xxHash, glslang (build time), the GCN ISA header library in `externals/gcn`. AnyPS5 already vendors Vulkan-Headers, VulkanMemoryAllocator, glslang, SPIRV-Headers and SDL2 under `3rdparty/`; Boost, fmt, magic_enum, sirit, robin_map, xbyak, half, xxHash and `gcn` are new. All are header-only or small except Boost, which is used header-only.

### 4.2 Where it lives

Put the vendored code in `3rdparty/shadps4-gnm/` with its own `CMakeLists.txt`, not under `core/`. The conventions checker (`tools/check_conventions.py`) forbids comments in `core/` C++ and skips `3rdparty/`; shadPS4 code is commented throughout, and keeping it close to upstream makes later merges possible. Only the adapter layer goes to `core/libs/orbis/gnm/`. shadPS4 is GPL-2.0-or-later, AnyPS5 is GPL-2.0-only; the combination is distributable under GPL-2.0.

Output: `add_library(orbis_libSceGnmDriver SHARED ...)` producing `libSceGnmDriver.prx`, copied also as `libSceGnmDriver_padebug.prx`, exported through `nid_patcher` like the other real modules. The HLE functions keep their shadPS4 names, so their NIDs come out right without aliases; `PS4_SYSV_ABI` maps to `__attribute__((sysv_abi))`, the same calling convention `APS5_VABI` denotes.

### 4.3 Seams to cut

Each item names the shadPS4 header the port currently includes and what replaces it.

1. `core/memory.h` (`MemoryManager`): shadPS4 translates guest addresses and tracks mappings. AnyPS5 runs the title natively, so guest addresses are host addresses; `GetDirectMemoryArea`, `IsValidAddress` and the mapping iteration become `VirtualQuery` plus the direct memory bookkeeping `libkernel` already keeps (`DirectMemory/Export.cpp`). The `MemoryManager` singleton shrinks to a 200-line facade.
2. `core/signals.h` and `video_core/page_manager.cpp`: write tracking for the buffer and texture caches. On Windows shadPS4 uses an access-violation handler plus `VirtualProtect`; AnyPS5's `libc` has `GuestWriteWatch` (`MEM_WRITE_WATCH` polling) and `libSceAgcDriver` has its own read guards. First version: the shadPS4 Windows path unchanged, registered through a vectored exception handler inside the DLL; later unify with `GuestWriteWatch`. This is the highest-risk seam.
3. `core/platform.h` (`IrqC`: `GfxEop`, `GfxFlip`, `GpuIdle`, `Compute*` interrupts): a small interrupt controller class with the same `Signal`/`Register` API, implemented in the adapter layer.
4. `core/libraries/kernel/*` (`equeue`, `process`, `time`): `sceGnmAddEqEvent` and the EOP path add events to a `KernelEqueue`; the adapter calls the `EqueueAddEvent_nid_postfix`/`EqueueTriggerEvent_nid_postfix` exports of `libkernel.prx`. `GetSdkVersion`/process parameters come from the `.procpar` block the title already exposes.
5. `core/libraries/videoout/*` (`VideoOutPort`, `BufferAttributeGroup`, flip requests): `Liverpool` calls the VideoOut driver when a flip packet completes. The adapter forwards to `libSceVideoOut.prx` through a dedicated internal export (`OrbisVideoOutFlipFromGpu_nid_no_patch`), which calls the module's `sceVideoOutSubmitFlip` path and the presenter.
6. `frontend/window_sdl.h` (SDL3 `WindowSDL`) in `vk_presenter`/`vk_instance`: replaced by AnyPS5's `PresentationWindow` (`createSurface(context, instance)`, `getDrawableSize`). The SDL2 window stays owned by `libSceVideoOut.prx`, exactly as it is for Agc.
7. `core/emulator_settings.h`, `core/debug_state.h`, `core/devtools/*`, `imgui/*`: compile-time constants and no-op stubs. The ImGui overlay and RenderDoc hooks are stripped.
8. `common/logging` (spdlog): a thin `LOG_*` macro set over `WriteLogLine` from `libc`'s `LogMacros.hpp`, with the same class names so upstream diffs stay small.
9. Shader cache on disk (`common/path_util.h`, `io_file.h`): point at `<exe dir>/shader_cache/<title id>/`; the title id is in `.procpar`'s process name or `param.sfo`.

### 4.4 Phases and exit criteria

**Phase 3.0, headless command processor.** Vendor `gnmdriver.cpp`, `video_core/amdgpu/*`, `common` subset and the seams 1, 3, 4, 7, 8. `Liverpool` runs with `BindRasterizer(nullptr)`: draws and dispatches are skipped, but `WaitRegMem`, `EVENT_WRITE_EOP`, `RELEASE_MEM`, `WRITE_DATA`, `DMA_DATA` (CPU-visible copies), `WAIT_ON_ADDRESS`, timestamps and flip labels execute, and EOP raises `GfxEop` into the title's equeue. Exit criterion: the main thread leaves its first submit, `sceVideoOutRegisterBuffers` and `sceVideoOutSubmitFlip` appear in the trace, and the title loops at its target rate with the audio stubs. This phase builds in the relinker-only configuration with Vulkan-Headers only.

**Phase 3.1, window and flips.** Build `libSceVideoOut.prx` from the Prospero sources with the six Orbis adapters (section 3), under the full build. Vendor `vk_instance`, `vk_platform`, `vk_scheduler`, `vk_swapchain`, `vk_presenter`, `vk_master_semaphore` and the blit host shaders, with seam 6. `PrepareBlankFrame` presents a cleared frame per flip. Exit criterion: a window at the title's resolution, flip events delivered, no GPU hangs over several minutes.

**Phase 3.2, rendering.** Vendor `shader_recompiler`, `renderer_vulkan` (rasterizer, pipeline caches, `liverpool_to_vk`), `texture_cache`, `buffer_cache` and seam 2 in its unmodified Windows form. Exit criterion: the title's loading screen and menu render; compare against shadPS4 on the same machine.

**Phase 3.3, consolidation.** Replace the exception-handler write tracking with `GuestWriteWatch`, move presentation onto AnyPS5's `VulkanDevice` if a single device proves necessary, and bring the shadPS4 fixes recorded for this title (the `WaitRegMem` watchdog and the `GpuIdle` signal that releases `sceGnmAreSubmitsAllowed`) across as regular code.

### 4.4.1 Phase 3.0 status (18 October 2026): headless processor running, title loops and flips

What exists:

- `3rdparty/shadps4-gnm/` holds verbatim copies of shadPS4's `core/libraries/gnmdriver`, `video_core/amdgpu` (`liverpool`, `regs`, `pm4_*`, tiling and register headers), the `common` subset they need, `fmt` and `magic_enum`; `VENDOR.md` lists the files and the upstream commit. `shims/` replaces the headers of seams 1, 3, 4, 5, 7 and 8 (`common/logging/log.h` over `fmt`, `common/debug.h` without Tracy, `core/memory.h` facade, `core/libraries/kernel/equeue.h` over `libkernel.prx`, `videoout/driver.h` headless port, no-op `vk_rasterizer.h`/`vk_presenter.h`, a three-file Boost stand-in). `adapter/` has `Logging.cpp` (`ORBIS_GNM_LOG=trace|debug|info|warning|error|critical|off`), `Runtime.cpp` (debug state, settings, the system reserved block for the tessellation ring, flip labels), `KernelBridge.cpp` (`EqueueInternal` over `EqueueAddEvent_nid_postfix`/`EqueueTriggerEvent_nid_postfix`, `sceKernelGetCompiledSdkVersion` from `.procpar`) and `Module.cpp` (`RegisterLib` from a static initializer). The one change inside vendored code is in `gnmdriver.cpp` `RegisterLib`: under `ANYPS4_HEADLESS` no presenter is created and `Liverpool` gets the headless port.
- `tools/stub_generator.py --vendored libSceGnmDriver_padebug.prx=3rdparty/shadps4-gnm:libSceGnmDriver` emits `extern "C"` forwarders (`<name>_nid_postfix`) for every NID the vendored sources provide, generated from the header declarations, plus logging stubs for the rest; the module is patched with `nid_patcher --preserve-exports` and copied as `libSceGnmDriver.prx` too. 57 of the title's Gnm imports are forwarded, and the forwarders keep the `[HLE]` trace line.
- Flip labels: `sceVideoOutGetBufferLabelAddress` hands out a 16-entry label array inside the module; `sceVideoOutSubmitEopFlip` registers a one-shot `GfxFlip` handler that clears the label of the previously flipped buffer, exactly as `VideoOutDriver::Flip` does, so `sceGnmInsertWaitFlipDone` waits complete without the watchdog. `VideoOutPort::WaitVoLabel` sleeps on the label with a two-second cap.

What the trace shows for CUSA02799 (`ORBIS_HLE_TRACE=1`, `app0/` junction to the title directory next to the executable):

1. Four `sceGnmSubmitCommandBuffers` during initialization, each followed by the EOP that unblocks the main thread; `sceVideoOutRegisterBuffers` and `sceVideoOutSetFlipRate` follow.
2. The title enters its main loop (`sceUserServiceGetEvent`, `scePadReadState` per frame) and the render thread flips through `sceGnmSubmitAndFlipCommandBuffers` -> `sceVideoOutSubmitEopFlip`: 39 frames, each with `sceGnmSubmitDone`, `sceGnmInsertWaitFlipDone`, a dozen `sceGnmSetCsShader` and two or three `sceGnmSetVsShader`/`sceGnmSetPsShader`. This title never calls `sceVideoOutSubmitFlip`; the Gnm submit-and-flip path is its only flip, so the exit criterion is met through `sceVideoOutSubmitEopFlip`.
3. Frame 40 crashes on `PhyreEngineRenderThread` at ELF `0xbe6352` (write to 0), right after the first `sceGnmUpdateVsShader`. The function (`0xbe6290`) walks the shader resource slots bound by `0xed2b00` (bitmask at `this+0x1c00`, 0x18-byte slots from `this+0x1000`) and unlinks each resource's 0xb0-byte usage record from an intrusive list; the record of the bound resource has a garbage `prev` and a null `next`. The resource pointer equals the second entry of a global object table at ELF `0x40c9f20`, a small (0x30-byte) default object, so the engine bound a placeholder where it expected a real resource. The crash is deterministic across runs and independent of the flip fix. Open: whether the placeholder comes from a stubbed query (the stubs that still return 0 without output are listed in `core/libs/orbis/stubs.json`) or from data the title expects the GPU to have produced (compute dispatches are skipped headless); phase 3.2 answers the second.

Stubs that had to behave for the title to get this far, all in `tools/stub_generator.py` (`STUB_IMPLEMENTATIONS`, `STUB_RETURNS`): `sceUserServiceGetInitialUser` and `sceUserServiceGetLoginUserIdList` write user 1, `scePadOpen`/`sceVideoOutOpen` return handle 1, `sceVideoOutGetResolutionStatus` writes 1920x1080, `scePlayGoOpen` writes a handle, `sceUserServiceGetEvent` returns `SCE_USER_SERVICE_ERROR_NO_EVENT` (the title spins forever on 0), `sceSystemServiceReceiveEvent` returns `NO_EVENT`, `sceNpTrophy*` return `SCE_NP_TROPHY_ERROR_NOT_INITIALIZED`.

Two runtime findings outside Gnm:

- The title's shader table (`newShader/*.fx.phyre`) is read from `/app0`; without `app0/` next to the executable the loader returns empty `shared_ptr`s and the title dereferences one at `0x8bd34`. The runtime layout in `USAGE.md` applies to Orbis titles as well.
- `~PSceneBase` joins its worker thread twice (`sub_c47f50`, then `sub_c47fa0` when the started flag is still set). FreeBSD returns `ESRCH` for the second join; AnyPS5 had already freed the thread object and threw `system_error`. `scePthreadJoin` in `core/libs/prx/libkernel/Pthread/src/Thread.cpp` now checks the live-thread registry first and returns `SCE_KERNEL_ERROR_ESRCH`.

Build: `cmake -S . -B C:/Users/manyv/AppData/Local/Temp/aps4 -G Ninja -DANYPS5_RELINKER_ONLY=ON -DANYPS4_LIBS_DIR=<exe dir>/libs` with the WinLibs GCC 15.2 compilers, then `--target orbis_libs`. The short build path matters: the vendored sources exceed `MAX_PATH` in depfiles under a deep directory.

### 4.5 Risks

- Two Vulkan devices in one process (Agc's and Gnm's) is fine in phases 3.1 and 3.2 because no Agc title runs in the same process; it only matters if both drivers must coexist.
- The texture cache relies on faulting pages the CPU writes while the GPU reads. Native execution means the title's threads touch those pages directly; the protection changes must be process-wide and thread-safe, which shadPS4 already handles on Windows, but AnyPS5's `libc` also protects guest pages for its own tracking. Conflicting protections surface as spurious access violations and must be reconciled in phase 3.3.
- GCN shaders that shadPS4 patches at translation time (the `LdsDirect` operand noted for this title) come along with the recompiler; nothing extra is needed, but the shader cache must not be shared with shadPS4's.
- `sceGnmSubmitAndFlipCommandBuffers` and `sceGnmRequestFlipAndSubmitDone` write a flip label into the title's command buffer (`PatchFlipRequest` in shadPS4). The title's buffers live in direct memory it owns; writing into them is legitimate on native execution as well.
