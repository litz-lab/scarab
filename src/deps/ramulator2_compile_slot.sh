#!/bin/sh
# ramulator2_compile_slot.sh SLOT_DIR SLOTS COMMAND...
#
# Compile launcher for the ramulator2 target (src/deps/CMakeLists.txt). Runs COMMAND while
# holding one of SLOTS lock files in SLOT_DIR, so at most SLOTS Ramulator 2 files compile at
# once. src/Makefile builds with an unlimited `make -j`; without this, all of Ramulator 2's
# translation units (0.3-0.45 GB each at -O3) start together and need ~24 GB, more than a CI
# runner has. The lock is held on fd 9 and released when COMMAND exits.
slot_dir=$1
slots=$2
shift 2
mkdir -p "$slot_dir"
while :; do
  i=0
  while [ "$i" -lt "$slots" ]; do
    exec 9>"$slot_dir/slot$i"
    if flock -n 9; then
      "$@"
      exit $?
    fi
    i=$((i + 1))
  done
  sleep 1
done
