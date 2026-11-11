#!/usr/bin/env python3
"""Packages the Windows build: GH1Recomp.exe (or GH2Recomp.exe), the DLLs it
loads, settings, the readme and every licence, zipped.

    tools/package_windows.py [--game gh1|gh2] <build dir> <output .zip>

The exe holds code recompiled from the player's own disc: the package is
for that player. Nothing from the disc itself goes in (the exe reads the
disc image the player supplies)."""

import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Each game's name, the folder its settings and readme come from, and its disc.
GAMES = {
    "gh1": ("GH1", ROOT / "dist" / "windows", "Guitar Hero (USA), SLUS-21224 v1.00"),
    "gh2": ("GH2", ROOT / "dist" / "windows-gh2", "Guitar Hero II (USA), SLUS-21447 v1.00"),
}
GAME = "gh1"
OBJDUMP = shutil.which("llvm-objdump-20") or shutil.which("llvm-objdump")


def imports(binary):
    out = subprocess.run([OBJDUMP, "-p", str(binary)], capture_output=True, text=True, check=True).stdout
    return [line.split()[-1] for line in out.splitlines() if "DLL Name" in line]


def needed_dlls(exe, search):
    """The DLLs beside the build's exe that it loads, directly or through another."""
    local = {p.name.lower(): p for p in search.glob("*.dll")}
    need, todo = {}, [exe]
    while todo:
        for name in imports(todo.pop()):
            path = local.get(name.lower())
            if path and path.name not in need:
                need[path.name] = path
                todo.append(path)
    return sorted(need.values())


def cache_path(build, key):
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if line.startswith(key + ":"):
            return Path(line.split("=", 1)[1])
    sys.exit(f"{key} is not set in {build}")


def licence_files(build):
    pcsx2 = cache_path(build, "GH1_PCSX2_DIR")
    sdl3 = cache_path(build, "GH1_SDL3_DIR")
    return [
        (f"{GAMES[GAME][0]} Recomp", ROOT / "LICENSE"),
        ("PS2Recomp (runtime)", ROOT / "lib" / "PS2Recomp" / "LICENSE"),
        ("PCSX2: GS, microVU and common code (GPL-3.0-or-later)", pcsx2 / "COPYING.GPLv3"),
        ("SDL3", sdl3 / "LICENSE.txt"),
        ("raylib", build / "_deps" / "raylib-src" / "LICENSE"),
        ("Dear ImGui (in PCSX2's GS)", pcsx2 / "3rdparty" / "imgui" / "LICENSE.txt"),
        ("{fmt} (in PCSX2's GS)", pcsx2 / "3rdparty" / "fmt" / "LICENSE"),
        ("cpuinfo (in PCSX2's GS)", pcsx2 / "3rdparty" / "cpuinfo" / "LICENSE"),
        ("fast_float (in PCSX2's GS)", pcsx2 / "3rdparty" / "fast_float" / "LICENSE-MIT"),
        ("SDL_GameControllerDB (gamecontrollerdb.txt)", ROOT / "dist" / "windows" / "gamecontrollerdb-LICENSE.txt"),
    ]


def main():
    global GAME
    args = sys.argv[1:]
    if args[:1] == ["--game"] and len(args) >= 2 and args[1] in GAMES:
        GAME, args = args[1], args[2:]
    if len(args) != 2:
        sys.exit(__doc__)
    name, dist, disc = GAMES[GAME]
    build, out_zip = Path(args[0]).resolve(), Path(args[1]).resolve()
    runner = build / "ps2xRuntime"
    exe = runner / "ps2EntryRunner.exe"
    with tempfile.TemporaryDirectory() as tmp:
        stage = Path(tmp) / f"{name}Recomp-win64"
        (stage / "iso").mkdir(parents=True)
        shutil.copy2(exe, stage / f"{name}Recomp.exe")
        for dll in needed_dlls(exe, runner):
            shutil.copy2(dll, stage / dll.name)
        for f in (f"{name}Recomp.ini", "LEIA-ME.txt"):
            shutil.copy2(dist / f, stage / f)
        shutil.copy2(ROOT / "dist" / "windows" / "gamecontrollerdb.txt", stage / "gamecontrollerdb.txt")
        # The list of shader programs GH1 draws with, made by playing it: a
        # first run then prepares them all as it opens. Not in the public
        # repository (it is made from the game); each player's own runs
        # write theirs to cache\ anyway.
        programs = dist / f"{GAME}_programs.bin"
        if programs.is_file():
            shutil.copy2(programs, stage / programs.name)
        # Windows line ends in the texts meant to be opened, however git checked them out.
        for f in (f"{name}Recomp.ini", "LEIA-ME.txt"):
            text = stage / f
            text.write_bytes(text.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
        (stage / "iso" / "COLOQUE-A-ISO-AQUI.txt").write_text(
            f"Coloque aqui o .iso do seu {disc}.\r\n"
            f"Put your {disc} disc image here.\r\n")

        text = []
        for title, path in licence_files(build):
            text.append("=" * 78 + "\n" + title + "\n" + "=" * 78 + "\n\n" + path.read_text(errors="replace") + "\n")
        ffmpeg = build / "ThirdParty" / "ffmpeg-prefix" / "src" / "ffmpeg_external"
        for lic in sorted((ffmpeg / "licenses").glob("*")) if (ffmpeg / "licenses").is_dir() else []:
            if lic.is_file():
                text.append("=" * 78 + "\nFFmpeg DLLs: " + lic.name + "\n" + "=" * 78 + "\n\n" +
                            lic.read_text(errors="replace") + "\n")
        (stage / "LICENCAS.txt").write_text("\n".join(text).replace("\r\n", "\n").replace("\n", "\r\n"))

        out_zip.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(out_zip, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for path in sorted(stage.rglob("*")):
                z.write(path, path.relative_to(stage.parent))
    print(out_zip, out_zip.stat().st_size)


if __name__ == "__main__":
    main()
