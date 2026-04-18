#!/usr/bin/env bash
# Clean + build POSEIDON from WSL. Logs to /tmp/pio_build.log.
set -e
export PATH="$HOME/.local/bin:$PATH"
cd /mnt/c/Users/D/poseidon
rm -rf .pio/build
echo "[wsl_pio_build] start $(date -u +%FT%TZ)" | tee /tmp/pio_build.log
pio run 2>&1 | tee -a /tmp/pio_build.log
rc=${PIPESTATUS[0]}
echo "[wsl_pio_build] done $(date -u +%FT%TZ) exit=$rc" | tee -a /tmp/pio_build.log
ls -la .pio/build/cardputer/firmware.bin 2>&1 | tee -a /tmp/pio_build.log
exit $rc
