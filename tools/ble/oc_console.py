#!/usr/bin/env python3
"""Log an OpenCell board's USB console to a file, without resetting it.

    ~/.venvs/opencell/bin/python tools/ble/oc_console.py [--reset] PORT LOG

Runs until stopped (Ctrl-C, or kill its PID), or until the port goes away
(exit status 1, with the reason on stderr). One process per serial port:
while this runs, give oc_ble.py the LOG file (--passkey-from-console LOG),
not the port. The port is opened raw with HUPCL off, so closing it leaves
the board running. --reset restarts the board first (RTS pulse with DTR
low, as esptool does on the ESP32-S3's USB serial), so the log starts at boot.
"""
import argparse
import fcntl
import os
import struct
import sys
import termios
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from oc_ble import ConsoleCodes, open_console  # noqa: E402


def reset(port: str) -> None:
    fd = open_console(port, os.O_RDWR)
    try:
        fcntl.ioctl(fd, termios.TIOCMBIC, struct.pack("I", termios.TIOCM_DTR))
        fcntl.ioctl(fd, termios.TIOCMBIS, struct.pack("I", termios.TIOCM_RTS))
        time.sleep(0.2)
        fcntl.ioctl(fd, termios.TIOCMBIC, struct.pack("I", termios.TIOCM_RTS))
    finally:
        os.close(fd)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--reset", action="store_true", help="restart the board first")
    ap.add_argument("port")
    ap.add_argument("log")
    a = ap.parse_args()
    c = ConsoleCodes(a.port, a.log)
    c.start()  # before the reset, so the boot lines are caught
    if a.reset:
        reset(a.port)
    print(f"logging {a.port} to {a.log}", flush=True)
    try:
        while c.dead is None:
            time.sleep(0.2)
    except KeyboardInterrupt:
        return 0
    finally:
        c.close()
    print(f"oc_console: {c.dead}; stopped", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
