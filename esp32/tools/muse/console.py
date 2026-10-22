#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Send console lines to a board and print what comes back.

    tools/muse/console.py <port> <secs> "<line>" ["<line>" ...]

monitor.py resets the board and only reads; this one leaves it running and
types at it.  The firmware's console wants a '>' before a command line, so each
argument is sent with that prefix and a newline.  Lines are timed from the
moment the port opened, which is what makes a sleep, a wake and a ring
orderable; keep the port open for as long as the test runs, since the board
holds USB-Serial/JTAG awake only while someone is listening.

Example - ring in 20 s, then read the wake-up log:
    tools/muse/console.py COM3 60 "alarm.arm=20"
"""
import sys
import time

import serial

s = serial.Serial()
s.port = sys.argv[1]
s.baudrate = 115200
s.timeout = 0.2
# DTR/RTS must be low *before* the port opens: pyserial's default open asserts
# them, and on USB-Serial/JTAG that is the reset line - a console session must
# not restart what it is measuring (it hides every pending alarm/F flag).
s.dtr = False
s.rts = False
s.open()
seconds = float(sys.argv[2])
lines = sys.argv[3:]

start = time.monotonic()
for line in lines:
    s.write((">" + line + "\n").encode("utf-8"))
    s.flush()
    time.sleep(0.8)

while time.monotonic() - start < seconds:
    data = s.read(4096)
    if not data:
        continue
    for raw in data.split(b"\n"):
        text = raw.decode("utf-8", "replace").rstrip("\r")
        if text:
            print("%7.2f %s" % (time.monotonic() - start, text), flush=True)
s.close()
