// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinFsrProfiles.h"
#include "GenshinFamilySkip.h"
#include "GenshinManualCapture.h"
#include "GenshinResourceProvenance.h"
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <bcrypt.h>
#include <json.hpp>
#include <array>
#include <algorithm>
#include <atomic>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#pragma comment(lib,"bcrypt.lib")
#pragma comment(lib,"d3dcompiler.lib")

namespace GenshinLegacy::RuntimeCapture {
using Microsoft::WRL::ComPtr;
using Json=nlohmann::ordered_json;
inline constexpr size_t MaxShaderBytes=1024*1024,MaxQueuedBytes=8*1024*1024,MaxDiskBytes=32*1024*1024;
inline constexpr unsigned MaxJobs=16,MaxShaders=32,MaxCbSamples=128,MaxCbBytes=4096;
inline constexpr size_t MaxModuleHashBytes=32*1024*1024;
inline constexpr uint64_t MaxWindowMs=60000;
inline constexpr uint64_t MaxArmedMs=10*60*1000;
inline constexpr unsigned MaxRouteSamples=256;
inline constexpr unsigned MaxPassSamples=128,MaxTraceDraws=4096,MaxTraceNodes=16;
inline constexpr size_t MaxArtifactMetadataBytes=1024*1024;
inline constexpr GUID CreateKey={0xd9f2cf62,0x754c,0x4de9,{0xb4,0xaf,0x91,0x23,0x18,0x7c,0x44,0x65}};
struct CreateId { uint64_t sequence=0,hash=0; bool queued=false; uint64_t sessionTick=0; };
struct DrawFact {
    uint64_t device=0,context=0,deviceEpoch=0,ownerEpoch=0,configEpoch=0,thread=0;
    uint64_t drawSerial=0,instance=0,generation=0,frame=0,captureTick=0,shaderIdentity=0,shaderHash=0,shaderObject=0,cbIdentity=0;
    uint32_t renderWidth=0,renderHeight=0,outputWidth=0,outputHeight=0,cbBytes=0,firstConstant=0,numConstants=0;
    uint64_t observationTick=0;
    float jitterX=0,jitterY=0;
    std::array<uint64_t,3> resources{},views{};
    std::array<uint32_t,3> slots{},formats{},widths{},heights{};
    std::array<uint64_t,D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> boundResources{},boundViews{};
    std::array<uint32_t,D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> boundFormats{},boundWidths{},boundHeights{};
    std::array<ResourceProof::Witness,D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> boundWrites{};
    std::array<D3D11_TEXTURE2D_DESC,D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> boundDescriptions{};
    std::array<D3D11_SHADER_RESOURCE_VIEW_DESC,D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> boundViewDescriptions{};
    std::array<uint64_t,D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> outputResources{},outputViews{};
    std::array<uint32_t,D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> outputFormats{},outputWidths{},outputHeights{};
    std::array<ResourceProof::Witness,D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> outputWrites{};
    std::array<D3D11_TEXTURE2D_DESC,D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> outputDescriptions{};
    std::array<D3D11_RENDER_TARGET_VIEW_DESC,D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> outputViewDescriptions{};
    struct Edge {uint64_t parentDraw=0,rootDraw=0,resourceLifetime=0;uint32_t readSlot=0,depth=0;};
    std::array<Edge,MaxTraceNodes> readEdges{};
    uint32_t readEdgeCount=0,drawCount=0;
    uint32_t captureTokenMatches=0,viewportCount=0;
    uint32_t selectedOutputSlot=0;
    std::array<ResourceProof::Witness,3> sourceWrites{};
    ResourceProof::Witness outputWitness{};
    D3D11_VIEWPORT viewport{};
    bool constantRangeKnown=false,invalidated=false,gpuReadbackComplete=false;
    bool familyPass=false,consumerRoute=false,drawCountKnown=false,readEdgesTruncated=false;
    CreateId creation{};
};
// CPU-only observations. No event grants admission or consumption.
struct RouteFact {
    enum class Kind { OriginalDrawReturned,PresentCompleted } kind=Kind::OriginalDrawReturned;
    uint64_t device=0,context=0,deviceEpoch=0,ownerEpoch=0,configEpoch=0,thread=0;
    uint64_t drawSerial=0,instance=0,generation=0,frame=0,shaderIdentity=0,shaderHash=0;
    uint64_t swapchain=0,presentSerial=0;
    uint64_t traceRootDraw=0;uint32_t traceDepth=0;
    int32_t presentResult=static_cast<int32_t>(E_PENDING);
    bool originalDrawReturned=false,presentRecorded=false,successfulNonTest=false,sameRender=false,directIdentityMatch=false;
    ResourceProof::Witness output{},backbuffer{};
};
struct Job {
    enum class Kind { Shader,Cb,Route,Binding,Artifact } kind=Kind::Shader;
    uint64_t sequence=0,deviceEpoch=0,shaderIdentity=0,shaderObject=0,fnv=0;
    uint64_t eventTick=0,readbackTick=0;
    std::vector<uint8_t> shader;
    DrawFact draw{};
    RouteFact route{};
    std::array<uint8_t,MaxCbBytes> cb{};
    Json metadata;size_t metadataBytes=0;
    std::vector<uint8_t> raw;std::wstring artifactName;
    size_t Payload()const noexcept{return kind==Kind::Shader?shader.size():(kind==Kind::Cb?draw.cbBytes:
        kind==Kind::Artifact?metadataBytes+raw.size():0);}
};
inline std::wstring ModulePath(HMODULE module) {
    std::wstring path(32768,L'\0');const auto n=GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size()));
    if(!n || n>=path.size())return {};path.resize(n);return path;
}
inline std::string Hex(const void* input,size_t n) {
    static constexpr char digits[]="0123456789abcdef";const auto* p=static_cast<const uint8_t*>(input);
    std::string value(n*2,'0');for(size_t i=0;i<n;++i){value[i*2]=digits[p[i]>>4];value[i*2+1]=digits[p[i]&15];}return value;
}
inline std::string Utf8(const std::wstring& wide) {
    if(wide.empty())return {};auto n=WideCharToMultiByte(CP_UTF8,0,wide.data(),static_cast<int>(wide.size()),nullptr,0,nullptr,nullptr);
    std::string s(n,'\0');WideCharToMultiByte(CP_UTF8,0,wide.data(),static_cast<int>(wide.size()),s.data(),n,nullptr,nullptr);return s;
}
class Sha256 final {
    BCRYPT_ALG_HANDLE algorithm_=nullptr;BCRYPT_HASH_HANDLE hash_=nullptr;std::vector<uint8_t> object_;
public:
    Sha256(){DWORD bytes=0,written=0;if(BCryptOpenAlgorithmProvider(&algorithm_,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return;
        if(BCryptGetProperty(algorithm_,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&bytes),sizeof(bytes),&written,0)<0)return;
        object_.resize(bytes);if(BCryptCreateHash(algorithm_,&hash_,object_.data(),bytes,nullptr,0,0)<0)hash_=nullptr;}
    ~Sha256(){if(hash_)BCryptDestroyHash(hash_);if(algorithm_)BCryptCloseAlgorithmProvider(algorithm_,0);}
    bool Add(const void* p,size_t n){return hash_ && n<=0xffffffffUL && BCryptHashData(hash_,static_cast<PUCHAR>(const_cast<void*>(p)),static_cast<ULONG>(n),0)>=0;}
    std::string Finish(){std::array<uint8_t,32> digest{};return hash_ && BCryptFinishHash(hash_,digest.data(),static_cast<ULONG>(digest.size()),0)>=0?Hex(digest.data(),digest.size()):"UNKNOWN";}
};
inline std::string HashBytes(const void* p,size_t n){Sha256 hash;if(!hash.Add(p,n))return "UNKNOWN";return hash.Finish();}
#ifdef GENSHIN_RUNTIME_CAPTURE_TEST
inline void (*TestAfterFileIdentityReservation)(const std::wstring&)=nullptr;
#endif
inline Json FileIdentity(const std::wstring& path,size_t& remaining) {
    Json result={{"path",Utf8(path)},{"sha256","UNKNOWN"},{"authority","actual loaded module path; file read on observer worker"}};
    if(path.empty()){result["status"]="PATH_UNAVAILABLE";return result;}
    std::ifstream file(std::filesystem::path(path),std::ios::binary|std::ios::ate);
    if(!file){result["status"]="OPEN_FAILED";return result;}
    const auto size=file.tellg();if(size<0 || static_cast<uint64_t>(size)>remaining){result["status"]="SESSION_HASH_BUDGET";return result;}
    remaining-=static_cast<size_t>(size); // Reserve before IO, including failed reads.
    const auto reserved=static_cast<size_t>(size);size_t readBytes=0;
    result["reservedBytes"]=reserved;result["bytes"]=readBytes;file.seekg(0);Sha256 hash;std::array<char,64*1024> chunk{};
#ifdef GENSHIN_RUNTIME_CAPTURE_TEST
    if(TestAfterFileIdentityReservation)TestAfterFileIdentityReservation(path);
#endif
    while(readBytes<reserved){
        const auto request=(std::min)(chunk.size(),reserved-readBytes);
        file.read(chunk.data(),static_cast<std::streamsize>(request));const auto actual=static_cast<size_t>(file.gcount());
        readBytes+=actual;result["bytes"]=readBytes;
        if(actual!=request){result["status"]="READ_FAILED";return result;}
        if(!hash.Add(chunk.data(),actual)){result["status"]="HASH_FAILED";return result;}
    }
    // Recheck the opened stream length without reading beyond the reserved byte budget.
    file.clear();file.seekg(0,std::ios::end);
    if(!file || file.tellg()!=size){result["status"]="FILE_CHANGED_DURING_READ";return result;}
    result["sha256"]=hash.Finish();result["status"]=result["sha256"]=="UNKNOWN"?"HASH_FAILED":"FILE_HASHED_NOT_MEMORY_ATTESTED";return result;
}
inline std::string TimeText(const SYSTEMTIME& time) {
    char buffer[64]{};snprintf(buffer,sizeof(buffer),"%04u-%02u-%02uT%02u:%02u:%02u.%03u",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,time.wMilliseconds);return buffer;
}
inline bool Expected(uint64_t hash) noexcept {
    return hash==fsr2_family_takeover::k_pre_hash_1 || hash==fsr2_family_takeover::k_pre_hash_2 ||
        hash==fsr2_family_takeover::k_pre_hash_3 || hash==fsr2_family_takeover::k_pre_hash_4 ||
        GenshinFsrProfiles::Accumulate(hash) || hash==0xF41E6080D4BEA352ull;
}
struct State {
    std::mutex mutex;
    std::deque<Job> queue;
    size_t queuedBytes=0;std::atomic_size_t diskBytes{0};
    std::atomic_bool active{false},stop{false};
    bool attempted=false,manual=false;
    std::atomic_bool workerFinished{true},workerReady{false};
    std::atomic<ManualCapture::Reason> finishReason{ManualCapture::Reason::None};
    std::atomic_uint64_t shaderArtifactsWritten{0},constantArtifactsWritten{0},bindingRecordsWritten{0},recordsWritten{0};
    std::atomic_uint64_t rawCreateSequence{0},rawCacheObjects{0},rawCacheBytes{0},rawCacheDrops{0};
    std::atomic_uint64_t inputOffers{0},inputDraws{0},inputDrops{0},inputBytesReserved{0};
    std::atomic_uint64_t gpuPending{0},gpuCompleted{0},gpuDropped{0},gpuQuarantined{0},gpuSlow{0};
    std::atomic_uint64_t artifactFilesWritten{0};
    HANDLE wake=nullptr;
    HMODULE module=nullptr;
    DWORD pid=0;SYSTEMTIME utc{},local{};FILETIME processCreation{};
    std::wstring directory,exe,host,unity,assembly,userAssembly;
    uint64_t startTick=0,configEpoch=0;std::string sessionId;
    std::atomic_uint64_t firstFamilyDrawTick{0};
    DXGI_ADAPTER_DESC adapter{};bool adapterKnown=false;
    std::atomic_uint64_t createSequence{0},knownCreates{0},oversize{0},queueDrops{0},lockDrops{0},allocationFailures{0},cbOffers{0},cbMissingCreate{0};
    std::atomic_uint64_t cbBusy{0},cbAllocationFailures{0},cbQueryFailures{0},cbMapRetries{0},wrongThread{0},shaderBudgetDrops{0},cbBudgetDrops{0},writeFailures{0},diskBudgetDrops{0};
    std::atomic_uint64_t workerDiscardJobs{0},workerDiscardBytes{0};
    std::atomic_uint64_t windowExpired{0},routeBudgetDrops{0},routeOffers{0};
    std::atomic_uint64_t passOffers{0},passBudgetDrops{0},passTokenMissing{0},passTokenAmbiguous{0},traceBudgetDrops{0};
    size_t shaderBudgetBytes=0;unsigned shaderBudgetCount=0,cbBudgetCount=0,routeBudgetCount=0,passBudgetCount=0;
};
// Published only under nativeMutex after the previous worker AND GPU sampler
// drained. Old CPU states remain alive for lock-free pre-boundary readers.
// ManualCapture caps this process at eight generations (plus the initial state).
inline std::atomic<State*>& StateSlot(){static std::atomic<State*> state{new State};return state;}
inline State& Get(){return *StateSlot().load(std::memory_order_acquire);}
inline bool ResetForManual() {
    auto* previous=StateSlot().load(std::memory_order_acquire);
    if(previous->active.load() || !previous->workerFinished.load(std::memory_order_acquire) || previous->gpuPending.load())return false;
    auto* next=new State;next->manual=true;StateSlot().store(next,std::memory_order_release);return true;
}
inline bool ProcessObservationRequested()noexcept {
    // This process-only opt-in affects only the Genshin native controls snapshot.
    // It is never saved through Config and cannot enable semantic admission.
    static const bool requested=[]()noexcept{wchar_t mode[3]{},capture[3]{};
        return GetEnvironmentVariableW(L"GENSHIN_FSR_OBSERVATION_ONLY",mode,3)==1 && mode[0]==L'1' &&
            GetEnvironmentVariableW(L"GENSHIN_FSR_RUNTIME_CAPTURE",capture,3)==1 && capture[0]==L'1';}();
    return requested;
}
inline bool InWindow(const State& s,uint64_t now)noexcept {
    if(now<s.startTick || now-s.startTick>=(s.manual?MaxWindowMs:MaxArmedMs))return false;
    const auto first=s.firstFamilyDrawTick.load(std::memory_order_acquire);
    return !first || (now>=first && now-first<MaxWindowMs);
}
inline bool Enabled()noexcept {
    const auto& s=Get();return s.active.load(std::memory_order_acquire) && InWindow(s,GetTickCount64());
}
inline void BeginFamilyWindow()noexcept {
    auto& s=Get();if(!Enabled())return;uint64_t expected=0;
    s.firstFamilyDrawTick.compare_exchange_strong(expected,GetTickCount64(),std::memory_order_acq_rel);
}
inline void CloseExpiredWindow(State& s)noexcept {
    const bool expired=!InWindow(s,GetTickCount64());
    const bool disk=s.manual && s.diskBudgetDrops.load()!=0;
    const bool count=s.manual && (s.passOffers.load()>=MaxPassSamples || s.cbBudgetDrops.load()!=0);
    const bool failed=s.manual && s.writeFailures.load()!=0;
    if(s.active.load(std::memory_order_acquire) && (expired || disk || count || failed)){
        if(s.manual){auto expected=ManualCapture::Reason::None;
            s.finishReason.compare_exchange_strong(expected,failed?ManualCapture::Reason::OutputFailure:disk?ManualCapture::Reason::DiskLimit:
                count?ManualCapture::Reason::CountLimit:ManualCapture::Reason::DurationLimit);}
        std::lock_guard lock(s.mutex);
        if(s.active.exchange(false,std::memory_order_acq_rel) && expired)++s.windowExpired;
        s.stop.store(true,std::memory_order_release);if(s.wake)SetEvent(s.wake);
    }
}
#ifdef GENSHIN_RUNTIME_CAPTURE_TEST
// Compile-time fixture seam only; production has no injectable graphics callbacks.
inline HRESULT (*TestGetData)(ID3D11DeviceContext*,ID3D11Query*,BOOL*)=nullptr;
#endif
inline HRESULT QueryComplete(ID3D11DeviceContext* context,ID3D11Query* query,BOOL* complete)noexcept {
#ifdef GENSHIN_RUNTIME_CAPTURE_TEST
    if(TestGetData)return TestGetData(context,query,complete);
#endif
    return context->GetData(query,complete,sizeof(*complete),D3D11_ASYNC_GETDATA_DONOTFLUSH);
}
inline bool Write(State& s,const std::wstring& name,const void* p,size_t n) {
    if(n>MaxDiskBytes || s.diskBytes>MaxDiskBytes-n){++s.diskBudgetDrops;return false;}
    s.diskBytes+=n; // Attempted bytes: partial/failed writes never refund the session budget.
    std::ofstream out(std::filesystem::path(s.directory)/name,std::ios::binary|std::ios::trunc);
    if(!out || !out.write(static_cast<const char*>(p),n) || !out.flush()){++s.writeFailures;return false;}return true;
}
inline Json Counters(const State& s) {
    return {{"firstFamilyDrawTick",s.firstFamilyDrawTick.load()},{"armedTimeoutMilliseconds",MaxArmedMs},
        {"familyWindowMilliseconds",MaxWindowMs},{"createCallbacks",s.createSequence.load()},{"knownFamilyCreates",s.knownCreates.load()},{"oversize",s.oversize.load()},
        {"queueDrops",s.queueDrops.load()},{"lockDrops",s.lockDrops.load()},{"allocationFailures",s.allocationFailures.load()},
        {"shaderBudgetDrops",s.shaderBudgetDrops.load()},{"cbBudgetDrops",s.cbBudgetDrops.load()},{"cbOffers",s.cbOffers.load()},
        {"cbMissingCreate",s.cbMissingCreate.load()},{"cbBusy",s.cbBusy.load()},{"cbAllocationFailures",s.cbAllocationFailures.load()},
        {"cbQueryFailures",s.cbQueryFailures.load()},{"cbMapRetries",s.cbMapRetries.load()},{"wrongThread",s.wrongThread.load()},
        {"writeFailures",s.writeFailures.load()},{"diskBudgetDrops",s.diskBudgetDrops.load()},
        {"workerDiscardJobs",s.workerDiscardJobs.load()},{"workerDiscardBytes",s.workerDiscardBytes.load()},
        {"windowExpired",s.windowExpired.load()},{"routeOffers",s.routeOffers.load()},{"routeBudgetDrops",s.routeBudgetDrops.load()},
        {"passOffers",s.passOffers.load()},{"passBudgetDrops",s.passBudgetDrops.load()},
        {"passTokenMissing",s.passTokenMissing.load()},{"passTokenAmbiguous",s.passTokenAmbiguous.load()},
        {"traceBudgetDrops",s.traceBudgetDrops.load()},{"manual",s.manual},
        {"rawCreateCallbacks",s.rawCreateSequence.load()},{"rawCacheObjects",s.rawCacheObjects.load()},
        {"rawCacheBytes",s.rawCacheBytes.load()},{"rawCacheDrops",s.rawCacheDrops.load()},
        {"inputOffers",s.inputOffers.load()},{"inputDraws",s.inputDraws.load()},{"inputDrops",s.inputDrops.load()},
        {"inputBytesReserved",s.inputBytesReserved.load()},{"gpuPending",s.gpuPending.load()},
        {"gpuCompleted",s.gpuCompleted.load()},{"gpuDropped",s.gpuDropped.load()},
        {"gpuQuarantined",s.gpuQuarantined.load()},{"gpuSlowInformational",s.gpuSlow.load()},
        {"artifactFilesWritten",s.artifactFilesWritten.load()},
        {"shaderArtifactsWritten",s.shaderArtifactsWritten.load()},{"constantArtifactsWritten",s.constantArtifactsWritten.load()},
        {"bindingRecordsWritten",s.bindingRecordsWritten.load()},{"recordsWritten",s.recordsWritten.load()},
        {"diskAttemptedBytes",s.diskBytes.load()},{"finishReason",static_cast<unsigned>(s.finishReason.load())}};
}
inline Json WitnessJson(const ResourceProof::Witness& w) {
    return {{"resourceLifetime",w.resourceLifetime},{"viewLifetime",w.viewLifetime},{"deviceEpoch",w.deviceEpoch},
        {"observationEpoch",w.observationEpoch},{"lastObservedSerial",w.lastObservedSerial},{"lastObservedEpoch",w.lastObservedEpoch},
        {"writerContext",w.writerContext},{"writerThread",w.writerThread},{"subresource",w.subresource},{"mapType",w.mapType},
        {"kind",static_cast<unsigned>(w.kind)},{"reason",static_cast<unsigned>(w.reason)},
        {"observedMutationCall",w.observedMutationCall},{"subresourceKnown",w.subresourceKnown},{"wholeResource",w.wholeResource},
        {"lastWriteProven",false},{"completeMutationCoverage",false}};
}
inline Json RouteJson(const RouteFact& r) {
    return {{"kind",r.kind==RouteFact::Kind::OriginalDrawReturned?"original-draw-returned":"present-completed"},
        {"device",r.device},{"context",r.context},{"deviceEpoch",r.deviceEpoch},{"ownerEpoch",r.ownerEpoch},{"configEpoch",r.configEpoch},
        {"threadId",r.thread},{"drawSerial",r.drawSerial},{"instance",r.instance},{"generation",r.generation},{"frame",r.frame},
        {"shaderIdentity",r.shaderIdentity},{"shaderHashFNV64",r.shaderHash},{"swapchain",r.swapchain},{"presentSerial",r.presentSerial},
        {"traceRootDraw",r.traceRootDraw},{"traceDepth",r.traceDepth},
        {"presentHRESULT",r.presentResult},{"originalDrawReturned",r.originalDrawReturned},{"presentRecorded",r.presentRecorded},
        {"successfulNonTest",r.successfulNonTest},{"sameRender",r.sameRender},{"directOutputIdentityMatchObserved",r.directIdentityMatch},
        {"outputWitness",WitnessJson(r.output)},{"backbufferWitness",WitnessJson(r.backbuffer)},
        {"sourceToOutputCorrespondence","UNKNOWN"},{"engineSemanticsCertified",false}};
}
inline Json TextureJson(const D3D11_TEXTURE2D_DESC& d) {
    return {{"size",{d.Width,d.Height}},{"mipLevels",d.MipLevels},{"arraySize",d.ArraySize},{"rawFormat",static_cast<unsigned>(d.Format)},
        {"sampleCount",d.SampleDesc.Count},{"sampleQuality",d.SampleDesc.Quality},{"usage",static_cast<unsigned>(d.Usage)},
        {"bindFlags",d.BindFlags},{"cpuAccessFlags",d.CPUAccessFlags},{"miscFlags",d.MiscFlags}};
}
inline Json DrawJson(const DrawFact& d) {
    Json j={{"device",d.device},{"context",d.context},{"deviceEpoch",d.deviceEpoch},{"ownerEpoch",d.ownerEpoch},{"configEpoch",d.configEpoch},
        {"threadId",d.thread},{"drawSerial",d.drawSerial},{"instance",d.instance},{"generation",d.generation},{"frame",d.frame},{"captureTick",d.captureTick},
        {"shaderIdentity",d.shaderIdentity},{"shaderHashFNV64",d.shaderHash},{"shaderObject",d.shaderObject},{"shaderCreateSequence",d.creation.sequence},
        {"shaderBytesQueuedAtCreate",d.creation.queued},{"shaderCreateSessionTick",d.creation.sessionTick},{"cb0Identity",d.cbIdentity},{"cb0Bytes",d.cbBytes},
        {"cb0FirstConstant",d.firstConstant},{"cb0NumConstants",d.numConstants},{"cb0RangeKnown",d.constantRangeKnown},
        {"rawRenderJitter",{d.jitterX,d.jitterY}},{"renderSize",{d.renderWidth,d.renderHeight}},{"outputSize",{d.outputWidth,d.outputHeight}},
        {"viewport",{d.viewport.TopLeftX,d.viewport.TopLeftY,d.viewport.Width,d.viewport.Height,d.viewport.MinDepth,d.viewport.MaxDepth}},
        {"invalidatedBeforeReadback",d.invalidated},{"gpuReadbackComplete",d.gpuReadbackComplete},{"upstreamWriterLineage","UNKNOWN"},
        {"familyPass",d.familyPass},{"consumerRoute",d.consumerRoute},{"drawCount",d.drawCount},{"drawCountKnown",d.drawCountKnown},
        {"captureTokenMatches",d.captureTokenMatches},{"tokenAssociated",d.instance!=0 && d.generation!=0},{"viewportCount",d.viewportCount},
        {"tokenAssociationAuthority","unique recent Render dimensions/generation candidate; no engine command-buffer correlation proof"},
        {"motionEncoding","UNKNOWN"},{"depthConvention","UNKNOWN"},{"jitterUnits","UNKNOWN"},{"colourDomain","UNKNOWN"},{"sourceSubrect","UNKNOWN"},
        {"engineSemanticsCertified",false},{"sourceToOutputCorrespondence","UNKNOWN"}};
    j["sourceReads"]=Json::array();for(unsigned i=0;i<3;++i)j["sourceReads"].push_back({{"resource",d.resources[i]},{"view",d.views[i]},
        {"slot",d.slots[i]},{"viewFormat",d.formats[i]},{"size",{d.widths[i],d.heights[i]}},{"writeWitness",WitnessJson(d.sourceWrites[i])}});
    j["boundPixelSrvs"]=Json::array();for(unsigned i=0;i<d.boundResources.size();++i)if(d.boundViews[i])j["boundPixelSrvs"].push_back({{"slot",i},
        {"resource",d.boundResources[i]},{"view",d.boundViews[i]},{"viewFormat",d.boundFormats[i]},
        {"size",{d.boundWidths[i],d.boundHeights[i]}},{"writeWitness",WitnessJson(d.boundWrites[i])},
        {"texture",TextureJson(d.boundDescriptions[i])},{"viewDimension",static_cast<unsigned>(d.boundViewDescriptions[i].ViewDimension)},
        {"texture2DMip",d.boundViewDescriptions[i].ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D?d.boundViewDescriptions[i].Texture2D.MostDetailedMip:0},
        {"texture2DMipLevels",d.boundViewDescriptions[i].ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D?d.boundViewDescriptions[i].Texture2D.MipLevels:0}});
    j["boundOutputRtvs"]=Json::array();for(unsigned i=0;i<d.outputResources.size();++i)if(d.outputViews[i])j["boundOutputRtvs"].push_back({{"slot",i},
        {"resource",d.outputResources[i]},{"view",d.outputViews[i]},{"viewFormat",d.outputFormats[i]},
        {"size",{d.outputWidths[i],d.outputHeights[i]}},{"writeWitness",WitnessJson(d.outputWrites[i])},
        {"texture",TextureJson(d.outputDescriptions[i])},{"viewDimension",static_cast<unsigned>(d.outputViewDescriptions[i].ViewDimension)},
        {"texture2DMip",d.outputViewDescriptions[i].ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D?d.outputViewDescriptions[i].Texture2D.MipSlice:0}});
    j["observedReadEdges"]=Json::array();for(unsigned i=0;i<d.readEdgeCount;++i){const auto& e=d.readEdges[i];
        j["observedReadEdges"].push_back({{"parentDraw",e.parentDraw},{"rootDraw",e.rootDraw},{"resourceLifetime",e.resourceLifetime},
            {"readSlot",e.readSlot},{"depth",e.depth}});}
    j["readEdgesTruncated"]=d.readEdgesTruncated;j["pixelSrvSlotsInspected"]=d.boundResources.size();
    j["selectedOutputSlot"]=d.selectedOutputSlot;j["outputWitness"]=WitnessJson(d.outputWitness);
    j["bindingMeaning"]="Same pre-draw API bindings; not semantic or final-output proof";return j;
}
inline Json ReflectShader(const std::vector<uint8_t>& bytes) {
    ComPtr<ID3D11ShaderReflection> reflection;
    const auto hr=D3DReflect(bytes.data(),bytes.size(),__uuidof(ID3D11ShaderReflection),reinterpret_cast<void**>(reflection.GetAddressOf()));
    Json result={{"hresult",static_cast<int32_t>(hr)},{"meaning","Bytecode declarations only; names are not engine semantic proof"}};
    if(FAILED(hr) || !reflection)return result;
    D3D11_SHADER_DESC desc{};if(FAILED(reflection->GetDesc(&desc)))return result;
    result["boundResources"]=Json::array();result["constantBuffers"]=Json::array();
    result["resourceCount"]=desc.BoundResources;result["constantBufferCount"]=desc.ConstantBuffers;
    for(UINT i=0;i<(std::min)(desc.BoundResources,256u);++i){D3D11_SHADER_INPUT_BIND_DESC binding{};
        if(SUCCEEDED(reflection->GetResourceBindingDesc(i,&binding)))result["boundResources"].push_back({
            {"name",binding.Name?binding.Name:""},{"type",static_cast<unsigned>(binding.Type)},{"slot",binding.BindPoint},
            {"count",binding.BindCount},{"dimension",static_cast<unsigned>(binding.Dimension)},
            {"returnType",static_cast<unsigned>(binding.ReturnType)},{"samples",binding.NumSamples},{"flags",binding.uFlags}});}
    for(UINT i=0;i<(std::min)(desc.ConstantBuffers,32u);++i){auto* buffer=reflection->GetConstantBufferByIndex(i);
        D3D11_SHADER_BUFFER_DESC cb{};if(!buffer || FAILED(buffer->GetDesc(&cb)))continue;
        Json constants={{"name",cb.Name?cb.Name:""},{"bytes",cb.Size},{"type",static_cast<unsigned>(cb.Type)},
            {"variables",Json::array()},{"variableCount",cb.Variables},{"variablesTruncated",cb.Variables>256}};
        for(UINT v=0;v<(std::min)(cb.Variables,256u);++v){auto* variable=buffer->GetVariableByIndex(v);D3D11_SHADER_VARIABLE_DESC value{};
            if(!variable || FAILED(variable->GetDesc(&value)))continue;D3D11_SHADER_TYPE_DESC type{};
            auto* variableType=variable->GetType();const bool typed=variableType && SUCCEEDED(variableType->GetDesc(&type));
            constants["variables"].push_back({{"name",value.Name?value.Name:""},{"offset",value.StartOffset},{"bytes",value.Size},
                {"flags",value.uFlags},{"typeKnown",typed},{"class",static_cast<unsigned>(type.Class)},
                {"type",static_cast<unsigned>(type.Type)},{"rows",type.Rows},{"columns",type.Columns},{"elements",type.Elements}});}
        result["constantBuffers"].push_back(std::move(constants));}
    result["truncated"]=desc.BoundResources>256 || desc.ConstantBuffers>32;return result;
}
inline void WorkerBody(State& s) {
    // A collision or absent parent refuses capture instead of replacing evidence.
    if(s.manual)std::filesystem::create_directories(std::filesystem::path(s.directory).parent_path());
    if(!std::filesystem::create_directory(s.directory)){++s.writeFailures;return;}
    size_t moduleBudget=MaxModuleHashBytes;
    Json session={{"schema","genshin-runtime-artifacts-v3"},{"sessionId",s.sessionId},{"captureStartTick",s.startTick},
        {"initialConfigEpoch",s.configEpoch},{"mode",s.manual?"ManualObservationParallel":"ObservationOnly"},{"windowMilliseconds",MaxWindowMs},
        {"windowStart",s.manual?"manual arm native boundary":"first observed known family pixel draw"},{"armedTimeoutMilliseconds",s.manual?MaxWindowMs:MaxArmedMs},
        {"processEnvironmentObservationOverride",ProcessObservationRequested()},
        {"routeSamples",s.manual?0:MaxRouteSamples},{"gameEvidenceAuthenticated",false},{"processId",s.pid},{"sessionUTC",TimeText(s.utc)+"Z"},{"sessionLocal",TimeText(s.local)},
        {"processCreationFileTime",(static_cast<uint64_t>(s.processCreation.dwHighDateTime)<<32)|s.processCreation.dwLowDateTime},
        {"exe",FileIdentity(s.exe,moduleBudget)},{"observerDll",FileIdentity(s.host,moduleBudget)},{"UnityPlayer",FileIdentity(s.unity,moduleBudget)},
        {"GameAssembly",FileIdentity(s.assembly,moduleBudget)},{"UserAssembly",FileIdentity(s.userAssembly,moduleBudget)},
        {"actualLoadedConfigPath","UNKNOWN - private Config path is not exposed; configEpoch is recorded per draw"},
        {"shaderHookCoverage","successful shader creation callbacks while capture is enabled; bounded known-family PS bytecode cache supports manual arm; pre-hook and uncached create bytes UNKNOWN"},
        {"passBindingCoverage",s.manual?"known family PS binding descriptors and CB0; unmatched Render tokens remain unassociated; output consumer route not observed":
            "known family PS and bounded same-token PS reads of observed accumulate outputs; no copy/compute route coverage"},
        {"traceLimits",{{"passSamples",MaxPassSamples},{"inspectedDraws",MaxTraceDraws},{"retainedOutputNodes",MaxTraceNodes}}},
        {"missedBeforeInstall","UNKNOWN count; draw cbMissingCreate is an observed lower bound"},
        {"limits",{{"shaderBytes",MaxShaderBytes},{"queueBytes",MaxQueuedBytes},{"queueJobs",MaxJobs},{"shaderCreations",MaxShaders},{"cbSamples",MaxCbSamples},{"diskAttemptedBytes",MaxDiskBytes},{"moduleHashReadBytes",MaxModuleHashBytes}}},
        {"engineSemanticsCertified",false}};
    if(s.adapterKnown)session["adapter"]={{"description",Utf8(s.adapter.Description)},{"vendor",s.adapter.VendorId},{"device",s.adapter.DeviceId},
        {"luidHigh",s.adapter.AdapterLuid.HighPart},{"luidLow",s.adapter.AdapterLuid.LowPart}};
    else session["adapter"]="UNKNOWN";
    auto initial=session.dump(2);if(!Write(s,L"session.json",initial.data(),initial.size()))return;
    std::ofstream records(std::filesystem::path(s.directory)/L"records.jsonl",std::ios::binary|std::ios::trunc);
    if(!records){++s.writeFailures;return;}
    auto armed=Counters(s);armed["processed"]=0;armed["phase"]="ARMED_WAITING_FOR_KNOWN_FAMILY_DRAW";
    auto armedText=armed.dump(2);if(!Write(s,L"status.json",armedText.data(),armedText.size()))return;
    s.workerReady.store(true,std::memory_order_release);
    uint64_t processed=0,writtenRecords=0,bindings=0,lastCheckpoint=GetTickCount64();
    auto checkpoint=[&]() {
        records.flush();if(!records){++s.writeFailures;return;}
        s.recordsWritten.store(writtenRecords);s.bindingRecordsWritten.store(bindings);
        auto counters=Counters(s);counters["processed"]=processed;counters["shutdownRequested"]=s.stop.load();
        counters["phase"]=s.stop.load()?"DRAINING":s.firstFamilyDrawTick.load()?"SAMPLING":"ARMED_WAITING_FOR_KNOWN_FAMILY_DRAW";
        counters["manual"]=s.manual;counters["finishReason"]=static_cast<unsigned>(s.finishReason.load());
        counters["shaderArtifactsWritten"]=s.shaderArtifactsWritten.load();counters["constantArtifactsWritten"]=s.constantArtifactsWritten.load();
        counters["bindingRecordsWritten"]=s.bindingRecordsWritten.load();auto text=counters.dump(2);Write(s,L"status.json",text.data(),text.size());
        lastCheckpoint=GetTickCount64();
    };
    for(;;) {
        CloseExpiredWindow(s);
        Job job;bool have=false;
        {std::lock_guard lock(s.mutex);if(!s.queue.empty()){job=std::move(s.queue.front());s.queuedBytes-=job.Payload();s.queue.pop_front();have=true;}}
        if(!have){if(s.stop.load(std::memory_order_acquire) && s.gpuPending.load(std::memory_order_acquire)==0)break;
            if(GetTickCount64()-lastCheckpoint>=1000)checkpoint();WaitForSingleObject(s.wake,1000);continue;}
        Json record={{"ordinal",++processed},{"processId",s.pid},{"sessionId",s.sessionId},{"captureEventTick",job.eventTick}};
        if(job.kind==Job::Kind::Shader){
            const auto base=L"ps-"+std::to_wstring(job.sequence);const auto bytesName=base+L".cso";
            const auto sha=HashBytes(job.shader.data(),job.shader.size());const bool written=Write(s,bytesName,job.shader.data(),job.shader.size());if(written)++s.shaderArtifactsWritten;
            record.update({{"kind","successful-pixel-shader-create"},{"createSequence",job.sequence},{"shaderIdentity",job.shaderIdentity},
                {"shaderObject",job.shaderObject},{"deviceEpoch",job.deviceEpoch},{"fnv64",job.fnv},{"sha256",sha},{"bytes",job.shader.size()},
                {"createSucceeded",true},{"exportedFromBoundedPreArmCache",job.eventTick<s.startTick},{"exactCreateHRESULT","UNKNOWN: existing hook forwards successful callback only"},{"artifact",Utf8(bytesName)},{"artifactWritten",written}});
            const auto actualHash=Family::Hash(job.shader.data(),job.shader.size());
            record["actualBytecodeFNV64"]=actualHash;record["tagMatchesBytecode"]=actualHash==job.fnv;
            record["reflection"]=ReflectShader(job.shader);
            ComPtr<ID3DBlob> assembly;const auto hr=D3DDisassemble(job.shader.data(),job.shader.size(),0,nullptr,&assembly);
            record["disassembleHRESULT"]=static_cast<int32_t>(hr);
            if(SUCCEEDED(hr) && assembly && assembly->GetBufferSize()<=4*MaxShaderBytes){const auto asmName=base+L".asm";
                record["disassemblyWritten"]=Write(s,asmName,assembly->GetBufferPointer(),assembly->GetBufferSize());record["disassembly"]=Utf8(asmName);}
        }else if(job.kind==Job::Kind::Cb){
            const auto name=L"cb0-"+std::to_wstring(job.draw.drawSerial)+L"-"+std::to_wstring(processed)+L".bin";
            record.update(DrawJson(job.draw));record["kind"]="source-draw-cb0";record["readbackObservationTick"]=job.readbackTick;record["cb0Artifact"]=Utf8(name);
            record["cb0SHA256"]=HashBytes(job.cb.data(),job.draw.cbBytes);const bool written=Write(s,name,job.cb.data(),job.draw.cbBytes);
            record["cb0ArtifactWritten"]=written;if(written)++s.constantArtifactsWritten;
        }else if(job.kind==Job::Kind::Artifact){
            record.update(DrawJson(job.draw));record.update(job.metadata);
            record["readbackObservationTick"]=job.readbackTick;
            if(!job.raw.empty()){
                // Names are generated internally; never accept a path from captured data.
                const std::filesystem::path name(job.artifactName);
                const bool safe=!job.artifactName.empty() && name.filename()==name && !name.is_absolute();
                record["artifact"]=Utf8(job.artifactName);record["artifactBytes"]=job.raw.size();
                record["artifactSHA256"]=HashBytes(job.raw.data(),job.raw.size());
                const bool written=safe && Write(s,job.artifactName,job.raw.data(),job.raw.size());
                record["artifactWritten"]=written;
                if(written){++s.artifactFilesWritten;
                    if(job.artifactName.ends_with(L".cso"))++s.shaderArtifactsWritten;
                    else if(job.artifactName.ends_with(L".cb.bin"))++s.constantArtifactsWritten;}
            }
        }else if(job.kind==Job::Kind::Binding){
            record.update(DrawJson(job.draw));record["kind"]="source-draw-bindings";
        }
        if(job.kind==Job::Kind::Route)record.update(RouteJson(job.route));
        auto line=record.dump()+"\n";if(records && line.size()<=MaxDiskBytes-s.diskBytes){s.diskBytes+=line.size();records.write(line.data(),line.size());if(!records)++s.writeFailures;else {++writtenRecords;if(job.kind==Job::Kind::Binding)++bindings;}}
        else if(!records)++s.writeFailures;else ++s.diskBudgetDrops;
        // At most one records/status flush per second, independent of render cadence.
        if(GetTickCount64()-lastCheckpoint>=1000)checkpoint();
    }
    checkpoint();
    auto final=Counters(s);final["processed"]=processed;final["writerReachedFinalStatus"]=true;final["diskBytesBeforeStatus"]=s.diskBytes.load();
    final["phase"]=s.writeFailures.load()?"FAILED":"CAPTURE_STOPPED";
    auto text=final.dump(2);Write(s,L"status.json",text.data(),text.size());
}
inline DWORD WINAPI Worker(void* opaque) {
    auto& s=*static_cast<State*>(opaque);try{WorkerBody(s);}catch(...){++s.writeFailures;}
    HANDLE wake;HMODULE module;
    {
        std::deque<Job> discarded;
        {std::lock_guard lock(s.mutex);s.active.store(false,std::memory_order_release);
            s.workerDiscardJobs.fetch_add(s.queue.size());s.workerDiscardBytes.fetch_add(s.queuedBytes);
            discarded.swap(s.queue);s.queuedBytes=0;
            wake=s.wake;s.wake=nullptr;module=s.module;s.module=nullptr;}
        // On worker failure these are discarded CPU payloads, never successful export/drain.
        // Destroy outside the mutex, and before unloading the worker's code module.
    }
    // Module reference keeps this code alive during asynchronous drain. No render-thread join, GPU use or wait.
    if(wake)CloseHandle(wake);
    // No access to s after this release. Native may now start a new generation.
    s.workerFinished.store(true,std::memory_order_release);FreeLibraryAndExitThread(module,0);
}
inline void Start(ID3D11Device* device,bool observationOnly=false,uint64_t configEpoch=0,bool manual=false)noexcept {
    auto& s=Get();if(s.attempted || !observationOnly || !configEpoch)return;
    wchar_t enabled[3]{};if(!manual && (GetEnvironmentVariableW(L"GENSHIN_FSR_RUNTIME_CAPTURE",enabled,3)!=1 || enabled[0]!=L'1'))return;
    s.attempted=true;s.configEpoch=configEpoch;
    try {
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(&Worker),&s.module))return;
        s.host=ModulePath(s.module);s.exe=ModulePath(nullptr);
        std::filesystem::path base;
        if(manual){base=std::filesystem::path(s.host).parent_path()/L"GenshinCapture";}
        else {wchar_t root[32768]{};const auto rootSize=GetEnvironmentVariableW(L"GENSHIN_FSR_RUNTIME_CAPTURE_DIR",root,32768);
            if(rootSize && rootSize<32768)base=root;
            if(!base.is_absolute() || !std::filesystem::is_directory(base)){FreeLibrary(s.module);s.module=nullptr;return;}}
        // Filesystem validation/creation for the manual route belongs exclusively to WorkerBody.
        auto module=GetModuleHandleW(L"UnityPlayer.dll");if(module)s.unity=ModulePath(module);
        module=GetModuleHandleW(L"GameAssembly.dll");if(module)s.assembly=ModulePath(module);
        module=GetModuleHandleW(L"UserAssembly.dll");if(module)s.userAssembly=ModulePath(module);
        s.pid=GetCurrentProcessId();GetSystemTime(&s.utc);GetLocalTime(&s.local);FILETIME exit{},kernel{},user{};
        if(!GetProcessTimes(GetCurrentProcess(),&s.processCreation,&exit,&kernel,&user)){
            FreeLibrary(s.module);s.module=nullptr;return;}
        s.startTick=GetTickCount64();
        const auto creation=(static_cast<uint64_t>(s.processCreation.dwHighDateTime)<<32)|s.processCreation.dwLowDateTime;
        if(!creation || s.exe.empty() || s.host.empty()){FreeLibrary(s.module);s.module=nullptr;return;}
        const auto folder=std::to_wstring(s.pid)+L"-"+std::to_wstring(creation)+L"-"+std::to_wstring(s.startTick);
        s.sessionId=Utf8(folder);s.directory=(base/folder).wstring();
        if(device){ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;if(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
            SUCCEEDED(dxgi->GetAdapter(&adapter)))s.adapterKnown=SUCCEEDED(adapter->GetDesc(&s.adapter));}
        s.wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!s.wake){FreeLibrary(s.module);s.module=nullptr;return;}
        s.workerFinished.store(false,std::memory_order_release);s.active.store(true,std::memory_order_release);const auto thread=CreateThread(nullptr,0,Worker,&s,0,nullptr);
        if(!thread){s.workerFinished=true;s.active=false;CloseHandle(s.wake);s.wake=nullptr;FreeLibrary(s.module);s.module=nullptr;return;}CloseHandle(thread);
    }catch(...){s.workerFinished=true;s.active=false;if(s.wake)CloseHandle(s.wake);s.wake=nullptr;if(s.module)FreeLibrary(s.module);s.module=nullptr;}
}
inline void StopManual(ManualCapture::Reason reason)noexcept {
    auto& s=Get();if(!s.manual)return;auto expected=ManualCapture::Reason::None;s.finishReason.compare_exchange_strong(expected,reason);
    std::lock_guard lock(s.mutex);s.active.store(false,std::memory_order_release);s.stop.store(true,std::memory_order_release);
    if(s.wake)SetEvent(s.wake);
}
inline void Stop()noexcept {
    auto& s=Get();std::lock_guard lock(s.mutex);
    if(s.manual)return;
    if(!s.active.exchange(false,std::memory_order_acq_rel))return;
    s.stop.store(true,std::memory_order_release);if(s.wake)SetEvent(s.wake);
}
inline bool Enqueue(Job&& job)noexcept {
    auto& s=Get();const bool draining=job.kind==Job::Kind::Artifact && s.gpuPending.load()!=0;
    if(!Enabled() && !draining)return false;std::unique_lock lock(s.mutex,std::try_to_lock);
    if(!lock){++s.lockDrops;return false;}
    if((!Enabled() || s.stop.load()) && !draining)return false;
    const auto bytes=job.Payload();if(s.queue.size()>=MaxJobs || bytes>MaxQueuedBytes || s.queuedBytes>MaxQueuedBytes-bytes){++s.queueDrops;return false;}
    try{s.queue.emplace_back(std::move(job));s.queuedBytes+=bytes;SetEvent(s.wake);return true;}catch(...){++s.allocationFailures;return false;}
}
inline void ObserveRoute(RouteFact route)noexcept {
    if(!Enabled() || Get().manual || route.configEpoch!=Get().configEpoch)return;auto& s=Get();++s.routeOffers;
    // Native entry holds nativeMutex. Events are never consumed as routing receipts.
    if(s.routeBudgetCount>=MaxRouteSamples){++s.routeBudgetDrops;return;}++s.routeBudgetCount;
    Job job;job.kind=Job::Kind::Route;job.eventTick=GetTickCount64();job.route=route;Enqueue(std::move(job));
}
inline void ShaderCreated(ID3D11PixelShader* shader,const void* bytes,size_t size,const Family::Shader& fact)noexcept {
    if(!Enabled() || Get().manual || !shader)return;auto& s=Get();const auto sequence=++s.createSequence;
    if(!Expected(fact.hash))return;++s.knownCreates;
    CreateId id{sequence,fact.hash,false,s.startTick};
    try {
        if(!bytes || !size || size>MaxShaderBytes){++s.oversize;shader->SetPrivateData(CreateKey,sizeof(id),&id);return;}
        // Called under nativeMutex; these session budgets are independent of pipeline resets.
        if(s.shaderBudgetCount>=MaxShaders || size>MaxQueuedBytes-s.shaderBudgetBytes){++s.shaderBudgetDrops;shader->SetPrivateData(CreateKey,sizeof(id),&id);return;}
        ++s.shaderBudgetCount;s.shaderBudgetBytes+=size;
        Job job;job.eventTick=GetTickCount64();job.sequence=sequence;job.deviceEpoch=fact.deviceEpoch;job.shaderIdentity=fact.identity;
        job.shaderObject=reinterpret_cast<uintptr_t>(shader);job.fnv=fact.hash;
        job.shader.assign(static_cast<const uint8_t*>(bytes),static_cast<const uint8_t*>(bytes)+size);
        id.queued=Enqueue(std::move(job));shader->SetPrivateData(CreateKey,sizeof(id),&id);
    }catch(...){++s.allocationFailures;shader->SetPrivateData(CreateKey,sizeof(id),&id);}
}


