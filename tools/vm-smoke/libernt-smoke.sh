#!/bin/sh
# libernt-smoke.sh - automated visual smoke test for a LiberNT CI image.
#
#   sh tools/vm-smoke/libernt-smoke.sh <image> [name]
#
# <image> is one of:
#   - path to a raw LiberNT.img
#   - path to the CI artifact zip (LiberNT-img-*.zip)
#   - a GitHub Actions run id (the clang-amd64 LiberNT-img artifact is downloaded with gh; gh unpacks it,
#     so only the .img sha256 is recorded in the output folder)
#
# It boots the image in QEMU (UEFI/OVMF + WHPX, per-VM qcow2 overlay, the image is never written),
# waits for the desktop, then opens Explorer (This PC), cmd, Notepad, Control Panel and Settings
# one at a time. For each window it saves a full screenshot and a 6x zoom of the caption buttons
# (minimize / maximize / close) and prints PASS or FAIL. The exit code is the number of FAILs
# (0 = all windows passed; 2 = could not run at all).
#
# Output: $OUT_ROOT/<sha256 of the image>/ : NN-<window>.png, NN-<window>-caption6x.png,
#   00-desktop.png, result.txt, com1.log (kernel debug log). Re-running an image replaces the folder.
#
# Env: OUT_ROOT   output root              (default E:/LiberNT/results/smoke)
#      IMG_DIR    where images are fetched (default C:/LiberNT/images)
#      REPO       owner/name for run ids   (default LeewayOpenSoftware/LiberNT-LDW)
#      GH         gh binary                (default C:/LiberNT/tools/gh/bin/gh.exe)
#      RAM        guest RAM in MB          (default 3072)
#      ACCEL      whpx | tcg               (default whpx)
#      BOOT_TIMEOUT  seconds to wait for the desktop (default 240)
#      KEEP_VM=1  leave the VM running when done
#      VMDIR      directory holding run-vm.sh and mon.py (default: /e/LiberNT/vm)
#
# The guest is driven only through the QEMU monitor (sendkey / screendump), never the host input.
# The VM is registered in E:/LiberNT/vm/RUNNING.txt while it runs and always removed on exit.

set -u
IMGARG=${1:?usage: libernt-smoke.sh <image.img|artifact.zip|run-id> [name]}
OUT_ROOT=${OUT_ROOT:-E:/LiberNT/results/smoke}
IMG_DIR=${IMG_DIR:-C:/LiberNT/images}
REPO=${REPO:-LeewayOpenSoftware/LiberNT-LDW}
GH=${GH:-C:/LiberNT/tools/gh/bin/gh.exe}
RAM=${RAM:-3072}
BOOT_TIMEOUT=${BOOT_TIMEOUT:-240}
VMDIR=${VMDIR:-/e/LiberNT/vm}
RUNNING=${RUNNING:-$VMDIR/RUNNING.txt}
Q=/c/msys64/ucrt64/bin
PY=/c/msys64/usr/bin/python3
MAGICK=$Q/magick
export ACCEL=${ACCEL:-whpx}
FAILS=0
MODE=${MODE:-launcher}      # launcher: launcher.cmd payload drives the apps (no typing; skips OOBE)
                            # keys: Win+R typing; also reports OOBE window and dead keyboard
FIRST_DELAY=${FIRST_DELAY:-150}; GAP=${GAP:-240}   # seconds before the first app / after an app with state checks (launcher mode)
SHORT_GAP=${SHORT_GAP:-120}                        # after apps that are only opened and closed

die() { echo "SMOKE ERROR: $*" >&2; exit 2; }
for t in "$Q/qemu-system-x86_64.exe" "$MAGICK" "$PY" "$VMDIR/run-vm.sh" "$VMDIR/mon.py"; do
  [ -e "$t" ] || die "missing $t"
done

