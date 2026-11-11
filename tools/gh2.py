#!/usr/bin/env python3
"""GH2 Recomp: build Guitar Hero II (PS2, USA, SLUS-21447) from the player's own disc.

    tools/gh2.py build <Guitar Hero II (USA).iso>      ISO -> generated C++ -> native binary (Linux)
    tools/gh2.py build --windows <xwin dir> <iso>      the same, cross-compiled to a Windows exe,
                                                       packaged into build/GH2Recomp-win64.zip
    tools/gh2.py verify <Guitar Hero II (USA).iso>     check the disc against Redump

Instead of the ISO, build also takes the disc's executable (SLUS_214.47)
already copied out of it; either way it must be the retail v1.00 one.

The disc is required and only ever read. Nothing from it is in this repository:
not its executable, not its assets, not the recompiled code. config/gh2-retail.symbols
(GH Classic's) holds function addresses and names only.

Every function the game runs is GH2's own code, recompiled. Runtime handlers
replace only Sony library / libc functions, bound by GH2's own names (and
never those in config/gh2-stub-denylist.txt); gameplay code has no handlers.
"""
import hashlib
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
RECOMP = BUILD / "recomp"
TOOLS = BUILD / "tools"
RUNTIME = BUILD / "runtime"
RUNNER = ROOT / "lib" / "PS2Recomp" / "ps2xRuntime" / "src" / "runner"
CONFIG = ROOT / "config"

GH2_ELF_NAME = "SLUS_214.47"
GH2_ELF_SHA1 = "aea7bbb01eb6e56056c3a33771ed046090d39168"
GH2_DISC_SHA1 = "e1a3a81827aa4264984b4f3678de58ca3d881528"  # Redump, Guitar Hero II (USA) v1.00

# PCSX2 (GPL-3.0-or-later), whose GS is built standalone by pcsx2gs/ as the
# graphics backend, and SDL3 (zlib) for the window, input and audio. Pinned:
# both are used unmodified, fetched at build time, never vendored.
EXT = BUILD / "ext"
PCSX2_REPO = "https://github.com/PCSX2/pcsx2.git"
PCSX2_COMMIT = "6d5e7daacefef49a0a921fb89730c50b70d9119e"
SDL3_REPO = "https://github.com/libsdl-org/SDL.git"
SDL3_COMMIT = "829a65d769d935c4852f8159e964312c0957260a"  # release-3.4.18, as PCSX2 pins


