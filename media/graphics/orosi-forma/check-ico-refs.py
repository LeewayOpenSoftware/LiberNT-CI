#!/usr/bin/env python3
# Pre-build gate for Orosi Forma icon batches.
# Fails if any added/modified .ico in the range is not referenced by an
# .rc whose ICON reference path RESOLVES to that ico. Refs are resolved
# repo-relative against the rc file's directory AND every ancestor
# (module-root-relative and repo-root-relative refs both resolve), with
# ../ segments normalized. Basename-only matching misses the dead-path
# defect class (see forma Phase 1 sample wiring fix); directory-walking
# misses cross-module refs (e.g. explorer.rc -> netshell tray icons via
# ../../../). Usage: check-ico-refs.py <base>..<tip>  (run from the repo
# root with the tip checked out)
import os
import re
import subprocess
import sys

rng = sys.argv[1] if len(sys.argv) > 1 else "HEAD~1..HEAD"


def git(*args):
    return subprocess.run(["git"] + list(args), capture_output=True, text=True).stdout


changed = set(l for l in git("diff", "--name-only", "--diff-filter=AM", rng, "--", "*.ico").splitlines() if l)
if not changed:
    print("gate: no .ico files added/modified in %s - nothing to check" % rng)
    sys.exit(0)

ICO_REF = re.compile(r'"([^"]+\.ico)"', re.IGNORECASE)
rcs = [l for l in git("ls-files", "*.rc").splitlines() if l]

resolved_refs = set()
for rc in rcs:
    try:
        txt = open(rc, encoding="utf-8", errors="replace").read()
    except OSError:
        continue
    d = os.path.dirname(rc)
    roots = []
    while True:
        roots.append(d)
        if d == os.path.dirname(d):
            break
        d = os.path.dirname(d)
    for m in ICO_REF.finditer(txt):
        ref = m.group(1).replace("\\", "/").lstrip("/")
        for root in roots:
            p = os.path.normpath(os.path.join(root, ref))
            if not p.startswith(".."):
                resolved_refs.add(p)

bad = []
for p in sorted(changed):
    ok = p in resolved_refs
    print("gate: %s -> %s" % (p, "referenced" if ok else "NOT REFERENCED"))
    if not ok:
        bad.append(p)

if bad:
    print("gate: FAIL - %d unreferenced .ico (dead art would ship):" % len(bad))
    for p in bad:
        print("  " + p)
    sys.exit(1)
print("gate: PASS - all %d .ico referenced (repo-relative resolution)" % len(changed))
