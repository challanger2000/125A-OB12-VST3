# OB-12 hardware and firmware facts

Each line says where it came from. Schematic pages are the service manual's own
`NN/15` numbers, read as rendered pages with vision, never by OCR.
`F:\MIZU\OB-12\OB-12-PROJECT-SUMMARY.md` is superseded where it disagrees with this file.

## Settled (schematic)

| Part | Detail | Source |
|---|---|---|
| Master CPU | IC12 MB90611APF, 4 MHz crystal, panel, keys, LCD, MIDI | 02/15 |
| Master flash | IC24 29F800T, 1 MB, CS0, F00000-FFFFFF | 03/15 |
| Master RAM | IC22/IC23 8128 SRAM, 256 KB, CS1, E00000-E1FFFF | 03/15 |
| Slave CPU | IC25 MB90611APF, 4 MHz crystal, debug RS232 test points | 06/15 |
| Slave flash | IC14 29F200T, 256 KB | 07/15 |
| Slave RAM | IC7/IC8 84256 SRAM, 64 KB | 07/15 |
| Clock | XTAL1 12.288 MHz = FSx256, so FS = 48 kHz | 08/15 |
| PLD | IC15 ATF1500-15J: D[7:0], A0, CSDSP, RD, WRL in; FS clocks, HCK[2:0], HR, HRS, HXS out; contents unknown | 08/15 |
| Voices | DASP0-3 = IC39, IC40, IC41, IC26 TMS57070, "DSP 24 bit", no external memory | 10/15 |
| LFO and effects | DASP4 = IC42 TMS57070 with 3x 44C256 DRAM on a 12-bit bus | 09/15 |
| Audio chain | DASPn AX2/AX3 feed DASPn+1 AR1/AR2; DASP3 feeds DASP4; DASP4 AX1 returns LFO to DASP0 AR1 | 09/15, 10/15 |
| DSP select | NDCS[4:0] on Slave P70-P74; shared HR/HX serial host bus, EMPTY and RDY back to Slave | 06/15, 10/15 |
| DSP clock | "PLL x 2" noted at DASP0: 24.576 MHz = 512 cycles per 48 kHz sample | 10/15 |

## Settled (measured from firmware)

- SysEx format, packing and checksum: see `tools/ob12_sysex.py`. All files pass.
- Master image: flash offset 0 = F00000. CALLP targets cluster in F0-F5, image ends F53D93.
- Boot block: flash offset FC000 = FFC000. Reset vector at FFFFDC: PC FFC012, mode 0x88.
- `unidasm -arch f2mc16` gives coherent code from FFC000 (stack, bus and chip-select setup).
- Slave image (`12dsp111`, section `[SoftSlave]`) is F2MC-16 code based at FC0000,
  not DSP code: string pointer FC00E0 hits "ERROR: interrupt IsrNull !!!".

## Settled (DSP microcode, carved by `tools/ob12_carve.py`)

The Slave holds parallel 10-entry tables at FC2340. All entries pass the layout
checks: header sits 12 bytes before PMEM, CMEM follows PMEM directly.

| Entry | PMEM | Words | CMEM | Words |
|---|---|---|---|---|
| 0 | FDB324 | 512 | FDBB24 | 487 |
| 1-3 (one shared block) | FDC0E6 | 512 | FDC8E6 | 487 |
| 4 | FDCEA8 | 512 | FDD6A8 | 313 |
| 5-8 (one shared block) | FDDD12 | 22 | FDDD6A | 3 |
| 9 | FCCFE8 | 10 | FCD010 | 1 |

- PMEM words are stored little-endian, CMEM words big-endian.
- Programs 0 and 1 differ in only 9 of 512 words.
- CMEM holds at least 487 words, so the wiki's 256-word CMEM cannot apply here.
- Static check (`tools/ob12_dsp_check.py`): primary opcodes 100% known in all programs,
  secondary 99.3-100%. Control on Slave CPU code: secondary 59.4%, jumps 0/23 in range.
- The three 512-word programs contain no 0xFx jump words at all.
- Secondary opcode 0x34 is absent from the public emulator's secondary switch, used by
  `6C341AB0` (programs 0, 1 at 11B, 186, 1F1) and `53F410D2` (program 4 at 01A, 048).
  The public IDA module does have an 0x34 branch.

## Open, not yet facts

- **Emulated run, 48000 samples per program, public emulator, no parameters from the Slave.**
  Not ground truth: the emulator is incomplete and the harness feeds a test sine.
  - Programs 0, 1, 4: all 512 words executed, 0 PC discontinuities, so straight-line
    execution with PC wrapping holds in this run. DMEM and CMEM change as it runs.
  - Speed: 26-35 ns per step, instrumented, one thread. Five DSPs need 123M steps per
    second, about 8 ns per step on one core, so a purpose-built core is needed.
  - Unimplemented paths hit, per sample: opcode 1FC divide step (30 in programs 0 and 1),
    0x3E with flag 8 (1 in programs 0 and 1), 0x43 MACLO multiply (3 in programs 0 and 1,
    2 in program 4), secondary 0x34 (3 in programs 0 and 1, 2 in program 4).
  - The emulator's 4-deep stack overflows, so the harness wraps it. The program has no
    return path used here; 192 interrupts fired in 48000 samples, then stopped. Not understood.
- **Audio output path, found by test.** Secondary 0x0C, 0x0D, 0x0E with flag 4 (bit 14)
  appear to write DMEM to AX1, AX2, AX3; flag 8 selects right. Without flag 4, 0x0C and
  0x0D read AR1, AR2. The public emulator treats these as reads only, and 0x0E as empty.
  Prediction tested on a patched local copy, 4000 frames, distinct sines on each input:
  - Program 1: AX3L = AR2L and AX3R = AR2R, 100% bit-exact at lag 0.
  - Program 0: AX3L = AR1L and AX3R = AR1R, 100%, matching 10/15 where DASP0's AR1 takes
    the LFO and forwards it down the AR2/AX3 bus.
  - Program 1: AX2L = AR1L and AX2R = AR1R, 2 half-frames late, with no voices playing.
    This is the summing bus. The first 93.7% for AX2R was a test artefact: compared on even
    half frames, 0 of 7968 differ.
  - Program 4 writes AX1, AX2, AX3, the pins 09/15 labels LFO, MOUT and AOUT.
  - Rates: AX2 and AX3 on program 4 write once per frame (48 kHz, as FSxd on the DAC
    side). AX L channels on the voice chain write twice per frame, matching the FSx2 clock.
- **Five-DSP chain, `tools/tms_harness/chain_test.cpp`, 8000 frames, wired per 09/15, 10/15.**
  Links are copied once per half frame, a model choice, so lags are model values.
  - Summing bus: tone into DASP1 AR1 reaches DASP4 AR1L 100% bit-exact, lag 9 half frames,
    stepping 3 per DSP. DASP4 AR1R best match 87.5% in the report; only DASP1's right
    channel was checked on matched parity.
  - LFO bus: tone into DASP0 AR1 reaches DASP4 AR2L and AR2R 100% bit-exact, lag 4 and 8.
  - Program 4 never reads AR2, so the LFO bus ends at DASP4.
  - 26.5 ns per DSP step across the chain.
- **DASP4 outputs stay silent.** Input reaches DMEM (D0D0, D0D1, copies at D053 onward).
  Outputs come from D017, D018 (AX2) and D00B, D00F (AX3), through CMEM pointers 7D-80,
  fed by a MAC chain at 060-06F whose accumulators stay zero. Setting each of the 180 zero
  CMEM words to 0x400000, singly and all at once, opened nothing. That test is weak: it
  also overwrites pointers and cannot change nonzero words. Cause unknown; candidates are
  Slave-written state, the unimplemented opcodes, or DMEM state set by the Slave.
- **Secondary 0x38.** One external write per sample in every large program, including
  voice DSPs with no external RAM. Purpose unknown.
- **Entry assignment hypothesis.** Entries 0-4 = DASP0-4 in normal use; 5-9 = short
  programs for the same chips, perhaps the Master's PROBEDASP tests. Needs the loader code.
- **12-byte header meaning.** Read as big-endian CR0-CR3, `3CF3CF B90100 FCFF00 030000`
  sets EXT=1 and EXTMEM=1, the 512-word CMEM mode that 487-word CMEM requires. DASP4's
  CR3 `250000` differs in the external bus width bits. Strong fit, but the loader is unread.
- **Slave boot block missing.** 167 of 1754 plausible CALLP targets land above the
  image, on 4 addresses: FFD3A0 (161 calls), FFD215, FFD12E, FFD1BD. The reset vector
  is also absent. `boot_152` at those offsets does not look like function entries.
  No supplied file contains this code. FFD3A0 falls mid-function in `boot_152`, so that
  file is not the Slave's. Likely factory-resident Slave code that updates never rewrite:
  the Master's own CPU update also stops at F53D93 and leaves FFC000 untouched, and the
  Slave image holds no flash-writing text or reset vector, so something resident must
  receive it. Inferred, not observed.
- **DMEM size.** The public emulator's EXT and EXTMEM logic gives 256-word DMEM with
  512-word CMEM, which would explain the wiki figure. Confirm from microcode addressing.
- **PLD behaviour** must be inferred from Slave code and the schematic.
- **Master to Slave link** runs over MSCK, MTX, STX; protocol unknown.

## Master boot, `src/ob12_master.cpp` (measured)

300M instructions, 76.8 s emulated, no crash. Real firmware, no patches to it.

- Reset at FFC012, boot hands over to the F00000 image, LCD splash drawn (35127 writes).
- RTOS with 9 tasks; stacks at EC:87F4 upward, switch by `MOVW SP,A` then `RETI` at F01C76.
- Idle loop at F00709 takes most cycles.
- UART1 sends MIDI Active Sensing `FE`: 257 bytes in 76.8 s, about one per 300 ms.
- A/D: 8-channel scan by EI2OS, 140032 transfers. Time base and reload timers all firing.
- UART0 (LED) and UART2 (Slave) have sent 0 bytes. The display stays on the splash with an
  empty progress bar. What it waits for is not yet known; the Slave is the prime suspect.
- The stall on the splash is solved. Causes, in order found:
  - Factory timbres and programs were not loaded. They belong at flash offsets C0000 and
    D0000, from `12tmbFS.mid` and `12prgFS.mid` in the presets zip.
  - The firmware then formats user flash, so the 29F800 needed a command model.
  - Upstream bug: `CALLP @RWx+disp8` pushed return address +4 for a 3-byte instruction.
  - External interrupts were not the cause: raising them changed nothing.
- Now, over 250M instructions, 65.6 s emulated: main screen layout drawn, UI dispatcher
  running, 6 sector erases and 196608 words programmed, UART1 sends MIDI, and UART2 sends
  849 bytes to the Slave. These look like MIDI: `B0 7B 00` etc., All Notes Off per channel.
- **Task names** from RAM: PRECORD, PPLAY, PKBD, PSWITCH, PHDISPL, PADC, PPANEL, PDISPLAY,
  PNULL. Queues: QPANEL, QKBD, QDISPLAY, QHDISPL, QRECORD, QPLAY.
- **Font missing.** The text routine reads its table at FF0002 (F0851F). No supplied file
  covers FF0000-FF9FFF, so screens show boxes without text. That 32 KB flash sector is
  probably factory-resident, like the Slave boot. ob12.clearalgo.com lists nothing newer.
- From sheet 04/15: CS2 LED- From sheet 04/15: CS2 LED 68FF80-8F, CS3 LCD 68FF00-01 (A0 = C/D), CS4 keyboard 0000C0-CF.
- RAM at EC0000-EFFFFF is the 256 KB SRAM seen through a mirror (A18, A19 unconnected).

Core changes against upstream MAME, all in `src/f2mc16`:
- Generic effective-address decoder `ob12_ea.inc`: upstream lacks many addressing modes.
- Added NCC, CMR, CALLV, MOVX A,dir, MOVX A,io, XCH Ri,ea, MOVW @AL,AH and ALU forms.
- Bug fixed: interrupt level narrowed to 8 bits, clearing ILM and RP in PS on every interrupt.
- Bug fixed: `CALLP @RWx+disp8` return address.

## Slave boot, `src/ob12_slave.cpp` (measured)

A replacement stub stands in for the missing resident boot: banks EF, SP FFF0, ILM 7, JMPP
FC0500. Those values, the vector mapping, the UART2 enable and interrupt levels 2 and 3 are
choices based on the firmware, not known hardware values.

Over 120M instructions, 42.9 s emulated, replaying the 849 bytes captured from the Master:
- Prints `Hello, those fellows!`, `Slave Ver: Dec 11 2000`, and its polyphony line.
- Consumes all 849 bytes and **answers the Master**: 18 bytes, `11 "Dec 11 2000 1.11" 00`.
- ISRs sit on fixed 0x100 boundaries: FC0100 UART2 RX, FC0200 UART2 TX, FC0300 not yet
  identified, main at FC0500. Vectors live in the resident boot.
- Resident calls: FFD12E(0,2), FFD12E(1,4), FFD1BD(0), FFD215(0,5), FFD215(1,3), FFD215(2,0).
  Meaning unknown. The firmware never configures UART2, so these probably do.
- UART1 sends 16 bytes to the DAC side: `20 10 60 C2 80 FF C0 FF` twice.

DSP host protocol, read from FC2690-FC2790 and checked against the captured stream:
- Bytes go to 68FF80, and DASP selection is active low on port 7 bits 0-4.
- Load: command `0C`, the 12 header bytes, then PMEM words high byte first, padded to 512.
- The stream holds program 5 first, then program 1. PMEM in the stream equals the carved
  `prog1.pmem.bin` byte for byte. CMEM follows, also matching at its start.
- P60 EMPTY is modelled low and P61 RDY high; real timing is unknown.
- DASP4 stream keeps growing (49160 bytes) as the loop runs: runtime CMEM updates, format
  not read yet.

## Whole instrument, `tools/tms_harness/ob12_system.cpp` (measured)

Master, Slave and five DSP models in one process, UART2 linked live, stable for 30 s
emulated. The Master sends 849 bytes and the Slave answers `Dec 11 2000 1.11` at about 12 s.

Runtime host protocol, decoded and in use: `03` then 5-byte groups, 9-bit CMEM address and
24-bit value, ended by NDCS changing. Load: `0C` + 12 + 2048, then `05` + 1536.

**First sound.** Note-on bytes injected straight into the Slave, bypassing the Master:
- Voices are allocated three per DSP: 8 notes went 3, 3, 2 to DASP0, 1, 2. So 12 voices.
- About 50 CMEM writes per note.
- The voice bus into DASP4 carries a sawtooth: 196.0 Hz with harmonics 1, 1/2, 1/3, 1/4,
  1/5, 1/6. Signal is on the left channel of the odd half-frame slot only.

Not understood or not working:
- Note 60 sounds at 196 Hz, not 261.6 Hz. Patch transposition or a fault, unknown.
- Level keeps rising for as long as notes hold, with no note-off sent. Unexplained.
- Channel 0 alone gave silence; the signal appears with the next channels.
- A MIDI note sent to the Master is received but not forwarded to the Slave.
- DASP4 output now passes signal, with a large fault: see below.
- Secondary 0x34 is a noise generator per the public IDA module, 3 uses per voice DSP.
  It, 1FC, 0x3E flag 8 and 0x43 MACLO remain unimplemented in the local emulator.

## DASP4 output (measured)

Why it was silent, found with `tools/tms_harness/dasp4_trace.cpp`, a differential trace:
- Program 4 stores input at D00/D04 and reads D03, D0B, D0F, D11. No word writes those
  addresses directly: they are the same data some samples later through circular DMEM.
- Word 000 sets LDMEM, so word 001, the circular step, never advanced DOFF in the emulator.
- Hypothesis, applied in the local emulator copy only: a lock set by secondary 0x2E does not
  yet cover the next instruction. With it the signal spreads to over 200 state items and
  outputs open. It fits the program but is not checked against silicon.
- Half-frame phase: DASP4 reads the voice bus at D0D0/D0D1 in one half frame and at D08/D0C
  in the other. D08/D0C went nowhere in this run. The harness now offsets DASP4 by 256 steps.
  Real chip start phase is unknown.

Result with notes sent to the Slave, 20 s emulated:
- MOUT and AOUT carry signal on both channels, peaks near full scale.
- **Noise fault:** output starts 0.06 s after DSP start with no note played. RMS about 5400 of
  32768, spectrally flat (0.72), the same before and after notes. Cause unknown.
- The note is present under it: peaks 187.5, 375, 562.5, 750 Hz and a second series at 179,
  366.5, 554, 741.5 Hz. The voice bus earlier read 196 Hz; the change is unexplained.

Noise investigation, later round. The fault changed form but is not solved:
- The program never sets DCIRC, so the emulator's window swept all 256 DMEM cells and hit
  the fixed variables. Direct addresses split cleanly: unlocked 00-CC, locked CE-FF.
- Local change: a true ring over the low cells, fixed cells above. Ring size unknown; CC to D0
  were tried, none is clearly right. 100 (the old behaviour) is far worse.
