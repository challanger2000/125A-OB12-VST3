// Targeted audit-only executable, NOT part of the shipping VST3.
// Tests a requirement independently from the GUI/Engine implementation:
// "Reset memory to factory" must genuinely reset private project flash,
// not only drop a legacy shared-file path and preserve the mutated patch.
//
// This deliberately returns non-zero while the product violates the contract.
// A RED audit CI job is evidence of the defect, not a failed source compilation.
#include "ob12_engine.h"
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) { std::puts("usage: audit_p007_adversarial <rom>"); return 2; }
    Engine e{std::string(argv[1])};
    e.detach_flash_file();
    std::vector<uint8_t> baseline;
    if (!e.snapshot_flash(baseline) || baseline.size() != 0x100000) {
        std::puts("AUDIT_PRECONDITION=FAIL baseline flash unavailable");
        return 3;
    }

    std::vector<uint8_t> programs(baseline.begin()+0xD0000,
                                  baseline.begin()+0xD0000+40448);
    std::vector<uint8_t> timbres(baseline.begin()+0xC0000,
                                 baseline.begin()+0xC0000+42752);
    const uint8_t original = programs[3];
    programs[3] ^= 0x7f; // one observable byte change; don't need musical fixtures
    if (!e.apply_bank_pair(programs,timbres)) {
        std::puts("AUDIT_PRECONDITION=FAIL could not load modified private bank");
        return 4;
    }
    std::vector<uint8_t> changed;
    if (!e.snapshot_flash(changed) || changed.size()!=baseline.size() ||
        changed[0xD0003] == original || changed[0xD0003] != programs[3]) {
        std::puts("AUDIT_PRECONDITION=FAIL mutation was not committed to flash");
        return 5;
    }
    // This is exactly what PanelView::reset_memory invokes after the user
    // presses OK. Run the original CPU input loop for >1 ms so the queued
    // flag is definitely processed before snapshotting.
    if (!e.forget_flash()) {
        std::puts("AUDIT_RESET_REQUEST=FAIL even though no update was active");
        return 6;
    }
    std::vector<uint8_t> reset;
    if (!e.snapshot_flash(reset)) return 6;
    const bool restored = reset == baseline;
    const bool still_mutated = reset[0xD0003] == programs[3];
    std::printf("AUDIT_FACTORY_RESET_EXPECTED=%02X ACTUAL=%02X "
                "RESTORED=%d STILL_MUTATED=%d\n",
                (unsigned)original,(unsigned)reset[0xD0003],
                (int)restored,(int)still_mutated);
    // Control: engine reload CAN actually restore the original snapshot.
    // Thus the oracle detects a real flash mutation and a working restoration.
    const bool explicit_restore = e.reload(argv[1], baseline);
    std::vector<uint8_t> restored_control;
    const bool control_pass = explicit_restore &&
        e.snapshot_flash(restored_control) && restored_control==baseline;
    std::printf("AUDIT_POSITIVE_RESTORE_CONTROL=%s\n",
                control_pass ? "PASS" : "FAIL");
    if (!control_pass) return 7;
    if (!restored && still_mutated) {
        std::puts("AUDIT_CONTRACT_RESET_MEMORY=FAIL "
                  "command does not restore factory data in private flash");
        return 9;
    }
    if (!restored) {
        std::puts("AUDIT_CONTRACT_RESET_MEMORY=FAIL changed but nonfactory flash");
        return 10;
    }
    std::puts("AUDIT_CONTRACT_RESET_MEMORY=PASS");
    // Negative/failure control: while a firmware update is queued the factory
    // reset must be rejected and the exact 1-MiB project image left unchanged.
    const uint8_t pending_sysex[] = {0xf0,0x31,0x30,0x00,0x68,0x36,0xf7};
    e.send_sysex(pending_sysex, (int)sizeof pending_sysex);
    const bool rejected = !e.forget_flash();
    std::vector<uint8_t> after_reject;
    const bool intact = e.snapshot_flash(after_reject) &&
                        after_reject == baseline;
    std::printf("AUDIT_RESET_WHILE_TRANSFER=%s request_rejected=%d "
                "flash_unchanged=%d\n",
                rejected && intact ? "PASS" : "FAIL",
                (int)rejected,(int)intact);
    if (!rejected || !intact) return 11;

    // Independent boot-race oracle: a genuine non-Factory patch in the flash
    // must survive the original firmware's startup, not just snapshot correctly
    // before the first 12 seconds of original CPU execution.
    Engine cold{std::string(argv[1])};
    cold.detach_flash_file();
    const bool staged = cold.apply_bank_pair(programs, timbres);
    std::vector<uint8_t> staged_state;
    const bool staged_saved = cold.snapshot_flash(staged_state) &&
        staged_state.size() == baseline.size() &&
        staged_state[0xD0003] == programs[3];
    if (!staged || !staged_saved) {
        std::puts("AUDIT_PREBOOT_BANK=FAIL not persisted before CPU boot");
        return 12;
    }
    std::vector<int32_t> audio(240*2);
    for (int i = 0; i < 13*200; ++i) cold.run(audio.data(), 240);
    std::vector<uint8_t> after_boot;
    const bool boot_ok = cold.sounding_ready() &&
        cold.snapshot_flash(after_boot) &&
        after_boot.size() == baseline.size() &&
        after_boot[0xD0003] == programs[3];
    std::printf("AUDIT_PREBOOT_BANK_SURVIVAL=%s before=%02X after=%02X "
                "factory=%02X ready=%d\n",
                boot_ok ? "PASS" : "FAIL",(unsigned)programs[3],
                after_boot.size() == baseline.size() ?
                  (unsigned)after_boot[0xD0003] : 0u,
                (unsigned)baseline[0xD0003],(int)cold.sounding_ready());
    if (!boot_ok) return 13;

    // A queued original SysEx transmission must reject an external
    // direct-install operation atomically, not only reject factory reset.
    std::vector<uint8_t> before_rejected_install;
    if (!e.snapshot_flash(before_rejected_install)) return 14;
    const bool install_rejected = !e.apply_bank_pair(programs, timbres);
    std::vector<uint8_t> after_rejected_install;
    const bool install_intact = e.snapshot_flash(after_rejected_install) &&
        before_rejected_install == after_rejected_install;
    std::printf("AUDIT_BANK_SWITCH_WHILE_SYSEX=%s request_rejected=%d intact=%d\n",
                install_rejected && install_intact ? "PASS" : "FAIL",
                (int)install_rejected,(int)install_intact);
    return install_rejected && install_intact ? 0 : 15;
}
