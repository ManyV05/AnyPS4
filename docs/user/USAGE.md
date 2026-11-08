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

## Add to Steam

**Add to Steam** on the launcher's Home page adds a non-Steam shortcut for the title to your Steam library:

1. Close Steam completely (Steam menu > Exit). Steam rewrites its shortcuts when it closes, so the button refuses to edit them while Steam runs.
2. Press **Add to Steam**. The launcher finds Steam through `HKCU\Software\Valve\Steam\SteamPath`, picks the signed-in account (`ActiveUser`, or the most recently used account folder) and adds an entry to `userdata/<account>/config/shortcuts.vdf` that runs `anyps4_launcher.exe --play`. The original file is kept once as `shortcuts.vdf.anyps4.bak`, and the other shortcuts are left as they are.
3. Start Steam. The entry appears in the library; pressing it starts the game without opening the launcher window, and Steam counts the play time because the launcher waits for the game to exit. The app id is the one Steam derives for non-Steam shortcuts (CRC-32 of the quoted executable path plus the name, with the top bit set).

`--play` starts the installed title with the settings saved in `launcher.ini` and shows an error box if it is not installed. A badge on the Home page shows when the shortcut exists; pressing the button again refreshes it.

The artwork goes to `userdata/<account>/config/grid/` as `<app id>p.png` (vertical capsule, 600x900), `<app id>.png` (horizontal header, 920x430), `<app id>_hero.png` (hero banner, 1920x620) and `<app id>_logo.png` (transparent logo). The launcher builds all four from `sce_sys/pic1.png` of your own game folder; no images are shipped or downloaded. To use other pictures, put files named `vertical`, `horizontal`, `hero` or `logo` (PNG, JPEG, BMP, GIF or TIFF; `grid_vertical`, `grid_horizontal`, `capsule`, `poster`, `header` and `banner` also work) in a `steam_art` folder inside the install folder or next to the launcher. A file found there replaces the generated one for that role. WebP and AVIF files are not supported.

The `ANYPS4_STEAM_DIR` environment variable points the launcher at another Steam folder, which is useful for trying the button without touching the real library.

## Saves and backups

**Export Saves** and **Import Saves** (Settings, under Paths and saves) back up and restore `savedata/` of the install folder:

- Export asks for a file name (the default is `AnyPS4_SaveBackup_YYYYMMDD.zip`) and writes a compressed ZIP with every file under `savedata/`.
- Import asks for a ZIP, asks for confirmation and replaces `savedata/` with its contents. The current saves are not deleted: they are moved to `savedata_backup_<date>-<time>` next to them. ZIPs made by this launcher, by Windows Explorer or by other tools work, with or without the `savedata/` folder inside; a ZIP with paths that leave the folder (`..`) is refused and nothing is changed. Import is refused while the game runs.

Both can also run without a window, which suits scripts: `anyps4_launcher.exe --export-saves <file.zip>` and `anyps4_launcher.exe --import-saves <file.zip>` use the install folder from `launcher.ini` and exit with code 0 on success and 1 on failure (the message goes to the console that started them).

## Volume while the game runs

The music, voices and effects sliders and the music floor in Settings act on a running game at once: the launcher writes the new values into a shared memory block named `Local\AnyPS4.LiveAudio.<game process id>` that `libSceAudioOut.prx` and `libSceNgs2.prx` read on every audio block, so nothing has to be restarted. The launcher finds the game it started, or any running copy of the installed executable. The block starts with the values from the environment variables (`ORBIS_AUDIO_BGM_GAIN`, `ORBIS_AUDIO_MAIN_GAIN`, `ORBIS_NGS2_MUSIC_FLOOR`), so a title started without the launcher behaves as before. Needs libraries built from this version: run Install again (or use the standalone package of this version).

## Discord Rich Presence

While the game runs, the launcher can show "Playing The Witch and the Hundred Knight: Revival Edition" with the line "AnyPS4 Runtime" and a timer of the session on your Discord profile. It talks to the Discord desktop app through its local IPC pipe, so Discord has to be running, and the presence is cleared when the game closes (it also disappears if you close the launcher while playing, except when the game was started by the Steam entry, where the launcher stays open until the game ends).

Discord needs an application id for this: create an application named after the game at discord.com/developers (its name is what Discord shows after "Playing"), copy its Application ID into the **Discord application id** field in Settings, and keep **Discord Rich Presence** ticked. The id is stored in `launcher.ini` (`discord`, `discord_client_id`). Without a valid id nothing is sent and the Home page log says so. `ANYPS4_DISCORD_CLIENT_ID` overrides the id and `ANYPS4_DISCORD_PIPE` names another pipe, for tests.

## Game language

**Game language** in Settings chooses the console language the title is told: English (US), the default, or Japanese. The launcher saves it in `launcher.ini` (`language=1` for English (US), `language=0` for Japanese) and starts the game with `ORBIS_SYSTEM_PARAM_LANGUAGE` set to that number; the runtime answers the title's `sceSystemServiceParamGetInt` language query with it, and with 1 (English US) when the variable is not set or not a console language number from 0 to 29. The menus and text follow the setting. The artwork of the title screen, including its Japanese logo, is the same in every language. Two more answers come from the game's own files, as on a console: `sceAppContentAppParamGetInt` returns `USER_DEFINED_PARAM_1` to `USER_DEFINED_PARAM_4` from `app0/sce_sys/param.sfo` (this title reads the first one at start-up and uses it to pick its Western or Japanese edition: the English logo and copyright on the title screen), and `sceSystemServiceParamGetInt` answers the enter button assignment (parameter 1000) with Cross (1); the launcher sets `ORBIS_SYSTEM_PARAM_ENTER_BUTTON_ASSIGN=1`, and 0 would make Circle the enter button. The libraries have to come from this version: run Install again (or use the standalone package of this version).

## Window size and fullscreen

**Resolution** and **Borderless Fullscreen** in Settings (Graphics) choose how the game window opens. The resolution list has 1280x720, 1920x1080 (the default), 2560x1440 and 3840x2160 and sets the size of the window's drawing area; the title itself always renders at its own resolution and the renderer scales the picture to the window. A size that does not fit on the screen is reduced, keeping its proportions. Borderless Fullscreen opens a window without title bar or frame that covers the whole primary monitor, and the resolution list is then ignored. The launcher saves them in `launcher.ini` (`resolution=1920x1080`, `borderless=0`) and starts the game with `ORBIS_WINDOW_WIDTH`, `ORBIS_WINDOW_HEIGHT` and `ORBIS_WINDOW_BORDERLESS`, which `libSceVideoOut.prx` reads when it creates the window; without them the window is sized as before. The libraries have to come from this version: run Install again (or use the standalone package of this version).

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
