// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinDx12Transport.h"
#include "GenshinTypedGuides.h"
#include "GenshinDepthMapping.h"
#include "GenshinReturnedRaster.h"
#include "GenshinFgGameMotion.h"
#include <native/FrameContract.h>
#include <atomic>
#include <mutex>
#include <cmath>

namespace native {class Dx11FrameSource;}

namespace GenshinTransport {
class ProducerFrame;

// Exact normalized coordinates of the PREPARED producer grid, not a claim
// about the later Scene/Picture shader's UVs, jitter phase or pixel validity.
// Origin zero and extents come from the actual typed render-token receipt.
class PreparedSceneGrid final {
    uint32_t width_=0,height_=0;
public:
    static PreparedSceneGrid Observe(const GenshinGuides::Facts& facts) noexcept {
        PreparedSceneGrid grid;const auto& depth=facts.textures[1];
        if(!facts.producerBuildKnown || !facts.engineSubrectKnown ||
           facts.renderWidth.source!=GenshinGuides::Source::ActualRenderToken ||
           facts.renderHeight.source!=GenshinGuides::Source::ActualRenderToken ||
           depth.width.source!=GenshinGuides::Source::TextureDescriptor ||
           depth.height.source!=GenshinGuides::Source::TextureDescriptor ||
           depth.format.value!=DXGI_FORMAT_R32_FLOAT || !depth.identity || !depth.width.value || !depth.height.value ||
           depth.width.value!=facts.renderWidth.value || depth.height.value!=facts.renderHeight.value ||
           depth.width.value>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || depth.height.value>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)return grid;
        grid.width_=depth.width.value;grid.height_=depth.height.value;return grid;
    }
    bool Matches(uint32_t width,uint32_t height) const noexcept {
        return width_ && height_ && width_==width && height_==height;
    }
    bool ToNormalized(uint32_t x,uint32_t y,double& u,double& v) const noexcept {
        u=v=0;
        if(!width_ || !height_ || x>=width_ || y>=height_)return false;
        u=(static_cast<double>(x)+0.5)/width_;v=(static_cast<double>(y)+0.5)/height_;return true;
    }
    bool ToTexel(double u,double v,uint32_t& x,uint32_t& y) const noexcept {
        x=y=0;
        if(!width_ || !height_ || !std::isfinite(u) || !std::isfinite(v) || u<0 || u>=1 || v<0 || v>=1)return false;
        x=static_cast<uint32_t>(u*width_);y=static_cast<uint32_t>(v*height_);return true;
    }
};

// One invocation-owned source and packet, shared by FG normalization and NR
// spatial adaptation. It owns neither consumer's completion token. Source
// identity refers to the original prepared plane; Packet::Prepared(1) is the
// immutable transport copy that both readers actually sample.
class PreparedDepthPacket final {
    friend class ProducerFrame;
    std::shared_ptr<const GenshinGuides::PreparedDepthSource> source_;
    std::shared_ptr<const QueuedTransfer> transfer_;
    PreparedDepthPacket()=default;
public:
    const auto& Source()const noexcept{return source_;}
    const auto& Transfer()const noexcept{return transfer_;}
    bool Matches(const GenshinGuides::Facts& facts)const noexcept {
        if(!source_ || !source_->Matches(facts) || !transfer_ ||
           !GenshinGuides::SameOwner(transfer_->Get().owner,source_->OwnerInterfaces()))return false;
        const auto& v=transfer_->Get();
        if(!v.sources[1] || !v.imported[1] || !transfer_->Prepared(1) || !v.surfaceLifetime ||
           !v.queue || !v.submitted || !v.submittedValue || v.submittedValue==UINT64_MAX)return false;
        Microsoft::WRL::ComPtr<IUnknown> identity;
        if(FAILED(v.sources[1]->QueryInterface(IID_PPV_ARGS(&identity))) ||
           reinterpret_cast<uint64_t>(identity.Get())!=source_->Identity())return false;
        D3D11_TEXTURE2D_DESC d{};transfer_->Prepared(1)->GetDesc(&d);
        const auto r=v.imported[1]->GetDesc();
        return d.Width==source_->Width() && d.Height==source_->Height() && d.Format==DXGI_FORMAT_R32_FLOAT &&
            d.MipLevels==1 && d.ArraySize==1 && d.SampleDesc.Count==1 && !d.SampleDesc.Quality &&
            r.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D && r.Width==d.Width && r.Height==d.Height &&
            r.Format==d.Format && r.DepthOrArraySize==1 && r.MipLevels==1 && r.SampleDesc.Count==1 && !r.SampleDesc.Quality;
    }
};

struct BridgeDepthReceipt
{
    GenshinGuides::DepthSource provenance{};
    std::shared_ptr<const PreparedDepthPacket> preparedPacket;
    Microsoft::WRL::ComPtr<ID3D11Resource> source;
    // Original prepared input retained independently from the mapped RG output.
    // This association is set only after actual Capture::Resolve succeeds.
    Microsoft::WRL::ComPtr<ID3D11Resource> producerSource;
    native::BridgeDepthLeasePtr lease;
    PreparedSceneGrid sceneGrid;
    std::shared_ptr<const GenshinDepthMapping::Node> mappingOwner;
    std::shared_ptr<const GenshinGameMotion::NativeGuideBundle> nativeGuide;
    std::shared_ptr<const QueuedTransfer> nativeTransfer;
    native::BridgeDepthLeasePtr nativeLease;
    uint32_t producerWidth=0,producerHeight=0;
    // NumericalMaximumWins can be copied as an observed producer fact; it
    // never implies forward/reversed Z, a sky sentinel, or metric distance.
    // For a native-only receipt this also carries the existing FG adapter
    // control expected by its unchanged depthInverted callsites. Only mapped
    // receipts use certified Z; source meaning is PreparedDepthSource::Convention.
    GenshinGuides::DepthConvention convention=GenshinGuides::DepthConvention::Unknown;
    ID3D12Resource* resource = nullptr;
    uintptr_t device = 0, context = 0;
    uint64_t deviceEpoch = 0, producerEpoch = 0, configEpoch = 0;
    uint64_t instance = 0, generation = 0, sourceFrame = 0, pictureSerial = 0, epoch = 0;
    uint32_t width = 0, height = 0, originX = 0, originY = 0;
    // coverage requires known valid-scene versus no-depth classification,
    // not depth for every HUD/sky pixel. ResolveDepth writes the paired mask.
    // producerReady is admission to an ordered read, not a CPU-completion bit:
    // Shared native sources use OrderRead; the strict fallback keeps CompletedTransfer.
    bool yFlip = false, transformKnown = false, coverage = false, producerReady = false;

