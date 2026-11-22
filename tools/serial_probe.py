#!/usr/bin/env python3
"""Read the device's serial output, optionally sending commands first.

`pio device monitor` needs a TTY, so this is the scriptable equivalent.
Usage: serial_probe.py [seconds] [command ...]
"""
import sys
import time

import serial

PORT = "/dev/cu.usbserial-52D20506211"
BAUD = 115200

duration = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
commands = sys.argv[2:]

with serial.Serial(PORT, BAUD, timeout=0.2) as s:
    time.sleep(0.3)
    for c in commands:
        s.write((c + "\n").encode())
        s.flush()
        time.sleep(0.5)
    deadline = time.time() + duration
    while time.time() < deadline:
        data = s.read(4096)
        if data:
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()
