// Targeted audit-only executable, NOT part of the shipping VST3.
// Tests a requirement independently from the GUI/Engine implementation:
// "Reset memory to factory" must genuinely reset private project flash,
// not only drop a legacy shared-file path and preserve the mutated patch.
//
// This deliberately returns non-zero while the product violates the contract.
// A RED audit CI job is evidence of the defect, not a failed source compilation.
#include "ob12_engine.h"
#include "panel_view.h"
#include <algorithm>
#include <fstream>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>


// Test only: write user-owned temporary files under the ephemeral CI directory.
static void u32(std::ofstream& out, uint32_t value) {
    out.write(reinterpret_cast<const char*>(&value), 4);
}
static bool audit_legacy_flash(const std::string& rom,
                               const std::vector<uint8_t>& base) {
    const std::string path = "build/audit-p007-legacy-memory.bin";
    constexpr uint32_t at = 0xD0003;
    const uint8_t changed = base[at] ^ 0x55;
    auto probe = [&](int kind, bool expect_changed) {
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out.write("OB12FLSH", 8);
            if (kind == 0) { // valid 1-byte transaction: positive parser control
                u32(out, at); u32(out, 1); out.put((char)changed);
            } else if (kind == 1) { // 32-bit wrapping start+size bypass attempt
                u32(out, 0xFFFFFFF0u); u32(out, 0x20u);
                for (int i=0;i<32;++i) out.put((char)0xCC);
            } else if (kind == 2) { // first valid record, then truncated second
                u32(out, at); u32(out, 1); out.put((char)changed);
                u32(out, 0xC0000); out.put((char)0x1u);
            } else if (kind == 3) { // overlapping duplicate update
                u32(out, at); u32(out, 1); out.put((char)changed);
                u32(out, at); u32(out, 1); out.put((char)0xAA);
            } else if (kind == 4) { // a declared 1MiB payload truncated at 1 byte
                u32(out, 0); u32(out, 0x100000u); out.put((char)0);
            } else return false;
        }
        Engine loaded(rom);
        loaded.set_flash_file(path);
        std::vector<uint8_t> snapshot;
        const bool captured = loaded.snapshot_flash(snapshot);
        const bool passed = captured &&
            snapshot.size() == base.size() &&
            (expect_changed ? snapshot[at] == changed : snapshot == base);
        std::printf("AUDIT_LEGACY_FLASH_%d=%s expected_change=%d\n",
                    kind,passed ? "PASS" : "FAIL",(int)expect_changed);
        std::remove(path.c_str());
        return passed;
    };
    for (int k=0;k<=4;++k)
        if (!probe(k, k == 0)) return false;
    std::puts("AUDIT_LEGACY_FLASH_BOUNDS_AND_ROLLBACK=PASS");
    return true;
}
static bool audit_panel_art(const std::string& rom, Engine& e) {
    const std::string stock = rom + "/panel.ob12p";
    std::ifstream input(stock, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
    if (bytes.size()<32) return false;
    PanelView known(e, stock);
    if (!known.has_valid_art()) {
        std::puts("AUDIT_PANEL_POSITIVE_CONTROL=FAIL original art rejected");
        return false;
    }
    const std::string path = "build/audit-p007-corrupt-panel.ob12p";
    for (int kind=0;kind<4;++kind) {
        std::vector<char> broken=bytes;
        if (kind==0) broken.resize(16); // valid header but missing run bodies
        else if (kind==1) {
            uint32_t huge=0xFFFFFFFFu;
            std::memcpy(broken.data()+4,&huge,4); // count causes huge allocation
        } else if (kind==2) {
            float bad = std::nanf("");
            std::memcpy(broken.data()+8,&bad,4); // invalid image width
        } else {
            // First run has only its fixed header; its fake command count
            // must never cause a pointer walk past the actual data.
            uint32_t too_many=0xFFFFFFFEu;
            std::memcpy(broken.data()+16+4+1+4,&too_many,4);
        }
        {
            std::ofstream o(path,std::ios::binary|std::ios::trunc);
            o.write(broken.data(),(std::streamsize)broken.size());
        }
        PanelView candidate(e,path);
        bool rejected=!candidate.has_valid_art();
        std::printf("AUDIT_PANEL_CORRUPT_%d=%s\n",
                    kind,rejected?"PASS":"FAIL");
        std::remove(path.c_str());
        if (!rejected) return false;
    }
    std::puts("AUDIT_PANEL_BOUNDED_PARSER=PASS");
    return true;
}

static bool audit_preset_owner(const std::string& rom) {
    const uint8_t host_program[] = {
        0xb0, 0x00, 0x40, 0xb0, 0x20, 0x00, 0xc0, 0x07
    };
    Engine host_first(rom);
    host_first.observe_host_midi(host_program, (int)sizeof host_program);
    const int before = host_first.selected_preset();
    const bool old_restore_rejected = !host_first.select_saved_preset_if_unset(134);
    const int after = host_first.selected_preset();
    const bool host_wins = old_restore_rejected && before == 263 && after == 263;
    std::printf("AUDIT_PRESET_HOST_BEFORE_DELAYED_RESTORE=%s before=%d after=%d\\n",
                host_wins?"PASS":"FAIL",before,after);
    if (!host_wins) return false;

    Engine saved_first(rom);
    const bool saved = saved_first.select_saved_preset_if_unset(134);
    const int selected = saved_first.selected_preset();
    const bool not_twice = !saved_first.select_saved_preset_if_unset(134);
    saved_first.observe_host_midi(host_program, (int)sizeof host_program);
    const int final = saved_first.selected_preset();
    const bool next_host_wins = saved && selected == 134 && not_twice && final == 263;
    std::printf("AUDIT_PRESET_SAVED_THEN_LIVE_MIDI=%s saved=%d final=%d\\n",
                next_host_wins?"PASS":"FAIL",selected,final);
    return next_host_wins;
}

int main(int argc, char** argv) {
    if (argc != 2) { std::puts("usage: audit_p007_adversarial <rom>"); return 2; }
    if (!audit_preset_owner(argv[1])) return 18;
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
    if (!audit_legacy_flash(argv[1],baseline)) return 16;
    if (!audit_panel_art(argv[1],e)) return 17;

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
