#!/usr/bin/env python3
r"""readmap_report.py - what part of the game data did the game actually read?

Input: one or more readmap.txt files written by BSI-Quest with BSI_READMAP=1 (build 144+), and the game folder on the PC.
Several maps (your own sessions, or maps from play reports) are merged: a block counts as read when any map has it.

    python readmap_report.py readmap.txt [more maps ...] [--game "C:\...\BioShock Infinite"] [--top 25] [--never never.txt]

Output: per top-level folder and per file type, how much was read; the biggest files that were never opened; and
(--never) the list of never-opened files, as paths relative to the game folder.
It only reads; it never changes the game folder.
"""
import argparse
import collections
import os
import sys

BLOCK = 65536
DEFAULT_GAME = r"C:\Program Files (x86)\Steam\steamapps\common\BioShock Infinite"
PUSHED = ("Binaries", "Engine", "XGame", "DLC")   # what the installer copies to the headset


def load(path, maps):
    cur = None
    with open(path, "r", errors="replace") as f:
        for line in f:
            if line.startswith("F "):
                p = line.rstrip("\r\n").split(" ", 4)
                if len(p) < 5:
                    cur = None
                    continue
                key = p[4].replace("\\", "/").lower()
                cur = maps.setdefault(key, {"size": int(p[1]), "blocks": set(), "name": p[4]})
            elif line.startswith("R") and cur is not None:
                for tok in line[1:].split():
                    a, _, b = tok.partition("-")
                    cur["blocks"].update(range(int(a), int(b or a) + 1))


def mb(n):
    return f"{n / 1048576:10.1f} MB"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("maps", nargs="+")
    ap.add_argument("--game", default=DEFAULT_GAME)
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--never", help="write the list of never-opened files here")
    a = ap.parse_args()

    maps = {}
    for m in a.maps:
        load(m, maps)
    if not os.path.isdir(a.game):
        sys.exit(f"game folder not found: {a.game} (pass --game)")

    files = []
    for top in PUSHED:
        for root, _, names in os.walk(os.path.join(a.game, top)):
            for n in names:
                full = os.path.join(root, n)
                rel = os.path.relpath(full, a.game).replace("\\", "/")
                files.append((rel, os.path.getsize(full)))

    by_top = collections.defaultdict(lambda: [0, 0, 0])   # total, read, never-opened bytes
    by_ext = collections.defaultdict(lambda: [0, 0, 0])
    never = []
    partial = []
    for rel, size in files:
        e = maps.get(rel.lower())
        read = min(size, len(e["blocks"]) * BLOCK) if e else 0
        top = rel.split("/")[0] + ("/" + rel.split("/")[1] if rel.startswith(("DLC/", "XGame/")) and rel.count("/") > 1 else "")
        ext = os.path.splitext(rel)[1].lower() or "(none)"
        for d, k in ((by_top, top), (by_ext, ext)):
            d[k][0] += size
            d[k][1] += read
            if not e:
                d[k][2] += size
        if not e:
            never.append((size, rel))
        elif size - read > 8 * 1048576:
            partial.append((size - read, size, rel))

    total = sum(s for _, s in files)
    read = sum(v[1] for v in by_top.values())
    nev = sum(s for s, _ in never)
    print(f"maps merged: {len(a.maps)}; files in the maps: {len(maps)}; files on the PC that the installer copies: {len(files)}")
    print(f"total {mb(total)} | read {mb(read)} ({100 * read / total:.1f} %) | in files never opened {mb(nev)} ({100 * nev / total:.1f} %)")
    print("\nby folder:        total          read     never opened")
    for k, v in sorted(by_top.items(), key=lambda kv: -kv[1][0]):
        print(f"  {k:28s} {mb(v[0])} {mb(v[1])} {mb(v[2])}")
    print("\nby file type:     total          read     never opened")
    for k, v in sorted(by_ext.items(), key=lambda kv: -kv[1][0])[:12]:
        print(f"  {k:28s} {mb(v[0])} {mb(v[1])} {mb(v[2])}")
    print(f"\nbiggest files never opened (top {a.top}):")
    for s, rel in sorted(never, reverse=True)[:a.top]:
        print(f"  {mb(s)}  {rel}")
    print(f"\nbiggest unread parts of files that were opened (top {a.top}):")
    for un, size, rel in sorted(partial, reverse=True)[:a.top]:
        print(f"  {mb(un)} unread of {mb(size)}  {rel}")
    print("\nNote: 'never opened' is only as complete as the play behind the maps. A file is safe to leave off the headset")
    print("only when maps covering the whole game (and all DLC you keep) agree that it is never opened.")
    if a.never:
        with open(a.never, "w", newline="\n") as f:
            for s, rel in sorted(never, key=lambda t: t[1]):
                f.write(f"{s}\t{rel}\n")
        print(f"never-opened list written: {a.never} ({len(never)} files)")


if __name__ == "__main__":
    main()
