// SPDX-License-Identifier: GPL-3.0-or-later
// Native D3D11 WARP adapter regression. No game, NGX, NR or FG execution.
#include "../../genshin/GenshinDepthPortableResources.h"
#include "../CpuIndexBounds.h"
#include <cstdio>
#include <cstdlib>
#include <array>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace GenshinLegacy {
bool testInputRequested=true;
FramePipeline::InputObservationDemand ReadNewInputObservationDemand()noexcept {return {testInputRequested};}
}
namespace {
unsigned checks=0;
void Check(bool condition,const char* message) {
    ++checks;if(!condition){std::fprintf(stderr,"FAILED: %s\n",message);std::abort();}
}
struct Device {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    Device(){Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,
        nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"create native software D3D11 device");}
    ComPtr<ID3D11Buffer> Buffer(UINT width=64) {
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=width;desc.Usage=D3D11_USAGE_DYNAMIC;
        desc.BindFlags=D3D11_BIND_VERTEX_BUFFER;desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Buffer> buffer;
        Check(SUCCEEDED(device->CreateBuffer(&desc,nullptr,&buffer)),"create real dynamic vertex buffer");
        GenshinDepthPortable::Created(buffer.Get(),nullptr);return buffer;
    }
    template<size_t Size=64> std::array<unsigned char,Size> ReadDriver(ID3D11Buffer* buffer) {
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=static_cast<UINT>(Size);desc.Usage=D3D11_USAGE_STAGING;
        desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> staging;
        Check(SUCCEEDED(device->CreateBuffer(&desc,nullptr,&staging)),"create readback buffer");
        context->CopyResource(staging.Get(),buffer);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"read software D3D11 writeback");
        std::array<unsigned char,Size> bytes{};std::memcpy(bytes.data(),mapped.pData,bytes.size());
        context->Unmap(staging.Get(),0);return bytes;
    }
    void Write(ID3D11Buffer* buffer,unsigned char value,bool lose=false,bool disable=false,bool stop=false) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const auto result=context->Map(buffer,0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
        Check(SUCCEEDED(result),"original native WRITE_DISCARD succeeds");
        auto* driver=mapped.pData;
        GenshinDepthPortable::Mapped(context.Get(),buffer,0,D3D11_MAP_WRITE_DISCARD,0,&mapped,result);
        Check(mapped.pData!=driver && reinterpret_cast<uintptr_t>(mapped.pData)%16==0,"real adapter returns aligned CPU shadow");
        std::memset(mapped.pData,value,64);
        if(lose)GenshinDepthPortable::Lost();
        if(disable)GenshinDepthPortable::enabled=false;
        if(stop)GenshinLegacy::testInputRequested=false;
        auto unmap=GenshinDepthPortable::BeforeUnmap(context.Get(),buffer,0);
        Check(unmap.map.shadow!=nullptr,"exact original map retained through owner/loss/disable");
        Check(!GenshinDepthPortable::Read(buffer),"never publish before original Unmap returns");
        context->Unmap(buffer,0);unmap.Returned();
        if(lose)Check(!GenshinDepthPortable::Read(buffer),"loss rejects optional numerical publication");
        GenshinDepthPortable::enabled=true;
        const auto actual=ReadDriver(buffer);
        Check(actual.front()==value && actual.back()==value,"mandatory writeback survives original owner eligibility loss");
        GenshinLegacy::testInputRequested=true;
    }
};
}
int main() {
    GenshinDepthPortable::enabled=true;Device d;auto buffer=d.Buffer();
    d.Write(buffer.Get(),17);
    auto first=GenshinDepthPortable::Read(buffer.Get());
    std::array<unsigned char,64> bytes{};
    Check(first && first->CopyTo(bytes.data(),0,64) && bytes[63]==17,"returned immutable snapshot contains full exact upload");
    {
        auto before=GenshinDepthPortable::Before(d.context.Get(),buffer.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const auto result=d.context->Map(buffer.Get(),0,D3D11_MAP_WRITE_NO_OVERWRITE,0,&mapped);
        Check(SUCCEEDED(result),"original native NO_OVERWRITE succeeds");
        GenshinDepthPortable::Mapped(d.context.Get(),buffer.Get(),0,D3D11_MAP_WRITE_NO_OVERWRITE,0,&mapped,result,before);
        std::memset(static_cast<unsigned char*>(mapped.pData)+16,44,4);
        auto unmap=GenshinDepthPortable::BeforeUnmap(d.context.Get(),buffer.Get(),0);
        d.context->Unmap(buffer.Get(),0);unmap.Returned();
        const auto actual=d.ReadDriver(buffer.Get());
        Check(actual[0]==17 && actual[15]==17 && actual[16]==44 && actual[19]==44 && actual[20]==17,"preserving writeback touches only changed range");
        auto current=GenshinDepthPortable::Read(buffer.Get());
        Check(current && current->CopyTo(bytes.data(),0,64) && bytes==actual,"preserving snapshot matches real software D3D contents");
        Check(first->CopyTo(bytes.data(),0,64) && bytes[16]==17,"older immutable snapshot remains unchanged");
    }
    d.Write(buffer.Get(),65,true);
    d.Write(buffer.Get(),91,false,true);
    d.Write(buffer.Get(),103,false,false,true);
    {
        GenshinLegacy::testInputRequested=false;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const auto result=d.context->Map(buffer.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
        Check(SUCCEEDED(result),"original Map while input collection stopped");
        auto* driver=mapped.pData;
        GenshinDepthPortable::Mapped(d.context.Get(),buffer.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped,result);
        Check(mapped.pData==driver && !GenshinDepthPortable::ActiveMaps().HasEntries(),"stopped collection returns original driver pointer without new shadow");
        std::memset(mapped.pData,119,64);
        auto unmap=GenshinDepthPortable::BeforeUnmap(d.context.Get(),buffer.Get(),0);
        d.context->Unmap(buffer.Get(),0);unmap.Returned();
        Check(!GenshinDepthPortable::Read(buffer.Get()),"stopped write still revokes old immutable image");
        GenshinLegacy::testInputRequested=true;
        auto before=GenshinDepthPortable::Before(d.context.Get(),buffer.Get());
        Check(!before,"restart cannot recover a preimage lost during stop");
        Check(SUCCEEDED(d.context->Map(buffer.Get(),0,D3D11_MAP_WRITE_NO_OVERWRITE,0,&mapped)),"native preserving Map after restart");
        driver=mapped.pData;
        GenshinDepthPortable::Mapped(d.context.Get(),buffer.Get(),0,D3D11_MAP_WRITE_NO_OVERWRITE,0,&mapped,S_OK,before);
        Check(mapped.pData==driver,"unknown preserving bytes cannot authorize a shadow");
        std::memset(static_cast<unsigned char*>(mapped.pData)+16,121,4);
        auto preserving=GenshinDepthPortable::BeforeUnmap(d.context.Get(),buffer.Get(),0);
        d.context->Unmap(buffer.Get(),0);preserving.Returned();
        Check(!GenshinDepthPortable::Read(buffer.Get()),"unknown preserving write cannot publish a fabricated whole image");
    }
    d.Write(buffer.Get(),127);
    {
        GenshinLegacy::testInputRequested=false;
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=64;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
        bytes.fill(33);D3D11_SUBRESOURCE_DATA initial{bytes.data(),0,0};ComPtr<ID3D11Buffer> immutable;
        Check(SUCCEEDED(d.device->CreateBuffer(&desc,&initial,&immutable)),"create immutable geometry while stopped");
        GenshinDepthPortable::Created(immutable.Get(),&initial);
        auto image=GenshinDepthPortable::Read(immutable.Get());
        Check(image && image->Covers(0,64),"immutable creation remains observed for future consumers");
        GenshinLegacy::testInputRequested=true;
    }
    {
        auto large=d.Buffer(4096);std::array<unsigned char,4096> expected{};expected.fill(17);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(SUCCEEDED(d.context->Map(large.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped)),"large native discard Map");
        GenshinDepthPortable::Mapped(d.context.Get(),large.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped,S_OK);
        std::memcpy(mapped.pData,expected.data(),expected.size());
        auto discard=GenshinDepthPortable::BeforeUnmap(d.context.Get(),large.Get(),0);
        d.context->Unmap(large.Get(),0);discard.Returned();
        auto original=GenshinDepthPortable::Read(large.Get());Check(original!=nullptr,"large preimage captured");
        auto before=GenshinDepthPortable::Before(d.context.Get(),large.Get());
        Check(SUCCEEDED(d.context->Map(large.Get(),0,D3D11_MAP_WRITE_NO_OVERWRITE,0,&mapped)),"large preserving Map");
        GenshinDepthPortable::Mapped(d.context.Get(),large.Get(),0,D3D11_MAP_WRITE_NO_OVERWRITE,0,&mapped,S_OK,before);
        auto* cpu=static_cast<unsigned char*>(mapped.pData);
        for(unsigned i=254;i<260;++i)cpu[i]=expected[i]=42;
        for(unsigned i=1023;i<1028;++i)cpu[i]=expected[i]=43;
        cpu[4095]=expected[4095]=44;
        auto unmap=GenshinDepthPortable::BeforeUnmap(d.context.Get(),large.Get(),0);
        d.context->Unmap(large.Get(),0);unmap.Returned();
        Check(d.ReadDriver<4096>(large.Get())==expected,"block-boundary and tail changes match real driver contents");
        auto current=GenshinDepthPortable::Read(large.Get());std::array<unsigned char,4096> snapshot{};
        Check(current && current->CopyTo(snapshot.data(),0,4096) && snapshot==expected,"large published version matches exact upload");
        Check(original->CopyTo(snapshot.data(),0,4096) && snapshot[255]==17 && snapshot[4095]==17,"large older snapshot stays immutable");
        before=GenshinDepthPortable::Before(d.context.Get(),large.Get());
        Check(SUCCEEDED(d.context->Map(large.Get(),0,D3D11_MAP_WRITE_NO_OVERWRITE,0,&mapped)),"fragmented preserving Map");
        GenshinDepthPortable::Mapped(d.context.Get(),large.Get(),0,D3D11_MAP_WRITE_NO_OVERWRITE,0,&mapped,S_OK,before);
        cpu=static_cast<unsigned char*>(mapped.pData);
        for(unsigned i=0;i<1200;i+=2)cpu[i]=expected[i]=99;
        auto fragmented=GenshinDepthPortable::BeforeUnmap(d.context.Get(),large.Get(),0);
        const auto completedShadow=fragmented.map.shadow;
        Check(fragmented.upload.image && fragmented.upload.image->parts.size()==1 &&
              fragmented.upload.image->parts[0].storage==completedShadow,
              "fragmented preserving publication shares the complete initialized map snapshot");
        d.context->Unmap(large.Get(),0);fragmented.Returned();
        Check(d.ReadDriver<4096>(large.Get())==expected,"fragment capacity exhaustion still writes all real changes");
        current=GenshinDepthPortable::Read(large.Get());
        Check(current && current->CopyTo(snapshot.data(),0,4096) && snapshot==expected,
              "all fragmented writes publish exact known CPU bytes without fragment metadata expansion");
        Check(original->CopyTo(snapshot.data(),0,4096) && snapshot[255]==17 && snapshot[4095]==17,
              "dense publication never mutates the retained previous snapshot");
        before=GenshinDepthPortable::Before(d.context.Get(),large.Get());
        Check(SUCCEEDED(d.context->Map(large.Get(),0,D3D11_MAP_WRITE_NO_OVERWRITE,0,&mapped)),"unchanged preserving Map");
        GenshinDepthPortable::Mapped(d.context.Get(),large.Get(),0,D3D11_MAP_WRITE_NO_OVERWRITE,0,&mapped,S_OK,before);
        auto unchanged=GenshinDepthPortable::BeforeUnmap(d.context.Get(),large.Get(),0);
        d.context->Unmap(large.Get(),0);unchanged.Returned();
        current=GenshinDepthPortable::Read(large.Get());
        Check(current && current->parts.size()==1 && current->CopyTo(snapshot.data(),0,4096) && snapshot==expected,
              "unchanged Map publishes complete identical snapshot with one bounded part");
    }
    {
        constexpr UINT width=256u*1024+37;
        auto largePartial=d.Buffer(width);std::vector<unsigned char> expected(width,51),snapshot(width);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(SUCCEEDED(d.context->Map(largePartial.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped)),"large partial preimage Map");
        GenshinDepthPortable::Mapped(d.context.Get(),largePartial.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped,S_OK);
        std::memcpy(mapped.pData,expected.data(),width);
        auto initial=GenshinDepthPortable::BeforeUnmap(d.context.Get(),largePartial.Get(),0);
        d.context->Unmap(largePartial.Get(),0);initial.Returned();
        const auto original=GenshinDepthPortable::Read(largePartial.Get());
        Check(original && original->CopyTo(snapshot.data(),0,width) && snapshot==expected,"complete real preimage for largePartial preserving Map");
        // Equivalent CPU-ledger fixture for the just-proven real preimage:
        // its final 37 bytes live at offset 11 in a separate short allocation,
        // while their buffer coordinates are over 256K. No GPU authority added.
        auto prefix=GenshinDepthPortable::Allocate(width-37),suffix=GenshinDepthPortable::Allocate(64);
        Check(prefix && suffix,"allocate bounded partial-Part CPU preimage");
        std::memset(prefix->data.data(),51,prefix->size);std::memset(suffix->data.data(),51,suffix->size);
        auto partial=GenshinDepthPortable::Splice(original->desc,{},0,width,
            {{prefix,0,width-37,0},{suffix,width-37,width,11}});
        Check(partial && partial->CopyTo(snapshot.data(),0,width) && snapshot==expected,"partial-Part fixture equals actual complete driver preimage");
        std::array<unsigned char,6> local{0xDA,0,0,0,0,0xDA};
        Check(partial->CopyTo(local.data()+1,width-4,width) &&
            local==std::array<unsigned char,6>{0xDA,51,51,51,51,0xDA},
            "high global Part range copies to bounded local destination with canaries");
        partial->epoch=original->epoch;partial->revision=original->revision;
        const auto verifyDriver=[&] {
            D3D11_BUFFER_DESC desc{};desc.ByteWidth=width;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Buffer> staging;
            Check(SUCCEEDED(d.device->CreateBuffer(&desc,nullptr,&staging)),"large largePartial writeback staging");
            d.context->CopyResource(staging.Get(),largePartial.Get());
            D3D11_MAPPED_SUBRESOURCE read{};
            Check(SUCCEEDED(d.context->Map(staging.Get(),0,D3D11_MAP_READ,0,&read)),"large largePartial writeback readback");
            Check(std::memcmp(read.pData,expected.data(),width)==0,"real WARP data equals largePartial sparse/dense upload");
            d.context->Unmap(staging.Get(),0);
        };
        for(unsigned step=0;step<3;++step) {
            const std::shared_ptr<const GenshinDepthPortable::Image> before=step==0?partial:
                GenshinDepthPortable::Before(d.context.Get(),largePartial.Get());
            Check(SUCCEEDED(d.context->Map(largePartial.Get(),0,D3D11_MAP_WRITE_NO_OVERWRITE,0,&mapped)),"large preserving original Map");
            auto* driver=mapped.pData;
            GenshinDepthPortable::Mapped(d.context.Get(),largePartial.Get(),0,D3D11_MAP_WRITE_NO_OVERWRITE,0,&mapped,S_OK,before);
            Check(mapped.pData!=driver,"largePartial preserving map lends distinct ordinary CPU snapshot");
            auto* largeBytes=static_cast<unsigned char*>(mapped.pData);
            if(step==0) {
                largeBytes[0]=expected[0]=92;largeBytes[4095]=expected[4095]=93;largeBytes[4096]=expected[4096]=94;
                largeBytes[width-1]=expected[width-1]=95;
            } else if(step==1) {
                for(UINT i=0;i<width;i+=2)largeBytes[i]=expected[i]=81;
            }
            auto unmap=GenshinDepthPortable::BeforeUnmap(d.context.Get(),largePartial.Get(),0);
            Check(unmap.map.shadow && !GenshinDepthPortable::Read(largePartial.Get()),"largePartial writeback still cannot publish before original Unmap");
            d.context->Unmap(largePartial.Get(),0);unmap.Returned();verifyDriver();
            const auto current=GenshinDepthPortable::Read(largePartial.Get());
            Check(current && current->CopyTo(snapshot.data(),0,width) && snapshot==expected,"largePartial full immutable snapshot matches actual driver contents");
            Check(original->CopyTo(snapshot.data(),0,width) && snapshot.front()==51 && snapshot.back()==51,
                "largePartial writes never mutate retained older image");
        }
    }
    {
        // Exercise real fragmented Image::CopyTo across a 1KiB read boundary,
        // including an index split between Parts and strip restart semantics.
        constexpr UINT count=601,begin=5,width=2,end=begin+count*width;
        auto source=GenshinDepthPortable::Allocate(end);
        Check(bool(source),"allocate immutable CPU index fixture");
        for(UINT i=0;i<count;++i){const uint16_t v=i%7==0?0xffffu:uint16_t(i+10);
            std::memcpy(source->data.data()+begin+i*width,&v,width);}
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=end;
        auto image=GenshinDepthPortable::Splice(desc,{},0,end,
            {{source,0,1000,0},{source,1000,1028,1000},{source,1028,end,1028}});
        FramePipeline::CpuIndexBounds bounds{};unsigned reads=0;
        Check(image&&image->Covers(begin,end)&&FramePipeline::ReadCpuIndexBounds(begin,count,width,-10,true,
            [&](void* target,UINT low,UINT high){++reads;return image->CopyTo(target,low,high);},bounds)&&
            bounds.first==1&&bounds.last==600&&reads==2,"fragmented immutable indices decode exact signed range in two reads");
        auto gap=GenshinDepthPortable::Splice(desc,{},0,end,
            {{source,0,1028,0},{source,1029,end,1029}});
        Check(gap&&!gap->Covers(begin,end)&&!FramePipeline::ReadCpuIndexBounds(begin,count,width,0,true,
            [&](void* target,UINT low,UINT high){return gap->CopyTo(target,low,high);},bounds),
            "missing byte never authorizes a geometry range");
    }
    Check(!GenshinDepthPortable::ActiveMaps().HasEntries(),"all native map transactions extracted");
    std::printf("CpuMappingDx11 WARP: %u checks passed; no SDK or game validation\n",checks);
}
