#!/usr/bin/env bash
# Kick off pio pkg install from a clean env, log to /tmp.
set -e
export PATH="$HOME/.local/bin:$PATH"
cd /mnt/c/Users/D/poseidon
echo "[wsl_pio_install] start $(date -u +%FT%TZ)" | tee /tmp/pio_install.log
pio pkg install 2>&1 | tee -a /tmp/pio_install.log
echo "[wsl_pio_install] done $(date -u +%FT%TZ) exit=$?" | tee -a /tmp/pio_install.log
