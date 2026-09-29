#!/usr/bin/env python3
"""Drive an OpenCell terminal over BLE (contract v4, oc_term_gatt.h) from the laptop.

Runs its steps in order over one connection and prints every EVENT and
STATUS notification, decoded. Exits 1 if a step fails (a wait times out, a
command is refused unexpectedly, or a ping frame doesn't come back).

    ~/.venvs/opencell/bin/python tools/ble/oc_ble.py [--name OpenCell-76AD0488] STEP...

    ~/.venvs/opencell/bin/python tools/ble/oc_ble.py scan    # adverts and RSSI for 10 s, no connection

Pairing (spec 2026-09-27-ble-pairing-design.md): the terminal refuses every
characteristic (ATT 0x05) until the laptop has paired with the 6-digit code on
its OLED's Pairing screen. The script registers a BlueZ agent that answers
the passkey request from --passkey N (read off the OLED) or, with
--passkey-from-console PORT, from a bench build's console ("pair code NNNNNN");
PORT may also be the file tools/ble/oc_console.py is logging the port to.
Without either it refuses to pair. BlueZ keeps the bond between runs, so pair
once, then run steps without a code; `unpair` removes the laptop's bond. Each
refused or wrong attempt rolls the terminal's code; 3 within 60 s lock
pairing for 60 s.

    ~/.venvs/opencell/bin/python tools/ble/oc_ble.py --passkey-from-console \
        /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_44:B1:76:AD:04:88-if00 pair status
    ~/.venvs/opencell/bin/python tools/ble/oc_ble.py unpair  # forget the terminal (no connection)

A laptop more than a few metres from the board hears it at about -95 dBm and
the connection dies at once (supervision timeout, reason 0x08). Keep the
RSSI above -80 dBm. The phone app must not be connected: the terminal takes
one connection (adb shell am force-stop org.opencell.app).

Steps:
    pair                   pair (if not yet) and turn notifications on; first step
    unpair                 remove the laptop's bond (disconnects: last step)
    status                 read STATUS
    activate:TEXT          COMMAND ACTIVATE (TEXT from ocbench mkqr)
    dial:NUMBER             COMMAND DIAL, any dialled form (dial:606-555-1235, dial:+883160655501235)
    answer | reject | hangup
    deactivate              COMMAND DEACTIVATE (with the 0xA5 confirmation)
    wait:EVENT[:S]          wait up to S s (default 30) for an EVENT: activated, act_failed,
                            registered, reg_failed, incoming, ringing, connected, ended, deactivated
    ping[:N]                send N (default 5) app data frames on UP, expect each back on DOWN
    send[:N]                send N (default 5) app data frames on UP, 200 ms apart (a call between two terminals)
    recv[:N[:S]]            wait up to S s (default 20) for N (default 5) frames from send on DOWN
    sleep:S
    err:0xNN:STEP           run STEP, expecting ATT error 0xNN (e.g. err:0x80:dial:+883160655500100)
    scanlist                read SCAN: the terminal's assembled scan list (channel-list spec §9)
    scan-set:MHZ[:fixed],...  COMMAND SCAN SET_USER, at most 4 grid channels (scan-set: clears them)
    scan-fallback:AFTER:CHUNK COMMAND SCAN SET_FALLBACK (after 0-15, 15 never; chunk 1-52)
    scan-forget             COMMAND SCAN FORGET_LEARNED

Notifications (EVENT, STATUS, DOWN) are turned on at connect only if BlueZ
already holds a bond; otherwise start with `pair`.

Every GATT operation is bounded by --op-timeout (default 15 s): unpaired, BlueZ
answers the terminal's 0x05 by pairing on its own, and when the agent refuses
(no code) the operation never completes. A timeout fails the step and ends the
run (the link is dropped), except in err:0x05:STEP on an unpaired link after
the agent refused: that is the terminal's 0x05, and the step passes.

Needs bleak (pip install bleak), which brings dbus-fast.
"""
import argparse
import asyncio
import decimal
import os
import re
import select
import sys
import termios
import threading
import time
import tty
from collections.abc import Awaitable, Callable

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError
from dbus_fast import BusType, DBusError, Message, MessageType
from dbus_fast.aio import MessageBus
from dbus_fast.service import ServiceInterface, dbus_method


