#!/usr/bin/env python3
# PROJECT:     LiberNT logon transitions
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     Classifier for rec3.py timelines (INVESTIGACION 6.4 rev. 4 + the 0b+ budgets):
#              EMPTY/DARK runs, fades, mode switches, frozen frames and the harness mark per case
#              window, measured against the budget of the transition named in the case mark.
#              Phase 3 measures and reports; --gate makes a budget miss the exit code (Phase 4).
# COPYRIGHT:   Copyright 2026 Leeway Open Software
#
#   classify3.py <timeline.tsv> [--gate] [--frame-period 0.065]
# Case windows: from a MARK whose first token starts with "C" (as in 0b+) to the next such mark.
# A token "T:<name>" anywhere in the case's marks selects the budget (see BUDGETS).
import argparse, re, sys

# Transition budgets: Windows 11 measured in 0b+ plus 100 ms (RESULTADOS.md); EMPTY runs that
# the storyboard allows are listed per transition, everything else EMPTY is a miss.
BUDGETS = {
    #            empty_ms  fade_ms  dark_ok  note
    "welcome":   (0,       700,     False,   "Welcome -> desktop: crossfade <= 700 ms, no EMPTY"),
    "boot":      (None,    600,     True,    "boot -> logon: EMPTY only before the first logon frame, then fade <= 600 ms"),
    "creds":     (0,       None,    False,   "credentials -> Welcome: no EMPTY"),
    "lock":      (1000,    600,     False,   "lock: EMPTY <= 1000 ms, then fade <= 600 ms"),
    "unlock":    (0,       600,     False,   "unlock: crossfade <= 600 ms, no EMPTY"),
    "changepw":  (200,     450,     False,   "Ctrl+Alt+Del / change password: EMPTY <= 200 ms"),
    "signout":   (0,       None,    True,    "sign out: DARK ('Signing out') allowed, no EMPTY while the console session is reused"),
    "shutdown":  (200,     None,    True,    "shutdown/restart: EMPTY <= 200 ms before the text; after the harness mark EMPTY is the kernel's"),
    "cad":       (0,       None,    True,    "entering Ctrl+Alt+Del: no EMPTY, DARK allowed"),
    "switchuser":(2500,    650,     True,    "switch user (later phase): EMPTY <= 2500 ms"),
}
FROZEN_SCREEN_S = 15.0
FROZEN_SPINNER_S = 2.0

def load(path):
    rows = []
    for line in open(path, encoding="utf-8", errors="replace"):
        p = line.rstrip("\n").split("\t")
        if len(p) < 3:
            continue
        try:
            rows.append((float(p[0]), p[1], p[2]))
        except ValueError:
            pass
    return rows

FRAME_RE = re.compile(r"(\d+)x(\d+) luma=([\d.]+) n(?:onblack|b)=([\d.]+) (EMPTY|DARK|CONTENT)(?: blocks=(-?\d+))?")

def frames_of(rows):
    out = []
    for t, k, x in rows:
        if k in ("F", "FRAME"):
            m = FRAME_RE.search(x)
            if m:
                out.append((t, int(m.group(1)), int(m.group(2)), float(m.group(3)), float(m.group(4)),
                            m.group(5), int(m.group(6)) if m.group(6) is not None else -1))
    return out

def runs(frames, t0, t1, period):
    out = []; cur = None
    for f in frames:
        if not (t0 <= f[0] <= t1):
            continue
        c = f[5]
        if cur and cur[0] == c:
            cur[2] = f[0]; cur[3] += 1
        else:
            if cur:
                out.append(cur)
            cur = [c, f[0], f[0], 1]
    if cur:
        out.append(cur)
    return [(c, s, e, n, e - s + period) for c, s, e, n in out]

def fades(frames, t0, t1, minlen=3):
    seq = [f for f in frames if t0 <= f[0] <= t1]
    res = []; i = 0
    while i < len(seq) - 1:
        d = 1 if seq[i + 1][3] > seq[i][3] else -1
        j = i
        while j + 1 < len(seq) and (seq[j + 1][3] - seq[j][3]) * d > 0.3 and seq[j + 1][5] != "EMPTY":
            j += 1
        if j - i + 1 >= minlen and abs(seq[j][3] - seq[i][3]) >= 5:
            res.append((seq[i][0], seq[j][0], seq[i][3], seq[j][3], j - i + 1))
            i = j
        else:
            i += 1
    return res