- With the ring alone, silent input built up to full scale: unstable feedback.
- 0x43 as multiply by MACLO cut silent output from about 6,000,000 to 100,000. As load-and-
  halve (the IDA module's reading) it gave about 1,000,000. MACLO looks right, not proven.
- 1FC as written here is wrong or incomplete: it makes idle voices output DC (055C10).
  It is off for now, so 30 divide steps per sample still do nothing.
- Current output with ring CE, 0x43 on, 1FC off: broadband noise gone, spectral flatness
  0.72 to 0.0. Instead a full-scale impulse repeats at 233.0 Hz, 48000/206, the ring period.
  One cell or wrap point is wrong. Note level on the voice bus rises under it but is not
  audible beside the spike.
- All of these changes live only in the local emulator copy: 0x2E lock delay, ring, 0x43,
  1FC, audio port writes. They are listed here because that copy is not in this repo.

## Own DSP core, `src/tms57070` (measured)

Flat integer core, no external code at build time. The whole instrument now builds and runs
from this repo alone: `src/ob12_system.cpp`.

- `tools/tms_harness/core_compare.cpp`, options off, all five programs, 2000 frames each:
  5.12M steps identical in PC, ACC, MACC, CR1, XRD, DA, CA, DMEM, CMEM. About 20 ns per step.
  This proves agreement with the public emulator, not with silicon. Program coverage was
  with default coefficients, so paths needing live parameters were less exercised.
- **Licence:** this core is not clean-room. It was written after reading the public source,
  and its behaviour comes from that work, while the code and structure are our own. Whether
  that is enough is a judgement for Thor; asking the author remains the safe route.

**Noise and impulse: solved.** Programs 5 and 9 are start-up code run before the main load.
Program 9 ends `C70CD000`: DCIRC = CD, ring of 206 cells, as the address split predicted.
Program 5 sets DIR and CIR for the voice chips. The old harness never let them run.
With them run and the registers surviving the reload:
- Output is exactly silent with no note: 0 from the 0.06 s fault onward.
- Earlier noise and the 233 Hz impulse were harness faults, not DSP behaviour.
- The earlier 187.5 Hz and 179 Hz readings came from those faulty runs.

Options on in the system: audio ports, true ring, 2E lock delay, MACLO multiply. 1FC off.

Open, in order:
- MOUT and AOUT are silent even with notes, in both DASP4 phases. The voice bus into DASP4
  carries signal on left, odd half frames, rising to RMS 111015. Right is zero. Phase 1 puts
  that slot on D0D0; phase 0 puts it on D08. Superseded by "Two-slot audio bus": phase 0
  is the default and D08 does reach the output.
- The gain path needs finding: C77 and other coefficients are zero. They may come from a
  Master message not yet sent, for example panel or volume values.
- Unimplemented and hit every sample: 1FC, 3E flag 8, secondary 34.

## DASP4 gain study, `tools/dasp4_gain.cpp` (measured, own core)

- With a true ring the tone reaches D08, D0A, D0D, D0E, then D12 and D16 at 0F3/0F4. At
  0FD/0FE those are overwritten by products that carry no signal, so the coefficient behind
  0FB/0FC (read through CA) is zero, and the signal stops there.
- Setting all 300 zero CMEM words gives output, and a minimal set C00D, C00F, C0CB keeps it.
  **This does not show they are gains.** Forcing them to 200000 in the full system pins MOUT
  at full-scale DC with notes or without: an unstable feedback path, not an opened gain.
- Control messages: `F0 7F 30 04 <param> <hi> <lo> F7` is a real Slave command family, the
  Master sends four of them. Params 00-07 include one that writes C0CB = 03513E. No CC
  0-119 and no param 00-1F with value 04 7F wrote C00D or C00F. Output stayed silent.
- Slave parameter handling is table driven around FCD7xx. Not reversed yet.
- Voice pitch re-measured in the corrected run: 196.0 Hz sawtooth, so that reading stands.
  Note 60 should be 261.6 Hz; the patch may transpose, unknown. Level still rises while held.

**Solved: the outputs needed panel values.** The Master sends them only on change, and the
harness A/D was constant. With every pot moving from 14 s on (`OB12_POT_SWEEP`):
- Master to Slave grows from 849 to 18069 bytes: `A0`, `A8`, `B0`, `E0` messages and
  `F0 7F 30 04 01 00 5B F7`, about 1700 bytes per second.
- CMEM writes rise: DASP4 8948 to 61751, DASP0 3 to 37524.
- MOUT left carries the note: 196.0 Hz sawtooth, RMS about 200, peak 1397 of 32768, with no
  forced coefficient. MOUT right and AOUT stay zero. Sound is present on the real output.
- C00D, C00F stay 0 throughout, so they were never output gains.
- Not measured: which pot drives which parameter. The sweep moves all at once, so the level
  here is arbitrary and DASP0's output changes as parameters move.

## MIDI through the Master (measured)

- The UART1 receive ISR F0C75D stores bytes in a ring at EC5779 and ends `MOV I:9F,#1`. That
  is DIRR, the delayed interrupt module, vector 2A. Bytes are parsed there. It was not
  modelled; added to Master and Slave.
- With it, notes on 16 channels gave one forwarded message: `94 3C 64` at stream offset 12B4.
  The other 15 were not forwarded, which fits a single receive channel. Which input channel
  passed is not measured, though channel 0 was sent first and writes began at once.
- Master turns MIDI channel into Slave channel 4. DASP0 then takes the voice: CMEM writes rise
  from 3 to 37091.
- That single note is silent, and so are two. See "Voice slots" below: the path works.

Silent timbre study:
- The Master loads four timbres, `F0 7E 00..03`, and program data `F0 7E 04`.
- `tools/compare_channels.py`: Slave channel 4 writes the same values as channel 0. So the
  Master plays timbre 0. Timbres 1 and 2 differ in about 30 CMEM values, for example C014,
  C04B, C087. **Retracted:** timbre 0 is not silent, see "Two-slot audio bus" below.
- Noise hypothesis tested and rejected: enabling secondary 34 changed nothing. Retracted, see "Panel map".
- Voice code runs two divides of 16 steps by D04: 1F8 then 15 x 1FC, at 046 and 059.
- 1FC with a zero divisor made idle voices output DC, so a guard treats divisor 0 as no
  subtract. This guard is a hypothesis.
- With 1FC on, direct notes: the level is now steady, RMS 160510 on the bus instead of rising,
  so the rise was caused by the missing division. A second bus slot carries signal, RMS
  about 150000. Spectrum changed: 392 Hz strongest, 196 Hz at 0.36. 1FC is on by default.
- Right channel still zero.

## Two-slot audio bus (measured)

**Retraction.** "Timbre 0 keeps D04 at 0" was an inference and it is false. Measured live on
DASP0, D04 peaks at 00F755 for timbre 0. Voice chips run with DCIRC 0 and DOFF 0, so `dmem[4]`
was the right cell after all; only DASP4 has a ring. The replay of `dsp_stream_N.bin` lacked
the chain inputs, so the probes now run inside `ob12_system`.

What D04 is: a scratch divisor, rewritten several times per sample. In one traced sample:
- 03D stores M1: 000061 for timbre 0, 000000 for timbre 1.
- 039 reads C04B into M0 as 00003C, 03C shifts it to 00000F, 040 stores that in D04.
- 046 clears it.

C04B is a mailbox: the host writes it, and 03A overwrites it with 00000F within a sample
(`OB12_WATCH_C=4B`). The program serves one table slot at a time: 020 loads CA from ACC, 44 in
the traced sample and 45 in the first. So the trace covers one slot only.

C04A: the DSP's first pass stored 0 over the loaded 02200E, at 036. Holding that one cell left
the audio identical. The cause was wider, see "Mode change after the primary".

Bus structure, from the programs:
- Each program has two slots, 000-0FF and 100-1FF, and each slot writes and reads the ports.
  That is four bus channels per sample: slot A L/R and slot B L/R.
- prog1 forwards D02/D03 in slot A and D00/D01 in slot B. prog0 clears them instead of reading.
- DASP4 reads slot A into D0D0/D0D1 and slot B into D008/D00C. It never reads AR2.
- Control words run on the AX3 to AR2 chain, D05/D06.

**Fixed: a word must stay in its slot from chip to chip.** A one half frame hop swapped slots
at every chip, so DASP0's D00 landed in DASP1's D02 and no sum formed. OB12_PHASE=1 had been
hiding this. Three notes on channels 0-2, pots moving, MOUT left RMS at 21 s:

| Hop | Phase | MOUT left | Bus |
|---|---|---|---|
| 1 | 0 | 24768 | slots mixed |
| 1 | 1 | 73591 | slots mixed |
| 2 | 0 | 82349 | sum grows 43368 to 224627 in slot B; slot A constant 116 |
| 2 | 1 | 38 | same bus, DASP4 on the wrong slot |

Hop 2, phase 0 are now the defaults: all five chips in step, no phase offset. Hop 2 is chosen
because the firmware's summing bus needs it. Serial timing on the board is not measured.
The earlier "DASP1 and DASP3 output 82" was an artefact of sampling odd half frames only.

**Caveat on this table.** Direct injection is not one note, see "Voice slots". It shows what
each Slave channel does with several notes, not a single-note result.

One note injected per Slave channel, hop 2, phase 0, note at 16 s, pots moving from 14 s:

| Slave channel | DASP4 AX2 L RMS | Sound |
|---|---|---|
| 0, 4 | 14331 | noise-like, flatness 0.84, 16-bit RMS about 82; CMEM written continuously |
| 1, 5 | 140403 | clean sawtooth, 196 Hz, harmonics 1, 0.51, 0.34, 0.26, 0.21 |
| 2, 3, 6 | 0 | silent |

Idle with no note: MOUT exactly 0 for 22 s. Right channel, AOUT and AX2 R stay 0; D01 and D03
were 0 on every chip at this stage. Superseded: the right bus carries signal after the fix in
"Mode change after the primary". The table above was also measured before that fix.

Slot A's constant is not audio. M0 holds 000074 at 1FE and D02 holds the same value; with one
note both are 000090. The `00021002` at 003 stores into D02 rather than clearing it.

## Voice slots (measured)

**The Master path works.** The Master's stream and the direct stream differ only at the note:

    MIDI:   ... 10 5e  94 3c 64  a0 3c 65 35 7f 2a 5f b0 ...
    direct: ... 10 5e  (94 3c 64 injected)  3c 65 35 7f 2a 5f b0 ...

The Master resends status `A0` after a note. My injection does not, so the pot data after it
can read as more notes under running status. That fits DASP1 receiving writes only in direct
runs. It is inferred from the bytes, the Slave's parser was not traced.

Notes sent through the Master with `OB12_MIDI_NOTES`, 0.5 s apart, from 16 s:

| Notes | Result |
|---|---|
| 3C; 37; 2A; 3C at 18.2 s | silent, except 37: RMS 78, 1500 Hz multiples, not studied |
| 3C,3C; 3C,40 | silent |
| 3C,40,43; 3C,35,2A | sound starts at 17.0 s, the third note; noise-like, flatness 0.84 |

So sound needed the third voice of DASP0. It was voice slot, not time: a single note at
18.2 s was also silent. All of this table was measured before the fix below.

The three voice blocks, stride 6B from 10B, are not the same code. Slots 0 and 1 have a stage
at 11C-12B and 187-196, using C1C9/C1CA and cells D04E-50, D05F-61. Slot 2 has none there; its
words at 04A-062 use C1CB.

**Retraction.** "The signal stops at 11C" was wrong. M0 is reloaded there, and 11D stores the
delayed M0 in D09C, so nothing is lost at that word. The real fault was earlier, below.

Secondary 34 as noise, retested with one note on the fixed bus: levels unchanged. Its real
meaning remains unknown.

## Mode change after the primary (firmware evidence, not checked on silicon)

Slot 0 was silent because its parameters were destroyed. Loaded and live CMEM disagreed:
C048 0E999B to 0, C04A 02200E to 0, C053 1D30C2 and C056 0A107F gone, C007 AC0000 to 000007.
`OB12_WATCH_C` showed the writers were the DSP's own words in the first samples, 036, 03A, 06C,
06F, 08D, 090, 0B8, 0BF, storing through CA.

Why CA was wrong. The program keeps a tick counter in D007, 0 to 5, and walks a table per tick:
- 01A: M1 = -(2 x tick) x C007. With C007 = AC0000 that is 1.3125 per tick.
- 01C sets output shift 4 with secondary 29. 01D `23A92027` adds M1 to C027 = 44 and sets
  output shift 0 with the same word.
- 1.3125 x 16 = 21 = 0x15, exactly the stride of the voice tables C045, C05A, C06F.

The core ran the secondary first, so 01D read M1 at shift 0, stepping by 1 instead of 15 hex,
and the walk wrote over its own tables. This order came from the public emulator.

Option `late_mode_change`: secondaries 28-2B act after the primary of the same word. On by
default in `ob12_system`, off in the comparison with the public emulator. Only 29 is proven by
this argument; 28, 2A, 2B are included by analogy.

Measured with it:
- CA base steps 44, 59, 6E. Tables survive: C045 02A00A, C048 0E999B, C04A 02200E, C000-C002.
- Right bus carries signal for the first time: DASP4 AX2 R 258, MOUT right 391 with one note.
- Idle stays exactly 0.

Not right yet:
- Single note 3C, no pots: voice bus reaches 8388114, full scale, within 0.2 s, then sits at
  144. Spectrum shows 6 and 12 kHz. That is a runaway, not an envelope.
- Single note 48, no pots: rises slowly to 5286 and stays.
- With pots moving: one note RMS about 500 on the bus; three notes 171300, MOUT left 86933,
  right 19, flatness 0.65. The sweep sets arbitrary parameters.
- MOUT left and right differ by a large factor; unexplained.

Open:
- Why the voice runs away. First candidates: the guard on 1FC, secondary 3E flag 8, secondary
  34, and panel start values.
- Pitch: note 60 has still not been measured as a stable tone. 196 Hz came from direct runs,
  which may have played other notes.
- Channel table and timbre character must be measured again.

## Tooling and licences

- MAME `src/devices/cpu/f2mc16` includes an `MB90611A` device, BSD-3-Clause. Some
  opcodes only log "Unimplemented" (MULU, DIVU, CALLV and others).
- `Prehistoricman/TMS57070` has no licence file, so its code must not be copied.
  Get permission or write our own core. Local measurement runs only, allowed by Thor;
  nothing from it is in this repo. `tools/tms_harness/harness.cpp` is our own driver and
  needs a local copy of that emulator to build.
- `ioelectro/fujitsu-dev` holds Softune-style MB90 installers and an ISP example, no
  licence shown. Not installed. Possible later source of register headers or a simulator.

## Compare primary and pointer pipeline (firmware evidence, not checked on silicon)

**Superseded.** The "Not right yet" list above: the runaway and the stall were emulation
faults. The full scale values on the bus were control words, 200000 and 7FFExx, not audio.

Two more core changes. Both are on by default in `ob12_system`, off in the core comparison.

`compare_3e`, primary 3E flag 8 compares DMEM with CMEM, flags only. Evidence:
- Each sample selects a voice slot: index = tick, minus 3 from tick 3 on, read from C00B = 3
  or C00C = 0. 091 `3E801A07` tests D07 against C00B, 092 conditionally sets CA1.
- 098/099 then add the index to D0B6 = 11C and load a pointer pair from C11C-C11E.
- Unimplemented, ACCN was stale, the index reached 3-5, and the pair came from C11F-C121.
  Those are not pointers, so later stores went through CA over C003, C00D, C068 and more.
  C004 also holds the tick wrap mask, so D07 stuck at 6.
- With it: pointers 11C, 11D, 11E cycle, D0B6 stays 11C, tables survive.

`queued_pointer_load`, secondaries 04, 05, 06, 08, 09 read their source at once and land two
steps later, the same as C1-C5 immediates. Evidence:
- 127 loads DA1, and 128 must still read D050 through the old DA1: 120-123 and 128-12B are
  the same section, one on D04F and one on D050.
- 11E loads DA0 and must still read D09C through the old DA0. Loading first made the filter
  read itself, so the signal stopped between D09C and D09D.
- Word pairs spaced exactly two apart: 0A7 to 0A9, 099 to 09B, 020 to 022.
- Tried and rejected: acting right after the primary. Secondary 04 then read the new ACC at
  109, which put control words on the bus while idle. Only 04 did that, measured one by one.

Measured with both, no pots, DASP0 cell log and voice bus:
- Idle bus is exactly 0.
- Note 3C: 130.5 Hz, autocorrelation 130.08 Hz, partials 392.5 and 654 Hz. C3 is 130.81.
- Note 48: oscillator cells D046-D053 at 260-262 Hz, an octave up. Key tracking works.
  The bus shows no clear pitch for this note: autocorrelation 0.065, flatness 0.80.
- Note 3C on the bus: flatness 0.77, 41 percent of the energy above 4 kHz.
- With OB12_POT_SWEEP=12: MOUT left 236688, right 190086, falling to 49750, 31489. The old
  left/right imbalance is gone. Bus L/R slots: left 1.9M, right 1.3M without pots.

Open:
- The noise. D0A4, D061, D062 run at full scale at 23938 Hz, and D09E, D050 near 20 kHz, even
  idle with no note. So these filter sections are unstable. Idle output is gated to 0.
  Not proven to be the noise on the bus.
- Why note 3C sounds an octave below 261.6 Hz: patch setting or fault, unknown.
- Secondary 34 still unimplemented; the 1FC zero guard is still a hypothesis.
- AOUT stays 0. The core comparison was not rerun; both options default to off.

## Low MAC word multiply (firmware evidence, not checked on silicon)

**Retracted**: see "Primary 43 keeps the last C" at the end of this file.

**Retraction.** "D0A4, D061, D09E are the noise" was a suspicion and it is false. Period
correlation per cell (`tools/cell_period.py`) showed only D048, D0AD and what they feed were
aperiodic: D048 0.09, D09C 0.08. D0A4 and D061 still run near Nyquist, but they do not reach
the bus with these notes. What they are remains open.

The noise came from primary 43, at 0DD, 154, 1BF, also 0E8 and 166 in program 4. My first
model took the low MAC word as signed and left the product at full scale, so 0DE added two
terms of the wrong size and D0AD jumped between saturated values 725800 and 8DA7E0. Saturation
is off from 0E2, so the error then wrapped into D048.

Two options, on by default in `ob12_system`, off in the core comparison:
- `maclo_unsigned`: the low word has no sign. Bus autocorrelation 0.47 to 0.95.
- `maclo_shift`: the product sits 24 bits lower, as double precision needs. 0.95 to 0.996.

Both are inferred. The shift is the strongest evidence here: D0AD turns smooth and D000 has no
energy above 4 kHz. Program 4 changes with this too, and was not studied separately.

Measured, no pots, voice bus:

| Notes | Pitch | Autocorrelation | Flatness |
|---|---|---|---|
| 3C | 130.5 Hz FFT, 130.79 Hz autocorrelation | 0.996 | 0.062 |
| 48 | 261.5 Hz FFT, 262.30 Hz autocorrelation | 0.998 | 0.002 |
| 3C, 40, 43 | 131, 165, 196 Hz | | 0.038 |

So key tracking and three voices work, and idle stays 0. Left and right slots read the same.

Open:
- Superseded by "Pots are 8 bit". MOUT was not clean. With OB12_POT_SWEEP=12 its flatness is 0.50-0.59, and the sweep sets
  arbitrary parameters, so the cause is not separated from the patch values.
- OB12_FORCE_GAIN puts full scale DC on MOUT while idle. The chord appears at 131, 165, 196 Hz
  but clipped. Those cells are not simply gain, or the value is wrong. Real panel start
  values are still unknown.
- Note 60 sounds at C3, an octave below 261.6 Hz: patch or fault, unknown.
- AOUT stays 0. Secondary 34 unimplemented. The core comparison was not rerun.

## Pots are 8 bit: MOUT was silent because every pot read 0 (measured)

**Retraction.** The harness "mid scale" 0x200 was not mid scale. The firmware uses only the low
byte of the A/D result, so 0x200 read as 0, and the old 10 bit sweep 512 + 480 sin wrapped
through 0-FF again and again. Every result taken with `OB12_POT_SWEEP` before this section,
including "flatness 0.50-0.59", came from wildly jumping pots. `OB12_FORCE_GAIN` is no longer
needed and its DC says nothing about the instrument.

Evidence: all pots at 300 send nothing, all pots at 2A5 send 133 bytes. Rest values 80 and 280
give identical runs, port levels equal to the last digit.

With all pots at 80, now the default, no forced value, MOUT left and right:

| Input | Pitch | Flatness L | Level L, R (24 bit RMS) |
|---|---|---|---|
| none | - | exactly 0 | 0, 0 |
| note 3C | 130.7 Hz, lines 392.7, 654 | 0.005 | 2687803, 1712669 |
| note 48 | 261.4 Hz, line 785 | 0.003 | 1608382, 1069513 |
| notes 3C, 40, 43 | 130.7, 165, 195.7 Hz | 0.034 (R 0.007) | 3601899, 2444735 |

- C0CB reads 03513E at rest 80 and 000000 at rest 0, when MOUT was 0. Whether C0CB alone is the
  cause is not separated.
- The chord reaches full scale on the left: 101 clipped samples of 67200. Right has none.
- L/R ratio is about 1.57. It is already on the bus, see "AOUT and clipping". AOUT stays 0.
- The real resting position of each pot is unknown. 80 is only a mid value.
- The two half frames of MOUT carry the same word.

Pot study, `OB12_POT_STEPS`, one group of 8 pots per ADC channel moved to C0, then back to 80,
note 3C held. Pots send parameter messages such as `A0 07 61`, value = pot / 2. Returning a pot
does not restore the patch: the parameter keeps the pot's value.
- Channels 0, 2, 4-7: flatness at most 0.054 after a move, level changes.
- Channel 1: 0.05-0.08, a line at 260 Hz appears. Not split further.
- Channel 3: 0.40-0.52. Split alone, only two pots matter:
  - mux 7, `A0 07`, writes DASP0 C1C5 and C057 in ramps of 64. Flatness 0.36-0.39, but pitch
    130 Hz with autocorrelation 0.96: a harmonic rich tone, not noise.
  - mux 6, `A0 08`, writes DASP4 C022. At 5F pitch glides 162 to 170 Hz; at 00 about 47 Hz;
    at 40 it returns to 130.8 Hz. Unexplained, though DASP4 carries the LFOs.

So flatness alone misled: a rich periodic tone also scores high. Autocorrelation decides.
- All pots swept, then stopped: periodic, 800 Hz, correlation 0.88. An arbitrary patch.
- All pots moving: correlation 0.08, truly aperiodic. Every parameter changes at once, so this
  does not point to a fault, and it does not prove there is none.

The Master CPU clock runs a little ahead of sample time here, so a pot schedule on CPU time
lands early. `pot_clock` now gives pots the sample time. An earlier one-run 64 pot table was
thrown away for that reason.

Parameter names: the Master has a list at 35EAC in `12cpu152`, starting LFO1 Rate, LFO1 Fade
Time, OscCommon Balance1. Its link to the message numbers is not established.

Open: note 60 sounds at 130.8 Hz (patch or fault), what A0 07 and A0 08 are, channel 1 line at
260 Hz, left clipping, AOUT 0.

## Panel map: pot, message, label (schematics pages 34 and 38 of the PDF, measured)

Labels and mux wiring are read by eye from the rendered Panel A and Panel B potentiometer
sheets. POT n is ADC channel n, mux is POTA. Messages are measured, one pot at a time
(`OB12_POT_STEPS`, 0.5 s apart, to C0). DSP cells are listed only where one pot was measured
alone; an empty cell means not measured. The A8 family and the A0 family are separate lists.

| ADC, mux | Label | Message | DSP cells written |
|---|---|---|---|
| 0, 4 | pitch wheel (by message only) | E0 | |
| 0, 6 | mod wheel (by message only) | B0 01 | |
| 1, 0 | LFO2 Depth | A0 47 | |
| 1, 1 | Keyboard Time | B0 05 | |
| 1, 2 | Osc Common Noise | A0 23 | DASP0 C12B, C128, C126 |
| 1, 3 | LFO2 Rate | A0 4E | DASP4 C02B, C02C, C025 |
| 1, 4 | Osc Common Ring Mod | A0 22 | DASP0 C12A, C128, C126 |
| 1, 5 | LFO1 Fade | A0 4B | |
| 1, 6 | Osc Common Osc2 | A0 21 | DASP0 C128, C126 |
| 1, 7 | LFO1 Rate | A0 49 | |
| 2, 0 | Effects Chorus | A8 17 | |
| 2, 1 | Effects Delay | A8 1D | |
| 2, 2 | Effects Reverb | A8 23 | |
| 2, 3 | Effects Overdrive | A8 12 | |
| 2, 4 | Osc 1 Wave Mix (SLID2) | A0 01 | |
| 2, 5 | Osc Common Attack | A0 24 | |
| 2, 6 | Osc Common Env Depth | A0 1F | |
| 2, 7 | Osc Common Decay | A0 25 | |
| 3, 0 | Osc 1 Wave Mix (SLID1) | A0 02 | DASP0 C0C9, C01A, C014 |
| 3, 1 | Arpeggio Tempo | none | |
| 3, 2 | Phrase Tempo | none | |
| 3, 3 | Osc 1 Wave Mix (SLID0) | A0 03 | DASP0 C01A, C014 |
| 3, 4 | Osc 1 PWM Depth | A0 0B | DASP0 C050 |
| 3, 5 | Osc 1 Wave Ctrl | A0 04, or A0 06 | DASP0 C1B8 with 04 |
| 3, 6 | Osc 1 LFO1 Depth | A0 08 | DASP4 C022 |
| 3, 7 | Osc 1 FM | A0 07 | DASP0 C1C5, C057 |
| 4, 0 | Osc2 Width | A0 14 | |
| 4, 1 | Osc2 PWM Depth | A0 18 | |
| 4, 2 | Osc2 Range | A0 1D | |
| 4, 3 | Osc2 Wave Mix (SLID2) | A0 11 | |
| 4, 4 | Osc2 Fine | A0 1E | |
| 4, 5 | Osc2 Wave Mix (SLID1) | A0 10 | |
| 4, 6 | Osc2 LFO1 Depth | A0 15 | |
| 4, 7 | Osc2 Wave Mix (SLID0) | A0 0F | |
| 5, 0-2 | open on the sheet | none | |
| 5, 3 | Filters Resonance | A0 3F | |
| 5, 4 | Filters Decay | A0 34 | |
| 5, 5 | Filters Frequency | A0 3C | |
| 5, 6 | Filters Attack | A0 33 | |
| 5, 7 | Filters KBD Track | A0 42 | |
| 6, 0 | Equalizer 200/Freq | A8 08 | |
| 6, 1 | Amplifiers Decay | A0 29 | |
| 6, 2 | Amplifiers Attack | A0 28 | |
| 6, 3 | Equalizer 60/Low | A8 07 | |
| 6, 4 | Filters LFO1 Depth | A0 41 | |
| 6, 5 | Filters Sustain | A0 35 | |
| 6, 6 | Filters Env Depth | A0 40 | |
| 6, 7 | Filters Release | A0 36 | |
| 7, 0 | Equalizer 2K/Q | A8 09 | |
| 7, 1 | Equalizer 500/B-C | A8 0A | |
| 7, 2 | Master Volume | F0 7F 30 04 01 00 v F7 | DASP4 C0CB |
| 7, 3 | Equalizer 6K/High | A8 0B | |
| 7, 4 | Amplifier LFO1 Depth | A0 30 | |
| 7, 5 | Amplifiers Sustain | A0 2A | |
| 7, 6 | Amplifier Amount | B0 07 | |
| 7, 7 | Amplifiers Release | A0 2B | |

The running status bytes were read back to their A0, A8 or B0 family from the log order.
Pots at ADC 0 other than the wheels sent nothing and are not on these two sheets.

The sheets and the measurement agree independently: the three open inputs of POT5 and the two
tempo pots send nothing; Keyboard Time is CC 5, portamento; Amount is CC 7; Master Volume is the
one system message. The public Cakewalk file lists 50 controllers with the same panel names; it
gives no link to these A0 numbers.

Answers to the open questions:
- **A0 07 is Osc 1 FM.** That fits the rich periodic tone.
- **A0 08 is Osc 1 LFO1 Depth**, a centre detent pot. So the pitch glide is probably the LFO,
  with 40 as no depth. LFO shape and rate not measured.
- Osc 1 Wave Ctrl sent 04 alone and 06 after other pots moved. The Master list has Saw Spread,
  Triangle Wrap and Pulse Width, so it probably follows the wave mix. Inferred.
- ADC channel 1 holds Ring Mod, which accounts for the 260 Hz line seen when that group moved.

Mixer, measured, note 3C, one slider to FF at 13 s:

| Slider | MOUT left RMS | Flatness | Autocorrelation | Note |
|---|---|---|---|---|
| none | 2290725 | 0.008 | 0.995 | 130.7 Hz |
| Ring Mod | 2867160 | 0.166 | 0.991 | strongest line 261 Hz, as two equal pitches multiplied would give |
| Osc2 | 323182 | 0.019 | 0.994 | 130 Hz |
| Noise, secondary 34 off | 0 | - | - | silent |
| Noise, secondary 34 on | 100440 | 0.832 | 0.069 | true noise |

**Retraction.** "Noise hypothesis tested and rejected: enabling secondary 34 changed nothing"
and the later retest were bad tests: this patch has the Noise slider at 0. Secondary 34 is the
noise source, shown by the rows above. It is now on by default (`OB12_NO_NOISE` turns it off),
and no unimplemented word remains in the run. Idle stays exactly 0; note 3C and the chord are
unchanged. The bit sequence is an xorshift of mine, not the real one, and the noise level is
unverified.

Retracted in "AOUT and clipping": master Volume: pot 40 gives C0CB 0063C2 and RMS 319709; pots 80 and FF both give 03513E, so the
upper half adds nothing here. Why is not known. So the left clipping on the chord happens at
the highest volume.

Tried and not useful: the Master read no name strings when pots moved, so the list at 35EAC is
not used on the normal page. The text probe stays in `OB12_HOST_LOG`.

## AOUT and clipping (measured)

The clipping part is superseded by "Slave PPG tick": it was measured without envelope stages.

**Retraction.** "Pots 80 and FF both give 03513E, so the upper half adds nothing" was wrong. At
rest the pot never moved, so no volume message was sent and C0CB kept its start-up value. The
Master Volume curve, chord 3C, 40, 43, pot moved in steps:

| Pot | Message value | C0CB | MOUT left RMS | Left peak | Clipped samples |
|---|---|---|---|---|---|
| 20 | 0E | 002DA2 | 168041 | 358329 | 0 |
| 40 | 1F | 0063C2 | 411075 | 953698 | 0 |
| 60 | 2F | 009AB9 | 685497 | 1528268 | 0 |
| 7F | 3F | 00D44F | 877727 | 2097084 | 0 |
| A0 | 51 | 011BB1 | 1123329 | 2802184 | 0 |
| FF | 7F | 03513E | 3498107 | 8388531 | 40 |

Clipping:
- The voice bus does not clip: peak 6428567 left, 4378873 right on the same chord.
- DASP4 raises the level by about 1.27 at C0CB 03513E (RMS 3601899 from 2840003), so the left
  channel reaches full scale.
- The harness starts at that highest value, because a pot at rest sends nothing. So the 101
  clipped samples came from running at full Master Volume. No level below FF clipped.
- Whether a real unit clips the same way at full volume is unknown.
- L/R ratio is already on the bus, 1.47 at the peaks, so DASP4 does not cause it. Pan is the
  candidate, not measured.

AOUT:
- The manual, p.74-75: a Part goes to one BUS, MAIN or AUX, and AUX outputs carry only AUX
  Parts. So silence on AOUT is expected when every Part is on MAIN. This setup was not read from
  the patch; it is inferred from the two points below.
- Program 4 sends AX3 from the cells named by pointers C07F, C080. They are 00000B, 00000F in
  the loaded CMEM and no host write changed them. No word addresses D00B or D00F directly, and
  neither cell changed in a run with a note.
- The bus has an empty slot, which reads into D0D0, D0D1 at 003, 005. The note is on the other
  slot, D008, D00C. Four buses per sample fit the manual's four outputs. Inferred.

The path works. Forcing the pointers (`OB12_AOUT_CELLS`):
- 17, 18: AOUT copies MOUT, RMS 2756045 against 2756027, 131.1 Hz.
- D0, D1 with a 440 Hz tone on the empty slot (`OB12_AUX_TONE`): AOUT carries 440.0 Hz, RMS
  707106, flatness 0.000.
That tone did not appear among the strong lines of MOUT. D0D0 is read at 059, 05A; what those
words do with it is not traced.

Not found: which message sets the BUS. Sending `A0`, `A4`, `A8` n 03 for all n, and
`F0 7F 30 04 p 00 40` for p 0-1F, gave no AOUT. Changing each byte of `F0 7E 04` to 03 and
retriggering the note gave none either, but that run lost all sound from byte 20 on, so bytes
20-55 are untested.

Probes added: `OB12_SLAVE_SEQ`, `OB12_SLAVE_FILE`, `OB12_AUX_TONE`, `OB12_AOUT_CELLS`,
`OB12_CELL_WRITERS`.

## Slave PPG tick: voices stopped after 12 notes (measured)

Found while searching for the BUS message. With note-offs, 20 repeated notes gave sound for 12
notes only, then silence for good. The cause was not the note-off: the Slave went back to idle
and its voice table stayed in state 08.

What was missing. FC0300, an open question for a long time, is a PPG interrupt:
- It clears I:30 bit 3 and I:31 bit 3. The datasheet gives 30, 31 as PPGC0, PPGC1, and bit 3 as
  PUF, the underflow flag.
- The firmware masks it by writing 7, then 6, to I:B8. That is ICR08, shared by PPG #0 (vector
  1B) and PPG #1 (vector 1C).