    // Immutable mapping identity survives the temporary depth reader. It is
    // not permission to start another read, nor evidence of GPU completion.
    bool IdentityKnown() const noexcept
    {
        if(preparedPacket && (!mappingOwner || mappingOwner->preparedSource!=preparedPacket->Source() ||
           mappingOwner->preparedTransfer!=preparedPacket->Transfer()))return false;
        return source && lease && device && context && deviceEpoch && producerEpoch && configEpoch && instance && generation &&
            sourceFrame && pictureSerial && epoch && width && height && !originX && !originY && !yFlip && transformKnown && coverage &&
            mappingOwner && mappingOwner->serial==pictureSerial &&
            sceneGrid.Matches(producerWidth,producerHeight) &&
            (convention==GenshinGuides::DepthConvention::DeviceZForward ||
             convention==GenshinGuides::DepthConvention::DeviceZReversed) && producerReady;
    }
    bool NativeIdentityKnown() const noexcept
    {
        if(!nativeGuide || nativeGuide->provenance!=GenshinGuides::Source::ActualNativeAccumulate ||
           !preparedPacket || nativeGuide->preparedSource!=preparedPacket->Source() ||
           nativeTransfer!=preparedPacket->Transfer())return false;
        const auto& o=nativeGuide->owner;
        return source && producerSource && device && context && deviceEpoch && producerEpoch && configEpoch && instance && generation &&
            pictureSerial && epoch && width && height && !originX && !originY && !yFlip && transformKnown && coverage && (producerReady || nativeTransfer) &&
            sceneGrid.Matches(producerWidth,producerHeight) && nativeGuide->pictureIdentity &&
            o.device==device && o.context==context && o.deviceEpoch==deviceEpoch && o.producerEpoch==producerEpoch &&
            o.configEpoch==configEpoch && o.instance==instance && o.generation==generation && o.sourceFrame==sourceFrame &&
            o.historyEpoch==epoch && (convention==GenshinGuides::DepthConvention::DeviceZForward ||
             convention==GenshinGuides::DepthConvention::DeviceZReversed);
    }
    // NR retains a mapped lease: completed for NR-only, or GPU-ordered on the
    // owning context for combined FG/NR. Ready is not a CPU completion claim.
    bool Known() const noexcept {return IdentityKnown() && lease->Ready();}
};

// A direct native-depth reader for Present NR. This is not the dilated FG
// depth and not a replacement for the mapped RGBA/validity path. It retains
// the actual low-resolution R32 producer plane and orders the real NR queue
// after that producer's submission, then through the final consumer fence.
class NativeQueuedDepthLease final : public native::BridgeDepthLease {
    friend class native::Dx11FrameSource;
    std::shared_ptr<ProducerFrame> producer_;
    std::shared_ptr<const GenshinGameMotion::NativeGuideBundle> guide_;
    std::shared_ptr<const QueuedTransfer> transfer_;
    GenshinGuides::Owner owner_{};
    GenshinGuides::Owner guideOwner_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> resource_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> consumerQueue_;
    Microsoft::WRL::ComPtr<ID3D12Fence> readyFence_,doneFence_;
    uint64_t readyValue_=0,doneValue_=0;
    bool began_=false,ordered_=false,retired_=false;
    struct RetainedRing {
        std::array<std::shared_ptr<NativeQueuedDepthLease>,3> slots{};
    };
    static RetainedRing& Retained() {static auto* ring=new RetainedRing;return *ring;}
    static std::mutex& RetainedMutex() {static auto* mutex=new std::mutex;return *mutex;}
    static bool Reclaimable(const std::shared_ptr<NativeQueuedDepthLease>& slot) noexcept {
        return slot && slot->owner_.thread==GetCurrentThreadId() &&
            (slot->ReaderRetired() || (!slot->began_ && slot.use_count()==1));
    }
    static void CollectRetiredReaders() noexcept {
        std::lock_guard lock(RetainedMutex());
        for(auto& slot:Retained().slots)if(Reclaimable(slot)) {
            LOG_INFO("R25 native depth reader retired: pid={} instance={} generation={} fence={} value={} surface_release_after_consumer=true",
                GetCurrentProcessId(),slot->owner_.instance,slot->owner_.generation,
                reinterpret_cast<uint64_t>(slot->doneFence_.Get()),slot->doneValue_);
            slot.reset();
        }
    }
    static bool RetainReader(const std::shared_ptr<NativeQueuedDepthLease>& value) {
        CollectRetiredReaders();std::lock_guard lock(RetainedMutex());
        for(auto& slot:Retained().slots)if(!slot) {slot=value;
            LOG_INFO("R25 native depth reader retained: pid={} instance={} generation={} bounded_slots=3 no_early_surface_release=true",
                GetCurrentProcessId(),value->owner_.instance,value->owner_.generation);return true;}
        static std::atomic<unsigned> reports{0};
        if(reports.fetch_add(1,std::memory_order_relaxed)<8)
            LOG_INFO("R25 native depth reader admission refused: pid={} instance={} generation={} reason=reader-retention-full depth_skipped_sr_preserved=true",
                GetCurrentProcessId(),value->owner_.instance,value->owner_.generation);
        return false;
    }
    static bool Same(IUnknown* a,IUnknown* b) noexcept {
        Microsoft::WRL::ComPtr<IUnknown> x,y;
        return a && b && SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&x))) &&
            SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&y))) && x.Get()==y.Get();
    }
    // Only Dx11FrameSource may replace the separate SR queue Wait. Its
    // BridgeDepthReader binds this exact ready token, then Acquire signals and
    // flushes it after Copyback's context Wait. NativeProducer still queues its
    // existing input-ready Wait before Execute. This is transitive GPU order,
    // never CPU completion; unknown context/packet keeps the original Wait.
    bool OrderForAcquire(ID3D12CommandQueue* queue,ID3D11DeviceContext4* context,
        ID3D12Fence* ready,uint64_t value) noexcept {
        if(!Ready() || ordered_ || retired_ || !queue || !ready || !value || value==UINT64_MAX ||
           !transfer_->CopybackOrdersRead(context))return Order(queue);
        Microsoft::WRL::ComPtr<ID3D12Device> resourceDevice,queueDevice;
        if(FAILED(resource_->GetDevice(IID_PPV_ARGS(&resourceDevice))) ||
           FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) || !Same(resourceDevice.Get(),queueDevice.Get()))
            return Order(queue);
        // BeginReader still validates this ready fence's actual device before
        // admission. HandOff requires the identical signalled FrameInput token.
        readyFence_=ready;readyValue_=value;
        consumerQueue_=queue;ordered_=true;return true;
    }
public:
    bool Order(ID3D12CommandQueue* queue) noexcept {
        if(!Ready() || ordered_ || retired_ || !queue) {
            static std::atomic<unsigned> reports{0};
            if(reports.fetch_add(1,std::memory_order_relaxed)<16)
                LOG_INFO("R17 native order refusal: pid={} stage=precondition ready={} ordered={} retired={} queue={} no_authority=true",
                    GetCurrentProcessId(),Ready(),ordered_,retired_,reinterpret_cast<uintptr_t>(queue));
            return false;
        }
        Microsoft::WRL::ComPtr<ID3D12Device> resourceDevice,queueDevice;
        if(FAILED(resource_->GetDevice(IID_PPV_ARGS(&resourceDevice))) ||
           FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) || !Same(resourceDevice.Get(),queueDevice.Get())) {
            static std::atomic<unsigned> reports{0};
            if(reports.fetch_add(1,std::memory_order_relaxed)<16)
                LOG_INFO("R17 native order refusal: pid={} stage=device resource_device={} queue_device={} no_authority=true",
                    GetCurrentProcessId(),reinterpret_cast<uintptr_t>(resourceDevice.Get()),reinterpret_cast<uintptr_t>(queueDevice.Get()));
            return false;
        }
        const auto& view=transfer_->Get();
        const auto wait=queue->Wait(view.submitted.Get(),view.submittedValue);
        if(FAILED(wait)) {
            static std::atomic<unsigned> reports{0};
            if(reports.fetch_add(1,std::memory_order_relaxed)<16)
                LOG_INFO("R17 native order refusal: pid={} stage=wait hr={:X} no_authority=true",GetCurrentProcessId(),static_cast<unsigned>(wait));
            return false;
        }
        consumerQueue_=queue;ordered_=true;return true;
    }
    static native::BridgeDepthLeasePtr Create(std::shared_ptr<ProducerFrame> producer,
        std::shared_ptr<const GenshinGameMotion::NativeGuideBundle> guide,
        std::shared_ptr<const QueuedTransfer> transfer,const GenshinGuides::Owner& owner);
    bool Ready() const noexcept override;
    bool BeginReader(IUnknown* source,ID3D12Resource* resource,ID3D12Fence* fence,uint64_t value) noexcept override {
        if(!Ready() || began_ || !ordered_ || retired_ || !source || !resource || !fence || !value ||
           (readyFence_ && (readyFence_.Get()!=fence || readyValue_!=value)) ||
           !Same(source,transfer_->Prepared(1)) || resource!=resource_.Get()) {
            static std::atomic<unsigned> reports{0};
            if(reports.fetch_add(1,std::memory_order_relaxed)<16)
                LOG_INFO("R17 native reader refusal: pid={} ready={} began={} ordered={} retired={} source_match={} resource_match={} fence={} value={} no_authority=true",
                    GetCurrentProcessId(),Ready(),began_,ordered_,retired_,source&&Same(source,transfer_->Prepared(1)),resource==resource_.Get(),reinterpret_cast<uintptr_t>(fence),value);
            return false;
        }
        Microsoft::WRL::ComPtr<ID3D12Device> device,fenceDevice;
        if(FAILED(resource->GetDevice(IID_PPV_ARGS(&device))) ||
           FAILED(fence->GetDevice(IID_PPV_ARGS(&fenceDevice))) || !Same(device.Get(),fenceDevice.Get()))return false;
        readyFence_=fence;readyValue_=value;began_=true;return true;
    }
    bool MatchesReader(ID3D12Resource* resource,ID3D12Fence* fence,uint64_t value) const noexcept override {
        return Ready() && began_ && ordered_ && !doneFence_ && !retired_ && resource==resource_.Get() &&
            readyFence_.Get()==fence && readyValue_==value;
    }
    bool BindReaderCompletion(ID3D12Fence* fence,uint64_t value) noexcept override {
        if(!began_ || retired_ || doneFence_ || !fence || !value)return false;
        Microsoft::WRL::ComPtr<ID3D12Device> device,fenceDevice;
        if(FAILED(resource_->GetDevice(IID_PPV_ARGS(&device))) ||
           FAILED(fence->GetDevice(IID_PPV_ARGS(&fenceDevice))) || !Same(device.Get(),fenceDevice.Get()))return false;
        doneFence_=fence;doneValue_=value;return true;
    }
    bool RetireReader() noexcept override {
        if(!began_ || !doneFence_ || !doneValue_)return false;
        retired_=true;return true;
    }
    bool ReaderRetired() const noexcept override {
        const auto completed=doneFence_?doneFence_->GetCompletedValue():0;
        return retired_ && doneFence_ && doneValue_ && completed!=UINT64_MAX && completed>=doneValue_ &&
            owner_.thread==GetCurrentThreadId();
    }
};

inline native::BridgeDepthLeasePtr NativeDepthLease(const BridgeDepthReceipt& receipt) {
    return receipt.lease ? receipt.lease : receipt.nativeLease;
}
inline uint32_t NativeDepthWidth(const BridgeDepthReceipt& receipt) {
    return receipt.lease ? receipt.width : receipt.producerWidth;
}
inline uint32_t NativeDepthHeight(const BridgeDepthReceipt& receipt) {
    return receipt.lease ? receipt.height : receipt.producerHeight;
}

