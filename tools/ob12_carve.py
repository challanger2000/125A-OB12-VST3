#!/usr/bin/env python3
"""Carve TMS57070 programs out of the decoded Slave image, driven by its own tables.

The Slave firmware holds parallel 10-entry tables at FC2340 (all little-endian):
  FC2340  u32[10]  PMEM pointer
  FC2368  u32[10]  12-byte header pointer
  FC2390  u16[10]  PMEM length in 32-bit words
  FC23A4  u16[10]  CMEM length in 24-bit words
  FC23B8  u32[10]  CMEM pointer
PMEM words are stored little-endian, CMEM words big-endian (0x7FFFFF, 0x400000
appear intact only that way).
"""
import argparse
import hashlib
import json
import os
import struct
import sys

SLAVE_BASE = 0xFC0000
TABLE = 0xFC2340
ENTRIES = 10
HEADER_SIZE = 12
PMEM_MAX_WORDS = 512
CMEM_MAX_WORDS = 512


def read_table(image):
    offset = TABLE - SLAVE_BASE
    entries = []
    pmem = struct.unpack_from(f"<{ENTRIES}I", image, offset)
    header = struct.unpack_from(f"<{ENTRIES}I", image, offset + 0x28)
    pmem_len = struct.unpack_from(f"<{ENTRIES}H", image, offset + 0x50)
    cmem_len = struct.unpack_from(f"<{ENTRIES}H", image, offset + 0x64)
    cmem = struct.unpack_from(f"<{ENTRIES}I", image, offset + 0x78)
    for index in range(ENTRIES):
        entries.append({"index": index, "pmem": pmem[index], "header": header[index],
                        "pmem_words": pmem_len[index], "cmem": cmem[index],
                        "cmem_words": cmem_len[index]})
    return entries


def check(entry, image):
    """Return a list of problems with one table entry."""
    problems = []
    if entry["pmem"] != entry["header"] + HEADER_SIZE:
        problems.append("header does not sit directly before PMEM")
    if entry["pmem_words"] > PMEM_MAX_WORDS:
        problems.append("PMEM length exceeds 512 words")
    if entry["cmem_words"] > CMEM_MAX_WORDS:
        problems.append("CMEM length exceeds 512 words")
    if entry["cmem"] != entry["pmem"] + entry["pmem_words"] * 4:
        problems.append("CMEM does not follow PMEM")
    if entry["cmem"] + entry["cmem_words"] * 3 - SLAVE_BASE > len(image):
        problems.append("CMEM runs past the image")
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("slave_image", help="decoded Slave .bin")
    parser.add_argument("-o", "--out", required=True, help="output directory")
    args = parser.parse_args()
    with open(args.slave_image, "rb") as handle:
        image = handle.read()
    os.makedirs(args.out, exist_ok=True)
    manifest = {}
    written = {}
    failed = False
    for entry in read_table(image):
        problems = check(entry, image)
        tag = f"prog{entry['index']}"
        if problems:
            failed = True
            print(f"FAIL {tag}: {'; '.join(problems)}")
            continue
        pmem_at = entry["pmem"] - SLAVE_BASE
        cmem_at = entry["cmem"] - SLAVE_BASE
        head_at = entry["header"] - SLAVE_BASE
        words = struct.unpack_from(f"<{entry['pmem_words']}I", image, pmem_at)
        pmem = struct.pack(f">{entry['pmem_words']}I", *words)
        cmem = image[cmem_at:cmem_at + entry["cmem_words"] * 3]
        header = image[head_at:head_at + HEADER_SIZE]
        key = (entry["pmem"], entry["cmem"])
        if key in written:
            alias = written[key]
            manifest[tag] = {"same_as": alias}
            print(f"OK {tag}: same block as {alias}")
            continue
        written[key] = tag
        for suffix, blob in (("pmem", pmem), ("cmem", cmem), ("hdr", header)):
            with open(os.path.join(args.out, f"{tag}.{suffix}.bin"), "wb") as handle:
                handle.write(blob)
        manifest[tag] = {
            "pmem_addr": f"{entry['pmem']:#x}", "pmem_words": entry["pmem_words"],
            "cmem_addr": f"{entry['cmem']:#x}", "cmem_words": entry["cmem_words"],
            "header": header.hex(" "),
            "pmem_sha256": hashlib.sha256(pmem).hexdigest(),
            "cmem_sha256": hashlib.sha256(cmem).hexdigest(),
        }
        print(f"OK {tag}: PMEM {entry['pmem']:#x} x{entry['pmem_words']} "
              f"CMEM {entry['cmem']:#x} x{entry['cmem_words']} hdr {header.hex(' ')}")
    with open(os.path.join(args.out, "manifest.json"), "w") as handle:
        json.dump(manifest, handle, indent=2)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