- The firmware never programs the PPG. The resident boot we do not have must do that, as it
  does for the UART. So in the harness the interrupt never ran.

What it does, from the disassembly (`unidasm -arch f2mc16`): a timing wheel of 256 slots at 7A9E
with the position in 7B9E. FC43D7 schedules timer n with a delay in ticks; when one expires the
ISR clears 79DE[n] and sets CB3C, and FC3D34 in the main loop then advances voices. Voice state
is at 8A6C, stride 22, 12 voices: 01 playing, 08 released, 00 free.

Modelled: vector 1B to FC0300, ICR08 = 6, PIE0 set, a tick from sample time, `OB12_SLAVE_TICK`.

Superseded by "PPG tick rate", which measures it. **The tick rate is inferred, not measured.** Default 1 ms. Evidence: one delay at note-on is
24999 ticks, next to a round 25000. With 1 ms the amp envelope stage change lands 0.83 s after
the note, and without the tick the level keeps rising for roughly 1.0-1.2 s, so a true rate
may differ somewhat. Envelope times depend on this value.

Measured with the tick, pots at 80:

| Input | Result |
|---|---|
| Idle | exactly 0 |
| 20 notes, note-off each | every note sounds, voices are reused |
| Note 3C | 130.7 Hz, flatness 0.005, autocorrelation 0.995 |
| Note 48 | 261.4 Hz, flatness 0.003 |
| Chord 3C, 40, 43 | 131, 165, 196 Hz, flatness 0.006 left and right, no clipped samples, peak 8068192 |

**Retraction.** The chord clipping in "AOUT and clipping" was measured without envelope stages:
amp stayed in its attack ramp. With the tick the chord no longer clips at start-up volume.
The volume table there also predates the tick.

Host writes now follow the envelope: at note-on C132 000129, C134 8000E8; at the stage change
C132 000013, C134 800014; at note-off C132 000000, C134 80011C. Meaning inferred, not traced.

**Fixed**, see "CPU clock exactly 16 MHz" below. The CPU clock in the harness runs about 4% ahead of sample time: a note sent at 13.00 s shows
13.52 s on `cycles_total`. Not fixed. The tick and pots use sample time.

## BUS search (measured, not solved)

Solved in "Panel keys, encoder and the BUS message".

Still not found. Tested with the tick on, retriggered notes, AOUT stayed 0 throughout:
- Each byte of `F0 7E 04` set to 0, 1, 2, 3, 4, 5, sound kept. Bytes 42-44 at 1 raise the level.
- `A8` n v for all n, v = 1, 2, 4, 5; for n 40-7F, v = 0B, 0F, 17, 18, 40, 7F.
- No host write ever touched C07F or C080.

A new fact: `A8 49 v` writes DASP4 C07D, the MOUT left pointer, with v itself, and `A8 4A v`
writes C007 the same way. Effect, right channel unchanged in the first three rows:

| v | MOUT left | MOUT right |
|---|---|---|
| 0B | 0 | 1863448 |
| 0F | 0 | 2266303 |
| 17 | 0 | 2301198 |
| 18 | 1652356 | 1652356 |

So the host can repoint outputs. v = 17 being silent is not explained: 17 is the normal value.
A8 4A wrote C007 just after, which may be the reason; the two were not separated.

Next: the BUS is set on an edit page, so pressing the panel keys in the Master (CS4, 0000C0-CF)
and reading what it sends would show the real message. `tools/bus_search.py` and
`tools/byte_test.py` hold the sweeps.

## PPG tick rate (measured against the firmware's own ramps)

Method: with the tick off, a stage is never cut short, so a DSP ramp runs to its end. The Slave
still writes its delay in ticks into the wheel. Move one pot, read the delay, and time the ramp
from DMEM or CMEM per sample (`OB12_CELL_PICK`, `tools/tick_rate.py`). 16 runs.

Which timer serves what, found by moving one pot at a time:

| Timer | Moved by | Ramp |
|---|---|---|
| 0 | LFO1 Fade | D010 |
| 1 | Osc Common Attack | C051, C052, C054 |
| 2 | Filters Attack | not found |
| 3 | Amplifiers Attack | D042 |

| Timer | Delay (ticks) | Ramp end (s) |
|---|---|---|
| 1 | 227 | 0.3290 (C054) |
| 1 | 762 | 0.8383 |
| 1 | 4221 | 4.2970 |
| 0 | 68 | 0.0671 |
| 0 | 166 | 0.1633 |
| 0 | 3097 | 3.1015 |
| 0 | 4221 | 4.2479 |
| 3 | 68 | 0.0897 |
| 3 | 166 | 0.2179 |
| 3 | 762 | 1.0161 |
| 3 | 863 | 1.1490 |
| 3 | 4221 | 5.8002 |

Result:
- Timer 1, between 762 and 4221: **0.99991 ms per tick**, the same on C051 and C054. There is a
  fixed part of about 0.08 s on top, not explained.
- Timer 0: 0.984 to 1.006 ms per tick across the range.
- So the tick is 1 ms. This replaces the weak 24999 argument. It says what the firmware's own
  tables assume, at 48 kHz; the PPG register values themselves stay unknown.
- Timer 3 runs at about 1.33 times the delay. D042 is an exponential approach clipped at
  7FFFFF: at C132 000129 it starts at 297 per sample and slows. So the Slave ends the amp attack
  before the clip, at about 90% of top. Why that point is not known. Also, 1.33 ms is 64 samples:
  that is noted, and nothing supports it as a tick.

Check with the tick on, Common Attack pot A0: the stage change arrives 0.762 s after the note,
as delayed, and the amp one 0.863 s after it.

The note at 13 s here runs with timer 2 at 24999 ticks, 25 s, in this patch.

## Panel keys, encoder and the BUS message (measured)

Keys. Sheets 02/15, 04/15: P30-33 select a button bar through IC4, P20-27 read BUT[7:0], active
low. Modelled, `OB12_KEYS`. The earlier "keyboard at C0-CF" is the musical keyboard. Bars from
the panel sheets: 0 Panel C, 1-4 Panel A, 5-11 Panel B. Used here: bar 10 bit 3 PROGRAM, bar 9
bits 1-4 F1-F4, bar 6 bit 2 and bar 7 bit 2 digits 2 and 3, bar 11 bit 4 ROUTERS, bar 7 bit 3
ENTER.

Encoder. P70, P71 with INT0, INT1 on falling edges, `OB12_ENC`. Direction taken as written, it
stepped the values up, so it works; the real A/B phase is not checked.

Screens without a font. The probe now follows strings through the font reads, as `S` lines in
`OB12_HOST_LOG`. PROGRAM, F4, 2 reaches the BUS page: "MAIN STEREO".

**The BUS message is `A8 27 v`.** Turning the encoder on that page:

| v | Display | MOUT L | MOUT R | Bus slot |
|---|---|---|---|---|
| 00 | MAIN STEREO | sound | sound | odd |
| 01 | MAIN MONO L | 4275862 | 0 | odd, left only |
| 02 | MAIN MONO R | 0 | sound | odd |
| 03 | AUX STEREO | 0 | 0 | even, 1786257, 1360048 |
| 04 | AUX MONO 1 | 0 | 0 | not read |
| 05 | AUX MONO 2 | 0 | 0 | not read |

It applies to the next note, which is why my earlier sweep with a held note saw nothing. It
confirms the empty bus slot is the AUX bus. Rows 01 and 03 measured on notes played after the
message; 04 and 05 only by silent MOUT.

Router pages, from ROUTERS: `A8 0C` router, SERIAL 1, SERIAL 2, CLEAN/SOLO; `A8 0D`; F2 page
`A8 0E 01` AUX MODE COUPLED. With COUPLED, an AUX Part comes out of MOUT again, as the manual
says (p.75).

**AOUT is silent because C0CE is 0.** STANDARD mode, AUX STEREO, note 3C, forced by
`OB12_FORCE_C`:

| Forced | AOUT left RMS | AOUT right RMS |
|---|---|---|
| none | 0 | 0 |
| C0C8 = 7FFFFF | 0 | 0 |
| C0CE = 400000 | 893155 | 680049 |
| C0CE = 7FFFFF | 1786309 | 1360097 |

So C0CE is a linear AUX output level, and the signal is the bus word itself: 1786309 against
1786257. C07F, C080 need no change. This replaces the "pointers parked on empty cells" reading:
D00B is D00A one sample later, because the ring moves, and my writer probe ignored that.

Retracted in "What writes C0CE". Not found: what writes C0CE. It is 0 in the loaded CMEM (C0CB is 03513F there) and no host write
touched it, with AUX active, through: `A8` n for all n at 7F, 40, 01; `F0 7F 30 04 p` for p 0-1F
with values 007F, 047F, 7F7F, 0100. So on a real unit either another message sets it, or a word
of mine is wrong and the value should not depend on C0CE. `OB12_FORCE_C` is a study switch only;
nothing is forced by default.

## What writes C0CE: AUX MODE (measured)

Found by reading the Slave, not by sweeping. The earlier search for `#0x00CE` missed the byte
form `MOV A,#0xCE`. At FDF1A9-FDF1D1 one routine makes two calls to FCA133, the ramped write, with
chip 4: C0C8 and C0CE, one to the constant at FC6706 and the other to 0, chosen by D:D139.

So `A8 0E v` is AUX MODE and sets the pair:
- `A8 0E 00`, STANDARD: C0CE ramps up in steps of 040000 to 7FFFFF. AOUT carries AUX Parts.
- `A8 0E 01`, COUPLED: C0C8 up, C0CE down. AUX Parts come out of MOUT.

My sweeps sent 01, 40, 7F, never 00. In "Panel keys" I wrote that no message reaches C0CE; that
was wrong, and so was the doubt about my DASP4 model. No core change was needed.

End to end through the panel, nothing forced: PROGRAM, F4, 2, encoder 3 steps to AUX STEREO, ESC,
ROUTERS, F2, encoder to COUPLED and back to STANDARD, then notes 3C, 40, 43 over MIDI:

| Output | Left RMS | Right RMS | Flatness | Lines |
|---|---|---|---|---|
| MOUT | exactly 0 | exactly 0 | - | - |
| AOUT | 2514445 | 1679453 | 0.006 | 131.1, 164.7, 195.8 Hz |

Direct `A8 27 03`, `A8 0E 00`, note 3C: AOUT 1771827, 1286908, MOUT 0. A first `A8 0E 00` works
as well, without COUPLED before it, so there is no change check in the way.

Why the panel walk needs COUPLED and back: the page shows STANDARD from the start and the
encoder sends only on a change, while the 835 byte boot stream holds no `A8 0E`. So after boot
in this harness C0CE is 0 although the display says STANDARD. Whether a real unit sends it at
start up is not known: a Master path not reached here may do so.

With C0CE up and no note, AOUT left reads a constant 113, the slot A word 116 seen on the bus
since the two-slot study. Source unknown.

C0C8 and C0CE act on the AUX path in the words 0F1-100. Not traced further. The manual says EQ
does not affect AUX (p.74); that is not measured here.

## Core comparison rerun (measured)

The public emulator's source is no longer on this machine: `core_compare.cpp` needs its header,
and a search of F: and the user folder was stopped before it finished, finding nothing in the
places reached. `build/core_compare.exe` still runs, but it holds the old core.

So the check has two links:
1. `build/core_compare.exe`, public emulator against the core of commit 87d17c3: 5 programs,
   2000 frames, 5.12M steps, failures 0. Run again today.
2. `tools/tms_harness/core_regress.cpp`, that old core against the current one, options off, every
   step: PC, ACC, full MAC, CR0-3, XRD, DA, CA, AX, all DMEM and CMEM. `tools/build_regress.bat`
   takes the old core from git.

| Program | Loaded CMEM | CMEM of a playing unit |
|---|---|---|
| prog0 | 10240000 identical | 10240000 identical |
| prog1 | 10240000 identical | 10240000 identical |
| prog4 | 10240000 identical | 10240000 identical |
| prog5 | 10240000 identical | - |
| prog9 | 10240000 identical | - |

20000 frames each, 71.68M steps in all, failures 0. So with options off the current core still
behaves as the one that matched the public emulator. This is not a direct run against it, and
the link holds only for the steps link 1 covered: link 2 runs ten times longer than link 1 and
with live CMEM, where no public result exists.

Where each option first leaves the plain core, 500 frames, playing CMEM. These are the
differences a PR would have to justify. All are firmware evidence, none is checked on silicon.

| Option | prog0, prog1 | prog4 |
|---|---|---|
| late_mode_change | 016 `2329201F` | 01E `6C282877` |
| queued_pointer_load | 00B `7A045005` | 00F `00043000` |
| compare_3e | 091 `3E801A07` | never |
| maclo_multiply | 0DD `43C81280` | never in 500 frames |
| divide_1fc | 047 `1FCB21E3` in prog0, 046 `1F825004` in prog1 | never |
| noise_generator | 11B `6C341AB0` | 01A `53F410D2` |
| audio_port_writes | 102 `104D7800` | 03C `104C7000` |
| lock_delay | never | 05A `782E10D1` |
| true_dmem_ring | never | never |

"Never" means within 500 frames, one option alone, stand-alone with a test sine, so it does not
mean the option is unused. prog5 and prog9 never differ. Limits of this table: true_dmem_ring
did nothing here, probably because DCIRC stays 0 without program 9 before it, so it was not
exercised; prog4 maclo_multiply did not differ although word 0E8 uses primary 43, reason not
checked; maclo_unsigned and maclo_shift only act with maclo_multiply and were not tried.

## Direct comparison with the public emulator (measured)

`Prehistoricman/TMS57070` cloned at 70357c3 (2026-10-23), the current head, into a scratch folder
outside this repo. It still has no licence file, so nothing of it is committed.
`tools/build_compare.bat <folder>` builds it unmodified; `open_private.h` is forced in only to read
its registers. `core_compare` now takes an `OB12_DUMP` file for live CMEM.

Current core, options off, 20000 frames per case, every step: PC, ACC, MACC upper word, CR1, XRD,
DA, CA, all DMEM and CMEM.

| Program | Loaded CMEM | CMEM of a playing unit |
|---|---|---|
| prog0 | 10240000 identical | 10240000 identical |
| prog1 | 10240000 identical | 10240000 identical |
| prog4 | 10240000 identical | 10240000 identical |
| prog5 | 10240000 identical | - |
| prog9 | 10240000 identical | - |

81.92M steps, failures 0. This replaces the two link argument above: the live CMEM rows now have
a public result behind them.

Limits: the input is a test sine, one voice state from one dump, and `core_compare` reads fewer
fields than `core_regress`. This shows agreement with the public emulator, not with silicon.
All inferred options stay off here, so none of them is tested by this; those are the differences
listed under "Core comparison rerun". The own core still counts 4 unknown encodings in programs
0 and 1 and 2 in program 4 with options off.

Upstream's head has a divide step: 1F8, and a comment that 1FC takes its divisor from an internal
ALU register, the same model as `divide_1fc`.

## PR material (measured, nothing sent)

Four findings are written up in `docs/PR-TMS57070.md`: secondaries 28-2B after the primary, 3E
compare, two-step pointer secondaries, primaries 43-4F. The patch is a local branch of the
clone, exported to `F:\Claude\OB-12-pr\ob12-findings.patch`, outside this repo.

Check: public emulator 70357c3 with the patch, against the own core with `OB12_CMP_PR=1`, which
turns on the six matching options: 81.92M steps identical, loaded and live CMEM, failures 0.
With the patch the own core's unknown encodings fall from 4 to 2 in programs 0 and 1, and from 2
to 1 in program 4.

Kept out of the patch: secondary 34, 0C-0E port writes, the DMEM ring and 2E lock delay, the 1FC
guard. So the patched public emulator alone will not reproduce the OB-12's output.

## Octave: note 60 at 130.8 Hz is the patch (measured)

Not a fault. The pitch word is phase per sample over 2^24: note 3C gives 00B29A = 130.81 Hz,
016534 = 261.62 Hz, 02CA69 = 523.25 Hz. So the Slave's tuning is exact, to the Hz shown here, and
the octave comes from the timbre.

- Osc 1 words are at C046, C05B, C070, with a paired value at C048 that halves as pitch doubles.
  Osc 2 words are at C085, C09A, C0AF. Notes 24, 30, 3C, 48, 54, 60 give Osc 2 words 002C71,
  0058E3, 00B1C7, 01638F, 02C71E, 058E3C: an exact doubling per octave.
- OCT+ and OCT- send `A0 00 v`, starting from the timbre's first dump byte, 3E for the default
  timbre. At the note, v = 40 writes C046 = 02CA69. So `A0 00` is Osc 1 Shift, 40 is the centre by
  its stepping, and each step is one octave. The manual's range is two octaves each way.
- The note number passes the Master unchanged: 94 3C 64 at every shift.
- In the default timbre both oscillators sound near 130 Hz, so a shift of Osc 1 leaves the
  fundamental in place. That is why autocorrelation did not move with OCT+.

Factory timbres stepped with TIMBRE and the encoder, note 3C over MIDI:

| Steps | First dump byte | Osc 1 word | Strongest lines (Hz) |
|---|---|---|---|
| boot default | 3E | not rewritten | 131, 392, 654 |
| 1 | 3E | not rewritten | 131 |
| 2 | 3E | not rewritten | 131, 262, 266, 654, 785 |
| 3 | 3F | 016534 | 262, 264, 523, 528, 785 |
| 4 | 3E | not rewritten | silent in the window, 0.7-1.9 s after the note |
| 5 | 3F | 016534 | 262, 785 |
| 6 or later | 3E | 02CA69 | 131, 392, 653, 915 |

So factory sounds use 3E and 3F, and with 3F note 60 is 261.6 Hz. The boot sound simply is a
3E timbre.

Open:
- Which steps past 5 were reached: the note came at 18.5 s, so runs with 8, 12 and 20 steps all
  show the same timbre.
- That last timbre: first byte 3E and word 523.25 Hz, yet the fundamental is 131 Hz with odd
  lines. Not explained.
- Step 4 being silent: a slow attack is likely, not checked.
- Boot reads "Default Prg" and "Default Tmb". Whether a real unit starts on these is unknown.
- The value of 40 as the panel's zero comes from the stepping, not from a display. If 40 is zero,
  factory timbres sit one or two octaves down, which seems a lot. The font is missing, so the
  display can't confirm it.

## Real time (measured on Ryzen 9 5900X, 12 cores)

Start: 16 s of sound took 53.8 s, 0.30 x real time, one thread.

What changed:
- Five DSPs on five threads, Master on a sixth, Slave on the main thread, with spinning gates.
- Pipeline: the CPU slice of half frame k runs while the DSPs run k-1. The Slave never reads a DSP,
  so this is safe. Its host bytes are held and applied before the chips' next run, in the same
  order as one thread would. Diagnostics that look inside a step fall back to one thread.
- `lean` skips the per instruction statistics of both CPUs. -O3, and no modulo in ring addressing.

| Build | 16 s of sound | Speed | Output |
|---|---|---|---|
| One thread, before | 53.8 s | 0.30 x | reference |
| Five DSP and two CPU threads | 16.9 s | 0.95 x | identical |
| CPUs overlapped with DSPs | 13.6 s | 1.18 x | identical |
| Same, final build, idle machine | 10.5 s | 1.52 x | identical |

"Identical" is the MD5 of the WAV of a three note run: 103e1c0e43f53d2b2255779eacd1011a. Speed
varies with what else runs on the machine: the same code measured between 1.13 and 1.53.

The DSPs are the limit: each chip is busy 60-87% of the time, about 7 us per 256 steps at the
slower measurements. Spinning uses seven threads fully, so this is not yet plugin behaviour.

Live mode, `src/live_io.cpp`, `OB12_LIVE`: MOUT to the sound card by waveOut, which also paces the
emulator, and MIDI in. Boot runs flat out first. Silent test, same timing, 12 notes held:

| Buffering | Underruns |
|---|---|
| 10 ms x 6 = 60 ms, the default | 0 of 2100 |
| 10 ms x 4 = 40 ms | 0 of 1700 |
| 5 ms x 8 = 40 ms | 0 of 3400 |
| 5 ms x 4 = 20 ms | 372 of 4200 |
| 3 ms x 4 = 12 ms | 1737 of 7000 |

Longest test: 34 s of live output, 8 notes, 0 underruns. Not tested: real sound through the
speakers, notes from a real MIDI device, runs longer than a minute, a loaded machine. The 20 ms
failure may be waveOut, not the emulator; WASAPI or a plugin host should tell.

MOUT plays at full Master Volume, as pots at rest send nothing.
﻿
## Thin plugin and player (measured)

Decision: own VST3 on pluginterfaces, Direct2D, no JUCE, no VSTGUI. VSTGUI brings nothing here
that the panel needs. The player is cheap, as it shares engine and view, so it exists too.

