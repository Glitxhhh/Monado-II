// SPDX-License-Identifier: GPL-2.0-or-later
// Test the actual provider rewrite against the actual locally prepared modules.
#include "../../third_party/fsr4_native/portable_dot.h"
#include <cstdlib>
#include <cstdio>
#define CHECK(x) do { if(!(x)){std::fprintf(stderr,"portable-dot FAIL line %d\n",__LINE__);std::abort();} } while(0)
#include <fstream>
#include <string>
int main(int argc,char**argv){
    CHECK(argc==3);std::ifstream in(argv[1],std::ios::binary|std::ios::ate);CHECK(in);
    auto size=in.tellg();CHECK(size>0&&size%4==0&&size<16000000);in.seekg(0);
    std::vector<uint32_t>w(size_t(size)/4);CHECK(in.read((char*)w.data(),size));auto before=w;
    CHECK(Fsr411::PortableDot(w));
    for(size_t i=5;i<w.size();){auto n=w[i]>>16,op=w[i]&65535;CHECK(n&&i+n<=w.size());CHECK(op!=6916);CHECK(!(op==17&&n==2&&w[i+1]==6912));i+=n;}
    std::ofstream out(argv[2],std::ios::binary);CHECK(out.write((char*)w.data(),w.size()*4));
    // Bad magic, zero-length/truncated instructions and missing types fail closed.
    auto bad=before;bad[0]=0;CHECK(!Fsr411::PortableDot(bad));
    bad=before;bad[5]=0;CHECK(!Fsr411::PortableDot(bad));
    bad=before;bad.resize(6);bad[5]=(8u<<16)|6916;CHECK(!Fsr411::PortableDot(bad));
}