def uuid(n: int) -> str:
    return f"6c63000{n}-7e2a-4b8e-9f2d-3c1a5e7b0d10"


UP, DOWN, STATUS, COMMAND, EVENT, SCAN = uuid(2), uuid(3), uuid(4), uuid(5), uuid(6), uuid(7)

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


NUM = 8  # contract v3: numbers are 8 BCD bytes (numbering v2); v2 firmware sent 7
# EVENT lengths without the code byte: v3, and the v2 lengths that mean old firmware
V3_LEN = {1: NUM, 3: NUM + 1, 5: 4 + NUM}
V2_LEN = {1: 7, 3: 8, 5: 11}


def event_is_old_firmware(e: bytes) -> bool:
    """True if e is a v2-firmware-length EVENT (7-byte numbers): old firmware, needs updating."""
    return e[0] in V2_LEN and len(e) - 1 == V2_LEN[e[0]]


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
    if event_is_old_firmware(e):
        return f"{name} OLD FIRMWARE (numbering v1, 7-byte numbers): update it {a.hex()}"
    if e[0] == 1 and len(a) == V3_LEN[1]:
        return f"{name} number={number(a)}"
    if e[0] == 2 and a:
        return f"{name} reason={a[0]} ({ACT_REASONS.get(a[0], '?')})"
    if e[0] == 3 and len(a) == V3_LEN[3]:
        return f"{name} number={number(a[:NUM])} mode={'part15' if a[NUM] == 1 else 'part97'}"
    if e[0] == 4 and a:
        return f"{name} reason={a[0]} ({REG_REASONS.get(a[0], '?')})"
    if e[0] == 5 and len(a) == V3_LEN[5]:
        return f"{name} call={int.from_bytes(a[:4], 'big')} from={number(a[4:])}"
    if e[0] in (6, 7) and len(a) >= 4:
        return f"{name} call={int.from_bytes(a[:4], 'big')}"
    if e[0] == 8 and len(a) >= 5:
        c = a[4]
        return f"{name} call={int.from_bytes(a[:4], 'big')} cause={c} ({CAUSES[c] if c < len(CAUSES) else '?'})"
    return f"{name} {a.hex()}"


SCAN_SOURCES = {1: "last", 2: "user", 3: "network", 4: "learned", 5: "default", 6: "sweep"}


def mhz(khz: int) -> str:
    return f"{khz // 1000}.{khz % 1000 // 10:02d}"


def decode_status(s: bytes) -> str:
    st = TERM_STATES[s[0]] if s[0] < len(TERM_STATES) else s[0]
    sig = SIG_STATES[s[3]] if s[3] < len(SIG_STATES) else s[3]
    rssi = int.from_bytes(s[4:6], "little", signed=True)
    snr = int.from_bytes(s[6:8], "little", signed=True) / 4
    tmid = int.from_bytes(s[8:12], "little")
    band = "2g4" if s[1] == 1 else "915"
    out = f"link={st} band={band} tier={s[2]} sig={sig} rssi={rssi} snr={snr:.1f} tmid={tmid:08x}"
    if len(s) >= 27:  # contract v4: the scan (channel-list spec §9)
        khz = int.from_bytes(s[23:27], "little")
        if s[20]:
            out += f" scan={s[20]}/{s[21]} {SCAN_SOURCES.get(s[22], s[22])} {mhz(khz)}"
        elif khz:
            out += f" ch={mhz(khz)}"
    return out


