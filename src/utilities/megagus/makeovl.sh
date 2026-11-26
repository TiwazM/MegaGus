#!/bin/sh
# makeovl.sh PROGRAM.elf OUT: the four overlays of the linked module player, one behind the
# other, as the program file MODPLAY.OVL (its first two bytes are a load address, which
# BLOAD with P passes over). Each is dumped by its section's name: two of them are linked
# to the same addresses, which a copy by address cannot tell apart.
set -e
OBJCOPY="${LLVM_MOS:+$LLVM_MOS/bin/}llvm-objcopy"
T=$(mktemp -d)
printf '\000\000' > "$2"
for n in 1 2 3 4; do
  "$OBJCOPY" --dump-section .ovl$n="$T/ovl$n" "$1" "$T/unused"
  cat "$T/ovl$n" >> "$2"
done
rm -r "$T"