# ---- 1. resolve the image ---------------------------------------------------------------------
case "$IMGARG" in
  *[!0-9]*) SRC=$IMGARG ;;
  *) # a run id
     RUN=$IMGARG; D=$IMG_DIR/$RUN
     if ! ls "$D"/*.img >/dev/null 2>&1; then
       mkdir -p "$D" || die "cannot create $D"
       NAME=$("$GH" api "repos/$REPO/actions/runs/$RUN/artifacts" --paginate \
              -q '.artifacts[]|select(.name|startswith("LiberNT-img-clang-amd64-"))|.name' | head -1)
       [ -n "$NAME" ] || die "run $RUN has no LiberNT-img-clang-amd64 artifact"
       "$GH" run download "$RUN" -R "$REPO" -n "$NAME" -D "$D" || die "download of $NAME failed"
     fi
     SRC=$D ;;
esac
if [ -d "$SRC" ]; then
  for z in "$SRC"/*.zip; do [ -e "$z" ] && (cd "$SRC" && unzip -oq "$z" && rm -f "$z"); done
  SRC=$(ls "$SRC"/*.img 2>/dev/null | head -1)
elif [ "${SRC%.zip}" != "$SRC" ]; then
  D=$(dirname "$SRC"); (cd "$D" && unzip -oq "$(basename "$SRC")") || die "unzip failed"
  SRC=$(ls "$D"/*.img | head -1)
fi
[ -f "$SRC" ] || die "no image found for '$IMGARG'"
IMG=$SRC; export IMG
SHA=$(sha256sum "$IMG" | cut -d' ' -f1)
OUT=$OUT_ROOT/$SHA
rm -rf "$OUT"; mkdir -p "$OUT" || die "cannot create $OUT"
VM=${2:-smoke-$(echo "$SHA" | cut -c1-8)}
VDIR=/c/LiberNT/vm/overlays/$VM
RES=$OUT/result.txt
: > "$RES"
log() { echo "$*" | tee -a "$RES"; }
log "image   $IMG"
log "sha256  $SHA"
log "vm      $VM  ram=${RAM}MB accel=$ACCEL"
log "start   $(date -u +%Y-%m-%dT%H:%M:%SZ)"

# ---- 2. VM lifecycle --------------------------------------------------------------------------
# the monitor takes one connection at a time and resets a rapid reconnect: retry a failed call
mon() { for _t in 1 2 3 4; do o=$("$PY" "$VMDIR/mon.py" "$VM" "$@" 2>/dev/null) && { echo "$o"; return 0; }; sleep 0.5; done; return 1; }
cleanup() {
  if [ "${KEEP_VM:-0}" != 1 ]; then
    mon quit >/dev/null 2>&1
    sleep 2
    [ -f "$VDIR/qemu.pid" ] && taskkill //F //PID "$(cat "$VDIR/qemu.pid" 2>/dev/null)" >/dev/null 2>&1
    [ -f "$RUNNING" ] && { grep -v "^$VM |" "$RUNNING" > "$RUNNING.$$" 2>/dev/null; cat "$RUNNING.$$" > "$RUNNING"; rm -f "$RUNNING.$$"; }
  fi
  [ -f "$VDIR/com1.log" ] && cp "$VDIR/com1.log" "$OUT/com1.log" 2>/dev/null
}
trap cleanup EXIT INT TERM

# the cap is 3 VMs at once: refuse to be the fourth
N=$(grep -c . "$RUNNING" 2>/dev/null); N=${N:-0}
[ "$N" -lt 3 ] || die "VM cap reached ($N running, see $RUNNING)"
rm -rf "$VDIR"                       # always a fresh guest
if [ "$MODE" = launcher ]; then
  # A launcher.cmd at the root of a non-system disk skips OOBE and runs an automated session. It opens
  # one app per GAP seconds; the host notices each new window by diffing the screen, then closes it.
  mkdir -p "$VDIR/payload"
  # launcher.cmd itself exits at once (its console would otherwise stay on screen and pollute the
  # window diffs). One minimised helper per app sleeps until its slot and then starts the app.
  { printf '@echo off\r\n'
    t=$FIRST_DELAY
    for a in 'explorer shell:MyComputerFolder' 'cmd /k' 'notepad' 'control' 'ms-settings:'; do
      printf 'start "" /min cmd /c ping -n %s 127.0.0.1 ^>nul ^& start "" %s\r\n' "$t" "$a"; case "$a" in control|ms-settings:) t=$((t+SHORT_GAP));; *) t=$((t+GAP));; esac; done; } > "$VDIR/payload/launcher.cmd"
  export PAYLOAD=$(cygpath -m "$VDIR/payload")
fi
# applaunch (the service that runs launcher.cmd at logon) only acts when SMBIOS carries this OEM string
EXTRA=; [ "$MODE" = launcher ] && EXTRA="-smbios type=11,value=libernt.applaunch"
sh "$VMDIR/run-vm.sh" "$VM" "$RAM" $EXTRA > "$OUT/qemu.out" 2>&1 &
echo "$VM | smoke (libernt-smoke.sh, image sha ${SHA%${SHA#????????}}) | $(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$RUNNING"
i=0; until [ -f "$VDIR/monitor.port" ] && mon "info status" 2>/dev/null | grep -q running; do
  i=$((i+1)); [ $i -le 30 ] || die "VM did not start (see $OUT/qemu.out)"; sleep 1
done

# shot <file.png>  -> screendump converted to png; prints WxH via $SW/$SH
shot() {
  mon "screendump $1.ppm" >/dev/null 2>&1
  [ -s "$1.ppm" ] || return 1
  "$MAGICK" "$1.ppm" "$1" && rm -f "$1.ppm"
}
# nonblank <png>: 0 when the picture has real content (not a flat colour)
nonblank() { s=$("$MAGICK" "$1" -format '%[fx:standard_deviation>0.02?1:0]' info: 2>/dev/null); [ "$s" = 1 ]; }
# diffpx <a.png> <b.png> [geometry-crop]: count of pixels that differ (fuzz 8%)
diffpx() {
  if [ -n "${3:-}" ]; then
    "$MAGICK" compare -metric AE -fuzz 8% "$1" -crop "$3" +repage "$2" -crop "$3" +repage null: 2>&1 | grep -o '^[0-9.e+]*' | head -1 | sed 's/[.].*//'
  else
    "$MAGICK" compare -metric AE -fuzz 8% "$1" "$2" null: 2>&1 | grep -o '^[0-9.e+]*' | head -1 | sed 's/[.].*//'
  fi
}

