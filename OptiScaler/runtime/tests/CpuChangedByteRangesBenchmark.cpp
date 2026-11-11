// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone CPU snapshot microbenchmark; build optimized (/O2), outside the
// product/runner. Results do not measure driver mappings or game frame time.
#include "../CpuChangedByteRanges.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace {
// Frozen pre-SIMD implementation for a meaningful same-binary baseline.
template<class Changed>
void BaselineScalar(const unsigned char* current,const unsigned char* previous,
                    size_t size,Changed&& changed) {
    for(size_t i=0;i<size;) {
        if(current[i]==previous[i]){++i;continue;}
        const auto begin=i++;
        while(i<size && current[i]!=previous[i])++i;
        changed(begin,i);
    }
}
template<class Changed>
void Baseline(const unsigned char* current,const unsigned char* previous,
              size_t size,Changed&& changed) {
    constexpr size_t Block=256;
    size_t i=0,fragmentBegin=0;
    unsigned fragments=0;
    while(i<size) {
        while(size-i>=Block && current[i]==previous[i] &&
              std::memcmp(current+i,previous+i,Block)==0)i+=Block;
        while(i<size && current[i]==previous[i])++i;
        if(i==size)return;
        const auto begin=i++;
        while(i<size && current[i]!=previous[i])++i;
        changed(begin,i);
        if(i-fragmentBegin>=Block){fragmentBegin=i;fragments=0;}
        else if(++fragments==8) {
            const auto offset=i;
            BaselineScalar(current+offset,previous+offset,size-offset,
                [&](size_t first,size_t last){changed(offset+first,offset+last);});
            return;
        }
    }
}

struct Result {
    size_t runs=0,bytes=0;
    std::uint64_t hash=0;
    bool operator==(const Result& other) const {
        return runs==other.runs && bytes==other.bytes && hash==other.hash;
    }
};
using Scan=Result(*)(const unsigned char*,const unsigned char*,size_t,unsigned char*);
#if defined(_MSC_VER)
#define CHANGED_RANGES_NOINLINE __declspec(noinline)
#elif defined(__GNUC__)
#define CHANGED_RANGES_NOINLINE __attribute__((noinline))
#else
#define CHANGED_RANGES_NOINLINE
#endif
template<bool Optimized>
CHANGED_RANGES_NOINLINE Result ScanRanges(const unsigned char* current,const unsigned char* previous,
                                        size_t size,unsigned char* driver) {
    Result result;
    const auto emit=[&](size_t begin,size_t end) {
        ++result.runs;result.bytes+=end-begin;
        result.hash=(result.hash*0x100000001B3ULL)^begin^(end*0x9E3779B97F4A7C15ULL);
        if(driver)std::memcpy(driver+begin,current+begin,end-begin);
    };
    if constexpr(Optimized)FramePipeline::ForEachChangedByteRange(current,previous,size,emit);
    else Baseline(current,previous,size,emit);
    return result;
}
#undef CHANGED_RANGES_NOINLINE

volatile std::uint64_t sink=0;
double Time(Scan scan,const unsigned char* current,const unsigned char* previous,size_t size,
            unsigned char* driver,size_t iterations) {
    std::uint64_t checksum=0;
    const auto start=std::chrono::steady_clock::now();
    for(size_t i=0;i<iterations;++i) {
        // Compiler barrier only: don't hoist the unchanged-input scan out of
        // the loop. No hardware fence or timer is added inside the scanner.
        std::atomic_signal_fence(std::memory_order_seq_cst);
        const auto r=scan(current,previous,size,driver);
        checksum^=r.hash+r.runs+r.bytes+i;
    }
    const auto end=std::chrono::steady_clock::now();
    sink=sink^checksum;
    return std::chrono::duration<double,std::milli>(end-start).count();
}

