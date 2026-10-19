# shadPS4 Gnm sources

Vendored from the local shadPS4 checkout `C:\Users\manyv\OneDrive\Documentos\Shadps4git\shadPS4` (branch `witch-fixes`, commit `354faa475753f53332a7d53a5feefbc98e1665f5`), GPL-2.0-or-later.

- `src/`: files copied verbatim (including `video_core/amdgpu/regs.cpp` for `Regs::SetDefaults`) from shadPS4's `src/` with their relative paths; local changes are wrapped in `#ifdef ANYPS4_HEADLESS` and listed below.
- `shims/`: AnyPS5 replacements for the shadPS4 headers the vendored files include but that are not vendored (logging, debug, memory, platform shims, kernel and VideoOut bridges, a null rasterizer).
- `adapter/`: implementations behind the shims.
- `externals/`: fmt (header-only) and magic_enum, copied from shadPS4's externals.

Local changes to vendored files:

- `core/libraries/gnmdriver/gnmdriver.cpp`: `RegisterLib` does not create the Vulkan presenter under `ANYPS4_HEADLESS` and binds the headless VideoOut port.

Local change (phase 3.2): `liverpool.cpp` performs memory fills and memory-to-memory `DMA_DATA` transfers on the CPU when there is no rasterizer (`HeadlessDmaData`, under `ANYPS4_HEADLESS`), since titles wait for those copies.

## Phase 3.3 (renderer)

Added verbatim from the same fork (commit `354faa475753f53332a7d53a5feefbc98e1665f5`): `video_core/renderer_vulkan` (without the presenter, swapchain and host passes, which present through Dear ImGui and SDL), `video_core/texture_cache`, `video_core/buffer_cache`, `page_manager`, `cache_storage`, `host_shaders` (GLSL; the string headers are generated into `generated/` by `tools`' vendoring script, a port of `StringShaderHeader.cmake`), the whole `shader_recompiler`, the `common` headers and the sources they need, `core/signals.h`, `core/libraries/videoout/buffer.h` and `core/libraries/kernel/memory.h`. External libraries (Boost, Vulkan-Headers, VMA, sirit, glslang, Zydis, xxHash, half, robin-map, xbyak, gcn, json, miniz) are taken from the shadPS4 checkout named by `ANYPS4_SHADPS4_EXTERNALS`, not copied.

`adapter/Renderer.cpp` replaces the presenter: a headless `Vulkan::Instance`, a `Scheduler` and the `Rasterizer` are created on the first submit, and at flip time the video-out image is read back from the texture cache into the Win32 window of `libSceVideoOut.prx`. `adapter/Signals.cpp` implements `Core::SignalDispatch` with a vectored exception handler; `adapter/Runtime.cpp` implements the `MemoryManager` facade (`VirtualQuery`/`VirtualProtect`) the caches use.

Local change (phase 3.5): `texture_cache.cpp` logs `Depth overlap` at debug level and `liverpool.cpp` logs the Gfx `WaitRegMem` watchdog once per thousand forced fences; CUSA02799 hits both every frame.

Phase 3.7: `renderer_vulkan/host_passes/pp_pass.{h,cpp}` are vendored unchanged; `shims/video_core/renderer_vulkan/vk_presenter.h` provides the `Vulkan::Frame` they expect. `adapter/Renderer.cpp` presents the video-out image through them, sampling an image view in the frame format of the video-out pixel format (`B8G8R8A8Srgb` for `A8R8G8B8Srgb`, alpha forced to one) exactly like `vk_presenter.cpp`, instead of a raw `vkCmdBlitImage` that copied the channels as stored and showed red and blue swapped.