// Owned by the actual capture history. The original bindings and immutable prepared guides
// survive until both the outer capture query and this transport's GPU work have completed.
// This records transport provenance only; it never establishes guide semantics.
class ProducerFrame final : public std::enable_shared_from_this<ProducerFrame> {
    static std::atomic<size_t>& ObservationOwners(){static std::atomic<size_t> count{0};return count;}
    struct ObservationOwnership {
        ObservationOwnership(){ObservationOwners().fetch_add(1,std::memory_order_acq_rel);}
        ~ObservationOwnership(){ObservationOwners().fetch_sub(1,std::memory_order_acq_rel);}
    } observationOwnership_; // Last member destroyed: retirement is published first.
    struct Retirement {
        std::shared_ptr<const void> snapshot;
        std::shared_ptr<const GenshinGuides::Receipt> guides;
        std::array<Microsoft::WRL::ComPtr<ID3D11Resource>,4> resources;
        std::shared_ptr<Lease> lease;
        uint32_t thread=0;
        Retirement* next=nullptr;
    };
    struct Retirements {std::mutex mutex;Retirement* head=nullptr;std::atomic<size_t> count{0};};
    static Retirements& Retired(){static auto* retained=new Retirements;return *retained;}
    std::unique_ptr<Retirement> retirement_=std::make_unique<Retirement>();
    std::shared_ptr<const GenshinGuides::Receipt> guides_;
    std::shared_ptr<const void> sourceSnapshot_;
    std::shared_ptr<PreparedSrInputs> preparedSrInputs_;
    std::shared_ptr<PreparedSrOutput> preparedSrOutput_;
    std::array<Microsoft::WRL::ComPtr<ID3D11Resource>,4> resources_;
    std::array<ID3D11Resource*,4> raw_{};
    std::shared_ptr<Lease> lease_;
    std::shared_ptr<const PreparedDepthPacket> preparedPacket_;
    std::shared_ptr<const QueuedTransfer> preparedTransfer_;
    // An early FG lookup may precede the accepted-grid receipt. Cache the real
    // transfer independently so later source sealing never exports a second
    // wrapper or mismatches FG and NR by shared_ptr identity.
    std::shared_ptr<const QueuedTransfer> PreparedTransferLocked() {
        if(!Live() || !lease_ || owner_.thread!=GetCurrentThreadId())return {};
        if(!preparedTransfer_)preparedTransfer_=lease_->Queued(owner_);
        return preparedTransfer_;
    }
    // Called under the existing ProducerFrame mutex, never the draw hook.
    std::shared_ptr<const PreparedDepthPacket> PreparedPacketLocked() {
        if(!Live() || !guides_ || !lease_ || owner_.thread!=GetCurrentThreadId())return {};
        if(preparedPacket_)return preparedPacket_->Matches(guides_->Get())?preparedPacket_:nullptr;
        const auto source=guides_->PreparedSourceForPresent();
        if(!source)return {};
        auto packet=std::shared_ptr<PreparedDepthPacket>(new PreparedDepthPacket);
        packet->source_=source;packet->transfer_=PreparedTransferLocked();
        if(!packet->Matches(guides_->Get()))return {};
        preparedPacket_=packet;return packet;
    }
    GenshinGuides::Owner owner_{};
    std::atomic<bool> revoked_{false};
    mutable std::mutex mutex_;
    Observation observation_{};
    bool attempted_=false;
public:
    ProducerFrame(std::shared_ptr<const GenshinGuides::Receipt> guides,
        const std::array<ID3D11Resource*,4>& resources,std::shared_ptr<const void> sourceSnapshot,
        std::shared_ptr<PreparedSrInputs> preparedSrInputs={},std::shared_ptr<PreparedSrOutput> preparedSrOutput={})
        :guides_(std::move(guides)),sourceSnapshot_(std::move(sourceSnapshot)),
         preparedSrInputs_(std::move(preparedSrInputs)),preparedSrOutput_(std::move(preparedSrOutput)),raw_(resources) {
        if(!guides_ || !sourceSnapshot_){Revoke();return;}
        const auto& facts=guides_->Get();owner_=facts.owner;observation_.owner=owner_;
        retirement_->snapshot=sourceSnapshot_;retirement_->guides=guides_;retirement_->thread=facts.owner.thread;
        const auto& o=facts.owner;
        if(!o.device || !o.context || !o.deviceEpoch || !o.producerEpoch || !o.configEpoch ||
            !o.instance || !o.generation || !o.historyEpoch || o.thread!=GetCurrentThreadId())Revoke();
        for(size_t i=0;i<4;++i){
            resources_[i]=resources[i];observation_.source[i]=reinterpret_cast<uint64_t>(resources[i]);
            retirement_->resources[i]=resources[i];
            Microsoft::WRL::ComPtr<IUnknown> identity;
            if(!resources[i] || FAILED(resources[i]->QueryInterface(IID_PPV_ARGS(&identity))) ||
                facts.textures[i].identity!=reinterpret_cast<uint64_t>(identity.Get()))Revoke();
        }
    }
    ~ProducerFrame(){
        Revoke();
        if(!retirement_->lease)return;
        const bool owner=Owner().thread==GetCurrentThreadId();
        if(owner){retirement_->lease->Revoke();if(retirement_->lease->Poll())return;}
        // A caller dropping the producer is not a GPU completion witness. Retain original
        // source bindings too, independently of the transport's own prepared-resource quarantine.
        auto& retained=Retired();std::lock_guard lock(retained.mutex);
        retirement_->next=retained.head;retained.head=retirement_.release();++retained.count;
    }
    static bool ObservationOwnersAccounted(size_t held)noexcept {return ObservationOwners().load(std::memory_order_acquire)==held;}
    static bool ObservationRetirementsEmpty()noexcept {return Retired().count.load(std::memory_order_acquire)==0;}
    static size_t CollectRetired()noexcept {
        auto& retained=Retired();std::lock_guard lock(retained.mutex);auto** next=&retained.head;
        while(*next){auto* item=*next;
            if(item->thread==GetCurrentThreadId()){
                item->lease->Revoke();
                if(item->lease->Poll()){*next=item->next;delete item;--retained.count;continue;}
            }
            next=&item->next;
        }
        return retained.count;
    }
    ProducerFrame(const ProducerFrame&)=delete;
    ProducerFrame& operator=(const ProducerFrame&)=delete;
    const GenshinGuides::Owner& Owner()const noexcept{return owner_;}
    const std::shared_ptr<const GenshinGuides::Receipt>& Guides()const noexcept{return guides_;}
    bool Live()const noexcept{return !revoked_.load(std::memory_order_acquire);}
private:
    friend class GenshinFsr::Dx11Owner;
    GenshinFsr::PreparedResourceAdmission PreparedAdmission(ID3D11DeviceContext*,
        const GenshinFsr::Parameters&,const GenshinFsr::ResourceSet&) noexcept;
public:
    // Scope destruction revokes a failed producer before Finish publishes its
    // lease. Keep the actual lease-creation fact independent from Stage::Revoked.
    bool LeaseCreated() noexcept {std::lock_guard lock(mutex_);return lease_!=nullptr;}
    // Cross-thread revocation touches only atomics. The owner thread revokes the Lease in Poll/Finish.
    void Revoke() noexcept {revoked_.store(true,std::memory_order_release);if(guides_)guides_->Retire();}
    void CheckCurrent(const GenshinGuides::Owner& owner) noexcept {
        if(!GenshinGuides::SameOwner(Owner(),owner))Revoke();
    }
    std::unique_ptr<Scope> Open(const GenshinGuides::Owner& owner,
        const std::array<ID3D11Resource*,4>& resources,const void* parameters) {
        std::lock_guard lock(mutex_);
        if(!Live() || attempted_ || owner.thread!=GetCurrentThreadId() || !parameters ||
            !GenshinGuides::SameOwner(Owner(),owner) || resources!=raw_ || Scope::Current()){
            Revoke();return {};
        }
        attempted_=true;
        auto scope=std::make_unique<Scope>(owner,resources,parameters);
        scope->preparedInputs=preparedSrInputs_;scope->preparedOutput=preparedSrOutput_;
        return scope;
    }
    // Called after the actual bridge Evaluate, including failure/exception unwind.
    void Finish(Scope& scope) noexcept {
        std::lock_guard lock(mutex_);
        if(!GenshinGuides::SameOwner(Owner(),scope.owner) || scope.resources!=raw_ ||
            scope.owner.thread!=GetCurrentThreadId() || lease_){
            if(scope.lease)scope.lease->Revoke();Revoke();return;
        }
        lease_=scope.lease;retirement_->lease=lease_;
        if(lease_){if(!Live())lease_->Revoke();observation_=lease_->Snapshot();}
    }
    bool Poll() noexcept {
        std::lock_guard lock(mutex_);
        if(Owner().thread!=GetCurrentThreadId())return false;
        if(!lease_)return true;
        if(!Live())lease_->Revoke();
        const bool done=lease_->Poll();observation_=lease_->Snapshot();
        if(observation_.revoked)Revoke();
        return done;
    }
    // Serialized with Open/Finish/export. Poll completion alone is insufficient:
    // the Lease must prove no escaped packet or external surface lifetime.
    bool ObservationQuiescent(const GenshinGuides::Owner& owner) const noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock() || !Live() || owner.thread!=GetCurrentThreadId() || !sourceSnapshot_ ||
            !GenshinGuides::SameOwner(owner,owner_))return false;
        if(!lease_)return !attempted_;
        return lease_->ObservationQuiescent(owner);
    }
    bool ObservationNoEscapedReaders(const GenshinGuides::Owner& owner) const noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock() || !Live() || owner.thread!=GetCurrentThreadId() || !sourceSnapshot_ ||
            !GenshinGuides::SameOwner(owner,owner_))return false;
        return lease_?lease_->ObservationNoEscapedReaders(owner):!attempted_;
    }
    Observation Snapshot()const noexcept {
        std::lock_guard lock(mutex_);auto result=observation_;
        if(!Live()){result.revoked=true;result.stage=Stage::Revoked;}
        return result;
    }
    std::shared_ptr<PreparedSrOutput> PrepareSrOutput() noexcept {
        std::lock_guard lock(mutex_);
        if(!Live() || !lease_ || owner_.thread!=GetCurrentThreadId())return {};
        return lease_->PrepareSrOutput(owner_);
    }
    // A separate optional FG receipt: normal SR/NR admission is unchanged.
    std::shared_ptr<const QueuedTransfer> QueuedFgTransfer() noexcept {
        std::lock_guard lock(mutex_);
        if(!Live() || !guides_ || !lease_ || owner_.thread!=GetCurrentThreadId())return {};
        auto result=PreparedTransferLocked();
        return Live()?result:nullptr;
    }
    std::shared_ptr<GenshinGameMotion::Lease> PrepareGameMotion(ID3D11DeviceContext* context,
        const BridgeDepthReceipt& depth,const GenshinGuides::Owner& predecessor,
        std::shared_ptr<const QueuedTransfer> completion) noexcept {
        try {
            if(!Live() || !guides_ || !depth.NativeIdentityKnown() || !completion ||
               !GenshinGuides::SameOwner(completion->Get().owner,owner_) ||
               !depth.preparedPacket || completion!=depth.preparedPacket->Transfer() ||
               !depth.nativeGuide || depth.nativeGuide->preparedSource!=depth.preparedPacket->Source()) {
                static std::atomic<unsigned> refusals{0};
                if(refusals.fetch_add(1,std::memory_order_relaxed)<16)
                    LOG_INFO("R835 native motion admission refusal: pid={} live={} guides={} native_identity={} completion={} completion_owner={} no_authority=true",
                        GetCurrentProcessId(),Live(),bool(guides_),depth.NativeIdentityKnown(),bool(completion),
                        completion?GenshinGuides::SameOwner(completion->Get().owner,owner_):false);
                return {};
            }
            GenshinGameMotion::Snapshot snapshot;
            if(!GenshinGuides::OwnerIdentity::Context(context).ToCanonical(owner_,snapshot.owner))return {};
            snapshot.native=depth.nativeGuide;snapshot.guides=guides_;snapshot.transfer=std::move(completion);
            snapshot.pictureSerial=depth.pictureSerial;snapshot.reference=depth.nativeGuide->reference;
            return GenshinGameMotion::Prepare(context,std::move(snapshot),predecessor);
        }catch(...){return {};}
    }
    BridgeDepthReceipt PreparedDepth(uint64_t pictureSerial,GenshinDepthMapping::Capture*,ID3D11DeviceContext*,IUnknown*,bool nativeOnly=false) noexcept;
};