double MedianNs(Scan scan,const unsigned char* current,const unsigned char* previous,
                size_t size,unsigned char* driver) {
    size_t iterations=1;
    while(Time(scan,current,previous,size,driver,iterations)<10.0 && iterations<(size_t{1}<<24))
        iterations*=2;
    double samples[3];
    for(auto& ms:samples)ms=Time(scan,current,previous,size,driver,iterations);
    std::sort(samples,samples+3);
    return samples[1]*1000000.0/static_cast<double>(iterations);
}

void Fill(const char* pattern,unsigned char* current,unsigned char* previous,size_t size) {
    std::mt19937 random(61473);
    for(size_t i=0;i<size;++i) {
        previous[i]=static_cast<unsigned char>(random());
        current[i]=previous[i];
        bool change=false;
        if(std::strcmp(pattern,"sparse")==0)change=(i%4096==31);
        else if(std::strcmp(pattern,"dense")==0)change=true;
        else if(std::strcmp(pattern,"fragmented")==0)change=(i%2==0);
        else if(std::strcmp(pattern,"floatlike")==0) {
            // Finite float representations with independently changing
            // mantissas and a common high byte, rather than all-different bytes.
            const std::uint32_t a=0x3F000000u|(random()&0x007FFFFFu);
            const std::uint32_t b=0x3F000000u|(random()&0x007FFFFFu);
            const size_t count=(std::min)(size-i,sizeof(a));
            std::memcpy(previous+i,&a,count);std::memcpy(current+i,&b,count);
            i+=count-1;
            continue;
        } else if(std::strcmp(pattern,"floatpattern")==0) {
            // Continuous float array with a small per-frame perturbation.
            const float a=1.0f+static_cast<float>(i/sizeof(float))*0.0001f;
            const float b=a+0.00005f;
            const size_t count=(std::min)(size-i,sizeof(a));
            std::memcpy(previous+i,&a,count);std::memcpy(current+i,&b,count);
            i+=count-1;
            continue;
        }
        if(change)current[i]^=0x5A;
    }
}
}

int main() {
    std::puts("CPU snapshots only; median of 3 calibrated trials per algorithm, no pass/fail speed threshold");
    std::puts("pattern,bytes,offset_current,offset_previous,writeback,runs,changed_bytes,baseline_ns,optimized_ns,speedup");
    for(const size_t size:{size_t{64},size_t{256},size_t{4096},size_t{65536},size_t{1048576}})
        for(const char* pattern:{"sparse","dense","floatlike","floatpattern","unchanged","fragmented"}) {
            std::vector<unsigned char> a(size+64),b(size+64),oldDriver(size,0xDA),newDriver(size,0xDA);
            auto* current=a.data()+7;auto* previous=b.data()+29;
            Fill(pattern,current,previous,size);
            for(bool copy:{false,true}) {
                auto* oldOutput=copy?oldDriver.data():nullptr;
                auto* newOutput=copy?newDriver.data():nullptr;
                const auto baseline=ScanRanges<false>(current,previous,size,oldOutput);
                const auto optimized=ScanRanges<true>(current,previous,size,newOutput);
                if(!(baseline==optimized) || oldDriver!=newDriver) {
                    std::fprintf(stderr,"baseline/result mismatch: %s %zu\n",pattern,size);
                    return EXIT_FAILURE;
                }
                // Alternate measurement order across sizes/modes to limit bias.
                double oldNs=0,newNs=0;
                if((size/64+static_cast<size_t>(copy))&1) {
                    newNs=MedianNs(ScanRanges<true>,current,previous,size,newOutput);
                    oldNs=MedianNs(ScanRanges<false>,current,previous,size,oldOutput);
                } else {
                    oldNs=MedianNs(ScanRanges<false>,current,previous,size,oldOutput);
                    newNs=MedianNs(ScanRanges<true>,current,previous,size,newOutput);
                }
                std::printf("%s,%zu,7,29,%s,%zu,%zu,%.1f,%.1f,%.3f\n",pattern,size,
                    copy?"yes":"no",baseline.runs,baseline.bytes,oldNs,newNs,oldNs/newNs);
            }
        }
    std::printf("checksum=%llu\n",static_cast<unsigned long long>(sink));
}
