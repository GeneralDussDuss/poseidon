"""Tail a serial port line-by-line with auto-reconnect on disconnect.
Used as a Monitor source so each log line arrives as a chat event."""
import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else 'COM19'
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass

def safe_print(s):
    try:
        print(s, flush=True)
    except Exception:
        print(s.encode('ascii', errors='replace').decode('ascii'), flush=True)

def clean(line_bytes):
    try:
        decoded = line_bytes.decode('utf-8', errors='replace').rstrip()
        return ''.join(c if 0x20 <= ord(c) < 0x7F or c == '\t' else '?'
                       for c in decoded)
    except Exception:
        return repr(line_bytes)

buf = b''
while True:
    try:
        s = serial.Serial(port, baud, timeout=0.1)
    except Exception as e:
        # Port not present yet (device resetting / unplugged). Retry.
        time.sleep(0.5)
        continue
    try:
        while True:
            chunk = s.read(256)
            if chunk:
                buf += chunk
                while b'\n' in buf:
                    line, buf = buf.split(b'\n', 1)
                    text = clean(line)
                    if text:
                        safe_print(text)
    except (serial.SerialException, OSError):
        # Device disappeared mid-read. Reopen after a brief pause.
        try: s.close()
        except Exception: pass
        time.sleep(0.5)
        continue
