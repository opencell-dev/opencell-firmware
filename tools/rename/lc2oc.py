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
"lc2oc: keep-file" in their first 1 KiB, are left alone -- but the names they
hold still count as already-kept names, so nothing renamed elsewhere is
allowed to collide with them.

A path collision -- a rename target that already exists, that two renames
land on together, or a directory a rename would have to merge into -- is
computed for every move before any file is written, and refuses --apply
(dry run and --check just report it).

A tracked path that is a symlink, or that is missing on disk, is reported
and left alone rather than crashing the tool.

Usage:
    lc2oc.py [--repo DIR] [--skip GLOB]...            dry run: counts, collisions, moves
    lc2oc.py [--repo DIR] [--skip GLOB]... --apply    rewrite files, git mv paths
    lc2oc.py [--repo DIR] [--skip GLOB]... --check    exit 1 if a legacy name is left
    lc2oc.py [--repo DIR] [--skip GLOB]... --filter < IN > OUT
                                                       rewrite a text stream (patches):
                                                       a diff hunk for a --skip'd or
                                                       keep-file path passes through intact
  --pre OLD=NEW  first rename the word OLD (also inside OLD_x, x_OLD) to NEW, in
                 both cases, before the lc mapping (plan 8: ocb=ocr, oc_cell=occ).
  --allow NAME   a new name (an identifier or a path) that may already exist
                 (a collision you checked).
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
    # -D is itself a letter ("D"), so the general LC(?=_) rule above can't
    # match "-DLC_..." (its lookbehind requires a non-alnum character); this
    # rule is the only one that reaches a compiler define. Proven load-bearing
    # by Words.test_renamed's "-DLC_BENCH_LOW_POWER=1" case -- not dead.
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

# A git-style diff header: "diff --git a/PATH b/PATH". Only this line starts
# a new hunk's path context for --filter; --- /+++ lines are content lines
# that get rewritten (or not) like any other line within that context.
DIFF_GIT = re.compile(r"^diff --git a/(\S+) b/(\S+)")


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


def split_tokens(text, pre=()):
    """Rewrite text line by line, like rewrite_text, but also split its
    tokens: `before` are tokens on lines eligible for renaming, `kept` are
    tokens on a KEEP_LINE line (never renamed, but still real names that
    must not be collided into)."""
    out, before, kept = [], set(), set()
    for line in text.splitlines(keepends=True):
        if KEEP_LINE.search(line):
            kept |= tokens(line)
            out.append(line)
        else:
            before |= tokens(line)
            out.append(rewrite_line(line, pre))
    return "".join(out), before, kept


def is_keep_file(full):
    """Is `full` (an absolute path) a whole file left alone: its first 1 KiB
    names the keep-file marker? Never follows a symlink; missing/unreadable
    counts as no."""
    if os.path.islink(full):
        return False
    try:
        with open(full, "rb") as f:
            return KEEP_FILE in f.read(1024)
    except OSError:
        return False


def tracked(repo, skip):
    """Every git-tracked path not under ALWAYS_SKIP/--skip globs, as
    (path, keep_file) pairs. keep_file paths are still returned (their
    tokens count as already-kept names) but are never rewritten or moved."""
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
        files.append((path, is_keep_file(os.path.join(repo, path))))
    return files


def read_text(path):
    """The file's text, or None if it's a symlink (never followed), missing,
    binary, or not valid UTF-8. A symlink or a missing tracked file is
    reported rather than crashing the tool."""
    if os.path.islink(path):
        print("%s: a symlink, left alone" % path)
        return None
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        print("%s: tracked but missing, left alone" % path)
        return None
    if b"\0" in data:
        return None
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return None


def path_collisions(repo, moves, allow):
    """A rename target already exists, two renames land on the same target,
    or a rename's target directory already exists (a merge, or a file
    landing on an existing directory) -- computed against the repository as
    it is now, before anything is written or moved."""
    targets = {}
    for o, n in moves.items():
        targets.setdefault(n, []).append(o)

    hits = set()
    for n, olds in targets.items():
        if len(olds) > 1 and n not in allow:
            hits.add(n)

    for o, n in moves.items():
        if n in allow or n in hits:
            continue
        if os.path.lexists(os.path.join(repo, n)) and n not in moves:
            hits.add(n)
            continue
        od, nd = os.path.dirname(o), os.path.dirname(n)
        if od != nd and nd and nd not in allow and os.path.isdir(os.path.join(repo, nd)):
            hits.add(nd)
    return sorted(hits)


def skip_hunk(repo, a_path, b_path, skip):
    for path in (a_path, b_path):
        if any(fnmatch.fnmatch(path, g) for g in ALWAYS_SKIP + list(skip)):
            return True
        if is_keep_file(os.path.join(repo, path)):
            return True
    return False


def rewrite_patch(text, pre, repo, skip):
    """Rewrite a text stream (typically a unified diff/patch): a hunk whose
    diff --git header names a --skip'd or keep-file path passes through
    unchanged, path and content alike; every other line is rewritten as
    usual."""
    out, keep = [], False
    for line in text.splitlines(keepends=True):
        m = DIFF_GIT.match(line)
        if m:
            keep = skip_hunk(repo, m.group(1), m.group(2), skip)
        out.append(line if keep else rewrite_line(line, pre))
    return "".join(out)


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
    repo = os.path.abspath(a.repo)

    if a.filter:
        raw = sys.stdin.buffer.read()
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            sys.stdout.buffer.write(raw)  # binary: left alone, not even decoded
            return 0
        sys.stdout.buffer.write(rewrite_patch(text, pre, repo, a.skip).encode("utf-8"))
        return 0

    files = tracked(repo, a.skip)

    if a.check:
        left = 0
        printed = 0
        for p, kf in files:
            if kf:
                continue
            text = read_text(os.path.join(repo, p))
            if LEGACY.search(p):
                left += 1
                if printed < 20:
                    print("%s: legacy path" % p)
                    printed += 1
            if text is None:
                continue
            for n, line in enumerate(text.splitlines(), 1):
                if not KEEP_LINE.search(line) and LEGACY.search(line):
                    left += 1
                    if printed < 20:
                        print("%s:%d: %s" % (p, n, line.strip()[:120]))
                        printed += 1
        print("legacy names left: %d" % left)
        return 1 if left else 0

    changed, moves, before, kept = {}, {}, set(), set()
    for p, kf in files:
        full = os.path.join(repo, p)
        if kf:
            text = read_text(full)
            if text is not None:
                kept |= tokens(text)  # a kept name: nothing else may rename onto it
            continue
        text = read_text(full)
        if text is not None:
            new, b, k = split_tokens(text, pre)
            before |= b
            kept |= k
            if new != text:
                changed[p] = new
        np = rewrite_path(p)
        if np != p:
            moves[p] = np
    # A name collision: a new name that is already used, by a name that stays.
    renamed = {t for t in before if rewrite_line(t, pre) != t}
    kept = (before - renamed) | kept
    collisions = sorted(t for t in {rewrite_line(r, pre) for r in renamed} & kept if t not in a.allow)
    collisions = sorted(set(collisions) | set(path_collisions(repo, moves, a.allow)))
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
