#!/usr/bin/env python3
"""Generate a temporary OB-12 factory-bank MIDI transfer fixture from a *local*
original factory binary, preserving every byte of the hardware data.

For integration tests only. The output must never be committed or redistributed.
The round-trip decoder validates all checksum, record address and declared-size
properties. The generator does not create or change sound content.
"""
from __future__ import annotations
import pathlib
import struct
import sys
from ob12_sysex import decode

HEADER = bytes((0x31, 0x30, 0x00))

def syx(cmd: int, data: bytes = b"") -> bytes:
    body = HEADER + bytes((cmd,)) + data
    return bytes((0xF0,)) + body + bytes(((~sum(body)) & 0x7F, 0xF7))

def pack_7_1(raw: bytes) -> bytes:
    result = bytearray()
    for offset in range(0, len(raw), 7):
        group = raw[offset:offset+7]
        top = sum((1 << i) for i, byte in enumerate(group) if byte & 0x80)
        result.extend(byte & 0x7F for byte in group)
        result.append(top)
    return bytes(result)

def vlq(n: int) -> bytes:
    assert 0 <= n < (1 << 28)
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.insert(0, (n & 0x7F) | 0x80)
        n >>= 7
    return bytes(out)

def midi_bank(image: bytes, base: int, label: str) -> bytes:
    # Authentic factory bank layout, original full image only.
    assert label in ("PROGRAM", "TIMBRE")
    assert (base, len(image)) in ((0xD0000, 40448), (0xC0000, 42752))
    events = ([syx(0x04, b"\x00\x44\x01")] if getattr(midi_bank, "va2", False) else []) + [syx(0x68), syx(0x68),
              syx(0x6F, f". Updating Sect.[{label}] .".encode("ascii")),
              syx(0x70, b"> From dump OB 12 <")]
    cursor = base
    for start in range(0, len(image), 42):
        chunk = image[start:start+42]
        events.append(syx(0x7F, f"{cursor:05X}".encode("ascii") +
                          bytes((len(chunk),)) + pack_7_1(chunk)))
        cursor += len(chunk)
    events.append(syx(0x7B if label=="PROGRAM" else 0x7C,
                      f"{len(image):05X}".encode("ascii")))  # original Program/Timbre section end
    events.append(syx(0x7D))
    # Standard MIDI format 0. Send 10ms between records in the synthetic test
    # to reduce CI runtime. The hardware-compatible loader itself still sends
    # the original documented 100ms inter-message gap.
    track = bytearray()
    for event in events:
        track.extend(vlq(8))  # 8 ticks, at 384 ppqn/120 BPM ~10.4ms
        track.extend(b"\xF0")
        track.extend(vlq(len(event)-1))
        track.extend(event[1:])
    track.extend(b"\x00\xFF\x2F\x00")
    return b"MThd" + struct.pack(">IHHH", 6, 0, 1, 384) + b"MTrk" + struct.pack(">I",len(track)) + track

def main():
    if len(sys.argv) != 4:
        sys.exit("usage: generate_bank_fixture.py <original rom/presets> <output folder> <suffix>")
    rom, target, suffix = pathlib.Path(sys.argv[1]),pathlib.Path(sys.argv[2]),sys.argv[3]
    if not 2 <= len(suffix) <= 8 or not suffix.isalnum():
        sys.exit("suffix must be 2 to 8 ASCII alphanumeric characters")
    target.mkdir(parents=True,exist_ok=True)
    midi_bank.va2 = suffix.upper() == "VA2"
    for kind,base,nbytes,label in (("prg",0xD0000,40448,"PROGRAM"),
                                   ("tmb",0xC0000,42752,"TIMBRE")):
        source=rom/f"12{kind}FS.bin"
        image=source.read_bytes()
        if len(image)!=nbytes:
            sys.exit(f"ERROR {source}: expected {nbytes} bytes; got {len(image)}")
        out=target/f"12{kind}{suffix}.mid"
        out.write_bytes(midi_bank(image,base,label))
        checked_base,checked_image,details=decode(str(out))
        if checked_base!=base or checked_image!=image or details["declared_size"]!=len(image):
            sys.exit("FAIL: the generated MIDI stream did not decode byte-identically")
        print(f"ROUNDTRIP_PASS {out.name}: records={details['records']} bytes={len(image)} checksum={details['checksums_ok']} base=0x{base:x}")
if __name__=="__main__": main()