def decode_scan(b: bytes) -> str:
    """The SCAN characteristic (fmt 1) as lines of text."""
    if len(b) < 6 or b[0] != 1:
        return f"SCAN format {b[:1].hex()} not understood: {b.hex()}"
    mode = {1: "part15", 2: "part97"}.get(b[1], str(b[1]))
    after = "never" if b[2] == 15 else f"after {b[2]}"
    lines = [f"SCAN mode={mode} fallback={after}/{b[3]} net_ver={b[4]} entries={b[5]}"]
    for i in range(b[5]):
        e = b[6 + 5 * i: 11 + 5 * i]
        if len(e) < 5:
            lines.append("  (truncated)")
            break
        hz, fl = int.from_bytes(e[:4], "little"), e[4]
        src = SCAN_SOURCES.get((fl >> 1) & 7, "?")
        lines.append(f"  {i + 1:2d} {mhz(hz // 1000)} {src}{' fixed' if fl & 1 else ''}"
                     f"{'' if fl & 0x10 else ' INACTIVE'}")
    return "\n".join(lines)


def scan_set_user(text: str) -> bytes:
    """COMMAND SCAN SET_USER from "917.25,922.25:fixed" (MHz; "" clears); the terminal checks the grid."""
    entries = []
    for item in filter(None, text.split(",")):
        f, _, flag = item.partition(":")
        if flag not in ("", "fixed"):
            raise SystemExit(f"scan-set: '{item}': only ':fixed' may follow a frequency")
        try:
            hz = int(decimal.Decimal(f) * 1_000_000)
        except decimal.InvalidOperation:
            raise SystemExit(f"scan-set: '{f}' is not a frequency in MHz") from None
        entries.append(hz.to_bytes(4, "little") + bytes([1 if flag else 0]))
    if len(entries) > 4:
        raise SystemExit("scan-set: at most 4 entries")
    return bytes([0x07, 0x01, len(entries)]) + b"".join(entries)


def att_error(e: Exception) -> int | None:
    m = re.search(r"ATT error: 0x([0-9a-fA-F]{2})", str(e)) or re.search(r"0x([0-9a-fA-F]{2})", str(e))
    if m:
        return int(m.group(1), 16)
    # BlueZ backend (this bleak version) raises BleakGATTProtocolError with a
    # repr like "<BleakGATTProtocolErrorCode.INVALID_ATTRIBUTE_VALUE_LENGTH: 13>"
    # instead of a hex ATT error text; the trailing int is the ATT error code.
    m = re.search(r": (\d+)>", str(e))
    return int(m.group(1)) if m else None


PAIR_CODE = re.compile(rb"pair code (\d{6})")


def parse_pair_code(line: bytes) -> int | None:
    """The code in a bench build's console line ("... oc_ble: boot; pair code 004271")."""
    m = PAIR_CODE.search(line)
    return int(m.group(1)) if m else None


class ConsoleCodes:
    """Follows the terminal's console in a thread and keeps the last pair code.

    `source` is the terminal's serial port, or a file that a console logger
    (tools/ble/oc_console.py) is writing: one process per serial port, so
    while a logger holds the port, follow its file. A port is opened like
    ocbench does (raw, HUPCL off): dropping DTR/RTS on close would reset an
    ESP32-S3 on its USB port. A file is read from the start (its last code
    counts), then followed.

    If the port goes away (EOF, or EIO when the board is unplugged or
    re-enumerates), `dead` says why and wait_code() answers None from then on:
    a code seen before may be stale by now.
    """

    def __init__(self, source: str, log_path: str | None = None):
        self.source = source
        self.latest: int | None = None
        self.dead: str | None = None  # why the source stopped, once it has
        self._log = open(log_path, "ab") if log_path else None
        self._buf = b""
        self._stop = threading.Event()
        self._fd: int | None = None
        self._is_file = False
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        self._is_file = os.path.isfile(self.source)
        if self._is_file:
            self._fd = os.open(self.source, os.O_RDONLY)
        else:
            self._fd = open_console(self.source, os.O_RDONLY)
        self._thread = threading.Thread(target=self._run, name="console", daemon=True)
        self._thread.start()

    def _run(self) -> None:
        assert self._fd is not None
        try:
            while not self._stop.is_set():
                if not self._is_file and not select.select([self._fd], [], [], 0.2)[0]:
                    continue
                data = os.read(self._fd, 4096)
                if data:
                    self.feed(data)
                elif self._is_file:
                    time.sleep(0.1)  # at the end of the file: wait for the logger to append
                else:
                    self.dead = f"{self.source}: end of file (the port went away)"
                    return
        except OSError as e:  # EIO: the port vanished under us
            if not self._stop.is_set():
                self.dead = f"{self.source}: {e.strerror or e}"

    def feed(self, data: bytes) -> None:
        if self._log:
            self._log.write(data)
            self._log.flush()
        self._buf += data
        *lines, self._buf = self._buf.split(b"\n")
        for line in lines:
            code = parse_pair_code(line)
            if code is not None:
                self.latest = code

    async def wait_code(self, timeout: float) -> int | None:
        """The last code seen, waiting up to timeout s for a first one. None once the source is dead."""
        deadline = time.monotonic() + timeout
        while self.latest is None and self.dead is None and time.monotonic() < deadline:
            await asyncio.sleep(0.05)
        if self.dead is not None:
            print(f"console: {self.dead}; not answering with a code that may be stale")
            return None
        return self.latest

    def close(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=1)
        if self._fd is not None:
            os.close(self._fd)
        if self._log:
            self._log.close()


