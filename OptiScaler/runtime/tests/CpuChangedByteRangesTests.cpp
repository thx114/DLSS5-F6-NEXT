// SPDX-License-Identifier: GPL-3.0-or-later
#include "../CpuChangedByteRanges.h"
#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <utility>
#include <vector>

namespace {
using Range=std::pair<size_t,size_t>;
unsigned checks=0,cases=0;
void Check(bool ok,const char* why) {
    ++checks;
    if(!ok){std::fprintf(stderr,"FAILED case %u: %s\n",cases,why);std::abort();}
}

void Verify(const unsigned char* current,const unsigned char* previous,size_t size) {
    ++cases;
    // Independent byte oracle, with no production mask/boundary helpers.
    std::vector<Range> expected,actual,scalar;
    for(size_t i=0;i<size;) {
        if(current[i]==previous[i]){++i;continue;}
        const auto begin=i++;
        while(i<size && current[i]!=previous[i])++i;
        expected.emplace_back(begin,i);
    }
    FramePipeline::ForEachChangedByteRange(current,previous,size,
        [&](size_t begin,size_t end){actual.emplace_back(begin,end);});
    FramePipeline::detail::ChangedByteRangesScalar(current,previous,size,
        [&](size_t begin,size_t end){scalar.emplace_back(begin,end);});
    Check(actual==expected,"exact maximal changed runs differ from oracle");
    Check(scalar==expected,"scalar fallback differs from oracle");

    // Model the actual driver's writes, with independent storage and canaries.
    // Every changed byte must be written once; every equal byte zero times.
    std::vector<unsigned char> driver(size+2,0xDA),written(size,0);
    bool precise=true;
    for(const auto& [begin,end]:actual) {
        for(size_t i=begin;i<end;++i) {
            precise=precise && current[i]!=previous[i] && !written[i];
            written[i]=1;
        }
        std::memcpy(driver.data()+1+begin,current+begin,end-begin);
    }
    for(size_t i=0;i<size;++i) {
        const bool differs=current[i]!=previous[i];
        precise=precise && written[i]==static_cast<unsigned char>(differs) &&
            driver[i+1]==(differs?current[i]:0xDA);
    }
    Check(precise && driver.front()==0xDA && driver.back()==0xDA,
          "writeback touched an equal byte, duplicated/missed a byte or escaped extent");
}

void Patterns(unsigned char* current,unsigned char* previous,size_t size) {
    std::memset(previous,3,size);
    std::memset(current,3,size);Verify(current,previous,size);
    std::memset(current,7,size);Verify(current,previous,size);
    for(size_t i=0;i<size;++i)current[i]=(i&1)?3:9;
    Verify(current,previous,size);
    // Float-like: three changed mantissa bytes separated by an equal exponent.
    for(size_t i=0;i<size;++i)current[i]=(i%4==3)?3:11;
    Verify(current,previous,size);
    std::memset(current,3,size);
    if(size){current[0]=9;current[size-1]=9;}
    Verify(current,previous,size);
}

void ExhaustiveMasks() {
    std::vector<unsigned char> current(89),previous(89,0);
    for(unsigned mask=0;mask<65536;++mask) {
        for(unsigned bit=0;bit<16;++bit)
            current[bit]=static_cast<unsigned char>((mask>>bit)&1);
        Verify(current.data(),previous.data(),16);

        // Exhaustive mixed masks straddling the 64-byte aggregate boundary,
        // with runs on each side and a remaining scalar tail.
        std::fill(current.begin(),current.end(),static_cast<unsigned char>(0));
        std::fill(current.begin()+49,current.begin()+57,static_cast<unsigned char>(1));
        for(unsigned bit=0;bit<16;++bit)
            current[57+bit]=static_cast<unsigned char>((mask>>bit)&1);
        std::fill(current.begin()+73,current.begin()+81,static_cast<unsigned char>(1));
        Verify(current.data(),previous.data(),current.size());
    }
}

void AdaptiveBoundaries() {
    std::vector<unsigned char> current(257),previous(257,0);
    for(size_t begin:{size_t{0},size_t{13},size_t{63},size_t{64}})
        for(size_t end:{size_t{64},size_t{65},size_t{79},size_t{128}}) {
            std::fill(current.begin(),current.end(),static_cast<unsigned char>(0));
            std::fill(current.begin()+begin,current.begin()+end,static_cast<unsigned char>(1));
            // The second full block forces scalar continuation while its
            // first changed run may belong to the previous SIMD block.
            for(size_t i=80;i<128;i+=2)current[i]=1;
            current.back()=1;
            Verify(current.data(),previous.data(),current.size());
        }
}

void AlignmentsAndRandom() {
    constexpr size_t sizes[]={0,1,2,15,16,17,31,32,33,47,48,49,63,64,65,
                              79,80,81,127,128,129,255,256,257,511,512,513};
    std::vector<unsigned char> current(8192+64),previous(8192+64);
    // Every independent SIMD alignment, with lengths around vector and block edges.
    for(size_t a=0;a<16;++a)for(size_t b=0;b<16;++b)
        for(const auto size:sizes)Patterns(current.data()+a,previous.data()+b,size);
    // Every independent cache-line offset as well, across multiple blocks.
    for(size_t a=0;a<64;++a)for(size_t b=0;b<64;++b)
        Patterns(current.data()+a,previous.data()+b,193);

    std::mt19937 random(754123);
    constexpr unsigned probabilities[]={0,1,16,64,128,240,255,256};
    for(unsigned iteration=0;iteration<2048;++iteration) {
        const auto size=static_cast<size_t>(random()%8193);
        const auto a=random()%64,b=random()%64;
        const auto probability=probabilities[iteration%8];
        for(size_t i=0;i<size;++i) {
            previous[b+i]=static_cast<unsigned char>(random());
            current[a+i]=previous[b+i];
            if(random()%256<probability)
                current[a+i]^=static_cast<unsigned char>(1+random()%255);
        }
        Verify(current.data()+a,previous.data()+b,size);
    }
    // Long runs must stay maximal across many 64-byte blocks and the tail.
    for(const size_t size:{size_t{65537},size_t{1048579}}) {
        current.resize(size+64);previous.resize(size+64);
        Patterns(current.data()+7,previous.data()+29,size);
        std::memset(current.data()+7,3,size);
        std::memset(current.data()+7+13,7,size-26);
        Verify(current.data()+7,previous.data()+29,size);
    }
}

struct GuardedPage {
    unsigned char* allocation=nullptr;
    unsigned char* bytes=nullptr;
    explicit GuardedPage(size_t page) {
        allocation=static_cast<unsigned char*>(VirtualAlloc(nullptr,page*3,
            MEM_COMMIT|MEM_RESERVE,PAGE_NOACCESS));
        Check(allocation!=nullptr,"guarded allocation");
        bytes=allocation+page;
        DWORD protection=0;
        Check(VirtualProtect(bytes,page,PAGE_READWRITE,&protection)!=0,"guarded middle page");
    }
    ~GuardedPage(){VirtualFree(allocation,0,MEM_RELEASE);}
    GuardedPage(const GuardedPage&)=delete;
    GuardedPage& operator=(const GuardedPage&)=delete;
};

void GuardPages() {
    SYSTEM_INFO info{};GetSystemInfo(&info);
    const size_t page=info.dwPageSize;
    GuardedPage a(page),b(page);
    std::vector<unsigned char> ordinary(page+64);
    // Both ends are protected. All short extents put each possible final
    // vector/tail alignment immediately before PAGE_NOACCESS.
    for(size_t size=1;size<=257;++size)
        Patterns(a.bytes+page-size,b.bytes+page-size,size);
    for(const size_t size:{size_t{511},size_t{512},size_t{513},size_t{1023},page-1,page}) {
        Patterns(a.bytes+page-size,b.bytes+page-size,size);
        Patterns(a.bytes,b.bytes,size);
    }
    // A zero extent must not dereference even non-null inaccessible pointers.
    Verify(a.bytes+page,b.bytes+page,0);
    // Independently misalign the other input while each input in turn ends at
    // a guard. This catches a comparison that only bounds one of its loads.
    for(size_t offset=0;offset<16;++offset)for(size_t size=1;size<=129;++size) {
        Patterns(a.bytes+page-size,ordinary.data()+offset,size);
        Patterns(ordinary.data()+offset,b.bytes+page-size,size);
        Patterns(a.bytes,ordinary.data()+offset,size);
        Patterns(ordinary.data()+offset,b.bytes,size);
    }
    // Inputs are snapshots, not scratch space: reads must also work on RO pages.
    std::memset(a.bytes,1,page);std::memset(b.bytes,1,page);
    a.bytes[63]=2;a.bytes[page-1]=2;
    DWORD protection=0;
    Check(VirtualProtect(a.bytes,page,PAGE_READONLY,&protection)!=0 &&
          VirtualProtect(b.bytes,page,PAGE_READONLY,&protection)!=0,"read-only snapshots");
    Verify(a.bytes,b.bytes,page);
}
}

int main() {
    Verify(nullptr,nullptr,0);
    ExhaustiveMasks();
    AdaptiveBoundaries();
    AlignmentsAndRandom();
    GuardPages();
    std::printf("CPU changed byte ranges: %u checks / %u cases; exhaustive masks, "
        "independent oracle, exact writes, scalar fallback, all offsets and guard pages\n",checks,cases);
}