- `src/ob12_engine.*`: the instrument as a library. Output equals `ob12_system`, WAV MD5
  103e1c0e43f53d2b2255779eacd1011a, when a note lands on the same millisecond.
- Live inputs, each applied at the next millisecond: MIDI, pots, keys, encoder. `tools/live_api_test`
  offline: Master Volume pot 40 gives RMS 234062 from 2155463, pot FF 2055084; PROGRAM, F4, 2 and
  three encoder steps to AUX STEREO, then a note: MOUT RMS 8.
- `tools/panel_blob.py`: manual page 7 into `rom/panel.ob12p`, 25347 runs with clip paths. The art
  stays out of git. `src/panel_view.cpp` renders it once per size into a bitmap by Direct2D, then
  draws values over it at 30 Hz. The LCD shows the firmware's own screen, without text: no font.
- `src/panel_controls.h`: 53 pots and 79 keys placed by eye. Left to right order of the Wave Mix
  sliders and Wave Select keys is assumed. Panel C, tap tempo keys and LEDs are not there.
- `src/ob12_player.cpp`: `build/ob12_player.exe [MIDI input]`.
- `src/ob12_vst3.cpp`, bundle `build/OB12.vst3`: stereo out, notes in, all channels to the Master.
  48 kHz engine, linear resampling to the host rate, 48 ms ring. ROM from OB12_ROM, a `rom` folder
  beside the DLL, or `F:\Claude\OB-12\rom`.

Tests:
- Steinberg validator, the build from the Emulator II project: 47 passed, 0 failed.
- `tools/vst3_smoke`, a small host at 44.1 kHz, 512 frames, wall clock paced: note sounds within
  40 ms of being sent, 0 of 346 blocks empty from 13 to 17 s. Boot gives about 10 s of silence.

Found on the way: a feeder thread above normal priority made the machine stall for about a second
at a time, spinning on workers it had pushed off their core. All threads now run at normal
priority, and workers stay awake on yield for a while before they sleep. So an idle instance still
uses cores. Timer resolution is raised to 1 ms while a plugin instance lives.

Not tested: a real DAW, the editor inside a host, mouse use of the controls by hand, more than one
instance. Each instance runs seven busy threads. Host MIDI takes a lock in process(), which is
not real time safe yet. No parameters, no state saving, pitch bend and controllers from the host
are not passed on. Measured speed ranged 1.14-1.59 x this round.

## Core speed against the public emulator (measured)

`core_compare`, Ryzen 9 5900X, one thread, both built with the same flags, public emulator at
70357c3, same program and state, best of five rounds of 5M steps, taken in turn. Before each
timing the two cores matched step by step, for 102400 steps per case.

| Program | Own, ns per step | Public, ns per step | Public / own |
|---|---|---|---|
| prog0 | 21.3 | 25.0 | 1.18 |
| prog1 | 20.7 | 24.9 | 1.20 |
| prog4 | 19.8 | 22.8 | 1.15 |
| prog5 | 7.2 | 12.7 | 1.77 |
| prog9 | 7.0 | 12.6 | 1.79 |

With the CMEM of a playing unit: 21.0 against 25.0, 21.2 against 25.6, 20.4 against 23.1. With the
instrument's own options on, the own core takes 19.5-20.4 ns on the same programs, which is
no slower; that row is own work only, as the public core does not do it.

So on the programs that matter the own core is about 15-20% faster, not a different class. The
short programs 5 and 9 only run at start-up. A chip has 10.4 us per half frame: 256 steps take
about 5.2 us with the own core and 6.2 us with the public one, so both would fit.
Not measured: the public emulator inside the instrument, which would also need its missing
behaviours. Timing is a loop over step() with no hooks, so a plugin run adds gate waits on top.

## Font and display (measured)

The sector at FF0000 is still missing, but its format is now read from F0851F, F08565 and F086E9.
`tools/make_font.py` builds a replacement, `rom/font_sector.bin`, loaded at FF0000 if present.

Format: u32 offsets per font at FF0002, relative to FF0002, indexed by font number. From a font's
base: +12 first char, +14 char count, +16 row width in words, +18 height, +1C advance, +1E offset
of a u16 column table, +20 offset of the bitmap. A glyph spans columns table[i] to table[i+1].
Column x is bit 15 - (x & 15) of little endian word x >> 4, by the mask table in RAM at 390C, read
from a RAM dump: 8000 down to 0001.

Glyphs are drawn from DejaVu Sans Mono, 6 x 8, not the original font. All 16 font numbers point at
this one font. Font 6 is passed as a literal; the others come packed from screen data, and which
ones exist is not known: the read counter I tried is polluted by header reads from the blank sector.

With it the firmware's own screens read clearly: PLAY PROGRAM, EDIT PROGRAM MENU, EDIT TIMBRE MENU,
BUS SETTINGS with its drawing of the part and outputs. A set bit is background, so the panel
draws set bits light.

**Correction.** The boot sound is not a "Default" patch: the screen shows program A-001, "08 Fat
Pad", timbre A-106. "Default Prg" and "Default Tmb" are read at boot, but that is not what plays.

The sound changed by a timing shift only, not in kind. With the font the Master spends other time
drawing, so from the first note the WAV differs from the old reference: new MD5
17c551a0a8e1d2eeef7d49160f9b9c1a, the same in `ob12_system` and the engine, RMS 10130 against 10121.
The old hash holds only without `rom/font_sector.bin`.

**Superseded** by "Editor size and display font" below.

Open: selected rows look heavy and some of their letters merge; zero is drawn as an O; the
original font's sizes and shapes are unknown; characters above 7F have no glyphs.

## CPU load (measured)

`tools/pace_test`: feeds a 48 ms ring against the wall clock as the plugin does, 6 notes held,
reports process CPU per second of sound.

| Build | Cores busy | Ring empties | Offline speed |
|---|---|---|---|
| Spinning workers, before | 6.56 | 0 | 1.57 x |
| Workers sleep on WaitOnAddress between bursts | 4.40-4.57 | 0 | 1.57 x |
| Same, with one CPU thread (`OB12_ONE_CPU`) | 3.70, 5.07 | 0, then 633 | 1.21 x |
| Profile guided f2mc16 and tms57070, `tools/build_pgo.bat` | 4.21-4.65 | 0 | 1.96 x |

- Output stays identical, MD5 17c551a0a8e1d2eeef7d49160f9b9c1a.
- Boot falls from about 6.7 s to 4.9 s with profile guidance.
- The one CPU thread variant is not the default: it ran out of margin in one of two runs.
- A shorter spin before sleeping costs more, not less: 2000 gives 4.65, 500 gives 5.52, 150 gives
  5.11, as sleeping and waking cost system calls every half frame.
- Profile guidance made the machine faster but did not lower the load much at real time. The load
  is now mostly waiting inside a burst, not work.

Profiling takes about 10 minutes, as the instrumented run is 0.03 x. It needs rerunning when the
cores change, and `build_master.bat` overwrites the two objects, so it comes after that.

Next, not done: two half frames per gate. Chips read each other two half frames late, so a
batch of two is legal, if host bytes are applied between the two halves to keep the output
identical. Then fewer threads, or a faster step().

## Two half frames per gate (measured)

Chips read each other two half frames late, so the engine now runs two half frames per DSP gate.
Each chip keeps host bytes per half and takes its inputs from the previous batch's port
snapshots, so bytes land between the same two half frames as on one thread.

- Output identical, MD5 17c551a0a8e1d2eeef7d49160f9b9c1a. Offline speed 2.21 x, from 1.96 x.
- Load at real time did not fall: 4.40-4.97 cores in three runs, 0 ring empties, also with one CPU
  thread, which is now safer but still off by default.

Why not: at 2.21 x a chip takes about 18 ns a step. 24.6M steps a second per chip is about 0.45 of
a core, so five chips are about 2.2 cores of real work, by arithmetic, not measured, and the two
CPUs come on top. So the floor is near 3 cores and about 1.5 cores are left to win in waiting.
Lowering the floor needs a faster step(). An idle instance is not measured: the machine runs
all the time, as a real one does.

## Bus sum saturates (found from Thor's recording, inferred, not checked on silicon)

Thor's recording, first preset, 6-7 notes held, resonance up, filter moved: at 96 kHz, every level
flat at +-0.1076, and single sample flips from one rail to the other, 10396 large steps. The
flat tops are expected clipping. The flips are not.

Reproduced: notes 30, 37, 3C, 40, 43, 48, 4C, Resonance pot FF, Frequency pot swept: MOUT left 1902
flips from 15 s on. The same flips sit on the voice bus. Plain 12 notes with no pot moved also
give 159. A chip log showed the cause: DASP1 output 8229948 then -8154229 while DASP0 stays
near full scale, so the running sum across chips overflowed and wrapped.

Why it wraps: each voice adds its output to the bus word with an ACC add, and AOVM, ACC
saturation, is off there. Counting wraps per word showed 21 and A1 adds on the bus: A13B2122,
2186A136, A13B2139, 2186A14D, 210921E2, 21A6B000. Other words wrap hundreds of thousands of
times a second and are meant to: 3C41127F and the like are the oscillators.

Why I call this an emulation fault, though my decoding of secondary 2D follows the public
emulator: a plain 12 note pad should not throw rail to rail spikes on a released instrument, and the
words 6C2D1AA1, 6C2D1AA7 clear AOVM right after these adds, which is pointless if it was already
off. So AOVM may really be on here, through something not understood. Treating 116Dxxxx
as not clearing AOVM removed the flips too, but changed normal sound slightly, up to 2043 of
32768, so it reaches further than the bus. That was dropped.

Option `bus_sum_saturates`: those six words add with saturation. `OB12_BUS_WRAP=1` turns it off.

Measured: 0 flips in both tests on MOUT and bus. The 3 note run is bit identical to before, MD5
17c551a0a8e1d2eeef7d49160f9b9c1a, as nothing overflows there.

Open: the real mechanism. The word list is OB-12 specific and not a chip rule, so it does not
belong in the upstream PR. Hard clipping stays: 12 notes put MOUT left at the rail for 34% of the
time at start-up Master Volume, which may mean voices are hotter here than on a real unit. A single
note peaks near 3.3M of 8.4M.

## Reload timer: portamento, and the effects that never started (measured)

Thor saw program A-018, Timeless LD, timbre B-071, OB Unis ak, play every note at one pitch.
Reproduced by encoder, 10 detents from A-001. Notes 30, 3C, 48, 54 all gave lines 130, 262, 526.

Cause: another missing timer. The note reached the voice, at 8A70. But at note-on the Slave stores
a glide offset at voice +0A, old note minus new note in cents times 64, read back as -2400 cents
for note 54. FCDB10 walks that offset to zero, and it runs only when D:D15A is set. Only FC0400 sets
it: an ISR that clears UF of reload timer 1 (I:3C bit 2), counts D:D148, and sets the flag every
second call. Vector 1E. The firmware never programs the timer, so like the PPG it belongs to the
missing boot, and the ISR never ran. So every note stayed on the pitch of the first.

Fix: `glide_tick` sets D15A every 2 ms. Measured with note-offs: lines 67, 133, 260, 520 for the
four notes. Rate inferred: 1 ms a tick, not calibrated. The glide table at FDE466 runs to 3250
steps, so about 6.5 s at most at this rate.

**The bigger finding.** Running the real timer (`OB12_RELOAD=1`) also advances D148, which a state
machine at FCAC50 and on uses for delays. Then 177 DASP4 cells get host writes instead of 135, 79
differ at the end, the effects get set up, and C0CE rises at start-up, with C0C4, C0C7 going to 0.
So the boot AUX MODE question above has this answer, and **no effect has run in any test so far**:
all sound here has been dry.

With the effects on, MOUT right goes silent, constant -9, left drops to RMS 1290906. AOUT left 141.
The right input is good at D00C and D004. D007 stays near 0 while D003 carries signal, and both
reach the output at 068, 06B. Words between are not traced. XMEM is modelled; the two read
paths may clash, unproven. That is why the default stays glide only: dry and stereo.

**Answered**: not XMEM. See "Primary 43 keeps the last C" below; the real timer is now the default.

New reference: MD5 2a42ebfc79fe39677a8678bf0246c54b, `ob12_system` and the engine agree.

Open: find why the effect path drops the right channel, then turn the real timer on. Rate of the
timer. Slave timer 0 and other vectors are still unset.

## Primary 43 keeps the last C: effects run, right channel back (firmware evidence, not checked on silicon)

**Retraction.** "Low MAC word multiply" above is wrong, and so is finding 4 in `docs/PR-TMS57070.md`.
Its shift and unsigned options made the noise go away, but they did not model anything: they turned
the term nearly to nothing. And "all sound here has been dry" was only half the fault. The timbre
was wrong too: a single note had odd lines only (131, 392 Hz) and peaked at 3.3M, too hot for 12
voices.

Where the right channel was lost: DASP4, words 0E8 `43462937` and 166 `4348288A`, the R lanes after
the L lanes `42002136` and `4200212C`. Same structure, but the L lane multiplies DMEM by CMEM and
the old model made the R lane multiply by the low MAC word.

Evidence for the new model. All five uses of primary 43 in the images, from `tools/scan43.py`:

| Program | Word | Secondary | Word before |
|---|---|---|---|
| 0, 1 | 0DD `43C81280` | 08 | `6C001881` |
| 0, 1 | 154 `43C8128B` | 08 | `6C00188C` |
| 0, 1 | 1BF `43C81296` | 08 | `6C001897` |
| 4 | 0E8 `43462937` | 06 | `42002136` |
| 4 | 166 `4348288A` | 08 | `4200212C` |

Each is paired with a secondary that takes the C bus, and each directly follows a primary that
multiplies by C. So 43 is the form of 42 for when the C bus is busy: DMEM times the C word the
multiplier still holds. Option `reuse_coefficient`, on by default, `OB12_MACLO=1` for the old model.

Which primaries set the held word is not settled by this: latching on 42 only, or on 40, 42, 6C
and 70, gives byte identical WAV for note 3C, with C 1D333A at 0DD and 154, and 26F0BB at 1BF
either way. The code latches on all four.

The real reload timer is now the default too, `OB12_NO_RELOAD=1` for the glide only form.

Measured, MOUT, 14 to 16.5 s:

| Case | Old model, no timer or timer | New model, timer |
|---|---|---|
| Note 3C, left | RMS 2345523 dry, 1244914 with timer | RMS 234275, peak 786929 |
| Note 3C, right | constant -9 with timer | RMS 129655, peak 530329 |
| Note 3C, lines | 131, 392 and odd lines | 130.0, 130.8, 132.4, 260.4, 261.6, 265.2 |
| Notes 3C, 40, 43, L, R | R constant -9 with timer | RMS 314729, 242124 |

Detuned saw pairs instead of an odd line series, which fits Fat Pad. Left and right now differ, as
effects should. 12 notes, no pots: MOUT peaks 2955067, 3146604, bus peaks 1210177, 785738, no
clipping, no wraps. The boot program needs no voice level correction for that any more.

Resonance sweep at maximum, with saturation at the bus adds off: wraps remain at A13B2122, A13B2139,
210921E2, 21A6B000, 213 and 137 flips at MOUT. With saturation on: no flips, top held 73% and 70%
of the time. So `bus_sum_saturates` stays, and the earlier sweep clipping stays unexplained in part.
Timeless LD by encoder still follows the keys: lines 133, 260, 520 after glide.

`ob12_system` and the engine agree, MD5 c27fe2d73360c82764c3284a09f4d5c8 for notes 3C, 40, 43 at
16 s. This replaces the earlier reference.

Open: AOUT left still constant 113. Timer rate is inferred. No recording of a real OB-12 has been
compared, so "fits Fat Pad" is a reading of the spectrum, not a match.

Correction for PR #3: sent 2026-11-07, text in `docs/PR3-CORRECTION.md`. Public emulator with the fix,
against the own core with `OB12_CMP_PR=1`: 81.92M steps identical, loaded and live CMEM, failures 0.
Control, the sent patch against the same core: mismatches at 0DD, 1BF, 0E8, 5 failures. Program 4
with live CMEM did not differ in that control at 2000 frames, so that run does not exercise 43.

## Secondary 2D acts after the primary: the LFOs run (firmware evidence, not checked on silicon)

Thor's report: program A-003 JMJ Journey, timbre A-108 OB Sweep ak. Once the sound settles, Filter
Frequency and KBD Track do nothing, Resonance still works. Thor suspected the cutoff was pinned at
the top. Measured, that was right.

Reproduced with two runs, the same but for one pot at 50 s, compared at equal times so slow
modulation cancels (`tools/ab_centroid.py`), MOUT left, spectral centroid, 52 to 66 s:

| Model | Pot 00 | Pot FF | FF over 00 |
|---|---|---|---|
| Before | 1337 Hz | 1337 Hz | 1.00, FF never higher |
| Now | 472 Hz | 884 Hz | 1.45, FF higher 97% of the time |

The path, followed down:
- The messages arrive. A0 3C reaches the Slave and DASP0 C01A, C014 get written late too.
- DASP0 C0D7 follows the pot, then stays at 1FA4F3, the top of its exponent table, 100% of the time
  from about 4 s after the note. Pot 00 or FF: same value. I read C0D7 as a cutoff term: it moves
  with Frequency and tops out here. Its exact role is not decoded.
- What pushed it there is the LFO term, multiplied up by LFO1 Fade, D010, which reaches 7FFFFF. With
  the LFO frozen near full scale that is a DC offset.
- **No LFO in this emulation has ever moved.** DASP4 AX1, the LFO bus back to DASP0, was identical
  sample for sample from 20 to 60 s, in all 24 slots.
- DASP4 keeps LFO phases at D0D3, D0D7 and on, stride 4. All sat at 7FFFFF. The phase add is 014
  `3C2D7040`, after 013 `116D3100`: saturation on, then the add with secondary 2D turning it off.

Finding: 2D has to act after the primary of its word, like 28-2B, or the phase add saturates instead
of wrapping. Same in DASP4 at 041, 042. Option `late_saturation_mode`, on by default,
`OB12_EARLY_2D=1` for the old order. The public emulator has the old order too, so this is a fifth
finding for PR #3, not sent.

Result: AX1 left swings -8.34M to +8.34M, AX1 right slots 886509 to 4942173. C0D7 on A-003, late:
0AE459 to 13A86C at pot 00, 169A14 to the top at FF, 0 to 31% of the time at the top. KBD Track late,
note 48: centroid 1985 Hz at 00, 389 Hz at FF. Note the direction: FF darkens, not explained.
Resonance late: 225 Hz at 00, 2075 Hz at FF.

**So all sound so far had no LFO at all.** Vibrato, filter sweeps, PWM by LFO, anything on the LFO bus
stood still at one value, and programs with depth got a fixed offset instead.

Notes 3C, 40, 43: MOUT RMS 561180, 440436. 12 notes, no pots: peaks 6226484, 5941738, no clipping, no
flips; that is twice the level before, as LFO now moves levels. Maximum resonance sweep: no flips
with bus saturation, top 72%, 66%; without it 356, 382 flips at the bus, so `bus_sum_saturates`
stays and 2D order does not explain it.

Reference with the new font: MD5 d620434501e83a6b6bb4591adfd94d3f, `ob12_system` and the engine agree.

## Editor size and display font (measured)

Right click on the panel: 50, 67, 75, 100, 125, 150, 200%. The plugin asks the host by resizeView and
falls back to its own resize if the host does not call back. The width is kept in
`%LOCALAPPDATA%\OB12\editor_width.txt`. Smoke test: 750, 1005 with a silent host, 3000, 1500, and a new
view opens at the saved size. Not tried by hand in a host yet. `OB12_BUNDLE_ROOT` builds beside a
bundle a running host has locked.

Font: `tools/make_font.py` now uses a classic 5 x 7 dot matrix set in a 6 x 8 cell for height 8, not
thresholded DejaVu. The blank row and column keep rows and letters apart, zero has its slash, so the
display reads "OB Sweep", which was "08 Sweep" before. 7E and 7F are drawn as arrows, as on
character LCDs; the firmware prints 7E after NEXT. That is inferred. The original font is still
unknown, and characters above 7F still have no glyphs.

**Retracted** by "The CPU rise was the machine" below. CPU, unexplained: pace test now 5.43 cores, 0 ring empties, engine 1.8 x real time. The last round
gave 3.29 cores and 2.35 x. Not the 2D order and not the font: A/B with the same binary gave 1.75 and
1.71, and 1.73 with the old font. Not other load: the machine was idle and the core alone still runs
20 ns a step. A second PGO run gave the same. VSTHost was open during the earlier figure, which may
matter or not; untested.

## The CPU rise was the machine, not the code (measured)

Retracts "CPU, unexplained" above. Nothing in the code made it slower. The machine runs in two
states, and the 5.43 cores came from the slow one.

Same flags, old commit 81dfb3b (where 3.29 cores was measured) beside the current code, run in turn:

| Build | Offline, quiet machine | Pace test, quiet machine |
|---|---|---|
| Plain, old / new | 1.14 x / 1.15 x | 6.9 cores both |
| Fresh profile guided, old | 2.32-2.42 x | 3.39-3.49 cores |
| Fresh profile guided, new | 2.67-2.72 x | 3.06-3.28 cores |
| `build/` as the tools build it | 2.71-2.85 x, 16 runs in a row | 2.91 cores |

So the current code is about 13% faster than 81dfb3b, not slower. Output MD5 d620434501e83a6b6bb4591adfd94d3f
in every build.

The slow state: the same binaries drop to 1.7-1.9 x and 5.1-5.5 cores, all at once, and come back
all at once, once in the middle of a series. Seen the first time while VSTHost was running OB12 at
3.13 cores; VSTHost gained about 390 s of CPU between two of my checks later on, so it was playing
in between. Not caught in the act the second time: a 3 s sampler over 16 fast runs showed nothing
else above 0.15 cores and the clock reading stayed at 3701 MHz. So VSTHost is the likely cause and
not proven for every slow run.

Side finding, profile counters: training runs five DSP threads on shared counters, which lose
25-35% of the increments (step 3.2-3.7G against 4.9G exact with `-fprofile-update=atomic`) and
differ run to run. It does not matter for speed: atomic profiles gave 2.61-2.69 x in the quiet state,
the same as the lossy ones. Not changed in `build_pgo.bat`, as atomic counters gave no gain and make
training slower, 900 s against 750 s.

How to measure from now on: nothing else playing (VSTHost closed or idle), several runs in turn, and
the offline figure next to the pace figure; a pace result alone does not tell machine from code.

In a real host: VSTHost ran one OB12 at 3.13 cores, near the pace figure.

## CPU clock exactly 16 MHz (measured, fixed)

Correction to "The CPU rise was the machine" above: Thor says the slow state was a YouTube video
playing in the background, which he then stopped, not VSTHost. Taken as the cause.

The known 4% excess ("The CPU clock in the harness runs about 4% ahead") had two parts:
- `cpu_device::run` set the budget to the slice each time and dropped the overrun of the last
  instruction, while `tick()` counted that overrun to the timers. So the core ran more cycles than
  it was given, every slice.
- The slice was 167 cycles per half frame, 16.032 MHz. The crystal is 4 MHz and the time-base timer
  counts 4 machine cycles per oscillator tick, so the machine clock is 16 MHz.

Fixed: the overrun is a debt carried into the next run, `executed()` gives the timers what really
ran, and the slice goes 167, 167, 166, 500 cycles per three half frames. `OB12_CLOCK_LOG=1` prints
it at the end of an engine run.

| | Master | Slave |
|---|---|---|
| Before, 16 s of sound | 16.434 s, +2.7% | 16.665 s, +4.2% |
| Now | 16.000021 s | 16.000021 s |

Consequence: the boot takes longer in sample time. The Slave answers at 11.96 s, the first note
sounds if sent at 12.2 s, and a note at 12.0 s is lost; before the fix it just made it. So
`engine_test` now sends its three notes at 13, 13.5, 14 s.

