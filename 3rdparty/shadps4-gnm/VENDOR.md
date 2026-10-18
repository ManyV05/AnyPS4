# shadPS4 Gnm sources

Vendored from the local shadPS4 checkout `C:\Users\manyv\OneDrive\Documentos\Shadps4git\shadPS4` (branch `witch-fixes`, commit `354faa475753f53332a7d53a5feefbc98e1665f5`), GPL-2.0-or-later.

- `src/`: files copied verbatim (including `video_core/amdgpu/regs.cpp` for `Regs::SetDefaults`) from shadPS4's `src/` with their relative paths; local changes are wrapped in `#ifdef ANYPS4_HEADLESS` and listed below.
- `shims/`: AnyPS5 replacements for the shadPS4 headers the vendored files include but that are not vendored (logging, debug, memory, platform shims, kernel and VideoOut bridges, a null rasterizer).
- `adapter/`: implementations behind the shims.
- `externals/`: fmt (header-only) and magic_enum, copied from shadPS4's externals.

Local changes to vendored files:

- `core/libraries/gnmdriver/gnmdriver.cpp`: `RegisterLib` does not create the Vulkan presenter under `ANYPS4_HEADLESS` and binds the headless VideoOut port.
