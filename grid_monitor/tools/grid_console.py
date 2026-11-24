#!/usr/bin/env python3
"""
grid_console.py - talk to the Grid Monitor over the ST-LINK serial port.

  python grid_console.py --list
  python grid_console.py --port COM5 "grid status"
  python grid_console.py --port COM5 "grid dump 2048" --out dump.txt
  python grid_console.py --port /dev/tty.usbmodem1103 "grid fault_log"
  python grid_console.py --port COM5 --interactive

Needs: pip install pyserial
"""
import argparse
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pip install pyserial")


def run(ser, cmd, until=None, quiet=0.8, timeout=20.0):
    ser.reset_input_buffer()
    ser.write((cmd + "\r\n").encode())
    out, t0, last = bytearray(), time.time(), time.time()
    while time.time() - t0 < timeout:
        chunk = ser.read(4096)
        if chunk:
            out += chunk
            last = time.time()
            if until and until.encode() in out:
                time.sleep(0.2)
                out += ser.read(4096)
                break
        elif time.time() - last > quiet and out:
            break
    return out.decode(errors="ignore")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("command", nargs="?")
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", help="save output to this file")
    ap.add_argument("--list", action="store_true", help="list serial ports")
    ap.add_argument("--interactive", action="store_true")
    a = ap.parse_args()
    if a.list:
        for p in list_ports.comports():
            print(p.device, "-", p.description)
        return
    if not a.port:
        ap.error("--port is required (see --list)")
    ser = serial.Serial(a.port, a.baud, timeout=0.2)
    if a.interactive:
        print("type commands, Ctrl-C to quit")
        try:
            while True:
                c = input("grid> ")
                print(run(ser, c, until="DUMP END" if "dump" in c else None))
        except (KeyboardInterrupt, EOFError):
            return
    if not a.command:
        ap.error("give a command, e.g. \"grid status\"")
    text = run(ser, a.command, until="DUMP END" if "dump" in a.command else None)
    if a.out:
        open(a.out, "w").write(text)
        print("saved", a.out, f"({len(text.splitlines())} lines)")
    else:
        print(text)


if __name__ == "__main__":
    main()
