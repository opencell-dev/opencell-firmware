#!/usr/bin/env python3
"""Drive an OpenCell terminal over BLE (contract v2, lc_term_gatt.h) from the laptop.

Runs its steps in order over one connection and prints every EVENT and
STATUS notification, decoded. Exits 1 if a step fails (a wait times out, a
command is refused unexpectedly, or a ping frame doesn't come back).

    ~/.venvs/opencell/bin/python tools/ble/oc_ble.py [--name OpenCell-76AD0488] STEP...

    ~/.venvs/opencell/bin/python tools/ble/oc_ble.py scan    # adverts and RSSI for 10 s, no connection

A laptop more than a few metres from the board hears it at about -95 dBm and
the connection dies at once (supervision timeout, reason 0x08). Keep the
RSSI above -80 dBm. The phone app must not be connected: the terminal takes
one connection (adb shell am force-stop org.opencell.app).

Steps:
    status                 read STATUS
    activate:TEXT          COMMAND ACTIVATE (TEXT from lcbench mkqr)
    dial:+883...            COMMAND DIAL
    answer | reject | hangup
    deactivate              COMMAND DEACTIVATE (with the 0xA5 confirmation)
    wait:EVENT[:S]          wait up to S s (default 30) for an EVENT: activated, act_failed,
                            registered, reg_failed, incoming, ringing, connected, ended, deactivated
    ping[:N]                send N (default 5) app data frames on UP, expect each back on DOWN
    sleep:S
    err:0xNN:STEP           run STEP, expecting ATT error 0xNN (e.g. err:0x80:dial:+8836065550100)

Needs bleak (pip install bleak).
"""
import argparse
import asyncio
import re
import sys

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError


def uuid(n: int) -> str:
    return f"6c63000{n}-7e2a-4b8e-9f2d-3c1a5e7b0d10"


UP, DOWN, STATUS, COMMAND, EVENT = uuid(2), uuid(3), uuid(4), uuid(5), uuid(6)

EVENTS = {1: "activated", 2: "act_failed", 3: "registered", 4: "reg_failed", 5: "incoming",
          6: "ringing", 7: "connected", 8: "ended", 9: "deactivated"}
SIG_STATES = ["not_activated", "activating", "registering", "registered", "calling", "ringing_out",
              "ringing_in", "in_call", "releasing"]
TERM_STATES = ["search", "synced", "attaching", "idle", "granted"]
ACT_REASONS = {1: "unknown token", 2: "token used", 3: "token expired", 4: "bad tag", 5: "bad confirm",
               6: "timeout"}
REG_REASONS = {1: "not activated", 2: "auth failed", 3: "network auth failed", 4: "timeout"}
CAUSES = ["normal", "rejected", "busy", "no answer", "unreachable", "network failure", "link lost"]
CMD = {"answer": 0x03, "reject": 0x04, "hangup": 0x05}


def number(b: bytes) -> str:
    digits = []
    for byte in b:
        for d in (byte >> 4, byte & 0x0F):
            if d > 9:
                return "+" + "".join(digits)
            digits.append(str(d))
    return "+" + "".join(digits)


def decode_event(e: bytes) -> str:
    name = EVENTS.get(e[0], f"0x{e[0]:02x}")
    a = e[1:]
    if e[0] == 1 and len(a) >= 7:
        return f"{name} number={number(a[:7])}"
    if e[0] == 2 and a:
        return f"{name} reason={a[0]} ({ACT_REASONS.get(a[0], '?')})"
    if e[0] == 3 and len(a) >= 8:
        return f"{name} number={number(a[:7])} mode={'part15' if a[7] == 1 else 'part97'}"
    if e[0] == 4 and a:
        return f"{name} reason={a[0]} ({REG_REASONS.get(a[0], '?')})"
    if e[0] == 5 and len(a) >= 11:
        return f"{name} call={int.from_bytes(a[:4], 'big')} from={number(a[4:11])}"
    if e[0] in (6, 7) and len(a) >= 4:
        return f"{name} call={int.from_bytes(a[:4], 'big')}"
    if e[0] == 8 and len(a) >= 5:
        c = a[4]
        return f"{name} call={int.from_bytes(a[:4], 'big')} cause={c} ({CAUSES[c] if c < len(CAUSES) else '?'})"
    return f"{name} {a.hex()}"


def decode_status(s: bytes) -> str:
    st = TERM_STATES[s[0]] if s[0] < len(TERM_STATES) else s[0]
    sig = SIG_STATES[s[3]] if s[3] < len(SIG_STATES) else s[3]
    rssi = int.from_bytes(s[4:6], "little", signed=True)
    snr = int.from_bytes(s[6:8], "little", signed=True) / 4
    tmid = int.from_bytes(s[8:12], "little")
    band = "2g4" if s[1] == 1 else "915"
    return f"link={st} band={band} tier={s[2]} sig={sig} rssi={rssi} snr={snr:.1f} tmid={tmid:08x}"


