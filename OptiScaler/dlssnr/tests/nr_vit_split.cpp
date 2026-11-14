#include "../DlssNrVitReuse.h"
#include <array>
#include <cstdio>
#include <stdexcept>

using namespace DlssNrVitReuse;
unsigned checks=0;
void Check(bool ok,const char* message)
{
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
struct Launch
{
    Role role;
    unsigned id;
    inline static unsigned copies=0;
    Launch(Role value,unsigned number):role(value),id(number) {}
    Launch(const Launch& other):role(other.role),id(other.id) {++copies;}
    Launch& operator=(const Launch& other) {role=other.role;id=other.id;++copies;return *this;}
};
int main() try
{
    const std::array<Launch,6> chain {{{Role::None,0},{Role::Start,1},{Role::Inner,2},
                                    {Role::Inner,3},{Role::End,4},{Role::None,5}}};
    for (const auto set:{KernelSet::Fp8,KernelSet::Plain})
        for (unsigned interval:{1u,2u,3u})
            for (size_t chunk=1;chunk<=chain.size();++chunk)
            {
                Filter split,single;
                for (unsigned frame=0;frame<24;++frame)
                {
                    const bool reset=frame==0 || frame==11;
                    std::vector<unsigned> splitIds,singleIds;
                    std::vector<size_t> splitBarriers,singleBarriers;
                    split.Begin(&chain,reset,interval,frame,interval);
                    for(size_t first=0;first<chain.size();first+=chunk)
                    {
                        const size_t count=std::min(chunk,chain.size()-first);
                        std::vector<Launch> kept;
                        std::ptrdiff_t gap=-1;
                        Launch::copies=0;
                        const bool changed=split.Split(chain.data()+first,count,[&](const Launch& launch) {
                            return std::pair{launch.role,set};
                        },kept,gap);
                        if (!changed)
                        {
                            Check(kept.empty() && kept.capacity()==0 && Launch::copies==0,
                                "unchanged kernel chain copies and allocates nothing");
                            Check(gap==-1,"unchanged kernel chain has no barrier");
                            for(size_t i=0;i<count;++i) splitIds.push_back(chain[first+i].id);
                        }
                        else
                        {
                            if(gap>=0)splitBarriers.push_back(splitIds.size()+static_cast<size_t>(gap));
                            for(const auto& launch:kept)splitIds.push_back(launch.id);
                        }
                    }
                    const bool splitAnomaly=split.End();
                    single.Begin(&chain,reset,interval,frame,interval);
                    for(const auto& launch:chain)
                    {
                        const auto result=single.One(launch.role,set);
                        if(result.barrier)singleBarriers.push_back(singleIds.size());
                        if(result.launch)singleIds.push_back(launch.id);
                    }
                    Check(single.End()==splitAnomaly,"split and single keep anomaly state");
                    Check(splitIds==singleIds,"lazy chain preserves actual launched order");
                    Check(splitBarriers==singleBarriers,"lazy chain preserves real UAV barrier placement");
                    Check(split.Computed()==single.Computed() && split.Reused()==single.Reused(),
                        "every kernel still updates the original reuse state machine");
                }
            }
    std::printf("PASS %u ViT lazy-chain checks.\n",checks);
    return 0;
}
catch(const std::exception& error)
{
    std::fprintf(stderr,"FAIL: %s\n",error.what());
    return 1;
}
