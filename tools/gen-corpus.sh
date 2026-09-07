#!/bin/sh
# gen-corpus.sh -- regenerate tests/vm-corpus.txt from hardware.
#
# Needs gdb and an x86-64 host; the everyday test does not, which is the
# whole point of checking the corpus in.  ~24 state blocks x ~210 forms.
set -e
cd "$(dirname "$0")/.."
out=${1:-tests/vm-corpus.txt}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
gcc -O0 -g -o "$tmp/oracle" tools/oracle.c
ORACLE_BLOCKS=${ORACLE_BLOCKS:-24} ORACLE_OUT="$tmp/corpus" \
gdb -q -batch -x tools/oracle.py --args "$tmp/oracle" > "$tmp/gdb.log" 2>&1 || {
    cat "$tmp/gdb.log" >&2; exit 1; }
grep -E "forms,|vectors" "$tmp/gdb.log" >&2 || true
mv "$tmp/corpus" "$out"
printf 'wrote %s: %s lines, %s\n' "$out" "$(wc -l < "$out")" "$(du -h "$out" | cut -f1)"
