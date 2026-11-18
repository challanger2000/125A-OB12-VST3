// Integration test using genuine OB-12 factory bank contents inside a transient
// original-format MIDI stream. No firmware or sounds are checked into Git.
#include "ob12_engine.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static std::vector<uint8_t> read_binary(const std::string& path) {
    std::ifstream f(path,std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),{});
}

int main(int argc,char** argv) {
    if (argc != 7) {
        puts("usage: bank_import_smoke <romdir> <tmb.mid> <prg.mid> <tmb.bin> <prg.bin> <program|timbre|pair>");
        return 2;
    }
    const std::string romdir = argv[1];
    const std::string mode=argv[6];
    if(mode!="program" && mode!="timbre" && mode!="pair") return 2;
    std::vector<uint8_t> tmb,prg;
    const std::string tmb_error = Engine::read_sysex(std::wstring(argv[2],argv[2]+strlen(argv[2])),tmb);
    const std::string prg_error = Engine::read_sysex(std::wstring(argv[3],argv[3]+strlen(argv[3])),prg);
    if(!tmb_error.empty()||!prg_error.empty()||tmb.size()<1000||prg.size()<1000) {
        printf("BANK_PARSE_FAIL timbre=%s program=%s size=%zu/%zu\n",
               tmb_error.c_str(),prg_error.c_str(),tmb.size(),prg.size());
        return 3;
    }
    std::vector<uint8_t> expected_tmb=read_binary(argv[4]);
    std::vector<uint8_t> expected_prg=read_binary(argv[5]);
    if(expected_tmb.size()!=42752||expected_prg.size()!=40448) return 4;

    Engine e(romdir);
    e.detach_flash_file();
    std::vector<int32_t> audio(240*2);
    auto advance = [&](int seconds) {
        for(int i=0;i<seconds*200;i++) e.run(audio.data(),240);
    };
    advance(13);
    std::vector<uint8_t> before;
    if(!e.sounding_ready() || !e.snapshot_flash(before)) {
        puts("BANK_TEST_FAIL engine failed to boot");
        return 5;
    }
    long baseline=e.flash_writes();
    printf("BANK_TEST_START mode=%s boot_writes=%ld sysex_bytes=%zu/%zu\n",mode.c_str(),baseline,tmb.size(),prg.size());
    fflush(stdout);
    // Firmware updates may reboot the instrument after each section. Test the
    // exact browser's current FIFO implementation rather than pretending that
    // independent stages have already been proven.
    if(mode=="pair") e.send_sysex_pair(tmb.data(),(int)tmb.size(),prg.data(),(int)prg.size(),10);
    else if(mode=="program") e.send_sysex(prg.data(),(int)prg.size(),10);
    else e.send_sysex(tmb.data(),(int)tmb.size(),10);
    bool seen_pending=false;
    bool drained=false;
    int drained_at=-1;
    long lastwrites=baseline;
    for(int simulated=0;simulated<340;simulated++) {
        advance(1);
        int waiting=e.sysex_waiting();
        long writes=e.flash_writes();
        if(waiting>0) seen_pending=true;
        if(simulated%20==0) {
            printf("BANK_TEST_PROGRESS simulated=%d sec, queued=%d, flashes=%ld, ready=%d\n",
                simulated+1,waiting,writes-baseline,e.sounding_ready());
            fflush(stdout);
        }
        if(seen_pending&&waiting==0&&!drained) {
            drained=true;
            drained_at=simulated;
        }
        if(drained && simulated-drained_at>=25) break;
        lastwrites=writes;
    }
    std::vector<uint8_t> after;
    if(!e.snapshot_flash(after)||after.size()!=0x100000) return 6;
    auto compare = [&](const std::vector<uint8_t>& expected, size_t offset, const char* label) {
        size_t diffs = 0;
        size_t first = 0;
        for (size_t i=0;i<expected.size();i++) if (expected[i]!=after[offset+i]) {
            if (!diffs) first=i;
            ++diffs;
        }
        printf("FLASH_COMPARE_%s diff=%zu first=0x%zx expected=%02X got=%02X, before=%02X\n",
               label,diffs,first,expected[first],after[offset+first],before[offset+first]);
        return diffs == 0;
    };
    const bool timbre_exact = compare(expected_tmb,0xC0000,"TIMBRE");
    // The original CPU normalizes byte 0xA6FD (FF->7F) while importing the
    // original factory image, preserving all 256 Timbre records otherwise.
    bool normalized = !timbre_exact && expected_tmb[0xA6FD] == 0xFF &&
                      after[0xC0000 + 0xA6FD] == 0x7F;
    if (normalized) for (size_t i = 0; i < expected_tmb.size(); ++i) {
        if (i != 0xA6FD && expected_tmb[i] != after[0xC0000 + i]) normalized = false;
    }
    const bool tmb_ok = timbre_exact || normalized;
    printf("TIMBRE_FIRMWARE_NORMALIZATION=%s\n", normalized ? "ONE_BYTE_ACCEPTED" : "NONE");
    const bool prg_ok=compare(expected_prg,0xD0000,"PROGRAM");
    long writes=e.flash_writes()-baseline;
    const long min_writes = mode=="pair" ? 40000 : mode=="timbre" ? 21000 : 20000;
    const bool regions_ok = (mode=="pair" && tmb_ok && prg_ok) ||
                            (mode=="program" && prg_ok) || (mode=="timbre" && tmb_ok);
    const bool success=seen_pending&&drained&&e.sounding_ready()&&
                       writes>=min_writes&&regions_ok;
    printf("BANK_IMPORT_TEST=%s seen_pending=%d drained=%d writes=%ld timbre_bytes=%s program_bytes=%s ready=%d\n",
        success?"PASS":"FAIL",seen_pending,drained,writes,tmb_ok?"PASS":"FAIL",
        prg_ok?"PASS":"FAIL",e.sounding_ready());
    return success?0:7;
}
