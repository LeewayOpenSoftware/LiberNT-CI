#!/bin/sh
# USN change journal: create, query, append, read, page-boundary padding and the
# $STANDARD_INFORMATION USN, checked against the independent ntfs-3g tools.
set -eu

driver=$1
mkntfs=$2
ntfscp=$3
ntfscat=$4
ntfsfix=$5
ntfsinfo=$6

workdir=$(mktemp -d "${TMPDIR:-/tmp}/ntfslib-usn.XXXXXX")
if test "${NTFSLIB_KEEP_TEST_DIR:-0}" = 1
then
    trap 'printf "preserved USN fixture: %s\n" "$workdir" >&2' EXIT HUP INT TERM
else
    trap 'rm -rf "$workdir"' EXIT HUP INT TERM
fi
image="$workdir/usn.ntfs"

fail()
{
    printf 'usn_smoke: %s\n' "$*" >&2
    exit 1
}

truncate -s 64M "$image"
"$mkntfs" -F -Q -q -L USN_TEST "$image" >/dev/null 2>&1

# No journal yet: STATUS_JOURNAL_NOT_ACTIVE.
if "$driver" --usn-query "$image" >"$workdir/err.txt" 2>&1
then
    fail "query succeeded before the journal existed"
fi
grep -q 'c00002b8' "$workdir/err.txt" || fail "expected STATUS_JOURNAL_NOT_ACTIVE: $(cat "$workdir/err.txt")"

"$driver" --usn-create "$image" 33554432 4194304
query=$("$driver" --usn-query "$image")
case "$query" in
    *" first=0 next=0 lowest=0 max=0x7fffffffffff0000 maxsize=33554432 delta=4194304") ;;
    *) fail "unexpected new journal: $query" ;;
esac
journal_id=${query%% *}

# Independent reader: the file has $J and $Max, and $Max holds the header.
"$ntfsinfo" -F '/$Extend/$UsnJrnl' "$image" >"$workdir/info.txt"
grep -q "Attribute name:.*'\$J'" "$workdir/info.txt" || fail "no \$J stream"
grep -q "Attribute name:.*'\$Max'" "$workdir/info.txt" || fail "no \$Max stream"
"$ntfscat" -a 0x80 -n '$Max' "$image" '/$Extend/$UsnJrnl' >"$workdir/max.bin"
test "$(wc -c <"$workdir/max.bin")" -eq 32 || fail "\$Max is not 32 bytes"
set -- $(od -An -tx8 -N16 "$workdir/max.bin")
test "$1 $2" = "0000000002000000 0000000000400000" || fail "\$Max sizes: $1 $2"

# 40 records of 264 bytes: 15 fit in a 4 KB page, so the 16th must start the next page.
i=0
prev=-1
while test $i -lt 40
do
    name=$(printf 'f%03d_%096d' "$i" 0)
    out=$("$driver" --usn-append "$image" $((0x10000 + i)) 5 0x100 "$name")
    usn=${out#usn=}
    test "$usn" -gt "$prev" || fail "USN went backwards: $usn after $prev"
    test $((usn % 4096 + 264)) -le 4096 || fail "record at $usn crosses a page"
    test $((usn % 8)) -eq 0 || fail "record at $usn is not 8-byte aligned"
    prev=$usn
    i=$((i + 1))
done
test "$prev" -eq $((8192 + 9 * 264)) || fail "last USN $prev, expected $((8192 + 9 * 264))"
next=$((prev + 264))

"$driver" --usn-read "$image" 0 >"$workdir/read.txt"
test "$(grep -c '^usn=' "$workdir/read.txt")" -eq 40 || fail "read returned $(grep -c '^usn=' "$workdir/read.txt") records"
grep -q "^usn=4096 len=264 reason=0x00000100 frn=0x000000000001000f parent=0x0000000000000005 name=f015_" "$workdir/read.txt" ||
    fail "record 15 was not moved to the second page"
grep -qx "next=$next" "$workdir/read.txt" || fail "read next: $(tail -n 1 "$workdir/read.txt")"

# Reading from the middle returns the remaining records only.
"$driver" --usn-read "$image" 4096 >"$workdir/read2.txt"
test "$(grep -c '^usn=' "$workdir/read2.txt")" -eq 25 || fail "partial read count"

# $J as ntfs-3g sees it ends at the next USN, and page 0 ends with zero fill.
"$ntfscat" -a 0x80 -n '$J' "$image" '/$Extend/$UsnJrnl' >"$workdir/j.bin"
test "$(wc -c <"$workdir/j.bin")" -eq "$next" || fail "\$J size $(wc -c <"$workdir/j.bin"), expected $next"
test "$(od -v -An -tx1 -j 3960 -N 136 "$workdir/j.bin" | tr -d ' \n' | tr -d 0)" = "" || fail "page 0 tail is not zero"

query=$("$driver" --usn-query "$image")
case "$query" in
    *" next=$next "*) ;;
    *) fail "query next: $query" ;;
esac

# Re-creating keeps the identifier and the records, and only changes the size targets.
"$driver" --usn-create "$image" 1048576 65536
query=$("$driver" --usn-query "$image")
test "${query%% *}" = "$journal_id" || fail "journal id changed: $query"
case "$query" in
    *" next=$next "*"maxsize=1048576 delta=65536") ;;
    *) fail "modify: $query" ;;
esac

# The USN stored in $STANDARD_INFORMATION, read back by ntfsinfo.
printf 'hello' >"$workdir/h.txt"
"$ntfscp" "$image" "$workdir/h.txt" /h.txt >/dev/null 2>&1
"$driver" --set-usn "$image" /h.txt 4242
"$ntfsinfo" -F /h.txt "$image" | grep -q 'Update Sequence Number:.*4242' || fail "SI USN not visible to ntfsinfo"

"$ntfsfix" -n "$image" >/dev/null || fail "ntfsfix found a problem"
printf 'usn_smoke: ok\n'