def frozen_runs(frames, spinner_windows, t0, t1):
    """Runs with blocks==0 longer than the bound; the bound is 2 s inside a spinner window."""
    res = []; start = None
    seq = [f for f in frames if t0 <= f[0] <= t1 and f[6] >= 0]
    for f in seq:
        if f[6] == 0:
            if start is None:
                start = f[0]
            last = f[0]
        else:
            if start is not None:
                res.append((start, last))
            start = None
    if start is not None:
        res.append((start, last))
    out = []
    for s, e in res:
        spin = any(a <= s < b for a, b in spinner_windows)
        bound = FROZEN_SPINNER_S if spin else FROZEN_SCREEN_S
        if e - s > bound:
            out.append((s, e, spin))
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("timeline")
    ap.add_argument("--gate", action="store_true", help="exit 1 when a budget is missed (Phase 4)")
    ap.add_argument("--frame-period", type=float, default=0.065)
    args = ap.parse_args()

    rows = load(args.timeline)
    frames = frames_of(rows)
    marks = [(t, x) for t, k, x in rows if k == "MARK"]
    modes = [t for t, k, x in rows if k == "MODE"]
    harness = [t for t, k, x in rows if k == "HARNESS"]
    procs = [(t, x) for t, k, x in rows if k == "SER" and (x.startswith("CHG") or x.startswith("MON"))]
    # spinner windows from SPINNER on/off events
    spinner_windows = []; on = None
    for t, k, x in rows:
        if k == "SPINNER":
            if x.startswith("on") and on is None:
                on = t
            elif x.startswith("off") and on is not None:
                spinner_windows.append((on, t)); on = None
    if on is not None:
        spinner_windows.append((on, 1e9))

    cases = []
    for t, x in marks:
        k = x.split()[0]
        if not cases or cases[-1][0] != k:
            cases.append([k, t, x])
    bounds = [(c[0], c[1], (cases[i + 1][1] if i + 1 < len(cases) else 1e9), c[2]) for i, c in enumerate(cases)]

    misses = 0
    for name, a, b, title in bounds:
        tag = None
        for t, x in marks:
            if a <= t < b:
                for tok in x.split():
                    if tok.startswith("T:"):
                        tag = tok[2:]
        budget = BUDGETS.get(tag) if tag else None
        print("\n=== %s  [%.2f .. %.2f]  %s%s" % (name, a, b, title, ("  budget: " + budget[3]) if budget else ""))
        for t, x in marks:
            if a <= t < b:
                print("  %8.2f MARK %s" % (t, x))
        for t, x in procs:
            if a <= t < b:
                print("  %8.2f PROC %s" % (t, x))
        for t in harness:
            if a <= t < b:
                print("  %8.2f HARNESS shutdown-final (EMPTY after this is the kernel's)" % t)
        first_content = next((f[0] for f in frames if a <= f[0] < b and f[5] == "CONTENT"), None)
        window_runs = runs(frames, a, b, args.frame_period)
        for c, s, e, n, dur in window_runs:
            if c == "CONTENT":
                continue
            flags = []
            if any(abs(m - s) <= args.frame_period * 1.5 or abs(m - e) <= args.frame_period * 1.5 for m in modes):
                flags.append("mode-switch")
            if c == "EMPTY" and budget:
                allowed_ms = budget[0]
                after_mark = any(h <= s for h in harness if a <= h < b)
                before_first = first_content is not None and e < first_content and tag == "boot"
                if after_mark:
                    flags.append("after-harness-mark: allowed")
                elif before_first:
                    flags.append("before first logon frame: allowed")
                elif allowed_ms == 0:
                    flags.append("MISS: EMPTY not allowed"); misses += 1
                elif allowed_ms is not None and dur * 1000 > allowed_ms:
                    flags.append("MISS: EMPTY %.0f ms > %d ms" % (dur * 1000, allowed_ms)); misses += 1
                else:
                    flags.append("within budget")
            if c == "DARK" and budget and not budget[2]:
                flags.append("DARK where the storyboard expects none (check)")
            print("  %8.2f-%8.2f %-5s %3d frames  ~%.2fs  %s" % (s, e, c, n, dur, " ".join(flags)))
        for s, e, l0, l1, n in fades(frames, a, b):
            flag = ""
            if budget and budget[1] is not None and (e - s) * 1000 > budget[1]:
                flag = "  MISS: fade %.0f ms > %d ms" % ((e - s) * 1000, budget[1]); misses += 1
            print("  %8.2f-%8.2f FADE luma %.1f->%.1f %d frames %.2fs%s" % (s, e, l0, l1, n, e - s, flag))
        for s, e, spin in frozen_runs(frames, spinner_windows, a, b):
            print("  %8.2f-%8.2f FROZEN %.1fs %s  MISS" % (s, e, e - s, "(spinner hung)" if spin else "(screen)"))
            misses += 1
        res = sorted(set((f[1], f[2]) for f in frames if a <= f[0] < b))
        print("  resolutions:", res)

    ts = [f[0] for f in frames]
    dts = sorted(ts[i + 1] - ts[i] for i in range(len(ts) - 1) if ts[i + 1] - ts[i] < 1)
    if dts:
        print("\nframe period median %.3f p90 %.3f" % (dts[len(dts) // 2], dts[int(len(dts) * .9)]))
    print("budget misses: %d (%s)" % (misses, "gate" if args.gate else "measured only, Phase 3"))
    sys.exit(1 if (args.gate and misses) else 0)

if __name__ == "__main__":
    main()
