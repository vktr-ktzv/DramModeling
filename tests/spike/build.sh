#!/bin/sh
# Сборка bare-metal теста под Spike.
#   build.sh [name]  ->  tests/spike/<name>.cpp (+ crt0.S)  или  <name>.S
# По умолчанию name=dram_test.
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
NAME=${1:-dram_test}
CC=${CC:-clang}
CXX=${CXX:-clang++}
LD=${LD:-riscv64-linux-gnu-ld}

TARGET="--target=riscv64 -march=rv64ima -mabi=lp64 -mcmodel=medany"
CFLAGS="-nostdlib -ffreestanding -fno-stack-protector"

if [ -f "$DIR/$NAME.cpp" ]; then
    "$CXX" $TARGET $CFLAGS -fno-exceptions -fno-rtti -fno-threadsafe-statics \
        -std=c++20 -O2 -c "$DIR/crt0.S" -o "$DIR/crt0.o"
    "$CXX" $TARGET $CFLAGS -fno-exceptions -fno-rtti -fno-threadsafe-statics \
        -std=c++20 -O2 -c "$DIR/$NAME.cpp" -o "$DIR/$NAME.o"
    "$LD" -T "$DIR/link.ld" "$DIR/crt0.o" "$DIR/$NAME.o" -o "$DIR/$NAME.elf"
else
    "$CC" $TARGET $CFLAGS -c "$DIR/$NAME.S" -o "$DIR/$NAME.o"
    "$LD" -T "$DIR/link.ld" "$DIR/$NAME.o" -o "$DIR/$NAME.elf"
fi

echo "built $DIR/$NAME.elf"
