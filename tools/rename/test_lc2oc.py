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


class Helpers:
    """A scratch git repo with the same git/write/read/run_tool helpers as Repo,
    for the new test classes below (review fixes)."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = self.tmp.name
        self.git("init", "-q")

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


class PathCollisions(Helpers, unittest.TestCase):
    """A path collision must be reported, and --apply refused, before any
    file is written or moved (review finding 1)."""

    def test_file_next_to_file(self):
        self.write("a/lc_x.c", "int x;\n")
        self.write("a/oc_x.c", "int y;\n")
        self.git("add", "-A")
        r = self.run_tool()
        self.assertEqual(1, r.returncode)
        self.assertIn("COLLISION: a/oc_x.c already exists", r.stdout)
        r = self.run_tool("--apply")
        self.assertEqual(1, r.returncode)
        self.assertTrue(os.path.exists(os.path.join(self.repo, "a/lc_x.c")))
        self.assertEqual("int y;\n", self.read("a/oc_x.c"))

    def test_directory_merge(self):
        self.write("lc_d/x.c", "int x;\n")
        self.write("oc_d/y.c", "int y;\n")
        self.git("add", "-A")
        r = self.run_tool()
        self.assertEqual(1, r.returncode)
        self.assertIn("COLLISION: oc_d already exists", r.stdout)
        r = self.run_tool("--apply")
        self.assertEqual(1, r.returncode)
        self.assertTrue(os.path.exists(os.path.join(self.repo, "lc_d/x.c")))

    def test_file_onto_existing_directory(self):
        self.write("lc_f", "int x;\n")
        self.write("oc_f/keep.txt", "keep\n")
        self.git("add", "-A")
        r = self.run_tool()
        self.assertEqual(1, r.returncode)
        self.assertIn("COLLISION: oc_f already exists", r.stdout)
        r = self.run_tool("--apply")
        self.assertEqual(1, r.returncode)
        self.assertTrue(os.path.exists(os.path.join(self.repo, "lc_f")))


class KeepLines(Helpers, unittest.TestCase):
    """Tokens on a KEEP_LINE line must not enter the renamed/collision
    universe: applying twice on an already-renamed tree must be a no-op
    (review finding 2)."""

    def test_double_apply_is_a_no_op(self):
        self.write("main/gatt.c",
                    "#define GATT_NVS_NS    \"lc_ble\" /* not lc, lc_id, lc_scan */\n"
                    "int lc_sig_x;\n")
        self.write("main/other.c", "int oc_ble;\n")
        self.git("add", "-A")
        r = self.run_tool("--apply")
        self.assertEqual(0, r.returncode, r.stdout)
        self.assertIn("names changed: 1", r.stdout)
        self.assertNotIn("COLLISION", r.stdout)
        self.assertEqual(
            "#define GATT_NVS_NS    \"lc_ble\" /* not lc, lc_id, lc_scan */\n"
            "int oc_sig_x;\n",
            self.read("main/gatt.c"))
        self.assertEqual("int oc_ble;\n", self.read("main/other.c"))
        r2 = self.run_tool("--apply")
        self.assertEqual(0, r2.returncode, r2.stdout)
        self.assertIn("names changed: 0", r2.stdout)
        self.assertNotIn("COLLISION", r2.stdout)


class CheckOutput(Helpers, unittest.TestCase):
    """--check caps its printed offending lines (paths and content lines
    share one cap), and still shows some content lines when paths alone
    would otherwise dominate the budget (review finding 3)."""

    def test_check_caps_output(self):
        for i in range(25):
            self.write("z/lc_%02d.c" % i, "x = 1;\n")
        self.write("keep.c", "int lc_z;\n")
        self.git("add", "-A")
        r = self.run_tool("--check")
        self.assertEqual(1, r.returncode)
        self.assertIn("legacy names left: 26", r.stdout)
        offending = [l for l in r.stdout.splitlines() if l and not l.startswith("legacy names left")]
        self.assertLessEqual(len(offending), 20)
        self.assertTrue(any("keep.c" in l for l in offending), offending)


class Robustness(Helpers, unittest.TestCase):
    """A symlink or a tracked-but-missing file must not crash the tool
    (review finding 3)."""

    def test_symlink_and_missing_file_do_not_crash(self):
        self.write("main/lc_ok.c", "int lc_ok;\n")
        os.symlink("nonexistent-target", os.path.join(self.repo, "main/lc_link.c"))
        self.git("add", "-A")
        os.remove(os.path.join(self.repo, "main/lc_ok.c"))
        r = self.run_tool("--check")
        self.assertEqual("", r.stderr)
        r = self.run_tool()
        self.assertEqual("", r.stderr)


class Filter(Helpers, unittest.TestCase):
    """--filter: diff headers renamed, hunk line counts intact, CRLF
    preserved, a binary blob left alone, and --skip/keep-file honoured so a
    patch touching a skipped or keep-file path stays intact (review finding
    3)."""

    def run_filter(self, data, *a):
        return subprocess.run(
            [sys.executable, os.path.join(HERE, "lc2oc.py"), "--repo", self.repo, "--filter"] + list(a),
            input=data, capture_output=True)

    def test_diff_headers_renamed(self):
        patch = (
            b"diff --git a/main/lc_x.c b/main/lc_x.c\n"
            b"--- a/main/lc_x.c\n"
            b"+++ b/main/lc_x.c\n"
            b"@@ -1,2 +1,2 @@\n"
            b" int lc_sig_x;\n"
            b"-int lc_old;\n"
            b"+int lc_new;\n"
        )
        r = self.run_filter(patch)
        self.assertEqual(0, r.returncode)
        out = r.stdout
        self.assertIn(b"diff --git a/main/oc_x.c b/main/oc_x.c\n", out)
        self.assertIn(b"--- a/main/oc_x.c\n", out)
        self.assertIn(b"+++ b/main/oc_x.c\n", out)
        self.assertIn(b" int oc_sig_x;\n", out)
        self.assertIn(b"+int oc_new;\n", out)

    def test_hunk_line_counts_intact(self):
        patch = b"@@ -12,7 +12,7 @@ static void lc_sig_init(void)\n int lc_sig_x;\n"
        r = self.run_filter(patch)
        self.assertEqual(0, r.returncode)
        self.assertIn(b"@@ -12,7 +12,7 @@", r.stdout)
        self.assertEqual(patch.count(b"\n"), r.stdout.count(b"\n"))

    def test_crlf_preserved(self):
        data = b"int lc_sig_x;\r\nint keep;\r\n"
        r = self.run_filter(data)
        self.assertEqual(0, r.returncode)
        self.assertEqual(b"int oc_sig_x;\r\nint keep;\r\n", r.stdout)

    def test_binary_left_alone(self):
        data = b"\x89PNG\r\n\x1a\n\x00\x01lc_\xff\xfe"
        r = self.run_filter(data)
        self.assertEqual(0, r.returncode)
        self.assertEqual(data, r.stdout)

    def test_skip_glob_keeps_matching_hunks_intact(self):
        self.write("docs/old-plan.md", "lc_sig history\n")
        self.git("add", "-A")
        patch = (
            b"diff --git a/main/lc_x.c b/main/lc_x.c\n"
            b"--- a/main/lc_x.c\n"
            b"+++ b/main/lc_x.c\n"
            b"@@ -1 +1 @@\n"
            b"-int lc_old;\n"
            b"+int lc_new;\n"
            b"diff --git a/docs/old-plan.md b/docs/old-plan.md\n"
            b"--- a/docs/old-plan.md\n"
            b"+++ b/docs/old-plan.md\n"
            b"@@ -1 +1 @@\n"
            b"-lc_sig history\n"
            b"+lc_sig history v2\n"
        )
        r = self.run_filter(patch, "--skip", "docs/*")
        self.assertEqual(0, r.returncode)
        out = r.stdout
        self.assertIn(b"diff --git a/main/oc_x.c b/main/oc_x.c\n", out)
        self.assertIn(b"+int oc_new;\n", out)
        self.assertIn(b"diff --git a/docs/old-plan.md b/docs/old-plan.md\n", out)
        self.assertIn(b"-lc_sig history\n", out)
        self.assertIn(b"+lc_sig history v2\n", out)

    def test_keep_file_hunk_intact(self):
        self.write("main/mig.c", "/* lc2oc: keep-file */\nint lc_y;\n")
        self.git("add", "-A")
        patch = (
            b"diff --git a/main/mig.c b/main/mig.c\n"
            b"--- a/main/mig.c\n"
            b"+++ b/main/mig.c\n"
            b"@@ -1,2 +1,2 @@\n"
            b" /* lc2oc: keep-file */\n"
            b"-int lc_y;\n"
            b"+int lc_y2;\n"
        )
        r = self.run_filter(patch)
        self.assertEqual(0, r.returncode)
        self.assertEqual(patch, r.stdout)


if __name__ == "__main__":
    unittest.main()
