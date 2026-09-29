#!/usr/bin/env python3
# lc2oc: keep-file (this file names the legacy NVS namespaces on purpose)
"""Fingerprints of OpenCell's own NVS keys in a dumped NVS partition, for the
lc -> oc migration's bench check: one line per key, "namespace key type
sha256[:16]", with the old namespaces (lc, lc_id, lc_scan, lc_ble) printed
under their new names so a dump from before and one from after can be
diffed. Values are never printed (lc_id/oc_id hold the subscriber's keys).

Usage (ESP-IDF environment active, for nvs_tool.py):
    nvs_fp.py DUMP.bin            fingerprints
    nvs_fp.py --raw DUMP.bin      the same, namespaces as stored
"""
import hashlib
import json
import os
import subprocess
import sys

OLD = {"lc": "oc", "lc_id": "oc_id", "lc_scan": "oc_scan", "lc_ble": "oc_ble"}
OURS = set(OLD) | set(OLD.values())


def main():
    raw = "--raw" in sys.argv[1:]
    dump = [a for a in sys.argv[1:] if a != "--raw"][0]
    tool = os.path.join(os.environ["IDF_PATH"], "components/nvs_flash/nvs_partition_tool/nvs_tool.py")
    out = subprocess.run([sys.executable, tool, "-d", "minimal", "-f", "json", dump],
                         check=True, capture_output=True, text=True).stdout
    keys = {}
    for e in json.loads(out):
        if e["namespace"] not in OURS:
            continue
        ns = e["namespace"] if raw else OLD.get(e["namespace"], e["namespace"])
        k = (ns, e["key"], e["encoding"])
        keys[k] = keys.get(k, "") + str(e["data"])  # a blob's chunks, in order
    for (ns, key, enc), data in sorted(keys.items()):
        print("%s %s %s %s" % (ns, key, enc, hashlib.sha256(data.encode()).hexdigest()[:16]))


if __name__ == "__main__":
    main()
