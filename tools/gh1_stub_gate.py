#!/usr/bin/env python3
"""Decide which GH1 functions may be swapped for a runtime handler.

    tools/gh1_stub_gate.py <gh1.toml> <sce-matches.txt> <gh1-retail.status> <stub-denylist.txt> <report>

The analyzer lists `name@address` for every function whose name has a runtime
handler (Sony's libraries, libc), and the recompiler binds those by address,
replacing GH1's code there with the handler. A binding is kept only when the
name at that address is proven by one of:

  - the analyzer's SCE SDK scanner: a hash of the function's code, relocations
    masked, equal to a known Sony library object (sce-matches.txt, written by
    ps2_analyzer when PS2RECOMP_SCE_MATCHES_OUT is set), or
  - an exact match from GH2 retail: same instructions with addresses masked
    (gh1-retail.status, from tools/gh1_names.py).

A fuzzy GH2 match is never enough: in libpad it lands one function off, which
would have put scePadRead's handler on scePadGetState. Unproven bindings are
removed, so GH1 runs its own code there. The gate only ever removes bindings,
and game logic has no runtime handlers, so gameplay code is never replaced.

The toml is rewritten in place, with stub_denylist added to [general].
"""
import re
import sys


def main():
    if len(sys.argv) != 6:
        sys.exit(__doc__)
    toml_path, sce_path, status_path, deny_path, report_path = sys.argv[1:]

    sce = {}
    for line in open(sce_path):
        if not line.startswith("#"):
            addr, _, lib, name = line.split()
            sce[int(addr, 16)] = (name, lib)
    exact = {}
    for line in open(status_path):
        if not line.startswith("#"):
            addr, name, status, _ = line.split()
            if status in ("exact", "exact_by_order"):
                exact[int(addr, 16)] = name

    text = open(toml_path).read()
    head, rest = text.split("stubs = [\n", 1)
    block, tail = rest.split("]\n", 1)
    entries = re.findall(r'"([^"]+)"', block)

    kept, report = [], []
    for entry in entries:
        name, _, at = entry.partition("@")
        if not at:
            report.append(f"drop   {name:<32} no address")
            continue
        addr = int(at, 16)
        if sce.get(addr, (None,))[0] == name:
            kept.append(entry)
            report.append(f"bind   {name:<32} {addr:08x} sdk-hash {sce[addr][1]}")
        elif exact.get(addr) == name:
            kept.append(entry)
            report.append(f"bind   {name:<32} {addr:08x} gh2-exact")
        else:
            report.append(f"drop   {name:<32} {addr:08x} unproven, keeps GH1's code")

    deny = [l.strip() for l in open(deny_path) if l.strip() and not l.startswith("#")]
    block = "".join(f'  "{e}",\n' for e in kept)
    text = head + "stubs = [\n" + block + "]\n" + tail
    if "[general]\n" not in text:
        sys.exit("no [general] table in the toml; the stub denylist was not applied")
    text = text.replace("[general]\n",
                        "[general]\nstub_denylist = [" + ",".join(f'"{n}"' for n in deny) + "]\n", 1)
    open(toml_path, "w").write(text)
    open(report_path, "w").write("\n".join(report) + "\n")
    print(f"{len(entries)} handler bindings: {len(kept)} proven and kept, {len(entries) - len(kept)} dropped")


if __name__ == "__main__":
    main()