def att_error(e: Exception) -> int | None:
    m = re.search(r"ATT error: 0x([0-9a-fA-F]{2})", str(e)) or re.search(r"0x([0-9a-fA-F]{2})", str(e))
    if m:
        return int(m.group(1), 16)
    # BlueZ backend (this bleak version) raises BleakGATTProtocolError with a
    # repr like "<BleakGATTProtocolErrorCode.INVALID_ATTRIBUTE_VALUE_LENGTH: 13>"
    # instead of a hex ATT error text; the trailing int is the ATT error code.
    m = re.search(r": (\d+)>", str(e))
    return int(m.group(1)) if m else None


class Terminal:
    def __init__(self, client: BleakClient):
        self.c = client
        self.events: asyncio.Queue[bytes] = asyncio.Queue()
        self.down: asyncio.Queue[bytes] = asyncio.Queue()

    async def start(self):
        await self.c.start_notify(EVENT, lambda _, d: (print(f"EVENT {decode_event(bytes(d))}"),
                                                       self.events.put_nowait(bytes(d))))
        await self.c.start_notify(STATUS, lambda _, d: print(f"STATUS {decode_status(bytes(d))}"))
        await self.c.start_notify(DOWN, lambda _, d: self.down.put_nowait(bytes(d)))

    async def command(self, data: bytes):
        await self.c.write_gatt_char(COMMAND, data, response=True)

    async def step(self, step: str) -> bool:
        kind, _, arg = step.partition(":")
        if kind == "status":
            print(f"STATUS {decode_status(bytes(await self.c.read_gatt_char(STATUS)))}")
        elif kind == "activate":
            await self.command(b"\x01" + arg.encode())
        elif kind == "dial":
            await self.command(b"\x02" + arg.encode())
        elif kind in CMD:
            await self.command(bytes([CMD[kind]]))
        elif kind == "deactivate":
            await self.command(b"\x06\xa5")
        elif kind == "sleep":
            await asyncio.sleep(float(arg))
        elif kind == "wait":
            name, _, t = arg.partition(":")
            try:
                async with asyncio.timeout(float(t or 30)):
                    while True:
                        e = await self.events.get()
                        if EVENTS.get(e[0]) == name:
                            return True
            except TimeoutError:
                print(f"FAIL: no {name} event")
                return False
        elif kind == "ping":
            n, ok = int(arg or 5), 0
            for i in range(n):
                frame = bytes([0xA0, i]) + b"oc-ping"
                await self.c.write_gatt_char(UP, frame, response=True)
                try:
                    async with asyncio.timeout(3):
                        while (await self.down.get()) != frame:
                            pass
                    ok += 1
                except TimeoutError:
                    pass
            print(f"ping: {ok}/{n} echoed")
            return ok == n
        elif kind == "err":
            code, _, inner = arg.partition(":")
            want = int(code, 16)
            try:
                await self.step(inner)
            except BleakError as e:
                got = att_error(e)
                print(f"{inner}: ATT error 0x{got:02x}" if got is not None else f"{inner}: {e}")
                return got == want
            print(f"FAIL: {inner} was accepted, expected ATT error 0x{want:02x}")
            return False
        else:
            raise SystemExit(f"unknown step: {step}")
        return True


async def scan(name: str | None) -> int:
    seen: dict[str, list[int]] = {}

    def cb(d, ad):
        n = ad.local_name or d.name or ""
        if (n == name) if name else n.startswith("OpenCell-"):
            seen.setdefault(f"{n} ({d.address})", []).append(ad.rssi)

    async with BleakScanner(cb):
        await asyncio.sleep(10)
    for k, r in seen.items():
        print(f"{k}: {len(r)} adverts in 10 s, RSSI {min(r)}..{max(r)} dBm")
    if not seen:
        print("no OpenCell terminal advertising (is the phone app connected to it?)")
    return 0 if seen else 1


async def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", help="advertised name (default: the first OpenCell-* found)")
    ap.add_argument("steps", nargs="+")
    a = ap.parse_args()
    if a.steps == ["scan"]:
        return await scan(a.name)
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: (ad.local_name or d.name or "") == a.name if a.name
        else (ad.local_name or d.name or "").startswith("OpenCell-"), timeout=15)
    if dev is None:
        print("no OpenCell terminal found", file=sys.stderr)
        return 1
    async with BleakClient(dev) as client:
        print(f"connected to {dev.name} ({dev.address}), MTU {client.mtu_size}")
        t = Terminal(client)
        await t.start()
        for s in a.steps:
            print(f"-- {s}")
            try:
                if not await t.step(s):
                    return 1
            except BleakError as e:
                code = att_error(e)
                print(f"FAIL: {s}: " + (f"ATT error 0x{code:02x}" if code is not None else str(e)))
                return 1
        await asyncio.sleep(0.5)  # late notifications
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
