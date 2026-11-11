// SPDX-License-Identifier: GPL-3.0-or-later
// CPU storage/journal contracts only. No D3D, SDK, GPU completion or game proof.
#include "../CpuSnapshotBytes.h"
#include "../MappedResourceJournal.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

namespace {
unsigned checks=0;
void Check(bool condition,const char* message) {
    ++checks;if(!condition){std::fprintf(stderr,"FAILED: %s\n",message);std::abort();}
}
struct Transaction {
    std::shared_ptr<FramePipeline::CpuSnapshotBytes> bytes;
    std::shared_ptr<unsigned> owner;
    bool returned=false;
};
Transaction Make(unsigned value=7) {
    Transaction result{std::make_shared<FramePipeline::CpuSnapshotBytes>(64),std::make_shared<unsigned>(value)};
    std::memset(result.bytes->data(),static_cast<int>(value),64);return result;
}
using Journal=FramePipeline::MappedResourceJournal<Transaction,2>;
constexpr FramePipeline::MappedResourceKey first{11,21,0},second{11,22,0};
}
int main() {
    {
        FramePipeline::CpuSnapshotBytes bytes(560);
        Check(bytes.size()==560 && reinterpret_cast<uintptr_t>(bytes.data())%16==0,"exact size and D3D mapped alignment");
        bool zero=true;for(size_t i=0;i<bytes.size();++i)zero=zero && bytes.data()[i]==0;
        Check(zero,"default snapshot initialization");
        bytes.data()[559]=42;const auto* address=bytes.data();
        auto moved=std::move(bytes);
        Check(!bytes.data() && bytes.size()==0,"moved-from storage has no stale extent");
        Check(moved.data()==address && moved.data()[559]==42,"moving a snapshot preserves pointer and content");
    }
    {
        Journal journal;auto original=Make();std::weak_ptr<unsigned> owner=original.owner;
        auto* data=original.bytes->data();
        Check(!journal.HasEntries(),"empty journal");
        Check(journal.TryInsert(first,std::move(original)),"accept exact original map");
        Check(journal.HasEntries() && !original.bytes,"journal owns admitted transaction");
        auto duplicate=Make(8);bool committed=false;
        Check(!journal.TryInsert({11,21,1},std::move(duplicate),[&]{committed=true;return true;}),"reject duplicate resource even for different subresource");
        Check(!committed && duplicate.bytes,"duplicate never commits or consumes caller storage");
        Check(!journal.Take({12,21,0}).bytes && !journal.Take({11,21,1}).bytes,"wrong context or subresource cannot consume mapping");
        Check(!owner.expired(),"rejected consumer retains mandatory writeback owner");
        auto taken=journal.Take(first);
        Check(taken.bytes && taken.bytes->data()==data && !journal.HasEntries(),"take exact transaction without copying storage");
        Check(!journal.Take(first).bytes,"transaction consumed once");
        auto snapshot=taken.bytes;taken={};
        Check(owner.expired() && snapshot->data()==data && snapshot->data()[0]==7,"snapshot outlives writeback transaction independently");
    }
    {
        Journal journal;auto a=Make(),b=Make(),c=Make();
        Check(journal.TryInsert(first,std::move(a)) && journal.TryInsert(second,std::move(b)),"fixed two-slot capacity");
        bool committed=false;
        Check(!journal.TryInsert({11,23,0},std::move(c),[&]{committed=true;return true;}),"full journal refuses instead of expanding");
        Check(!committed && c.bytes,"capacity refusal leaves transaction and pointer unchanged");
        auto taken=journal.Take(first);Check(taken.bytes!=nullptr,"free a full journal slot");auto rejected=Make();
        Check(!journal.TryInsert(first,std::move(rejected),[]{return false;}),"owner revision refusal cancels admission");
        Check(rejected.bytes && journal.TryInsert(first,std::move(c)),"refused commit leaves slot reusable");
        Check(journal.Take(second).bytes && journal.Take(first).bytes && !journal.HasEntries(),"all actual admissions retire exactly once");
    }
    {
        Journal perContext;auto a=Make(),b=Make();
        Check(perContext.TryInsert(first,std::move(a)) && perContext.TryInsert({12,21,0},std::move(b)),"context-resource conflict policy preserved");
        FramePipeline::MappedResourceJournal<Transaction,2,FramePipeline::MapConflict::Resource> perResource;
        a=Make();b=Make();
        Check(perResource.TryInsert(first,std::move(a)) && !perResource.TryInsert({12,21,0},std::move(b)),"resource-wide conflict policy preserved");
    }
    {
        Journal journal;auto mapped=Make(31);auto snapshot=mapped.bytes;
        Check(journal.TryInsert(first,std::move(mapped)),"admit before owner/config loss");
        std::array<unsigned char,64> driver{};
        // A consumer on another thread may still owe writeback. Journal does
        // not infer optional frame eligibility from a thread or CPU API return.
        std::thread consumer([&]{
            auto taken=journal.Take(first);
            if(taken.bytes)std::memcpy(driver.data(),taken.bytes->data(),driver.size());
        });consumer.join();
        Check(driver[0]==31 && driver[63]==31 && !journal.HasEntries(),"owner-independent exact take preserves required bytes");
        Check(snapshot->data()[0]==31,"writeback does not mutate retained snapshot");
        auto invalid=Make();Check(!journal.TryInsert({0,21,0},std::move(invalid)) && invalid.bytes,"reject missing context without consuming storage");
    }
    std::printf("CpuMappingTransactions: %u checks passed\n",checks);
}
