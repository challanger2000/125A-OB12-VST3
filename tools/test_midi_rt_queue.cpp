// Native release-active positive + deliberate test-only timing mutation.
#include "midi_rt_queue.h"
#include <array>
#include <cstdio>
#include <cstring>

static bool require(bool valid, const char* description) {
    if (!valid) std::printf("P007_MIDI_QUEUE_FAIL=%s\n", description);
    return valid;
}
int main() {
    MidiRtQueue<8,4096> q;
    const uint8_t note[] = {0x90, 0x3C, 0x7F};
    if (!require(q.empty(),"initially empty")) return 1;
    for (int i=0; i<8; ++i) if (!require(q.push(i,note,sizeof(note)),"first 8 accepted")) return 2;
    if (!require(!q.push(8,note,sizeof(note)),"ninth rejected, old payload retained")) return 3;
    for (int i=0; i<8; ++i) {
        const auto* e=q.front();
        if (!require(e && e->frame==(uint64_t)i && e->length==3 &&
                     std::memcmp(e->bytes.data(),note,3)==0,"FIFO payload identity")) return 4;
        q.pop();
    }
    if (!require(q.empty(),"drained")) return 5;
    q.reset_after_stop();
    const uint64_t targets[]={0,7,31,31,127};
    for(auto target:targets)if(!require(q.push(target,note,3),"timed append"))return 6;
    const int parts[]={1,3,9,1,17,11,47,80};
    uint64_t actual=0;
    int consumed=0;
    for(int p:parts) {
        int remaining=p;
        while(remaining>0) {
            auto* e=q.front();
            if(e && e->frame<=actual) {
                if(!require(e->frame==actual,"exact 48-kHz event boundary"))return 7;
                ++consumed;q.pop();continue;
            }
            int n=q.frames_before_next(actual,remaining);
            if(!require(n>0 && n<=remaining,"bounded native render segment"))return 8;
            actual+=n;remaining-=n;
        }
    }
    while(auto* e=q.front()) {if(!require(e->frame==actual,"final boundary"))return 9; ++consumed;q.pop();}
    if(!require(consumed==5 && q.empty(),"all timed messages dispatched"))return 10;
    std::puts("P007_MIDI_QUEUE_TIMING_AND_BOUNDS=PASS");

    // Overflow is a recovery FENCE, not an immediate CC123. A panic before
    // the accepted NoteOn backlog would leave those accepted notes ringing.
    q.reset_after_stop();
    for(int i=0;i<8;++i)
        if(!require(q.push(100,note,3),"overload fixture filled"))return 11;
    if(!require(!q.push(100,note,3) && q.recovery_pending(),"overflow requests recovery"))return 12;
    if(!require(!q.finish_recovery_after_drain(),"recovery cannot overtake accepted notes"))return 14;
    for(int i=0;i<8;++i) {
        if(i==4 && !require(!q.push(101,note,3),"no new voices during recovery"))return 15;
        if(!require(!q.finish_recovery_after_drain(),"no early panic until ALL notes delivered"))return 14;
        if(!require(q.front()!=nullptr,"retained accepted note"))return 16;
        q.pop();
    }
    if(!require(q.finish_recovery_after_drain(),"one recovery after backlog drained"))return 17;
    if(!require(!q.recovery_pending() && q.empty(),"recovery rearmed"))return 18;
    if(!require(q.push(102,note,3),"new notes accepted after recovery"))return 19;
    std::puts("P007_MIDI_OVERFLOW_RECOVERY_ORDER=PASS");
    return 0;
}
