// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GuideDiagnostics.h"
#include <d3d12.h>
#include <memory>
#include <array>
#include <climits>
#include <vector>

namespace GuideCapture {
enum class Stream : unsigned { Source, NrModel, Fg, History, Count };
// Sampled RGB changes above this absolute threshold count as a contribution.
// This is a measured changed-pixel ratio, never the shader's blend weight.
inline constexpr float ContributionThreshold=1e-5f;
struct Pixel {float r=0,g=0,b=0,a=0;};
struct Vec2 {float x=0,y=0;};
struct Image {
    unsigned width=0,height=0;
    GuideDiagnostics::Snapshot stamp;
    Stream stream=Stream::Source;
    uint64_t completedTick=0;
    std::vector<Pixel> colour;
    std::vector<float> depth;
    std::vector<Vec2> motion;
    std::vector<float> contribution;
    // Raw samples; multiply MV by these scales to obtain current->previous UV.
    float motionToUvX=0,motionToUvY=0;
    bool valid=false;
    bool colourPresent=false,depthPresent=false,motionPresent=false,contributionPresent=false;
};
struct Inputs {
    ID3D12Resource* colour=nullptr;
    ID3D12Resource* depth=nullptr;
    ID3D12Resource* motion=nullptr;
    ID3D12Resource* contribution=nullptr;
    // When true, contribution is the fresh model answer. Measure RGB difference
    // from colour (the actual fused answer), never confidence or blend weight.
    bool compareFreshAnswer=false;
    D3D12_RESOURCE_STATES colourState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES depthState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES motionState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES contributionState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    GuideDiagnostics::Rect colourRect,depthRect,motionRect;
    // In compare mode an empty region inherits colourRect; otherwise it means
    // the complete contribution texture. Formats may differ from colour.
    GuideDiagnostics::Rect contributionRect;
    float motionToUvX=0,motionToUvY=0;
    // This sampler changes compute state: only independently owned recordings.
    bool independentRecording=false;
    // Display interpretation only; does not alter actual consumer inputs.
    bool colourHdr=false;
    float colourWhite=1;
    bool historyDiagnostic=false; // already-colourized history inspector; never an algorithm input
    unsigned historyView=0; // exact subview used by the producer shader
};
// Persisted 0..4 retain their original NR meanings. Additional modes append.
enum class PreviewMode : unsigned { Off, NrOriginal, NrColour, NrDepth, NrMotion, FgColour, FgDepth, FgMotion, History };
struct PreviewDemand {
    PreviewMode mode=PreviewMode::Off;
    bool raw=false,flipY=false,frozen=false;
    float gain=16;
    unsigned historyView=0; // 0 actual contribution, 1 rejection, 2 age, 3 birth reprojection, 4 fresh, 5 fused, 6 stability weight
};
struct SampleToken {
    unsigned slot=UINT_MAX;
    uint64_t serial=0,revision=0;
    Stream stream=Stream::Source;
    unsigned historyView=0; // captured atomically with the reservation revision
};
// lease pins immutable storage on the CPU. GPU recordings additionally MUST
// RetainRead and pass through Executing/Submitted/Reset on their actual lists.
struct PreviewSnapshot {
    std::shared_ptr<void> lease;
    ID3D12Resource* texture=nullptr;
    unsigned width=0,height=0;
    uint64_t serial=0,revision=0,source=0,tick=0,completedTick=0;
    Stream consumer=Stream::Source;
    uint64_t epoch=0,feature=0,configuration=0,owner=0;
    bool colourHdr=false;
};
void SetPreviewDemand(const PreviewDemand&) noexcept;
PreviewDemand GetPreviewDemand() noexcept;
unsigned HistoryDebugView() noexcept;
bool PreviewActive() noexcept;
bool PreviewWanted(Stream) noexcept;
bool OfflineWanted(Stream) noexcept;
// Advisory demand/rate check before any resource queries or stamp construction.
// RecordOffline rechecks under its lifecycle lock; this grants no reservation.
bool OfflineSampleDue(Stream) noexcept;
// Call BEFORE resource queries or stamp construction. One reservation per
// selected source, at most 30 Hz, three fixed slots, no waiting or pool growth.
// Consume with TryRecord or CancelSample on EVERY exit.
bool TryBeginSample(Stream,SampleToken&) noexcept;
void CancelSample(SampleToken&) noexcept;
bool TryRecord(SampleToken&,ID3D12GraphicsCommandList*,const Inputs&,const GuideDiagnostics::Snapshot&) noexcept;
// Separate explicit offline path. Call only after a pair request; normal
// preview tokens never Map or create CPU images.
bool RecordOffline(Stream,ID3D12GraphicsCommandList*,const Inputs&,const GuideDiagnostics::Snapshot&) noexcept;
bool TryAcquireCompleted(PreviewSnapshot&) noexcept;
// Metadata of the snapshot actually recorded by the output renderer.
bool ReadDisplayed(PreviewSnapshot&) noexcept;
void NoteDisplayed(const PreviewSnapshot&) noexcept;
// Clear only this snapshot's displayed identity. Metadata is never GPU
// completion or successful Reset evidence and does not retire any recording.
void ClearDisplayed(const PreviewSnapshot&) noexcept;
bool RetainRead(const PreviewSnapshot&,ID3D12GraphicsCommandList*) noexcept;
// Separate display pass after SDK inputs are sealed. Caller binds its RTV.
// Accepts an independently owned DIRECT recording and supported SDR RTV only.
bool DrawPreview(const PreviewSnapshot&,ID3D12GraphicsCommandList*,DXGI_FORMAT,unsigned,unsigned) noexcept;
void PreviewUnavailable(const char*) noexcept;
std::string PreviewStatus() noexcept;
// Explicit OFFLINE proxy capture (<=160x100), independent of GPU display.
// Exactly two adjacent source IDs in the same owner/feature/configuration domain.
// No new captures after two reservations or 15 s; completion does not refresh age.
bool RequestPair(Stream) noexcept;
bool ReadPair(Stream,std::array<Image,2>&,std::string&) noexcept;
std::vector<Image> DrainPairs(Stream) noexcept;
// Demand expires unless the visible panel/session renews it. Normal operation
// records no extra GPU work. Calibration requests adjacent source pairs.
void Request(bool enabled,bool calibration=false) noexcept;
// Product previews request one stream. Calibration additionally requests Source.
void Request(Stream,bool enabled,bool calibration=false) noexcept;
bool Wanted() noexcept;
bool Wanted(Stream) noexcept;
// Records reduced full-frame data on the SAME owner list before its NR/FG use.
// Does not submit, flush, wait, or infer completion. Returns false when busy.
bool Record(Stream,ID3D12GraphicsCommandList*,const Inputs&,const GuideDiagnostics::Snapshot&) noexcept;
// EXACT pre-Execute boundary, for the actual queue and canonical list identity.
// Required for every execution of a sampled recording. Repeated prehooks for
// the same not-yet-submitted recording are idempotent. Caller serializes that
// exact recording; no new recording can start before its successful Reset.
struct ExecutionReceipt {
    std::array<uint64_t,27> preview{}; // three producers, eight readers each
    std::array<uint64_t,12> offline{}; // four lanes, three fixed slots each
};
ExecutionReceipt Executing(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*) noexcept;
// Call only after the real Execute returned, using its real queue and lists.
// Carry Executing's return across THIS exact real Execute. Late post callbacks
// acknowledge only captured generations, even after Reset/new Execute. Legacy
// receipt-less callers fail closed, retaining storage without guessing a match.
void Submitted(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*,const ExecutionReceipt& = {}) noexcept;
// Call only AFTER a successful Reset of this exact command-list recording.
// With the required Executing contract, no prehook proves an unsubmitted
// recording was discarded. Its GPU storage state is rolled back by the worker.
void Reset(ID3D12GraphicsCommandList*) noexcept;
// Nonblocking polling: preparation, fence polling, Map and pixel processing run
// on one bounded worker. Read copies an immutable completed image for the UI;
// its mutex is independent of lifecycle bookkeeping. No callback waits on GPU.
bool Read(Stream,Image&,std::string& status) noexcept;
// Visible detail-history panel only. Uses the existing asynchronous sampler at
// <=2 Hz, expires without renewal, and does not change preview/pair demand.
void RequestHistoryMeasurement(bool enabled=true) noexcept;
struct ContributionSample {
    uint64_t source=0,owner=0,feature=0,configuration=0,tick=0,completedTick=0;
    float changed=-1,coverage=-1;
    bool known=false;
    bool Matches(uint64_t currentOwner,uint64_t currentConfiguration,uint64_t currentSource,
                 uint64_t now) const noexcept {
        return known&&owner==currentOwner&&feature==currentOwner&&configuration==currentConfiguration&&
            source&&source<=currentSource&&tick&&now>=tick&&now-tick<=2000;
    }
};
// Copies metadata only, never the sampled image vectors. Observation cannot
// authorize history admission, GPU completion, or resource retirement.
bool ReadHistoryMeasurement(ContributionSample&) noexcept;
std::vector<Image> DrainSourcePairs() noexcept;
} // namespace GuideCapture