def iso_root(f):
    f.seek(16 * 2048)
    pvd = f.read(2048)
    if pvd[1:6] != b"CD001":
        sys.exit("not an ISO 9660 image")
    root = pvd[156:190]
    lba, size = struct.unpack("<I", root[2:6])[0], struct.unpack("<I", root[10:14])[0]
    f.seek(lba * 2048)
    data = f.read(size)
    entries, i = {}, 0
    while i < size:
        n = data[i]
        if n == 0:
            i = (i // 2048 + 1) * 2048
            continue
        rec = data[i:i + n]
        name = rec[33:33 + rec[32]].decode("ascii", "replace").split(";")[0]
        entries[name] = (struct.unpack("<I", rec[2:6])[0], struct.unpack("<I", rec[10:14])[0])
        i += n
    return entries


def read_file(image, name):
    with open(image, "rb") as f:
        entries = iso_root(f)
        if name not in entries:
            return None
        lba, size = entries[name]
        f.seek(lba * 2048)
        return f.read(size)


def boot_elf(image, name, sha1):
    """The disc's boot executable, refused unless it is exactly the expected one.
    image is the disc image, or that executable already copied out of it."""
    if Path(image).stat().st_size < 64 << 20:
        data = Path(image).read_bytes()
        if data[:4] == b"\x7fELF":
            got = hashlib.sha1(data).hexdigest()
            if got != sha1:
                sys.exit(f"{image}: SHA-1 {got}, expected {sha1}; not the retail {name}")
            return data
    data = read_file(image, name)
    if data is None:
        sys.exit(f"{image}: no {name} on this disc; GH2 Recomp needs Guitar Hero II (USA), SLUS-21447")
    got = hashlib.sha1(data).hexdigest()
    if got != sha1:
        sys.exit(f"{image}: {name} has SHA-1 {got}, expected {sha1}; the disc is not a clean "
                 "Guitar Hero (USA) v1.00 dump")
    return data


def run(cmd, log, env=None):
    print(f"  {Path(cmd[0]).name} ... ", end="", flush=True)
    with open(log, "w") as out:
        r = subprocess.run([str(c) for c in cmd], stdout=out, stderr=subprocess.STDOUT, env=env)
    print("ok" if r.returncode == 0 else f"FAILED (see {log})")
    if r.returncode:
        sys.exit(r.returncode)


def fetch(repo, commit, dest):
    """A pinned checkout of a dependency, fetched once."""
    if (dest / ".git").exists():
        head = subprocess.run(["git", "-C", str(dest), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
        if head == commit:
            return dest
    dest.mkdir(parents=True, exist_ok=True)
    for cmd in (["git", "init", "-q"], ["git", "fetch", "-q", "--depth", "1", repo, commit],
                ["git", "checkout", "-q", "FETCH_HEAD"]):
        r = subprocess.run(cmd, cwd=dest)
        if r.returncode:
            sys.exit(f"could not fetch {repo} at {commit}")
    return dest


def linux_toolchain():
    """Clang and lld, as the Linux build is tested: ThinLTO links the
    generated code in minutes, where GCC's whole-program LTO takes hours,
    so with no Clang the build goes without LTO."""
    for cc, cxx in (("clang-20", "clang++-20"), ("clang", "clang++")):
        if shutil.which(cc) and shutil.which(cxx):
            flags = [f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_CXX_COMPILER={cxx}"]
            probe = subprocess.run([cc, "-fuse-ld=lld", "-x", "c", "-", "-o", str(BUILD / "lld-probe")],
                                   input="int main(void) { return 0; }\n", capture_output=True, text=True)
            if probe.returncode == 0:
                flags.append(f"-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -Wl,--thinlto-cache-dir={BUILD / 'lto-cache'}")
            else:
                flags.append("-DPS2X_RELEASE_IPO=OFF")
            return flags
    return ["-DPS2X_RELEASE_IPO=OFF"]


def build_tools():
    if (TOOLS / "ps2xRecomp" / "ps2_recomp").exists() and (TOOLS / "ps2xAnalyzer" / "ps2_analyzer").exists():
        return
    run(["cmake", "-S", ROOT / "lib" / "PS2Recomp", "-B", TOOLS, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         "-DPS2X_BUILD_RUNTIME=OFF", "-DPS2X_BUILD_TEST=OFF", "-DPS2X_BUILD_STUDIO=OFF"], BUILD / "tools-configure.log")
    run(["cmake", "--build", TOOLS, "--target", "ps2_analyzer", "ps2_recomp"], BUILD / "tools.log")


def cmd_build(argv):
    xwin = None
    if argv[:1] == ["--windows"]:
        if len(argv) < 3:
            sys.exit(__doc__)
        xwin, argv = Path(argv[1]).resolve(), argv[2:]
    if not argv:
        sys.exit(__doc__)
    image = Path(argv[0])
    import os
    sys.path.insert(0, str(ROOT / "tools"))
    import symbolize  # noqa: E402  (GHClassic's: writes a symbols file into the ELF)

    shutil.rmtree(RECOMP, ignore_errors=True)
    RECOMP.mkdir(parents=True)
    print("reading the disc's executable")
    elf = boot_elf(image, GH2_ELF_NAME, GH2_ELF_SHA1)
    (RECOMP / "retail.elf").write_bytes(elf)

    print("naming its functions")
    run([sys.executable, ROOT / "tools" / "symbolize.py", RECOMP / "retail.elf",
         CONFIG / "gh2-retail.symbols", RECOMP / "gh2.elf"], RECOMP / "symbolize.log")

    build_tools()
    toml = RECOMP / "gh2.toml"
    env = dict(os.environ, PS2RECOMP_SCE_MATCHES_OUT=str(RECOMP / "sce-matches.txt"))
    print("analysing")
    run([TOOLS / "ps2xAnalyzer" / "ps2_analyzer", RECOMP / "gh2.elf", toml], RECOMP / "analyzer.log", env)
    # The names are GH2's own (GH Classic's list): every one counts as
    # proven, so the gate only adds the denylist and writes its report.
    status = RECOMP / "gh2-retail.status"
    with open(status, "w") as out:
        out.write("# gh2 address, name, match, score: GH2's own names\n")
        for line in (CONFIG / "gh2-retail.symbols").read_text().splitlines():
            if line and not line.startswith("#"):
                addr, name = line.split()[:2]
                out.write(f"{addr} {name} exact 1.0\n")
    print("binding runtime handlers")
    run([sys.executable, ROOT / "tools" / "gh1_stub_gate.py", toml, RECOMP / "sce-matches.txt",
         status, CONFIG / "gh2-stub-denylist.txt", RECOMP / "stub-gate.txt"], RECOMP / "gate.log")
    print("recompiling")
    run([TOOLS / "ps2xRecomp" / "ps2_recomp", toml], RECOMP / "recomp.log")

    # The runtime's runner globs src/runner/*.cpp. Everything there is
    # generated from the disc's executable: the folder is gitignored and
    # must never be committed or shared.
    # Only files whose text changed are replaced, so building again (after
    # an interrupted build, say) recompiles only what is new.
    RUNNER.mkdir(parents=True, exist_ok=True)
    generated = {f.name: f for f in (RECOMP / "output").iterdir()}
    for f in RUNNER.iterdir():
        if f.suffix in (".cpp", ".h") and f.name not in generated:
            f.unlink()
    for name, f in generated.items():
        dest = RUNNER / name
        if not dest.exists() or dest.read_bytes() != f.read_bytes():
            shutil.copy2(f, dest)

    print("fetching PCSX2 (its GS) and SDL3")
    pcsx2 = fetch(PCSX2_REPO, PCSX2_COMMIT, EXT / "pcsx2")
    sdl3 = fetch(SDL3_REPO, SDL3_COMMIT, EXT / "SDL")

    print("building the game")
    common = ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
              "-DPS2X_BUILD_RUNTIME=ON", "-DPS2X_BUILD_TEST=OFF", "-DPS2X_BUILD_STUDIO=OFF",
              "-DPS2X_BUILD_RECOMP=OFF", "-DPS2X_BUILD_ANALYZER=OFF",
              # GH2 sizes its heaps from the top of its 32 MB; the runtime's own
              # memory goes above that.
              "-DPS2X_EE_RAM_MB=64", "-DPS2X_GUEST_RAM_MB=32",
              "-DPS2X_ENABLE_DEBUG_UI=OFF", "-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF", "-DPS2X_ENABLE_SCCACHE=OFF",
              f"-DPS2X_GAME_DIR={ROOT / 'gh2'}", f"-DGH1_PCSX2_DIR={pcsx2}", f"-DGH1_SDL3_DIR={sdl3}"]
    if xwin is None:
        if not (RUNTIME / "build.ninja").exists():
            # The generated code uses SSE4.1 for the EE's vector ops.
            run(["cmake", "-S", ROOT / "lib" / "PS2Recomp", "-B", RUNTIME, *common, *linux_toolchain(),
                 "-DCMAKE_C_FLAGS=-march=x86-64-v2", "-DCMAKE_CXX_FLAGS=-march=x86-64-v2"],
                BUILD / "runtime-configure.log")
        run(["cmake", "--build", RUNTIME, "--target", "ps2EntryRunner"], BUILD / "runtime.log")
        print(f"built {RUNTIME / 'ps2xRuntime' / 'ps2EntryRunner'}")
        return

    # Windows: clang-cl and lld-link against Microsoft's C runtime and SDK as
    # xwin lays them out (tools/cmake/windows-clang-cl.cmake sets SSE4.1).
    win = BUILD / "win-runtime"
    if not (win / "build.ninja").exists():
        run(["cmake", "-S", ROOT / "lib" / "PS2Recomp", "-B", win, *common,
             f"-DCMAKE_TOOLCHAIN_FILE={ROOT / 'tools' / 'cmake' / 'windows-clang-cl.cmake'}", f"-DXWIN_DIR={xwin}",
             "-DPS2X_RELEASE_IPO=OFF", "-DPS2X_SHOW_WINDOWS_CONSOLE=OFF"],
            BUILD / "win-runtime-configure.log")
    run(["cmake", "--build", win, "--target", "ps2EntryRunner"], BUILD / "win-runtime.log")
    out = BUILD / "GH2Recomp-win64.zip"
    run([sys.executable, ROOT / "tools" / "package_windows.py", "--game", "gh2", win, out], BUILD / "package.log")
    print(f"built {out}: for you only, never share it (it holds code recompiled from your disc)")


def cmd_verify(argv):
    if not argv:
        sys.exit(__doc__)
    h = hashlib.sha1()
    with open(argv[0], "rb") as f:
        while chunk := f.read(1 << 24):
            h.update(chunk)
    ok = h.hexdigest() == GH2_DISC_SHA1
    print(f"{argv[0]}: {h.hexdigest()} {'matches Redump, Guitar Hero II (USA) v1.00' if ok else 'does NOT match Redump'}")
    sys.exit(0 if ok else 1)


def main():
    cmds = {"build": cmd_build, "verify": cmd_verify}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds:
        sys.exit(__doc__)
    cmds[sys.argv[1]](sys.argv[2:])


if __name__ == "__main__":
    main()
