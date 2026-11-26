#!/bin/sh
# Builds ChaseGuard.asi on Linux with LLVM (clang, lld-link, llvm-dlltool, llvm-rc).
# No Windows SDK, no MSVC and no C runtime are needed; the output has no CRT dependency.
#   Ubuntu/Debian:  sudo apt install clang lld llvm
# Usage: ./build.sh            -> build/ChaseGuard.asi
set -eu
cd "$(dirname "$0")"
CC=${CC:-clang}
OUT=build
mkdir -p "$OUT"

CFLAGS="--target=i686-pc-windows-msvc -O2 -std=c11 -ffreestanding -fno-builtin -nostdinc
 -isystem $($CC -print-resource-dir)/include
 -fno-stack-protector -fno-ident -ffunction-sections -fdata-sections -mno-sse
 -DZYAN_NO_LIBC -DZYDIS_STATIC_BUILD -DZYAN_RELEASE
 -ffile-prefix-map=$(pwd)=. -Wall -Wextra -Wno-unused-parameter"

for lib in kernel32 user32; do
  llvm-dlltool -m i386 -k -d tools/def/$lib.def -l "$OUT/$lib.lib"
done
$CC $CFLAGS -w -c third_party/zydis/Zydis.c -o "$OUT/zydis.obj"
$CC $CFLAGS -c src/core.c       -o "$OUT/core.obj"
$CC $CFLAGS -c src/chaseguard.c -o "$OUT/chaseguard.obj"
$CC $CFLAGS -c src/crt.c        -o "$OUT/crt.obj"
llvm-rc -no-preprocess -I src -fo "$OUT/version.res" src/version.rc 2>/dev/null \
  || llvm-rc -I src -fo "$OUT/version.res" src/version.rc

lld-link /nologo /dll /entry:DllMain@12 /nodefaultlib /subsystem:windows,5.01 /machine:x86 \
  /safeseh /dynamicbase /nxcompat /opt:ref /opt:icf /brepro /pdbaltpath:%_PDB% \
  /out:"$OUT/ChaseGuard.asi" \
  "$OUT/chaseguard.obj" "$OUT/core.obj" "$OUT/zydis.obj" "$OUT/crt.obj" "$OUT/version.res" \
  "$OUT/kernel32.lib" "$OUT/user32.lib"

sha256sum "$OUT/ChaseGuard.asi"