def open_console(port: str, flags: int) -> int:
    """Opens an ESP32-S3's USB serial port raw, with HUPCL off so closing it doesn't reset the board."""
    fd = os.open(port, flags | os.O_NOCTTY)
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[2] = (attrs[2] | termios.CLOCAL | termios.CREAD) & ~termios.HUPCL
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


AGENT_PATH = "/org/opencell/oc_ble/agent"
REJECTED = "org.bluez.Error.Rejected"


class PasskeyAgent(ServiceInterface):
    """A BlueZ pairing agent (org.bluez.Agent1, capability KeyboardOnly).

    The terminal is DisplayOnly, so BlueZ asks for the passkey the terminal
    shows (RequestPasskey); `source` supplies it, or None to refuse. Every
    other request is refused: nothing else should be pairing while this runs.
    """

    def __init__(self, source: Callable[[], Awaitable[int | None]]):
        super().__init__("org.bluez.Agent1")
        self._source = source
        self.answered: list[int] = []
        self.refused = 0  # passkey requests refused (no code): BlueZ pairing on its own

    async def passkey(self, device: str) -> int:
        code = await self._source()
        if code is None:
            self.refused += 1
            print(f"agent: no pair code for {device}; refusing (use --passkey or --passkey-from-console)")
            raise DBusError(REJECTED, "no pair code")
        print(f"agent: passkey {code:06d} for {device}")
        self.answered.append(code)
        return code

    @dbus_method()
    async def RequestPasskey(self, device: "o") -> "u":  # noqa: F821 (D-Bus signatures)
        return await self.passkey(device)

    @dbus_method()
    def RequestPinCode(self, device: "o") -> "s":  # noqa: F821
        raise DBusError(REJECTED, "PIN codes are not used")

    @dbus_method()
    def RequestConfirmation(self, device: "o", passkey: "u"):  # noqa: F821
        raise DBusError(REJECTED, "numeric comparison is not used")

    @dbus_method()
    def RequestAuthorization(self, device: "o"):  # noqa: F821
        raise DBusError(REJECTED, "not expected")

    @dbus_method()
    def DisplayPasskey(self, device: "o", passkey: "u", entered: "q"):  # noqa: F821
        pass

    @dbus_method()
    def DisplayPinCode(self, device: "o", pincode: "s"):  # noqa: F821
        pass

    @dbus_method()
    def AuthorizeService(self, device: "o", uuid: "s"):  # noqa: F821
        pass

    @dbus_method()
    def Cancel(self):
        print("agent: pairing cancelled")

    @dbus_method()
    def Release(self):
        pass


async def bluez_call(bus: MessageBus, member: str, signature: str = "", body: list | None = None) -> None:
    reply = await bus.call(Message(destination="org.bluez", path="/org/bluez", interface="org.bluez.AgentManager1",
                                   member=member, signature=signature, body=body or []))
    if reply.message_type == MessageType.ERROR:
        raise RuntimeError(f"{member}: {reply.error_name} {reply.body}")


