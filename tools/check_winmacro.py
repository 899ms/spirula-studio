#!/usr/bin/env python3
"""Fail when a file this branch touched breaks under <windows.h>'s macros.

<windows.h> (reached through GlLoader.h on _WIN32) defines `near` and `far`
empty and `small` as `char`, so a local of that name fails only on Windows.
Each touched .cpp is re-run with -fsyntax-only and the three macros; only
errors located in touched files count, since upstream's own names are not
this branch's to rename. Needs a configured Ninja tree. A tarball, a shallow
clone without the base, or no touched source compiled here is a skip, not a fail.

Usage:  python3 tools/check_winmacro.py [--build DIR] [--base REV]
"""

import os
import re
import shlex
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

MACROS = ["-Dnear=", "-Dfar=", "-Dsmall=char"]
SOURCE_EXTS = (".cpp", ".cc")
HEADER_EXTS = (".h", ".hpp")
# The upstream commit feat/mask-editor is based on.
DEFAULT_BASE = "e6d38a2a"
ERROR = re.compile(r"^(?P<path>[^:\s][^:]*):(?P<line>\d+):(?:\d+:)? (?:fatal )?error: (?P<msg>.*)$")


def git(root, *args):
    try:
        p = subprocess.run(["git", "-C", root, *args], capture_output=True, text=True)
    except OSError:
        return None
    return p.stdout if p.returncode == 0 else None


def base_reachable(root, base):
    top = git(root, "rev-parse", "--show-toplevel")
    if top is None or os.path.realpath(top.strip()) != os.path.realpath(root):
        return False
    return git(root, "rev-parse", "--verify", "--quiet", base + "^{commit}") is not None


def touched_files(root, base):
    diff = git(root, "diff", "--name-only", "--diff-filter=d", base)
    if diff is None:
        return None
    others = git(root, "ls-files", "--others", "--exclude-standard") or ""
    return {p for p in (diff + others).splitlines()
            if p.endswith(SOURCE_EXTS + HEADER_EXTS)}


def compile_commands(root, build):
    p = subprocess.run(["ninja", "-C", build, "-t", "commands"], capture_output=True, text=True)
    if p.returncode != 0:
        return None
    found = {}
    for line in p.stdout.splitlines():
        m = re.search(r" -c (\S+)$", line)
        if not m or not m.group(1).endswith(SOURCE_EXTS):
            continue
        src = os.path.relpath(os.path.normpath(os.path.join(build, m.group(1))), root)
        found.setdefault(src, line)
    return found


def check(root, build, path, cmd):
    argv = shlex.split(cmd)
    out, skip = [], False
    for a in argv:   # drop the depfile and object outputs: syntax only
        if skip:
            skip = False
        elif a in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
        elif a not in ("-MD", "-MMD"):
            out.append(a)
    version = subprocess.run([out[0], "--version"], capture_output=True, text=True).stdout
    out += ["-fsyntax-only", "-ferror-limit=0" if "clang" in version else "-fmax-errors=0"] + MACROS
    p = subprocess.run(out, cwd=build, capture_output=True, text=True)
    errors = []
    for line in p.stderr.splitlines():
        m = ERROR.match(line)
        if m:
            where = os.path.relpath(os.path.normpath(os.path.join(build, m.group("path"))), root)
            errors.append((where, int(m.group("line")), m.group("msg")))
    return path, p.returncode, errors


def main():
    args = sys.argv[1:]
    opts = {"--build": "build", "--base": DEFAULT_BASE}
    while args:
        key = args.pop(0)
        if key not in opts or not args:
            print(f"check_winmacro: unknown or incomplete argument {key}")
            return 2
        opts[key] = args.pop(0)

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    build = os.path.join(root, opts["--build"])
    if not base_reachable(root, opts["--base"]):
        print(f"check_winmacro: skipped, {opts['--base']} is not reachable (no .git, or a shallow clone)")
        return 0
    touched = touched_files(root, opts["--base"])
    if touched is None:
        print(f"check_winmacro: cannot diff against {opts['--base']}")
        return 2
    commands = compile_commands(root, build)
    if commands is None:
        print(f"check_winmacro: no Ninja tree at {build}; configure it first")
        return 2

    sources = sorted(p for p in touched if p.endswith(SOURCE_EXTS))
    runnable = [p for p in sources if p in commands]
    for p in sources:
        if p not in commands:
            print(f"skip  {p} (not compiled in this tree)")
    if not runnable:
        print("check_winmacro: skipped, no touched source is compiled in this tree")
        return 0

    failed = 0
    with ThreadPoolExecutor(max_workers=min(8, os.cpu_count() or 1)) as pool:
        results = pool.map(lambda p: check(root, build, p, commands[p]), runnable)
        for path, rc, errors in results:
            ours = [e for e in errors if e[0] in touched]
            theirs = [e for e in errors if e[0] not in touched]
            if ours or (rc != 0 and not errors):
                failed += 1
                print(f"FAIL  {path}")
                for where, line, msg in ours:
                    print(f"      {where}:{line}: {msg}")
                if not errors:
                    print(f"      the compiler exited {rc} with no located error")
            else:
                note = f" ({len(theirs)} error(s) in untouched files ignored)" if theirs else ""
                print(f"ok    {path}{note}")
    print(f"check_winmacro: {len(runnable)} checked, {failed} failed, since {opts['--base']}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
