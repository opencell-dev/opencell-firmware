#!/usr/bin/env python3
"""lc2oc: rename OpenCell's legacy "lc" (LoRaCell) names to "oc" in one repository.

The mapping (a name is changed only where "lc"/"LC" starts a word: the
character before it is not a letter or digit; "_", "/", ".", "-" all count as
word starts):

    lcbench   -> ocbench      LCBENCH -> OCBENCH
    lcb_...   -> ocb_...      LCB_... -> OCB_...
    lc_...    -> oc_...       LC_...  -> OC_...
    -DLC_...  -> -DOC_...     (CMake/compiler options)
    import-lcb-hss -> import-ocb-hss   (plan 8's admin command)

Never changed: a bare "lc" word, "lc-" (branch and file names such as lc-core,
2026-09-26-lc-sig-c-side.md), "LCB1" (the bench payload's magic, on the air),
BLE UUIDs (6c63...: hex, no rule can match), words that merely contain lc
(calc_, esp_lcd_, ...), and every line that holds a keep marker (see
KEEP_LINE). Files under --skip globs and tools/rename/, and files that say
"lc2oc: keep-file" in their first 1 KiB, are left alone.

Usage:
    lc2oc.py [--repo DIR] [--skip GLOB]...            dry run: counts, collisions, moves
    lc2oc.py [--repo DIR] [--skip GLOB]... --apply    rewrite files, git mv paths
    lc2oc.py [--repo DIR] [--skip GLOB]... --check    exit 1 if a legacy name is left
    lc2oc.py --filter < IN > OUT                      rewrite a text stream (patches)
  --pre OLD=NEW  first rename the word OLD (also inside OLD_x, x_OLD) to NEW, in
                 both cases, before the lc mapping (plan 8: ocb=ocr, oc_cell=occ).
  --allow NAME   a new name that may already exist (a collision you checked).
"""
import argparse
import fnmatch
import os
import re
import subprocess
import sys

# (pattern, replacement), applied in this order.
RULES = [
    (re.compile(r"import-lcb-hss"), "import-ocb-hss"),
    (re.compile(r"(?<![A-Za-z0-9])lcbench(?![a-z0-9])"), "ocbench"),
    (re.compile(r"(?<![A-Za-z0-9])LCBENCH(?![A-Z0-9])"), "OCBENCH"),
    (re.compile(r"(?<![A-Za-z0-9])lcb(?=_)"), "ocb"),
    (re.compile(r"(?<![A-Za-z0-9])LCB(?=_)"), "OCB"),
    (re.compile(r"(?<![A-Za-z0-9])lc(?=_)"), "oc"),
    (re.compile(r"(?<![A-Za-z0-9])LC(?=_)"), "OC"),
    (re.compile(r"(?<=-D)LC(?=_)"), "OC"),
]

# A line that matches is left exactly as it is: the explicit marker, and the
# legacy NVS namespace definitions (#define ...NVS_NS "lc..."), which the
# NVS migration task moves by hand.
KEEP_LINE = re.compile(r'lc2oc:\s*keep|NVS_NS\s+"lc')

# What --check counts as a legacy name left behind (same word starts).
LEGACY = re.compile(r"(?<![A-Za-z0-9])(?:lcbench|LCBENCH|lcb_|LCB_|lc_|LC_)|(?<=-D)LC_")

ALWAYS_SKIP = ["tools/rename/*"]
KEEP_FILE = b"lc2oc: keep-file"  # in a file's first 1 KiB: the whole file is left alone


def pre_rules(pairs):
    out = []
    for old, new in pairs:
        for o, n in ((old.lower(), new.lower()), (old.upper(), new.upper())):
            out.append((re.compile(r"(?<![A-Za-z0-9])%s(?![A-Za-z0-9])" % re.escape(o)), n))
    return out


def rewrite_line(line, pre=()):
    if KEEP_LINE.search(line):
        return line
    for pat, rep in list(pre) + RULES:
        line = pat.sub(rep, line)
    return line


