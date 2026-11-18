#!/usr/bin/env python3
"""Decode Oberheim OB-12 firmware .mid files (SysEx update stream) to flat images.

Stream layout, measured from the files (every message starts 31 30 00 <cmd>):
  68            start marker (sent twice)
  6F <text>     section title, e.g. ". Updating Sect.[Software] ."
  70 <text>     version text
  7F <addr:5 ASCII hex> <len> <payload> <chk>   data record
  7E/67/71/7A <size:5 ASCII hex>                section end, size in bytes
  7D            end marker

Payload packing: groups of 8 bytes = 7 data bytes followed by one byte that
holds their top bits (bit k -> data byte k). Selected because it is the only
tested variant that yields intact long strings; the size check below proves
the record framing, and the strings prove the bit order.

Checksum: the byte before F7 is ~sum(all preceding message bytes) & 0x7F.
Every message is checked; a mismatch aborts the decode.
"""
import argparse
import hashlib
import json
import os
import struct
import sys

HEADER = bytes((0x31, 0x30, 0x00))
SIZE_CMDS = {0x7E, 0x67, 0x71, 0x7A}


def read_vlq(buf, pos):
    value = 0
    while True:
        byte = buf[pos]
        pos += 1
        value = (value << 7) | (byte & 0x7F)
        if not byte & 0x80:
            return value, pos


def sysex_messages(path):
    """Return the SysEx bodies (without the leading F0) of a standard MIDI file."""
    with open(path, "rb") as handle:
        buf = handle.read()
    if buf[:4] != b"MThd":
        raise ValueError(f"{path}: not a standard MIDI file")
    pos = 8 + struct.unpack(">I", buf[4:8])[0]
    messages = []
    while pos < len(buf):
        length = struct.unpack(">I", buf[pos + 4:pos + 8])[0]
        cur = pos + 8
        end = cur + length
        running = 0
        while cur < end:
            _, cur = read_vlq(buf, cur)
            status = buf[cur]
            if status in (0xF0, 0xF7):
                size, cur = read_vlq(buf, cur + 1)
                messages.append(buf[cur:cur + size])
                cur += size
            elif status == 0xFF:
                size, cur = read_vlq(buf, cur + 2)
                cur += size
            else:
                if status & 0x80:
                    running = status
                    cur += 1
                cur += 1 if (running & 0xF0) in (0xC0, 0xD0) else 2
        pos = end
    return messages


def unpack(payload):
    out = bytearray()
    for start in range(0, len(payload), 8):
        group = payload[start:start + 8]
        top = group[-1]
        for index, byte in enumerate(group[:-1]):
            out.append(byte | (0x80 if top & (1 << index) else 0))
    return out


def text_of(message):
    return bytes(message[4:-2]).decode("ascii", "replace").strip(" .")


def decode(path):
    """Return (base_offset, image, info). Raises ValueError on any inconsistency."""
    info = {"title": None, "version": None, "declared_size": None, "records": 0,
            "checksums_ok": 0}
    chunks = []
    for message in sysex_messages(path):
        if message[:3] != HEADER:
            raise ValueError(f"{path}: unexpected SysEx header {message[:4].hex()}")
        if (~sum(message[:-2])) & 0x7F != message[-2]:
            raise ValueError(f"{path}: checksum mismatch in {message[:10].hex(' ')}")
        info["checksums_ok"] += 1
        cmd = message[3]
        if cmd == 0x7F:
            addr = int(bytes(message[4:9]).decode("ascii"), 16)
            data = unpack(message[10:-2])
            length = message[9]
            if length > len(data):
                raise ValueError(f"{path}: record {addr:#x} shorter than its length")
            chunks.append((addr, bytes(data[:length])))
        elif cmd == 0x6F:
            info["title"] = text_of(message)
        elif cmd == 0x70:
            info["version"] = text_of(message)
        elif cmd in SIZE_CMDS:
            info["declared_size"] = int(bytes(message[4:9]).decode("ascii"), 16)
    if not chunks:
        raise ValueError(f"{path}: no data records")
    info["records"] = len(chunks)
    base = chunks[0][0]
    cursor = base
    image = bytearray()
    for addr, data in chunks:
        if addr != cursor:
            raise ValueError(f"{path}: gap at {addr:#x}, expected {cursor:#x}")
        image += data
        cursor += len(data)
    if info["declared_size"] is not None and info["declared_size"] != len(image):
        raise ValueError(
            f"{path}: declared size {info['declared_size']:#x} != image {len(image):#x}")
    return base, bytes(image), info


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("inputs", nargs="+", help=".mid firmware files")
    parser.add_argument("-o", "--out", required=True, help="output directory")
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    manifest = {}
    failed = False
    for path in args.inputs:
        name = os.path.basename(path)
        try:
            base, image, info = decode(path)
        except ValueError as error:
            print(f"FAIL {error}")
            failed = True
            continue
        stem = os.path.splitext(name)[0]
        with open(os.path.join(args.out, stem + ".bin"), "wb") as handle:
            handle.write(image)
        digest = hashlib.sha256(image).hexdigest()
        manifest[stem] = {"flash_offset": f"{base:#x}", "size": len(image), "sha256": digest, **info}
        declared = "ok" if info["declared_size"] == len(image) else "NOT DECLARED"
        print(f"OK {stem}: offset {base:#07x} size {len(image):#x} size-check {declared} "
              f"records {info['records']} checksums {info['checksums_ok']} "
              f"[{info['title']}] [{info['version']}] {digest[:12]}")
    with open(os.path.join(args.out, "manifest.json"), "w") as handle:
        json.dump(manifest, handle, indent=2)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