New reference, engine and `OB12_MIDI_NOTES=3c,40,43 ob12_system rom 16 13`: MD5
d2d0db6880111aec99d58203db319e48, byte identical. All three notes sound: 130, 164, 196 Hz.

The real boot time is not known; a 12 s boot is long for an instrument, which may mean something
in the boot still waits longer than it should. Not looked into.

## Amp envelope checked against its cell (measured)

Thor suspects the envelopes. `tools/env_test` plays note 3C at 13 s for a hold time with pots moved
at 12.5 s and prints times from the output level. Amp pots alone, other pots at rest:

| Pot | 00 | FF |
|---|---|---|
| Attack | 90% at 0.12 s | still rising after 3 s |
| Release | -40 dB 0.38 s after note off | still sounding 3 s after |
| Decay, Sustain | output level nearly the same | |

The output level misleads: it carries the filter envelope and the LFOs too. Read at the amp
cell instead, DASP0 D042 per 0.1 s (`OB12_CELL_PICK=42,232,234`), Attack 00:

| Sustain, Decay | D042 after the attack |
|---|---|
| 7F, 00 | 7EFF1A, flat: the top |
| 00, FF | 6FF060 falling to 4F7037 over 3 s |
| 00, 00 | 0 within 0.1 s |
| preset, 00 (Decay pot only) | jumps to 7EFF1A at the stage change |
| preset, 7F | climbs from 6F8224 by about 0x13C0 per 0.1 s |

So Attack, Decay, Sustain and Release all act on the amp, in the right direction. No fault found.
The preset sustain is at the top. The attack still ends near 87% of top, below the sustain, as noted
in "PPG tick rate"; whether the real unit does that is not known. Not changed.

Not checked: the filter envelope. Its attack ramp has no cell yet ("Filters Attack: not found").

## A-004 Canada rings forever (measured, not fixed)

Thor: on A-004 Canada a released note keeps sounding, like an endless release; once the pitch did
not follow new notes either, which does not happen every time.

Reproduced, `tools/seq_test` and `OB12_MIDI_EVENTS` (new, timed note on and off in `ob12_system`):
note 3C on at 14 s, off at 15 s, 43 at 18-19 s. Encoder 2 then 1 detent from boot, display reads
A-004 Canada (timbre slots 2 and 3, A-042 Mono Synth). Three detents at once gave A-005: the encoder
model double steps on a fast turn, not looked into.

Where the sound is (`OB12_CHIP_LOG`, now all ports of all chips, odd half frames):
- The voice bus is right: DASP0-3 AX2 carry 528 Hz while the note is held, fall to about 0 after
  note off, and carry 788 Hz for the next note. In this run the pitch did follow.
- MOUT (DASP4 AX2) holds 524 Hz from the note on at 385000-465000 rms, about 8 times the voice,
  for as long as the run lasts. So new notes are buried under the ring. That may be what sounded
  like a pitch that does not follow; not proven, as Thor says that part comes and goes.
- The Master sends the right bytes: 95 3C 64, 96 48 64, then 95 3C 00, 96 48 00 (channels 5, 6).

The loop: DASP4 runs the same program in A-001 and A-004 (PMEM identical); 156 CMEM words differ.
- 165 `4200212C` multiplies by C12C, the 43 at 166 `4348288A` reuses it for the other channel.
- The Slave ramps C12C in 32 steps to its target: 3D6FD0 in A-001, 7FFFFF in A-004. So 1.0 is the
  program's value, not an overflow seen here.
- `OB12_POKE` (new) after note off at 16 s: C12C = 0 stops the ring at once; 3D6FD0 lets it die in
  about 0.3 s, to 1/1000, so a loop of about 30 ms a pass. So C12C is a gain inside a recirculating
  path, and with 1.0 the loop gain is 1.
- Model options, release ratio MOUT 17-18 s over 14.3-14.9 s: now 1.41; `OB12_EARLY_2D` 1.30;
  `OB12_BUS_WRAP` 1.41; `OB12_NO_NOISE` 1.47; `OB12_MACLO` 0.000; `OB12_EARLY_POINTER` 0.000 (16066
  at 16-17 s); `OB12_EARLY_MODE` 1.005 at a tenth of the level; `OB12_NO_RELOAD` silences effects.

Open: if the firmware means 1.0, the real loop holds a gain below 1 somewhere we hold 1, or C12C is
not in the loop on the real chip. Candidates: what the 43 at 166 really multiplies by, the queued
pointer load (secondaries 06, 08 sit on both 43s and the early pointer load also ends the ring).
Not decided, nothing changed.

### Canada, continued: it is the delay, and its loop runs through LEVEL (measured)

- Panel keys (`seq_test` t:key): Chorus On off after note off stops the ring at once; Delay On and
  Reverb On do not. The chorus key also drives the effect chain order, so this alone did not name
  the effect.
- Sensitivity sweep, each of 116 DASP4 multiply coefficients halved after note off
  (`OB12_CREADS` lists them, `OB12_POKE` halves them): only C0F2, C0FA, C12C end the ring. Output
  gains settle near 0.59. Run serial (`OB12_SERIAL=1`) when many runs go in parallel: six threaded
  runs at once took 39 minutes for 12 runs, spinning workers fight for the cores.
- The Slave sets them from the effects block `F0 7E 04` plus `F0 7F 30 04 04 04 7F` (`OB12_SLAVE_SYSEX`
  injects bytes into A-001). One byte at a time, matched to the display (EDIT DELAY):

| Byte | A-001 | A-004 | Display | DASP4 |
|---|---|---|---|---|
| 30 | 66 | 7F | Delay LEVEL 80 / 100 | C12C 3D6FD0 / 7FFFFF |
| 31 | 7F | 33 | TIME 100 / 40 | C12D, C12E, C12F |
| 32 | 4C | 59 | FEEDBACK +25 / +50 | C0F4, C0F8 = 400000 at +50 |
| 33 | 07 | 00 | HI-DAMP 11K5 / OFF | C0F5/C0F6, C0F9/C0FA (pairs summing to 7FFFFF) |
| 24-28 | | | Chorus LEVEL, RATE, DEPTH, FEEDBACK, PRE-DELAY | |

  Bytes scale display values by 1.27; FEEDBACK is centred on 40.
- So the loop holds LEVEL and the right channel damping, at 1.0 each in Canada, and not FEEDBACK.
  Halving FEEDBACK (C0F4, C0F8) changes nothing, to the last digit, in the current model,
  `OB12_MACLO` and `OB12_EARLY_POINTER` alike; halving LEVEL changes nothing in the two old models.
  A delay's repeats should follow FEEDBACK. So the delay line as modelled is wrong, not one
  coefficient, and the old options only hide the ring.
- A-001, one short note: the level bumps about every 250 ms, not clearly echoes. Not settled.

Next: follow the delay line through XMEM (the 39 reads and writes, their timing) and the right
channel words 165-186, and check that repeats follow FEEDBACK and TIME. Nothing changed in the model.

### Canada, the delay line traced (measured, not fixed)

`OB12_CREADS=t OB12_TRACE4_ALL=1` prints one whole DASP4 sample from PC 0: word, C and D operand,
the D cell after, ACC, the XMEM read address and XRD.

- The delay reads come in pairs, n and n+1 (C09C 006963/006964, C09D 010324/010325): an
  interpolated, modulated tap. TIME sets the read offsets, C12E (L, 006963) and C12F (R, 010324).
  Writes go through a CMEM table walked by CA1: C0EE, C0F0, C0F7, C0FB are XMEM write addresses,
  the cells between them the coefficients (C0F4 FEEDBACK, C0F5/C0F6 and C0F9/C0FA HI-DAMP).
- The core keeps one XMEM read in flight, and a new read cancels it: 167 starts 04BAF, 16C starts
  04BB0 before it lands, so 16E takes a stale XRD (FF1C22, read at 150). Same at 171/176.
- Study option `xread_flush` (`OB12_XREAD_FLUSH=1`): a new read first lands the pending one in XRD.
  Canada after note off: 10731, 659, 231 at 15.1, 15.5, 16 s, the ring is gone. But a short note
  still gives no echo at hundreds of ms in either model, and FEEDBACK 0.25 or 0.88 gives the same
  envelope. So the delay returns no signal of its own; the flush alone is not the fix and stays off.

Next: why the taps never meet the writes: XMEM size and the 12-bit DRAM bus (3 x 44C256, 256K x 12),
`xoff` stepping, and where the read pairs land.

### Canada fixed: DASP4 has 128K words of XMEM, and XMEM reads pipeline (measured, inferred)

The trace with XMEM write indices (`XW`) showed the delay's own writes were never read back: 155
wrote the same index as 10C, another effect, in the same sample, and the delay read at 167 sat
5789 samples behind its write but only 2530 behind another effect's. The buffers overlapped.

Why: DASP4 CR3 is 250000, bus field 2 with XWORD, which the core read as 64K halved to 32K words.
The firmware uses read offsets up to C12F = 010324, beyond 64K. The board has 3 x 44C256 DRAM,
256K x 12 bits, so 128K words of 24 bits in two accesses. In 32K the buffers wrap onto each other
four times. Option `xmem_words`, 0x20000 for DASP4 only (`OB12_XMEM_CR3=1` for the old size).

Second: the core kept one XMEM read in flight and a new read cancelled it. The firmware reads tap
pairs n, n+1 back to back and uses both, which it would not do if the second cancelled the first.
Option `xread_flush`, now on: a new read first lands the pending one in XRD (`OB12_XREAD_CANCEL=1`
for the old model).

Canada after note off, MOUT left rms, FEEDBACK poked after the note:

| FEEDBACK | 15.1 s | 15.5 s | 16 s | 17 s |
|---|---|---|---|---|
| 0 | 74700 | 1812 | 268 | 134 |
| 0.25 | 71031 | 15477 | 1291 | 130 |
| 0.5, Canada's +50 | 73805 | 35249 | 9478 | 1099 |
| 0.88 | 90957 | 88656 | 59066 | 38239 |

With the 128K alone FEEDBACK acts but 0.5 still grows (98420 to 370830); with both it decays, in
order of FEEDBACK. Through the engine: 3C sounds at 524 Hz and is gone by 16.5 s, 43 at 18 s sounds
at 792 Hz and decays, 30 at 22 s at 264 Hz. A-001: all three notes, peak 5354.

New reference d2d0db68 -> 12b99e29202c84dbeeeae1ed2621d50b, engine and `ob12_system` identical.
Every program with delay, reverb or chorus sounds different now: until this round DASP4's
effect buffers overwrote each other.

Not checked on silicon: the 128K is from the board's parts, the pipeline from the firmware's use.
Echo times against the TIME parameter are not measured yet. Superseded: "the old options only hide
the ring" stands; `OB12_MACLO` and `OB12_EARLY_POINTER` are not the fix.

## Output level and resonance (measured)

Thor: most programs sit at -30 to -17 dB in the host, and maximum Resonance on A-001 gets very loud
and clips when a new note comes. The plugin output is MOUT / 2^23, so 1:1 with the DSP word.

Tone on the voice bus into DASP4 (`OB12_AUX_TONE=amplitude OB12_BUS_TONE=1`, the odd slot where
voices arrive; the even slot is the AUX input and stays silent): -20 dBFS peak in gives MOUT -10.3
dBFS peak, -17.1 rms, so DASP4 adds about 6 to 10 dB. Full scale in clips. Master Volume at the
plugin's rest position 80 gives the same as FF.

A-001, note 3C, then 40 and 43 (`OB12_MIDI_EVENTS`, `OB12_CHIP_LOG`):

| | Voice bus, DASP3 AX2 L | MOUT L |
|---|---|---|
| Resonance 80, one note | -24.7 dBFS peak, -34.5 rms | -19.3 peak, -28.3 rms |
| Resonance 80, three notes | -21.8 / -33.0 | -17.0 / -26.6 |
| Resonance FF, one note | -9.0 / -13.0 | 0.0 / -7.1 |
| Resonance FF, three notes | -2.9 / -8.6 | 0.0 / -1.4 |
| Resonance FF, Master Volume 40 | -0.1 / -7.4 | -18.6 / -19.8 |
| Resonance FF, Master Volume 20 | -2.8 / -8.4 | -25.4 / -26.4 |

- Maximum resonance raises the voice bus by 21 dB; the bus itself does not clip.
- The clip is inside DASP4, before Master Volume: with the volume down the peak stays within
  about 1 dB of the rms, a clipped wave made quieter.
- Whether the real unit does the same is not known. A makeup gain at the output would raise
  normal programs and not touch this clip.

### Maximum resonance runs the filter to the rail (measured, cause open)

Thor, from the real unit: on A-001, maximum Resonance at minimum cutoff self-oscillates at about
the level of the plain sound, not far above it.

Here, A-001, cutoff pot 00 (`OB12_POT_STEPS=12.5:5:5:00,12.6:5:3:..`), one note, voice bus rms:
reso 00 -38.1 dBFS, reso FF -13.8 dBFS; at cutoff FF with reso FF -13.4. The level at maximum
resonance does not depend on the cutoff, and at reso 00 the cutoff does not change the level either
(-38.1 at 00, 80, FF).

- The Resonance pot writes one cell, DASP0 C12E: 200000 at 00, 000000 at FF.
- DASP0 trace (`OB12_TRACE_CHIP=0`): 11F `916C9A4F` is a dual word; its paired half `6C801A4F`
  is MAC -= C12E x D4F, the damping term of a state variable filter, the main half loads C12E into
  ACC1. Integrators: D9D and D4F with C0D7, a second section D9E and D50 with C0D8.
- So at FF the firmware makes the first section lossless. Filter states per ms, 0.3-1.0 s after the
  note, peak and rms of full scale: reso 00 D9D 14/4%, D4F 6/3%, D9E 13/5%, D50 4/2%; reso FF D9D
  86/53%, D4F 83/53%, D9E 100/75% (at the rail 30% of the time), D50 100/51%.

The damping step decodes as the firmware means it. What holds a lossless section at a moderate
level on the real chip is not known: saturation or truncation in the integrators, or the second
section. Open, nothing changed.

### The zero damping at Resonance 127 is in the firmware's own table (measured)

C12E per Resonance value (Master byte A0 3F v, with a note held): 0E 178408, 1F 102D50, 2F 0B6049,
51 056234, 61 036436, 69 020000, 72 011AA6, 76 00D90E, 7A 00A6AF, 7C 009211, 7E 008000, 7F 000000.
A smooth exponential, 10.5 steps an octave, then 0 at the last step; continuing the curve would give
about 7800.

Followed back through the Slave (`OB12_DSP_BYTE_LOG`: Slave PC per DSP byte and the sender's reads;
`OB12_SLAVE_WATCH` now covers internal RAM too):
- FC29B9-FC29F1 sends the bytes from EF525A; FC3150 queues it from D:29D8; FD1798 stores D:29D8 from
  the voice block at +0x120 (EFBE7E); FD6FD2 writes that when Resonance changes, from
  `long[FCD910 + 4 x resonance]`.
- The table at FCD910 in 12dsp111: entry 00 20000000 ... 7E 00800000, 7F 00000000; entry 80 is
  other data. So 0 at 127 is the firmware's choice, and the emulation carries it out.

Measured with that: A-001, min cutoff, one note, voice bus rms: 7E -28.3 dBFS, 7F -13.8; 7F with
C12E poked to 7800 -27.9, the level of 7E. So the last step alone is 14 of the 24 dB.

Thor recalls the real unit at Resonance max, min cutoff, near the plain level. If so, the real chip
holds a lossless section down by some means the emulation lacks, since the firmware does not.
Not settled; nothing changed.

Decision (Thor, 2026-11-07): Resonance 127 stays firmware-true, damping 0, no limiter. Revisit only
with a recording of a real unit.

### A real unit's recording: A-002 at Resonance 50 % and 100 % (measured, cause open)

Thor's two takes from a demo video (`F:\OB-12\Brass OI unknown cutoff position Reso 50percent.wav`
and `... Reso 100 percent.wav`, 48 kHz stereo, cutoff position unknown, recording chain unknown,
volume not touched between them). One note, f0 131.3 Hz (C3, note 30). Steady part, left:

| | rms | crest | fundamental's share of the energy |
|---|---|---|---|
| real, Resonance 50 % | -20.1 dBFS | 8.7 dB | -10 dB |
| real, Resonance 100 % | -17.0 | 4.3 | -40 |

So the real unit at 100 % is also lost in the resonance (the note 40 dB down, a strong line at
about 3.1 kHz) and its waveform is squared off, yet it is only 3 dB louder than at 50 %.

Here: A-002, note 30, Filters Frequency pot EC (puts the oscillation at 3.1-3.3 kHz, as the take),
Resonance pot moved through FF first (the pot catches: moved straight to 00 it sends nothing, so the
program's own value stays). Pot FC and above all give C12E 0. Left, 13.2-14.1 s:

| Resonance pot | C12E | rms | crest | fundamental's share |
|---|---|---|---|---|
| 80 | 07D36D | -34.2 | 9.7 | -5 |
| A0 | 056234 | -30.5 | | -8 |
| C0 | 036436 | -25.0 | 8.3 | -13 |
| E0 | | -12.2 | 6.1 | -27 |
| FF | 000000 | -10.1 | 4.8 | -40 |

- Shapes match: the take at 50 % looks like pot C0 here (crest 8.7 against 8.3, fundamental -10
  against -13), the take at 100 % like FF (4.3 against 4.8, -40 against -40).
- Levels do not: C0 to FF is +15 dB here, +3 dB on the real unit. About 12 dB, a factor of 4, two
  bits, is missing somewhere between a resonant filter and a self-oscillating one.
- Not settled: an AGC in the video's recording chain could squeeze the difference too. Thor's earlier
  recollection (maximum resonance near the plain level) points the same way as the takes.
- The Resonance pot is VR21, 10KB (linear) ALPS RK09, between VCC and ground (p38), so 50 % is
  near 80; the real 50 % is more resonant than pot 80 here (fundamental -10 against -5 dB).
- Next: trace the DASP0 filter words at FF (D9D, D4F, D9E, D50) for where the real chip could limit
  the lossless section about 12 dB below the rail; the firmware's coefficients stay as they are.

Thor, 2026-11-15: no AGC. In the same video Resonance 0 notes are much quieter than 15 % ones; those
were cut in apart (the presenter likely turned the volume down when going to 50 %), so only the 50 %
and 100 % takes are a level pair. Their +3 dB stands.

The filter section decoded (`OB12_CREADS=13.6 OB12_TRACE_CHIP=0 OB12_TRACE4_ALL=1 OB12_TRACE_MAC=1`):
- 117-11D: the input D48 x C126 (07FFFF here, 1/16) into MAC 0, stored x16 to D9C: unity.
- 11C-11F: MAC 0 = 0.25 (D9C - D4E) - C12E x D4F (C1C9 = 200000 is 0.25); 120 sets the output shift
  x4; 121 stores it as D9D: hp = (in - lp) - 4 q bp. 121-12B: two integrators each (C0D7, C0D8)
  stored x4. So the section is a state variable filter at unit scale, the 0.25 and x4 cancelling,
  and C12E 0 is lossless, as the firmware means.
- The filter's input runs low: D48 rms 0.032 (C0) against the oscillators' 0.5-0.7 earlier on the
  chip (0C0-0EE). At FF the states reach 0.54 rms and D9E sits at the rail; the output stage at
  151-15C (x16 shift) clips too (D8C, D8D at 7FFFFF), at C0 nothing clips.
- So against the clean sound the rail is about 25 dB up here, about 12 dB less on the unit. A filter
  input 4 times (12 dB) higher would give the unit's +3 dB with everything else as it is, and is the
  same factor as "the dry programs are about 13 dB too quiet" below. Not shown; where a 4 could be
  lost is not found.
- Product-time output shift word by word (`OB12_SHIFT_PRODUCT=1 OB12_SHIFT_PCS=9:pc`) for the 25 DASP0
  words whose read shift differs from their product's, C0 / FF rms (now -25.0 / -10.1): 121 (the hp
  store) -36.4 / -16.5, damping 4 times and no longer reaching the rail, wrong way; 0AE -14.2 / -10.0
  (+4.2 dB, near the unit) but it detunes the oscillator (131.4 to 130.6 Hz) and makes the even
  harmonics lead, so its gain is a side effect; the other eleven on the signal path change nothing.
  No single shift time explains it. Nothing changed in the engine.

Following the voice to the filter (`OB12_CELL_LOG=0,0.5,0.53 OB12_CELL_PICK=...`, per sample):
- Osc 1's output D48 is a saw of about +-0.05 (rms 0.025), the same on notes 24, 30, 3C, 48
  (0.0253, 0.0264, 0.0242, 0.0287): its level does not hang on the pitch. C0C8 is 0.003086 on all
  four (not a per note value, as the FM section above supposed).
- The stage multiplies by C04C (1D333A, 0.228) on the way, a constant of the DSP program's own CMEM
  that no host write touches; 0.228 squared is 0.052, the saw's height. Then x512 (0E3-0E8).
- The filter takes D48 x C126 x16: unity at A-002 (C126 07FFFF).
- After the filter all is linear: filter out (12D) rms 0.136 at C0, 0.738 at FF (14.7 dB), then
  C125, the pan gains and the bus add carry both alike (bus 0.024 and 0.132).
So the level that sets the gap is the filter state against its rail, and both ends come from the
firmware's constants: the oscillator at about -26 dBFS into a unit-scale filter whose states clip
at full scale. On the unit the self-oscillation sits only 3 dB over a resonant note, so either the
unit's filter clips (or loses) about 12 dB lower, or its voice reaches the filter about 12 dB
higher, by a rule of the chip not seen from inside the trace. Open; nothing changed. A same-volume
take of one note at Resonance 0 and at 100 % on a unit would give the resonance gain curve and
settle which.

## Panel: positions from the vector art, LCD as a dot matrix (measured)

Thor: controls sit off the original, the Amp Attack slider seems missing, and the LCD strokes vary
in thickness. He pointed out the art is vector, so every position is in it.

Positions. `rom/panel.ob12p` parsed into figures: 25 slider tracks of 8 x 88, 79 key ellipses of
40 x 20, 48 small circles of 12 x 12 (LEDs, not yet mapped), and 28 knobs, each two stacks of
about 130 gradient polygons of 64 x 56 offset from each other; the knob centre is their midpoint.
`src/panel_controls.h` now takes knob, slider and key centres from these. The cursor pad is one
shape and stays by hand. Before, by eye: the Amp sliders sat 14 to 36 units right, so Attack's bar
fell between Attack and Decay and every bar one track over; EQ and Volume 7 right and 7 low; knobs
15 to 18 from the gradient stacks, a few units from their centres. Slider bars now move over the
track's 88 units, not 104.

LCD. The 240 x 64 bitmap was stretched with nearest neighbour to a non-integer size, so some pixel
columns doubled. Now, as on the EII panel, every LCD pixel is a dot of 86% of its pitch with a gap,
computed at the exact device size by area coverage and drawn 1:1; recomputed only when the
content or size changes.

Checking by eye: the panel saves itself as PNG off screen (hidden command 1999, `OB12_PANEL_SHOT`,
`OB12_PANEL_SHOT_PERCENT`, `OB12_PANEL_BARE` for the art alone), from `vst3_smoke`. A screen copy
was tried first and dropped: it grabbed whatever window was on top.

Not done: LEDs, and knobs drawn turning rather than a pointer over the art.

### Moving caps and knob faces from the art (measured)

