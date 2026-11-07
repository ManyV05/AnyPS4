# AnyPS4: The Witch and the Hundred Knight: Revival Edition

Dedicated port and runtime to play **The Witch and the Hundred Knight: Revival Edition (CUSA02799)** natively on Windows.

**This project was developed and tested ONLY for The Witch and the Hundred Knight: Revival Edition (CUSA02799). The primary goal is to offer a native, stable, and smooth execution experience dedicated exclusively to this game.**

It is built on [AnyPS5](https://github.com/boykopovar/AnyPS5): a [relinker](core/relinker) converts the game's executable to a native Windows executable, and the system libraries it imports are provided as `.prx` libraries that the executable loads directly ([PS4 modules](core/libs/orbis), [shared libraries](core/libs/prx)). There is no emulation and no separate runtime process. Graphics go through a Vulkan renderer taken from [shadPS4](https://github.com/shadps4-emu/shadPS4) ([3rdparty/shadps4-gnm](3rdparty/shadps4-gnm)) and audio through a software NGS2 implementation ([3rdparty/shadps4-ngs2](3rdparty/shadps4-ngs2)).

[Usage](docs/user/USAGE.md), [Build instructions](docs/dev/BUILD.md), [Architecture](docs/dev/ARCHITECTURE.md), [Graphics and runtime notes](docs/dev/GNM_PLAN.md), [code style conventions](docs/dev/CONVENTIONS.md)

## What it does for this game

- Relinks the game's `eboot.bin` and generates the Orbis libraries the title imports.
- Save data, English language, keyboard and XInput pad input, audio output and a launcher with the settings the game needs.
- Diagnostics for this game: a circular audio recorder (`dump_now.txt` in the working directory writes the last 30 s of audio and a snapshot of the NGS2 voices), `ORBIS_AUDIO_DUMP`, and the renderer traces described in [GNM_PLAN](docs/dev/GNM_PLAN.md).

Unsupported or unexpected states strictly throw `std::runtime_error`. `what()` is printed to stderr and the process terminates.

## Input mapping

Keyboard keys are mapped in the launcher and saved in `anyps4/pad.ini` beside the executable (`ORBIS_PAD_CONFIG` points to another file). An XInput controller in slot 1 is used when connected. Details are in [GNM_PLAN](docs/dev/GNM_PLAN.md).

## Disclaimer

This project was developed and tested ONLY for The Witch and the Hundred Knight: Revival Edition (CUSA02799). The primary goal is to offer a native, stable, and smooth execution experience dedicated exclusively to this game. Other titles are not supported and have not been tested.

This project is intended for interoperability, research, preservation, and compatibility purposes. It does not include, distribute, or require copyrighted software, firmware, cryptographic keys, or proprietary libraries. The installer links the game folder you provide and never copies its files. Users are responsible for ensuring that any binaries used with this project are obtained and used in accordance with applicable laws and their respective license terms.

## License

This project is licensed under the GNU General Public License version 2 only. It includes code from AnyPS5 and shadPS4, which keep their own copyright notices.
