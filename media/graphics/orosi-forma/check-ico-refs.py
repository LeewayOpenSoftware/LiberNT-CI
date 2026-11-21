#!/usr/bin/env python3
# Pre-build gate for Orosi Forma icon batches.
# Fails if any added/modified .ico in the range is not referenced by an
# .rc in its own module subtree (dead-art guard; see forma sample defect).
# Usage: check-ico-refs.py <base>..<tip>   (run from the repo root)
import subprocess, sys, os

rng = sys.argv[1] if len(sys.argv) > 1 else "HEAD~1..HEAD"

def git(*args):
    return subprocess.run(["git"] + list(args), capture_output=True, text=True).stdout

changed = [l for l in git("diff", "--name-only", "--diff-filter=AM", rng, "--", "*.ico").splitlines() if l]
if not changed:
    print("gate: no .ico files added/modified in %s - nothing to check" % rng)
    sys.exit(0)

bad = []
for path in changed:
    base = os.path.basename(path)
    d = os.path.dirname(path)
    referenced = False
    # walk ancestors; pass if any .rc directly in that dir references the basename
    while d and d != os.path.dirname(d):
        rcs = [f for f in os.listdir(d) if f.lower().endswith(".rc")]
        for rc in rcs:
            try:
                txt = open(os.path.join(d, rc), encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            if base in txt:
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
