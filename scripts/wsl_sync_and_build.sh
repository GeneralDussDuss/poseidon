#!/usr/bin/env bash
# Build POSEIDON on WSL-native FS (much faster than /mnt/c).
#
# Strategy:
#   - Sync /mnt/c/Users/D/poseidon -> ~/poseidon_build (skip .pio, .git, _audit*)
#   - Build there
#   - Copy firmware.bin + partitions.bin + bootloader.bin back to /mnt/c/.pio
#
set -e
export PATH="$HOME/.local/bin:$PATH"
SRC=/mnt/c/Users/D/poseidon
DST=$HOME/poseidon_build
LOG=/tmp/pio_build.log

mkdir -p "$DST"
echo "[sync] $(date -u +%FT%TZ) start" | tee "$LOG"
rsync -a --delete \
  --exclude='.pio/' \
  --exclude='.git/' \
  --exclude='_audit_raspyjack/' \
  --exclude='.refs/' \
  --exclude='scripts/' \
  --exclude='docs/' \
  "$SRC/" "$DST/" 2>&1 | tee -a "$LOG"
echo "[sync] done" | tee -a "$LOG"

cd "$DST"
echo "[build] $(date -u +%FT%TZ) start" | tee -a "$LOG"
pio run 2>&1 | tee -a "$LOG"
rc=${PIPESTATUS[0]}
echo "[build] done exit=$rc $(date -u +%FT%TZ)" | tee -a "$LOG"
ls -la .pio/build/cardputer/firmware.bin 2>&1 | tee -a "$LOG"

if [ -f .pio/build/cardputer/firmware.bin ]; then
    mkdir -p "$SRC/.pio/build/cardputer"
    cp .pio/build/cardputer/firmware.bin "$SRC/.pio/build/cardputer/firmware.bin"
    cp .pio/build/cardputer/partitions.bin "$SRC/.pio/build/cardputer/partitions.bin" 2>/dev/null || true
    cp .pio/build/cardputer/bootloader.bin "$SRC/.pio/build/cardputer/bootloader.bin" 2>/dev/null || true
    cp .pio/build/cardputer/firmware.elf  "$SRC/.pio/build/cardputer/firmware.elf"  2>/dev/null || true
    echo "[copyback] firmware.bin -> $SRC" | tee -a "$LOG"
fi
exit $rc
