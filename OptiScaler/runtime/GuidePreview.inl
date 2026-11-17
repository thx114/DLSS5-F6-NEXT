// SPDX-License-Identifier: GPL-3.0-or-later
// Included once by GuideCapture.cpp, sharing its event-driven retirement worker.
namespace GuideCapture {
namespace PreviewDetail {
constexpr unsigned SlotCount=3,ReadersPerSlot=8,UsesPerSlot=ReadersPerSlot+1;
struct Display {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12DescriptorHeap> heap;
    std::vector<std::pair<DXGI_FORMAT,ComPtr<ID3D12PipelineState>>> pipelines;
    std::array<ComPtr<ID3D12Fence>,ReadersPerSlot> fences;
};
struct Surface {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> computeRoot;
    ComPtr<ID3D12PipelineState> compute;
    ComPtr<ID3D12Resource> texture;
    ComPtr<ID3D12DescriptorHeap> computeHeap;
    std::array<ComPtr<ID3D12Resource>,2> inputs;
    // The output list supplies its own interface domain. All display objects,
    // including reader fences, are created there, never on the producer proxy.
    std::shared_ptr<Display> display;
    ComPtr<ID3D12Device> requestedDisplayDevice;
    bool displayPreparing=false;
    unsigned width=0,height=0;
    // Balanced even on the very first executable recording: quarantine retains
    // uncertain owners but cannot prevent the application's actual replay.
    D3D12_RESOURCE_STATES state=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
};
struct Use {
    ComPtr<ID3D12CommandList> list;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12Fence> fence;
    std::shared_ptr<Display> display;
    uint64_t generation=0,value=0,completedValue=0,diagnosticListIdentity=0,diagnosticQueueIdentity=0;
    bool pre=false,post=false,reset=false,done=false,quarantined=false,signalled=false,signalling=false;
    bool Active() const {return list!=nullptr;}
};
struct Slot {
    std::shared_ptr<Surface> surface;Use producer;std::array<Use,ReadersPerSlot> readers;
    uint64_t serial=0,revision=0,source=0,tick=0,epoch=0,feature=0,configuration=0,owner=0,completedTick=0;
    Stream consumer=Stream::Source;bool colourHdr=false;bool reserved=false,published=false,discarded=false,preparing=false;
    D3D12_RESOURCE_STATES startState=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ComPtr<ID3D12Device> requestedDevice;unsigned requestedWidth=0,requestedHeight=0;
};
struct State {
    std::mutex mutex;PreviewDemand demand;std::atomic<unsigned> mode{0},historyView{0};
    std::atomic<const char*> status{"Preview off"};
    std::atomic<const char*> outputStatus{"Output not observed"};
    std::atomic<uint64_t> attempts{0},reservations{0},recorded{0},submitted{0},completed{0},resets{0};
    std::atomic<uint64_t> publications{0},acquisitions{0},draws{0},outputs{0},lockBusy{0},rateLimited{0},slotsBusy{0};
    std::array<Slot,SlotCount> slots;
    std::array<std::atomic<uint64_t>,SlotCount> cancelled{};
    PreviewSnapshot displayed; // metadata only, never pins a storage slot
    // Exact canonical list registration. Misses never take a mutex or call COM.
    std::array<std::atomic<ID3D12CommandList*>,SlotCount*UsesPerSlot> active{};
    uint64_t revision=1,serial=0,generation=0,lastTick=0,lastSource=0,lastFeature=0,lastOwner=0,lastConfig=0;
    unsigned latest=UINT_MAX;
#ifdef GUIDE_PREVIEW_TESTING
    bool failSignal=false;
#endif
};
State& Get(){static auto* s=new State;return *s;}
bool Matches(UINT count,ID3D12CommandList* const* lists){
    if(!lists)return false;for(auto& key:Get().active){const auto p=key.load(std::memory_order_acquire);
        if(p)for(UINT n=0;n<count;++n)if(p==lists[n])return true;}return false;
}
template<class F> void Each(F&& f){auto& s=Get();for(unsigned i=0;i<SlotCount;++i){
    f(s.slots[i],s.slots[i].producer,i*UsesPerSlot,true);
    for(unsigned j=0;j<ReadersPerSlot;++j)f(s.slots[i],s.slots[i].readers[j],i*UsesPerSlot+j+1,false);}}
Stream Selected(PreviewMode mode){switch(mode){
case PreviewMode::NrOriginal:return Stream::Source;
case PreviewMode::NrColour:case PreviewMode::NrDepth:case PreviewMode::NrMotion:return Stream::NrModel;
case PreviewMode::FgColour:case PreviewMode::FgDepth:case PreviewMode::FgMotion:return Stream::Fg;
case PreviewMode::History:return Stream::History;default:return Stream::Count;}}
void Mark(const char* text){Get().status.store(text,std::memory_order_release);}
bool Compile(const char* text,size_t length,const char* entry,const char* target,ComPtr<ID3DBlob>& blob){
    ComPtr<ID3DBlob> errors;return SUCCEEDED(D3DCompile(text,length,"F6 independent GPU preview",nullptr,nullptr,entry,target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&errors));
}
std::shared_ptr<Surface> Prepare(ID3D12Device* device,unsigned width,unsigned height){
    auto p=std::make_shared<Surface>();p->device=device;p->width=width;p->height=height;
    ComPtr<ID3DBlob> code,serialized,error;
    if(!Compile(GuidePreviewShader::Compute,sizeof(GuidePreviewShader::Compute)-1,"main","cs_5_0",code))return {};
    D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,2}};
    D3D12_ROOT_PARAMETER roots[2]{};roots[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;roots[0].Constants={0,0,16};
    roots[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;roots[1].DescriptorTable={2,ranges};
    D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=2;rd.pParameters=roots;
    if(FAILED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error))||
       FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&p->computeRoot))))return {};
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=p->computeRoot.Get();pd.CS={code->GetBufferPointer(),code->GetBufferSize()};
    if(FAILED(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&p->compute))))return {};
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;hd.NumDescriptors=3;
    if(FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&p->computeHeap))))return {};
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC td{};
    td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=width;td.Height=height;td.DepthOrArraySize=1;td.MipLevels=1;
    td.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;td.SampleDesc.Count=1;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if(FAILED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,p->state,nullptr,IID_PPV_ARGS(&p->texture))))return {};
    return p;
}
ID3D12PipelineState* PrepareDisplayPipeline(Display& display,DXGI_FORMAT format){
    for(auto& p:display.pipelines)if(p.first==format)return p.second.Get();
    // HDR needs an explicit output transfer contract; never display SDR as PQ.
    if(format!=DXGI_FORMAT_R8G8B8A8_UNORM&&format!=DXGI_FORMAT_B8G8R8A8_UNORM&&
       format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB&&format!=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)return nullptr;
    ComPtr<ID3DBlob> vs,ps;if(!Compile(GuidePreviewShader::Display,sizeof(GuidePreviewShader::Display)-1,"vs","vs_5_0",vs)||
        !Compile(GuidePreviewShader::Display,sizeof(GuidePreviewShader::Display)-1,"ps","ps_5_0",ps))return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=display.root.Get();
    pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    auto& blend=pd.BlendState.RenderTarget[0];
    blend.SrcBlend=D3D12_BLEND_ONE;blend.DestBlend=D3D12_BLEND_ZERO;blend.BlendOp=D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlendAlpha=D3D12_BLEND_ZERO;blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;
    blend.LogicOp=D3D12_LOGIC_OP_NOOP;blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask=UINT_MAX;pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable=TRUE;pd.DepthStencilState.DepthEnable=FALSE;pd.DepthStencilState.StencilEnable=FALSE;
    pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pd.NumRenderTargets=1;pd.RTVFormats[0]=format;pd.SampleDesc.Count=1;
    ComPtr<ID3D12PipelineState> pipeline;if(FAILED(display.device->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pipeline))))return nullptr;
    auto* result=pipeline.Get();display.pipelines.emplace_back(format,std::move(pipeline));return result;
}
std::shared_ptr<Display> PrepareDisplay(ID3D12Device* device,ID3D12Resource* texture){
    auto p=std::make_shared<Display>();p->device=device;
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};
    D3D12_ROOT_PARAMETER roots[2]{};roots[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;roots[0].Constants={0,0,4};
    roots[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;roots[1].DescriptorTable={1,&range};
    D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=2;rd.pParameters=roots;
    rd.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> serialized,error;
    if(FAILED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error))||
       FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&p->root))))return {};
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;hd.NumDescriptors=1;
    if(FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&p->heap))))return {};
    D3D12_SHADER_RESOURCE_VIEW_DESC vd{};vd.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;vd.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    vd.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;vd.Texture2D.MipLevels=1;
    device->CreateShaderResourceView(texture,&vd,p->heap->GetCPUDescriptorHandleForHeapStart());
    constexpr DXGI_FORMAT formats[]={DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    for(auto format:formats)if(!PrepareDisplayPipeline(*p,format))return {};
    for(auto& fence:p->fences)if(FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))))return {};
    return p;
}
bool Register(Use& use,ID3D12GraphicsCommandList* list,ID3D12Device*,unsigned key){
    ComPtr<ID3D12CommandList> canonical;if(FAILED(list->QueryInterface(IID_PPV_ARGS(&canonical))))return false;
    if(!use.fence)return false;
    if(use.value==UINT64_MAX)return false;
    uint64_t generation=0;
    Each([&](Slot&,Use& other,unsigned,bool){if(other.Active()&&!other.reset&&other.list.Get()==canonical.Get())generation=other.generation;});
    use.list=std::move(canonical);use.generation=generation?generation:++Get().generation;++use.value;
    use.diagnosticListIdentity=reinterpret_cast<uintptr_t>(use.list.Get());use.diagnosticQueueIdentity=0;
    use.completedValue=0;
    use.pre=use.post=use.reset=use.done=use.quarantined=use.signalled=use.signalling=false;
    Get().active[key].store(use.list.Get(),std::memory_order_release);return true;
}
bool Poll(){
    std::array<std::shared_ptr<Surface>,SlotCount> retired;
    auto& s=Get();std::unique_lock lock(s.mutex);bool pending=false;
    Each([&](Slot& slot,Use& use,unsigned key,bool producer){
        if(!use.Active()||use.quarantined)return;
        if(use.post&&!use.signalled&&!use.signalling){
            use.signalling=true;auto queue=use.queue;auto fence=use.fence;const auto value=use.value;
            bool inject=false;
#ifdef GUIDE_PREVIEW_TESTING
            inject=s.failSignal;
#endif
            lock.unlock();const HRESULT hr=inject?E_FAIL:queue->Signal(fence.Get(),value);lock.lock();
            use.signalling=false;
            if(use.quarantined)return;
            if(FAILED(hr)){use.quarantined=true;Mark("Preview Signal failed; recording quarantined");return;}
            use.signalled=true;
        }
        if(use.post&&use.signalled&&!use.done){
            auto fence=use.fence;const auto generation=use.generation,value=use.value;
            lock.unlock();const auto complete=fence->GetCompletedValue();lock.lock();
            if(use.quarantined||!use.Active()||use.generation!=generation||use.value!=value)return;
            if(complete==UINT64_MAX){use.quarantined=true;Mark("Preview device removed; recording retained");return;}
            use.completedValue=complete;
            if(complete<use.value){pending=true;return;}use.done=true;if(producer)++s.completed;}
        // Publish only after both the actual producer fence AND successful Reset
        // of this exact executable recording. Completion alone permits replay.
        if(!use.done||!use.reset)return;
        if(producer&&!slot.discarded){slot.published=true;slot.completedTick=GuideDiagnostics::Now();
            ++s.publications;
            if(slot.revision==s.revision&&s.demand.mode!=PreviewMode::Off&&!s.demand.frozen&&
               (s.latest==UINT_MAX||slot.serial>s.slots[s.latest].serial)){
                s.latest=key/UsesPerSlot;Mark("GPU snapshot published after its producer fence and successful Reset");}}
        s.active[key].store(nullptr,std::memory_order_release);use.list.Reset();use.queue.Reset();use.display.reset();
        if(producer){for(auto& input:slot.surface->inputs)input.Reset();
            if(slot.discarded)slot.surface->state=slot.startState;}
    });
    for(unsigned n=0;n<SlotCount;++n){auto& slot=s.slots[n];
        if(slot.reserved&&s.cancelled[n].load(std::memory_order_acquire)==slot.serial)slot.reserved=false;
        bool readers=false;for(auto& read:slot.readers)readers|=read.Active();
        if(!slot.reserved&&!slot.producer.Active()&&!readers&&s.demand.mode==PreviewMode::Off&&
           slot.surface&&slot.surface.use_count()==1){retired[n]=std::move(slot.surface);slot.published=false;}}
    lock.unlock(); // Resource destruction is outside the source admission mutex.
    return pending;
}
bool Executing(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists,ExecutionReceipt& receipt){
    if(!queue||!Matches(count,lists))return false;auto& s=Get();std::lock_guard lock(s.mutex);
    Each([&](Slot&,Use& u,unsigned key,bool){if(!u.Active()||u.reset||u.quarantined)return;
        bool match=false;for(UINT n=0;n<count;++n)match|=u.list.Get()==lists[n];if(!match)return;
        if(u.pre&&!u.post&&u.queue.Get()==queue){receipt.preview[key]=u.generation;return;}
        if(u.pre||u.post){u.quarantined=true;Mark("Preview replay or queue mismatch; recording retained");return;}
        u.queue=queue;u.diagnosticQueueIdentity=reinterpret_cast<uintptr_t>(queue);
        u.pre=true;receipt.preview[key]=u.generation;});return true;
}
bool Submitted(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists,const ExecutionReceipt& receipt){
    if(!queue||!Matches(count,lists))return false;auto& s=Get();std::lock_guard lock(s.mutex);
    Each([&](Slot&,Use& u,unsigned key,bool){
        const auto generation=receipt.preview[key];
        if(!generation||!u.Active()||u.quarantined||u.generation!=generation)return;
        bool match=false;for(UINT n=0;n<count;++n)match|=u.list.Get()==lists[n];if(!match)return;
        if(!u.pre||u.queue.Get()!=queue){u.quarantined=true;Mark("Preview receipt queue mismatch; retained");return;}
        if(!u.post){u.post=true;if(key%UsesPerSlot==0)++s.submitted;}
    });return true;
}
bool Reset(ID3D12GraphicsCommandList* list){
    ID3D12CommandList* base=list;if(!Matches(1,&base))return false;auto& s=Get();std::lock_guard lock(s.mutex);
    Each([&](Slot& slot,Use& u,unsigned,bool producer){if(!u.Active()||u.quarantined||u.reset||u.list.Get()!=base)return;
        u.reset=true;if(producer)++s.resets;if(!u.pre){u.done=true;if(producer)slot.discarded=true;}});return true;
}
bool NeedsReset(ID3D12GraphicsCommandList* list){
    ID3D12CommandList* base=list;if(!Matches(1,&base))return false;auto& s=Get();
    std::unique_lock lock(s.mutex,std::try_to_lock);if(!lock.owns_lock())return false;
    bool needed=false;Each([&](Slot&,Use& u,unsigned,bool){
        needed|=u.Active()&&!u.reset&&!u.quarantined&&u.list.Get()==base;});return needed;
}
} // namespace PreviewDetail
void SetPreviewDemand(const PreviewDemand& requested) noexcept {
    try {auto& s=PreviewDetail::Get();auto d=requested;
        if(static_cast<unsigned>(d.mode)>8)d.mode=PreviewMode::Off;
        d.historyView=(std::min)(d.historyView,6u);
        d.gain=std::isfinite(d.gain)?(std::clamp)(d.gain,.01f,4096.f):16.f;
        {std::lock_guard lock(s.mutex);const auto& old=s.demand;
            if(d.mode==old.mode&&d.raw==old.raw&&d.flipY==old.flipY&&d.frozen==old.frozen&&d.gain==old.gain&&d.historyView==old.historyView)return;
            const bool changed=d.mode!=old.mode||d.raw!=old.raw||d.flipY!=old.flipY||d.gain!=old.gain||d.historyView!=old.historyView;
            if(changed){++s.revision;s.latest=UINT_MAX;s.displayed={};s.lastTick=0;s.lastSource=0;
                s.outputStatus.store("Waiting for authenticated physical output",std::memory_order_release);}
            s.demand=d;
            s.historyView.store(d.mode==PreviewMode::History?d.historyView:0,std::memory_order_release);
            s.mode.store(static_cast<unsigned>(d.mode),std::memory_order_release);
            PreviewDetail::Mark(d.mode==PreviewMode::Off?"Preview off":d.frozen?"Frozen immutable snapshot":"Waiting for selected actual input");}
        if(d.mode!=PreviewMode::Off)StartWorker(Storage());else Wake(Storage());
    }catch(...){}
}
PreviewDemand GetPreviewDemand() noexcept {try {auto& s=PreviewDetail::Get();std::lock_guard lock(s.mutex);return s.demand;}catch(...){return {};}}
PreviewMode GetPreviewMode() noexcept {
    return static_cast<PreviewMode>(PreviewDetail::Get().mode.load(std::memory_order_acquire));
}
unsigned HistoryDebugView() noexcept {
    // Advisory demand only. Contention must not masquerade as Off and cause
    // the NR path to retire/reallocate its diagnostic scratch repeatedly.
    return PreviewDetail::Get().historyView.load(std::memory_order_acquire);
}
bool PreviewActive() noexcept {return GetPreviewMode()!=PreviewMode::Off;}
bool NeedsOpenFgGuideRecording() noexcept {
    const auto mode=GetPreviewMode();
    // Conservatively keep a selected guide recording open even if frozen or
    // the optional preview mutex is busy. Neither is evidence of no demand.
    return mode==PreviewMode::FgDepth||mode==PreviewMode::FgMotion||OfflineWanted(Stream::Fg);
}
bool PreviewWanted(Stream stream) noexcept {
    auto& s=PreviewDetail::Get();if(PreviewDetail::Selected(static_cast<PreviewMode>(s.mode.load(std::memory_order_acquire)))!=stream)return false;
    try{std::unique_lock lock(s.mutex,std::try_to_lock);return lock.owns_lock()&&!s.demand.frozen;}catch(...){return false;}
}
bool TryBeginSample(Stream stream,SampleToken& token) noexcept {
    token={};auto& s=PreviewDetail::Get();
    if(PreviewDetail::Selected(GetPreviewMode())!=stream)return false;
    ++s.attempts;
    try {std::unique_lock lock(s.mutex,std::try_to_lock);
        if(!lock.owns_lock()){++s.lockBusy;return false;}
        if(s.demand.frozen||s.demand.mode==PreviewMode::Off||PreviewDetail::Selected(s.demand.mode)!=stream)return false;
        const auto now=GuideDiagnostics::Now();if(s.lastTick&&now-s.lastTick<34){++s.rateLimited;return false;}
        for(unsigned n=0;n<PreviewDetail::SlotCount;++n){auto& slot=s.slots[n];
            if(slot.reserved&&s.cancelled[n].load(std::memory_order_acquire)==slot.serial)slot.reserved=false;
            if(n==s.latest||slot.preparing||slot.reserved||slot.producer.Active()||(slot.surface&&slot.surface.use_count()!=1))continue;
            bool reading=false;for(auto& r:slot.readers)reading|=r.Active();if(reading)continue;
            slot.reserved=true;slot.published=false;slot.discarded=false;slot.serial=++s.serial;slot.revision=s.revision;
            slot.source=slot.epoch=slot.feature=slot.configuration=slot.owner=slot.completedTick=0;
            slot.producer.generation=slot.producer.completedValue=0;
            slot.producer.diagnosticListIdentity=slot.producer.diagnosticQueueIdentity=0;
            slot.producer.pre=slot.producer.post=slot.producer.reset=slot.producer.done=false;
            slot.producer.signalled=slot.producer.signalling=slot.producer.quarantined=false;
            token={n,slot.serial,s.revision,stream,s.demand.historyView};s.lastTick=now;++s.reservations;return true;}
        ++s.slotsBusy;
    }catch(...){}return false;
}
void CancelSample(SampleToken& token) noexcept {
    // Cancellation never waits for the worker or source lifecycle mutex. The
    // monotonically increasing serial prevents stale cancellation of a reuse.
    if(token.slot<PreviewDetail::SlotCount){auto& s=PreviewDetail::Get();auto& cancelled=s.cancelled[token.slot];
        auto old=cancelled.load(std::memory_order_relaxed);
        while(old<token.serial&&!cancelled.compare_exchange_weak(old,token.serial,std::memory_order_release,std::memory_order_relaxed)){}
        Wake(Storage());}
    token={};
}
namespace PreviewDetail {
bool PrepareOne(){
    auto& s=Get();
    for(unsigned n=0;n<SlotCount;++n){std::shared_ptr<Surface> surface;ComPtr<ID3D12Device> device;
        {std::lock_guard lock(s.mutex);surface=s.slots[n].surface;
            if(!surface||!surface->requestedDisplayDevice||surface->displayPreparing||s.demand.mode==PreviewMode::Off)continue;
            device=surface->requestedDisplayDevice;surface->displayPreparing=true;}
        // An immutable binding for this snapshot, prepared away from Present.
        // Old bindings remain held by their actual reader recordings.
        auto display=PrepareDisplay(device.Get(),surface->texture.Get());
        {std::lock_guard lock(s.mutex);surface->displayPreparing=false;
            if(surface->requestedDisplayDevice.Get()==device.Get()){
                surface->requestedDisplayDevice.Reset();
                if(display)surface->display=std::move(display);else Mark("Independent output preview preparation failed");}}
        return true;
    }
    for(unsigned n=0;n<SlotCount;++n){ComPtr<ID3D12Device> device;unsigned w=0,h=0;
        {std::lock_guard lock(s.mutex);auto& slot=s.slots[n];
            if(!slot.requestedDevice||slot.preparing||slot.reserved||slot.producer.Active()||s.demand.mode==PreviewMode::Off)continue;
            bool busy=false;for(auto& r:slot.readers)busy|=r.Active();
            if(busy||(slot.surface&&slot.surface.use_count()!=1)||n==s.latest)continue;
            device=slot.requestedDevice;w=slot.requestedWidth;h=slot.requestedHeight;
            slot.requestedDevice.Reset();slot.preparing=true;}
        // Producer storage and fence belong to its actual device interface.
        // Reader storage/fences are prepared separately on the output device.
        auto surface=Prepare(device.Get(),w,h);
        ComPtr<ID3D12Fence> fence;
        if(surface&&FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))))surface.reset();
        {std::lock_guard lock(s.mutex);auto& slot=s.slots[n];
            slot.preparing=false;
            if(surface&&!slot.reserved&&!slot.producer.Active()&&n!=s.latest){
                slot.surface.swap(surface);slot.producer.fence.Swap(fence);slot.producer.value=slot.producer.completedValue=0;
                for(auto& reader:slot.readers){reader.fence.Reset();reader.value=0;}
            }else if(!surface)Mark("Independent DX12 preview preparation failed");}
        // Old idle resources/fences destruct outside the shared mutex.
        return true;
    }return false;
}
}
bool TryRecord(SampleToken& token,ID3D12GraphicsCommandList* list,const Inputs& in,const GuideDiagnostics::Snapshot& stamp) noexcept {
    using namespace PreviewDetail;bool recorded=false;
    try{auto& s=Get();std::unique_lock lock(s.mutex,std::try_to_lock);
        if(!lock.owns_lock()){++s.lockBusy;CancelSample(token);return false;}
        if(token.slot>=SlotCount){token={};return false;}auto& slot=s.slots[token.slot];
        if(!slot.reserved||slot.serial!=token.serial||token.revision!=s.revision||s.demand.frozen||s.demand.mode==PreviewMode::Off){
            Mark("Preview reservation superseded by a different demand or frozen view");lock.unlock();CancelSample(token);return false;}
        const auto demand=s.demand;lock.unlock();
        const auto fail=[&](const char* why){Mark(why);CancelSample(token);return false;};
        if(!list||!in.independentRecording||list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)return fail("Unavailable: independently owned DX12 DIRECT recording required");
        if(!stamp.source)return fail("Unavailable: actual source identity is unknown");
        if(in.colourHdr)return fail("Unavailable: HDR input transfer contract is not supported");
        unsigned mode=static_cast<unsigned>(demand.mode);if(mode==6)mode=3;if(mode==7)mode=4;
        if(mode==8&&demand.historyView){
            if(!in.historyDiagnostic||in.historyView!=token.historyView||in.historyView!=demand.historyView)
                return fail("Waiting for the exact reserved history inspector view");
            mode=9; // colourized by the exact history evaluation; don't reinterpret as contribution
        }else if(in.historyDiagnostic)return fail("Discarded inspector from a different demand revision");
        auto* resource=mode==3?in.depth:mode==4?in.motion:mode==8&&!in.compareFreshAnswer?in.contribution:in.colour;
        auto rect=mode==3?in.depthRect:mode==4?in.motionRect:mode==8&&!in.compareFreshAnswer?in.contributionRect:in.colourRect;
        auto state=mode==3?in.depthState:mode==4?in.motionState:mode==8&&!in.compareFreshAnswer?in.contributionState:in.colourState;
        auto* other=mode==8&&in.compareFreshAnswer?in.contribution:nullptr;auto otherRect=in.contributionRect;
        if(demand.raw)rect={};
        if(!Region(resource,rect))return fail("Unavailable: selected current input/valid rect missing");
        if(other&&!otherRect.width&&!otherRect.height)otherRect=in.colourRect;
        if(mode==8&&in.compareFreshAnswer&&(!other||other==resource||!Region(other,otherRect)))return fail("Unavailable: same-frame history comparison missing");
        if(mode==4&&(!std::isfinite(in.motionToUvX)||!std::isfinite(in.motionToUvY)))return fail("Unavailable: SDK motion scale unknown");
        ComPtr<ID3D12Device> device,owner;if(list->GetDevice(IID_PPV_ARGS(&device))!=S_OK||resource->GetDevice(IID_PPV_ARGS(&owner))!=S_OK||!D3D12DeviceIdentity::Same(device.Get(),owner.Get()))return fail("Unavailable: source/list device mismatch");
        const auto format=ViewFormat(resource->GetDesc().Format,mode==3?1:0);
        const auto otherFormat=other?ViewFormat(other->GetDesc().Format,0):DXGI_FORMAT_R32G32B32A32_FLOAT;
        if(format==DXGI_FORMAT_UNKNOWN||otherFormat==DXGI_FORMAT_UNKNOWN)return fail("Unavailable: unsupported input view format");
        if(other){owner.Reset();if(other->GetDevice(IID_PPV_ARGS(&owner))!=S_OK||!D3D12DeviceIdentity::Same(owner.Get(),device.Get()))return fail("Unavailable: comparison device mismatch");}
        if(!lock.try_lock()){++s.lockBusy;CancelSample(token);return false;}
        if(!slot.reserved||slot.serial!=token.serial||token.revision!=s.revision||s.demand.frozen||s.demand.mode!=demand.mode){lock.unlock();CancelSample(token);return false;}
        if(stamp.source==s.lastSource&&stamp.feature==s.lastFeature&&stamp.owner==s.lastOwner&&stamp.configuration==s.lastConfig){Mark("Waiting for a new source ordinal in this preview domain");lock.unlock();CancelSample(token);return false;}
        if(!slot.surface||slot.surface->device.Get()!=device.Get()||slot.surface->width!=rect.width||slot.surface->height!=rect.height){
            slot.requestedDevice=device;slot.requestedWidth=rect.width;slot.requestedHeight=rect.height;
            Mark("Preparing immutable preview storage on the producer device");
            lock.unlock();CancelSample(token);StartWorker(Storage());return false;}
        auto surface=slot.surface;slot.source=stamp.source;slot.tick=stamp.tick?stamp.tick:GuideDiagnostics::Now();
        slot.epoch=stamp.epoch;slot.feature=stamp.feature;slot.configuration=stamp.configuration;slot.owner=stamp.owner;
        slot.consumer=token.stream;slot.colourHdr=in.colourHdr;slot.completedTick=0;
        slot.startState=surface->state;
        if(!Register(slot.producer,list,device.Get(),token.slot*UsesPerSlot)){lock.unlock();return fail("Unavailable: preview producer registration failed");}
        surface->inputs={resource,other};lock.unlock();
        const auto stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);auto handle=surface->computeHeap->GetCPUDescriptorHandleForHeapStart();
        for(unsigned i=0;i<2;++i){D3D12_SHADER_RESOURCE_VIEW_DESC vd{};vd.Format=i?otherFormat:format;vd.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            vd.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;vd.Texture2D.MipLevels=1;device->CreateShaderResourceView(i?other:resource,&vd,handle);handle.ptr+=stride;}
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};ud.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;ud.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device->CreateUnorderedAccessView(surface->texture.Get(),nullptr,&ud,handle);
        Barrier(list,resource,state,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);Barrier(list,other,in.contributionState,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(list,surface->texture.Get(),surface->state,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        GuidePreviewShader::Constants c{rect.width,rect.height,mode,(demand.raw?1u:0u)|(demand.flipY?2u:0u)|(stamp.depthInverted?4u:0u)|(in.colourHdr?8u:0u)|(other?16u:0u),
            {rect.x,rect.y,rect.width,rect.height},{otherRect.x,otherRect.y,otherRect.width,otherRect.height},demand.raw?1.f:in.motionToUvX,demand.raw?1.f:in.motionToUvY,demand.gain,in.colourWhite};
        ID3D12DescriptorHeap* heaps[]{surface->computeHeap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(surface->computeRoot.Get());
        list->SetPipelineState(surface->compute.Get());list->SetComputeRoot32BitConstants(0,16,&c,0);list->SetComputeRootDescriptorTable(1,surface->computeHeap->GetGPUDescriptorHandleForHeapStart());
        list->Dispatch((rect.width+7)/8,(rect.height+7)/8,1);
        Barrier(list,surface->texture.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Barrier(list,other,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,in.contributionState);Barrier(list,resource,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,state);
        lock.lock();surface->state=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;s.lastSource=stamp.source;s.lastFeature=stamp.feature;s.lastOwner=stamp.owner;s.lastConfig=stamp.configuration;slot.reserved=false;
        token={};recorded=true;++s.recorded;Mark("GPU snapshot recorded; awaiting actual Execute, fence and Reset");
    }catch(...){if(token.slot<SlotCount){auto& s=Get();std::lock_guard lock(s.mutex);auto& slot=s.slots[token.slot];
        if(slot.producer.Active())slot.producer.quarantined=true;}CancelSample(token);}if(recorded)StartWorker(Storage());return recorded;
}
bool TryAcquireCompleted(PreviewSnapshot& out) noexcept {
    out={};try{auto& s=PreviewDetail::Get();std::unique_lock lock(s.mutex,std::try_to_lock);
        if(!lock.owns_lock()||s.demand.mode==PreviewMode::Off||s.latest==UINT_MAX)return false;const auto& slot=s.slots[s.latest];
        if(!slot.published||slot.revision!=s.revision||!slot.surface||slot.producer.quarantined)return false;
        for(auto& r:slot.readers)if(r.quarantined)return false;
        out.lease=slot.surface;out.texture=slot.surface->texture.Get();out.width=slot.surface->width;out.height=slot.surface->height;
        out.serial=slot.serial;out.revision=slot.revision;out.source=slot.source;out.tick=slot.tick;out.completedTick=slot.completedTick;
        out.consumer=slot.consumer;out.epoch=slot.epoch;out.feature=slot.feature;out.configuration=slot.configuration;out.owner=slot.owner;out.colourHdr=slot.colourHdr;++s.acquisitions;return true;
    }catch(...){return false;}
}
namespace PreviewDetail {
bool RetainOutputRead(const PreviewSnapshot& snapshot,ID3D12GraphicsCommandList* list,std::shared_ptr<Display>& display) noexcept {
    try{auto& s=PreviewDetail::Get();std::unique_lock lock(s.mutex,std::try_to_lock);if(!lock.owns_lock()||!snapshot.lease||!list||s.demand.mode==PreviewMode::Off||snapshot.revision!=s.revision)return false;
        if(list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)return false;ComPtr<ID3D12Device> device;if(list->GetDevice(IID_PPV_ARGS(&device))!=S_OK)return false;
        for(unsigned n=0;n<PreviewDetail::SlotCount;++n){auto& slot=s.slots[n];
            if(slot.serial!=snapshot.serial||slot.surface.get()!=snapshot.lease.get()||!slot.published||slot.producer.quarantined)continue;
            if(!D3D12DeviceIdentity::Same(slot.surface->device.Get(),device.Get())){PreviewOutputUnavailable("Unavailable: preview output device mismatch");return false;}
            for(auto& r:slot.readers)if(r.Active()&&r.list.Get()==static_cast<ID3D12CommandList*>(list)&&!r.reset){
                if(r.pre||r.quarantined)return false;display=r.display;return bool(display);}
            display=slot.surface->display;
            if(!display||display->device.Get()!=device.Get()){
                slot.surface->requestedDisplayDevice=device;PreviewOutputUnavailable("Preparing preview on the actual output device");
                lock.unlock();StartWorker(Storage());return false;}
            for(unsigned j=0;j<PreviewDetail::ReadersPerSlot;++j)if(!slot.readers[j].Active()){
                auto& reader=slot.readers[j];
                if(reader.fence.Get()!=display->fences[j].Get()){reader.fence=display->fences[j];reader.value=0;}
                if(!PreviewDetail::Register(reader,list,device.Get(),n*PreviewDetail::UsesPerSlot+j+1))return false;
                reader.display=display;return true;}
            PreviewOutputUnavailable("Preview output reader slots busy; skipped");
        }return false;
    }catch(...){return false;}
}
}
bool RetainRead(const PreviewSnapshot& snapshot,ID3D12GraphicsCommandList* list) noexcept {
    std::shared_ptr<PreviewDetail::Display> display;return PreviewDetail::RetainOutputRead(snapshot,list,display);
}
bool DrawPreview(const PreviewSnapshot& snapshot,ID3D12GraphicsCommandList* list,DXGI_FORMAT format,unsigned width,unsigned height) noexcept {
    try{if(!snapshot.lease||!list||!width||!height)return false;
        if(format!=DXGI_FORMAT_R8G8B8A8_UNORM&&format!=DXGI_FORMAT_B8G8R8A8_UNORM&&
           format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB&&format!=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB){
            PreviewOutputUnavailable("Unavailable: output HDR/format transfer contract is not supported");return false;}
        std::shared_ptr<PreviewDetail::Display> display;
        if(!PreviewDetail::RetainOutputRead(snapshot,list,display))return false;
        ID3D12PipelineState* pipeline=nullptr;
        // Immutable output-domain pipelines and descriptors were prepared on the worker.
        for(const auto& entry:display->pipelines)if(entry.first==format){pipeline=entry.second.Get();break;}
        if(!pipeline)return false;
        const float c[]{float(width),float(height),float(snapshot.width),float(snapshot.height)};ID3D12DescriptorHeap* heaps[]{display->heap.Get()};
        list->SetDescriptorHeaps(1,heaps);list->SetGraphicsRootSignature(display->root.Get());list->SetPipelineState(pipeline);
        list->SetGraphicsRoot32BitConstants(0,4,c,0);list->SetGraphicsRootDescriptorTable(1,display->heap->GetGPUDescriptorHandleForHeapStart());
        const D3D12_VIEWPORT vp{0,0,float(width),float(height),0,1};const D3D12_RECT rect{0,0,LONG(width),LONG(height)};
        list->RSSetViewports(1,&vp);list->RSSetScissorRects(1,&rect);list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);list->DrawInstanced(3,1,0,0);
        ++PreviewDetail::Get().draws;PreviewOutputUnavailable("Preview draw recorded; waiting for actual output submission");return true;
    }catch(...){return false;}
}
void NoteDisplayed(const PreviewSnapshot& snapshot) noexcept {
    try{auto& s=PreviewDetail::Get();std::lock_guard lock(s.mutex);
        if(snapshot.revision!=s.revision||s.demand.mode==PreviewMode::Off)return;
        s.displayed=snapshot;s.displayed.lease.reset();s.displayed.texture=nullptr;
        ++s.outputs;s.outputStatus.store("Preview output submitted; scanout not measured",std::memory_order_release);
    }catch(...){}
}
void ClearDisplayed(const PreviewSnapshot& snapshot) noexcept {
    try{auto& s=PreviewDetail::Get();std::lock_guard lock(s.mutex);
        const auto& displayed=s.displayed;
        if(!snapshot.serial||displayed.serial!=snapshot.serial||displayed.revision!=snapshot.revision||
           displayed.source!=snapshot.source||displayed.consumer!=snapshot.consumer||displayed.epoch!=snapshot.epoch||
           displayed.feature!=snapshot.feature||displayed.configuration!=snapshot.configuration||displayed.owner!=snapshot.owner)return;
        s.displayed={}; // Metadata only: producer/reader queue, fence and Reset gates are untouched.
    }catch(...){}
}
bool ReadDisplayed(PreviewSnapshot& out) noexcept {
    out={};try{auto& s=PreviewDetail::Get();std::unique_lock lock(s.mutex,std::try_to_lock);
        if(!lock.owns_lock()||!s.displayed.serial||s.displayed.revision!=s.revision)return false;
        out=s.displayed;return true;
    }catch(...){return false;}
}
void PreviewUnavailable(const char* reason) noexcept {PreviewDetail::Mark(reason?reason:"Preview unavailable");}
void PreviewOutputUnavailable(const char* reason) noexcept {
    PreviewDetail::Get().outputStatus.store(reason?reason:"Preview output unavailable",std::memory_order_release);
}
void ObservePreviewOutput(bool sealed,bool acquired,bool cached) noexcept {
    if(!PreviewActive())return;
    if(!sealed)PreviewOutputUnavailable("Waiting for authenticated physical output after SDK input seal");
    else if(!acquired&&!cached)PreviewOutputUnavailable("Physical output authenticated; no completed producer snapshot");
}
bool ReadPreviewProgress(PreviewProgress& out) noexcept {
    try{auto& s=PreviewDetail::Get();std::unique_lock lock(s.mutex,std::try_to_lock);if(!lock.owns_lock())return false;
        out={};out.mode=s.demand.mode;out.historyView=s.demand.historyView;out.revision=s.revision;
        out.attempts=s.attempts.load();out.reserved=s.reservations.load();out.recorded=s.recorded.load();
        out.submitted=s.submitted.load();out.gpuCompleted=s.completed.load();out.reset=s.resets.load();
        out.published=s.publications.load();out.acquired=s.acquisitions.load();out.drawn=s.draws.load();out.outputSubmitted=s.outputs.load();
        out.lockBusy=s.lockBusy.load();out.rateLimited=s.rateLimited.load();out.slotsBusy=s.slotsBusy.load();
        out.producerReason=s.status.load(std::memory_order_acquire);out.outputReason=s.outputStatus.load(std::memory_order_acquire);
        for(unsigned index=0;index<PreviewDetail::SlotCount;++index){const auto& slot=s.slots[index];const auto& use=slot.producer;auto& value=out.slots[index];
            value.serial=slot.serial;value.revision=slot.revision;value.source=slot.source;value.epoch=slot.epoch;
            value.feature=slot.feature;value.configuration=slot.configuration;value.owner=slot.owner;
            value.recording=use.generation;value.list=use.diagnosticListIdentity;value.queue=use.diagnosticQueueIdentity;
            value.fence=reinterpret_cast<uintptr_t>(use.fence.Get());
            value.fenceTarget=use.generation?use.value:0;value.fenceCompleted=use.completedValue;
            value.reserved=slot.reserved;value.recorded=use.generation!=0;value.executing=use.pre;value.submitted=use.post;
            value.signalled=use.signalled;value.gpuCompleted=use.post&&use.signalled&&use.done&&use.value&&
                use.completedValue>=use.value&&use.completedValue!=UINT64_MAX;
            value.reset=use.reset;value.published=slot.published;value.quarantined=use.quarantined;
            for(const auto& reader:slot.readers)if(reader.Active())++value.readers;
        }return true;
    }catch(...){return false;}
}
std::string PreviewStatus() noexcept {
    try{auto& s=PreviewDetail::Get();std::string text="Producer: ";text+=s.status.load(std::memory_order_acquire);
        text+=" | Output: ";text+=s.outputStatus.load(std::memory_order_acquire);return text;}catch(...){return {};}
}
} // namespace GuideCapture
