# P007 — OB-12 self-challenge audit (2026-11-18)

**Status: PARTIAL / NO RELEASE GO.** This is a real adversarial investigation, not a claim of FULL AUDIT, QA PASS, Studio One PASS or complete source inspection.

## Exact scope and provenance
- User repository: `challanger2000/125A-OB12-VST3`; Windows x64 VST3, original hardware-emulation architecture, GPL-3.0 sources.
- Stable `main` preserved at `0ce0baa22e51dfd8ee31c09fba445360425721c6`. No owner-approved commercial FULL release or separate Demo is identified (N/A for this private/freeware project).
- Feature baseline: `feature/preset-browser-audit@04d16b5594782593d9972799fe122c165a6a40d7`; pre-audit Windows CI [44376294031](https://github.com/challanger2000/125A-OB12-VST3/actions/runs/44376294031) passed the existing tests.
- Audit work branch: `audit/p007-adversarial-20261118`, based on feature SHA above. Audit-only fixes on this branch are **not** an approved VST3 release.
- Authentic emulation reference: `Tuth/ob12-emu` v0.10.0 firmware/ROM staged transiently during Windows CI; neither copyrighted ROM nor actual Studio One environment is checked into this audit branch. Temporary factory-derived alternate Program+Timbre MIDI files are generated for tests. No independent user hardware recording was reviewed here.

## Inventory

The repository at the feature baseline contains **94 tracked files**, of which **22** reside in `src/`, **62** in `tools/`, **4** in `docs/`, one CI workflow and remaining metadata. Principal production paths:
- VST3 COM/component/controller, MIDI event conversion, async render ring, project state, editor: `src/ob12_vst3.cpp`
- Emulator, firmware CPU/DSP coordination, SysEx, flash: `src/ob12_engine.cpp`, `src/ob12_master.cpp`, `src/ob12_slave.cpp`, `src/f2mc16/*`, `src/tms57070/*`
- UI and native Win32 interaction: `src/panel_view.cpp`, `src/preset_browser.h`, `src/panel_controls.h`
- Test oracles/host harness: `tools/vst3_smoke.cpp`, `tools/bank_import_smoke.cpp`, `tools/vst3_norom_smoke.cpp`, `tools/generate_bank_fixture.py`, `tools/ob12_sysex.py`, `.github/workflows/ci-125a.yml`

**251 surface entries** enumerated in [P007-CONTROL-MATRIX.csv](P007-CONTROL-MATRIX.csv): 53 potentiometers/sliders, 85 push keys, 96 LED outputs, 10 browser widgets, 7 panel/menu functions. **155 interactive entries + 96 LED outputs.** Existing regression evidence on these 251 entries: 7 PARTIAL, 244 NOT RUN for per-control verification; real Studio One: 251 NOT RUN. This inventory enumerates controls; it does not assert that all paths have been exercised.

## Findings and adversarial evidence

| ID | Status | Severity / contract | Independent evidence |
|---|---|---|---|
| P007-F01 | **FAIL reproduced on baseline; fix pending validation** | **HIGH** — factory reset must restore the exact original 1 MiB project flash, not merely forget an external path | [Windows audit run 44381951577](https://github.com/challanger2000/125A-OB12-VST3/actions/runs/44381951577): `AUDIT_FACTORY_RESET_EXPECTED=46 ACTUAL=39 RESTORED=0 STILL_MUTATED=1`; independent `Engine::reload` positive restore control PASS. The original bug was confirmed, not guessed. |
| P007-F02 | **FAIL on intermediate audit build** | **HIGH** — immediately after a saved project is reloaded, selecting a nonfactory library must install the exact bank into saved flash | [Windows run 44383971248](https://github.com/challanger2000/125A-OB12-VST3/actions/runs/44383971248): first `LIBRARY_SWITCH_TEST=PASS`; second after `setState` `LIBRARY_SWITCH_TEST=FAIL flash_changed=0` despite browser names. Root-cause hypothesis: firmware-boot race. Audit branch now stages a preboot bank and reapplies when the Slave becomes ready; its exact SHA still needs CI confirmation. |
| P007-F03 | **SOURCE DEFECT** | **HIGH / realtime** — VST3 `process()` allocates `std::vector<Message>` and `Message::raw`; performs `stable_sort` and calls `Engine::midi()`, which acquires a blocking `std::mutex`. Optional `midi_log.on` also enables FILE I/O / `fflush` inside the callback | Trace `src/ob12_vst3.cpp::process` → `src/ob12_engine.cpp::midi` → `Impl::input_lock`. No independent paced 64/48k deadline test, so no numerical dropout rate is claimed. |
| P007-F04 | **SOURCE DEFECT** | **HIGH / MIDI timing** — host event `sampleOffset` is stored and sorted but every MIDI message is submitted before rendering the entire current block. Two notes with distinct sample offsets can reach the firmware at the same point | `src/ob12_vst3.cpp::process`: for-each `list` loop sends all MIDI before `inst->render(...)`. No independent sample-accurate MIDI/audio test run. |
| P007-F05 | **NOT VERIFIED** | **HIGH / project recall** — only the 1 MiB flash and last MIDI program are serialized. GUI `pot_value` positions, hardware key/latch state and all 53 independent panel controls have no demonstrated VST3 automation + controller/PCM recall route | `ProjectStateHeader` in `ob12_vst3.cpp`, `PanelView::Impl::pot_value`. The UI initializes displays to 0x80 while Master constructor initializes `pot_live` to 0xff; whether hardware/firmware immediately reconciles these values is not proven. |
| P007-F06 | **NOT VERIFIED** | **MEDIUM / latency** — plugin reports 0 samples though asynchronous producer uses a nominal 2304-frame lead. Actual note-on-to-audio latency and PDC must be measured; the ring lead alone is not proof of reported latency error | `getLatencySamples()` and `Instrument::start_feeder()`. No measured host latency fixture. |
| P007-F07 | **NOT VERIFIED** | **HIGH / original bank sound oracle** — `bank_import_smoke` mutates a Program byte, checks flash, restores original bank, then verifies nonzero audio. That proves original CPU playback, **not** audibly different imported user-bank PCM or audio parity with independent original-machine baseline | `tools/bank_import_smoke.cpp`. Need altered valid real patch with independently specified audio comparison. |
| P007-F08 | **NOT VERIFIED** | **MEDIUM / multi-instance + GUI** — three sequential DLL reload cycles test open/close/reopen only; no concurrent instances with different libraries and editor lifetimes; no real Studio One crash/black-band acceptance | `tools/vst3_smoke.cpp::test_second_plugin_instance` and delayed-frame fake `IPlugFrame` harness. |
| P007-F09 | **NOT VERIFIED** | **HIGH / external assets** — corrupt/truncated `panel.ob12p` and external MIDI/SysEx streams need full negative/rollback/resource-lifetime coverage. `PanelView::Impl::load()` uses unbounded decoded `count` / vector reads without a full file-length guard; no malformed-asset fuzz test is available | `src/panel_view.cpp::load`, `src/ob12_engine.cpp::read_sysex/decode_bank`. |
| P007-F10 | **SOURCE RISK** | **MEDIUM / MIDI mapping** — the `raw_channel_messages` flag is sticky once an arbitrary raw MIDI channel data event arrives; afterward `inputParameterChanges` is disabled even if the host later sends mapped CC/pitchbend only | `src/ob12_vst3.cpp::process`. Needs mixed-format event regression before a behavioral change. |

## Existing test adequacy challenge

- Existing `AUDIO_TEST` uses one note and nonzero RMS; it does not test exact timestamped MIDI, staccato NoteOff, altered-bank PCM, 0/100% control sweeps, glitches or sustained high-parameter UI interaction.
- Existing `BROWSER_TEST` verifies names, counts and switching in a synthetic Win32 host; this is not 251 control clicks and does not prove DAW host ownership.
- Existing `PROJECT_STATE_TEST` is an exact header/flash size plus program location and same-instance restore; it does not prove fresh-DAW session audio equivalence across all panel settings or multiple concurrent instances.
- Negative oracle for factory reset was executed on the original broken implementation and failed for the intended reason with an independent positive-control restoration. New adversarial checks add boot-time bank survival and reject concurrent original SysEx transfer, but need successful Windows execution.
- No independent second reviewer inspected the complete source/tests. This is labeled **self-challenge**, not an independent review.

## Remaining audit/release gates (all independently tracked)

| Gate | State / actual evidence |
|---|---|
| Source inventory | **PARTIAL**: 22 production files enumerated; critical paths examined; every source/test line not yet inspected |
| Existing Windows build | **PASS only for original feature commit**: 44376294031 |
| Adversarial reset baseline detection | **PASS as negative control**: 44381951577 reproduced the defect |
| New reset + preboot-bank fixes | **NOT VERIFIED** until same-SHA Windows CI is green |
| Official Steinberg Validator | **NOT RUN** |
| 125A Plugin Tester / lifecycle / I/O / realtime p95-p99-max | **NOT RUN** |
| Real Studio One editor clicks/zoom/multi-instance/project restore/audio | **NOT RUN** |
| Real OB-12 hardware sonic comparison | **NOT RUN** |
| Owner listening and approval | **NOT RUN** |

**Verdict:** DO NOT call this a FULL AUDIT, GUI PASS, QA PASS or RELEASE READY. The source and tests already contain material gaps. Keep main/feature release unchanged until fixes and required independent end-to-end evidence are verified.

## Audit continuation: parser hardening and test-gate status (2026-11-18)

**Pinned P007 baseline remains `04d16b5594782593d9972799fe122c165a6a40d7`.**
The work in `audit/p007-adversarial-20261118` is investigatory, not shipped.
Its exact audit head after the follow-up source changes is `c62afa4aaf29304773020a44936f2fee3b2d7da3` (the report commit itself follows this SHA).

### Corrected source paths — NOT YET RUNTIME VERIFIED

| Finding | Applied audit-branch change | Executed evidence and remaining proof |
|---|---|---|
| F01 reset | `Engine::forget_flash()` now attempts a synchronous, private 1-MiB restore from immutable constructor factory image. Win32 reset dialog reports rejection; browser invalidates pre-reset user-memory cache. | **Previous P007 reproducible FAIL** in run 44381951577. Updated code **NOT RUN** at exact new SHA. A native test with transfer-rejection control was committed, but no passing Windows run for that revision is available. |
| F02 bank switch after state recall | `apply_bank_pair` stages bank payloads through original CPU boot, snapshots include selected preboot bank, and a handoff re-applies when the Slave becomes ready; check and lock pending SysEx transaction. | **Previously reproduced FAIL** in run 44383971248, which stopped the old suite. New boot-survival and negative pending-transfer tests committed; new SHA runtime **NOT RUN**. |
| F04 untrusted legacy flash file | `Impl::load_flash` validates offsets/lengths with subtraction (avoids uint32 overflow), rejects partial, zero-sized, duplicate and overlapping sections and commits only a complete temporary 1-MiB image. | Native integration now covers 1 valid + 4 malicious/truncated/overlapping fixture cases; **NOT RUN** after code edits. |
| F04 corrupt binary panel art | `PanelView::Impl::load` has a 32-MiB file cap, bounded run/command/point reads, valid finite geometry dimensions, semantic opcodes and a balanced clip-stack requirement. Only a fully parsed valid art commits. | The **real original P007 panel asset** was independently parsed offline: 1,753,960 bytes, 25,347 runs, 472×126 art units, 188 balanced clip pushes/pops. **Eight altered in-memory fixtures** were rejected by an independently authored reference parser for the intended reasons (length, count, NaN, command overrun, illegal mode, truncated point, orphan closing clip and trailing data). This proves **fixture validity only**; native C++ `PanelView::load` negative integration remains **NOT RUN**. |
| F04 malformed firmware ROM | Original Master and Slave external firmware copies now reject oversized files instead of copying past their emulated flash vectors. | Original shipped ROM sizes offline: Master 343,444 bytes, Slave 149,744 bytes, both inside emulated ranges. Malformed-file native controls **NOT RUN**. |

### Source/test scope and independent findings

The 251-item matrix remains an inventory rather than a claim of functional control-by-control testing. The original P007 Windows host harness checks direct synthetic bank switching, the panel container and a few preset names, but it does **not** exercise each of the 53 pots, 85 hardware push buttons or 96 LED routes. CPU/DSP algorithmic parity with the physical OB-12 was not established. User's prior actual Studio One black-edge and lifecycle regressions cannot be closed by a synthetic fake-frame test.

The VST3 audio callback still uses dynamic `std::vector`, blocking `std::mutex` in `midi_log()` and `Engine::midi()`, sample offsets are sorted but not dispatched at their block offsets, and an offline render loop can wait unboundedly on the feeder. These are material source-identified realtime/MIDI/liveness concerns. No source change to these is marked PASS and no deadline/PDC measurement has been run.

### Executed evidence and CI availability

- Baseline Windows P007 CI: [44376294031](https://github.com/challanger2000/125A-OB12-VST3/actions/runs/44376294031), success on the **unmodified** feature SHA.
- Independent negative reset control: [44381951577](https://github.com/challanger2000/125A-OB12-VST3/actions/runs/44381951577), intended red with actual wrong byte 39 versus expected 46, positive reload control PASS.
- Intermediate remediation: [44383971248](https://github.com/challanger2000/125A-OB12-VST3/actions/runs/44383971248), failed after project recall with `LIBRARY_SWITCH_TEST=FAIL flash_changed=0`.
- Local/offline real asset verification: 12/12 valid Program/Timbre MIDI files plus 4/4 corrupted-MIDI negatives, one original panel asset plus **8/8** corrupted-panel negatives. Both were independent reference parsers, **not the compiled project C++**.
- Latest source edits have **no matching successful Windows workflow run**. They cannot be evaluated by reusing or rerunning an older commit's green logs. The workflow remains configured for manual `workflow_dispatch`, but a dispatch capability is not available in the current GitHub connector. All native post-fix tests are **NOT RUN**, not FAIL and not PASS.

### Latest verdict

**PARTIAL ADVERSARIAL AUDIT / NO-GO for release.** Source inventory and static review advanced; the original known P007 and an intermediate remediation failed distinct executed tests. New defensive source modifications and new tests are **unverified at the same commit**. The required complete per-control interactive audit, official Steinberg Validator, 125A Tester, realtime p95/p99/max, Studio One project/black-band replay, parallel VST3 instances, physical hardware PCM and second independent source review remain NOT RUN. No P007 bundle or stable branch was overwritten.

## P007 follow-up — exact Windows CI and MIDI raw/mapped lifetime repair (2026-11-19)

**Previous post-fix native Windows validation is now confirmed at exact SHA** `afee54f4b7e32d4fa4aae10321f011652effc549`, [run 44417149724](https://github.com/challanger2000/125A-OB12-VST3/actions/runs/44417149724), **SUCCESS** (the older "NOT RUN" statement applied to an earlier audit HEAD and is preserved as historical context). Native Windows job evidence:
- `AUDIT_FACTORY_RESET_EXPECTED=46 ACTUAL=46 RESTORED=1 STILL_MUTATED=0`, positive reload control PASS.
- `LIBRARY_SWITCH_TEST=PASS direct_playable=1 flash_changed=1 project_restore=1` before and after `setState`.
- `AUDIT_PREBOOT_BANK_SURVIVAL=PASS before=39 after=39 factory=46 ready=1`.
- `AUDIT_LEGACY_FLASH_0..4=PASS` (one positive, four malformed negative cases), `AUDIT_PANEL_CORRUPT_0..3=PASS` plus original panel positive.
- `AUDIT_RESET_WHILE_TRANSFER=PASS` and `AUDIT_BANK_SWITCH_WHILE_SYSEX=PASS` with exact flash unchanged.
- Real firmware-backed VST3 audio/browser/project-state/Win32 lifecycle smoke and direct-bank instrument test PASS **within their stated coverage only**.
- Four native damaged-panel cases are not the same as the eight separate offline reference-parser cases; neither constitutes exhaustive parser fuzzing.

**Additional confirmed source defect F10 (raw/mapped MIDI):** `Plugin::raw_channel_messages` was an instance-lifetime flag. Any single raw channel MIDI event set it to true permanently, so later host `inputParameterChanges` containing valid CC/pitchbend could be silently discarded. An isolated candidate changes duplicate suppression to per-callback scope and adds a native real-VST3 host regression (`P007_MIDI_RAW_THEN_MAPPED_CC`): raw Bank Program A-008 (selected=263), next callback mapped CC0/CC32 to Timbre A, following callback raw Program 8 must save selected=7, not 263. **Native post-change result is NOT RUN until the exact next SHA CI executes.** This is a scoped lifetime fix, not general mixed MIDI stream deduplication: raw channel and mapped changes within the same callback may still be suppressed too broadly, and that broader issue remains NOT VERIFIED.

**Still release-blocking:** F03 blocking mutex/vector allocations/logging in realtime `process()`; F04 event `sampleOffset` is sorted but not sample-scheduled against the feeder's 2304-frame asynchronous lead. Ring producer timing is not proven by calling `render()` in shorter sections, because the emulator already rendered samples in advance. The exact MIDI onsets, min/mean/p95/p99/max callback duration and under/overflow, correct sample-rate host timeline/latency/PDC remain **NOT VERIFIED**. A corrected architecture requires host timeline → timestamped, bounded RT-safe event queue → time-aligned firmware input → 48-kHz producer output → resampled host frames; a hardware MIDI input naturally adds transmission/scheduling latency, so an independent measured tolerance and the real original reference must be established rather than asserting zero delay. DO NOT set `getLatencySamples()` to the nominal lead count without measuring end-to-end behavior. No independent/full review, validator, 125A Tester or real Studio One acceptance occurred in this focused follow-up. Overall **PARTIAL / NO-GO**.

## P007 asynchronous MIDI transport candidate (2026-11-19; exact candidate SHA must be read from Git history)

**Source finding:** Previous `process()` used callback-allocated `std::vector` (including raw data), `stable_sort`, `Engine::midi` with blocking `input_lock` and dynamic `midi_bytes`, and `midi_log()` mutex/files. Its sorted `sampleOffset` events were all delivered before rendering the host block, and the feeder had already pre-rendered the 48-kHz asynchronous ring. This architecture could not promise host offset timing.

**Candidate, isolated audit branch only:** Fixed stack array (2,048 events); bounded SPSC queue (512 messages of max 4,096 bytes), native-frame targets derived from host sample offsets; split 48-kHz emulator rendering at queue boundaries; MIDI firmware I/O and optional timing-probe file writes exclusively on the feeder. Queue overflow reports a count and schedules an all-channel CC123/CC120 release from the feeder. Host bank/program selection is mirrored with atomic fields at accepted RT enqueue so a save immediately after a callback does not wait for the emulator worker.

**Test oracles:** `tools/test_midi_rt_queue.cpp` checks 8/8 accepted vs 9th rejection, FIFO bytes, irregular 48-kHz partitions and exact boundary dispatch. `P007_TEST_TIMING_MUTANT` deliberately restores the old whole-block scheduling and must fail with exit code 7 at the exact-boundary assertion. Real Windows VST3 host smoke additionally checks two incoming sampleOffsets (64, 320 at 44.1 kHz), detecting their independently derived native offset spacing in the actual worker's diagnostic timestamps. **The results of this new candidate are NOT RUN unless GitHub Actions completes on this exact SHA.**

**Crucial evidence limits:** These tests prove native MIDI ingestion/dispatched boundary, NOT acoustic sample-exact NoteOn. The original firmware polls incoming MIDI at its own cadence (~1ms) and MIDI UART behavior may introduce further delay. The existing lead window, host block variation, long-session reconfiguration, measured end-to-end MIDI-to-PCM onset, compensation latency, p95/p99/max ASIO timing, multiple instances and audible fidelity are NOT VERIFIED. No 125A Plugin Tester, Steinberg Validator, full 251-control sweep or actual Studio One test. Release verdict remains PARTIAL / NO-GO.
