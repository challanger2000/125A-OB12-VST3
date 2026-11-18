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
