#!/usr/bin/env python3
"""Fingerprints of OpenCell's own NVS keys (namespaces oc, oc_id, oc_scan,
oc_ble) in a dumped NVS partition, for bench checks around a flash: one line
per key, "namespace key type sha256[:16]", so a dump from before and one from
after can be diffed. Values are never printed (oc_id holds the subscriber's
keys).

Usage (ESP-IDF environment active, for nvs_tool.py):
    nvs_fp.py DUMP.bin
"""
import hashlib
import json
import os
import subprocess
import sys

OURS = {"oc", "oc_id", "oc_scan", "oc_ble"}


def main():
    dump = sys.argv[1]
    tool = os.path.join(os.environ["IDF_PATH"], "components/nvs_flash/nvs_partition_tool/nvs_tool.py")
    out = subprocess.run([sys.executable, tool, "-d", "minimal", "-f", "json", dump],
                         check=True, capture_output=True, text=True).stdout
    keys = {}
    for e in json.loads(out):
        if e["namespace"] not in OURS:
            continue
        k = (e["namespace"], e["key"], e["encoding"])
        keys[k] = keys.get(k, "") + str(e["data"])  # a blob's chunks, in order
    for (ns, key, enc), data in sorted(keys.items()):
        print("%s %s %s %s" % (ns, key, enc, hashlib.sha256(data.encode()).hexdigest()[:16]))


if __name__ == "__main__":
    main()
