#!/usr/bin/env bash
# Full build+flash+test cycle for deauth autotest. Run from WSL.
# Outputs to /tmp/deauth_loop.log and prints pass/fail at the end.
set -e
export PATH="$HOME/.local/bin:$PATH"

LOG=/tmp/deauth_loop.log
: > "$LOG"

echo "[$(date +%T)] sync+build..." | tee -a "$LOG"
bash /mnt/c/Users/D/poseidon/scripts/wsl_sync_and_build.sh 2>&1 | tail -10 | tee -a "$LOG"

BIN=/home/kali/poseidon_build/.pio/build/cardputer/firmware.factory.bin
if [ ! -f "$BIN" ]; then
    echo "[FAIL] no factory.bin" | tee -a "$LOG"
    exit 1
fi

cp "$BIN" /mnt/c/Users/D/poseidon/_release/poseidon-v0.4.0-factory.bin
echo "[$(date +%T)] flashing..." | tee -a "$LOG"

# Flash from Windows esptool (WSL can't see COM16 without USBIPD)
# Shell out to cmd.exe which can invoke python from Windows.
cmd.exe /c "python -m esptool --chip esp32s3 --port COM16 --baud 460800 --no-stub write_flash 0x0 C:\\Users\\D\\poseidon\\_release\\poseidon-v0.4.0-factory.bin" 2>&1 | tail -4 | tee -a "$LOG"

echo "[$(date +%T)] grabbing serial 20s..." | tee -a "$LOG"
cmd.exe /c "powershell -ExecutionPolicy Bypass -File C:\\Users\\D\\poseidon\\scripts\\serial_grab.ps1 -Port COM16 -Seconds 20" 2>&1 | tee -a "$LOG"

echo "" | tee -a "$LOG"
echo "=== DEAUTH AUTOTEST RESULT ===" | tee -a "$LOG"
grep -E "autotest\]|DEAUTH|rc=|ok=|fail=|wifi:" "$LOG" | tail -20 | tee -a "$LOG"
