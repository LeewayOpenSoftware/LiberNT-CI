#!/usr/bin/env python3
# PROJECT:     LiberNT logon transitions
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     Black-frame recorder for the Phase 3 legs (PHASE3-PLAN commit 11, INVESTIGACION 6.4):
#              QEMU HMP screendump loop at ~60 ms, EMPTY / DARK / CONTENT classes, full-resolution
#              8x8 block diff for frozen frames and hung spinners, mode changes, serial marks.
# COPYRIGHT:   Copyright 2026 Leeway Open Software
#
# Runs next to the VM (same host or container): needs the HMP monitor socket and the serial socket
# of the guest. Writes <out>/timeline.tsv (one event per line: seconds, kind, text) and <out>/frames/
# (quarter-size PPM thumbnails on every visible change). Commands are appended to <out>/cmd:
#   "#C3 lock Win+L T:lock"   a MARK (first token = case id; "T:<transition>" selects the budget)
#   "sendkey ctrl-alt-delete" any HMP command
#   "sleep 5"                 pause the command reader, recording continues
# A file <out>/stop ends the run. The analysis is classify3.py.
import argparse, os, socket, sys, threading, time

try:
    import numpy as np
except ImportError:                 # the classes still work by sampling; the 8x8 diff needs numpy
    np = None

T0 = time.time()
def now():
    return time.time() - T0

class Timeline:
    def __init__(self, path, append=False):
        # T0 restarts with every run: two runs in one file would mix time bases.
        if not append and os.path.exists(path) and os.path.getsize(path) > 0:
            sys.exit("rec3: %s is not empty; use a fresh --out or --append" % path)
        self.f = open(path, "a", buffering=1, encoding="utf-8")
        self.lock = threading.Lock()
    def ev(self, kind, text):
        with self.lock:
            self.f.write("%.3f\t%s\t%s\n" % (now(), kind, text))

class Serial(threading.Thread):
    """Copies the guest's COM1 into the timeline; LOGONSTAGE lines also drive the spinner window."""
    def __init__(self, path, tl, state):
        super().__init__(daemon=True)
        self.path, self.tl, self.state = path, tl, state
    def run(self):
        while True:
            try:
                s = socket.socket(socket.AF_UNIX)
                s.connect(self.path)
                buf = b""
                while True:
                    d = s.recv(4096)
                    if not d:
                        break
                    buf += d
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        text = line.decode("ascii", "replace").strip()
                        if text:
                            self.tl.ev("SER", text)
                            self.state.on_serial(text)
            except Exception:
                time.sleep(0.5)

class Mon:
    """HMP monitor over its unix socket (as the 0b+ recorder did)."""
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(path)
        self.s.settimeout(0.3)
        self.drain()
    def drain(self):
        try:
            while True:
                if not self.s.recv(65536):
                    break
        except socket.timeout:
            pass
    def cmd(self, c):
        self.s.sendall((c + "\n").encode())
        end = time.time() + 3
        buf = b""
        while time.time() < end:
            try:
                buf += self.s.recv(65536)
                if b"(qemu)" in buf.split(c.encode(), 1)[-1]:
                    break
            except socket.timeout:
                pass
        return buf

# Spinner screens, where a frozen picture for more than 2 s is a hung spinner (6.4.5). The window
# is derived from winlogon's LOGONSTAGE lines on COM1 (LiberNTStageTrace=1) or set by hand with
# the marks "#spinner on" / "#spinner off".
SPINNER_ON = ("authenticated", "logoff-start", "shutdown-start")
SPINNER_OFF = ("logged-on", "logon-failed", "logged-off", "shutdown-final")