inline native::BridgeDepthLeasePtr NativeQueuedDepthLease::Create(std::shared_ptr<ProducerFrame> producer,
    std::shared_ptr<const GenshinGameMotion::NativeGuideBundle> guide,
    std::shared_ptr<const QueuedTransfer> transfer,const GenshinGuides::Owner& owner) {
    if(!producer || !guide || !transfer || !producer->Live() ||
       !GenshinGuides::SameOwner(guide->owner,owner) || guide->owner.sourceFrame!=owner.sourceFrame ||
       !transfer->Get().queue || !transfer->Get().submitted || !transfer->Get().submittedValue ||
       !transfer->Prepared(1) || !transfer->Get().imported[1])return {};
    const auto& producerOwner=producer->Owner();GenshinGuides::Owner canonicalProducer{};
    if(!GenshinGuides::OwnerIdentity::Context(reinterpret_cast<ID3D11DeviceContext*>(producerOwner.context))
            .ToCanonical(producerOwner,canonicalProducer) || !GenshinGuides::SameOwner(canonicalProducer,owner))return {};
    const auto& view=transfer->Get();
    if(!producer->Guides() || !GenshinGuides::SameOwner(view.owner,producer->Guides()->Get().owner))return {};
    D3D11_TEXTURE2D_DESC raw{};transfer->Prepared(1)->GetDesc(&raw);
    const auto desc=view.imported[1]->GetDesc();
    if(raw.Format!=DXGI_FORMAT_R32_FLOAT || desc.Format!=DXGI_FORMAT_R32_FLOAT ||
       desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize!=1 || desc.MipLevels!=1 ||
       desc.SampleDesc.Count!=1 || desc.SampleDesc.Quality ||
       raw.Width!=desc.Width || raw.Height!=desc.Height ||
       desc.Width!=guide->frame.renderWidth || desc.Height!=guide->frame.renderHeight)return {};
    Microsoft::WRL::ComPtr<ID3D12Device> device,queueDevice,fenceDevice;
    if(FAILED(view.imported[1]->GetDevice(IID_PPV_ARGS(&device))) ||
       FAILED(view.queue->GetDevice(IID_PPV_ARGS(&queueDevice))) ||
       FAILED(view.submitted->GetDevice(IID_PPV_ARGS(&fenceDevice))) ||
       !Same(device.Get(),queueDevice.Get()) || !Same(device.Get(),fenceDevice.Get()))return {};
    auto result=std::shared_ptr<NativeQueuedDepthLease>(new NativeQueuedDepthLease);
    result->producer_=std::move(producer);result->guide_=std::move(guide);result->transfer_=std::move(transfer);
    result->owner_=producerOwner;result->guideOwner_=owner;result->resource_=view.imported[1];result->queue_=view.queue;
    if(!RetainReader(result))return {};
    return result;
}
inline bool NativeQueuedDepthLease::Ready() const noexcept {
    const bool live=producer_ && producer_->Live();
    const bool producerOwner=live && GenshinGuides::SameOwner(producer_->Owner(),owner_);
    const bool guideOwner=guide_ && GenshinGuides::SameOwner(guide_->owner,guideOwner_);
    const bool ready=!retired_ && live && guide_ && transfer_ && resource_ && queue_ &&
        producerOwner && guideOwner && owner_.thread==GetCurrentThreadId();
    if(!ready) {
        static std::atomic<unsigned> reports{0};
        if(reports.fetch_add(1,std::memory_order_relaxed)<16)
            LOG_INFO("R17 native ready refusal: pid={} retired={} live={} guide={} transfer={} resource={} queue={} producer_owner={} guide_owner={} thread={}/{} no_authority=true",
                GetCurrentProcessId(),retired_,live,static_cast<bool>(guide_),static_cast<bool>(transfer_),static_cast<bool>(resource_),static_cast<bool>(queue_),
                producerOwner,guideOwner,GetCurrentThreadId(),owner_.thread);
    }
    return ready;
}