# ---- 3. wait for the desktop ------------------------------------------------------------------
# The desktop is up when the picture is non-flat and unchanged between two shots 4 s apart.
T0=$(date +%s); prev=; ok=0
while [ $(( $(date +%s) - T0 )) -lt "$BOOT_TIMEOUT" ]; do
  sleep 4
  shot "$OUT/.cur.png" || continue
  if nonblank "$OUT/.cur.png" && [ -n "$prev" ] && [ "$(diffpx "$prev" "$OUT/.cur.png")" -lt 200 ]; then ok=$((ok+1)); else ok=0; fi
  [ $ok -ge 3 ] && break
  cp "$OUT/.cur.png" "$OUT/.prev.png"; prev=$OUT/.prev.png
done
if [ $ok -lt 3 ]; then
  shot "$OUT/00-desktop.png"
  log "BOOT    FAIL  no stable desktop within ${BOOT_TIMEOUT}s"
  log "result  1 FAIL"; exit 1
fi
sleep 10                              # let the shell finish its start-up work
cp "$OUT/.cur.png" "$OUT/00-desktop.png"; shot "$OUT/00-desktop.png"
BASE=$OUT/00-desktop.png
GEO=$("$MAGICK" identify -format '%w %h' "$BASE"); SW=${GEO% *}; SH=${GEO#* }
log "BOOT    PASS  desktop up in $(( $(date +%s) - T0 ))s, screen ${SW}x${SH}"

# typeslow <text>: one key per monitor call with a pause, so a busy guest does not drop keys
# (lowercase letters, digits, space, '-', '.', ':' only)
typeslow() {
  t=$1
  while [ -n "$t" ]; do
    ch=${t%"${t#?}"}; t=${t#?}
    case "$ch" in
      " ") k=spc ;; "-") k=minus ;; ".") k=dot ;; ":") k=shift-semicolon ;; *) k=$ch ;;
    esac
    mon "sendkey $k" >/dev/null; sleep 0.25
  done
}
# ---- 3b. first-boot checks: OOBE window and keyboard liveness ----------------------------------
if [ "$MODE" = keys ]; then
# A clean boot must show a bare desktop. A "LiberNT Setup" (OOBE) window leaves a button on the
# taskbar, left of the tray; anything drawn there at boot is reported.
TBY=$((SH-TB+6)); TBH=$((TB-12))
tbs=$("$MAGICK" "$BASE" -crop 700x${TBH}+70+${TBY} +repage -format '%[fx:standard_deviation>0.02?1:0]' info: 2>/dev/null)
if [ "$tbs" = 1 ]; then log "OOBE    FAIL  a window is open at first boot (taskbar button present; LiberNT Setup/OOBE?), see 00-desktop.png"; FAILS=$((FAILS+1))
else log "OOBE    PASS  bare desktop, no window open at first boot"; fi
# Keyboard: open Run (Win+R) and type two bursts a few seconds apart; the field must change both times.
RD=360x220+0+$((SH-TB-220))
mon "sendkey esc" >/dev/null; sleep 1; shot "$OUT/.k0.png"
mon "sendkey meta_l-r" >/dev/null; sleep 3; shot "$OUT/.kA.png"
typeslow "abcdef"; sleep 2; shot "$OUT/.kB.png"
sleep 5; typeslow "ghijkl"; sleep 2; shot "$OUT/.kC.png"
cp "$OUT/.kC.png" "$OUT/00-keyboard.png"
dA=$(diffpx "$OUT/.k0.png" "$OUT/.kA.png" $RD); dB=$(diffpx "$OUT/.kA.png" "$OUT/.kB.png" $RD); dC=$(diffpx "$OUT/.kB.png" "$OUT/.kC.png" $RD)
mon "sendkey esc" >/dev/null; sleep 1
if [ "${dA:-0}" -gt 500 ] && [ "${dB:-0}" -gt 30 ] && [ "${dC:-0}" -gt 30 ]; then KB=alive; log "KEYBOARD PASS  Win+R opened Run and both typing bursts reached it (px: $dA/$dB/$dC)"
else KB=dead; log "KEYBOARD FAIL  guest keyboard not working (Run opened px=$dA, burst1 px=$dB, burst2 px=$dC; need >500/>30/>30), see 00-keyboard.png"; FAILS=$((FAILS+1)); fi
rm -f "$OUT"/.k0.png "$OUT"/.kA.png "$OUT"/.kB.png "$OUT"/.kC.png
else
  KB=alive
  log "OOBE    SKIPPED  launcher.cmd payload bypasses first-boot setup (MODE=keys checks it)"
  log "KEYBOARD SKIPPED  no typing in launcher mode (MODE=keys checks it)"