Thor: the red marks sit apart from the art's own pointer and cap. The art draws every knob pointer
at -59 degrees (10 o'clock) and every slider cap at the bottom, so a mark at the value never met
them. Now the moving parts come out of the static art and are drawn at the value:

- Slider cap: the runs under the track within +-14 units and from 36 to 92 units below the track
  centre (gradient strips, white handle line, outline, its clip): 350 or so runs per slider. Value 0
  is the art's rest (handle on the bottom tick); full is 88 units up.
- Knob face: the body is the grey (BCBEC0) fill, its box centre the knob centre (this replaces the
  stack midpoint, 3 to 4 units off). The face marks are the runs whose every point lies on the body,
  minus the knurled ring. The pointer angle is the centroid of its dark (6D6E71) fill: -59 degrees.
  Osc2 Range has no body fill; there the knurled ring gives the centre and the light fill the angle.
- Clip pairs go with a part only if everything between push and pop moves. Taking pairs by net
  depth broke the art (an outer pop went missing and every later knob was clipped away).
- Drawn each paint over the cached art with a translate or a rotation about the centre; the red
  marks stay only as a fallback where nothing was found. All 53 found.

Not done: LEDs, the encoder, keys shown pressed.

## LEDs (measured)

The Master drives the LEDs neither through UART0 (0 bytes) nor by the CS2 data byte (always 0).
F0F1D9 shifts each byte out on port 8, bit 5 data, bit 6 clock, LSB first, eight rising edges; the
write to 68FF80+n then loads the shifted byte into latch n. Modelled in the Master (`led_shift`,
`led[16]`), exposed as `Engine::leds`. `OB12_LED_LOG` logs each strobe with its byte.

After boot on A-001: 69 82 44 14 40 00 08 02 48 22 00 40 18 00 00 00. Blinking with no key: latch 5
bit 0 and latch 6 bit 7.

Mapping by pressing each key with a lamp and diffing the latches (`seq_test` t:dump):
Single 12.4, Layer 12.5, Split 12.6, Program 11.6, Timbre 11.5, System 11.4, Panel Scan 11.2, Morph
Auto 10.6, Manual 10.5, Loc 1 10.3, Loc 2 10.2, Motion Rec 10.1, Overdrive 5.6, Chorus 5.5, Delay
5.4, Reverb 5.3, Arp On 5.2, EQ Bypass 9.7. These 18 are in `panel_leds` and light on the panel;
in play mode Single and Program are lit, as on the unit.

Not mapped yet: the other lamps (Osc wave select, Sync, Mono, Portamento and others showed no
clear bit in one press each), the small indicator LEDs (LP/BP/HP, routing, LFO waves, octave,
arpeggio mode and range, and so on), and the two blinking bits. The 48 circles of 12 x 12 in the
art are the key lamps; the indicators are smaller circles, not yet listed.

### LED schematic, Panel A (read from the service manual, p36 "OB12 Panel A: Leds", p35 buttons)

Page index of the PDF (no text layer, read as images): p2-17 main board (p3 Master CPU, p5 peripherals
interface), p18-32 the same again, p33 Panel A connector, p34-36 Panel A, p37 Panel B connector,
p38-40 Panel B, p41 Panel C connector, p43-48 PCB layouts.

Panel A LEDs: six 74HC595 (IC6, IC7, IC2, IC9, IC8, IC4), SCK = LEDCK and SI = LEDD shared, each RCK
its own LED[n], n = 1..6. That is the port 8 shift (bit 6 clock, bit 5 data) and the strobe at
68FF80+n: latch n is LED[n]. The first bit shifted ends on Q7, so Q(k) is bit (7-k) of the byte the
firmware sends; `led[n]` holds that byte.

| RCK | IC | outputs | group on the sheet |
|---|---|---|---|
| LED[1] | IC6 | ONT0-7 | LFO2: DL17, DL18, DL24, DL29, DL19, DL20, DL25, DL30 |
| LED[2] | IC7 | ON8-15 | KEYBOARD: DL13, DL23, DL31, DL36, DL37, DL38, DL32, DL39 |
| LED[3] | IC2 | ONT16-23 | WAVE SELECT, ENV DEPT, KEYBOARD: DL21, DL22, DL1-DL6 |
| LED[4] | IC9 | ON24-31 | ARPEGGIO, PHRASE: DL27, DL28, DL45, DL46, DL47, DL33, DL34, DL35 |
| LED[5] | IC8 | ON32-39 | EFFECTS, ARPEGGIO, PHRASE: DL40-42, DL14, DL43, DL15, DL26, DL44 |
| LED[6] | IC4 | ON40-46 | OSC1, PHRASE: DL10-12, DL16, DL7-DL9 |

The Q to ON order differs per chip (IC7: Q0 ON8, Q1 ONT10, Q2 ONT12, ... Q6 ON9), to be read per
output. Which DL sits where on the panel is on the PCB layouts, not yet read. Panel B (p38-40) and C
(p41) not yet read.

Panel A buttons (p35), bar BUTB[1]: BUT0 LFO1/LFO2/EG (PWM source), BUT1-3 Osc1 Wave Select, BUT4
Osc Common, BUT5 Effects Level/Param, BUT6 Keyboard Mono, BUT7 LFO1 Wave Select. BUTB[2]: Phrase
On/Off, Arpeggio Hold, Portamento, Legato, Unison, Phrase Rec. BUTB[3]: LFO2 Depth Select, Keyboard
OCT-, OSC Shift, OCT+, LFO2 Wave Select. BUTB[4]: Arpeggio Mode, Arpeggio On/Off, Reverb, Delay,
Overdrive, Chorus, Arpeggio Range. Tap tempos on BTIME[0], BTIME[1].

### All 89 LEDs mapped from the schematic and layouts (measured against the latches)

Chain: latch n (strobe 68FF80+n) = LED[n] on the sheets; Q(k) = bit (7-k); Q to ON per chip from
p36 (Panel A, LED[1..6]) and p40 (Panel B, LED[7..12]); ON to DL on the same sheets; DL to its
place from the PCB layouts p46 (Panel A, 192521) and p47 (Panel B, 192522), by the key P number it
sits beside or the row it is in. The art has 46 lamps (12 x 12 circles in keys) and 43 indicators
(A7A9AC fills of 6 x 8 beside labels); every one has a bit. Table in `src/panel_controls.h`.

Checks against the earlier key presses, all agree: Overdrive/Chorus/Delay/Reverb/Arp On = latch 5
bits 6..2; Program/Timbre/System/Panel Scan = latch 11 bits 6, 5, 4, 2; Split/Layer/Single = latch
12 bits 6, 5, 4; Auto/Manual/Enable and Loc 1/Loc 2/Rec = latch 10 bits 6..1; EQ Bypass = 9.7.
The two bits that blink alone are the tap tempo lamps: 5.0 Arpeggio (DL15), 6.7 Phrase (DL16).
At rest on A-001 Part 1 lights with Single, LFO1 wave 2, Filter 1 and 2 HP, Routing Serial and so on.

Order within a group is from the layout where the DLs sit in a row (LFO1 waves DL1-4 left to
right, LFO2 waves DL17, 18, 24, 29 top down, filters DL8-10 and DL11-13 left to right, LP BP HP).
The filter wiring is not monotonic (ONT8 DL9, ONT9 DL8), so Filter 1 LP is bit 2 and BP bit 1.
Not checked on a unit. Panel C (wheels, keyboard sense) is not drawn on the panel art.

Font note (Thor): the large program name on the display is Handel Gothic; file
E:/dl/handelgotdlig-regular.zip. To use when the display font is reworked.

## A-014 The Raver clicks (measured, cause open)

Thor: The Raver clicks, and its slide sounds strange. Selected with 13 single encoder detents 0.35 s
apart (display: A-014 The Raver, timbre A-113); it plays on DASP1. Notes 3C at 18 s, 48 legato at
19 s, 3C legato at 20.5 s (`OB12_SERIAL`, `OB12_CHIP_LOG`).

- No glide on the voice bus: 133 Hz goes to 265 Hz at 19.04 s and back at 20.6 s at once (pitch per
  15 ms by autocorrelation, DASP3 AX2).
- The clicks are steps in the waveform, on the voice bus already, so not the effects: high band
  bursts (above 6 kHz, 1 ms windows) at 18.861, 18.877, 18.892, 18.907 (about 15 ms apart) and
  21.345, 21.379, 21.409, 21.438 (about 30 ms apart). At each the wave drops at once.
- No host write to DASP1 within 3 ms of any of them; so the step comes from inside the DSPs, a
  modulator. The LFO bus is time multiplexed (a value change every 3 ms in every slot), so a plain
  jump count says nothing; one slot has to be followed.
- Note-on writes a 25 ms burst to C070/C072, C085/C087, C09A/C09C, C0AF/C0B1 (22 values each); their
  role is not known.

Next: follow the LFO slot that DASP1 reads and the cell it modulates; check whether an LFO wrap or
step lands on the click times. Thor also hears LFO on programs that should have none; the same
trace answers that.

### The Raver click located: the new voice's start on a legato change (measured)

Thor marked the click in the rendered WAV at sample 72 800-72 950 of the clip: 19.017 s in the run,
the legato change 3C to 48. In his view: a sudden drop, a short burst of fast zig-zag, a spike up.

DASP1 per sample (`OB12_CELL_LOG=1,0.995,1.05` with the note time at 18):
- The cells that move most are a voice that was idle until 19.0164 s: D1F, D1E, D16, D20, D45, D44.
- D45 is that voice's waveform: a clean saw from about 19.022 s, with two spikes before it, about
  2 and 4 ms after the start.
- At the same moments D16 and D1E rise in steps and fall back, D20 rises in steps to the rail
  (8388608) and stays there, D1F dips negative and returns. The steps keep the rhythm of the
  Slave's note-start burst (C070/C072, C085/C087, C09A/C09C, C0AF/C0B1, a pair every 0.9 ms for
  25 ms).
- Saturating the rare wrapping words on DASP1 (A6727140 at 1D9, 26425AB5 at 079, 26403140 at 082)
  removes their wraps but not the click; reverted.

Open: which words write D16, D1E, D20, D1F, whether those cells are meant to reach the rail, and
how the voice output D45 uses them. `OB12_WATCH_D` lists the writer PCs of DMEM cells on DASP0;
it needs a chip choice for DASP1.

### The Raver click: the writers (measured)

DASP1 traced one sample at 19.0170 and 19.0185 s (`OB12_TRACE_CHIP=1 OB12_TRACE4_ALL=1 OB12_CREADS=t`):
- 01B `79023A40` writes D1E from the MAC (C03A FF3412); 01E `79023A40` writes D1F (C03C 476BE0);
  021 `00023A00` writes D20 from the MAC (C03D C76BE0, C03E E00000; 01C reads C03B 811041).
- The coefficients read as a second order filter on a control value: 476BE0 = 0.558, C76BE0 =
  -0.442, 811041 = -0.992, E00000 = -0.25. D1E, D1F, D20 are its states, D20 its output.
- D20 goes on to 02E-038 with C0B0-C0B3, the block the Slave writes in its note-start burst
  (C0B1 049800 at 19.0170, 031800 at 19.0185).
- D20: 7B1D1B at 19.0170, 7FFFED/7FFFF7 at 19.0185, at the rail from then on.

Open: the input of this filter at a voice start, and whether D20 is meant to reach the rail; a
different overflow rule on the real chip (unsigned, or wrap where the model saturates) is the
candidate. Nothing changed.

### The Raver click: a control stage only this program turns on (measured)

The block 018-021: `79` loads a constant into the MAC upper word (core: mac_set_upper, lower 0), the
following `6C` add coefficient x state; so y = constant + sum of products, stored by secondary 02.
Input D34 = 0B7504 (0.089) at the voice start.

The coefficients are set per program. A-001 and A-004 on DASP0: C03A 0, C03B 0, C03C 2AAAAA,
C03D 0, C03E E00000, so D20 is a constant one third and the stage does nothing. The Raver on DASP1:
C03A FF3412, C03B 811041, C03C 476BE0, C03D C76BE0, an active two pole stage. A rough DC gain,
(0.558 - 0.442) / (1 - 0.992), is about 15; times 0.089 that is above full scale, so in the model
the stage runs into the rail at every voice start, and the click is its states hitting the limits.

Not settled: whether the real chip saturates here too (then the click is real) or the arithmetic
differs (the alignment of the `79` constant against the products, or the MAC shift mode). The
stage is probably what Thor hears as the strange slide. Next: the role of C03A-C03E from the Slave
side (which timbre parameter writes them), and the `79` alignment checked against another
program that uses the stage without clicking.

### The Raver: the stage is its glide, and the first write is a transient (measured)

Host writes to DASP1 C03A-C03E (`OB12_HOST_LOG`): at the voice start 19.0163 C03A = A2AE92 (-0.72),
19.0165 C03C 476BE0 and C03D C76BE0, 19.0169 C03A FF3412 and C03B 811041; then C03A alone in small
steps (FF3181 at 19.0286, FF2CA6 at 19.0390, ...); at the release 20.6411 C03C and C03D to 0. The
constant term carries the slide and the coefficients its speed: the program's glide.

For the 0.6 ms between the first and the second write the stage runs with the constant -0.72 and
the old coefficients; that drives it into the rail, and the click follows. Open: whether the real
DSP sees these writes in the same order and spacing (the host interface timing), or the Slave on
the unit writes them closer together. Nothing changed.

Thor: with portamento off The Raver does not click. So the click is the glide stage.

Slave side: the bytes 3A A2 AE 92 go to DASP1 through the generic sender FC29B9-FC29F1 (byte 7751 of
8169 in `OB12_DSP_BYTE_LOG`), but the entry does not pass the D:29D6/D:29D8 path watched for
Resonance; that queue is shared by all chips (its C03A/C03B/C03D entries at 19.005-19.014 belong to
other chips). The routine that queues the glide start is not found yet. Next: watch the queue
buffer itself (where FC29B7 reads @RW0) for the A2AE92 entry and its writer.

### The Raver: where A2AE92 comes from (measured, reading inferred)

`OB12_SLAVE_WATCH=100,f00000 OB12_SLAVE_WATCH_FROM=19.0` (new: watch from a time on): the routine at
FC2BAF-FC2C2B builds A2AE92 on the stack (EFFFA7-EFFFAA) at 19.0159-19.0160 and sends it at once at
19.0163 through the sender code (FC29D5-FC29F1), not through the queue.

Reading, inferred: 3C to 48 is one octave, and A2AE92 = -0.72 has that size, so this is the glide's
start offset. C03A then seems to serve twice: for a moment as the offset that loads the stage's
state, then as the small rate constant (FF3412 from 19.0169). With the offset in place for one sample
the state would be -0.72 and fine; in the model it stays 0.6 ms, about 29 samples, and the stage adds
it to itself each sample into the rail. Open: how long the offset lives in C03A on the unit (the
Slave's time to the next write, the DSP host interface latency), and the stage's exact equations.
Not fixed.

### The Raver: the glide start offset is lost by one sample (measured)

Stage equations, from the per-sample cells (the 6C words negate): D1F = C03A + 0.9917 x D1F (the
glide; with C03B = 0 it copies C03A, which is how the start is loaded) and D20 = C03C + 0.442 x D20,
whose steady value is exactly 1.0, so D20 at the rail is by design.

Sample by sample at the legato change:
- 19.0163: C03A = -0.729, C03B still 0, so D1F = -0.729: the start offset loads.
- 19.01688: C03A = -0.006 (the rate constant) while C03B is still 0 for one sample: D1F = -0.006.
  The offset is lost.
- 19.01690: C03B = -0.992 arrives; the glide runs smoothly from -0.006 instead of -0.729.

So the click is the step -0.729 to -0.006 in one sample, and the glide runs from the wrong start:
Thor's unnatural slide. The firmware writes C03A and C03B back to back, so it means them to take
effect together. In the model they land one sample apart. Candidates: the f2mc16 core's instruction
cycle counts (MAME tables, approximations) make the Slave slower than the unit, or the DSP host
interface takes writes in at another point. The earlier 29-sample theory is retracted: with C03B 0
the stage copies C03A and does not accumulate it. Not fixed.

### The Raver: a race in the firmware's write order (measured, decision open)

Host writes timed to the microsecond (`OB12_SERIAL`, host log at 6 decimals, a study change since
reverted): the Slave's word writes land one per half frame, about 10.4 us apart, as one five byte
word takes it about 160 cycles. C03A at 19.016875, C03B at 19.016885. The DSP reads them back to
back (01B, 01C) in the first half of each sample. So a pair whose C03A lands at a sample's start
loses the offset; one whose C03A lands mid-sample keeps it.

Eight legato changes on The Raver, four with a glide pair: the offset was lost at 19.017 and 23.396 s
(C03A in a first half), kept at 20.476 and 21.936 s (second half). So the click comes about every
second legato change, as Thor heard: not every time.

Neither order is safe: C03A first loses the offset; C03B first drives the state to -1.45 for a sample.
On the unit either the same race clicks at random, or the TMS57070 host interface takes words in a
way that hides it (buffering a group and taking it in at once). The "TMS57070 basic info" document
has no detail on the host interface. Not changed; a decision is open between staying firmware-true
and modelling a take-in that applies a Slave burst together.

Thor: with portamento on it clicks every time, legato or not, poly or mono; off, never. A non-legato
note (3C released, 48 at 19 s) lands on DASP0 and shows the same writes for three stages:
19.0144 C03A A2AE92, then 19.0148-19.0149 C01B 811041, C02B 811041, C03A FF3412, C03B 811041
(C01A/C01B, C02A/C02B, C03A/C03B: three glide stages). Each has the race; any one losing its offset
clicks, so with three of them nearly every note does. With portamento off these stages do not run.
That the real unit does not click (as Thor reports for the program's behaviour) points to the chip
taking a stage's A and B words in together.

### The paired take-in, tried and reverted (measured)

01b8766 held each host word one slot and let A, A+1 land together. In `ob12_system` the glide kept its
offset and the voice bus jump at 19.0 s fell from 0.091 to 0.029. But the engine (`seq_test`, the
plugin path) got worse: at the same legato change three single sample spikes up to 21132 against
8744 before, on MOUT. Reverted in 5c9b153; the reference is 12b99e29 again.

Not understood: why the engine and the harness differ with the slot. Candidates: the engine applies
a part's held bytes before the slot ages, so the half frame count differs; or a pair other than the
glide stages now lands together and breaks a sequence the Slave means as two steps. Next time: an
engine and harness run side by side with the slot, per sample at 19.01-19.03 s, before any fix.

### Font 1 is the program name, drawn in Handel Gothic (measured, from Thor's find)

Which font number draws what: four sectors, each making one bit group of font numbers 16 rows high
(`make_font.py` heights). Only bit 0 enlarged the program name, and nothing else; bits 1-3 left it
small. So font 1 is the program name and only that, and 16 rows fill the space the screen leaves
for it between the header and the part box.

Thor identified the face from a photo of a unit: Handel Gothic (HandelGotDLig Regular, freefonts).
`make_font.py` now takes a height with p for a proportional font from OB12_FONT_TTF: capitals over
rows 1 to 12, descenders below, one blank column after each glyph, each glyph its own width in
the column table. Build:
`OB12_FONT_TTF="rom/fonts/HandelGotDLig Regular.ttf" python tools/make_font.py rom/font_sector.bin 8,16p,8,8,8,8,8,8,8,8,8,8,8,8,8,8`
(the TTF lives in rom/fonts, outside git like the ROMs). Copy font_sector.bin into each bundle's rom.

Audio reference unchanged, 12b99e29. Stroke weight against the photo not compared closely.

### Lamps that stay dark: the firmware's own image lacks them (measured)

Thor: Mono, Legato, Portamento, Unison and others never light. The Master builds each latch byte
from RAM: EC5B09 + latch normally, EC5AFC + latch when EC5B30 is non zero (F0F1A6). The LED log now
prints both images and the flag at each strobe. Pressing Mono, Legato, Portamento, Unison
(`OB12_KEYS` bars 1 and 2): the Master sends the MIDI change (Mono: B0 7E 01), rewrites latches 2 and
3, but both images stay 44 and 14, equal to the bytes shifted out, and EC5B30 stays 00. So the shift
model is right and the firmware does not set these lamps. Open: which Master routine sets the lamp
image after a key press, and why it does not run or does not see the change here.

Retracted: "the firmware does not set these lamps". It does; our CPU core lost the writes. The lamp
dispatcher F0E983 stores the latch index (Mono: 02) at ECB7BA, then jumps with `F0E9DC: 05 61`
(DTB prefix, JMP @A). The core kept the prefix valid after JMP @A, because only memory-using
instructions cleared it, so the handler's first `MOVW A,@RW3-2` (F0EA1E) read from the DTB bank and
got latch 0. A bank prefix covers only the next instruction: the core now drops it after every
instruction that is not itself a prefix (src/f2mc16/f2mc16.cpp, end of execute_run).
Measured after the fix: the lamp image writes reach latches 2, 3, 5, 6, 7 (EC5B0B..EC5B10) on the
key presses; A-001 opens with the Osc 1 and Osc 2 saw, Chorus, Delay, Reverb, Level, Part 1,
Single, Program lamps and the filter and routing indicators lit. Audio reference unchanged:
engine_test rom 16 MD5 12b99e29202c84dbeeeae1ed2621d50b (the Slave never hit the case).


### Host bursts: the DSP takes a select's words in together, at its next sample (measured, inferred)

Thor: A-018 (mono lead) pops on every note; on a unit in a video it does not. A-014 clicks on the
unit too, so that one is the firmware's.

Measured on A-018 (`OB12_MIDI_EVENTS=12:pc:11,13:on:3c,...`, host log in microseconds with the chip
select edges): single sample spikes up to 8386 at note starts, each where the Slave writes an
oscillator's coupled words (C046/C048, C05B/C05D, C070/C072, C085/C087, C09A/C09C, C0AF/C0B1, rate
and its partner) and the glide stages (C0x4/C0x5, C0xA/C0xB). Taken in one by one, about 10 us
apart, a DSP sample between the two halves of a pair runs one sample with a mismatched pair.

The Slave's write loop (FC29B9-FC29F1) is unrolled, five bytes per word, with no wait on the DSP, so
the pacing is CPU speed alone. But it sends at most 16 words per chip select (03, then 16 address
and value words, select released, about 220 us per burst). Sixteen is the TMS57002's update ring
size, and MAME's 57002 notes that a write "shouldn't really happen until CLOAD is high". So the
model now holds a select's update words and takes them in together at the chip's next sample start
after the select is released (engine and ob12_system alike; `OB12_HOST_SINGLE=1` is the old model).

Results (inferred, not checked on a unit):
- A-018 harness: largest spikes 8386, 7565 at 14.016 s gone; worst now 2931, on the first note after
  the program change (13.018). Engine: the pops at 14.22 and 17.012 s gone.
- The Raver: the note start spike at 13.015 s gone; the first legato change (14.02) stays rough,
  as the unit's video also clicks.
- New reference, engine = `OB12_MIDI_NOTES=3c,40,43 ob12_system rom 16 13`: MD5
  3c2d15f9399f304600b874fa530738e1.

Supersedes "The paired take-in, tried and reverted": that held every word a half frame by address;
this follows the select, which the firmware itself frames.
### Program loudness: Overdrive adds about 18 dB (measured, cause open)

Thor: some programs, A-015 most, are far louder than on a unit in a video. One C3 per program,
A-001 to A-032, engine (`seq_test`, 0.2-1.4 s after note on), rms against the median 786:
most within +-6 dB; A-015 +17.4, A-022 +13.6; A-012, A-023 silent on C3, A-026 -22.
The Overdrive lamp (latch 5 bit 6) after each program change is on for A-015 only. A-015 with its
Overdrive key (bar 4 bit 5) pressed off: rms 615 against 5008, peak 2185 against 14409. So our
overdrive raises the level by about 18 dB; a unit's overdrive presumably keeps it near the dry
level. Open: which chip runs it, what the firmware writes for it, and whether a missing
saturation or level step in the TMS57070 model makes the difference. A-022 (+13.6, no overdrive)
not looked at.

### Aftertouch, the manual and Thor's log (measured)

The operating manual (ob12.clearalgo.com, 7.13) has the KEYBOARD SENSE AFTERTOUCH button
enabling aftertouch, and per timbre AFTER-T settings; the MIDI IN filter has an AFTERT. switch.
In our machine it is on: A-002 with D0 7F moves the spectral centroid from about 2000 to 3300 Hz.
Thor's VSTHost log: his keyboard's pressure reached 06 of 127 in that take, as D1 and D0 both
(two channels). Panel C (KEYBOARD SENSE, wheels) is still not drawn; its keys are bar 0.

Bulk dump reload (manual 10.4): the dump comes back through MIDI IN and runs the UPDATING
PROCEDURE, flash writes and a reset, so a .syx loader needs the flash kept between sessions and
the reset modelled.

### Overdrive on DASP4: where the 18 dB comes from (measured, not fixed)

The Overdrive key sends `A8 0F 00`; the Slave then crossfades DASP4 C0EB (dry, 0 to 7FFFFF) against
C0EA (wet, 7FFFFF to 0) in 1.3 ms steps. `OB12_PC_RMS=4,13.5,14.5` (new study switch: rms per program
address of the D cell after the word, and of both accumulators) on A-015, overdrive on / off:
- dry input of the mix (139, `6C003100`): 0.014 either way, about -37 dBFS;
- the drive stage 118-122 (`2BC02132` sets CR1 bits 9-11, `59402133` accumulates with C133 = 000001,
  `2B002132` restores) leaves the accumulators at rms 0.70, a near square wave, independent of input;
- after the tone filters (123-131) and C0E8 = 0.40 (133): wet 0.133, ten times the dry 0.014.
Master Volume does not explain it: its maximum (`F0 7F 30 04 01 00 7F F7`) writes C0CB = 03513E, the
power on value, and C0CB is applied at 0F8, before the overdrive. So in the firmware's design the dry
path runs at about -32 dB there and the fuzz runs at full scale. Retracted: "a clip made from the rounding mode". Decoded word by word, the drive stage uses no
rounding mode:
- 117 `42295A2C`: C0D6 x D37 into MAC 0, secondary 29 sets the output shift to x4 (CR1 12-13 = 1).
- 118 `2BC02132`: primary AND (28-2B are AND), acc1 = C132 (800000) AND MAC 1 output: its sign.
- 119 `59402133`: MAC 1 += unsigned acc1 x C133 (000001): the sign as a tiny term.
- 11A, 11B: C0D7 x D36, C0D8 x D2F into MAC 0. 11D, 11E: the same sign trick for MAC 0.
- 11F `0028B000`: accumulate shift <<4 (CR1 14-15 = 2). 120: C0D9 x D35 into MAC 1.
- 121 `5169A123`: MAC 1 += acc1 x C123 (0), secondary 29 sets the output shift to x16.
- 122 `00283000`: accumulate shift back to 0.
The shifts and the overflow handling follow the TMS57002 as MAME has it (output shift none, <<2, <<4,
>>8; clamp only in the saturating mode), and the gains come from coefficients the firmware writes.
So no model error is found in this stage: the loud fuzz may be what the firmware does. Settling it
needs a recording of a unit with A-015 and a clean program at the same settings.

### Against a unit: the dry programs are about 13 dB too quiet, not the overdrive too loud (measured)

Thor's recording of a unit (a demo video's audio, programs from A-001 upwards, A-015 from 6:53),
1 s rms in dBFS: 0:00-6:50 median -19.3, A-015 (6:53-7:10) -15.4, whole video median -19.8. So on
the unit A-015 is about 4 dB above the rest. Ours: A-015 -14.9 dBFS (rms 5858), median of A-001 to
A-032 -32.4 (rms 786): 17 dB apart. The overdrive sits where the unit's does in absolute level
(if the video's level is near the unit's), and the clean programs are about 13 dB low. That fits the
DASP4 trace: the clipper's output does not depend on its input, so a dry path too low by some factor
leaves the overdrive untouched. 12 dB is a factor of 4, one output shift step (<<2); where the
voices or the bus could lose it is the next thing to look at. Caveats: the video is mixed (peaks
near 0 dBFS, likely limited), and the takes play chords and sweeps, not one note.