struct CachedShader {Family::Shader fact{};uint64_t tick=0;std::vector<uint8_t> bytes;};
inline std::array<CachedShader,8>& ShaderCache(){static auto* cache=new std::array<CachedShader,8>;return *cache;}
inline void CacheShader(const void* bytes,size_t size,const Family::Shader& fact)noexcept {
    if(!fact.Known() || !Expected(fact.hash) || !bytes || !size || size>MaxShaderBytes)return;
    auto& cache=ShaderCache();for(const auto& entry:cache)if(entry.fact.hash==fact.hash && entry.fact.deviceEpoch==fact.deviceEpoch)return;
    for(auto& entry:cache)if(entry.bytes.empty()){try{entry.bytes.assign(static_cast<const uint8_t*>(bytes),static_cast<const uint8_t*>(bytes)+size);
        entry.fact=fact;entry.tick=GetTickCount64();}catch(...){}return;}
}
inline uint64_t CachedShaderCount()noexcept{uint64_t count=0;for(const auto& entry:ShaderCache())if(!entry.bytes.empty())++count;return count;}
inline void ExportCachedShaders(uint64_t deviceEpoch)noexcept {
    auto& s=Get();for(const auto& entry:ShaderCache())if(!entry.bytes.empty() && entry.fact.deviceEpoch==deviceEpoch){
        try{Job job;job.eventTick=entry.tick;job.sequence=++s.createSequence;job.deviceEpoch=deviceEpoch;job.shaderIdentity=entry.fact.identity;
            job.fnv=entry.fact.hash;job.shader=entry.bytes;++s.shaderBudgetCount;s.shaderBudgetBytes+=job.shader.size();++s.knownCreates;Enqueue(std::move(job));
        }catch(...){++s.allocationFailures;}}
}