fi

# ---- 3c. pointer probe -------------------------------------------------------------------------
# Move the pointer onto the "This PC" desktop icon and click it: the icon must get a selection highlight.
# (Known bug on current images: the pointer is pinned at the bottom-right corner and every click toggles Show Desktop.)
THISPC_X=${THISPC_X:-44}; THISPC_Y=${THISPC_Y:-105}
IR=70x70+10+$((THISPC_Y-30))
shot "$OUT/.pa.png"; sleep 3; shot "$OUT/.pb.png"; noise=$(diffpx "$OUT/.pa.png" "$OUT/.pb.png" $IR)   # idle noise of the region
mon "mouse_move $(( THISPC_X * 32767 / SW )) $(( THISPC_Y * 32767 / SH ))" >/dev/null; sleep 1
mon "mouse_button 1" >/dev/null; sleep 0.3; mon "mouse_button 0" >/dev/null; sleep 2
shot "$OUT/00-pointer-probe.png"
pd=$(diffpx "$OUT/.pb.png" "$OUT/00-pointer-probe.png" $IR); rm -f "$OUT/.pa.png" "$OUT/.pb.png"
if [ "${pd:-0}" -gt $(( ${noise:-0} + 300 )) ]; then log "POINTER PASS  clicking the This PC icon selected it (changed px $pd, idle noise ${noise:-0})"
else log "POINTER FAIL  the guest pointer did not land on the This PC icon (changed px ${pd:-0}, idle noise ${noise:-0}, need noise+300); caption-button click tests are not possible, window states are driven by keyboard instead, see 00-pointer-probe.png"; FAILS=$((FAILS+1)); fi
mon "sendkey esc" >/dev/null; sleep 1


