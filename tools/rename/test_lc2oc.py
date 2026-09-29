"""Tests for lc2oc.py: python3 -m unittest discover -s tools/rename -p 'test_*.py'"""
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lc2oc  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


class Words(unittest.TestCase):
    def test_renamed(self):
        for old, new in [
            ("lc_sig_net_t *n", "oc_sig_net_t *n"),
            ("#ifndef LC_SIG_H", "#ifndef OC_SIG_H"),
            ("lcbench net \"$A\"", "ocbench net \"$A\""),
            ("lcbench_core lcbench.c test_lcbench LCBENCH_CORE_H", "ocbench_core ocbench.c test_ocbench OCBENCH_CORE_H"),
            ("lcb_cell_t LCB_LEAD_FRAMES", "ocb_cell_t OCB_LEAD_FRAMES"),
            ("test_lcb_net_mo_call test_grant_leg_channel_matches_lc_hop", "test_ocb_net_mo_call test_grant_leg_channel_matches_oc_hop"),
            ("idf.py -DLC_BENCH_LOW_POWER=1 build", "idf.py -DOC_BENCH_LOW_POWER=1 build"),
            ("firmware/components/lc_sig/lc_sig_net.c", "firmware/components/oc_sig/oc_sig_net.c"),
            ("static const char *TAG = \"lc_term\";", "static const char *TAG = \"oc_term\";"),
            ("oc-core admin --offline import-lcb-hss FILE", "oc-core admin --offline import-ocb-hss FILE"),
        ]:
            self.assertEqual(new, lc2oc.rewrite_line(old))

    def test_left_alone(self):
        for s in [
            "esp_lcd_panel_handle_t calc_x calculateRxTimeout",
            "memcmp(buf, \"LCB1\", 4)",  # the bench payload's magic, on the air
            "6c630001-7e2a-4b8e-9f2d-3c1a5e7b0d10",  # BLE UUIDs
            "branch lc-core, file 2026-09-26-lc-sig-c-side.md",
            "NVS `lc/term = 1`, an LC filter",
            "#define NVS_NS  \"lc_id\"",
            "#define GATT_NVS_NS    \"lc_ble\" /* not lc, lc_id, lc_scan */",
            "old = lc_sig_x; /* lc2oc: keep */",
        ]:
            self.assertEqual(s, lc2oc.rewrite_line(s))

    def test_pre_rename_runs_first(self):
        pre = lc2oc.pre_rules([("ocb", "ocr")])
        self.assertEqual("ocr_t OCR_H `ocr` test_ocr.c ocb_cell", lc2oc.rewrite_line("ocb_t OCB_H `ocb` test_ocb.c lcb_cell", pre))


class Repo(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = self.tmp.name
        self.git("init", "-q")
        self.write("components/lc_sig/lc_sig.c", "#include \"lc_sig.h\"\nint lc_sig_x;\n")
        self.write("components/lc_sig/include/lc_sig.h", "#define LC_SIG_H\n")
        self.write("main/nvs.c", "#define NVS_NS  \"lc_id\"\nstatic const char *TAG = \"lc_ident\";\n")
        self.write("main/mig.c", "/* lc2oc: keep-file */\nconst char *old = \"lc_id\"; int lc_y;\n")
        self.write("docs/old-plan.md", "lc_sig history\n")
        self.git("add", "-A")

    def tearDown(self):
        self.tmp.cleanup()

    def git(self, *a):
        subprocess.run(["git", "-C", self.repo] + list(a), check=True, capture_output=True)

    def write(self, path, text):
        full = os.path.join(self.repo, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w") as f:
            f.write(text)

    def read(self, path):
        with open(os.path.join(self.repo, path)) as f:
            return f.read()

    def run_tool(self, *a):
        return subprocess.run([sys.executable, os.path.join(HERE, "lc2oc.py"), "--repo", self.repo] + list(a),
                              capture_output=True, text=True)

    def test_check_apply_check(self):
        r = self.run_tool("--skip", "docs/*", "--check")
        self.assertEqual(1, r.returncode)
        self.assertIn("legacy names left: 6", r.stdout)
        r = self.run_tool("--skip", "docs/*", "--apply")
        self.assertEqual(0, r.returncode, r.stdout)
        self.assertEqual("#include \"oc_sig.h\"\nint oc_sig_x;\n", self.read("components/oc_sig/oc_sig.c"))
        self.assertEqual("#define OC_SIG_H\n", self.read("components/oc_sig/include/oc_sig.h"))
        self.assertFalse(os.path.exists(os.path.join(self.repo, "components/lc_sig")))
        self.assertEqual("#define NVS_NS  \"lc_id\"\nstatic const char *TAG = \"oc_ident\";\n", self.read("main/nvs.c"))
        self.assertIn("int lc_y;", self.read("main/mig.c"))
        self.assertEqual("lc_sig history\n", self.read("docs/old-plan.md"))
        self.assertEqual(0, self.run_tool("--skip", "docs/*", "--check").returncode)

    def test_a_collision_is_refused(self):
        self.write("main/other.c", "int oc_sig_x;\n")
        self.git("add", "-A")
        r = self.run_tool("--skip", "docs/*", "--apply")
        self.assertEqual(1, r.returncode)
        self.assertIn("COLLISION: oc_sig_x", r.stdout)
        self.assertIn("int lc_sig_x;", self.read("components/lc_sig/lc_sig.c"))
        self.assertEqual(0, self.run_tool("--skip", "docs/*", "--allow", "oc_sig_x", "--apply").returncode)


if __name__ == "__main__":
    unittest.main()
