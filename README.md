# 125A OB-12 VST3 — development branch

> **P007 audit / development branch:** Critical test gaps remain. A negative
> control reproduced a project-private factory-reset bug; another Windows
> integration run exposed a bank-selection/flash-recall race. Corrections are
> in the isolated audit branch and MUST pass the exact SHA's Windows integration
> checks before merge. Official validator, 125A Tester, Studio One interactions
> and all-panel-control QA remain **NOT VERIFIED**.
> See [P007 findings](docs/P007-AUDIT-FINDINGS-2026-11-18.md) and the
> [251-entry control matrix](docs/P007-CONTROL-MATRIX.csv).

This repository is a derivative of [Tuth/ob12-emu](https://github.com/Tuth/ob12-emu) under GPL-3.0.
It adds a native searchable **Program/Timbre Preset Browser**, paired \`12prgXX.mid\` /
\`12tmbXX.mid\` import, and preliminary **per-DAW-project flash-state recall**.

**Status:** Original-firmware audio, native browser, per-project flash recall, and an
original-format 256-Program/256-Timbre bank-install integration test have passed on Windows CI.
The Steinberg Validator and Studio One practical acceptance remain outstanding; keep a backup
of your working plug-in before installing this development build.

**Preset Browser:** right-click the original OB-12 panel and select
**Preset Browser...** The six sound libraries are permanently visible in the
**left sidebar**: Factory, JR Sounds, Virtual Analog 2 (VA2), Jexus 1, Jexus 2,
and Syntpelle. Click one to activate it immediately, without a separate
`Load Bank Pair` dialog. Then choose Programs or Timbres, hardware Bank A
or B, and click a sound on the right to play it through the original emulated
CPU/DSP firmware. Disable the original OB-12 MIDI Program Map for normal
bank/slot addressing.

**How it works:** the original hardware has only A/B (2 x 128 slots per
type). The VST3 holds the other libraries as validated original MIDI
Program/Timbre file pairs on disk, then places the chosen pair in its
**project-private** flash automatically. The first `Current project memory`
entry returns to the sounds that were active before browsing. A saved DAW
project embeds its active 1 MiB flash image and selected MIDI program.
Individual custom edits to *inactive* libraries are not backed up by this
browser: save those edits before switching libraries. Favorites are
separate for each library.

**Additional bank files:** place matching `12prg<suffix>.mid` and
`12tmb<suffix>.mid` under
`OB12.vst3/Contents/x86_64-win/rom/presets`, with suffix length 2-8.
Files like `12prgVA2.mid` are supported. **Add External Bank...** can
load a different file pair; it is not needed for the six included banks.

**Tests:** original-firmware Windows host audio, actual bank-switch flash
write/readback, restoration of project memory, editor reload, three DLL
unload/reload cycles and project recall are automated. Studio One 8
practical acceptance and official Steinberg Validator remain open.

**Build on Windows:** install Visual Studio 2022 LLVM support, Python 3 and Steinberg VST3
SDK 3.7.12; set \`OB12_VST3SDK\`; supply your **own** original firmware in \`rom/\`; then run
\`tools\build_125A.bat\`. The firmware is not tracked in this repository.

See [the 125A source audit](docs/125A-CODE-AUDIT.md) for confirmed fixes, limitations and
the required DAW/regression tests.

---

# ob12-emu

A hardware-level emulator of the **Oberheim OB-12** (Viscount, 2000), a virtual analog synthesizer.
Its two Fujitsu MB90611A (F2MC-16LX) CPUs run the original Master and Slave firmware, and its
five TI TMS57070 DSPs run the original DSP programs. The synth engine is not re-implemented:
the sound comes from the machine's own code.

**Bring your own firmware.** This repository holds no firmware, no ROM-derived data and no
manual artwork. The decoder in `tools/` turns the official OB-12 SysEx update files (`.mid`)
into the images the emulator loads, in `rom/`, which git ignores.

**Ready to play:** the [releases](https://github.com/Tuth/ob12-emu/releases) hold a VST3 bundle
for Windows x64 with the firmware included. The firmware is decoded from the community archive at
https://ob12.clearalgo.com/ and remains copyright Viscount International. It is included for
personal and archival use, not for commercial redistribution. The release binary is built from
the tagged source: rebuilt with the build order below, its code is byte for byte the same, and
only the link timestamps differ.

## Status

- The whole instrument on one thread at about 0.2 of a core, bit identical from run to run.
- Master: the RTOS boots, the LCD (T6963C) works, presets load, and panel data and MIDI reach the Slave.
- Slave: it allocates 12 voices, 3 per DSP, loads the DSP programs and runs the summing bus to the outputs.
- A standalone player with a front panel (Direct2D) and a VST3 instrument (Windows x64).
- Known open question: at maximum Resonance the level rises more than a real unit's recording
  suggests. The firmware drives the filter, so the gap lies in undocumented DSP behaviour or the
  analog output stage. See `docs/HARDWARE.md`.

## The TMS57070 core

`src/tms57070` is this project's own core. It has a flat integer design, and a translated path
runs the jump-free OB-12 programs as straight-line code (`tools/gen_fast.py`, generated into
`build/`).

Instruction behaviour was learned from the public reverse engineering at
[Prehistoricman/TMS57070](https://github.com/Prehistoricman/TMS57070). The test
`tools/tms_harness/core_compare.cpp` checks this core against that emulator: 5.12M steps
identical on all five programs. That emulator has no licence, so none of its code is here.
The comparison tools build only against a local copy you supply (`tools/build_compare.bat <folder>`).

## Building (Windows)

Needs Visual Studio 2022 Build Tools with clang-cl and Python 3. The VST3 build also needs the
Steinberg VST3 SDK, set in `OB12_VST3SDK`. The batch files hold the author's paths, so adjust them.

1. Decode the firmware:
   `python tools/ob12_sysex.py <folder with the update .mid files>/*.mid -o rom`
2. Carve the DSP programs:
   `python tools/ob12_carve.py rom/12dsp111-DSP-v1.11-FINAL.bin -o rom/dsp`
3. Build in this order: `tools/build_master.bat`, `tools/build_engine.bat`, `tools/build_vst3.bat`.

`STATE.json` lists every gate and its pass condition. `docs/HARDWARE.md` is the full research log.

## Third-party code

- `src/f2mc16`: the F2MC-16 CPU core from MAME, BSD-3-Clause, by R. Belmont, with OB-12
  additions (marked). `src/shim` holds minimal stand-ins so it builds outside MAME.
- `src/t6963_cgrom.h`: 5x7 glyphs from Adafruit-GFX-Library `glcdfont.c`, BSD licence,
  Copyright (c) 2012 Adafruit Industries.

## Licence

GPL-3.0, see `LICENSE`. The third-party parts above keep their own licences.

Oberheim is a trademark of its owner. This project is not affiliated with Oberheim or Viscount.