class State:
    def __init__(self, tl):
        self.tl = tl
        self.spinner = False
        self.spinner_since = 0.0        # when the current spinner window opened
        self.lock = threading.Lock()
    def on_serial(self, text):
        parts = text.split()
        if len(parts) >= 4 and parts[0] == "LOGONSTAGE" and parts[2] == "winlogon":
            stage = parts[3]
            if stage in SPINNER_ON:
                self.set_spinner(True, stage)
            elif stage in SPINNER_OFF:
                self.set_spinner(False, stage)
            if stage == "shutdown-final":
                self.tl.ev("HARNESS", "shutdown-final")     # EMPTY after this mark is the kernel's
    def set_spinner(self, on, why):
        with self.lock:
            if self.spinner != on:
                self.spinner = on
                if on:
                    self.spinner_since = now()
                self.tl.ev("SPINNER", "%s %s" % ("on" if on else "off", why))

def parse_ppm(data):
    # P6\n<w> <h>\n255\n<rgb...>
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]

def analyze_numpy(w, h, px):
    a = np.frombuffer(px, dtype=np.uint8, count=w * h * 3).reshape(h, w, 3).astype(np.uint16)
    luma = (a[:, :, 0] * 299 + a[:, :, 1] * 587 + a[:, :, 2] * 114) // 1000
    mean = float(luma.mean())
    nonblack = float((luma > 16).mean())
    bh, bw = h // 8, w // 8
    blocks = luma[:bh * 8, :bw * 8].reshape(bh, 8, bw, 8).mean(axis=(1, 3))
    return mean, nonblack, blocks

