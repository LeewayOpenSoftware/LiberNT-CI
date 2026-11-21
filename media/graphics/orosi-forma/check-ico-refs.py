#!/usr/bin/env python3
# Pre-build gate for Orosi Forma icon batches.
# Fails if any added/modified .ico in the range is not referenced by an
# .rc in its own module subtree, where the reference path RESOLVES
# (relative to the rc) to that ico. Basename-only matching misses the
# dead-path defect class (see forma Phase 1 sample wiring fix).
# Usage: check-ico-refs.py <base>..<tip>   (run from the repo root)
import os
import re
import subprocess
import sys

rng = sys.argv[1] if len(sys.argv) > 1 else "HEAD~1..HEAD"


def git(*args):
    return subprocess.run(["git"] + list(args), capture_output=True, text=True).stdout


changed = [l for l in git("diff", "--name-only", "--diff-filter=AM", rng, "--", "*.ico").splitlines() if l]
if not changed:
    print("gate: no .ico files added/modified in %s - nothing to check" % rng)
    sys.exit(0)

ICO_REF = re.compile(r'"([^"]+\.ico)"', re.IGNORECASE)


def rc_references(rc_path, ico_repo_path):
    try:
        txt = open(rc_path, encoding="utf-8", errors="replace").read()
    except OSError:
        return False
    rc_dir = os.path.dirname(rc_path)
    for m in ICO_REF.finditer(txt):
        ref = m.group(1).replace("\\", "/").lstrip("/")
        if os.path.normpath(os.path.join(rc_dir, ref)) == os.path.normpath(ico_repo_path):
            return True
    return False


bad = []
for path in changed:
    d = os.path.dirname(path)
    referenced = False
    while d and d != os.path.dirname(d):
        if os.path.isdir(d):
            for rc in [f for f in os.listdir(d) if f.lower().endswith(".rc")]:
                if rc_references(os.path.join(d, rc), path):
                    referenced = True
                    break
        if referenced:
            break
        d = os.path.dirname(d)
    print("gate: %s -> %s" % (path, "referenced" if referenced else "NOT REFERENCED"))
    if not referenced:
        bad.append(path)

if bad:
    print("gate: FAIL - %d unreferenced .ico (dead art would ship):" % len(bad))
    for p in bad:
        print("  " + p)
    sys.exit(1)
print("gate: PASS - all %d .ico referenced by an .rc in-module" % len(changed))