async def register_agent(agent: PasskeyAgent) -> MessageBus:
    """Exports the agent on the system bus and makes it BlueZ's default until the bus closes."""
    bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
    bus.export(AGENT_PATH, agent)
    await bluez_call(bus, "RegisterAgent", "os", [AGENT_PATH, "KeyboardOnly"])
    await bluez_call(bus, "RequestDefaultAgent", "o", [AGENT_PATH])
    return bus


async def unregister_agent(bus: MessageBus, timeout: float = 5.0) -> None:
    """Unregisters the agent, then closes the bus. Closing the bus makes BlueZ drop the agent too,
    so a bluetoothd that doesn't answer (hung, or restarting after a crash) can't keep it."""
    try:
        await asyncio.wait_for(bluez_call(bus, "UnregisterAgent", "o", [AGENT_PATH]), timeout)
    except Exception as e:  # noqa: BLE001 (cleanup: report, never raise)
        print(f"agent: UnregisterAgent failed ({e!r}); dropping the D-Bus connection")
    finally:
        bus.disconnect()


class OpTimeout(Exception):
    """A GATT operation didn't complete within --op-timeout: the link is stalled."""


def inferred_auth_error(want: int, paired: bool, refused: bool) -> bool:
    """Whether an operation that timed out in err:0xNN:STEP counts as the expected ATT error.

    Unpaired, BlueZ answers ATT 0x05 (insufficient authentication) by pairing on its own; when
    the agent refuses (no code), the kernel keeps the ATT socket suspended and the operation
    never completes, so the error never reaches us. Only 0x05, only unpaired, only after a refusal.
    """
    return want == 0x05 and not paired and refused


