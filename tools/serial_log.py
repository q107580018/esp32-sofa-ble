#!/usr/bin/env python3
"""Capture N seconds of CLEAN text from the ESP32 UART.

Why this exists: `idf.py monitor` is an interactive TTY. It grabs the terminal,
emits ANSI escape sequences, and never returns on its own - so a coding agent
cannot use it. This script runs for a bounded time, prints plain text, and exits.

Usage:
    tools/monitor.sh 10                                  # 10s, auto-detect port
    tools/monitor.sh 5 --port /dev/cu.usbserial-2110
    tools/monitor.sh 10 --reset                          # hard-reset first, catch boot log
    tools/monitor.sh 10 --raw                            # no timestamp prefix
    tools/monitor.sh 10 --send "sofa status"             # type a console command first
    tools/monitor.sh 8 --reset --send "sofa list"        # boot, then poke the REPL

--send is a project-local addition to the shared wrapper. This firmware runs an
esp_console REPL on the same UART, and an agent cannot drive an interactive TTY
-- so --send is the only way to exercise the commands without a human.

Exit codes:
    0  capture finished
    2  no serial port found
    3  port could not be opened (usually busy - another monitor is running)
"""

import argparse
import glob
import sys
import time


def autodetect_port() -> str | None:
    """Prefer a USB-UART bridge (CH340/CP210x/FTDI), then native USB-CDC."""
    for pattern in ("/dev/cu.usbserial-*", "/dev/cu.SLAB_USBtoUART*",
                    "/dev/cu.wchusbserial*", "/dev/cu.usbmodem*"):
        hits = sorted(glob.glob(pattern))
        if hits:
            return hits[0]
    return None


def hard_reset(ser) -> None:
    """esptool-style reset into normal boot mode (GPIO0 high, pulse EN low)."""
    ser.setDTR(False)   # GPIO0 = high -> normal boot, not download mode
    ser.setRTS(True)    # EN = low  -> hold chip in reset
    time.sleep(0.1)
    ser.setRTS(False)   # EN = high -> release, chip boots
    time.sleep(0.05)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("seconds", nargs="?", type=float, default=5.0,
                    help="how long to capture (default: 5)")
    ap.add_argument("--port", default=None, help="serial device (default: autodetect)")
    ap.add_argument("--baud", type=int, default=115200, help="baud rate (default: 115200)")
    ap.add_argument("--reset", action="store_true",
                    help="hard-reset the chip before capturing, to catch the boot log")
    ap.add_argument("--raw", action="store_true", help="do not prefix lines with a timestamp")
    ap.add_argument("--send", action="append", metavar="TEXT", default=None,
                    help="write TEXT + CR/LF to the console after opening the port; "
                         "repeat the flag to send several lines")
    ap.add_argument("--send-gap", type=float, default=1.0,
                    help="seconds to wait between --send lines (default: 1)")
    args = ap.parse_args()

    try:
        import serial  # pyserial, provided by the ESP-IDF python environment
    except ImportError:
        print("error: pyserial missing. Run this via tools/monitor.sh, "
              "or: python3 -m pip install pyserial", file=sys.stderr)
        return 3

    port = args.port or autodetect_port()
    if not port:
        print("error: no serial port found. Plug in the board, or pass --port.",
              file=sys.stderr)
        return 2

    try:
        ser = serial.Serial(port, args.baud, timeout=0.2)
    except serial.SerialException as exc:
        print(f"error: cannot open {port}: {exc}", file=sys.stderr)
        print("hint: another monitor may hold the port. Close it and retry.", file=sys.stderr)
        return 3

    with ser:
        if args.reset:
            hard_reset(ser)
        print(f"# capturing {args.seconds:g}s from {port} @ {args.baud} baud", file=sys.stderr)

        pending = list(args.send or [])
        next_send = time.time() + 0.5 if pending else None

        start = time.time()
        while time.time() - start < args.seconds:
            if pending and time.time() >= next_send:
                line = pending.pop(0)
                ser.write((line + "\r\n").encode())
                ser.flush()
                print(f"# sent: {line}", file=sys.stderr)
                next_send = time.time() + args.send_gap
            try:
                line = ser.readline().decode("utf-8", "replace").rstrip("\r\n")
            except serial.SerialException as exc:
                print(f"# serial error: {exc}", file=sys.stderr)
                return 3
            if not line:
                continue
            if args.raw:
                print(line)
            else:
                elapsed = time.time() - start
                print(f"[{elapsed:7.3f}] {line}")
            sys.stdout.flush()

    return 0


if __name__ == "__main__":
    sys.exit(main())