# ---- 4. the windows ---------------------------------------------------------------------------
# Every window is located by diffing the screen before and after it opens (the taskbar is ignored),
# so it does not matter whether Win+Up maximised it. The caption strip is the top-right 160x32 px
# of that window's rectangle, zoomed 6x. The window is then closed with a real click on its close
# button (Alt+F4, then "exit" for cmd, are fallbacks); the close result is reported next to PASS/FAIL.
CW=160; CH=32; TB=48                  # caption strip size, taskbar height to ignore
runcmd() {                            # clear stray dialogs, Win+R, type the command, Enter
  mon "sendkey esc" >/dev/null; sleep 1; mon "sendkey esc" >/dev/null; sleep 1
  mon "sendkey meta_l-r" >/dev/null; sleep 3
  typeslow "$1"; sleep 1
  mon "sendkey ret" >/dev/null
}
# winbox <before.png> <after.png> -> "WxH+X+Y" of the biggest changed area above the taskbar (empty if none).
# Small unrelated changes (a window losing focus, the clock) form separate blobs and are ignored.
winbox() {
  "$MAGICK" "$1" "$2" -compose difference -composite -crop ${SW}x$((SH-TB))+0+0 +repage \
    -separate -evaluate-sequence max -threshold 10% -morphology Dilate Square:4 -define connected-components:verbose=true \
    -connected-components 8 null: 2>/dev/null | awk '/gray\(255\)/ && $4+0 > m { m=$4+0; b=$2 } END { if (m > 20000) print b }'
}
# click <x> <y>: absolute pointer click through the monitor (usb-tablet scale is 0..32767)
click() { mon "mouse_move $(( $1 * 32767 / SW )) $(( $2 * 32767 / SH ))" >/dev/null; sleep 1
          mon "mouse_button 1" >/dev/null; sleep 0.3; mon "mouse_button 0" >/dev/null; }