class PreparedDepthLease final : public native::BridgeDepthLease
{
    friend class ProducerFrame;
    // Only OrderMappingRead can issue this private proof, before Resolve reads
    // the native source. A QueuedTransfer pointer alone cannot authorize NR.
    struct OrderedDepthRead {
        std::shared_ptr<const QueuedTransfer> transfer;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context;
        // Set only after a real isolated output exists. The game context then
        // waits for that output, not for an unused read of original scratch.
        std::shared_ptr<GenshinFgQueryIsolation::PictureJob> isolatedOutput;
    };
    static std::shared_ptr<const OrderedDepthRead> OrderMappingRead(
        std::shared_ptr<const QueuedTransfer> transfer,const Owner& owner,
        ID3D11DeviceContext* context,ID3D11Resource* source) {
        if(!transfer || !source || !GenshinGuides::SameOwner(transfer->Get().owner,owner))return {};
        GenshinGuides::Owner canonicalOwner{};
        if(!GenshinGuides::OwnerIdentity::Context(context).ToCanonical(owner,canonicalOwner))return {};
        const auto& native=transfer->Get();
        Microsoft::WRL::ComPtr<IUnknown> sourceIdentity,nativeIdentity;
        if(!native.sources[1] || !native.imported[1] ||
           FAILED(source->QueryInterface(IID_PPV_ARGS(&sourceIdentity))) ||
           FAILED(native.sources[1]->QueryInterface(IID_PPV_ARGS(&nativeIdentity))) ||
           sourceIdentity.Get()!=nativeIdentity.Get())return {};
        auto ordered=std::make_shared<OrderedDepthRead>();
        if(FAILED(context->QueryInterface(IID_PPV_ARGS(&ordered->context))))return {};
        ordered->transfer=std::move(transfer);
        // OrderRead checks the exact owning context/thread and queues Wait on
        // the real submitted11 fence/value. It does not poll the CPU or flush.
        if(!ordered->transfer->OrderRead(ordered->context.Get()))return {};
        return ordered;
    }
    static std::shared_ptr<const OrderedDepthRead> OrderIsolatedMappingRead(
        std::shared_ptr<const QueuedTransfer> transfer,const Owner& owner,ID3D11DeviceContext* context,
        const std::shared_ptr<const GenshinDepthMapping::Node>& mapping) {
        if(!transfer || !mapping || !mapping->isolatedJob || !mapping->preparedSource ||
           !GenshinGuides::SameOwner(transfer->Get().owner,owner) ||
           mapping->preparedSource->OwnerInterfaces().thread!=GetCurrentThreadId() ||
           !GenshinGuides::SameOwner(mapping->preparedSource->OwnerInterfaces(),owner) ||
           mapping->preparedDepth!=mapping->preparedSource->Identity() || mapping->preparedTransfer!=transfer)return {};
        GenshinGuides::Owner canonical{};
        if(!GenshinGuides::OwnerIdentity::Context(context).ToCanonical(owner,canonical) ||
           !GenshinGuides::SameOwner(mapping->owner,canonical))return {};
        auto ordered=std::make_shared<OrderedDepthRead>();
        if(FAILED(context->QueryInterface(IID_PPV_ARGS(&ordered->context))))return {};
        ordered->transfer=std::move(transfer);ordered->isolatedOutput=mapping->isolatedJob;
        // The existing PictureJob ordered its own source read and submission.
        // Its output fence is the only dependency needed before the mapped
        // D3D11 copy. Failed Wait keeps that job's original quarantine.
        if(!ordered->isolatedOutput->OrderRead(ordered->context.Get()))return {};
        return ordered;
    }
    struct Reader {
        std::shared_ptr<ProducerFrame> producer;
        std::shared_ptr<Lease> transport;
        std::shared_ptr<const CompletedTransfer> completion;
        std::shared_ptr<const OrderedDepthRead> queuedRead;
        std::shared_ptr<const void> mapping;
        Owner owner{};
        Microsoft::WRL::ComPtr<ID3D11Resource> source;
        Microsoft::WRL::ComPtr<IUnknown> copy11;
        Microsoft::WRL::ComPtr<ID3D12Resource> copy12;
        Microsoft::WRL::ComPtr<ID3D12Fence> readyFence,doneFence;
        uint64_t readyValue=0,doneValue=0;
        bool started=false,retired=false;
        static bool Reached(ID3D12Fence* fence,uint64_t value) noexcept {
            if(!fence || !value)return false;
            const auto completed=fence->GetCompletedValue();
            return completed!=UINT64_MAX && completed>=value;
        }
        bool Complete() const noexcept {
            return retired && Reached(readyFence.Get(),readyValue) && Reached(doneFence.Get(),doneValue);
        }
        void ReleaseCompletedRoots() noexcept {
            if(owner.thread!=GetCurrentThreadId() || !Complete())return;
            // HeldFrame may retain this retired wrapper until its ring slot is
            // reused, AFTER the next picture mapper needs a free job. Retire
            // the payload here on BOTH real fences, not on wrapper use_count.
            // Keep the fence witnesses/owner so ReaderRetired stays truthful.
            mapping.reset();queuedRead.reset();completion.reset();source.Reset();
            copy11.Reset();copy12.Reset();producer.reset();transport.reset();
        }
    };
    // Three bounded slots cover the real reader/copy fence pipeline. Full,
    // failed, unfenced or foreign-owner occupancy still skips depth, never SR.
    // These roots survive destruction of Ticket, FrameInput AND this wrapper.
    struct RetainedRing {
        std::array<std::shared_ptr<Reader>,3> slots{};
        size_t next=0;
    };
    static RetainedRing& Retained() {static auto* ring=new RetainedRing;return *ring;}
    static std::mutex& RetainedMutex() {static auto* mutex=new std::mutex;return *mutex;}
    static bool ReclaimableRetained(const std::shared_ptr<Reader>& slot) noexcept {
        if(!slot || slot->owner.thread!=GetCurrentThreadId())return false;
        if(slot->Complete()){slot->ReleaseCompletedRoots();return true;}
        return !slot->started && slot.use_count()==1;
    }
    std::shared_ptr<Reader> reader_;
public:
    // Reclaim only already-retired real readers before mapping allocation.
    // Waiting until Create after Resolve can strand both bounded mapping slots.
    static void CollectRetained() {
        std::lock_guard lock(RetainedMutex());auto& retained=Retained();
        for(auto& slot:retained.slots)if(ReclaimableRetained(slot))slot.reset();
    }
    static bool ProducerReady(const std::shared_ptr<Lease>& transport,const Owner& owner) noexcept {
        if(!transport || owner.thread!=GetCurrentThreadId() || !transport->Poll())return false;
        const auto observed=transport->Snapshot();
        // These completion bits are set only by Lease::Poll using the real
        // input11/wait12/output12/retire11 fences. No pointer/Live inference.
        return GenshinGuides::SameOwner(observed.owner,owner) && !observed.revoked &&
            observed.stage==Stage::Complete && observed.inputFence && observed.submissionFence && observed.consumerFence &&
            observed.parametersRestored && observed.pipelineRestored && observed.copyback &&
            observed.inputsComplete && observed.inputWaitComplete && observed.consumerComplete && observed.copybackComplete;
    }
    static native::BridgeDepthLeasePtr Create(std::shared_ptr<ProducerFrame> producer,
        std::shared_ptr<Lease> transport,const Owner& owner,ID3D11Resource* source,std::shared_ptr<const GenshinDepthMapping::Node> mapping,
        std::shared_ptr<const CompletedTransfer> completion,std::shared_ptr<const OrderedDepthRead> queuedRead={}) {
        auto refuse=[&](const char* stage)->native::BridgeDepthLeasePtr {
            static std::atomic<unsigned> reports{0};
            if(reports.fetch_add(1,std::memory_order_relaxed)<16)
                LOG_INFO("R823 depth lease: pid={} generation={} frame={} stage={} producer={} live={} guides={} source={} completion={} queued_read={} transport={} thread={}/{} no_authority=true",
                    GetCurrentProcessId(),owner.generation,owner.sourceFrame,stage,static_cast<bool>(producer),producer&&producer->Live(),
                    producer&&static_cast<bool>(producer->Guides()),reinterpret_cast<uintptr_t>(source),static_cast<bool>(completion),
                    static_cast<bool>(queuedRead),static_cast<bool>(transport),GetCurrentThreadId(),owner.thread);
            return {};
        };
        if(!producer || !producer->Live() || !producer->Guides() || !transport || !source || !mapping ||
           owner.thread!=GetCurrentThreadId() || !GenshinGuides::SameOwner(producer->Owner(),owner))return refuse("producer-or-owner");
        if(queuedRead) {
            if(completion || transport->Snapshot().revoked || !queuedRead->transfer || !queuedRead->context ||
               !GenshinGuides::SameOwner(queuedRead->transfer->Get().owner,owner))return refuse("unordered-native-read");
        } else if(!completion || !GenshinGuides::SameOwner(completion->Get().owner,owner) ||
                  !ProducerReady(transport,owner))return refuse("producer-or-completion");
        // Both private proof paths retain the real native source/import/fence
        // roots. Only the NR-only path asserts CPU-observed completion.
        Microsoft::WRL::ComPtr<IUnknown> identity;
        const auto& native=queuedRead?queuedRead->transfer->Get():completion->Get();
        if(!native.sources[1] || !native.imported[1] ||
           FAILED(native.sources[1]->QueryInterface(IID_PPV_ARGS(&identity))) ||
           reinterpret_cast<uint64_t>(identity.Get())!=producer->Guides()->Get().textures[1].identity)return refuse("native-source-identity");
        std::lock_guard lock(RetainedMutex());auto& retained=Retained();
        std::shared_ptr<Reader>* available=nullptr;
        for(auto& slot:retained.slots)if(ReclaimableRetained(slot))slot.reset();
        for(size_t i=0;i<retained.slots.size();++i) {
            const auto index=(retained.next+i)%retained.slots.size();
            if(!retained.slots[index]) {available=&retained.slots[index];retained.next=(index+1)%retained.slots.size();break;}
        }
        if(!available)return refuse("retained-reader-busy");
        auto next=std::make_shared<PreparedDepthLease>();next->reader_=std::make_shared<Reader>();
        auto& r=*next->reader_;r.producer=std::move(producer);r.transport=std::move(transport);r.owner=owner;r.source=source;
        r.completion=std::move(completion);r.queuedRead=std::move(queuedRead);
        const auto isolated=mapping->isolatedJob;
        if(mapping->preparedSource && mapping->preparedTransfer!=(r.queuedRead?r.queuedRead->transfer:nullptr))
            return refuse("shared-packet-identity");
        r.mapping=std::move(mapping);*available=next->reader_;
        // Retain the reader and mapper before ordering the genuine isolated
        // output. A failed Wait poisons/quarantines the physical picture job.
        if(r.queuedRead && r.queuedRead->isolatedOutput && r.queuedRead->isolatedOutput!=isolated)
            return refuse("isolated-output-identity");
        if(isolated&&(!r.queuedRead || (!r.queuedRead->isolatedOutput &&
           !isolated->OrderRead(r.queuedRead->context.Get()))))return refuse("isolated-output-order");
        return next;
    }
    bool Ready() const noexcept override {
        if(!reader_ || reader_->owner.thread!=GetCurrentThreadId() || reader_->retired ||
           !reader_->source || !reader_->producer || !reader_->producer->Live())return false;
        if(reader_->queuedRead)return !reader_->completion && reader_->transport && reader_->mapping &&
            !reader_->transport->Snapshot().revoked &&
            reader_->queuedRead->context && reader_->queuedRead->transfer &&
            GenshinGuides::SameOwner(reader_->queuedRead->transfer->Get().owner,reader_->owner);
        return reader_->completion && GenshinGuides::SameOwner(reader_->completion->Get().owner,reader_->owner) &&
            ProducerReady(reader_->transport,reader_->owner);
    }
    bool BeginReader(IUnknown* copy11,ID3D12Resource* copy12,ID3D12Fence* fence,uint64_t value) noexcept override {
        if(!Ready() || reader_->started || !copy11 || !copy12 || !fence || !value)return false;
        Microsoft::WRL::ComPtr<ID3D12Device> copyDevice,fenceDevice;
        if(FAILED(copy12->GetDevice(IID_PPV_ARGS(&copyDevice))) ||
           FAILED(fence->GetDevice(IID_PPV_ARGS(&fenceDevice))) || copyDevice.Get()!=fenceDevice.Get())return false;
        auto& r=*reader_;r.copy11=copy11;r.copy12=copy12;r.readyFence=fence;r.readyValue=value;r.started=true;return true;
    }
    bool MatchesReader(ID3D12Resource* copy12,ID3D12Fence* fence,uint64_t value) const noexcept override {
        return Ready() && reader_->started && !reader_->doneFence && reader_->copy12.Get()==copy12 &&
            reader_->readyFence.Get()==fence && reader_->readyValue==value;
    }
    bool BindReaderCompletion(ID3D12Fence* fence,uint64_t value) noexcept override {
        // Revocation closes admission, never the ability to record retirement.
        if(!reader_ || reader_->owner.thread!=GetCurrentThreadId() || !reader_->started || reader_->retired ||
           reader_->doneFence || !fence || !value)return false;
        Microsoft::WRL::ComPtr<ID3D12Device> copyDevice,fenceDevice;
        if(FAILED(reader_->copy12->GetDevice(IID_PPV_ARGS(&copyDevice))) ||
           FAILED(fence->GetDevice(IID_PPV_ARGS(&fenceDevice))) || copyDevice.Get()!=fenceDevice.Get())return false;
        reader_->doneFence=fence;reader_->doneValue=value;return true;
    }
    bool RetireReader() noexcept override {
        if(!reader_ || reader_->owner.thread!=GetCurrentThreadId() || !reader_->started ||
           !reader_->doneFence || !reader_->doneValue)return false;
        reader_->retired=true;return true;
    }
    bool ReaderRetired() const noexcept override {
        return reader_ && reader_->owner.thread==GetCurrentThreadId() && reader_->Complete();
    }
};