def rewrite_text(text, pre=()):
    return "".join(rewrite_line(l, pre) for l in text.splitlines(keepends=True))


def rewrite_path(path):
    return "/".join(rewrite_line(p) for p in path.split("/"))


def tokens(text):
    return set(re.findall(r"[A-Za-z0-9_]+", text))


def tracked(repo, skip):
    out = subprocess.run(["git", "-C", repo, "ls-files", "-s", "-z"], check=True,
                         capture_output=True).stdout.decode()
    files = []
    for rec in out.split("\0"):
        if not rec:
            continue
        meta, path = rec.split("\t", 1)
        if meta.split()[0] == "160000":  # a submodule: its own repository
            continue
        if any(fnmatch.fnmatch(path, g) for g in ALWAYS_SKIP + skip):
            continue
        try:
            with open(os.path.join(repo, path), "rb") as f:
                if KEEP_FILE in f.read(1024):  # the NVS migration: it names the old namespaces
                    continue
        except OSError:
            pass
        files.append(path)
    return files


def read_text(path):
    with open(path, "rb") as f:
        data = f.read()
    if b"\0" in data:
        return None
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default=".")
    ap.add_argument("--skip", action="append", default=[], help="glob of paths to leave alone")
    ap.add_argument("--allow", action="append", default=[], help="a target name that may already exist")
    ap.add_argument("--pre", action="append", default=[], help="OLD=NEW whole-word rename first")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--apply", action="store_true")
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--filter", action="store_true")
    a = ap.parse_args()
    pre = pre_rules([p.split("=", 1) for p in a.pre])

    if a.filter:
        sys.stdout.write(rewrite_text(sys.stdin.read(), pre))
        return 0

    repo = os.path.abspath(a.repo)
    files = tracked(repo, a.skip)

    if a.check:
        left = 0
        for p in files:
            text = read_text(os.path.join(repo, p))
            if LEGACY.search(p):
                print("%s: legacy path" % p)
                left += 1
            if text is None:
                continue
            for n, line in enumerate(text.splitlines(), 1):
                if not KEEP_LINE.search(line) and LEGACY.search(line):
                    left += 1
                    if left <= 20:
                        print("%s:%d: %s" % (p, n, line.strip()[:120]))
        print("legacy names left: %d" % left)
        return 1 if left else 0

    changed, moves, before = {}, {}, set()
    for p in files:
        text = read_text(os.path.join(repo, p))
        if text is not None:
            new = rewrite_text(text, pre)
            before |= tokens(text)
            if new != text:
                changed[p] = new
        np = rewrite_path(p)
        if np != p:
            moves[p] = np
    # A collision: a new name that is already used, by a name that stays.
    renamed = {t for t in before if rewrite_line(t, pre) != t}
    kept = before - renamed
    collisions = sorted(t for t in {rewrite_line(r, pre) for r in renamed} & kept if t not in a.allow)
    print("files to rewrite: %d, paths to move: %d, names changed: %d"
          % (len(changed), len(moves), len(renamed)))
    for t in collisions:
        print("COLLISION: %s already exists" % t)
    if not a.apply:
        for o, n in sorted(moves.items()):
            print("  mv %s -> %s" % (o, n))
        return 1 if collisions else 0
    if collisions:
        print("refusing to apply: resolve the collisions or pass --allow NAME")
        return 1
    for p, new in changed.items():
        with open(os.path.join(repo, p), "w", encoding="utf-8", newline="") as f:
            f.write(new)
    for o, n in sorted(moves.items()):
        os.makedirs(os.path.dirname(os.path.join(repo, n)) or repo, exist_ok=True)
        subprocess.run(["git", "-C", repo, "mv", o, n], check=True)
    for d in sorted({os.path.dirname(o) for o in moves}, key=len, reverse=True):
        while d and os.path.isdir(os.path.join(repo, d)) and not os.listdir(os.path.join(repo, d)):
            os.rmdir(os.path.join(repo, d))  # the old directory, now empty
            d = os.path.dirname(d)
    print("applied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
