"""Install a PS4 title for AnyPS4: relink its eboot.bin, generate the runtime libraries and lay out the output folder.

Steps, each logged on its own line so a launcher can show progress:
  1. relinker --windows --registry <game>/eboot.bin <out>/<name>.exe
  2. tools/stub_generator.py --registry <out>/<name>.registry.json --eboot ... --shadps4 ... --vendored ...
  3. cmake --build <build-dir> --target orbis_libs  (configured once with ANYPS4_LIBS_DIR=<out>/libs if the build dir is empty)
  4. <out>/app0 junction to the game folder, <out>/anyps4 and <out>/savedata directories

The game files are never copied: app0 is a directory junction to the folder the user already owns.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
VENDORED = [
    "libSceGnmDriver_padebug.prx=3rdparty/shadps4-gnm:libSceGnmDriver",
    "libSceNgs2.prx=3rdparty/shadps4-ngs2:libSceNgs2",
]


def log(text: str) -> None:
    print(text, flush=True)


def run(command: list[str], cwd: Path | None = None) -> int:
    log("$ " + " ".join(f'"{c}"' if " " in c else c for c in command))
    process = subprocess.Popen(command, cwd=str(cwd) if cwd else None, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                               encoding="utf-8", errors="replace")
    assert process.stdout is not None
    for line in process.stdout:
        log("  " + line.rstrip())
    return process.wait()


def find_relinker(explicit: str | None, build_dir: Path) -> Path | None:
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    if os.environ.get("ANYPS4_RELINKER"):
        candidates.append(Path(os.environ["ANYPS4_RELINKER"]))
    candidates += [build_dir / "core" / "relinker" / "relinker.exe", build_dir.parent / "build-relinker" / "core" / "relinker" / "relinker.exe"]
    candidates += [p for p in (REPO / "build").glob("**/relinker.exe")] if (REPO / "build").exists() else []
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", required=True, type=Path, help="folder with eboot.bin and sce_sys/ (the user's own dump)")
    parser.add_argument("--out", required=True, type=Path, help="install folder: <name>.exe, libs/, app0 -> game, savedata/")
    parser.add_argument("--name", default=None, help="executable name without .exe (default: TITLE_ID from the game folder name)")
    parser.add_argument("--shadps4", type=Path, default=Path(os.environ.get("ANYPS4_SHADPS4", "")), help="shadPS4 checkout used to name functions")
    parser.add_argument("--build-dir", type=Path, default=Path(os.environ.get("ANYPS4_BUILD_DIR", str(REPO / "build-anyps4"))), help="CMake build directory for the runtime libraries")
    parser.add_argument("--relinker", default=None, help="relinker.exe (default: searched in the build directories)")
    parser.add_argument("--toolchain-bin", default=os.environ.get("ANYPS4_TOOLCHAIN_BIN", ""), help="directory with gcc/g++ to prepend to PATH")
    parser.add_argument("--cmake", default=os.environ.get("ANYPS4_CMAKE", "cmake"))
    parser.add_argument("--ninja", default=os.environ.get("ANYPS4_NINJA", ""), help="ninja executable (for a fresh configure)")
    parser.add_argument("--skip-build", action="store_true", help="only relink and generate; reuse the libraries already in <out>/libs")
    parser.add_argument("--pool", default=None, help="directory of <name>.json records for every installed title (default %%APPDATA%%/AnyPS4/registries); the libraries are generated for the union of them")
    parser.add_argument("--image-base", type=lambda v: int(v, 0), default=0, help="experimental: rebase the relinked executable to this address with ASLR off (default 0: keep the relinker's 0x140000000 with ASLR; 0x400000 does not start on Windows yet, see GNM_PLAN.md 4.4.13)")
    args = parser.parse_args()

    game = args.game.resolve()
    out = args.out.resolve()
    eboot = game / "eboot.bin"
    if not eboot.is_file():
        log(f"error: {eboot} not found")
        return 2
    name = args.name or game.name
    if out == game or game in out.parents or out in game.parents:
        log(f"error: the install folder ({out}) must be separate from the game folder ({game}); app0 becomes a junction to the game")
        return 2
    out.mkdir(parents=True, exist_ok=True)
    if args.toolchain_bin:
        os.environ["PATH"] = args.toolchain_bin + os.pathsep + os.environ["PATH"]

    log(f"== 1/4 relinking {eboot} -> {out / (name + '.exe')}")
    relinker = find_relinker(args.relinker, args.build_dir)
    if relinker is None:
        log("error: relinker.exe not found; build it with cmake -DANYPS5_RELINKER_ONLY=ON or pass --relinker")
        return 3
    code = run([str(relinker), "--windows", "--registry", str(eboot), str(out / (name + ".exe"))], cwd=game.parent)
    if code != 0:
        log(f"error: relinker exited with {code}")
        return code
    registry = out / (name + ".registry.json")
    if not registry.is_file():
        log(f"error: {registry} was not written")
        return 4
    if args.image_base:
        from anyps4_rebase import rebase
        old_base, new_base = rebase(out / (name + ".exe"), args.image_base)
        log(f"  image base {old_base:#x} -> {new_base:#x}, ASLR off")

    log("== 2/4 generating the runtime libraries' sources")
    pool = Path(args.pool) if args.pool else Path(os.environ.get("APPDATA", str(REPO))) / "AnyPS4" / "registries"
    pool.mkdir(parents=True, exist_ok=True)
    (pool / (name + ".json")).write_text(json.dumps({"registry": str(registry), "eboot": str(eboot)}, indent=1), encoding="utf-8")
    titles = []
    for entry in sorted(pool.glob("*.json")):
        try:
            info = json.loads(entry.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if Path(info.get("registry", "")).is_file() and Path(info.get("eboot", "")).is_file():
            titles.append((info["registry"], info["eboot"]))
        else:
            log(f"  skipping {entry.name}: its registry or eboot is gone")
    log(f"  union of {len(titles)} installed title(s) from {pool}")
    command = [sys.executable, str(REPO / "tools" / "stub_generator.py")]
    for title_registry, title_eboot in titles:
        command += ["--registry", title_registry, "--eboot", title_eboot]
    if args.shadps4 and str(args.shadps4) != ".":
        command += ["--shadps4", str(args.shadps4)]
    for spec in VENDORED:
        command += ["--vendored", spec]
    code = run(command, cwd=REPO)
    if code != 0:
        log(f"error: stub generator exited with {code}")
        return code

    libs = out / "libs"
    if args.skip_build:
        log("== 3/4 build skipped (--skip-build)")
    else:
        log(f"== 3/4 building the runtime libraries into {libs}")
        build_dir = args.build_dir.resolve()
        if not (build_dir / "CMakeCache.txt").is_file():
            configure = [args.cmake, "-S", str(REPO), "-B", str(build_dir), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                         f"-DANYPS4_LIBS_DIR={libs.as_posix()}"]
            if args.ninja:
                configure.append(f"-DCMAKE_MAKE_PROGRAM={args.ninja}")
            code = run(configure)
            if code != 0:
                log(f"error: cmake configure exited with {code}")
                return code
        code = run([args.cmake, "--build", str(build_dir), "--target", "orbis_libs"])
        if code != 0:
            log(f"error: build exited with {code}")
            return code
        configured_libs = None
        for line in (build_dir / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("ANYPS4_LIBS_DIR:PATH="):
                configured_libs = Path(line.split("=", 1)[1])
        if configured_libs is not None and configured_libs.resolve() != libs.resolve():
            libs.mkdir(parents=True, exist_ok=True)
            copied = 0
            for library in list(configured_libs.glob("*.prx")) + list(configured_libs.glob("*.dll")):
                shutil.copy2(library, libs / library.name)
                copied += 1
            log(f"  copied {copied} libraries from {configured_libs}")

    log("== 4/4 laying out the install folder")
    runtime = ["libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll"]
    missing = [name for name in runtime if not (libs / name).is_file()]
    if missing and args.toolchain_bin:
        for name in missing:
            source = Path(args.toolchain_bin) / name
            if source.is_file():
                shutil.copy2(source, libs / name)
                log(f"  runtime {name} copied from the toolchain")
    missing = [name for name in runtime if not (libs / name).is_file()]
    if missing:
        log("warning: the libraries need " + ", ".join(missing) + " next to them; copy them from the compiler's bin directory")
    app0 = out / "app0"
    if not app0.exists():
        if os.name == "nt":
            code = subprocess.call(["cmd", "/c", "mklink", "/J", str(app0), str(game)], stdout=subprocess.DEVNULL)
            if code != 0:
                log("error: could not create the app0 junction")
                return 5
        else:
            app0.symlink_to(game, target_is_directory=True)
        log(f"  app0 -> {game}")
    for folder in ("anyps4", "savedata"):
        (out / folder).mkdir(exist_ok=True)
    log(f"done: {out / (name + '.exe')} with {len(list(libs.glob('*.prx')))} libraries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