// Only CB0, never another copy of M05's three guide textures. All GPU calls stay on the source thread.
class ConstantReadback final {
    struct Slot {
        ComPtr<ID3D11Buffer> source,staging;
        ComPtr<ID3D11PixelShader> shader;
        ComPtr<ID3D11Query> query;
        DrawFact draw{};bool pending=false,queryFailed=false;
    };
    // Reserve the last slot for accumulate: early pre-passes must not starve its CB.
    std::array<Slot,6> slots_{};
public:
    void Revoke()noexcept{for(auto& slot:slots_)if(slot.pending)slot.draw.invalidated=true;}
    void CheckGeneration(uint64_t(*generation)(uint64_t))noexcept {
        for(auto& slot:slots_)if(slot.pending && generation(slot.draw.instance)!=slot.draw.generation)slot.draw.invalidated=true;
    }
    bool Queue(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Buffer* source,DrawFact draw)noexcept {
        if(!Enabled() || draw.configEpoch!=Get().configEpoch || !device || !context || !source)return false;auto& s=Get();++s.cbOffers;
        if(GetCurrentThreadId()!=draw.thread){++s.wrongThread;return false;}
        if(s.cbBudgetCount>=MaxCbSamples){++s.cbBudgetDrops;return false;}
        Slot* target=nullptr;const auto usable=GenshinFsrProfiles::Accumulate(draw.shaderHash)?slots_.size():slots_.size()-1;
        for(size_t i=0;i<usable;++i)if(!slots_[i].pending){target=&slots_[i];break;}if(!target){++s.cbBusy;return false;}
        try {
            if(context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
            ComPtr<ID3D11Device> owner;source->GetDevice(&owner);if(owner.Get()!=device)return false;
            context->GetDevice(owner.ReleaseAndGetAddressOf());if(owner.Get()!=device)return false;
            D3D11_BUFFER_DESC desc{};source->GetDesc(&desc);if(!desc.ByteWidth || desc.ByteWidth>MaxCbBytes || !(desc.BindFlags&D3D11_BIND_CONSTANT_BUFFER))return false;
            draw.cbBytes=desc.ByteWidth;draw.cbIdentity=reinterpret_cast<uintptr_t>(source);
            ComPtr<ID3D11PixelShader> shader;context->PSGetShader(&shader,nullptr,nullptr);const auto shaderFact=Family::Read(device,shader.Get(),draw.deviceEpoch);
            if(!shaderFact.Known() || shaderFact.bytes>MaxShaderBytes || shaderFact.hash!=draw.shaderHash || shaderFact.identity!=draw.shaderIdentity)return false;
            draw.shaderObject=reinterpret_cast<uintptr_t>(shader.Get());UINT bytes=sizeof(draw.creation);
            if(FAILED(shader->GetPrivateData(CreateKey,&bytes,&draw.creation)) || bytes!=sizeof(draw.creation) || draw.creation.hash!=draw.shaderHash || draw.creation.sessionTick!=s.startTick){draw.creation={};++s.cbMissingCreate;}
            ComPtr<ID3D11DeviceContext1> context1;ComPtr<ID3D11Buffer> actual;
            if(SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context1)))){
                context1->PSGetConstantBuffers1(0,1,&actual,&draw.firstConstant,&draw.numConstants);draw.constantRangeKnown=true;
            }else{context->PSGetConstantBuffers(0,1,&actual);draw.firstConstant=0;draw.numConstants=desc.ByteWidth/16;draw.constantRangeKnown=true;}
            if(actual.Get()!=source)return false;
            if(target->staging){D3D11_BUFFER_DESC old{};target->staging->GetDesc(&old);if(old.ByteWidth!=desc.ByteWidth)target->staging.Reset();}
            if(!target->staging){desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=desc.StructureByteStride=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                if(FAILED(device->CreateBuffer(&desc,nullptr,&target->staging))){++s.cbAllocationFailures;return false;}}
            if(!target->query){D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0};if(FAILED(device->CreateQuery(&q,&target->query))){++s.cbAllocationFailures;return false;}}
            target->draw=draw;target->source=source;target->shader=shader;target->pending=true;target->queryFailed=false;++s.cbBudgetCount;
            context->CopyResource(target->staging.Get(),source);context->End(target->query.Get());return true;
        }catch(...){++s.cbAllocationFailures;return false;}
    }
    bool Poll(ID3D11Device* device,ID3D11DeviceContext* context)noexcept {
        if(!device || !context)return Idle();auto& s=Get();
        for(auto& slot:slots_)if(slot.pending){
            if(GetCurrentThreadId()!=slot.draw.thread || reinterpret_cast<uintptr_t>(device)!=slot.draw.device ||
                reinterpret_cast<uintptr_t>(context)!=slot.draw.context){++s.wrongThread;return false;}
            if(FAILED(device->GetDeviceRemovedReason())){slot=Slot{};continue;}
            BOOL complete=FALSE;const auto hr=QueryComplete(context,slot.query.Get(),&complete);
            if(FAILED(hr)){slot.queryFailed=true;slot.draw.invalidated=true;++s.cbQueryFailures;continue;}if(hr==S_FALSE || !complete)continue;
            D3D11_MAPPED_SUBRESOURCE mapped{};const auto read=context->Map(slot.staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
            if(FAILED(read)){++s.cbMapRetries;continue;}
            Job job;job.kind=Job::Kind::Cb;job.eventTick=slot.draw.observationTick;job.readbackTick=GetTickCount64();job.draw=slot.draw;
            job.draw.gpuReadbackComplete=true;memcpy(job.cb.data(),mapped.pData,slot.draw.cbBytes);
            context->Unmap(slot.staging.Get(),0);Enqueue(std::move(job));slot.source.Reset();slot.shader.Reset();slot.pending=false;
        }
        return Idle();
    }
    bool Idle()const noexcept{for(const auto& slot:slots_)if(slot.pending)return false;return true;}
};
}
