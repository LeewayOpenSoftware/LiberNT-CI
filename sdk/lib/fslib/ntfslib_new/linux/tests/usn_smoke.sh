#!/bin/sh
# USN change journal: create, query, append, read, page-boundary padding, records of other
# versions written by Windows, a half-created journal, and the $STANDARD_INFORMATION USN,
# checked against the independent ntfs-3g tools and ntfslib's own checker.
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

new_image()
{
    rm -f "$1"
    truncate -s 64M "$1"
    "$mkntfs" -F -Q -q -L USN_TEST "$1" >/dev/null 2>&1
}

new_image "$image"

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
set -- $(od -v -An -tx8 -N16 "$workdir/max.bin")
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

# A USN inside a record is bad caller input (STATUS_INVALID_PARAMETER), not corruption.
if "$driver" --usn-read "$image" 8 >"$workdir/err3.txt" 2>&1
then
    fail "read from inside a record succeeded"
fi
grep -q 'c000000d' "$workdir/err3.txt" || fail "mid-record read: $(cat "$workdir/err3.txt")"

# $J as ntfs-3g sees it ends at the next USN, and page 0 ends with zero fill.
"$ntfscat" -a 0x80 -n '$J' "$image" '/$Extend/$UsnJrnl' >"$workdir/j.bin"
test "$(wc -c <"$workdir/j.bin")" -eq "$next" || fail "\$J size $(wc -c <"$workdir/j.bin"), expected $next"
test "$(od -v -An -tx1 -j 3960 -N 136 "$workdir/j.bin" | tr -d ' \n' | tr -d 0)" = "" || fail "page 0 tail is not zero"

# A V3 record (128-bit references, as Windows writes) must be walked past, not taken for a
# torn tail: write one at the next USN, append after it, and read both back.
perl -e 'print pack("VvvQ<Q<Q<Q<Q<Q<VVVVvva8x4", 0x58, 3, 0, 0x2000A, 0, 5, 0, $ARGV[0], 0, 0x100, 0, 0, 0x20, 8, 0x4C, "v\0003\0.\0x\0")' "$next" >"$workdir/v3.bin"
test "$(wc -c <"$workdir/v3.bin")" -eq 88 || fail "V3 fixture is not 88 bytes"
"$driver" --write "$image" '/$Extend/$UsnJrnl:$J' "$next" "$workdir/v3.bin"
query=$("$driver" --usn-query "$image")
case "$query" in
    *" next=$((next + 88)) "*) ;;
    *) fail "V3 record not counted: $query" ;;
esac
out=$("$driver" --usn-append "$image" 0x2000B 5 0x100 after_v3)
test "${out#usn=}" -eq $((next + 88)) || fail "append after V3 at ${out#usn=}"
"$driver" --usn-read "$image" "$next" >"$workdir/read3.txt"
grep -q "^usn=$next len=88 reason=0x00000100 frn=0x000000000002000a .* name=v3.x ver=3" "$workdir/read3.txt" ||
    fail "V3 record lost: $(cat "$workdir/read3.txt")"
grep -q "^usn=$((next + 88)) .* name=after_v3 ver=2" "$workdir/read3.txt" || fail "record after V3 missing"
next=$((next + 88 + 80))

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

# FSCTL_ENUM_USN_DATA: files by their $STANDARD_INFORMATION USN.
"$driver" --usn-enum "$image" 0 4242 4242 >"$workdir/enum1.txt"
test "$(grep -c '^frn=' "$workdir/enum1.txt")" -eq 1 || fail "enum 4242: $(cat "$workdir/enum1.txt")"
grep -q ' usn=4242 attrs=.* name=h.txt$' "$workdir/enum1.txt" || fail "enum did not find h.txt: $(cat "$workdir/enum1.txt")"
"$driver" --usn-enum "$image" 0 0 0 >"$workdir/enum0.txt"
grep -q 'name=h.txt$' "$workdir/enum0.txt" && fail "enum 0..0 returned h.txt"
grep -q 'name=\.$' "$workdir/enum0.txt" || fail "enum 0..0 is missing the root"
h_frn=$(sed -n 's/^frn=0x\([0-9a-f]*\) .*name=h.txt$/\1/p' "$workdir/enum1.txt")
h_num=$(( 0x$h_frn & 0xFFFFFFFFFFFF ))
next_after=$(grep '^next=' "$workdir/enum1.txt" | cut -d= -f2)
test "$next_after" -gt "$h_num" || fail "enum next $next_after not past h.txt ($h_num)"
if "$driver" --usn-enum "$image" "$next_after" 0 9223372036854775807 >"$workdir/enum2.txt" 2>&1
then
    grep -q 'name=h.txt$' "$workdir/enum2.txt" && fail "enum resumed before h.txt"
else
    grep -q 'c0000011' "$workdir/enum2.txt" || fail "enum past the end: $(cat "$workdir/enum2.txt")"
fi
if "$driver" --usn-enum "$image" 5000 0 9223372036854775807 >"$workdir/enum4.txt" 2>&1
then
    fail "enum from past the end of a small MFT succeeded"
fi
grep -q 'c0000011' "$workdir/enum4.txt" || fail "enum from 5000: $(cat "$workdir/enum4.txt")"
if "$driver" --usn-enum "$image" 4294967296 0 9223372036854775807 >"$workdir/enum3.txt" 2>&1
then
    fail "enum beyond the MFT succeeded"
fi
grep -q 'c0000011' "$workdir/enum3.txt" || fail "enum beyond the MFT: $(cat "$workdir/enum3.txt")"

"$ntfsfix" -n "$image" >/dev/null || fail "ntfsfix found a problem"
"$driver" --check "$image" >"$workdir/check.txt" 2>&1 || fail "ntfslib check: $(tail -n 3 "$workdir/check.txt")"
"$driver" --verify-shape "$image" >"$workdir/shape.txt" 2>&1 || fail "ntfslib verify-shape: $(tail -n 3 "$workdir/shape.txt")"

# A half-created journal (the file exists without $J and $Max) is not active, and
# creating the journal completes it instead of failing.
half="$workdir/half.ntfs"
new_image "$half"
: >"$workdir/empty.bin"
printf '/$Extend/$UsnJrnl=%s\n' "$workdir/empty.bin" >"$workdir/half.list"
"$driver" --addfiles "$half" "$workdir/half.list" >/dev/null
if "$driver" --usn-query "$half" >"$workdir/err2.txt" 2>&1
then
    fail "query succeeded on a half-created journal"
fi
grep -q 'c00002b8' "$workdir/err2.txt" || fail "half-created: $(cat "$workdir/err2.txt")"
"$driver" --usn-create "$half" 33554432 4194304
query=$("$driver" --usn-query "$half")
case "$query" in
    *" first=0 next=0 lowest=0 "*) ;;
    *) fail "half-created journal not repaired: $query" ;;
esac
"$ntfscat" -a 0x80 -n '$Max' "$half" '/$Extend/$UsnJrnl' | wc -c | grep -qx 32 || fail "repaired \$Max missing"

printf 'usn_smoke: ok\n'
