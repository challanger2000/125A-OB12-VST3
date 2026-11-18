#include "TMS57070.h"
#include <windows.h>
#undef min
#undef max
#include <vector>
#include <map>
#include <string>
#include <fstream>
#include <chrono>
#include <cmath>
#include <cstdlib>
using namespace TMS57070;
static std::map<std::pair<std::string,int>,long> unk; static long cur_pc=-1; static std::map<std::pair<int,long>,int> firstinsn;
void unk_hit(int l,const char*f){ std::string s(f); auto p=s.find_last_of("\/"); if(p!=s.npos)s=s.substr(p+1); unk[{s,l}]++; firstinsn[{l,cur_pc}]++; }
static std::vector<uint8_t> rd(const std::string&p){ std::ifstream f(p,std::ios::binary); return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),{}); }
static Emulator* G=nullptr; static long G_steps=0;
static LONG WINAPI crash(EXCEPTION_POINTERS* x){ printf("relrip %llx ",(unsigned long long)x->ExceptionRecord->ExceptionAddress-(unsigned long long)GetModuleHandleA(NULL)); printf("base %p off %llx ",G,(unsigned long long)x->ExceptionRecord->ExceptionInformation[1]-(unsigned long long)G); printf("CRASH code %08lX addr %p rw=%llu target %llx pc %03lx insn %08X steps %ld XOFF %X\n",x->ExceptionRecord->ExceptionCode,x->ExceptionRecord->ExceptionAddress,(unsigned long long)x->ExceptionRecord->ExceptionInformation[0],(unsigned long long)x->ExceptionRecord->ExceptionInformation[1],cur_pc,G?G->PMEM[cur_pc&511]:0,G_steps,G?G->XOFF:0); fflush(stdout); ExitProcess(9); return 0; }
static long outs=0; static long chn[8]; static double che[8]; static int32_t chmax[8]; static long exin=0,exout=0;
static void onout(Channel c,int32_t v){ int i=(int)c&7; chn[i]++; che[i]+=(double)v*v; if(abs(v)>chmax[i])chmax[i]=abs(v); outs++; }
static int32_t onin(uint32_t a){ exin++; return 0; }
static void onext(int32_t v,uint32_t a){ exout++; }
int main(int c,char**v){
  std::string d=v[1]; std::string prog=v[2]; int samples=atoi(v[3]);
  SetUnhandledExceptionFilter(crash); auto*e=new Emulator(); G=e; e->reset(); e->register_sample_out_callback(onout); e->register_external_bus_in_callback(onin); e->register_external_bus_out_callback(onext);
  auto p=rd(d+"/"+prog+".pmem.bin"), m=rd(d+"/"+prog+".cmem.bin");
  for(size_t i=0;i<p.size()/4;i++) e->PMEM[i]=p[i*4]<<24|p[i*4+1]<<16|p[i*4+2]<<8|p[i*4+3];
  for(size_t i=0;i<m.size()/3;i++) { int32_t w=m[i*3]<<16|m[i*3+1]<<8|m[i*3+2]; e->CMEM[i].value=w; }
  auto hd=rd(d+"/"+prog+".hdr.bin"); if(c>4){ e->CR0.value=hd[0]<<16|hd[1]<<8|hd[2]; e->CR1.value=hd[3]<<16|hd[4]<<8|hd[5]; e->CR2.value=hd[6]<<16|hd[7]<<8|hd[8]; e->CR3.value=hd[9]<<16|hd[10]<<8|hd[11]; e->update_mac_modes(); printf("hdr CR0=%06X CR1=%06X CR2=%06X CR3=%06X EXT=%d EXTMEM=%d XWORD=%d XBUS=%d\n",(unsigned)e->CR0.value,(unsigned)e->CR1.value,(unsigned)e->CR2.value,(unsigned)e->CR3.value,e->CR1.EXT,e->CR1.EXTMEM,e->CR3.XWORD,e->CR3.XBUS);}
  long disc=0,lastpc=-1; std::map<int,long> jumpto; std::vector<long> vis(512,0); long steps=0; long maxpc=0;
  auto t0=std::chrono::high_resolution_clock::now();
  for(int s=0;s<samples;s++){
    int32_t in=(int32_t)(0.5*sin(s*0.05)*0x7FFFFF);
    e->sample_in(Channel::in_1L,in); e->sample_in(Channel::in_1R,in); e->sample_in(Channel::in_2L,in); e->sample_in(Channel::in_2R,in);
    for(int i=0;i<512;i++){ cur_pc=e->PC.value; if(lastpc>=0 && cur_pc!=((lastpc+1)&511)){disc++; jumpto[(int)cur_pc]++;} lastpc=cur_pc; vis[e->PC.value]++; e->step(); steps++; G_steps=steps; }
  }
  auto t1=std::chrono::high_resolution_clock::now();
  double ns=std::chrono::duration<double,std::nano>(t1-t0).count()/steps;
  int cov=0; for(size_t i=0;i<p.size()/4;i++) if(vis[i])cov++;
  int dm=0; for(int i=0;i<512;i++) if(e->DMEM[i].value)dm++;
  int cmchg=0; for(size_t i=0;i<m.size()/3;i++){int32_t w=m[i*3]<<16|m[i*3+1]<<8|m[i*3+2]; int24_t t; t.value=w; if(t.value!=e->CMEM[i].value)cmchg++;}
  printf("%s: words %zu visited %d (%.1f%%) steps %ld %.1f ns/step DMEM nonzero %d CMEM changed %d PC end %d SP %d\n",prog.c_str(),p.size()/4,cov,100.0*cov/(p.size()/4),steps,ns,dm,cmchg,(int)e->PC.value,(int)e->SP);
  printf("  out calls %ld ext in %ld out %ld;",outs,exin,exout); for(int i=0;i<8;i++) if(chn[i]) printf(" ch%d n=%ld rms=%.0f max=%X",i,chn[i],sqrt(che[i]/chn[i]),chmax[i]);
  printf("\n  unknown paths %zu\n",unk.size());
  for(auto&k:unk) printf("   %s:%d hits %ld\n",k.first.first.c_str(),k.first.second,k.second);
  int n=0; for(auto&k:firstinsn){ if(n++>12)break; long pc=k.first.second; printf("   line %d at PC %03lx insn %08X\n",k.first.first,pc,pc>=0?e->PMEM[pc]:0);} 
  // never-visited pcs summary
  std::string unv; int run=0; for(size_t i=0;i<p.size()/4;i++){ if(!vis[i]){ if(!run){char b[16];sprintf(b," %03zx",i);unv+=b;} run++;} else { if(run){char b[16];sprintf(b,"+%d",run);unv+=b;} run=0; } } if(run){char b[16];sprintf(b,"+%d",run);unv+=b;}
  printf("  discontinuities %ld; targets:",disc); {int n=0; for(auto&k:jumpto){ if(n++>8)break; printf(" %03x x%ld",k.first,k.second);} } printf(" int-stack SP=%d",(int)e->SP); puts(""); printf("  unvisited:%s\n",unv.substr(0,300).c_str());
}