class Terminal:
    def __init__(self, client: BleakClient, op_timeout: float = 15.0, paired: bool = False,
                 agent: PasskeyAgent | None = None):
        self.c = client
        self.events: asyncio.Queue[bytes] = asyncio.Queue()
        self.down: asyncio.Queue[bytes] = asyncio.Queue()
        self.subscribed = False
        self.op_timeout = op_timeout
        self.paired = paired
        self.agent = agent
        self.stalled = False  # an operation timed out: the link is unusable

    async def op(self, aw: Awaitable):
        """One GATT operation, bounded by op_timeout (raises OpTimeout)."""
        try:
            return await asyncio.wait_for(aw, self.op_timeout)
        except TimeoutError:
            self.stalled = True
            raise OpTimeout from None

    def refusals(self) -> int:
        return self.agent.refused if self.agent else 0

    async def start(self):
        self.subscribed = True
        await self.op(self.c.start_notify(EVENT, lambda _, d: (print(f"EVENT {decode_event(bytes(d))}"),
                                                               self.events.put_nowait(bytes(d)))))
        await self.op(self.c.start_notify(STATUS, lambda _, d: print(f"STATUS {decode_status(bytes(d))}")))
        await self.op(self.c.start_notify(DOWN, lambda _, d: self.down.put_nowait(bytes(d))))

    async def command(self, data: bytes):
        await self.op(self.c.write_gatt_char(COMMAND, data, response=True))

    async def step(self, step: str) -> bool:
        try:
            return await self._step(step)
        except OpTimeout:
            print(f"{step}: timed out after {self.op_timeout:g} s (BlueZ may be pairing: the agent refused)")
            return False

    async def _step(self, step: str) -> bool:
        kind, _, arg = step.partition(":")
        if kind == "pair":
            await self.op(self.c.pair())  # no-op if BlueZ already holds a bond
            self.paired = True
            print("paired")
            if not self.subscribed:
                await self.start()
        elif kind == "unpair":
            await self.op(self.c.unpair())
            print("unpaired (BlueZ bond removed; the terminal keeps its own until PRG is held 5 s)")
        elif kind == "status":
            print(f"STATUS {decode_status(bytes(await self.op(self.c.read_gatt_char(STATUS))))}")
        elif kind == "activate":
            await self.command(b"\x01" + arg.encode())
        elif kind == "dial":
            await self.command(b"\x02" + arg.encode())
        elif kind in CMD:
            await self.command(bytes([CMD[kind]]))
        elif kind == "deactivate":
            await self.command(b"\x06\xa5")
        elif kind == "scanlist":
            print(decode_scan(bytes(await self.op(self.c.read_gatt_char(SCAN)))))
        elif kind == "scan-set":
            await self.command(scan_set_user(arg))
        elif kind == "scan-fallback":
            after, _, chunk = arg.partition(":")
            await self.command(bytes([0x07, 0x02, int(after), int(chunk)]))
        elif kind == "scan-forget":
            await self.command(b"\x07\x03")
        elif kind == "sleep":
            await asyncio.sleep(float(arg))
        elif kind == "wait":
            name, _, t = arg.partition(":")
            try:
                async with asyncio.timeout(float(t or 30)):
                    while True:
                        e = await self.events.get()
                        if EVENTS.get(e[0]) == name:
                            if event_is_old_firmware(e):
                                print(f"FAIL: {name} event from old firmware (numbering v1): "
                                      f"{decode_event(e)}")
                                return False
                            return True
            except TimeoutError:
                print(f"FAIL: no {name} event")
                return False
        elif kind == "ping":
            n, ok = int(arg or 5), 0
            for i in range(n):
                frame = bytes([0xA0, i]) + b"oc-ping"
                await self.op(self.c.write_gatt_char(UP, frame, response=True))
                try:
                    async with asyncio.timeout(3):
                        while (await self.down.get()) != frame:
                            pass
                    ok += 1
                except TimeoutError:
                    pass
            print(f"ping: {ok}/{n} echoed")
            return ok == n
        elif kind == "send":
            for i in range(int(arg or 5)):
                await self.op(self.c.write_gatt_char(UP, bytes([0xB0, i]) + b"oc-send", response=True))
                await asyncio.sleep(0.2)
            print(f"send: {int(arg or 5)} frames")
        elif kind == "recv":
            n_s, _, t = arg.partition(":")
            n, got = int(n_s or 5), 0
            try:
                async with asyncio.timeout(float(t or 20)):
                    while got < n:
                        f = await self.down.get()
                        if f[:1] == b"\xb0" and f[2:] == b"oc-send":
                            got += 1
            except TimeoutError:
                pass
            print(f"recv: {got}/{n} frames")
            return got == n
        elif kind == "err":
            code, _, inner = arg.partition(":")
            want = int(code, 16)
            before = self.refusals()
            try:
                await self._step(inner)
            except BleakError as e:
                got = att_error(e)
                print(f"{inner}: ATT error 0x{got:02x}" if got is not None else f"{inner}: {e}")
                return got == want
            except OpTimeout:
                if inferred_auth_error(want, self.paired, self.refusals() > before):
                    print(f"{inner}: ATT error 0x05 (inferred: BlueZ auto-pair refused, op timed out)")
                    return True
                raise
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


def subscribe_at_connect(paired: bool, steps: list[str]) -> bool:
    """Turn notifications on before the steps only over an existing bond.

    Unpaired, each CCCD write would be refused (0x05) and BlueZ would retry it
    by pairing: three refused pairings (one per characteristic) and the
    terminal locks pairing for 60 s. The pair step subscribes after pairing.
    """
    return paired and steps[0] != "pair"


def is_terminal(name: str | None):
    def match(d, ad) -> bool:
        n = ad.local_name or d.name or ""
        return n == name if name else n.startswith("OpenCell-")
    return match


async def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", help="advertised name (default: the first OpenCell-* found)")
    ap.add_argument("--passkey", type=int, help="answer the pairing request with this code (the OLED's Pairing screen)")
    ap.add_argument("--passkey-from-console", metavar="PORT",
                    help="answer with the last 'pair code' a bench build logged on this serial port "
                         "(or in this file, while tools/ble/oc_console.py logs the port)")
    ap.add_argument("--console-log", metavar="FILE", help="with --passkey-from-console: append the console to FILE")
    ap.add_argument("--op-timeout", type=float, default=15.0, metavar="S",
                    help="fail a GATT operation (read, write, notify, pair) after S s (default 15)")
    ap.add_argument("steps", nargs="+")
    a = ap.parse_args()
    if a.steps == ["scan"]:
        return await scan(a.name)

    console = ConsoleCodes(a.passkey_from_console, a.console_log) if a.passkey_from_console else None
    if console:
        console.start()

    async def source() -> int | None:
        if a.passkey is not None:
            return a.passkey
        return await console.wait_code(5) if console else None

    try:
        return await session(a, PasskeyAgent(source))
    finally:
        if console:
            console.close()


