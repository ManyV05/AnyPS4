# Usage

AnyPS4 runs **The Witch and the Hundred Knight: Revival Edition (CUSA02799)** natively on Windows. It was developed and tested only for this title; other titles are not supported. You need your own copy of the game: a folder with the decrypted `eboot.bin`, `sce_sys/` and the rest of the game's files.

The pipeline has three steps: the relinker converts `eboot.bin` into a native Windows executable, the Orbis libraries the game imports are built as `.prx` libraries, and the output folder is laid out with `app0` pointing at your game folder. The game files are never copied.

## Install and play

The launcher (`anyps4_launcher.exe`, built by the top-level CMake on Windows) does the whole install:

1. Pick the game folder and an install folder (not inside the game folder).
2. Press **Install**. It relinks `eboot.bin`, generates the runtime libraries, builds them and creates `<install>/CUSA02799.exe`, `libs/`, `app0` (a directory junction to the game folder), `anyps4/` and `savedata/`.
3. Press **Play**. It starts the executable with the settings from the launcher (music and effects volume, present mode, pipeline cache, validation).

The installer can also be run by hand:

```sh
python tools/anyps4_install.py --game <game folder> --out <install folder> --shadps4 <shadPS4 checkout> --toolchain-bin <mingw64/bin>
```

`--skip-build` only relinks and regenerates, and reuses the libraries already in `<install>/libs`. Build requirements are in the [build instructions](../dev/BUILD.md).

The executable can be started directly, but then its output goes nowhere. To keep a log, start it from a terminal in the install folder:

```sh
./CUSA02799.exe > anyps4/log/session.log 2>&1
```

Saves go to `savedata/` beside the executable (`ORBIS_SAVEDATA_DIR` overrides it). Keyboard and controller input is described in [INPUT_MAPPING.md](INPUT_MAPPING.md). The console language is fixed to English (United States).

## Runtime layout

```text
CUSA02799.exe
libs/
    *.prx                 Orbis libraries built for this game, plus the MinGW runtime DLLs
app0/                     directory junction to the game folder
anyps4/
    pad.ini               keyboard mapping
    log/, audio_dumps/    diagnostics (created when used)
savedata/
```

A self-built `libs/` also needs `libgcc_s_seh-1.dll`, `libstdc++-6.dll` and `libwinpthread-1.dll` from the `mingw64/bin` directory of the toolchain that built the libraries; the installer copies them. The libraries are loaded without searching `PATH`, so a copy elsewhere on the system is not used, and a library that needs one of them fails with error 126 (the specified module could not be found) although its `.prx` file is present. Do not place original PS4 or PS5 modules in `libs/`: Windows tries to load them as DLLs and fails with error 193 (not a valid Win32 application).

Direct memory (`sceKernelAllocateDirectMemory`) is committed in full when the title allocates it, not when its pages are first used. The system commit limit (installed memory plus page file size, the second value of Committed in Task Manager) must cover it together with all other committed memory. Otherwise the allocation throws `create direct memory backing of 0x<n> bytes (<m> MiB)` with the Windows error; enlarge the page file or close other applications.

## Relinker

The installer calls the relinker for you. To run it by hand:

```text
relinker --windows --registry <game>/eboot.bin <output>.exe
```

The input is a decrypted PS4 `eboot.bin` in a fake SELF container (magic `4F 15 3D 1D` with unencrypted, uncompressed segments), or a clean ELF. The relinker unwraps the container into its ELF image and promotes a standalone `PT_SCE_RELRO` segment to a writable `PT_LOAD` so the GOT it holds is mapped. See [ORBIS.md](../dev/ORBIS.md) for the PS4 input layout. The game's bundled modules are read from `sce_module/` (or `sce_modules/`, `prx/`) beside the input, or under the parent directory selected with `--sce-module-path <path>`; the option takes the parent of the module directory, not the directory itself.

The executable, registry, icon and desktop entry output files must not refer to the input executable or a bundled module, including through a hard link or symbolic link; these collisions fail before any output is written.

| Option                     | Effect                                                                                                                                                              |
|----------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `--windows`                | Produce a Windows PE executable. Without it the output format is Linux ELF, whatever the file name.                                                                  |
| `--windows-diagnostics`    | Include startup dependency diagnostics. Requires `--windows`.                                                                                                        |
| `--windows-gui`            | Select the Windows GUI subsystem instead of the console subsystem. Requires `--windows`.                                                                              |
| `--registry`               | Write `<output-stem>.registry.json` beside the executable, and `<output-stem>.<module>.registry.json` for each converted bundled module. The installer needs it.     |
| `--sce-module-path <path>` | Parent of the module directories. Default: the input's parent directory.                                                                                              |
| `--rpath <path>`           | Library search path; on Windows a nonempty ASCII path, with `$ORIGIN` meaning the executable directory. Default `$ORIGIN/libs`.                                       |
| `--autorun`                | Run the output after conversion, print its exit code, and wait for Enter.                                                                                             |
| `unused-filter=0\|1\|2`    | Filter unused imports (`0` keeps all, the default). Written without `--`, at most once.                                                                              |

Unknown options and extra positional arguments are errors; running `relinker` without arguments prints the syntax. The `--skip-sce-module`, `--exclude-sce-module`, `--skip-syscall-check` and `--lazy-binding` flags are deprecated debugging aids that make the result extremely unstable. Exit codes: `0` conversion succeeded, `1` invalid arguments, `2` conversion failed (the error is on stderr).

Unsupported or unexpected states strictly throw `std::runtime_error`. `what()` is printed to stderr and the process terminates.

## Diagnostics

- `ORBIS_AUDIO_DUMP=<folder>` writes WAV files of every audio port and of the final mix (`ORBIS_AUDIO_DUMP_START` and `ORBIS_AUDIO_DUMP_SECONDS` choose the window).
- A circular audio recorder keeps the last 30 s of port 2 and of the final mix. Creating or touching `dump_now.txt` in the working directory writes them to `anyps4/audio_dumps/flight-<stamp>/`, with `audio_levels.txt` and `ngs2_state.txt` (every NGS2 rack and voice). `ORBIS_AUDIO_RING=0` turns it off.
- `ORBIS_GNM_LOG=info` (or `debug`, `trace`) raises the renderer and NGS2 log level; the default is warnings and errors.
- [tools/import_audit.py](../../tools/import_audit.py) lists the system functions a converted game imports and whether the built libraries provide them, before the game is launched (needs Python 3 only):

```sh
relinker --windows --registry <game>/eboot.bin app.exe
python tools/import_audit.py app.registry.json app.libc.prx.guest.prx.registry.json --libs <install>/libs --modules <game>/sce_module
```

Every import falls into one class: `implemented`, `stub` (exported but throws `NotImplemented_nid_no_patch` when called), `absent` (no library exports it, so the loader fails) or `module` (it names a file of the title's own `--modules` directory). Exit codes: `0` nothing blocks loading, `1` there are `absent` imports or needed libraries without a file, `2` an input could not be read.