Follow up, with chords (C2 C3 E3 G3, programs A-001 to A-016, engine): clean median -28.4 dBFS,
A-015 -13.4, 15 dB apart; the video has about 4. So the single note test did not cause the gap.
DASP4 passes the clean bus at about unity (its port 0 input at 103, 105: rms 0.024-0.026; MOUT about
the same), so the level is set before it, on the voice chips: on DASP0 a voice runs near 0.4-0.95
inside and the bus cell it adds into (A13B2139 at 1A0) holds 0.024. `OB12_PC_RMS` now also prints the
rms of an AX port word written at that address. Next: follow one sounding voice from its oscillator
to its bus add and find which word scales it by about 1/4 too much.

### The voice level hangs on when the output shift takes effect (measured, open)

Thor checked the video in Sound Forge: A-014 to A-015 is nowhere near our step, with the unit's
master volume untouched. Following one voice on DASP0 (`OB12_CREADS`, `OB12_TRACE_CHIP=0`): the voice
leaves its filter near 0.14, is scaled by C126 (a per program level the Slave writes once at note on:
A-001 051A34, A-015 ramps to 07FFFF, so its top is 1/16) and by the pan gains C0DE, C0DF (about 0.5),
and adds into the bus at about 0.02. Velocity does not change C126 (harness `t:on:nn:vv`, new).
Amplifier Amount (CC 7) does not either.

The 1/16 is made up by output shifts (secondary 29, CR1 bits 12-13: none, x4, x16, /256), set and
reset many times per voice (DASP0: 0FE x16, 120 x4, 12E none, ...). When a shift change takes effect
decides the level. Measured, harness, one C3, A-001 / A-015 dBFS:
- model now (29 after the primary of its word, shift applied when the MAC is read): -25.3 / -16.9;
- 29 before the primary (`OB12_LATE_MASK`, new, bit 1 clear): -0.9 / -10.6, A-001 clipping;
  28, 2A, 2B early or late change nothing here;
- shift carried with the product (`OB12_SHIFT_PRODUCT=1` end of word, `=2` start): -2.0 / -1.4 and
  -0.0 / -0.8, clipping.
The unit sits between: clean programs about 12 dB above ours, A-015 about 4 dB over them. None of
the three simple rules gives that, so the real rule is something else, perhaps different for the
paths that store (02, 03) and the paths that add (alu with the MAC). Defaults are unchanged and the
references stand; the options are study switches.

Follow up. The TMS57070 wiki (Architecture, CR1 byte 1) confirms the fields: MRDM bits 9-11 rounding,
MOSM 12-13 output shift (none, <<2, <<4, >>8), MASM 14-15 accumulation shift, and says the MAC has
"an output shifter, and two 52-bit output registers": the shift is applied on the way into the
output registers, not when they are read. It gives no pipeline timing. Measured variants
(`OB12_SHIFT_PRODUCT`, `OB12_SHIFT_PATHS` 1 = stores and port writes, 2 = ALU and loads):
- shift of the product's own word (1, 2), any path split: everything near 0 dBFS, clipping;
- shift one word after the product (3): identical to the model now, -25.3 / -16.9.
So no single global rule reproduces the unit's balance. The next step is per word: list the voice
and overdrive words where a shift change falls between a product and its read, and see which reading
of each keeps the envelope and LFO paths sane while moving the voice level.

