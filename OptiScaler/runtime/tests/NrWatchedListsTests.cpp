#include <shaders/dlssnr/DlssNr_WatchedLists.h>
#include <array>
#include <barrier>
#include <future>
#include <iostream>
#include <thread>
int main()
{
    std::array<int,160> objects {};
    unsigned checks=0;
    const auto require=[&](bool ok){++checks;if(!ok){std::cerr << "watch index failed at " << checks;std::exit(1);}};
    for(unsigned round=0;round<20;++round)
    {
        for(auto& object:objects) DlssNrWatchedLists::Add(&object);
        int unrelated=0;const void* other=&unrelated;
        require(!DlssNrWatchedLists::Any(1,&other));
        for(auto& object:objects){const void* p=&object;require(DlssNrWatchedLists::Any(1,&p));}
        // Removing fast slots does not forget identities that overflowed.
        for(unsigned i=0;i<64;++i){const void* p=&objects[i];DlssNrWatchedLists::Remove(1,&p);}
        const void* tail=&objects.back();require(DlssNrWatchedLists::Any(1,&tail));
        for(auto& object:objects){const void* p=&object;DlssNrWatchedLists::Remove(1,&p);require(!DlssNrWatchedLists::Any(1,&p));}
        require(DlssNrWatchedLists::g_overflowCount.load()==0);
        require(!DlssNrWatchedLists::Any(1,&other));
    }
    for (unsigned i=0;i<65;++i) DlssNrWatchedLists::Add(&objects[i]);
    const void* first=&objects[0];DlssNrWatchedLists::Remove(1,&first);
    DlssNrWatchedLists::Add(&objects[64]);
    unsigned copies=0;
    for(auto& entry:DlssNrWatchedLists::g_lists)copies+=entry.load()==&objects[64];
    {std::lock_guard lock(DlssNrWatchedLists::g_overflowMutex);
        copies+=static_cast<unsigned>(DlssNrWatchedLists::g_overflow.count(&objects[64]));}
    require(copies==1);
    for(auto& object:objects){const void* p=&object;DlssNrWatchedLists::Remove(1,&p);}

    // Actual publication precedes the reader's simulated submission gate.
    // Successful Reset/removal then precedes its negative membership check.
    std::barrier handoff(2);
    std::jthread submitter([&]{
        for(unsigned round=0;round<256;++round){
            const void* p=&objects[round%objects.size()];
            handoff.arrive_and_wait();require(DlssNrWatchedLists::Any(1,&p));
            handoff.arrive_and_wait();handoff.arrive_and_wait();
            require(!DlssNrWatchedLists::Any(1,&p));handoff.arrive_and_wait();
        }
    });
    for(unsigned round=0;round<256;++round){
        const void* p=&objects[round%objects.size()];DlssNrWatchedLists::Add(p);
        handoff.arrive_and_wait();handoff.arrive_and_wait();
        DlssNrWatchedLists::Remove(1,&p);handoff.arrive_and_wait();handoff.arrive_and_wait();
    }
    submitter.join();

    // A reader queries both stores while the single lifecycle writer mutates
    // other identities. Unknown batches never acquire NR even during overflow.
    for(unsigned i=0;i<80;++i)DlssNrWatchedLists::Add(&objects[i]);
    std::atomic<bool> done=false,valid=true;
    std::promise<void> sampled;
    std::jthread reader([&]{
        int unrelated=0;const void* miss[]{nullptr,&unrelated};
        const void* hit[]{&unrelated,&objects[79]};
        bool firstSample=true;
        while(!done.load(std::memory_order_acquire)){
            if(DlssNrWatchedLists::Any(2,miss)||!DlssNrWatchedLists::Any(2,hit))valid=false;
            if(firstSample){sampled.set_value();firstSample=false;}
        }
    });
    sampled.get_future().get();
    for(unsigned round=0;round<2048;++round){
        const void* p=&objects[80+round%80];
        DlssNrWatchedLists::Add(p);DlssNrWatchedLists::Remove(1,&p);
    }
    done.store(true,std::memory_order_release);reader.join();require(valid.load());
    for(auto& object:objects){const void* p=&object;DlssNrWatchedLists::Remove(1,&p);}
    require(DlssNrWatchedLists::g_overflowCount.load()==0);
    require(DlssNrWatchedLists::g_overflowPeak.load()>=96);
    std::cout << "NR exact watched identities: " << checks << " checks passed; overflow, handoff and concurrent readers\n";
}