inline BridgeDepthReceipt ProducerFrame::PreparedDepth(uint64_t pictureSerial,GenshinDepthMapping::Capture* mapping,
    ID3D11DeviceContext* context,IUnknown* picture,bool nativeOnly) noexcept
{
    BridgeDepthReceipt result;
    try {
        std::lock_guard lock(mutex_);
        if(!Live() || !pictureSerial || !guides_ || !raw_[1] || !lease_)return result;
        auto facts=guides_->Get();const auto owner=facts.owner;
        if(owner.thread!=GetCurrentThreadId() || !owner.generation || !owner.sourceFrame || !owner.historyEpoch ||
           !GenshinGuides::SameOwner(owner,owner_) || !facts.textures[1].identity ||
           !facts.textures[1].width.value || !facts.textures[1].height.value ||
           facts.textures[1].format.value!=DXGI_FORMAT_R32_FLOAT)return result;
        Microsoft::WRL::ComPtr<IUnknown> identity;Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        if(FAILED(raw_[1]->QueryInterface(IID_PPV_ARGS(&identity))) ||
           reinterpret_cast<uint64_t>(identity.Get())!=facts.textures[1].identity ||
           FAILED(raw_[1]->QueryInterface(IID_PPV_ARGS(&texture))))return result;
        texture->GetDevice(&device);D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
        if(!device || reinterpret_cast<uint64_t>(device.Get())!=owner.device || FAILED(device->GetDeviceRemovedReason()) ||
           desc.Width!=facts.textures[1].width.value || desc.Height!=facts.textures[1].height.value ||
           desc.Format!=DXGI_FORMAT_R32_FLOAT || desc.ArraySize!=1 || desc.MipLevels!=1 ||
           desc.SampleDesc.Count!=1 || desc.SampleDesc.Quality)return result;
        const GenshinGuides::OwnerIdentity::Context identities(context);GenshinGuides::Owner canonicalOwner{};
        if(!identities.ToCanonical(owner,canonicalOwner))return result;
        // Only outward Present receipt identities are canonical. The genuine
        // producer/transport lease continues to use its original interface owner.
        result.source=raw_[1];result.device=canonicalOwner.device;result.context=canonicalOwner.context;
        result.deviceEpoch=owner.deviceEpoch;result.producerEpoch=owner.producerEpoch;result.configEpoch=owner.configEpoch;
        result.instance=owner.instance;result.generation=owner.generation;result.sourceFrame=owner.sourceFrame;
        result.pictureSerial=pictureSerial;result.epoch=owner.historyEpoch;result.width=desc.Width;result.height=desc.Height;
        result.provenance=facts.depthSource;
        result.sceneGrid=PreparedSceneGrid::Observe(facts);result.nativeGuide=guides_->NativeGuideForPresent();
        result.preparedPacket=PreparedPacketLocked();
        // A failed native source packet cannot be replaced by labels or a
        // separately exported packet. The old strict raster branch is separate.
        if(result.preparedPacket)result.nativeTransfer=result.preparedPacket->Transfer();
        else result.nativeGuide.reset();
        result.producerWidth=desc.Width;result.producerHeight=desc.Height;
        // Present source steps: 1 native transfer, 2 capture absent, 3 order,
        // 4 grid, 5 owner, 6 mapping, 7 reader admission, 8 mapped reader ready.
        unsigned step=0;bool mappingKnown=false;
        std::shared_ptr<const CompletedTransfer> completion;
        std::shared_ptr<const PreparedDepthLease::OrderedDepthRead> queuedRead;
        const auto publish=[&]()noexcept {try{
            // One selected-source state transition at this existing Present
            // publication boundary. No per-draw log/IO/extra lock or byte dump.
            struct Publication {uint64_t calls=0;unsigned burst=0,reason=UINT_MAX,refusal=UINT_MAX,capture=UINT_MAX;
                unsigned step=UINT_MAX;uintptr_t source=0;uint64_t deviceEpoch=0,configEpoch=0;bool mapped=false;};
            static thread_local Publication published;
            const auto& content=facts.depthSource;const auto reason=mapping?unsigned(mapping->Status()):unsigned(GenshinDepthMapping::Reason::NoScene);
            const bool changed=published.reason!=reason || published.refusal!=content.refusal || published.capture!=content.captureReason ||
                published.source!=content.selected || published.deviceEpoch!=owner.deviceEpoch || published.configEpoch!=owner.configEpoch ||
                published.mapped!=mappingKnown || published.step!=step;
            if(published.calls!=UINT64_MAX)++published.calls;
            const bool emit=(published.burst<24 && changed) || published.calls%300==0;
            published.reason=reason;published.refusal=content.refusal;published.capture=content.captureReason;published.source=content.selected;
            published.deviceEpoch=owner.deviceEpoch;published.configEpoch=owner.configEpoch;published.mapped=mappingKnown;published.step=step;
            if(emit){if(published.burst<24)++published.burst;
                LOG_INFO("Genshin depth mapping: pid={} generation={} sourceFrame={} pictureSerial={} source_step={} reason={} reason_text={} mapped={} producer_ready={} producer_cpu_complete={} producer_read_ordered={} depth_convention={} depth_source={} clear_source={} jitter_sources={},{} semantic_failure={} returned_raster={} selected_source={} origin_source={} prepared_source={} copied_from={} source_lifetime={} origin_lifetime={} content_loss={} content_version={} clear_serial={} returned_serial={} copy_hops={} writer_total={} writer_covered={} writer_unresolved={} coverage_complete={} first_refusal={} first_boundary={} first_operation={} claim_refusal={} first_call={} stages={} capture_reason={} terminal_stage={} shader={} program_failure={} resource_slot={} resource_type={} terminal_instruction={} terminal_opcode={} dependency={} cb_slot={} cb_revision={} cb_loss={} cb_state={} observation_only=true",
                    GetCurrentProcessId(),owner.generation,owner.sourceFrame,pictureSerial,step,reason,mapping?GenshinDepthMapping::ReasonText(mapping->Status()):"NO_MAPPING_CAPTURE",mappingKnown,result.producerReady,
                    bool(completion),bool(queuedRead),unsigned(facts.depthConvention.value),unsigned(facts.depthConvention.source),
                    unsigned(facts.depthClear.source),unsigned(facts.depthToOutputJitterX.source),unsigned(facts.depthToOutputJitterY.source),facts.depthSemanticFailure,bool(facts.returnedRaster),
                    content.selected,content.original,content.prepared,content.copiedFrom,content.lifetime,content.originalLifetime,content.loss,content.version,content.clear,content.returned,content.copyHops,
                    content.total,content.covered,content.unresolved,content.coverageComplete,GenshinReturnedRaster::RefusalText(content.refusal),content.boundary,content.operation,content.claim,content.firstCall,content.stageMask,
                    GenshinDepthSemantic::CaptureFailureText(GenshinDepthSemantic::CaptureFailure(content.captureReason)),content.captureStage,content.shader,GenshinDepthSemantic::detail::ProgramFailureText(content.programFailure),content.resourceSlot,content.resourceType,content.terminalInstruction,content.terminalOpcode.data(),content.dependency,
                    content.cbSlot,content.cbRevision,content.cbLoss,content.cbState);
                if(result.preparedPacket){
                const auto* projection=result.preparedPacket->Source()->RasterProjection();
                const auto* interval=facts.returnedRaster?&facts.returnedRaster->Get():nullptr;
                LOG_INFO("R1067 shared prepared source: pid={} generation={} source_frame={} picture={} source_receipt={} packet={} prepared_identity={} immutable11={} immutable12={} source_size={}x{} picture_size={}x{} output_to_raster={},{} source_flip={} numerics_known={} convention={} clear={} raster_basis={} projection_z={},{} actual_clear_returned={} actual_clear={} clear_serial={} mapped={} mapping_owner={} mapping_source_receipt={} nr_reader={} producer_cpu_complete=UNKNOWN final_model_consumed=UNKNOWN",
                    GetCurrentProcessId(),owner.generation,owner.sourceFrame,pictureSerial,
                    reinterpret_cast<uintptr_t>(result.preparedPacket->Source().get()),reinterpret_cast<uintptr_t>(result.preparedPacket.get()),
                    result.preparedPacket->Source()->Identity(),reinterpret_cast<uintptr_t>(result.preparedPacket->Transfer()->Prepared(1)),
                    reinterpret_cast<uintptr_t>(result.preparedPacket->Transfer()->Get().imported[1].Get()),
                    result.producerWidth,result.producerHeight,result.width,result.height,
                    result.preparedPacket->Source()->OutputToRasterX(),result.preparedPacket->Source()->OutputToRasterY(),
                    result.preparedPacket->Source()->YFlip(),result.preparedPacket->Source()->NumericsKnown(),unsigned(result.preparedPacket->Source()->Convention()),
                    result.preparedPacket->Source()->ClearValue(),projection!=nullptr,projection?(*projection)[10]:0.f,projection?(*projection)[14]:0.f,
                    interval && interval->clearReturned,interval?interval->clearValue:0.f,content.clear,mappingKnown,reinterpret_cast<uintptr_t>(result.mappingOwner.get()),
                    result.mappingOwner?reinterpret_cast<uintptr_t>(result.mappingOwner->preparedSource.get()):0,static_cast<bool>(result.lease));
                }
            }
        }catch(...){} };
        struct PublishOnReturn {const decltype(publish)& callback;~PublishOnReturn(){callback();}} publication{publish};
        // FG receives the actual successful SR packet. Its consumer orders
        // reads on the owning D3D11 context; CPU completion/world replay are
        // unnecessary here. Shared-source NR independently orders this same
        // immutable packet; the non-native strict fallback retains its original
        // completed lease. No FG-enable or predecessor requirement is added.
        // Both existing adapters sample the same immutable packet. Preserve
        // the legal game-context/checkpoint route; unknown query coverage uses
        // the existing whole-chain isolated lane, never a query bypass.
        const bool sharedSource=static_cast<bool>(result.preparedPacket);
        const bool isolatedMapping=mapping&&mapping->RequiresIsolation(context);
        if(nativeOnly||isolatedMapping||sharedSource) {
            // Transport ownership/order is independent of the optional native
            // game-motion guide. Mapping below still requires OrderMappingRead
            // and its own raster semantics; a transfer alone admits neither.
            if(!result.nativeTransfer)result.nativeTransfer=PreparedTransferLocked();
            if(!result.nativeTransfer){step=1;return result;}
            if(nativeOnly&&result.nativeGuide) {
                // The lease compares the guide and canonicalized producer in the
                // same COM representation. The transport lookup above intentionally
                // used the original interface owner instead.
                result.nativeLease=NativeQueuedDepthLease::Create(shared_from_this(),result.nativeGuide,result.nativeTransfer,canonicalOwner);
                result.producerSource=raw_[1];result.source=raw_[1];
                result.transformKnown=true;result.coverage=true;
                result.width=result.nativeGuide->frame.outputWidth;result.height=result.nativeGuide->frame.outputHeight;
                // Legacy FG normalization control, preserved for its actual
                // downstream depthInverted readers. This is NOT source DeviceZ:
                // PreparedDepthSource stays Unknown until the actual proof,
                // and this raw NR lease remains UnmappedProducer.
                result.convention=result.nativeGuide->camera.reversed?GenshinGuides::DepthConvention::DeviceZReversed:GenshinGuides::DepthConvention::DeviceZForward;
            }
        }
        auto nativeCompletion=!nativeOnly&&!isolatedMapping&&!sharedSource && result.nativeGuide?lease_->Completed(owner):nullptr;
        static std::atomic<unsigned> nativeReports{0};
        if(nativeReports.fetch_add(1,std::memory_order_relaxed)<16)
            LOG_INFO("R835 native depth receipt: pid={} generation={} source_frame={} picture={} guide={} completion={} native_identity={} no_authority=true",
                GetCurrentProcessId(),owner.generation,owner.sourceFrame,pictureSerial,bool(result.nativeGuide),bool(nativeCompletion),result.NativeIdentityKnown());
        if(nativeCompletion){
            result.producerSource=raw_[1];result.source=raw_[1];result.producerReady=true;
            result.transformKnown=true;result.coverage=true;result.width=result.nativeGuide->frame.outputWidth;
            result.height=result.nativeGuide->frame.outputHeight;
            // Same legacy FG control; no NR numerical certificate.
            result.convention=result.nativeGuide->camera.reversed?GenshinGuides::DepthConvention::DeviceZReversed:GenshinGuides::DepthConvention::DeviceZForward;
        }
        if(mapping && result.sceneGrid.Matches(desc.Width,desc.Height)) {
            if(nativeOnly||isolatedMapping||sharedSource) {
                if(sharedSource && isolatedMapping){
                    // Source admission only: the existing private mapper orders
                    // its immutable packet. Bind the actual output dependency
                    // after ResolveIsolated returns that exact completed job.
                    result.producerReady=true;
                }else{
                    queuedRead=PreparedDepthLease::OrderMappingRead(result.nativeTransfer,owner,context,raw_[1]);
                    result.producerReady=static_cast<bool>(queuedRead);
                }
            } else {
                // Preserve the bounded CompletedDepth + ProducerReady gates
                // exclusively for NR-only game mapping; isolated/combined never call either.
                completion=nativeCompletion?nativeCompletion:lease_->CompletedDepth(owner,context,raw_[1]);
                if(completion)result.producerReady=PreparedDepthLease::ProducerReady(lease_,owner);
            }
        }
        if(!mapping || !result.producerReady || !result.sceneGrid.Matches(desc.Width,desc.Height)){
            step=!mapping?2:!result.producerReady?3:4;
            return result;
        }
        // Refresh immutable writer/CB facts, never fill unknown semantics.
        // The queued path makes no CPU-completion assertion about these facts.
        facts=guides_->DepthFactsForPresent();result.provenance=facts.depthSource;
        if(!Live() || !GenshinGuides::SameOwner(facts.owner,owner) ||
           facts.textures[1].identity!=reinterpret_cast<uint64_t>(identity.Get()) ||
           (facts.preparedSource && (!result.preparedPacket || result.preparedPacket->Source()!=facts.preparedSource ||
            !result.preparedPacket->Matches(facts)))){step=5;return result;}
        result.sceneGrid=PreparedSceneGrid::Observe(facts);if(!result.nativeGuide)result.convention=facts.depthConvention.value;
        if(!result.sceneGrid.Matches(desc.Width,desc.Height)){step=4;return result;}
        PreparedDepthLease::CollectRetained();
        auto mapped=isolatedMapping?
            mapping->ResolveIsolated(context,raw_[1],facts,picture,pictureSerial,result.nativeTransfer):
            mapping->Resolve(context,raw_[1],facts,picture,pictureSerial,
                sharedSource?result.nativeTransfer:nullptr);
        // Native validation may discover a fresh refusal after the cheap
        // routing snapshot. Only an intact WHOLE deferred chain can retry in
        // isolation; ResolveIsolated refuses any consumed/partial checkpoint.
        if(!mapped.Known()&&!isolatedMapping&&mapping->Status()==GenshinDepthMapping::Reason::UnprovedQueryScope){
            if(!result.nativeTransfer)result.nativeTransfer=PreparedTransferLocked();
            if(!sharedSource)queuedRead=PreparedDepthLease::OrderMappingRead(result.nativeTransfer,owner,context,raw_[1]);
            completion.reset();result.producerReady=sharedSource || static_cast<bool>(queuedRead);
            if(!result.producerReady){step=3;return result;}
            mapped=mapping->ResolveIsolated(context,raw_[1],facts,picture,pictureSerial,result.nativeTransfer);
        }
        if(!mapped.Known()){step=6;return result;}
        if(sharedSource && mapped.owner->isolatedJob){
            queuedRead=PreparedDepthLease::OrderIsolatedMappingRead(result.nativeTransfer,owner,context,mapped.owner);
            if(!queuedRead){step=7;return result;}
        }
        result.producerSource=raw_[1];
        mappingKnown=true;result.source=mapped.texture;result.mappingOwner=mapped.owner;result.width=mapped.width;result.height=mapped.height;
        result.convention=mapped.convention;
        // These receipts refer to an executed private raster mapping and the
        // ResolveDepth per-pixel validity writer, never descriptor equality.
        result.transformKnown=mapped.owner->scene && mapped.owner->returned;
        result.coverage=mapped.texture && mapped.owner->serial==pictureSerial;
        result.lease=PreparedDepthLease::Create(shared_from_this(),lease_,owner,result.source.Get(),mapped.owner,
            completion,queuedRead);
        step=result.lease?8:7;
    } catch(...) {return {};}
    return result;
}
// This synchronous binding surrounds CapturePipeline::Submit. It never escapes to another call.
class ProducerInvocation final {
    inline static thread_local ProducerInvocation* active_=nullptr;
    ProducerInvocation* previous_=nullptr;
    std::shared_ptr<ProducerFrame> frame_;
public:
    explicit ProducerInvocation(std::shared_ptr<ProducerFrame> frame):previous_(active_),frame_(std::move(frame)) {
        if(previous_){if(previous_->frame_)previous_->frame_->Revoke();if(frame_)frame_->Revoke();}
        active_=this;
    }
    ~ProducerInvocation(){if(active_==this)active_=previous_;}
    ProducerInvocation(const ProducerInvocation&)=delete;
    ProducerInvocation& operator=(const ProducerInvocation&)=delete;
    static std::shared_ptr<ProducerFrame> Current()noexcept{return active_?active_->frame_:nullptr;}
};