Per word (`OB12_SHIFT_MISMATCH=1` lists the reads whose shift differs from their product's word;
`OB12_SHIFT_PCS=chip:pc,...` takes the product's shift only there, chip 9 = the four voice chips).
25 such words per voice chip, 11 on DASP4. One word at a time, A-001 / A-015 dBFS, model now -25.3 /
-16.9:
- voice words 014, 01E, 02C, 03A, 0B1 change the sound drastically (A-001 -47, A-015 -48, silence,
  -35): envelope and LFO paths that only make sense with the shift taken when the MAC is read, as now;
  01D clips; the rest change nothing or a little (0AE -24.5 / -12.5, 121 -27.2 / -15.8);
- DASP4 06E, 1A6, 1BC, 1FE each raise both programs by about 8 dB (-17.2 / -7.6), the ratio unchanged;
  133 raises only A-015 (+12 dB), the wrong way.
So the words that work today are evidence for the read time rule, and no single word moves the clean
programs up against the overdrive. The shift timing is not where the balance goes wrong.

Reading the video again by program rather than by median: before A-015 (6:20-6:50, A-014 most likely)
about -18.5, A-015 -15.4, so about 3 dB; ours A-014 -20.1, A-015 -13.4 with chords, 6.7 dB. The
overdrive is a few dB high against A-014, not 13: the median comparison mixed in knob sweeps and quiet
passages. What stays is the absolute level, -25 dBFS for one clean note, which the unit's analog
output stage (DAC, op amps) may make up; its gain is not known from the firmware.
Note: tms57070.h changed (study fields), so build_pgo.bat must run before the next engine or plug-in
build.

## Analog output (service manual sheets 1/15 "OUTPUT STEREO CHANNEL", 11/15 "ANALOG OUTPUTs")

- DAC: PCM1716 (Burr-Brown), full scale 0.62 Vcc = 3.1 Vpp at 5 V, as the sheet marks it.
- Its setup from the Slave (UART1, `20 10 60 C2 80 FF C0 FF`, sent LSB first, so each byte bit
  reversed on MD): words 0408, 0643, 01FF, 03FF. Register 2: IW1 0, IW0 1; register 3: I2S 1, LRP 1,
  SF0 1: 24-bit I2S. Registers 0 and 1: attenuation FF with LDL/LDR set, 0 dB both channels. So the
  DAC adds no gain or loss; the DSP word's full scale is the DAC's 3.1 Vpp.
- Then per channel: C80 10u into R141 47K (0.34 Hz), an MFB low pass with gain 1 (R143, R148, R150
  2K55; C86 220p, C82 470p; the sheet: Ft1 47.0 kHz, G 1), an inverting stage R149 47K5 / R146 20K0 =
  2.375 with C83 47p (Ft2 70.5 kHz), 7.35 Vpp; STOUT mixes the other channel through R145 10K for mono;
  C25 10u into R54 10K (1.6 Hz), R55 1K5, the J108 mute FET, R52 680E, a ferrite, the jack.
- TL072s on +-12 V: 7.35 Vpp is far from their swing, so the path is linear. It sets the absolute
  level (+7.5 dB over the DAC) and cannot change one program against another.

The plug-in now models it (`AnalogOut` in ob12_vst3.cpp): gain 2.375 and the two DC blocks; the low
passes (about 0.5 dB at 20 kHz) are left out. The engine and its references are unchanged; the
smoke test's rms went from 0.0216 to 0.0552.

## The unit's memory and sysex loading

No NVRAM: sheet 03/15 ties the SRAMs' CS2 straight to VCC, no battery. What lasts is the Master's
29F800 flash. With a blank user area the firmware formats it at every boot (6 sector erases,
196608 words: sectors 60000-B0000).

- `Engine::set_flash_file`: the factory image stays in memory; at start the stored sectors replace
  it; a second after the firmware stops writing flash, the sectors that differ from the factory
  image are written (`OB12FLSH`, offset, size, data; through a temp file and rename), and at the end.
  Measured: first boot 196614 writes, file 393272 bytes; second boot 0 writes, no format.
- The plug-in keeps one memory for every project, as a unit: `%LOCALAPPDATA%\OB12\flash.bin`.
  Panel menu: "Reset memory to factory" deletes it (the factory memory returns at the next load).
- `Engine::send_sysex`: a .syx (or a .mid's F0 events, `Engine::read_sysex`) into MIDI IN one
  message at a time, each after the previous has left the UART and 100 ms more, as the factory
  .mid files are timed (about 76 ticks at 384 PPQ, 120 BPM). The UART queue is a deque now.
  Panel menu: "Load .syx / .mid into MIDI IN".

Factory programs (12prgFS from the presets zip): start marker twice, ". Updating Sect.[PROGRAM] .",
version, size 9E00 (the 40448 byte factory file), 964 records, end marker. Measured: from the first
message all LEDs go dark (the updating mode); nothing is written while the records arrive; after the
end the firmware erases D0000 and writes 0x1332 bytes, equal to the factory image, then stops: the
rest of the sector stays FF.

Solved, a fault of our flash model (`OB12_FLASH_LOG`, new: every bus write to the flash with PC):
the firmware's flash routine (in RAM, ED141F-ED1445) programmed word by word to D1332, whose word is
F0 00. The model took any F0 on an even address as the reset command, also as the data of a word
program, so that word stayed FF and the firmware's read back stopped it. F0 is data in the program
state now. Measured after the fix: 20225 words written, D0000 equal to the factory file byte for
byte, the machine resets and comes back to PLAY PROGRAM with a clean display. The same fault would
have spoiled any saved program with an F0 byte in a low half word. References unchanged (8cecf664).

### Bulk dump both ways, the LCD's text layer (measured)

- The LCD is a T6963C (Varitronix MGLS24064 datasheet). Our model drew only its graphic layer; the
  UPDATE PROCEDURE page is written in its text layer with the built in character ROM, so during a
  load the old page stayed with a garbled top line: it looked frozen (Thor's VSTHost screenshot).
  The text layer is drawn now (30 x 8 cells of 8 x 8, combined by the mode command), with the
  T6963C-0101 ROM as the standard 5x7 ASCII set (`src/t6963_cgrom.h`, from Adafruit-GFX's
  glcdfont.c, BSD; the Toshiba datasheet table could not be fetched). Measured: "OBERHEIM-VISCOUNT
  OB 12 / UPDATE PROCEDURE / . Updating Sect.[PROGRAM] . / > From dump OB 12 < / *****".
- Bulk dump out: SYSTEM, 4-BULK DUMP, ENTER shows "MIDI BULK DUMP PROCEDURES / DUMPING / PLEASE WAIT
  (10% COMPLETED)" and MIDI OUT carries the same format as the factory files: start marker, ". Updating
  Sect.[ALL] .", "> From dump OB 12 <", size 53D94 (343444 bytes), records. At TX SPEED SLOW, SECTION
  ALL: 2192 messages, 133507 bytes in 225 s, so the whole takes over half an hour. `Engine::sysex_sent`
  keeps MIDI OUT's system exclusive; the panel menu saves it as .syx; it goes back through "Load .syx".
- LCD colours from Thor's photo: blue-white backlight, blue-violet dots.
- Thor's VSTHost load of a programs dump (wc_olo_garb_ob12_programs1.syx, the same format) stopped at
  "Waiting for MIDI data". Reproduced with MIDI clock on the input (SEQ_CLOCK, F8 every 21 ms as
  VSTHost sends): the sender waited for an empty MIDI input before each message, and the clock never
  let it empty, so only the start marker went out. The gap now runs from the message's own line time
  (320 us a byte) and ignores other input. Measured with the clock running: 970 messages, 20225 words,
  A-001 "INIT" from the file after the reset. The firmware itself takes F8 between the messages.

### Panel C (service manual p41 "OB12 Panel C: Top Level", measured)

Buttons on BUTB[0] (bar 0): BUT5 Aftertouch Exchange, BUT4 Wheel Mode, BUT3 Velocity, BUT1 Aftertouch,
BUT0 Hold, BUT2 Relative. Lamps on latch 0 (its own 74HC595), measured by pressing each key: Relative
bit 6, Hold bit 2, Aftertouch Exchange bit 7, Velocity bit 3, Aftertouch bit 1, Stnd bit 5. At power
on 0x28: Velocity and Stnd lit, as on Thor's photo. Velocity opens EDIT CONTROLLERS VELOCITY and turns
it off; Aftertouch opens AFTER-T with STATUS ON: aftertouch is off until this key is pressed, which is
why Thor's aftertouch did nothing. Wheel Mode changed nothing in the tests (alone, twice, after MORPH
ENABLE or MANUAL), so M.Morph's lamp bit (taken as 4, the one left) is not verified.
The art has no Panel C; the panel view draws it in a 240 unit column left of LFO1, in the art's style.

### The overdrive's own settings on A-015 (measured)

EDIT OVERDRIVE (Level/Parameter, then the Overdrive key; pressing it toggles BYPASSED / INSERTED):
A-015 "Edy's Lead" stores DRIVE 64, TYPE BOOSTED 1, HI-DAMP 4K, LEVEL 100, the panel knob on TYPE.
LEVEL goes out as `A8 10 v` and lands in DASP4 C125 through a ramp; v 127 (LEVEL 100) 07FFFF, the
same 1/16 top as the voices' C126; then about 0.9 dB a step: v 107 -16.7 dB under the top, v 89
-30.9, v 69 -42.3, v 36 -65.3, v 3 and below silent. So the program runs the overdrive at its
maximum level by the firmware's own table, and the parameter path (message, cell, curve) is sound.
The remaining difference to the video (A-014 to A-015: about 3 dB there, 6.7 here) has no fault left
to point at in the level path; the drive stage itself was decoded earlier and follows the firmware's
coefficients. Taken as it stands; the plug-in's Output gain covers the overall level.

## XMEM read time: 5 words, not 9 (firmware evidence, measured; not checked on silicon)

Thor: A-017 Crystal PPG (timbre B-021 FM Bell) crackles on the right with the chorus on, not with
it off; later the sound turned into loud digital noise (records/OB-12 brutal digital error.wav).

Measured. A 440 Hz tone on the voice bus into DASP4 (`OB12_AUX_TONE=400000 OB12_BUS_TONE=1`, program
A-017, `tools/tone_junk.py`), everything outside 400-480 Hz against the whole:

| | Left | Right |
|---|---|---|
| Chorus on, before | -55 to -62 dB | -38 to -39 dB, lines at 440 +- 182 Hz |
| Chorus off | -57 to -68 dB | -56 to -66 dB |
| Chorus on, now | -53 to -67 dB | -55 to -67 dB |

A-001 had the same on the right (440 +- 219 Hz, -34 to -39 dB): every program with chorus.

Cause. The chorus reads each tap as a pair n, n+1 and interpolates: x[n] + f (x[n+1] - x[n]), f the
low MAC word kept in C09A (left) and C09B (right). The right tap reads n+1 at 150 and takes XRD at
156 (`242090CC`), six words later, with an XMEM write (155) between. The core gave XRD its data 9
words after a read (the public emulator's table, CR3 bus field 2 with XWORD), or earlier when the
next read flushed it (`xread_flush`). So at 156 XRD still held x[n], the difference was 0, and the
right tap stepped a whole sample each time f wrapped: C09B moves 00F900 a sample, a wrap every
263.2 samples, 182.4 Hz. The left tap was saved by the read at 14B, which flushed its n+1.

The whole DASP4 program, every read (primary 39) and every take of XRD (secondary 20):
- takes 1 or 2 words after a read expect the read before it (148, 14C, 152, 16E, 172, 178, 084);
- 080 comes 3 words after 07D and 8 after 078: 18 reverb reads and 18 takes pair up one to one only
  if 080 still gets 078, so the data is not there after 3 words;
- the reverb reads every 5 words and takes XRD in the same word as the next read (087 to 0CD, 15
  times); a secondary runs before its primary, so the flush came too late there and each take got
  the read before the one meant: the reverb taps were shifted by one, 078's data went nowhere and
  0CD's was never used. Takes at 6 words: 0D3, 156.
So the data is in XRD 4 or 5 words after its read; both give the same output, bit for bit (measured,
MD5 equal). 3 and 6 each differ. Option `xread_cycles`, 5 for DASP4 (`OB12_XREAD_CYCLES=n` in
ob12_system, 0 = the old table). With 5 the firmware never starts a read while one is pending, so
`xread_flush` no longer acts; it stays on. Only DASP4 has XMEM reads.

A-017, one note, right channel above 12 kHz: peak 0.0259, kurtosis 10.1 before; 0.0097 and 5.1 now,
equal to the left. Chorus, delay and reverb all sound different now: the reverb reads the taps it
was written for.
New reference, engine = `OB12_MIDI_NOTES=3c,40,43 ob12_system rom 16 13`: MD5
7a47ad74c1612850a09ab8639650b691.

The loud noise is not reproduced as a fault of the engine. Osc2 wave keys toggled 18 times, the
Osc Common and Osc2 Wave Mix sliders moved, 4 to 8 notes held, old and new core: MOUT stays a
normal signal (rms 0.03 to 0.30 of full scale, autopan swapping the sides every second). The
recording fits the plug-in's output clipping: its Output gain was +12 dB over the analog stage's
2.375, a factor 9.46. Our A-017 runs through that gain and a clip give rms 0.90 / 0.66 with 70% /
30% of the samples at the rail, sides swapping every second; the recording has 0.92 / 0.49 and 45
to 53% at the rail with the same swap. A-017 is a loud program: four notes at velocity 100 peak at
0.4 to 0.6 of full scale before any gain. Not proven: a take with Output gain 0 dB would settle it.

Open, measured: on A-017 the Osc Common sliders (Balance 1 Osc1/Osc2, Ring Mod, Noise) and Osc1 FM
change nothing, the output is bit identical, though the display follows (BALANCE1 0 to 100) and the
Master sends A0 21 v and the Slave ramps DASP0 C128 / C126 in 32 steps, as on A-001 where it works.
DASP0 118 `42021A9B` multiplies C128 by D9B with ACC0 = 000000 on A-017 (E20BDE on A-001). Whether
FM Bell's voice path really bypasses the balance or our core loses it is not known.

Panel: the Osc2 Wave 1 and Wave 3 keys were exchanged (BUT5 lights Wave 1's lamp, latch 7 bit 6;
BUT3 Wave 3's, bit 4; measured with `seq_test` t:key and t:dump). Osc1's keys were right.

### A-017's dead Osc Common sliders: the Split routing, firmware true (measured)

Closes the open point above. A-017's timbre (B-021 FM Bell) has the filter Routing on Split (lamp
latch 9 bit 3 lit; A-001 has Serial, bit 5). DASP0 trace of the first voice (`OB12_CREADS`,
`OB12_TRACE4_ALL=1`, `OB12_TRACE_CHIP=0`): the balance mix is still computed (115-11D, C126 and C128
into D9C), but the filter inputs are taken through pointers the Slave writes at the first note of a
program: C129 (loaded at 119, used at 11E) and C12D (11E).

| | C129 | C12D | Filter 1 input | 
|---|---|---|---|
| A-001, Serial | 00009C | 00009D | D9C, the balance mix |
| A-017, Split | 000048 | 00009B | D48, Osc 1 before the mix; D9B (Osc 2) for filter 2 |

So in Split each oscillator goes to its own filter and the Osc1/Osc2 balance, which sits in the
mix, is out of the path: by the firmware's own pointers, not by our core. Check: Routing key (bar 6
bit 6) on A-017 moves the lamp to Serial (latch 9 4C to 64), and then Balance 1 at 00 against FF
gives different sound (rms 0.025 against 0.008, one note). Osc1 FM (A0 07) and Sync (A0 19) do not
change the pointers. Not checked: what a unit does with the balance in Split, and why Osc 2 through
filter 2 is hardly heard on this timbre (filter 2's own settings were not looked at).

## CPU load: where it goes now (measured), and a constant word core that did not pay (dropped)

Thor: the emulator eats the CPU. `pace_test` (6 notes, 48 ms ring, Ryzen 9 5900X), current build:
3.21 to 3.39 cores, 0 ring empties; with one CPU thread (`OB12_ONE_CPU=1`) 2.63 to 2.70, 0 ring
empties, three runs each, output identical. Offline 2.54 x and 1.54 x real time.

The parts:
- DSP cores alone (`tools/dsp_bench`, new: a carved program through step(), noise in): 13.1 ns a
  step on the voice program, 12.4 on the effects program, profile guided. Five chips at 24.576M
  steps a second: 1.57 cores.
- The two CPUs: from the one thread run, 10.41 s for 16 s of sound with the CPUs as the slow path,
  13.5 us a sample for both: 0.65 of a core (by arithmetic).
- So about 2.2 cores are work and about 1.1 is waiting: threads spinning for each other inside a
  burst. One CPU thread wins 0.65 of that back and costs the margin: 1.54 x leaves little when the
  machine is in its slow state (see "The CPU rise was the machine").

Tried and dropped, the core with each program word as a compile time constant (a generator wrote
one case per PMEM word calling the step with the constant, from rom/dsp into build/): output bit
identical (MD5 7a47ad74, `dsp_bench` state equal), but 1.05 x on the core alone, 0.9 x once laid
out as straight line code, and the whole engine slower than the profile guided step() (3.69
against 3.29 cores). Why: decoding is not the cost. A sampling profile of step() is flat, 1813
distinct addresses and the hottest 2.2%; a word costs about 50 cycles of work that depends on
state, not on the word: the DMEM ring index, the CMEM offset and lock flags, the MAC output shift,
rounding and saturation modes read from CR1 each time, the flag updates. The generated function was
1.3 MB and the compiler kept its pointers on the stack. Two small simplifications (one write for
the ACC flags, the address pipeline skipped when empty) gave under 20% on the core without profile
guidance and were not kept.

What would lower the floor: a translator that follows the mode bits through each program (the
secondaries 28-2D that set them are constants in the word), so a word's shift, rounding and
saturation are known when it is translated; and fewer, fuller threads (both CPUs on one, the chips
two to a thread: about 2.4 cores by arithmetic, margin 1.54 x). Not done.

## Threads: the Master's own thread is gone (measured); corrections to the CPU section above

Corrections first. "The two CPUs: 0.65 of a core (by arithmetic)" was wrong. Measured alone, linked
as in the engine, 167 cycle slices, from 14 s with three notes: Master 0.061 s of work per second of
sound (run 0.049, tick 0.012; 3.95M instructions a second, 12.4 ns each), Slave 0.073 s (2.81M,
22.1 ns): 0.13 of a core together, 2.8 us a sample. And "13.1 ns a step" holds for a chip alone on a
quiet machine; inside the engine a chip took 8.9 to 10.4 us a sample (17 to 20 ns a step) in the runs
of this round (`OB12_TIME_LOG=1`, new: time stamp counter per part, us a sample). So the real work
was about 1.6 to 2.2 cores and the rest waiting.

The Master's thread did 1.3 us of work in a sample of 8 to 10 us and spun for the rest; the caller's
thread, with the Slave, the same. The layout is now a setting (`OB12_THREADS`, groups by /, chips by
comma, first group the caller's thread, `cpu` for a Master thread): default `/0/1/2/3/4`, both CPUs
on the caller's thread and a worker per chip. `OB12_ONE_CPU` is no longer read; the old layout is
`OB12_THREADS=/cpu/0/1/2/3/4`. Every layout gives the same output, MD5 7a47ad74.

`pace_test`, cores busy, 0 ring empties in all of these:

| Machine state | Old layout | Now |
|---|---|---|
| Fast (three runs each, the round before) | 3.21 to 3.39 | 2.63 to 2.70 |
| Slow (a media player running; three rounds in turn) | 5.50 to 5.74 | 4.80 to 4.93 |

Offline, slow state: 1.69 x old, 1.68 x now. One fast state run gave 1.54 x against 2.54 x; not
repeated, the machine went slow for the rest of the round, so the margin in the fast state is open.
The last two rounds, with the state changing between runs, gave 4.08 and 2.82 now against 3.35 and
3.04 old: single runs on this machine do not decide anything.

Layouts that put more on one thread lose (slow state): the effects chip on the caller's thread
1.22 x and ring empties; two chips to a worker 0.83 x, below real time. A chip is 9 to 10 us of a
20.8 us sample there, so two do not fit with any margin. "About 2.4 cores, margin 1.54 x" above was
arithmetic from the quiet benchmark and does not hold.

The slow state itself: the benchmark of one chip on one thread does not change (13.8 ns), the engine
with its six threads does (chips 6.6 to 10 us). `OB12_SPREAD` (study): 1, an ideal processor per
worker on separate physical cores, changes nothing; 2, a hard affinity, runs faster offline (1.96 to
2.12 x against 1.50 to 1.61) but stalls 26 ms when something else takes a pinned core (5 ring
empties in 10 s). Off by default. So the slow state looks like our threads sharing physical cores
with other work; not shown.

What is left: one thread needs the whole sample's work under about 13 us; it is 35 to 50 us, nearly
all of it the five DSP cores. Only a faster DSP core gets there.

## A-017 in Split: the timbre stores BALANCE 0, filter 1 only (measured, firmware and preset true)

Thor: in Split, Osc 2 is not heard on A-017 and the loudness differs. Measured (`seq_test`, one note,
16.5 to 19.5 s): Osc 2's wave mix sliders at 00 give the same sound as Osc1 FM at 00 (rms 0.0181,
centroid 536 Hz against 0.0398 and 6825 Hz), and Osc 1's sliders at 00 give silence (-90 dB). So on
this timbre Osc 2 acts only as the FM modulator of Osc 1.

Why, followed down:
- DASP0 voice 1: filter 2 runs (states D50, D51 non zero with Osc 1 muted). The output mix is 12C
  `42403140` MAC1 = C131 x D[da0], 12E `6C693940` MAC1 += C133 x D[da1], 130 takes it. The Slave
  writes the pointers (C130: 09E000 Serial; 05104E Parallel and Split, so D4E filter 1 out and D51
  filter 2 out) and the two gains. Routing is `A0 43 v` (0 Serial, 1 Parallel, 2 Split):

| Routing | C129 (F1 in) | C12D (F2 in) | C131 | C133 |
|---|---|---|---|---|
| Serial | 9C mix | 4E F1 out | 0 | 456FBD |
| Parallel | 9C mix | 9C mix | 456FBD | 0 |
| Split | 48 Osc 1 | 9B Osc 2 | 456FBD | 0 |

- The Slave computes the gains at FD2683-FD27CC: P = long[voice parameters + 54], L from the level
  table FCF174; C133 from L x P, C131 from L - L x P (each with a shift byte, +58 and +59). P is set
  at FDADF0-FDAE09 from `long[FCB616 + 4 x parameter 2E]`, a linear table 0 to 7FFFFFFF over 0 to
  127. Sending `A0 pp 40` for every pp straight to the Slave (`OB12_SLAVE_FILE`,
  `OB12_SLAVE_WATCH=efc9b2,efc9b6`): pp 44 sets P to 40810204. So `A0 44 v` is a filter balance.
- The Master shows it: EDIT FILTERS, ROUTING page, in PARALLEL and SPLIT a line "BALANCE = 0"
  (and in SPLIT "FILTER 1 IN=OSC 1, FILTER 2 IN=OSC 2"); Serial has no such line. The encoder moves
  it up from 0, not below. B-021 FM Bell stores 0: filter 1 only.
- Check: BALANCE raised on that page with Osc 1's sliders at 00: -50 dB instead of -90, Osc 2 now
  comes through filter 2.

So nothing is lost in our machine: the factory timbre uses Osc 2 as a silent modulator. The
"Not checked: why Osc 2 through filter 2 is hardly heard" above is answered.

## Osc 1 FM raises the level: the oscillator's own arithmetic (measured; not checked on a unit)

Thor: Osc1 FM makes the sound louder. Measured, one note, MOUT rms: A-001 FM pot 00 -22.4 dB, 40
-18.0, C0 -9.7, FF -8.9 with the peak at the rail; A-017 FM 00 -34.9, as stored -28.0.

`A0 07 v` writes DASP0 C057 and C1C5 (FF: 07FFFF, 7FFFFF). 0C1 `6C691A9B` adds C1C5 x D9B (Osc 2)
to the rate; 0C3 stores the sum in C1CC: 00B299 without FM, around F708C9 with it, so the
instantaneous rate swings to about 13 times the note's and below zero. The phase (D7F, rms 0.578, a
full scale ramp) is the same either way. The level changes in the waveform stage 0DA-0E8
(`OB12_TRACE_MAC=1`, new: both MACs and CR1 per word): differences of the phase terms are multiplied
by C0C8 and C0C9 (006520, 003264 here; the Slave writes them once at note on, for the note's
pitch) and brought up by 512: twice loaded into a MAC upper word and read back through the x16
output shift (0E3/0E4, 0E7/0E8), and once shifted left (0E5). That is a differentiated waveform:
its size follows the rate, and the fixed coefficients make it right only for the note's own pitch.
Osc 1's output D48: rms 0.040 without FM, 0.734 with FM FF, written at the rail (800000) under the
saturating mode (CR1 bit 16 set at 0E7, 0E8).

No rule of our core that is in doubt takes part here: plain multiplies, the x16 output shift read
as elsewhere, saturation on. So the FM loudness is the firmware's own arithmetic as far as we can
read it. Two things would change how much: if the clean level is really about 12 dB higher on a
unit (see "Against a unit"), Osc 1 would sit near 0.16 and FM could add only about 13 dB less of
that; and a unit's recording of one note with FM at 0 and at maximum would settle it.

## Signal path audit: DASP4 to the DACs and the S/PDIF output (read from rendered sheets; measured where said)

Thor asks for the signal to be true to the hardware at least up to the digital output. Sheets 05,
08, 09, 11 of the service manual and the PCM1716 data sheet, read as rendered pages
(build/sm/*.png, made with PyMuPDF at 4 to 5 times; not in git).

Clocks, sheet 08/15 "DASPs CONTROL":
- XTAL1 is a 12.288 MHz oscillator; its output is FSx256 (a, d, c through R34, R31, R33). So FS is
  48.000 kHz exactly, and the DSPs' CLKIN is 12.288 MHz (with the x2 PLL, 512 cycles a sample, as
  the engine runs them).
- IC15 ATF1500 takes FSx256c on its clock pin and gives FSx128, FSx64, FSx2, FS: the divider. It
  also sits on the Slave's bus (D[7:0], A[0], CSDSP, RD, WRL) and drives the DSPs' serial host
  port (HR, HRS, HX, HXS, HCK[2:0]): the byte to serial converter. Nothing of it touches audio data.
- IC16 74HC541 is the clock fan out only: FSx128 to d1, s, d2; FSx64 to s, d, a; FSx2 to d1, d2; FS
  to s, d, a (d = DSPs, a = DACs, s = S/PDIF), each through a series resistor.
  So neither part needs a model beyond what the engine does: an exact 48 kHz and host bytes.

DASP4, sheet 09/15:
- AR1 and AR2 take D3SO2, D3SO3 (DASP3 AX2, AX3), both clocked by FSx2d1 / FSx128d1: two frames
  per sample, the two slots of the engine's bus.
- AX1 is the LFO line back to the voices, on the same 2 x FS clocks.
- AX2 and AX3 are clocked by FSxd / FSx64d: one frame per sample, 64 bit clocks. AX2 goes through
  47 ohm to MOUTa and MOUTs, AX3 to AOUTa and AOUTs.
- The effects program writes AX2 once a sample (072 left, 074 right), AX3 once (108, 109), AX1 in
  both halves (03C, 03E, 13C).

DACs, sheet 11/15: two PCM1716. Main: Din = MOUTa, LRCin = FSxa, BCKin = FSx64a, XTI = FSx256a.
Aux: Din = AOUTa, same clocks. Each has its own latch enable (NDACLE[0], [1]) on the shared ACK /
ADI control lines; DACRST, DACMUTE, and JOFF (the J108 FETs to ground on every output) mute.
The setup words against the data sheet (page 9, 10: B10, B9 are the register address):
- 0408: register 2, IW1 0, IW0 1, OPE 0, DEM 0, MUT 0: operating, no de-emphasis, no mute.
- 0643: register 3, I2S 1, LRP 1, ATC 0, SRO 0, REV 0, CKO 0, SF 01, IZD 0. IW 01 with I2S 1 is
  "24-Bit I2S" in the data sheet's table: sharp roll off, output not inverted, no zero detect mute.
- 01FF, 03FF: registers 0 and 1, LDL / LDR set, FF = 0 dB.
  This confirms the earlier decode; the DAC takes the whole 24 bit word at 0 dB.

S/PDIF, sheet 05/15 "REAR PANEL": IC10 CS8402A, SDATA from three NAND gates of IC9 (74HC00):
MOUTs when AESEL is high, AOUTs when it is low. SCK = FSx64s, FSYNC = FSxs, MCK = FSx128s, RST =
DORST; M2 high, M1 and M0 low; PRO tied high (consumer); TXP through 374 ohm and 90.9 ohm to the
PE65612 transformer and the RCA jack CN8. No CS8402A data sheet here: M = 100 as the I2S
compatible format is from memory, not checked, and the channel status straps were not read.
Where AESEL comes from was not followed.

So the digital output carries the same serial word as the main DAC: DASP4's AX2 (or AX3), 24 bits,
48 kHz, nothing between but gates and a transmitter. The engine's output is that word: AX2 after
the sample's second half frame, 24 bits, taken 1:1 by the plug-in (float of the word / 2^23). At
this point there is no model in the path that could differ from the board; what differs from a unit
is inside the five DSPs and what the two CPUs send them.

Not modelled, by choice or not yet:
- The aux pair (AX3, its DAC, AESEL). Checked on A-012, A-023, A-026 (the programs once found
  silent on one note): all sound on MOUT, AOUT stays at its constant 113 left, 0 right.
- Mute lines at power on (DMUTE, DACMUTE, JOFF) and the S/PDIF channel status.
- Whether AX2's left and right are latched in the same sample on the chip: they are written two
  words apart; a frame edge between them would skew one channel by a sample. Not known.
- After the DAC (analog only, not on the digital output): the DAC's own filter, the two low passes
  (47.0 and 70.5 kHz, about 0.5 dB at 20 kHz together). The plug-in has the gain and the DC blocks.

CS8402A, from its data sheet (Thor's CS8401.PDF, page 18, Table 3, rendered): M2 M1 M0 = 1 0 0 is
"4 - Left/Right, I2S Compatible", 16 to 24 bits. So the transmitter takes the whole 24 bit word in
the same format as the DAC. Closes that point of the audit; the channel status straps stay unread.

## The DSP programs translated: one thread, 0.43 of a core (measured)

Supersedes "a constant word core that did not pay" and "What is left: one thread needs ..." above.

What the three programs are: 512 words each with no jump, call or repeat (no word at E0 or above),
13, 13 and 3 conditional address loads, one CR write by a secondary, 56, 56 and 37 mode
secondaries. So a half frame, 256 words from PC 000 or 100, is a straight line.

What changed in `src/tms57070`:
- The registers are a struct of their own (`Regs`; `Core` is one, so `core.acc`, `core.pc` read as
  before). A run copies them into a `Machine` on the stack with pointers to the memories, steps,
  and copies them back. Every step function is a `Machine` member, forced inline.
- `tools/gen_fast.py` writes `build/tms_fast.inc` from rom/dsp (firmware words, outside git;
  `build_master.bat` runs it): per program and half one function of 256 `step_word(constant)`
  calls. It also follows the mode secondaries around the loop and gives each half the CR1 mode bits
  it starts with (mask B9FE00 or B9FF00 on the voice programs).
- `Core::run(steps)` enters a translated half when PC is 000 or 100 and `fast_ready()` holds:
  no study switch, no repeat, no XMEM read on its way, nothing queued for the address registers,
  CR1's mode bits as expected. The half's code is told the same as assumptions. Anything else runs
  word by word, so a wrong guess of the generator costs speed and never output.
  `Core::select_fast()` (after PMEM is loaded) compares all 512 words and the options.

Why it works now and did not before: with the registers in a local struct the compiler keeps them
in machine registers, follows the mode bits from word to word as constants, and drops the flag
updates nobody reads. The first build of this still kept everything on the stack, 18 ns a step:
the queued address register write indexed the struct by a run time number
(`preg[dual_reg[1]]`), and one such index stops the compiler splitting the struct at all. Written
as a switch over the eight registers (`put_preg`), 1.4 ns.

Measured (`tools/dsp_bench`, 100M steps, noise in; word by word and translated must leave the same
state, and do: FA417F6D, 356212FB, CBDD87BF), profile guided build:

| Program | Word by word | Translated | |
|---|---|---|---|
| Voices, DASP0 | 13.1 ns a step | 1.26 | 10.4 x |
| Voices, DASP1-3 | 12.9 | 1.18 | 11.0 x |
| Effects, DASP4 | 12.1 | 3.43 | 3.5 x |

The whole machine, bit for bit: `engine_test rom 16` and `OB12_MIDI_NOTES=3c,40,43 ob12_system rom
16 13` both 7a47ad74c1612850a09ab8639650b691 as before (the harness steps word by word, so this
also compares the two paths). Eight `seq_test` runs of 24 to 26 s against the engine of main
(A-001, A-004, A-015, A-017, A-018, A-030, B-100 with a program change, 14 notes at once; pots,
keys, pitch bend, mod wheel moved): all eight WAVs equal.

`OB12_TIME_LOG`, us a sample: CPUs 3.25, chips 0.83, 0.71, 0.61, 0.59, 2.42: 8.4 of the 20.8 us.
`pace_test`, 6 notes, 0 ring empties in every run:

| Threads (`OB12_THREADS`) | Cores busy | Offline |
|---|---|---|
| One, `0,1,2,3,4`, the default now | 0.42 to 0.44 | 2.38 x |
| Two, `4/0,1,2,3` | 0.58 | 4.03 x |
| Six, `/0/1/2/3/4`, the default before | 1.07 to 1.17 | 6.0 x |
| Before the translation, six threads | 2.63 to 2.70 | 2.5 x |

The two CPUs are now the largest part, 3.25 us of 8.4: the next thing to make faster.
VST3 with it: build/next7. Not yet played in a host.

## The two CPUs made cheaper: idle loop, direct memory, statistics off (measured)

Where the CPUs were (CPU only bench, from 14 s, three notes; `HOT=1` lists the addresses run):
- Master: 96.1% of its 3.95M instructions a second at one address, F00709 `60 FE`, BRA to itself:
  the RTOS idles there until an interrupt.
- Slave: no such loop; half its 2.81M instructions in one routine (FC2813-FC282F, FC2A1D-FC2A20,
  17 instructions) and 9% in FCDB8E-FCDB98: real scanning, not idle.

Changes, each keeping the output:
- `f2mc16.cpp`, BRA to itself: an interrupt request changes only between runs, so the remaining
  turns of the loop in a run are taken at once, 4 cycles each, the cycle count ending where it
  would have (`ob12_idle_skip`; not while a prefix, a delayed flag restore or a pending request
  could make a turn differ). `ob12_idle_turns` counts them.
- `shim/emu.h`: the machine's handlers are `slow_read` / `slow_write`; `read_byte` / `write_byte`
  first try two read windows and one write window of plain memory. `go_lean()` on Master and Slave
  (the engine calls it) opens them for flash (read) and RAM (read and write), unless a study watches
  those accesses. The harness keeps `lean = true` only and so runs the old way.
- I/O register statistics (a map lookup per access) are skipped when lean.
- `mb90611.h`: the reload timer counts in runs, not one by one. No time gained (the timers were not
  the cost; the CPU bench's "tick" figure was its own clock reads), kept as it is simpler work.

Measured, all with the reference 7a47ad74 unchanged on engine and harness, the eight `seq_test`
scenarios equal to the old engine's again, and a boot on an empty memory with a store equal too
(196614 flash writes, the stored file, the audio and the LED latches the same in both):
- CPU only bench, Master run 0.048 to 0.006 s per second of sound, Slave 0.062 to 0.043.
- `OB12_TIME_LOG=14` (now a time: counts from there, so the boot is left out), us a sample, three
  notes: CPUs 1.57 (Master 0.36, Slave 1.18), chips 0.63, 0.62, 0.59, 0.57, 2.43: 6.4 in all.
  The earlier "CPUs 3.25" was an average over the boot, where the Master is busy.
- `engine_test rom 16`: 2.64 x from 2.38 x. `pace_test`: 0.36 to 0.42 cores (0.42 to 0.44 before),
  0 ring empties; the machine's timer was coarse in these runs (about 790 sleeps in 12 s, 5000
  before), so the pace figures are rough.

What is left of the 6.4 us: the effects chip 2.43 (4.7 ns a step against 1.2 on a voice chip: its
128K words of XMEM do not fit the cache), the Slave 1.18, the four voice chips 2.4 together.
VST3: build/next8. Not yet played in a host.

## The translation told more: status flags apart, ring size, CR3, XMEM timing (measured)

The effects chip took 4.7 ns a step where a voice chip took 1.2. The translated code showed why
(disassembly of `fast_2a`: 350 bytes a word, tests of CR1 bits 16, 19 to 21, 23 all through it):
- CR1 holds the mode bits and the status flags (bits 0 to 6: AOV, AOVL, ACCZ, ACCN, MOV, MOVL, MOVR)
  in one word. Nearly every word changes a flag, and after a few words of such updates the compiler
  no longer knew the mode bits. `Machine` now keeps the flags in `status` during a run and only
  the mode bits in `cr[1]`; `store()` puts them together again. `flag`, `set_flag`, `get_cr`,
  `set_cr` hide the split.
- The options were assumed, and two of them (XMEM size, read flush) were still read from the core
  at each use. A translated half now writes its options as constants (`fast_options`);
  `select_fast()` has checked that the core's are those.
- The XMEM read time and size are constants for the program that reads XMEM (5 words, 20000), so
  the count down per step is gone; `select_fast()` refuses the program on other values.
- CR3 is what the header loads (no word writes it): checked in `fast_ready()`, a constant inside.
- The DMEM ring size: no big program sets it, a start-up program does (prog9 ends with `C70CD000`,
  ring 00 to CD, for the effects chip; the voice chips keep 0). `gen_fast.py` takes the ring of the
  start-up program with a C7 word for the program with XMEM reads and 0 for the others;
  `fast_ready()` checks it (and DOFF within the ring), so a wrong pairing would only cost speed.
  `dsp_bench` sets the ring for program 4 as the machine has it; its state number is now 77E99AC1.

Measured, profile guided, `dsp_bench`: voices 0.70 and 0.71 ns a step (were 1.26, 1.18), effects
0.98 (was 3.43); same state word by word and translated. The object file is 238 KB (was 540).
`OB12_TIME_LOG=14`, us a sample: chips 0.38, 0.38, 0.38, 0.37, 0.55 (were 0.63, 0.62, 0.59, 0.57,
2.43); CPUs 1.54 (Master 0.35, Slave 1.16): 3.6 in all, of a 20.8 us sample.
`engine_test rom 16`: 4.64 x on one thread (2.64 x). `pace_test`: 0.19 to 0.25 cores, 0 ring empties.
All the checks again: reference 7a47ad74 on engine and harness, the eight scenarios, and the flash
format with a store, equal to the old engine. VST3: build/next9, not yet played in a host.
The Slave is now the largest single part.

## The CPUs again: no hook when lean, interrupt sources asked once (measured)

- `ob12_quiet_hook`: with the direct windows open (`go_lean`, no study on), the instruction hook
  is not called while the PC is in bank E0 or above; below it still runs and stops the machine.
  `pc`, `steps` and the Slave's call log at FFD3A0, FFD215, FFD12E, FFD1BD are then not kept; only
  the studies read them. CPUs 1.54 to 1.40 us a sample.
- `best_vector()` went through 32 vectors and asked every source for each, at every tick and
  register write. `PendingScan` (mb90611.h) asks each source once, in the order
  `source_pending()` asks them, so a vector two sources share is still answered by the first, then
  takes the lowest level, on a tie the lowest vector. `source_pending()` stays for the interrupt
  acknowledge. CPUs 1.40 to 1.02 (Master 0.31 to 0.13, Slave 1.06 to 0.87).

`OB12_TIME_LOG=14`, us a sample: CPUs 1.02, chips 0.37, 0.37, 0.38, 0.36, 0.55: 3.05 in all.
`engine_test rom 16`: 5.44 x on one thread. `pace_test`: 0.12 to 0.19 cores, 0 ring empties.
Checks: reference 7a47ad74 on engine and harness, the eight scenarios and the flash run with a
store all equal to the old engine. VST3: build/next10, not yet played in a host.

Where the 3.05 us go now: Slave 0.87, voice chips 1.48 together, effects chip 0.55, Master 0.13.
