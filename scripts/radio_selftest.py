#!/usr/bin/env python3
"""
radio_selftest.py - drive POSEIDON's on-device radio self-test suite over USB
serial and report a verdict.

The firmware side lives in src/selftest.cpp; this script only orchestrates and
judges. It resets the board into its application, issues a suite command, parses
the machine-readable [TEST] lines, prints a table and exits non-zero on failure
so it can gate a build.

  python scripts/radio_selftest.py                  # all suites, auto-detect port
  python scripts/radio_selftest.py --suite N        # nRF24 only
  python scripts/radio_selftest.py --port COM3 --json results.json

Suites: W WiFi | B BLE | C CC1101 | N nRF24 | L OTA loopback | A all

Requires pyserial:  python -m pip install pyserial
"""

import argparse
import json
import re
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial not installed. Run: python -m pip install pyserial")


BAUD = 115200

# A suite may legitimately take a while: the BLE sweep alone is 4 s and the
# loopback runs three scan rounds. Give each one room, then give up rather than
# hang a CI job forever.
SUITE_TIMEOUT = {
    "W": 40, "B": 40, "C": 30, "N": 30, "L": 90, "P": 60, "X": 90, "Z": 60, "Y": 40, "A": 180,
}

TEST_RE = re.compile(
    r"\[TEST\]\s+name=(?P<name>\S+)\s+status=(?P<status>\w+)\s+"
    r"ms=(?P<ms>\d+)(?:\s+heap=(?P<heap>-?\d+))?\s+detail=(?P<detail>.*)$"
)
SUM_RE = re.compile(
    r"\[TESTSUM\]\s+pass=(?P<p>\d+)\s+fail=(?P<f>\d+)\s+skip=(?P<s>\d+)\s+"
    r"ms=(?P<ms>\d+)\s+heap_delta=(?P<heap>-?\d+)"
)

GREEN, RED, YELLOW, DIM, RESET = (
    "\033[32m", "\033[31m", "\033[33m", "\033[2m", "\033[0m"
)
PAINT = {"PASS": GREEN, "FAIL": RED, "SKIP": YELLOW}


def find_port(explicit=None):
    """Pick the ESP32-S3 CDC port. The board can move between COM numbers after
    a dropped connection, so prefer probing over hardcoding."""
    if explicit:
        return explicit
    candidates = []
    for p in list_ports.comports():
        blob = f"{p.description} {p.manufacturer or ''} {p.hwid or ''}".lower()
        # Skip Bluetooth virtual ports, which look like serial devices but are not.
        if "bluetooth" in blob:
            continue
        score = 0
        if "usb jtag" in blob or "303a" in blob:   # 303a = Espressif VID
            score += 10
        if "usb serial" in blob or "cdc" in blob:
            score += 5
        candidates.append((score, p.device))
    if not candidates:
        sys.exit("No serial ports found. Is the board plugged in?")
    candidates.sort(reverse=True)
    return candidates[0][1]


def open_board(port):
    """Open the port and boot the application.

    On the ESP32-S3's native USB-Serial/JTAG, RTS drives EN (reset) and DTR
    drives BOOT. Asserting both drops the chip into the ROM download stub and
    the app never runs, so hold BOOT released and pulse reset instead.
    """
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = BAUD
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()

    # Reboot into a KNOWN-FRESH session. The RTS pulse alone does not reliably
    # reset this board over USB-Serial/JTAG - every run after the first was
    # landing on a stale session, which is why BLE (which cannot be
    # re-initialised after teardown in the same boot) failed from run 2 onward.
    # The harness's own R command does a real ESP.restart().
    ser.rts = True
    time.sleep(0.15)
    ser.rts = False
    time.sleep(0.2)
    ser.reset_input_buffer()
    try:
        ser.write(b"R\n")
        ser.flush()
    except Exception:
        pass
    time.sleep(0.4)
    ser.reset_input_buffer()
    return ser


