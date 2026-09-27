#!/usr/bin/env python3
"""Draw an OpenCell activation QR code (the web portal's job, on the bench).

    ~/.venvs/opencell/bin/python tools/qr/qr.py 'opencell:1:...'             # in the terminal
    ~/.venvs/opencell/bin/python tools/qr/qr.py 'opencell:1:...' --png qr.png

`lcbench mkqr` prints the text. Needs segno (pip install segno).
"""
import argparse
import sys

import segno


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("text", help="the opencell:1:... activation text")
    ap.add_argument("--png", help="also write a PNG (scale 8) to this path")
    a = ap.parse_args()
    if not a.text.startswith("opencell:1:"):
        print("not an OpenCell activation text (expected opencell:1:...)", file=sys.stderr)
        return 2
    qr = segno.make(a.text, error="m", micro=False)
    qr.terminal(compact=True)
    if a.png:
        qr.save(a.png, scale=8, border=4)
        print(f"wrote {a.png}")
    print(f"QR version {qr.version}, error level {qr.error}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