# ---- caption button checks (hover, minimise/restore, maximise/restore) -------------------------
# Buttons are laid out min | max | close, flush with the window's top-right corner. Their size depends on
# the window's caption style (measured on the Nov-1/main images; override with CAP_<LABEL>="close_w btn_w btn_h top_off"):
#   close_w = width of the close button, btn_w = width of min and max, btn_h = height, top_off = px below the window top.
capgeom() {
  case "$1" in
    explorer) g=${CAP_EXPLORER:-"43 26 18 3"} ;;
    cmd)      g=${CAP_CMD:-"43 26 18 1"} ;;
    notepad)  g=${CAP_NOTEPAD:-"43 26 18 1"} ;;
    *)        g="43 26 18 3" ;;
  esac
  set -- $g; CLW=$1; BTW=$2; BTH=$3; BTY=$4
  RI=${CAP_RIGHT_INSET:-20}   # the diff box includes the window shadow on the right
}
setbox() { box=$1; [ -n "$box" ] || return 1
  w=${box%%x*}; r=${box#*x}; h=${r%%+*}; r=${r#*+}; x=${r%%+*}; y=${r#*+}; }
curbox() { shot "$OUT/.m.png"; winbox "$pre" "$OUT/.m.png"; }
ismax() { [ "$w" -ge $((SW-30)) ] && [ "$h" -ge $((SH-TB-30)) ]; }
# geometry of button k (1=close 2=max 3=min): left edge, width, centre x; common centre y
btn_wd() { [ "$1" = 1 ] && echo $CLW || echo $BTW; }
btn_l()  { case $1 in 1) echo $((x+w-RI-CLW));; 2) echo $((x+w-RI-CLW-BTW));; 3) echo $((x+w-RI-CLW-2*BTW));; esac; }
btn_x()  { echo $(( $(btn_l $1) + $(btn_wd $1) / 2 )); }
btn_cy() { echo $(( y + BTY + BTH / 2 )); }
point() { mon "mouse_move $(( $1 * 32767 / SW )) $(( $2 * 32767 / SH ))" >/dev/null; sleep 1; }
cap_tests() {
  # KEYBOARD-driven window-state checks. Mouse clicks on the caption buttons cannot be tested on these
  # images: the guest pointer is pinned at the bottom-right corner, so every absolute click toggles
  # "Show Desktop" (ledger: input / mouse, Kelly). The pointer probe at boot reports that as its own FAIL line.
  lb=$1; box0=$box
  # start from the normal state
  if ismax; then mon "sendkey meta_l-down" >/dev/null; sleep 3; setbox "$(curbox)" || setbox "$box0"; fi
  mon "sendkey meta_l-up" >/dev/null; sleep 3; shot "$OUT/$n-$lb-maximized.png"
  if setbox "$(winbox "$pre" "$OUT/$n-$lb-maximized.png")" && ismax; then log "$lb.max      PASS  Win+Up maximised it (${w}x${h}), see $n-$lb-maximized.png"
  else log "$lb.max      FAIL  Win+Up did not maximise it (box ${box:-none}), see $n-$lb-maximized.png"; FAILS=$((FAILS+1)); fi
  mon "sendkey meta_l-down" >/dev/null; sleep 3; shot "$OUT/$n-$lb-restored.png"
  if setbox "$(winbox "$pre" "$OUT/$n-$lb-restored.png")" && ! ismax; then log "$lb.restore  PASS  Win+Down restored it to ${w}x${h}, see $n-$lb-restored.png"; box0=$box
  else log "$lb.restore  FAIL  Win+Down did not restore the normal size (box ${box:-none}), see $n-$lb-restored.png"; FAILS=$((FAILS+1)); setbox "$box0"; fi
  mon "sendkey meta_l-down" >/dev/null; sleep 3; shot "$OUT/$n-$lb-minimized.png"
  if setbox "$(winbox "$pre" "$OUT/$n-$lb-minimized.png")"; then
    log "$lb.min      FAIL  Win+Down again did not minimise it (still ${w}x${h}), see $n-$lb-minimized.png"; FAILS=$((FAILS+1)); setbox "$box0"
  else
    log "$lb.min      PASS  Win+Down again minimised it (window left the screen), see $n-$lb-minimized.png"
    mon "sendkey alt-tab" >/dev/null; sleep 3
    ow=${box0%%x*}; if setbox "$(curbox)" && [ "$(( w > ow ? w - ow : ow - w ))" -lt 80 ]; then log "$lb.unmin    PASS  Alt+Tab brought it back (${w}x${h})"; box0=$box
    else log "$lb.unmin    FAIL  Alt+Tab did not bring the minimised window back (box ${box:-none})"; FAILS=$((FAILS+1)); setbox "$box0"; fi
  fi
}
# dclick <x> <y>: pointer to the spot, then two quick clicks
dclick() { mon "mouse_move $(( $1 * 32767 / SW )) $(( $2 * 32767 / SH ))" >/dev/null; sleep 1
  mon "mouse_button 1" "mouse_button 0" "mouse_button 1" "mouse_button 0" >/dev/null; }
# guest_check: stop at once with a clear FAIL when the guest is no longer running
guest_check() { mon "info status" 2>/dev/null | grep -q running && return 0
  log "GUEST   FAIL  the VM is no longer running (powered off or crashed); remaining windows not tested"; log "result  FAIL"; exit 1; }
