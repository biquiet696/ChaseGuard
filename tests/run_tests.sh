#!/bin/sh
# Runs the decision-logic tests natively against a real speed.exe v1.3.
# Usage: tests/run_tests.sh /path/to/speed.exe
set -eu
cd "$(dirname "$0")/.."
EXE=${1:?usage: tests/run_tests.sh /path/to/speed.exe}
mkdir -p build/tests
cc -O1 -g -w -DZYDIS_STATIC_BUILD -c third_party/zydis/Zydis.c -o build/tests/zydis.o
cc -O1 -g -Wall -Wextra -DZYDIS_STATIC_BUILD -c src/core.c -o build/tests/core.o
cc -O1 -g -Wall -Wextra -DZYDIS_STATIC_BUILD -c tests/test_core.c -o build/tests/test_core.o
cc -o build/tests/test_core build/tests/test_core.o build/tests/core.o build/tests/zydis.o
LISTING=build/tests/text.lst
if command -v llvm-objdump >/dev/null 2>&1; then
  llvm-objdump -d --no-show-raw-insn -M intel --start-address=0x401000 --stop-address=0x88f2a6 "$EXE" > "$LISTING"
  build/tests/test_core "$EXE" "$LISTING"
else
  build/tests/test_core "$EXE"
fi