def wait_for_banner(ser, timeout=20):
    """Wait until the harness announces itself, so we do not fire a command at a
    device that is still in its splash screen and drop it on the floor."""
    deadline = time.time() + timeout
    buf = ""
    while time.time() < deadline:
        chunk = ser.read(4096).decode("utf-8", "replace")
        if chunk:
            buf += chunk
            if "serial test harness ready" in buf:
                return True
    return False


def run_suite(ser, suite, verbose=False):
    """Issue one suite and collect its results."""
    ser.reset_input_buffer()
    ser.write(f"T{suite}\n".encode())
    ser.flush()

    results, summary = [], None
    deadline = time.time() + SUITE_TIMEOUT.get(suite, 60)
    buf = ""
    while time.time() < deadline:
        chunk = ser.read(4096).decode("utf-8", "replace")
        if not chunk:
            continue
        buf += chunk
        while "\n" in buf:
            line, buf = buf.split("\n", 1)
            line = line.strip()
            if verbose and line:
                print(f"{DIM}  {line}{RESET}")
            m = TEST_RE.search(line)
            if m:
                results.append(m.groupdict())
                continue
            m = SUM_RE.search(line)
            if m:
                summary = m.groupdict()
        if summary:
            break
    return results, summary


def main():
    ap = argparse.ArgumentParser(description="POSEIDON radio self-test runner")
    ap.add_argument("--port", help="serial port (default: auto-detect)")
    ap.add_argument("--suite", default="A",
                    help="W B C N L A (default A = everything)")
    ap.add_argument("--json", metavar="FILE", help="write results as JSON")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="echo raw serial output")
    args = ap.parse_args()

    suite = args.suite.upper()
    if suite not in SUITE_TIMEOUT:
        sys.exit(f"Unknown suite '{suite}'. Choose from {', '.join(SUITE_TIMEOUT)}")

    port = find_port(args.port)
    print(f"port   : {port}")
    print(f"suite  : {suite}")

    ser = open_board(port)
    try:
        if not wait_for_banner(ser):
            # Not fatal: the board may already be booted and past the banner.
            print(f"{YELLOW}warning: never saw the harness banner; "
                  f"sending anyway{RESET}")
        results, summary = run_suite(ser, suite, args.verbose)
    finally:
        ser.close()

    if not results:
        print(f"{RED}No test output received.{RESET} The device may be stuck, "
              f"or this firmware predates the selftest module.")
        return 2

    width = max(len(r["name"]) for r in results)
    print()
    for r in results:
        colour = PAINT.get(r["status"], "")
        hp = r.get("heap")
        # Only show heap movement worth a second look; zeros are noise.
        hs = ""
        if hp is not None and abs(int(hp)) >= 1024:
            hs = f" {YELLOW}{int(hp)/1024:+.1f}KB{RESET}"
        print(f"  {r['name']:<{width}}  {colour}{r['status']:<4}{RESET}  "
              f"{int(r['ms']):>6} ms{hs}  {DIM}{r['detail']}{RESET}")
    print()

    if summary:
        p, f, s = int(summary["p"]), int(summary["f"]), int(summary["s"])
        heap = int(summary["heap"])
        verdict = f"{GREEN}ALL PASS{RESET}" if f == 0 else f"{RED}{f} FAILED{RESET}"
        print(f"  {verdict}   pass={p} fail={f} skip={s}   "
              f"heap delta {heap:+d} B   total {int(summary['ms'])} ms")
        # A run that leaks meaningfully is worth surfacing even when green.
        if heap < -4096:
            print(f"  {YELLOW}note: heap dropped {-heap} B across the run{RESET}")
    else:
        f = sum(1 for r in results if r["status"] == "FAIL")
        print(f"  {YELLOW}incomplete: no summary line (timed out?){RESET}")

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump({"port": port, "suite": suite,
                       "results": results, "summary": summary}, fh, indent=2)
        print(f"  wrote {args.json}")

    if summary is None:
        return 2
    return 1 if int(summary["f"]) else 0


if __name__ == "__main__":
    sys.exit(main())
