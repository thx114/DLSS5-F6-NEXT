// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "TaaConstantBuffer.h"
#include <array>

namespace taa::fullscreen {
// Recovery for static geometry whose CPU mirror was revoked at an unknown
// context boundary. Copies current bytes once, then admits them only after this
// immediate context's EVENT completes and the source write stamp still matches.
// The owning session keeps this object until Idle(), including on cancellation.
class Evidence final {
    struct Ticket {
        constants::ComPtr<ID3D11Buffer> source,staging;
        constants::ComPtr<ID3D11Query> done;
        constants::ReadbackStamp stamp;
        unsigned bytes=0;
        bool pending=false;
    };
    constants::ComPtr<ID3D11DeviceContext> context_;
    constants::ComPtr<ID3D11Device> device_;
    std::array<Ticket,2> tickets_;
    bool Complete(const Ticket& ticket) const {
        if(!ticket.pending)return true;
        BOOL completed=FALSE;
        return context_->GetData(ticket.done.Get(),&completed,sizeof(completed),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK&&completed;
    }
public:
    explicit Evidence(ID3D11DeviceContext* context):context_(context){context->GetDevice(&device_);}
    bool Idle() const {for(const auto& ticket:tickets_)if(!Complete(ticket))return false;return true;}
    bool Recover(ID3D11Buffer* buffer,bool indexed) {
        if(!buffer||context_->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
        D3D11_BUFFER_DESC desc{};buffer->GetDesc(&desc);
        constexpr UINT geometry=D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_INDEX_BUFFER;
        if((desc.Usage!=D3D11_USAGE_DEFAULT&&desc.Usage!=D3D11_USAGE_IMMUTABLE)||!desc.ByteWidth||desc.ByteWidth>4096||!(desc.BindFlags&geometry)||(desc.BindFlags&~geometry)||desc.MiscFlags)return false;
        constants::ComPtr<ID3D11Device> owner;buffer->GetDevice(&owner);if(owner.Get()!=device_.Get())return false;
        auto& ticket=tickets_[indexed?1:0];
        if(ticket.pending) {
            if(!Complete(ticket))return false;
            ticket.pending=false;
            if(ticket.source.Get()==buffer) {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                const auto result=context_->Map(ticket.staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
                if(FAILED(result)){ticket.source.Reset();return false;}
                const bool published=constants::PublishReadback(buffer,ticket.stamp,mapped.pData,ticket.bytes);
                context_->Unmap(ticket.staging.Get(),0);ticket.source.Reset();return published;
            }
            ticket.source.Reset();
        }
        constants::ReadbackStamp stamp;if(!constants::StampReadback(buffer,stamp))return false;
        if(!ticket.staging||ticket.bytes!=desc.ByteWidth) {
            ticket.staging.Reset();D3D11_BUFFER_DESC read{};read.ByteWidth=desc.ByteWidth;
            read.Usage=D3D11_USAGE_STAGING;read.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            if(FAILED(device_->CreateBuffer(&read,nullptr,&ticket.staging)))return false;
            ticket.bytes=desc.ByteWidth;
        }
        if(!ticket.done){D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};if(FAILED(device_->CreateQuery(&query,&ticket.done)))return false;}
        ticket.source=buffer;ticket.stamp=stamp;
        context_->CopyResource(ticket.staging.Get(),buffer);context_->End(ticket.done.Get());ticket.pending=true;
        return false;
    }
};
} // namespace taa::fullscreen
