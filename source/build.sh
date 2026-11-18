#!/bin/sh
# Rebuilds ChaseGuard.asi from the base build.  Needs: GNU binutils (as, ld, objcopy, nm), python3.
# Put the base build next to this script as orig.asi (SHA-256 in BUILD.txt).
set -e
DEFS="--defsym RET0=0x10001159 --defsym P_AFTER=0x10001177 --defsym A_AFTER=0x1000121d \
--defsym B_AFTER=0x10001277 --defsym B_START=0x10001271 --defsym C_AFTER=0x100011e5 \
--defsym RET_TRUE=0x10001398 --defsym DLL_CONT=0x100013ad --defsym IAT_MSGBOX=0x10012040 \
--defsym IAT_FOPEN=0x1001208c --defsym G_FILE=0x100863c8 --defsym MODE_A=0x100123b4"
as --32 -o caves.o caves.s
ld -m elf_i386 -Ttext=0x10011f25 -e 0 $DEFS -o caves.elf caves.o
objcopy -O binary -j .text caves.elf caves.bin
python3 patch.py
python3 rename.py
sha256sum ChaseGuard.asi
