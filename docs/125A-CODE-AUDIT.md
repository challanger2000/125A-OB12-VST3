# 125A OB-12 — source audit and preset browser (development branch)

Base: \`Tuth/ob12-emu\` main, imported without firmware/ROM assets. License: GPL-3.0; original attribution preserved.

## Findings from the original sources

| Severity | Location | Evidence / original problem | Development-branch action |
|---|---|---|---|
| High | \`src/ob12_vst3.cpp\` | Component \`getState/setState\` returned success but serialized **nothing**. Reloaded DAW projects could not recover their private user flash. | Added versioned state with checksum, exact flash snapshot and last observed MIDI selection. Requires host tests. |
| High | \`src/ob12_vst3.cpp\` | Every plugin instance loaded \`%LOCALAPPDATA%\OB12\flash.bin\`; multiple projects shared mutable memory. | Restored project images now use isolated memory; unsaved/new instances still use the legacy global image. |
| High | \`src/ob12_engine.cpp\` | MIDI-file SysEx parser read lengths and positions without complete bounds validation; truncated files could overrun buffers. | Validates SMF headers, track/event lengths, VLQs and complete SysEx messages. |
| Medium | \`src/ob12_vst3.cpp\` | ROM fallback was fixed to the original author's \`F:\Claude\OB-12\rom\`. | Replaced it with portable bundle-relative lookup. |
| Medium | \`src/ob12_vst3.cpp\` | An edit controller and component are facets of the same COM object. Memory restoration must never invalidate \`PanelView\`'s stored Engine reference. | Added in-place Engine replacement, feeder stop/restart, and gating of audio callbacks during reinitialization. Pending Windows host stress tests. |
| Medium | \`tools/build_*.bat\` | Original build scripts reference the original author's machine paths. | Added relocatable \`tools/build_125A.bat\` using VS2022 clang-cl and \`OB12_VST3SDK\`. |

## New preset browser

The original sound generator (F2MC-16LX CPUs and five TMS57070 DSPs) is unchanged.
The browser is implemented in \`src/preset_browser.h\`, opened through the panel right-click menu **Preset Browser...**.
The page contains Programs/Timbres, banks A/B, searchable 128-slot lists, persistent favorites,
and a guarded loader for matching \`12prgXX.mid\` + \`12tmbXX.mid\` files.
Selection uses the documented OB-12 Bank Select MSB/LSB + Program Change on MIDI channel 1.
This requires the original firmware's **MIDI Program Map** to be disabled for standard slot numbering.

For program names the browser uses a conservative common-offset extractor on 256-record images:
\`rom/presets/12prgFS.bin\` and \`12tmbFS.bin\`. For imported update files it can
decode the documented OB-12 7+1-bit SysEx packing to create an index.
If names do not pass validation the browser **shows slot IDs**, rather than inventing names.
**This name inference still needs verification with authentic complete factory/user bank files.**

**Caution:** Loading a paired bank sends a complete SysEx stream into the emulated hardware;
it overwrites matching memory. This is **not** yet a non-destructive multi-bank library and
does not provide instantaneous cross-bank audition. Projects should be backed up beforehand.

## Remaining issues / blockers

1. **Source compile and link:** Windows 2022 CI compiles the modified files against VST3 SDK 3.7.12 and links a real Windows VST3 DLL without ROM assets (GitHub Actions run 44323882494: PASS).
   Successful compilation and linkage do not establish functional audio performance.
2. **No firmware-backed release plugin** is provided in this branch; firmware is copyright
   Viscount and is not stored in Git. Supply it locally before running \`tools/build_125A.bat\`.
3. **DAW tests outstanding:** Steinberg validator, Studio One 8.1.2 note playback,
   multiple instances, editor open while restoring state, current preset recall,
   128 Program/Timbre slots per bank and crossbank MIDI Program Change.
4. **Memory recall caveat:** The state records the last MIDI bank/Program Change,
   not all selections performed through the emulated hardware's front panel;
   a firmware-level active-location observer is still needed.
5. **Realtime-safety audit pending:** \`Plugin::process()\` creates vectors for MIDI
   event lists, and the engine's MIDI input uses a mutex and resizable vector;
   event sample offsets are sorted but not delayed to their exact sample in rendering.
6. **Factory reset mismatch:** The original reset-memory operation deletes the
   global flash file, but project-private flash images have a separate ownership model.
   A safe project-specific reset requires an explicit implementation and test.
7. **Latency:** \`getLatencySamples()\` reports 0 while the asynchronous ring is
   operated with \`lead_frames=2304\` at 48 kHz. Actual end-to-end MIDI latency
   and correct host compensation require a measurement before changing the value.
8. **Program name heuristic and import:** Compare decoded display names with
   the actual LCD for several factory and third-party banks; verify paired import
   boundaries, queuing and post-import flash writes.
9. **Audio regression:** Compare identical MIDI streams in the upstream emulator
   and this fork before claiming bit-identical sound. No DSP code was intentionally changed.

## Local build

Set \`OB12_VST3SDK\` to the VST3 SDK root. Put your personal original firmware and
generated DSP assets in \`rom/\`, then run \`tools\build_125A.bat\` on Windows with Visual Studio 2022
LLVM components and Python 3 installed.

This branch deliberately does not claim a verified Windows VST3 or DAW-ready release.


---

## Nachtrag 2026-11-18: P007 adversarialer Audit nach 125A-Engineering

**Urteil: NO-GO / PARTIAL / NOT VERIFIED. Kein FULL AUDIT, kein QA PASS.**

**Exakte Produktquelle:** feature/preset-browser-audit@04d16b5594782593d9972799fe122c165a6a40d7. Die P007-Produktquelle bleibt unveraendert. Audit-only-Pruefzweig: audit/p007-adversarial-20261118.

**Inventar und Limitation:** 94 Git-Dateien (22 src, 62 tools, 1 CI, 4 docs, 5 Root), 53 Regler/Slider, 85 Taster, 96 LEDs, 10 Browser- und 7 Menue-Elemente = 251 UI/Hardware-Eintraege. Hardware-Routen statisch eindeutig, aber keine 251 Einzelwirkungen ausgefuehrt. Source-Review: Kernpfade/Hosttests untersucht, nicht jede historische CPU-/DSP-Funktion und alle Tools vollstaendig unabhaengig auditiert.

**High F-01, reproduzierter Vertragsbruch:** Panel-Werksreset ruft Engine::forget_flash auf, das nur eine alte Flash-Dateireferenz loescht, nicht aber die private emulierte Flashkopie. Echter nativer Windows-Gegentest aendert Program-Flash bei 0xD0003, ruft den Resetpfad auf, misst vorher 0x46 und nachher 0x39: STILL_MUTATED=1, RESTORED=0. Die echte Positivkontrolle ueber Engine::reload(originalSnapshot) stellt das alte Image korrekt wieder her: PASS. Der neue Audit-Check beendet sich deshalb absichtlich mit Exitcode 9 und der Job ist zu Recht ROT.

**Nachweis:** https://github.com/challanger2000/125A-OB12-VST3/actions/runs/44381951577. AUDIT_CONTRACT_RESET_MEMORY=FAIL; AUDIT_POSITIVE_RESTORE_CONTROL=PASS. Dagegen waren im selben Run die bisherigen Firmware-Audio-, Browser-, State- und Bank-Smokes PASS.

**High F-02, Quellcode:** Plugin::process verwendet resizierbare Vectoren, stable_sort, durch midi_log() einen Mutex im Audiocallback und Engine::midi mit Mutex/Vector-Anhang. Vertrag zur Realtime-Sicherheit verletzt; p95/p99/Overrun-Messungen NOT RUN.
**High F-03, Quellcode:** Midi sampleOffset sortiert, aber alle Ereignisse vor dem Render des gesamten Audioblocks versendet. Samplegenaue Audiobestaetigung NOT RUN.
**High F-04, Quellcode:** panel.ob12p-Loader ohne Boundschecks fuer Recordanzahl/Kommandos, Legacy-flash.bin-Loader mit potenziell ueberlaufendem start+size vor memcpy. Crash-Execution NOT RUN.
**Medium F-05, Quellcode:** raw_channel_messages bleibt dauerhaft aktiv und unterdrueckt spaetere VST3 ParameterChanges. Mix-Hosttest NOT RUN.
**Medium F-06, Testadäquanz:** P007-Library-Switch-Test aendert nur Preset-Namensbytes und prueft Text/Flash/Enabled, nicht die PCM-Klangunterschiede aus sechs Originalbanken. P007-Hostfenster-Test ist eine Mock-Simulation, keine Studio-One-Abnahme.

**Echte separate Offline-Positiv- und Negativkontrollen:** 12/12 originale private Programm-/Timbre-MIDI-Dateien mit unabhängigem MIDI/SysEx-Decoder geprueft (Checksummen, Adressen, Laenge, 7+1-Gruppierung). 4 absichtliche RAM-Mutationen (Checksumme, MIDI-Laenge, Adresse mit korrigierter Checksumme, fehlender Endmarker mit korrigierter Checksumme) wurden alle begruendet abgewiesen. Dies beweist das Datei-Format, NICHT den nativen C++-Klang aller Bänke.

**Getrennte Gates:** P007 Windows-Basissmoke: PASS in seinem engen Umfang (run 44376294031). Factory-Reset: FAIL. Realtime-Sicherheit und MIDI-Offsets: SOURCE FAIL. Vollständige Quell-/Control-Auditdeckung: NOT VERIFIED. Steinberg-Validator: NOT RUN. 125A-Plugin-Tester: NOT RUN. Echtes Studio One 8, Multiinstanz-/Black-Border-Praxistest: NOT RUN. Exakte Audio-/Hardwarevergleiche aller Bänke: NOT RUN. Unabhaengiger Zweitpruefer: NOT RUN. RELEASE READY: NO-GO.

**Folge:** Den reproduzierten Factory-Reset-Check vor weiterer Freigabe als roten Release-Gate beibehalten. P007-Produktion nicht stillschweigend mit einem GREEN-CI-Satz als FULL/FINAL bewerten. Dieser Nachtrag ist eine datierte Korrektur, der historische Bericht oben bleibt erhalten.