// Available only to Dx11Owner, before ValidateResources and before provider
// registration. The current TLS producer, immutable receipt and real slots must
// all agree. Normal external/native resources cannot populate the capability.
inline GenshinFsr::PreparedResourceAdmission ProducerFrame::PreparedAdmission(ID3D11DeviceContext* context,
    const GenshinFsr::Parameters& p,const GenshinFsr::ResourceSet& resources) noexcept {
    GenshinFsr::PreparedResourceAdmission result;
    if(ProducerInvocation::Current().get()!=this || !context)return result;
    std::lock_guard lock(mutex_);
    if(!Live() || attempted_ || lease_ || !guides_ || !sourceSnapshot_ ||
       (!preparedSrInputs_ && !preparedSrOutput_))return result;
    const auto& o=owner_;const auto& facts=guides_->Get();
    if(!GenshinGuides::SameOwner(o,facts.owner) || o.thread!=GetCurrentThreadId() ||
       !o.device || !o.context || !o.deviceEpoch || !o.producerEpoch || !o.configEpoch || !o.sourceFrame ||
       o.instance!=p.instance || o.historyEpoch!=p.epoch || o.generation!=p.generation ||
       o.sourceFrame!=p.sourceFrame || o.context!=reinterpret_cast<uint64_t>(context) ||
       context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return result;
    Microsoft::WRL::ComPtr<ID3D11Device> device;context->GetDevice(&device);
    if(!device || reinterpret_cast<uint64_t>(device.Get())!=o.device || FAILED(device->GetDeviceRemovedReason()) ||
       p.originX || p.originY || !p.renderWidth || !p.renderHeight)return result;
    const GenshinFsr::ResourceInfo* actual[]={&resources.color,&resources.depth,&resources.motion,&resources.output};
    for(size_t i=0;i<4;++i) {
        Microsoft::WRL::ComPtr<IUnknown> identity;
        if(!raw_[i] || resources_[i].Get()!=raw_[i] ||
           actual[i]->resourcePointer!=reinterpret_cast<uintptr_t>(raw_[i]) ||
           FAILED(raw_[i]->QueryInterface(IID_PPV_ARGS(&identity))) ||
           actual[i]->identity!=reinterpret_cast<uintptr_t>(identity.Get()) ||
           actual[i]->identity!=facts.textures[i].identity || actual[i]->deviceIdentity!=o.device ||
           facts.textures[i].device!=o.device)return result;
    }
    if(preparedSrInputs_) {
        const auto& slot=*preparedSrInputs_;
        if(slot.context_.Get()!=context || slot.thread_!=o.thread || slot.device_.Get()!=device.Get())return result;
        for(size_t i=0;i<2;++i) {
            auto* texture=slot.textures_[i].Get();const auto& input=*actual[i+1];
            if(!texture || !slot.uavs_[i] || raw_[i+1]!=texture)return result;
            Microsoft::WRL::ComPtr<ID3D11Device> textureDevice;
            Microsoft::WRL::ComPtr<ID3D11Resource> uavResource;
            texture->GetDevice(&textureDevice);slot.uavs_[i]->GetResource(&uavResource);
            Microsoft::WRL::ComPtr<IUnknown> uavIdentity;
            if(textureDevice.Get()!=device.Get() || !uavResource ||
               FAILED(uavResource.As(&uavIdentity)) ||
               reinterpret_cast<uintptr_t>(uavIdentity.Get())!=input.identity)return result;
            D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);
            if(d.Width!=p.renderWidth || d.Height!=p.renderHeight || d.MipLevels!=1 || d.ArraySize!=1 ||
               d.SampleDesc.Count!=1 || d.SampleDesc.Quality || d.Usage!=D3D11_USAGE_DEFAULT || d.CPUAccessFlags ||
               d.BindFlags!=(D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS) ||
               d.MiscFlags!=(D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE) ||
               d.Format!=(i==0?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_R16G16_FLOAT) ||
               facts.textures[i+1].width.value!=d.Width || facts.textures[i+1].height.value!=d.Height ||
               facts.textures[i+1].format.value!=d.Format)return result;
        }
    }
    if(preparedSrOutput_) {
        const auto& output=*preparedSrOutput_;
        if(!output.Authenticate(o,context,raw_[3],p.outputWidth,p.outputHeight,resources.output.desc.Format) ||
           facts.textures[3].width.value!=p.outputWidth || facts.textures[3].height.value!=p.outputHeight ||
           facts.textures[3].format.value!=resources.output.desc.Format)return result;
    }
    // Match also checks receipt retirement (including a derived receipt's
    // parent) and the exact temporal values. No NR/FG semantic gate is used.
    GenshinGuides::Call call;call.owner=o;call.resources=raw_;
    call.renderWidth=p.renderWidth;call.renderHeight=p.renderHeight;
    call.jitterX=p.jitterX;call.jitterY=p.jitterY;
    const float sign=p.positiveMotionScale?1.0f:-1.0f;
    call.motionScaleX=sign*p.renderWidth*p.motionScale;call.motionScaleY=sign*p.renderHeight*p.motionScale;
    call.temporalParametersRead=true;
    if(guides_->Match(call)!=GenshinGuides::Reason::None)return result;
    result.parameters_=p;
    result.device_=o.device;result.context_=o.context;result.thread_=o.thread;
    result.deviceEpoch_=o.deviceEpoch;result.producerEpoch_=o.producerEpoch;result.configEpoch_=o.configEpoch;
    if(preparedSrInputs_) {
        result.depth_=resources.depth;result.motion_=resources.motion;result.slot_=preparedSrInputs_;
    }
    if(preparedSrOutput_) {
        result.output_=resources.output;result.outputSlot_=preparedSrOutput_;
    }
    return result;
}

// CPU observation of the current Genshin recording wait, on the same thread as
// DispatchGenshinFsr2Dx11. A timeout is retryable only when no DX12 lease was
// ever created; it never reclassifies an already-submitted provider failure.
inline thread_local bool recordingWaitTimeout=false;
inline void ClearRecordingWaitTimeout() noexcept {recordingWaitTimeout=false;}
inline void NoteRecordingWaitTimeout() noexcept {recordingWaitTimeout=true;}
inline bool ConsumeRecordingWaitTimeout() noexcept {
    const bool result=recordingWaitTimeout;recordingWaitTimeout=false;return result;
}
}
