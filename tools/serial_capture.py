"""Capture serial output from the board for a fixed duration."""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM7"
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0

s = serial.Serial()
s.port = port
s.baudrate = 115200
s.timeout = 0.2
s.dtr = False
s.rts = False
s.open()

end = time.time() + seconds
buf = bytearray()
while time.time() < end:
    buf += s.read(4096)
s.close()

text = buf.decode(errors="replace")
print(text)
print(f"--- captured {len(buf)} bytes in {seconds:.0f}s ---")
