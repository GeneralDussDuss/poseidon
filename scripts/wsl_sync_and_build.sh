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

copy_env() {
    local env="$1"
    [ -f ".pio/build/$env/firmware.bin" ] || return 0
    mkdir -p "$SRC/.pio/build/$env"
    cp ".pio/build/$env/firmware.bin"         "$SRC/.pio/build/$env/firmware.bin"
    cp ".pio/build/$env/firmware.factory.bin" "$SRC/.pio/build/$env/firmware.factory.bin" 2>/dev/null || true
    cp ".pio/build/$env/partitions.bin"       "$SRC/.pio/build/$env/partitions.bin"       2>/dev/null || true
    cp ".pio/build/$env/bootloader.bin"       "$SRC/.pio/build/$env/bootloader.bin"       2>/dev/null || true
    cp ".pio/build/$env/firmware.elf"         "$SRC/.pio/build/$env/firmware.elf"         2>/dev/null || true
    echo "[copyback] $env -> $SRC" | tee -a "$LOG"
}
copy_env cardputer
copy_env cardputer-launcher
exit $rc
