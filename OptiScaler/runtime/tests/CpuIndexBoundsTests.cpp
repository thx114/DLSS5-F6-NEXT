// SPDX-License-Identifier: GPL-3.0-or-later
#include "../CpuIndexBounds.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <random>

namespace {
unsigned checks=0;
void Check(bool value){++checks;if(!value)std::abort();}
bool Oracle(const std::vector<unsigned char>& data,uint32_t begin,uint32_t count,uint32_t width,
            int32_t base,bool strip,FramePipeline::CpuIndexBounds& output){
    if(!count||(width!=2&&width!=4)||uint64_t(begin)+uint64_t(count)*width>data.size())return false;
    uint32_t first=UINT32_MAX,last=0;bool found=false;
    for(uint32_t i=0;i<count;++i){
        uint32_t v=0;for(uint32_t b=0;b<width;++b)v|=uint32_t(data[begin+i*width+b])<<(b*8);
        if(strip&&v==(width==2?0xffffu:UINT32_MAX))continue;
        const auto n=int64_t(v)+base;if(n<0||uint64_t(n)>UINT32_MAX)return false;
        first=(std::min)(first,uint32_t(n));last=(std::max)(last,uint32_t(n));found=true;
    }
    if(found)output={first,last};return found;
}
}
int main(){
    std::mt19937 random(20261108);
    for(uint32_t width:{2u,4u})for(bool strip:{false,true})
    for(uint32_t count:{1u,255u,256u,257u,511u,512u,513u,2049u})
    for(unsigned trial=0;trial<40;++trial){
        const uint32_t begin=trial%7;std::vector<unsigned char> data(begin+count*width);
        for(uint32_t i=0;i<count;++i){
            uint32_t value=random()%50000;
            if(trial%4==0&&i%3==0)value=width==2?0xffffu:UINT32_MAX;
            if(trial%9==0)value=width==2?0xffffu:UINT32_MAX;
            std::memcpy(data.data()+begin+i*width,&value,width);
        }
        const int32_t base=trial%3==0?-300:trial%3==1?200:0;
        FramePipeline::CpuIndexBounds expected{77,88},actual=expected;
        const bool ok=Oracle(data,begin,count,width,base,strip,expected);
        unsigned reads=0;
        const bool got=FramePipeline::ReadCpuIndexBounds(begin,count,width,base,strip,
            [&](void* target,uint32_t low,uint32_t high){
                ++reads;Check(high>low&&high-low<=1024&&high<=data.size());
                std::memcpy(target,data.data()+low,high-low);return true;
            },actual);
        Check(got==ok&&actual.first==expected.first&&actual.last==expected.last);
        if(got)Check(reads==(count*width+1023)/1024);
        actual={77,88};unsigned fails=0;
        Check(!FramePipeline::ReadCpuIndexBounds(begin,count,width,base,strip,
            [&](void*,uint32_t,uint32_t){++fails;return false;},actual)&&fails==1&&actual.first==77&&actual.last==88);
    }
    for(auto width:{0u,1u,3u,8u}){
        FramePipeline::CpuIndexBounds result{};
        Check(!FramePipeline::ReadCpuIndexBounds(0,1,width,0,false,
            [](void*,uint32_t,uint32_t){Check(false);return false;},result));
    }
    FramePipeline::CpuIndexBounds result{};
    auto unreachable=[](void*,uint32_t,uint32_t){Check(false);return false;};
    Check(!FramePipeline::ReadCpuIndexBounds(0,0,2,0,false,unreachable,result));
    Check(!FramePipeline::ReadCpuIndexBounds(UINT32_MAX-2,2,2,0,false,unreachable,result));
    std::printf("CpuIndexBounds: %u checks passed; scalar oracle and bounded immutable reads\n",checks);
}
