"""Tests for oc_ble.py's pairing helpers (no Bluetooth needed).

    ~/.venvs/opencell/bin/python -m unittest discover -s tools/ble -v
"""
import asyncio
import os
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import oc_ble  # noqa: E402
from dbus_fast import DBusError  # noqa: E402


class ParsePairCode(unittest.TestCase):
    def test_bench_console_line(self):
        self.assertEqual(4271, oc_ble.parse_pair_code(b"W (5123) lc_ble: boot; pair code 004271"))

    def test_line_with_colour_codes(self):
        line = b"\x1b[0;33mW (91234) lc_ble: phone disconnected; pair code 999999\x1b[0m\r"
        self.assertEqual(999999, oc_ble.parse_pair_code(line))

    def test_other_lines(self):
        self.assertIsNone(oc_ble.parse_pair_code(b"I (5123) lc_ble: pairing started"))
        self.assertIsNone(oc_ble.parse_pair_code(b"pair code 12345"))  # 5 digits: not a code


class SubscribeAtConnect(unittest.TestCase):
    def test_only_over_an_existing_bond(self):
        self.assertTrue(oc_ble.subscribe_at_connect(True, ["status"]))
        # unpaired: each refused CCCD write would cost the terminal one of its 3 tries a minute
        self.assertFalse(oc_ble.subscribe_at_connect(False, ["status"]))
        self.assertFalse(oc_ble.subscribe_at_connect(False, ["err:0x05:dial:+8836065550100"]))

    def test_the_pair_step_subscribes_itself(self):
        self.assertFalse(oc_ble.subscribe_at_connect(True, ["pair", "status"]))
        self.assertFalse(oc_ble.subscribe_at_connect(False, ["pair", "status"]))


class ConsoleCodesTest(unittest.TestCase):
    def test_follows_the_last_code_across_split_reads(self):
        c = oc_ble.ConsoleCodes("/dev/null")
        c.feed(b"I (1) boot\nW (2) lc_ble: boot; pair code 11")
        self.assertIsNone(c.latest)  # the line isn't complete yet
        c.feed(b"1111\r\nW (3) lc_ble: phone disconnected; pair code 222222\r\n")
        self.assertEqual(222222, c.latest)

    def test_reads_a_serial_port_and_keeps_a_log(self):
        master, slave = os.openpty()
        with tempfile.TemporaryDirectory() as d:
            log = os.path.join(d, "t.log")
            c = oc_ble.ConsoleCodes(os.ttyname(slave), log)
            c.start()
            try:
                os.write(master, b"W (7) lc_ble: pairing started; pair code 654321\r\n")
                deadline = time.monotonic() + 2
                while c.latest is None and time.monotonic() < deadline:
                    time.sleep(0.02)
                self.assertEqual(654321, c.latest)
            finally:
                c.close()
                os.close(master)
                os.close(slave)
            with open(log, "rb") as f:
                self.assertIn(b"pair code 654321", f.read())

    def test_follows_a_log_file_from_its_last_code(self):
        with tempfile.TemporaryDirectory() as d:
            log = os.path.join(d, "t.log")
            with open(log, "wb") as f:
                f.write(b"W (1) lc_ble: boot; pair code 111111\r\nW (9) lc_ble: phone disconnected; pair code 222222\r\n")
            c = oc_ble.ConsoleCodes(log)
            c.start()
            try:
                deadline = time.monotonic() + 2
                while c.latest != 222222 and time.monotonic() < deadline:
                    time.sleep(0.02)
                self.assertEqual(222222, c.latest)
                with open(log, "ab") as f:  # the logger appends
                    f.write(b"W (20) lc_ble: new code after a failed attempt; pair code 333333\r\n")
                deadline = time.monotonic() + 2
                while c.latest != 333333 and time.monotonic() < deadline:
                    time.sleep(0.02)
                self.assertEqual(333333, c.latest)
            finally:
                c.close()

    def test_wait_code_times_out_without_a_code(self):
        c = oc_ble.ConsoleCodes("/dev/null")
        t0 = time.monotonic()
        self.assertIsNone(asyncio.run(c.wait_code(0.2)))
        self.assertGreaterEqual(time.monotonic() - t0, 0.2)


class OcConsoleTest(unittest.TestCase):
    def test_logs_a_port_until_stopped(self):
        master, slave = os.openpty()
        with tempfile.TemporaryDirectory() as d:
            log = os.path.join(d, "t.log")
            here = os.path.dirname(os.path.abspath(__file__))
            p = subprocess.Popen([sys.executable, os.path.join(here, "oc_console.py"), os.ttyname(slave), log],
                                 stdout=subprocess.PIPE)
            try:
                self.assertIn(b"logging", p.stdout.readline())
                os.write(master, b"W (5) lc_ble: boot; pair code 042042\r\n")
                deadline = time.monotonic() + 2
                while time.monotonic() < deadline:
                    with open(log, "rb") as f:
                        if b"pair code 042042" in f.read():
                            break
                    time.sleep(0.05)
                with open(log, "rb") as f:
                    self.assertIn(b"pair code 042042", f.read())
            finally:
                p.terminate()
                p.wait()
                p.stdout.close()
                os.close(master)
                os.close(slave)


class PasskeyAgentTest(unittest.TestCase):
    def test_answers_with_the_source_code(self):
        async def source():
            return 4271

        agent = oc_ble.PasskeyAgent(source)
        self.assertEqual(4271, asyncio.run(agent.passkey("/org/bluez/hci0/dev_44_B1_76_AD_04_8A")))
        self.assertEqual([4271], agent.answered)

    def test_refuses_without_a_code(self):
        async def source():
            return None

        agent = oc_ble.PasskeyAgent(source)
        with self.assertRaises(DBusError) as e:
            asyncio.run(agent.passkey("/org/bluez/hci0/dev_44_B1_76_AD_04_8A"))
        self.assertEqual("org.bluez.Error.Rejected", e.exception.type)

    def test_exports_the_agent1_methods(self):
        async def source():
            return None

        names = {m.name for m in oc_ble.PasskeyAgent(source).introspect().methods}
        self.assertTrue({"RequestPasskey", "RequestConfirmation", "Cancel", "Release"} <= names)


if __name__ == "__main__":
    unittest.main()
