// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11.h>
#include <array>
#include <cstdint>
namespace GenshinLegacy::ResourceProof
{
enum class Kind : std::uint8_t { None, CopyResource, CopyRegion, Update, ClearRtv, ClearDepth, DrawPotential, DispatchPotential,
    ClearUavUint, ClearUavFloat, Resolve, GenerateMips, WriteUnmap, ClearView, CopyRegion1, Update1 };
enum class Reason : std::uint8_t { Unknown, Observed, Disabled, WrongOwner, Deferred, UnknownAlias, Invalidated, MappedWrite, Discarded };
struct Witness
{
    std::uint64_t resourceLifetime=0,viewLifetime=0,deviceEpoch=0,observationEpoch=0;
    std::uint64_t lastObservedSerial=0,lastObservedEpoch=0,writerContext=0,writerThread=0;
    std::uint32_t subresource=0;
    std::uint32_t mapType=0;
    Kind kind=Kind::None;Reason reason=Reason::Unknown;
    // A forwarded mutation API call is observed, never its GPU effect/completion.
    bool observedMutationCall=false,subresourceKnown=false,wholeResource=false;
    // Partial hooks are never a complete last-write proof.
    bool lastWriteProven=false,completeMutationCoverage=false;
};
void Configure(ID3D11Device*,std::uint64_t deviceEpoch,bool enabled) noexcept;
void HooksInstalled(bool) noexcept;
bool Active() noexcept;
void Invalidate() noexcept;
void InvalidateContext(ID3D11DeviceContext*) noexcept;
void NoteWrite(ID3D11DeviceContext*,ID3D11Resource*,Kind,UINT subresource,bool whole) noexcept;
void NoteViewWrite(ID3D11DeviceContext*,ID3D11View*,Kind,bool whole) noexcept;
void AfterMap(ID3D11DeviceContext*,ID3D11Resource*,UINT,D3D11_MAP,HRESULT) noexcept;
void AfterUnmap(ID3D11DeviceContext*,ID3D11Resource*,UINT) noexcept;
void AfterDiscard(ID3D11DeviceContext*,ID3D11Resource*) noexcept;
void AfterDiscardView(ID3D11DeviceContext*,ID3D11View*) noexcept;
bool CurrentIdentityPair(const Witness&,const Witness&) noexcept;
void AfterGraphics(ID3D11DeviceContext*) noexcept;
void AfterCompute(ID3D11DeviceContext*) noexcept;
std::array<Witness,3> Snapshot(ID3D11DeviceContext*,const std::array<ID3D11Texture2D*,3>&,
                             const std::array<ID3D11ShaderResourceView*,3>&) noexcept;
Witness Identify(ID3D11DeviceContext*,ID3D11Resource*,ID3D11View* view=nullptr) noexcept;
}
