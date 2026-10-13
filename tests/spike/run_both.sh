#!/bin/sh
# Прогоняет один и тот же ELF на асинхронном и синхронном DRAM-контроллере.
#   run_both.sh [name]   (по умолчанию fib; ожидается tests/spike/<name>.elf)
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
NAME=${1:-fib}
ELF="$DIR/$NAME.elf"
BIN="$ROOT/build/spike_dram"

echo "=== async: $NAME ==="
"$BIN" --async "$ELF"
echo
echo "=== sync: $NAME ==="
"$BIN" --sync "$ELF"
