# Draft: correction to PR #3, finding 4

Status: sent with Thor's approval, 2026-11-07: https://github.com/Prehistoricman/TMS57070/pull/3#issuecomment-6700756737, commit d1531ba on the PR branch,
finding 4 and the "Result with all four" section replaced in the description.

Sent as: one commit on `Tuth/TMS57070` branch `ob12-findings`
(`F:\Claude\OB-12-pr\ob12-fix-43.patch`), this comment on
https://github.com/Prehistoricman/TMS57070/pull/3, and the finding 4 section of the PR description
replaced by the short text at the end.

## Comment

Correction to finding 4: I got primaries 43, 47, 4B, 4F wrong, and I've pushed a commit that replaces
that part of the PR. Findings 1 to 3 are unchanged.

What was wrong: I took these as low MAC word times DMEM, unsigned, 24 bits down. The autocorrelation
table in the description was real, but it showed less than I claimed. The shift made the term almost
vanish, so the noise went away without the instruction being modelled. When the OB-12 effect program
finally ran, the right channel of the fifth chip went silent, and that traced back to this primary.

What the firmware shows: there are five uses in the OB-12 programs, and all five look the same.

| Program | Address | Word | Secondary | Previous word |
|---|---|---|---|---|
| voice | 0DD | `43C81280` | 08 | `6C001881` |
| voice | 154 | `43C8128B` | 08 | `6C00188C` |
| voice | 1BF | `43C81296` | 08 | `6C001897` |
| fifth chip | 0E8 | `43462937` | 06 | `42002136` |
| fifth chip | 166 | `4348288A` | 08 | `4200212C` |

Each one has a secondary that uses the C bus, and each directly follows a primary that multiplies by
a CMEM word. So my reading now is that 43 is the form of 42 for when the C bus is taken: DMEM times
the C word the multiplier still holds. At 0E8 and 166 it is the right lane of a pair whose left lane
is a plain 42.

Result on the OB-12, with the effects running: the right output goes from a constant -9 to RMS
129655 for a single note, and a single note shows detuned pairs of lines around 130 and 261 Hz
instead of odd harmonics only. Peak level falls from about 3.3M to 787k, so twelve voices fit
without clipping.

Limits:
- This is inferred from firmware, not checked on silicon or against a recording of the instrument.
- Your source labels this primary MACLO. If that came from documentation or a measurement, it
  outweighs my reading, and I'd like to know.
- I can't say which primaries set the held word. The commit sets it on 40, 42, 6C and 70 families.
  Setting it on 42 alone gave a byte identical result for one test note.
- Flag bits on 47, 4B, 4F are assumed to match 46, 4A, 4E. The OB-12 only uses 43.

Check: the patched emulator against my own core, 81.92M steps over five programs with loaded and
live CMEM, no differences. As a control, the previous patch against the same core mismatches at
0DD, 1BF and 0E8. In that control the fifth chip with live CMEM showed no mismatch in 2000 frames,
so that one run does not exercise the change.

Sorry for the noise.

## New finding 4 for the PR description

### 4. Primaries 43, 47, 4B, 4F: DMEM times the held C word

Used at 0DD, 154, 1BF in the voice programs and at 0E8, 166 in the program for the fifth chip. Each
use has a secondary on the C bus and directly follows a multiply by CMEM, so this reads as the form
of 42 for when the C bus is taken. The multiplier keeps the last C word. Replaces an earlier version
of this finding, low MAC word times DMEM, which silenced the right channel once effects ran.

## New Result section for the PR description, sent 2026-11-07

## Result with all four

With these, the items under "Not in this PR", and the harness's own parts, saturating bus sums and
the Slave's timers among them, the instrument plays from unmodified firmware with its effects
running, in stereo. Measured at the main output, program A-001:
- Notes C3 and C4 sit an octave apart, strongest lines 130.8 Hz and 261.6 Hz, each with detuned
  neighbours, 130.0 and 132.4 Hz, 260.4 and 265.2 Hz. Spectral flatness 0.017 and 0.006.
- A three note chord shows its three pitches, 130.8, 164.8 and 195.2 Hz.
- Left and right differ: RMS 234275 and 129655 for C3.
- Idle output is exactly 0.
