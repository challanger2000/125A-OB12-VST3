#include "TMS57070.h"
#include <vector>
#include <string>
#include <fstream>
#include <cmath>
#include <cstdlib>
#include <cstdio>
using namespace TMS57070;
void unk_hit(int,const char*){}
static std::vector<uint8_t> rd(const std::string&p){ std::ifstream f(p,std::ios::binary); return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),{}); }
static std::vector<int32_t> O[6];
static void onout(Channel c,int32_t v){ int i=(int)c-(int)Channel::out_1L; if(i>=0&&i<6) O[i].push_back(v); }
static int32_t onin(uint32_t){return 0;} static void onext(int32_t,uint32_t){}
static const char* nm[6]={"AX1L","AX1R","AX2L","AX2R","AX3L","AX3R"};
int main(int c,char**v){
  std::string d=v[1],prog=v[2]; int frames=atoi(v[3]);
  auto*e=new Emulator(); e->reset(); e->register_sample_out_callback(onout); e->register_external_bus_in_callback(onin); e->register_external_bus_out_callback(onext);
  auto p=rd(d+"/"+prog+".pmem.bin"),m=rd(d+"/"+prog+".cmem.bin"),hd=rd(d+"/"+prog+".hdr.bin");
  for(size_t i=0;i<p.size()/4;i++) e->PMEM[i]=p[i*4]<<24|p[i*4+1]<<16|p[i*4+2]<<8|p[i*4+3];
  for(size_t i=0;i<m.size()/3;i++) e->CMEM[i].value=m[i*3]<<16|m[i*3+1]<<8|m[i*3+2];
  e->CR0.value=hd[0]<<16|hd[1]<<8|hd[2]; e->CR1.value=hd[3]<<16|hd[4]<<8|hd[5]; e->CR2.value=hd[6]<<16|hd[7]<<8|hd[8]; e->CR3.value=hd[9]<<16|hd[10]<<8|hd[11]; e->update_mac_modes();
  e->CR2.FREE=0;
  std::vector<int32_t> I[4];
  for(int f=0;f<frames;f++) for(int h=0;h<2;h++){
    int k=f*2+h;
    int32_t a=(int32_t)(0.5*sin(k*0.031)*0x7FFFFF), b=(int32_t)(0.4*sin(k*0.047+1)*0x7FFFFF), cc=(int32_t)(0.3*sin(k*0.071+2)*0x7FFFFF), dd=(int32_t)(0.2*sin(k*0.113+3)*0x7FFFFF);
    e->AR1L.value=a; e->AR1R.value=b; e->AR2L.value=cc; e->AR2R.value=dd;
    I[0].push_back(a);I[1].push_back(b);I[2].push_back(cc);I[3].push_back(dd);
    for(int i=0;i<256;i++) e->step();
  }
  const char* in[4]={"AR1L","AR1R","AR2L","AR2R"};
  for(int o=0;o<6;o++){ size_t n=O[o].size(); double s=0; for(auto x:O[o]) s+=(double)x*x; printf("%s: writes %zu rms %.0f",nm[o],n,n?sqrt(s/n):0);
    int bi=-1,bs=0,bl=0; double best=0;
    for(int i=0;i<4;i++) for(int st=1;st<=2;st++) for(int l=0;l<6;l++){ long eq=0,t=0; for(size_t k=8;k<n;k++){ size_t q=k*st; if(q<(size_t)l||q-l>=I[i].size())continue; t++; if(O[o][k]==I[i][q-l])eq++; } if(t>100){double r=(double)eq/t; if(r>best){best=r;bi=i;bs=st;bl=l;}} }
    if(bi>=0) printf("  best exact match %s stride %d lag %d: %.1f%%",in[bi],bs,bl,best*100); puts(""); }
}
