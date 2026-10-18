"""Drive the board's console over USB and print everything it says.

    python board_serial.py <seconds> [port] [--cmd ">imu" ...]

Commands are written as console lines (the firmware reads a line after '>'),
spaced out so each one's answer is separable in the output. The board resets
first, so every run starts from a known state.
"""
import sys
import time

import serial

args = sys.argv[1:]
secs = float(args.pop(0)) if args and not args[0].startswith("--") else 15.0
port_name = "COM3"
if args and not args[0].startswith("--"):
    port_name = args.pop(0)
cmds = [args[i + 1] for i, a in enumerate(args) if a == "--cmd"]
gap = 3.0

port = serial.Serial(port_name, 115200, timeout=0.05)
port.setDTR(False)
port.setRTS(True)
time.sleep(0.15)
port.reset_input_buffer()
port.setRTS(False)

start = time.time()
next_cmd = start + 6.0          # let it boot before the first command
pending = list(cmds)
while time.time() - start < secs:
    data = port.read(4096)
    if data:
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()
    if pending and time.time() >= next_cmd:
        cmd = pending.pop(0)
        sys.stdout.write("\n>>> %s\n" % cmd)
        sys.stdout.flush()
        port.write((cmd + "\n").encode())
        port.flush()
        next_cmd = time.time() + gap
port.close()
