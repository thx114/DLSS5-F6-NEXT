// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Windows.h>
#include <runtime/F6InputPolicy.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>

// CPU observations only. No retained GPU/COM objects, waits, readbacks or
// per-frame file writes. Present success never certifies displayed pixels.
namespace GenshinPresentNrTrace {
inline uintptr_t Identity(IUnknown* p) noexcept {
    Microsoft::WRL::ComPtr<IUnknown> q;
    return p && SUCCEEDED(p->QueryInterface(IID_PPV_ARGS(&q))) ? reinterpret_cast<uintptr_t>(q.Get()):0;
}
inline uint64_t Sequence() noexcept {static std::atomic<uint64_t> n{0};return ++n;}
// OBSERVATION-ONLY. Scalar snapshots; raw interface addresses are not COM
// identity/content proof. Serials distinguish query reuse and retained events.
struct OuterQueryEvent {
    uint64_t seq=0,endSeq=0,pollSeq=0,readySeq=0,draw=0,instance=0,generation=0,sourceFrame=0;
    uint64_t ownerEpoch=0,deviceEpoch=0,configEpoch=0;
    uintptr_t query=0,context=0,device=0,sourceTexture=0,destinationTexture=0,destinationResource=0;
    uint32_t thread=0,kind=0,subresource=0; // kind: 1 native submit, 2 paired repair
    int32_t hr=0; // meaningful only when polled
    bool active=false,polled=false,done=false,copyOrdered=false,transportPending=false;
};
struct RepairEvent {
    OuterQueryEvent query{};
    uint64_t seq=0,draw=0,attemptSeq=0,copySeq=0,gateQuerySeq=0,gatePollSeq=0;
    uint64_t instance=0,generation=0,canonicalGeneration=0,cpuGeneration=0;
    uintptr_t sourceTexture=0,destinationTexture=0,destinationResource=0,selectedIdentity=0;
    uint32_t subresource=0,stage=0,pollStop=0,matches=0,refusal=0,bindingsRefusal=0;
    bool attempted=false,submitted=false,retargeted=false;
};
struct PendingRepairTrace {
    OuterQueryEvent outer{};
    RepairEvent target{},blocked{},sceneTarget{};
    uint64_t sceneSeq=0,sceneDraw=0,sceneGeneration=0;
    uintptr_t sceneInputTexture=0,sceneOutputTexture=0,sceneSelectedIdentity=0;
};
static_assert(sizeof(PendingRepairTrace)<=1280); // <=5 MiB added to the fixed 4096-record store
inline void WritePendingRepair(std::ostream& f,const PendingRepairTrace& p) {
    const auto query=[&](const OuterQueryEvent& r) {
        f<<"{";
#define R407_FIELD(name) f<<",\"" #name "\":"<<r.name
        f<<"\"seq\":"<<r.seq;
        R407_FIELD(endSeq);R407_FIELD(pollSeq);R407_FIELD(readySeq);R407_FIELD(draw);
        R407_FIELD(instance);R407_FIELD(generation);R407_FIELD(sourceFrame);
        R407_FIELD(ownerEpoch);R407_FIELD(deviceEpoch);R407_FIELD(configEpoch);
        R407_FIELD(query);R407_FIELD(context);R407_FIELD(device);R407_FIELD(thread);R407_FIELD(kind);
        R407_FIELD(sourceTexture);R407_FIELD(destinationTexture);R407_FIELD(destinationResource);R407_FIELD(subresource);
        R407_FIELD(hr);R407_FIELD(active);R407_FIELD(polled);R407_FIELD(done);R407_FIELD(copyOrdered);R407_FIELD(transportPending);
#undef R407_FIELD
        f<<"}";
    };
    const auto target=[&](const RepairEvent& r) {
        f<<"{\"seq\":"<<r.seq;
#define R407_FIELD(name) f<<",\"" #name "\":"<<r.name
        R407_FIELD(draw);R407_FIELD(attemptSeq);R407_FIELD(copySeq);R407_FIELD(gateQuerySeq);R407_FIELD(gatePollSeq);
        R407_FIELD(instance);R407_FIELD(generation);R407_FIELD(canonicalGeneration);R407_FIELD(cpuGeneration);
        R407_FIELD(sourceTexture);R407_FIELD(destinationTexture);R407_FIELD(destinationResource);R407_FIELD(selectedIdentity);
        R407_FIELD(subresource);R407_FIELD(stage);R407_FIELD(pollStop);R407_FIELD(matches);R407_FIELD(refusal);R407_FIELD(bindingsRefusal);
        R407_FIELD(attempted);R407_FIELD(submitted);R407_FIELD(retargeted);
#undef R407_FIELD
        f<<",\"query\":";query(r.query);f<<"}";
    };
    f<<",\"pendingRepair\":{\"version\":1,\"outer\":";query(p.outer);
    f<<",\"target\":";target(p.target);f<<",\"blocked\":";target(p.blocked);
    f<<",\"sceneTarget\":";target(p.sceneTarget);
    f<<",\"sceneSeq\":"<<p.sceneSeq<<",\"sceneDraw\":"<<p.sceneDraw<<",\"sceneGeneration\":"<<p.sceneGeneration
     <<",\"sceneInputTexture\":"<<p.sceneInputTexture<<",\"sceneOutputTexture\":"<<p.sceneOutputTexture
     <<",\"sceneSelectedIdentity\":"<<p.sceneSelectedIdentity<<"}";
}
struct PictureCandidate {
    uint64_t seq=0,draw=0,shader=0,sceneSeen=0,pictureSeen=0;
    uintptr_t input=0,output=0;
    uint32_t kind=0,count=0,reason=0; // 1 shape, 2 source, 3 descriptors, 4 cadence evaluated, 5 selected content
    bool countKnown=false,scene=false;
};
struct Record {
    __declspec(noinline) Record() noexcept {}
    PendingRepairTrace pendingRepair{};
    PictureCandidate pictureCandidate{},sceneCandidate{};
    uint64_t submitSerial=0,sourceCopySerial=0,gpuReadySerial=0;
    uint64_t selectedNativeDraw=0,selectedContentCopy=0;
    bool selectedContent=false;
    unsigned pictureProbeSamples=0,pictureProbeRemaining=0;
    uint64_t id=0,parent=0,begin=0,end=0,tick=0,epoch=0,sourceSeq=0,ticketSeq=0,historySeq=0,consumerSeq=0,modelSeq=0,copySeq=0,presentSeq=0;
    uintptr_t sc=0,rawSc=0,context=0,device=0,selected=0,privateOutput=0,currentPicture=0,intermediate=0,observedPicture=0,historyOutput=0,queue=0,list=0,completionFence=0,copyFence=0;
    uint64_t generation=0,currentGeneration=0,sourceFrame=0,ownerEpoch=0,deviceEpoch=0,configEpoch=0,historyGeneration=0,sourceAge=UINT64_MAX;
    uint64_t sceneDraw=0,pictureDraw=0,pictureSerial=0,consumedDraw=0,previousPresent=0,previousPicture=0,completionValue=0,copyValue=0,modelCalls=0;
    uint64_t postCopyWrites=0,firstPostCopyDraw=0,firstPostCopyShader=0,firstPostCopySeq=0;
    uint64_t disjointScenes=0,disjointScenesSinceTrace=0;
    uint64_t lastSceneDraw=0,lastPictureDraw=0,pairedCopies=0,pairedDraw=0;
    uint64_t repairTargetDraw=0,repairPollBlocked=0,repairCanonical=0,repairCpuGeneration=0;
    uint32_t repairStage=0,repairMask=0,repairTokenMatches=0,repairPollStop=0;
    uint32_t picturePath=0; // 0 no ticket, 1 Scene -> ToPicture -> B, 2 Scene -> B
    uintptr_t lastSceneInput=0,lastSceneOutput=0,lastPictureInput=0,lastPictureOutput=0,pairedOutput=0;
    const char* lastSceneReason="not-observed";
    const char* lastPictureReason="not-observed";
    uint32_t thread=0,index=UINT32_MAX,flags=0,sourceFailure=0,resetReasons=0,acquireStatus=UINT32_MAX;
    int32_t result=0;
    bool nested=false,presentKnown=false,ticket=false,historyFailed=false,historyFound=false,consumer=false,composed=false,submitted=false,copy=false;
    bool historyReset=false,flowReset=false,nativeReset=false,lateReset=false,dispatchReset=false,modelObserved=false;
    bool inputWait=false,returnWait=false,copySignal=false,sourceReady=false;
    const char* reject="not-reached";
    const char* producerStop="none";
};
struct Store {
    static constexpr size_t Capacity=4096;
    std::mutex mutex;
    std::array<Record,Capacity> ring{};
    size_t count=0,next=0;
    uint64_t overwritten=0,started=0,attemptStarted=0,trigger=0,firstId=0,lastId=0;
    const char* triggerReason="none";
    const char* freezeReason="none";
    bool frozen=false;
    std::filesystem::path path;
    std::atomic<int> writer{0}; // 0 idle, 1 writing, 2 saved, -1 failed
    HMODULE writerModule=nullptr;
};
inline Store& Data(){static auto* s=new Store;return *s;}
inline bool IsGenshinProcess() noexcept {
    if constexpr (!F6InputPolicy::FsrBridgeEnabled) return false;
    static const bool match=[] {wchar_t b[MAX_PATH]{};if(!GetModuleFileNameW(nullptr,b,MAX_PATH))return false;
        const wchar_t* p=wcsrchr(b,L'\\');p=p?p+1:b;
        return _wcsicmp(p,L"YuanShen.exe")==0 || _wcsicmp(p,L"GenshinImpact.exe")==0;}();return match;
}
inline void Save(Store* s) noexcept {
    try {
        std::ofstream f(s->path,std::ios::binary|std::ios::trunc);
        if(!f){s->writer=-1;return;}
        const auto start=(s->next+Store::Capacity-s->count)%Store::Capacity;
        f<<"{\"type\":\"metadata\",\"schema\":1,\"pid\":"<<GetCurrentProcessId()<<",\"capacity\":"<<Store::Capacity
         <<",\"records\":"<<s->count<<",\"overwritten\":"<<s->overwritten<<",\"firstId\":"<<s->firstId<<",\"lastId\":"<<s->lastId
         <<",\"trigger\":\""<<s->triggerReason<<"\",\"freezeReason\":\""<<s->freezeReason<<"\",\"startedTick\":"<<s->started<<",\"attemptStartedTick\":"<<s->attemptStarted<<",\"frozenTick\":"<<GetTickCount64()
         <<",\"modelCallsMeaning\":\"provider-evaluate-call-attempts-including-failures\",\"copyMeaning\":\"D3D11-copy-command-ordered-not-GPU-completed\""
         <<",\"coverage\":\"LocalPresent-TLS-scope-hooked-returned-draws-only;outside-scope-next-frame-other-thread-clear-copy-deferred-unhooked-writes-UNKNOWN;GPU-completion-not-polled;final-visible-UNKNOWN;disjoint-delta-since-same-cadence-last-trace\"}\n";
        for(size_t i=0;i<s->count;++i){const auto& r=s->ring[(start+i)%Store::Capacity];
            f<<"{\"type\":\"present\"";
#define R294_FIELD(name) f<<",\"" #name "\":"<<r.name
            R294_FIELD(id);R294_FIELD(parent);R294_FIELD(begin);R294_FIELD(end);R294_FIELD(tick);R294_FIELD(epoch);R294_FIELD(thread);R294_FIELD(nested);
            R294_FIELD(sc);R294_FIELD(rawSc);R294_FIELD(context);R294_FIELD(device);R294_FIELD(index);R294_FIELD(flags);
            R294_FIELD(sourceSeq);R294_FIELD(ticketSeq);R294_FIELD(historySeq);R294_FIELD(consumerSeq);R294_FIELD(modelSeq);R294_FIELD(copySeq);R294_FIELD(presentSeq);
            R294_FIELD(selected);R294_FIELD(privateOutput);R294_FIELD(currentPicture);R294_FIELD(historyOutput);
            R294_FIELD(intermediate);R294_FIELD(observedPicture);R294_FIELD(acquireStatus);
            R294_FIELD(generation);R294_FIELD(currentGeneration);R294_FIELD(sourceFrame);R294_FIELD(ownerEpoch);R294_FIELD(deviceEpoch);R294_FIELD(configEpoch);
            R294_FIELD(historyGeneration);R294_FIELD(sourceAge);R294_FIELD(historyFound);R294_FIELD(historyFailed);R294_FIELD(sourceFailure);
            R294_FIELD(sceneDraw);R294_FIELD(pictureDraw);R294_FIELD(pictureSerial);R294_FIELD(consumedDraw);R294_FIELD(previousPresent);R294_FIELD(previousPicture);
            R294_FIELD(picturePath);R294_FIELD(ticket);R294_FIELD(resetReasons);R294_FIELD(historyReset);R294_FIELD(flowReset);R294_FIELD(nativeReset);R294_FIELD(lateReset);R294_FIELD(dispatchReset);
            R294_FIELD(consumer);R294_FIELD(modelObserved);R294_FIELD(modelCalls);R294_FIELD(composed);R294_FIELD(submitted);R294_FIELD(copy);
            R294_FIELD(queue);R294_FIELD(list);R294_FIELD(completionFence);R294_FIELD(completionValue);R294_FIELD(copyFence);R294_FIELD(copyValue);
            R294_FIELD(sourceReady);R294_FIELD(inputWait);R294_FIELD(returnWait);R294_FIELD(copySignal);
            R294_FIELD(disjointScenes);R294_FIELD(disjointScenesSinceTrace);
            R294_FIELD(lastSceneDraw);R294_FIELD(lastSceneInput);R294_FIELD(lastSceneOutput);
            R294_FIELD(lastPictureDraw);R294_FIELD(lastPictureInput);R294_FIELD(lastPictureOutput);
            R294_FIELD(pairedCopies);R294_FIELD(pairedDraw);R294_FIELD(pairedOutput);
            R294_FIELD(submitSerial);R294_FIELD(sourceCopySerial);R294_FIELD(gpuReadySerial);
            R294_FIELD(selectedContent);R294_FIELD(selectedNativeDraw);R294_FIELD(selectedContentCopy);
            R294_FIELD(pictureProbeSamples);R294_FIELD(pictureProbeRemaining);
            for(unsigned candidateIndex=0;candidateIndex<2;++candidateIndex){
            const auto& c=candidateIndex?r.sceneCandidate:r.pictureCandidate;
            f<<",\""<<(candidateIndex?"sceneCandidate":"pictureCandidate")<<"\":{\"seq\":"<<c.seq<<",\"draw\":"<<c.draw<<",\"shader\":"<<c.shader
             <<",\"sceneSeen\":"<<c.sceneSeen<<",\"pictureSeen\":"<<c.pictureSeen<<",\"kind\":"<<c.kind
             <<",\"count\":"<<c.count<<",\"countKnown\":"<<c.countKnown<<",\"scene\":"<<c.scene
             <<",\"reason\":"<<c.reason<<",\"input\":"<<c.input<<",\"output\":"<<c.output<<"}";
            }
            R294_FIELD(repairTargetDraw);R294_FIELD(repairPollBlocked);R294_FIELD(repairCanonical);R294_FIELD(repairCpuGeneration);
            R294_FIELD(repairStage);R294_FIELD(repairMask);R294_FIELD(repairTokenMatches);R294_FIELD(repairPollStop);
            WritePendingRepair(f,r.pendingRepair);
            f<<",\"lastSceneReason\":\""<<r.lastSceneReason<<"\",\"lastPictureReason\":\""<<r.lastPictureReason<<"\"";
            R294_FIELD(postCopyWrites);R294_FIELD(firstPostCopyDraw);R294_FIELD(firstPostCopyShader);R294_FIELD(firstPostCopySeq);
            R294_FIELD(presentKnown);R294_FIELD(result);
#undef R294_FIELD
            f<<",\"reject\":\""<<r.reject<<"\",\"producerStop\":\""<<r.producerStop<<"\",\"finalVisible\":\"UNKNOWN\"}\n";
        }
        f.close();s->writer=f?2:-1;
    } catch(...){s->writer=-1;}
}
inline DWORD WINAPI Writer(void* value) noexcept {
    auto* s=static_cast<Store*>(value);const auto module=s->writerModule;
    Save(s);
    // The module reference owns all worker code until this atomic release/exit.
    // No detach/join, DllMain wait, COM/GPU ownership or static destruction.
    FreeLibraryAndExitThread(module,0);
}
inline void Commit(const Record& r) noexcept {
    try {
        auto& s=Data();bool freeze=false;
        {std::lock_guard lock(s.mutex);if(s.frozen)return;
            s.ring[s.next]=r;s.next=(s.next+1)%Store::Capacity;
            if(s.count<Store::Capacity)++s.count;else ++s.overwritten;
            s.firstId=s.ring[(s.next+Store::Capacity-s.count)%Store::Capacity].id;s.lastId=r.id;
            const auto now=GetTickCount64();
            if(!s.attemptStarted && r.sourceSeq && r.generation)s.attemptStarted=now;
            if(!s.started && r.composed)s.started=now;
            if(s.started && !s.trigger && ((r.presentKnown && r.result==S_OK && !r.copy) || r.postCopyWrites)) {
                s.trigger=now;s.triggerReason=r.postCopyWrites?"post-copy-returned-draw":"successful-present-without-NR-copy";
            }
            // Failed admission/init must still produce evidence. First compose
            // starts its own 30s world window, with a 60s attempt hard bound.
            freeze=(s.started && (now-s.started>=30000 || (s.trigger && now-s.trigger>=10000))) ||
                (s.attemptStarted && now-s.attemptStarted>=60000);
            if(freeze){s.freezeReason=s.started && now-s.started>=30000?"compose-window":
                s.started && s.trigger && now-s.trigger>=10000?"trigger-tail":"attempt-timeout";s.frozen=true;s.writer=1;}
        }
        if(freeze) {
            if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(&Writer),&s.writerModule)){s.writer=-1;return;}
            const auto thread=CreateThread(nullptr,0,&Writer,&s,0,nullptr);
            if(thread)CloseHandle(thread);else {FreeLibrary(s.writerModule);s.writerModule=nullptr;s.writer=-1;}
        }
    } catch(...){}
}
class Scope {
    inline static thread_local Scope* current_=nullptr;
    Scope* previous_=nullptr;
    bool enabled_=false;
public:
    Record record;
    Scope(IDXGISwapChain* sc,uint32_t flags,uint64_t epoch,const std::filesystem::path& log,bool forceForTest=false) noexcept {
        // Push even a disabled nested call, so its stages cannot mutate an
        // outer record (TEST, another game/route, or a frozen recorder).
        previous_=current_;current_=this;
        if((flags&DXGI_PRESENT_TEST) || (!forceForTest && !IsGenshinProcess()))return;
        try {
            auto& s=Data();{std::lock_guard lock(s.mutex);if(s.frozen)return;
                if(s.path.empty())s.path=log.parent_path()/(L".nr-present-trace-"+std::to_wstring(GetCurrentProcessId())+L".jsonl");}
            record.id=Sequence();record.parent=previous_?previous_->record.id:0;record.nested=previous_!=nullptr;
            record.begin=Sequence();record.tick=GetTickCount64();record.thread=GetCurrentThreadId();record.epoch=epoch;
            record.sc=Identity(sc);record.rawSc=reinterpret_cast<uintptr_t>(sc);record.flags=flags;
            Microsoft::WRL::ComPtr<IDXGISwapChain3> indexed;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> buffer;
            if(sc && SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&indexed)))) {
                record.index=indexed->GetCurrentBackBufferIndex();
                if(SUCCEEDED(sc->GetBuffer(record.index,IID_PPV_ARGS(&buffer))))record.currentPicture=Identity(buffer.Get());
            }
            enabled_=true;
        }catch(...){}
    }
    ~Scope(){current_=previous_;if(enabled_){record.end=Sequence();Commit(record);}}
    Scope(const Scope&)=delete;
    static Record* Current() noexcept {return current_ && current_->enabled_?&current_->record:nullptr;}
    void Presented(HRESULT result) noexcept {if(enabled_){record.presentKnown=true;record.result=result;record.presentSeq=Sequence();}}
};
inline Record* Current() noexcept {return Scope::Current();}
inline void PostCopyDraw(ID3D11DeviceContext* context,uint64_t draw,uint64_t shader) noexcept {
    auto* r=Current();if(!r || !r->copy || !r->currentPicture)return;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> views[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11RenderTargetView* raw[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,raw,nullptr);
    bool hit=false;for(unsigned i=0;i<D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;++i){views[i].Attach(raw[i]);
        if(views[i]){Microsoft::WRL::ComPtr<ID3D11Resource> resource;views[i]->GetResource(&resource);hit|=Identity(resource.Get())==r->currentPicture;}}
    if(hit){++r->postCopyWrites;if(!r->firstPostCopySeq){r->firstPostCopySeq=Sequence();r->firstPostCopyDraw=draw;r->firstPostCopyShader=shader;}}
}
}
