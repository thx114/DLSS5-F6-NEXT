// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

namespace FramePipeline {
struct CpuIndexBounds { uint32_t first=0,last=0; };
// Reads only the caller's immutable CPU-proven image. Bounded stack storage
// amortizes fragmented-image traversal without mapping GPU memory or caching
// geometry across draw/content versions. Failure leaves the result unchanged.
template<class Read>
bool ReadCpuIndexBounds(uint32_t begin,uint32_t count,uint32_t width,int32_t base,
                        bool strip,Read&& read,CpuIndexBounds& result) {
    constexpr auto max=(std::numeric_limits<uint32_t>::max)();
    if(!count||(width!=2&&width!=4)||uint64_t(begin)+uint64_t(count)*width>max)return false;
    std::array<unsigned char,1024> bytes{};
    uint32_t first=max,last=0;bool found=false;
    for(uint32_t done=0;done<count;){
        const auto n=(std::min)(count-done,uint32_t(bytes.size()/width));
        const auto offset=uint32_t(uint64_t(begin)+uint64_t(done)*width);
        if(!read(bytes.data(),offset,offset+n*width))return false;
        for(uint32_t i=0;i<n;++i){
            uint32_t value=0;
            if(width==2){uint16_t v=0;std::memcpy(&v,bytes.data()+i*width,2);value=v;}
            else std::memcpy(&value,bytes.data()+i*width,4);
            if(strip&&value==(width==2?0xffffu:max))continue;
            const auto vertex=int64_t(value)+base;
            if(vertex<0||uint64_t(vertex)>max)return false;
            const auto v=uint32_t(vertex);first=(std::min)(first,v);last=(std::max)(last,v);found=true;
        }
        done+=n;
    }
    if(!found)return false;
    result={first,last};return true;
}
} // namespace FramePipeline
