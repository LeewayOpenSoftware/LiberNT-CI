# logonframes: black-frame recorder and classifier for the logon transitions

Phase 3 of the LogonUI work (PHASE3-PLAN commit 11; criterion INVESTIGACION 6.4 rev. 4 with the
0b+ budgets). In Phase 3 the kit **measures and reports**; it becomes a gate in Phase 4
(`classify3.py --gate`).

## What it records (`rec3.py`)

- A QEMU HMP `screendump` about every 60 ms, through the monitor socket; the guest's COM1
  through the serial socket.
- Every frame is classified from its luminance: **EMPTY** (< 0.05 % of pixels above 16),
  **DARK** (< 0.5 %, text or spinner on black), **CONTENT** (the rest).
- With numpy, a **full-resolution 8x8 block diff** against the previous frame: a block changed
  when its mean luminance moved by more than 4. No block changing for more than **15 s** is a
  frozen screen; more than **2 s** while a spinner is expected is a hung spinner (both `FROZEN`
  events). The spinner window follows winlogon's `LOGONSTAGE` lines on COM1
  (`authenticated`..`logged-on`/`logon-failed`, `logoff-start`..`logged-off`,
  `shutdown-start`..`shutdown-final`; set `LiberNTStageTrace=1` under the Winlogon key) or the
  marks `#spinner on` / `#spinner off`.
- **Mode changes**: a change of the screendump's dimensions is a `MODE` event; an EMPTY run that
  coincides with one (±1 frame) is tagged `mode-switch` in the report.
- **Harness mark**: winlogon writes `LOGONSTAGE <tick> winlogon shutdown-final` right before its
  final `NtShutdownSystem`; the recorder turns it into a `HARNESS` event. EMPTY after that mark
  belongs to the kernel and is allowed; EMPTY between the shutdown text and the mark is not.
- Thumbnails (quarter size PPM) on every visible change under `frames/`.

Without numpy the classes still work (every 23rd pixel, the 0b+ method) and the diff is off
(`WARN` in the timeline). Install `python3-numpy` in the recorder container.

## Running

```
python3 rec3.py --out /out --monitor /vm/monitor.sock --serial /vm/serial.sock
echo "#C1 lock Win+L T:lock" >> /out/cmd      # a case mark: first token = case id, T:<transition> = budget
echo "sendkey meta_l-l"      >> /out/cmd      # any HMP command
echo "sleep 5"               >> /out/cmd
echo "#C1 end"               >> /out/cmd
touch /out/stop
python3 classify3.py /out/timeline.tsv > report.txt
```

Transitions (`T:`): `welcome`, `boot`, `creds`, `lock`, `unlock`, `changepw`, `signout`,
`shutdown`, `cad`, `switchuser`. The budgets are the table in `classify3.py` (`BUDGETS`): the
Windows 11 figure measured in 0b+ plus 100 ms, and the EMPTY rules of 6.4.

## Report (`classify3.py`)

Per case window: marks, process-monitor lines from COM1, the harness mark, EMPTY/DARK runs with
their verdict against the budget (`within budget`, `MISS ...`, `after-harness-mark: allowed`,
`before first logon frame: allowed`, `mode-switch`), fades (monotonic luminance over at least
3 frames with no EMPTY inside; checked against the fade budget), frozen runs, resolutions. The
last line counts the misses; the exit code is 0 in Phase 3 and 1 with `--gate` when there are
misses.

## Validation

The classifier reproduces the 0b+ analysis: run it on `0bplus/timeline.tsv` (hive
`logonui-1123/0bplus/`) and compare the EMPTY/DARK runs and fades per case with
`0bplus/analisis-por-caso.txt` (same run boundaries and frame counts; the 0b+ timeline has no
`T:` tags and no `blocks=` field, so no budget verdicts and no frozen detection appear). This
was done before measuring Orosi (see the commit message).

## Files

- `rec3.py`: recorder (runs next to the VM).
- `classify3.py`: offline classifier and report.
- Lab copy: `hive/shared/libernt/logonui-1123/phase3/kit/` (same files); the source of truth is
  this directory.