def analyze_sampled(w, h, px):
    # No numpy: every 23rd pixel for the classes (0b+ method) and a 16x12 grid signature.
    n = w * h
    tot = cnt = nonblack = 0
    for i in range(0, n, 23):
        o = i * 3
        l = (px[o] * 299 + px[o + 1] * 587 + px[o + 2] * 114) // 1000
        tot += l; cnt += 1
        if l > 16:
            nonblack += 1
    sig = []
    for gy in range(12):
        for gx in range(16):
            x = gx * w // 16 + w // 32; y = gy * h // 12 + h // 24
            o = (y * w + x) * 3
            sig.append((px[o] + px[o + 1] + px[o + 2]) // 48)
    return tot / max(cnt, 1), nonblack / max(cnt, 1), sig

def classify(nonblack):
    if nonblack < 0.0005:
        return "EMPTY"
    if nonblack < 0.005:
        return "DARK"
    return "CONTENT"

def thumb(w, h, px, path):
    tw, th = w // 4, h // 4
    out = bytearray()
    for y in range(th):
        row = (y * 4) * w
        for x in range(tw):
            o = (row + x * 4) * 3
            out += px[o:o + 3]
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (tw, th) + bytes(out))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="/out")
    ap.add_argument("--monitor", default="/vm/monitor.sock")
    ap.add_argument("--serial", default="/vm/serial.sock")
    ap.add_argument("--period", type=float, default=0.06, help="target frame period in seconds")
    ap.add_argument("--frozen", type=float, default=15.0, help="seconds without a changed 8x8 block = frozen")
    ap.add_argument("--spinner-frozen", type=float, default=2.0, help="same, while a spinner is expected")
    ap.add_argument("--block-delta", type=float, default=4.0, help="mean-luma move that counts as a changed block")
    ap.add_argument("--no-diff", action="store_true", help="classes only, no 8x8 diff (no numpy)")
    ap.add_argument("--dump", default="/tmp/f.ppm", help="screendump path, as seen by QEMU and by this script")
    ap.add_argument("--append", action="store_true", help="append to an existing timeline (time base restarts)")
    args = ap.parse_args()

    os.makedirs(args.out + "/frames", exist_ok=True)
    tl = Timeline(args.out + "/timeline.tsv", args.append)
    state = State(tl)
    Serial(args.serial, tl, state).start()
    while not os.path.exists(args.monitor):
        time.sleep(0.2)
    mon = Mon(args.monitor)
    use_diff = (np is not None) and not args.no_diff
    tl.ev("REC", "start diff=%s period=%.3f frozen=%.1f spinner=%.1f delta=%.1f" %
          ("8x8" if use_diff else "none", args.period, args.frozen, args.spinner_frozen, args.block_delta))
    if not use_diff:
        tl.ev("WARN", "numpy missing or --no-diff: frozen-frame detection is off")

    cmdf = args.out + "/cmd"
    open(cmdf, "a").close()
    cmdpos = 0
    sleep_until = 0.0
    last_blocks = None; last_sig = None; last_cls = None; last_dim = None
    still_since = None; frozen_reported = False
    dump = args.dump

    while not os.path.exists(args.out + "/stop"):
        t_frame = time.time()
        if time.time() >= sleep_until:
            with open(cmdf, encoding="utf-8", errors="replace") as f:
                lines = f.read().split("\n")
            while cmdpos < len(lines) - 1 and time.time() >= sleep_until:
                c = lines[cmdpos].strip(); cmdpos += 1
                if not c:
                    continue
                if c.startswith("#"):
                    text = c[1:].strip()
                    tl.ev("MARK", text)
                    if text.startswith("spinner "):
                        state.set_spinner(text.split()[1] == "on", "mark")
                elif c.startswith("sleep "):
                    sleep_until = time.time() + float(c.split()[1]); tl.ev("CMD", c)
                else:
                    tl.ev("CMD", c); mon.cmd(c)

        t = now()
        try:
            mon.cmd("screendump " + dump)
            with open(dump, "rb") as f:
                w, h, px = parse_ppm(f.read())
            if use_diff:
                mean, nb, blocks = analyze_numpy(w, h, px)
                sig = None
            else:
                mean, nb, sig = analyze_sampled(w, h, px)
                blocks = None
        except Exception as e:
            tl.ev("ERR", repr(e)); time.sleep(0.5)
            try:
                mon = Mon(args.monitor)
            except Exception:
                time.sleep(1)
            continue

        cls = classify(nb)
        if (w, h) != last_dim:
            if last_dim is not None:
                tl.ev("MODE", "%dx%d -> %dx%d" % (last_dim[0], last_dim[1], w, h))
            last_dim = (w, h)
            last_blocks = None; last_sig = None

        # change detection: 8x8 blocks (full resolution) or the sampled signature
        if blocks is not None:
            if last_blocks is None or last_blocks.shape != blocks.shape:
                changed = blocks.size
            else:
                changed = int((np.abs(blocks - last_blocks) > args.block_delta).sum())
            last_blocks = blocks
            visibly_changed = changed > 0
        else:
            changed = -1
            visibly_changed = last_sig is None or sum(abs(a - b) for a, b in zip(sig, last_sig)) > 40
            last_sig = sig

        if visibly_changed or cls != last_cls:
            name = "k%08.2f.ppm" % t
            thumb(w, h, px, args.out + "/frames/" + name)
            tl.ev("FRAME", "%s %dx%d luma=%.2f nonblack=%.5f %s blocks=%d" % (name, w, h, mean, nb, cls, changed))
            last_cls = cls
        else:
            tl.ev("F", "%dx%d luma=%.2f nb=%.5f %s blocks=%d" % (w, h, mean, nb, cls, changed))

        # frozen frames (6.4.5): the screen bound counts the whole still run; the spinner bound
        # counts only the part of it that lies inside the spinner window (a page that was already
        # still when the spinner started is not a hung spinner until 2 s of spinner have passed).
        if changed == 0:
            if still_since is None:
                still_since = t
            screen_frozen = t - still_since > args.frozen
            spinner_frozen = state.spinner and (t - max(still_since, state.spinner_since)) > args.spinner_frozen
            if not frozen_reported and (screen_frozen or spinner_frozen):
                tl.ev("FROZEN", "start %.3f %s" % (max(still_since, state.spinner_since) if spinner_frozen and not screen_frozen else still_since,
                                                   "spinner" if spinner_frozen else "screen"))
                frozen_reported = True
        else:
            if frozen_reported:
                tl.ev("FROZEN", "end %.3f after %.2fs" % (t, t - still_since))
            still_since = None; frozen_reported = False

        rest = args.period - (time.time() - t_frame)
        if rest > 0:
            time.sleep(rest)
    tl.ev("REC", "stop")

if __name__ == "__main__":
    main()