async def find_terminal(name: str | None):
    return await BleakScanner.find_device_by_filter(is_terminal(name), timeout=15)


async def session(a: argparse.Namespace, agent: PasskeyAgent, register=register_agent, find=find_terminal,
                  make_client=BleakClient) -> int:
    """Registers the agent, runs the steps, and always unregisters it: every Bluetooth operation in
    run() is bounded, so the finally is reached even when bluetoothd stops answering."""
    bus = await register(agent)
    try:
        return await run(a, agent, find, make_client)
    finally:
        await unregister_agent(bus)


def why(e: BaseException) -> str:
    return "timed out" if isinstance(e, TimeoutError) else repr(e)


async def bounded_disconnect(client, timeout: float) -> None:
    """Disconnects once, giving up after timeout s (bleak's own disconnect has no D-Bus timeout)."""
    try:
        await asyncio.wait_for(client.disconnect(), timeout)
    except Exception as e:  # noqa: BLE001 (cleanup in a finally: e.g. a D-Bus error while bluetoothd restarts)
        print(f"disconnect: {why(e)}; giving up")


async def run(a: argparse.Namespace, agent: PasskeyAgent, find=find_terminal, make_client=BleakClient) -> int:
    dev = await find(a.name)
    if dev is None:
        print("no OpenCell terminal found", file=sys.stderr)
        return 1
    if a.steps == ["unpair"]:  # no connection needed
        try:
            await asyncio.wait_for(make_client(dev).unpair(), a.op_timeout)
            print(f"unpaired {dev.name} ({dev.address})")
        except (TimeoutError, BleakError) as e:
            print(f"unpair: {e!r}")
        return 0
    paired = bool(dev.details.get("props", {}).get("Paired")) if isinstance(dev.details, dict) else False
    # No `async with`: its exit would disconnect again, without a timeout.
    client = make_client(dev)
    try:
        try:
            await asyncio.wait_for(client.connect(), a.op_timeout)
        except (TimeoutError, BleakError) as e:
            print(f"FAIL: connect: {why(e)}")
            return 1
        return await run_steps(a, Terminal(client, a.op_timeout, paired, agent), dev, paired)
    finally:
        await bounded_disconnect(client, a.op_timeout)  # exactly once, on every path


async def run_steps(a: argparse.Namespace, t: Terminal, dev, paired: bool) -> int:
    print(f"connected to {dev.name} ({dev.address}), MTU {t.c.mtu_size}, {'bonded' if paired else 'not paired'}")
    if subscribe_at_connect(paired, a.steps):
        hint = "; if the terminal's bonds were cleared, run unpair, then pair"
        try:
            await t.start()
        except BleakError as e:
            code = att_error(e)
            print("FAIL: notifications refused" + (f" (ATT error 0x{code:02x})" if code is not None else f": {e}") +
                  hint)
            return 1
        except OpTimeout:
            print(f"FAIL: notifications timed out after {a.op_timeout:g} s" + hint)
            return 1
    elif a.steps[0] != "pair":
        print("notifications off until paired (start with the pair step)")
    for i, s in enumerate(a.steps):
        print(f"-- {s}")
        try:
            ok = await t.step(s)
        except BleakError as e:
            code = att_error(e)
            print(f"FAIL: {s}: " + (f"ATT error 0x{code:02x}" if code is not None else str(e)))
            return 1
        if t.stalled:  # an operation timed out: run nothing more over the link (the caller drops it)
            if ok and i + 1 < len(a.steps):
                print("FAIL: the link stalled; the remaining steps were not run")
                return 1
            return 0 if ok else 1
        if not ok:
            return 1
        if s == "unpair":
            return 0  # the link is gone
    await asyncio.sleep(0.5)  # late notifications
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