# window <n> <label> <run-command> <settle seconds>
window() {
  n=$1; lab=$2; cmd=$3; wait=$4; caps=${5:-}
  f=$OUT/$n-$lab.png; c=$OUT/$n-$lab-caption6x.png; pre=$OUT/.pre.png
  mon "sendkey esc" >/dev/null; sleep 1; shot "$pre"
  box=; try=0
  while [ -z "$box" ] && [ $try -lt 2 ]; do      # a dropped key gets one retry
    try=$((try+1))
    if [ "$MODE" = launcher ]; then          # wait for launcher.cmd to open the next app
      # rolling baseline: the shot just before the change is the "before" picture, so windows that
      # opened earlier (the launcher's own console) never count as the new window
      t=0; box=; cp "$pre" "$OUT/.roll.png"
      while [ -z "$box" ] && [ $t -lt $((GAP+90)) ]; do
        sleep 3; t=$((t+3)); shot "$OUT/.cur.png"; box=$(winbox "$OUT/.roll.png" "$OUT/.cur.png")
        [ -n "$box" ] || cp "$OUT/.cur.png" "$OUT/.roll.png"
      done
      [ -n "$box" ] && cp "$OUT/.roll.png" "$pre"
      sleep "$wait"
    elif [ "$KB" = dead ]; then
      if [ "$lab" = explorer ]; then mon "sendkey esc" >/dev/null; dclick ${THISPC_X:-44} ${THISPC_Y:-104}; sleep "$wait"   # keyboard dead: double-click the This PC icon
      else log "$lab    FAIL  not run: guest keyboard is dead (no way to launch it by mouse)"; FAILS=$((FAILS+1)); return; fi
    else runcmd "$cmd"; sleep "$wait"; fi
    shot "$f" || { guest_check; log "$lab    FAIL  screendump failed"; FAILS=$((FAILS+1)); return; }
    box=$(winbox "$pre" "$f")
    by=${box##*+}; [ -n "$box" ] && [ "$by" -gt $((SH/2)) ] && box=   # the Run dialog is still open: the typing was lost
  done
  if [ -z "$box" ]; then log "$lab    FAIL  no window appeared (screen did not change)"; FAILS=$((FAILS+1)); return; fi
  w=${box%%x*}; r=${box#*x}; h=${r%%+*}; r=${r#*+}; x=${r%%+*}; y=${r#*+}
  if [ "$w" -lt 200 ] || [ "$h" -lt 100 ]; then
    log "$lab    FAIL  changed area too small ($box)"; FAILS=$((FAILS+1)); return
  fi
  capgeom "$lab"
  # annotated picture: the three buttons the checks will use (red=min, green=max, blue=close)
  "$MAGICK" "$f" -fill none -stroke red -draw "rectangle $(btn_l 3),$((y+BTY)) $(( $(btn_l 3)+BTW )),$((y+BTY+BTH))" \
    -stroke lime -draw "rectangle $(btn_l 2),$((y+BTY)) $(( $(btn_l 2)+BTW )),$((y+BTY+BTH))" \
    -stroke blue -draw "rectangle $(btn_l 1),$((y+BTY)) $(( $(btn_l 1)+CLW )),$((y+BTY+BTH))" "$OUT/$n-$lab-buttons.png"
  cx=$(( x + w - RI - CW )); [ $cx -ge 0 ] || cx=0
  "$MAGICK" "$f" -crop ${CW}x${CH}+${cx}+${y} +repage -filter point -resize 600% "$c"
  cs=$("$MAGICK" "$c" -format '%[fx:standard_deviation>0.03?1:0]' info: 2>/dev/null)
  if [ "$cs" = 1 ]; then verdict=PASS; else verdict=FAIL; FAILS=$((FAILS+1)); fi
  [ -n "$caps" ] && [ "$verdict" = PASS ] && cap_tests "$lab"
  # close with Alt+F4 (keyboard: the pointer is unusable). Esc afterwards cancels the shutdown dialog
  # that Alt+F4 opens if no window had the focus. Success = the screen is back to how it was before.
  closed=no
  for how in 1 2; do
    mon "sendkey alt-f4" >/dev/null; sleep 3; mon "sendkey esc" >/dev/null; sleep 1; shot "$OUT/.cur.png"
    [ "$(diffpx "$pre" "$OUT/.cur.png" ${SW}x$((SH-TB))+0+0)" -lt 3000 ] && { closed=alt-f4; break; }
  done
  [ $closed = no ] && [ "$verdict" = PASS ] && { verdict=FAIL; FAILS=$((FAILS+1)); }
  log "$lab    $verdict  window ${w}x${h} at ${x},${y}; caption strip $([ "$cs" = 1 ] && echo drawn || echo blank); closed by: $closed"
}
window 01 explorer 'explorer shell:mycomputerfolder' 8 caps
window 02 cmd       'cmd'                             5 caps
window 03 notepad   'notepad'                         5 caps
window 04 control   'control'                         8
window 05 settings  'ms-settings:'                    12

# ---- 5. verdict -------------------------------------------------------------------------------
log "end     $(date -u +%Y-%m-%dT%H:%M:%SZ)"
rm -f "$OUT"/.cur.png "$OUT"/.prev.png "$OUT"/.pre.png "$OUT"/.m.png "$OUT"/.idle.png "$OUT"/.roll.png
if [ $FAILS -eq 0 ]; then log "result  PASS (5/5 windows + boot)"; else log "result  $FAILS FAIL (of 5 windows)"; fi
echo "output  $OUT"
exit $FAILS
