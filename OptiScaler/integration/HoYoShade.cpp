#include "pch.h"
// Keep the /Yu checkpoint outside conditionals; standalone hosts supply a minimal pch.h.
#ifdef HOYOSHADE_STAGE_TEST
#include <Windows.h>
#include <string>
#include <array>
#include <memory>
#include <iterator>
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#endif
#include "HoYoShade.h"
#include "HoYoShadeApi.h"
#include "HoYoShadeColorContract.h"
#include "HoYoShadeOwnedRuntime.h"
#include "HoYoShadeReporting.h"
#include "HoYoShadeDeviceValidation.h"
#include "HoYoShadeAssociationDiagnostics.h"
#include "HoYoShadeNaturalAssociation.h"
#include <cstdio>
#include "HoYoShadeSelection.h"
#include "HoYoShadeDepthSemantics.h"
#include <bcrypt.h>
#include <psapi.h>
#include <filesystem>
#include <atomic>
#include <mutex>
#include <cstring>
#include <thread>
#include <vector>
#include <wrl/client.h>
#include <d3d11.h>
#ifndef HOYOSHADE_STAGE_TEST
#include <genshin/GenshinProducerTransport.h>
#include <genshin/GenshinPresentNr.h>
#include <Config.h>
#include <upscalers/GameRoutePolicy.h>
#include <Util.h>
#endif
#include "HoYoShadeOwnedStage.h"
#pragma comment(lib, "bcrypt.lib")

namespace HoYoShade
{
struct ExistingFactoryOwner final {
    HMODULE module=nullptr;
    uintptr_t terminal=0;
    std::array<char,40> symbol{};
    struct Range {
        const void* address=nullptr;MEMORY_BASIC_INFORMATION mapping{};
        std::array<unsigned char,320> bytes{};size_t size=0;
    };
    std::array<Range,64> ranges{};unsigned count=0;
    ~ExistingFactoryOwner() = default; // pinned module: never unload on this path
};
namespace {
bool KnownDiskIdentity(const wchar_t*,HANDLE = INVALID_HANDLE_VALUE) noexcept;
bool OwnerRead(const void* p,void* out,size_t size) noexcept {
    SIZE_T read=0;return ReadProcessMemory(GetCurrentProcess(),p,out,size,&read) && read==size;
}
bool OwnerRange(ExistingFactoryOwner& owner,const unsigned char* reference,size_t imageSize,
                uint32_t rva,size_t size,bool code=false) noexcept {
    if(!size || size>320 || rva>imageSize || size>imageSize-rva || owner.count==owner.ranges.size())return false;
    auto& r=owner.ranges[owner.count];r.address=reinterpret_cast<const unsigned char*>(owner.module)+rva;r.size=size;
    if(VirtualQuery(r.address,&r.mapping,sizeof(r.mapping))!=sizeof(r.mapping))return false;
    const auto at=reinterpret_cast<uintptr_t>(r.address),base=reinterpret_cast<uintptr_t>(r.mapping.BaseAddress);
    const auto protection=r.mapping.Protect&0xff;
    if(r.mapping.State!=MEM_COMMIT || r.mapping.Type!=MEM_IMAGE || r.mapping.AllocationBase!=owner.module ||
       (r.mapping.Protect&(PAGE_NOACCESS|PAGE_GUARD)) || at<base || at-base>r.mapping.RegionSize ||
       size>r.mapping.RegionSize-(at-base) ||
       (code && protection!=PAGE_EXECUTE_READ) ||
       (!code && protection!=PAGE_READONLY && protection!=PAGE_EXECUTE_READ))return false;
    if(!OwnerRead(r.address,r.bytes.data(),size) || std::memcmp(r.bytes.data(),reference+rva,size))return false;
    ++owner.count;return true;
}
bool ExistingCoreExports(HMODULE module,const void* terminal,const char* symbol,
                         ExistingFactoryOwner& result)
{
    if (!module || !terminal || !symbol) return false;
    wchar_t path[32768]{};
    const auto length=GetModuleFileNameW(module,path,static_cast<DWORD>(std::size(path)));
    if (!length || length>=std::size(path)) return false;
    const std::filesystem::path diskPath(path);
    if (_wcsicmp(diskPath.filename().c_str(),L"ReShade64.dll") ||
        _wcsicmp(diskPath.parent_path().filename().c_str(),L"HoYoShade")) return false;
    struct File {HANDLE value=INVALID_HANDLE_VALUE;~File(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}} file;
    file.value=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if(file.value==INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(file.value,FileAttributeTagInfo,&tag,sizeof(tag)) ||
       (tag.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) || !KnownDiskIdentity(path,file.value))return false;
    // Same held file as the existing content identity, mapped only as DATA.
    struct View {HANDLE mapping=nullptr;const unsigned char* data=nullptr;
        ~View(){if(data)UnmapViewOfFile(data);if(mapping)CloseHandle(mapping);}} view;
    view.mapping=CreateFileMappingW(file.value,nullptr,PAGE_READONLY,0,0,nullptr);
    if(!view.mapping)return false;
    view.data=static_cast<const unsigned char*>(MapViewOfFile(view.mapping,FILE_MAP_READ,0,0,0));
    if(!view.data)return false;
    constexpr size_t fileSize=5592064;
    const auto span=[](size_t at,size_t n,size_t limit){return at<=limit && n<=limit-at;};
    const auto read=[]<class T>(const unsigned char* bytes){T value{};std::memcpy(&value,bytes,sizeof(value));return value;};
    const auto dos=read.template operator()<IMAGE_DOS_HEADER>(view.data);
    if(dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || !span(dos.e_lfanew,sizeof(IMAGE_NT_HEADERS64),fileSize))return false;
    const auto nt=read.template operator()<IMAGE_NT_HEADERS64>(view.data+dos.e_lfanew);
    const auto& opt=nt.OptionalHeader;
    if(nt.Signature!=IMAGE_NT_SIGNATURE || nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 ||
       opt.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.FileHeader.SizeOfOptionalHeader!=sizeof(opt) ||
       opt.NumberOfRvaAndSizes<=IMAGE_DIRECTORY_ENTRY_BASERELOC ||
       !opt.SizeOfImage || opt.SizeOfImage>32*1024*1024 || opt.SizeOfHeaders>fileSize ||
       opt.SizeOfHeaders>opt.SizeOfImage || nt.FileHeader.NumberOfSections>32 ||
       !span(dos.e_lfanew,sizeof(nt),opt.SizeOfHeaders))return false;
    std::vector<unsigned char> image(opt.SizeOfImage);std::memcpy(image.data(),view.data,opt.SizeOfHeaders);
    const size_t sections=size_t(dos.e_lfanew)+sizeof(nt);
    if(!span(sections,size_t(nt.FileHeader.NumberOfSections)*sizeof(IMAGE_SECTION_HEADER),opt.SizeOfHeaders))return false;
    for(unsigned i=0;i<nt.FileHeader.NumberOfSections;++i){const auto s=read.template operator()<IMAGE_SECTION_HEADER>(view.data+sections+i*sizeof(IMAGE_SECTION_HEADER));
        if(!span(s.PointerToRawData,s.SizeOfRawData,fileSize) || !span(s.VirtualAddress,s.SizeOfRawData,image.size()))return false;
        std::memcpy(image.data()+s.VirtualAddress,view.data+s.PointerToRawData,s.SizeOfRawData);}
    // Relocation is the only permitted transform of the trusted raw code bytes.
    const auto reloc=opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if(!span(reloc.VirtualAddress,reloc.Size,image.size()))return false;
    for(size_t at=reloc.VirtualAddress;at<size_t(reloc.VirtualAddress)+reloc.Size;){
        if(!span(at,sizeof(IMAGE_BASE_RELOCATION),size_t(reloc.VirtualAddress)+reloc.Size))return false;
        const auto block=read.template operator()<IMAGE_BASE_RELOCATION>(image.data()+at);
        if(block.SizeOfBlock<sizeof(block) || (block.SizeOfBlock-sizeof(block))%2 ||
           !span(at,block.SizeOfBlock,size_t(reloc.VirtualAddress)+reloc.Size))return false;
        for(size_t o=sizeof(block);o<block.SizeOfBlock;o+=2){const auto item=read.template operator()<WORD>(image.data()+at+o);
            if(!(item>>12))continue;if((item>>12)!=IMAGE_REL_BASED_DIR64)return false;
            const auto target=uint64_t(block.VirtualAddress)+(item&4095);if(target>UINT32_MAX || !span(target,8,image.size()))return false;
            auto value=read.template operator()<uint64_t>(image.data()+target);
            value+=reinterpret_cast<uintptr_t>(module)-opt.ImageBase;std::memcpy(image.data()+target,&value,8);}
        at+=block.SizeOfBlock;}
    result.module=module;result.terminal=reinterpret_cast<uintptr_t>(terminal);
    if(!OwnerRange(result,image.data(),image.size(),0,sizeof(dos)) ||
       !OwnerRange(result,image.data(),image.size(),dos.e_lfanew,sizeof(nt)))return false;
    const auto directory=opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if(directory.Size<sizeof(IMAGE_EXPORT_DIRECTORY) || !span(directory.VirtualAddress,directory.Size,image.size()))return false;
    const auto ex=read.template operator()<IMAGE_EXPORT_DIRECTORY>(image.data()+directory.VirtualAddress);
    if(ex.NumberOfNames>65536 || ex.NumberOfFunctions>65536 ||
       !span(ex.AddressOfNames,size_t(ex.NumberOfNames)*4,image.size()) ||
       !span(ex.AddressOfNameOrdinals,size_t(ex.NumberOfNames)*2,image.size()) ||
       !span(ex.AddressOfFunctions,size_t(ex.NumberOfFunctions)*4,image.size()) ||
       !OwnerRange(result,image.data(),image.size(),directory.VirtualAddress,sizeof(ex)))return false;
    const auto resolve=[&](const char* name,const void* expected){
        unsigned found=0;
        for(DWORD i=0;i<ex.NumberOfNames;++i){const auto nameRva=read.template operator()<DWORD>(image.data()+ex.AddressOfNames+4*i);
            if(!span(nameRva,1,image.size()))return false;
            const auto end=static_cast<const unsigned char*>(std::memchr(image.data()+nameRva,0,(std::min)(size_t(128),image.size()-nameRva)));
            if(!end)return false;if(std::strcmp(reinterpret_cast<const char*>(image.data()+nameRva),name))continue;
            if(++found!=1)return false;
            const auto ordinal=read.template operator()<WORD>(image.data()+ex.AddressOfNameOrdinals+2*i);
            if(ordinal>=ex.NumberOfFunctions)return false;
            const auto rva=read.template operator()<DWORD>(image.data()+ex.AddressOfFunctions+4*ordinal);
            if((rva>=directory.VirtualAddress && rva<uint64_t(directory.VirtualAddress)+directory.Size) ||
               (expected && reinterpret_cast<const unsigned char*>(module)+rva!=expected) ||
               GetProcAddress(module,name)!=reinterpret_cast<FARPROC>(reinterpret_cast<uintptr_t>(module)+rva) ||
               !OwnerRange(result,image.data(),image.size(),ex.AddressOfNames+4*i,4) ||
               !OwnerRange(result,image.data(),image.size(),ex.AddressOfNameOrdinals+2*i,2) ||
               !OwnerRange(result,image.data(),image.size(),nameRva,size_t(end-(image.data()+nameRva))+1) ||
               !OwnerRange(result,image.data(),image.size(),ex.AddressOfFunctions+4*ordinal,4) ||
               !OwnerRange(result,image.data(),image.size(),rva,32,true))return false;
        }return found==1;
    };
    constexpr const char* apis[]={"ReShadeRegisterAddon","ReShadeUnregisterAddon",
        "ReShadeRegisterEvent","ReShadeUnregisterEvent","ReShadeCreateEffectRuntime",
        "ReShadeDestroyEffectRuntime","ReShadeGetConfigValue","ReShadeGetBasePath"};
    for(const auto name:apis)if(!resolve(name,nullptr))return false;
    if(!resolve(symbol,terminal) || std::strlen(symbol)>=result.symbol.size())return false;
    std::memcpy(result.symbol.data(),symbol,std::strlen(symbol)+1);
    HMODULE pinned=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(module),&pinned) || pinned!=module)return false;
    return true;
}
}
ExistingFactoryOwnerRef AcquireExistingFactoryOwner(const void* terminal,const char* symbol) noexcept
{
    if(!terminal || !symbol || (std::strcmp(symbol,"D3D11CreateDevice") &&
       std::strcmp(symbol,"D3D11CreateDeviceAndSwapChain") && std::strcmp(symbol,"D3D11On12CreateDevice")))return {};
    // Loader reference precedes all reads. Temporary release occurs here at the
    // installer boundary; retained receipts never invoke unload callbacks.
    HMODULE owner=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(terminal),&owner))return {};
    struct Release {HMODULE module;~Release(){FreeLibrary(module);}} release{owner};
    try {auto result=std::make_shared<ExistingFactoryOwner>();
        if(!ExistingCoreExports(owner,terminal,symbol,*result) || !ExistingFactoryOwnerIntact(result,terminal,symbol))return {};
        return result;
    }catch(...){return {};}
}
bool ExistingFactoryOwnerIntact(const ExistingFactoryOwnerRef& owner,const void* terminal,const char* symbol,
                               FactoryOwnerFreeze freeze,void* context) noexcept
{
    if(!owner || !owner->module || !terminal || !symbol || owner->terminal!=reinterpret_cast<uintptr_t>(terminal) ||
       std::strcmp(owner->symbol.data(),symbol) || !owner->count || owner->count>owner->ranges.size())return false;
    try {for(unsigned i=0;i<owner->count;++i){const auto& r=owner->ranges[i];MEMORY_BASIC_INFORMATION m{};
        std::array<unsigned char,320> bytes{};
        if(VirtualQuery(r.address,&m,sizeof(m))!=sizeof(m) || m.AllocationBase!=r.mapping.AllocationBase ||
           m.BaseAddress!=r.mapping.BaseAddress || m.RegionSize!=r.mapping.RegionSize || m.Protect!=r.mapping.Protect ||
           m.Type!=r.mapping.Type || m.State!=r.mapping.State || !OwnerRead(r.address,bytes.data(),r.size) ||
           std::memcmp(bytes.data(),r.bytes.data(),r.size) ||
           (freeze && !freeze(context,r.address,r.bytes.data(),r.size,r.mapping)))return false;
    }return true;}catch(...){return false;}
}
namespace
{
constexpr uint64_t SettingsMask = 15, EnabledBit = 8, SettingsShift = 4;
bool SettingsEnabled(uint64_t value) noexcept { return (value & EnabledBit) != 0; }
unsigned SettingsStage(uint64_t value) noexcept { return static_cast<unsigned>(value & 7); }
uint64_t SettingsGeneration(uint64_t value) noexcept { return value >> SettingsShift; }
// Diagnostic only: the order mirrors the existing natural D3D11 QI short-circuit.
enum class NaturalQiGateReject : uint8_t
{
    Ready, ApiNotD3D11, NativeDeviceMissing, CallbackThreadMismatch,
    DesiredSettingsMissing, SettingsMismatch, AppliedDisabled, Connecting,
    StageNotBeforeNr, GameNotGenshin, NrDisabled, NotFinishedPicture,
    BeforeSrEnabled, DeferredDlssEnabled
};
const char* NaturalQiGateRejectText(NaturalQiGateReject reason) noexcept
{
    switch (reason) {
    case NaturalQiGateReject::Ready: return "ready";
    case NaturalQiGateReject::ApiNotD3D11: return "api-not-d3d11";
    case NaturalQiGateReject::NativeDeviceMissing: return "native-device-missing";
    case NaturalQiGateReject::CallbackThreadMismatch: return "callback-thread-mismatch";
    case NaturalQiGateReject::DesiredSettingsMissing: return "desired-settings-missing";
    case NaturalQiGateReject::SettingsMismatch: return "desired-applied-mismatch";
    case NaturalQiGateReject::AppliedDisabled: return "applied-disabled";
    case NaturalQiGateReject::Connecting: return "connecting";
    case NaturalQiGateReject::StageNotBeforeNr: return "stage-not-before-nr";
    case NaturalQiGateReject::GameNotGenshin: return "game-not-genshin";
    case NaturalQiGateReject::NrDisabled: return "nr-disabled";
    case NaturalQiGateReject::NotFinishedPicture: return "not-finished-picture";
    case NaturalQiGateReject::BeforeSrEnabled: return "before-sr-enabled";
    case NaturalQiGateReject::DeferredDlssEnabled: return "deferred-dlss-enabled";
    }
    return "UNKNOWN";
}
struct SelectionObservation
{
    uint64_t generation = 0, lifetime = 0, snapshots = 0, graphBuilds = 0;
    bool captureReady = false, selectionReady = false;
    bool presetKnown = false, globalKnown = false, globalEnabled = false, countsKnown = false;
    size_t total = 0, enabled = 0;
    Selection::CaptureFacts capture;
    Selection::ApplyFacts applied;
    // Current complete live-state observations only. Detect equal-count switches
    // enabled counts without another SDK enumeration or per-frame allocation.
    std::array<uint64_t, (Selection::MaxItems + 63) / 64> enabledBits{};
    bool operator==(const SelectionObservation&) const = default;
};
// R100 observations only: no candidate tuple or first-false result authorizes a donor.
enum class BeforeSrDonorReject : unsigned
{
    None, SettingsMissing, SettingsDisabled, StageMismatch, AppliedSettingsMismatch, DesiredSettingsMismatch,
    NaturalStatusIncomplete, EventCacheExhausted, EventMutationExhausted, NotLive, RuntimeMissing,
    RuntimeIdentityMismatch, SdkDeviceMissing, SdkQueueMissing, NativeDeviceMissing, NativeQueueMissing,
    ApiUnsupported, LifetimeMissing, LifetimeExhausted, EpochMissing, EpochExhausted, ThreadMismatch,
    FrameInvalid, CopiedSerial, FrameCannotArm, FrameNotArmed, SettingsEpochNotReady, SelectionInvalid,
    SelectionRefresh, SelectionFilesMissing, SelectionConfigEmpty, SelectionPresetEmpty,
    SelectionRuntimeMismatch, SelectionLifetimeMismatch, GraphUnknown, GraphInvalid, NoEnabledTechniques,
    EventRuntimeMismatch, EventRevisionMissing, EventRevisionExhausted, SelectionRevisionMismatch,
    SnapshotsMissing, SnapshotsExhausted, GraphBuildsExhausted
};
const char* BeforeSrDonorRejectText(BeforeSrDonorReject reason) noexcept
{
    switch (reason) {
    case BeforeSrDonorReject::None: return "none";
    case BeforeSrDonorReject::SettingsMissing: return "settings-missing";
    case BeforeSrDonorReject::SettingsDisabled: return "settings-disabled";
    case BeforeSrDonorReject::StageMismatch: return "settings-stage-not-before-sr";
    case BeforeSrDonorReject::AppliedSettingsMismatch: return "applied-settings-mismatch";
    case BeforeSrDonorReject::DesiredSettingsMismatch: return "desired-settings-mismatch";
    case BeforeSrDonorReject::NaturalStatusIncomplete: return "natural-status-incomplete";
    case BeforeSrDonorReject::EventCacheExhausted: return "event-cache-exhausted";
    case BeforeSrDonorReject::EventMutationExhausted: return "event-mutation-exhausted";
    case BeforeSrDonorReject::NotLive: return "natural-not-live";
    case BeforeSrDonorReject::RuntimeMissing: return "natural-runtime-missing";
    case BeforeSrDonorReject::RuntimeIdentityMismatch: return "natural-runtime-identity-mismatch";
    case BeforeSrDonorReject::SdkDeviceMissing: return "natural-sdk-device-missing";
    case BeforeSrDonorReject::SdkQueueMissing: return "natural-sdk-queue-missing";
    case BeforeSrDonorReject::NativeDeviceMissing: return "natural-native-device-missing";
    case BeforeSrDonorReject::NativeQueueMissing: return "natural-native-queue-missing";
    case BeforeSrDonorReject::ApiUnsupported: return "natural-api-unsupported";
    case BeforeSrDonorReject::LifetimeMissing: return "natural-lifetime-missing";
    case BeforeSrDonorReject::LifetimeExhausted: return "natural-lifetime-exhausted";
    case BeforeSrDonorReject::EpochMissing: return "natural-frame-epoch-missing";
    case BeforeSrDonorReject::EpochExhausted: return "natural-frame-epoch-exhausted";
    case BeforeSrDonorReject::ThreadMismatch: return "natural-thread-mismatch";
    case BeforeSrDonorReject::FrameInvalid: return "natural-frame-invalid";
    case BeforeSrDonorReject::CopiedSerial: return "natural-copied-serial";
    case BeforeSrDonorReject::FrameCannotArm: return "natural-frame-cannot-arm";
    case BeforeSrDonorReject::FrameNotArmed: return "natural-frame-not-armed";
    case BeforeSrDonorReject::SettingsEpochNotReady: return "settings-epoch-not-ready";
    case BeforeSrDonorReject::SelectionInvalid: return "selection-invalid";
    case BeforeSrDonorReject::SelectionRefresh: return "selection-refresh";
    case BeforeSrDonorReject::SelectionFilesMissing: return "selection-files-missing";
    case BeforeSrDonorReject::SelectionConfigEmpty: return "selection-config-empty";
    case BeforeSrDonorReject::SelectionPresetEmpty: return "selection-preset-empty";
    case BeforeSrDonorReject::SelectionRuntimeMismatch: return "selection-runtime-mismatch";
    case BeforeSrDonorReject::SelectionLifetimeMismatch: return "selection-lifetime-mismatch";
    case BeforeSrDonorReject::GraphUnknown: return "selection-graph-unknown";
    case BeforeSrDonorReject::GraphInvalid: return "selection-graph-invalid";
    case BeforeSrDonorReject::NoEnabledTechniques: return "selection-has-enabled-techniques-false";
    case BeforeSrDonorReject::EventRuntimeMismatch: return "event-runtime-mismatch";
    case BeforeSrDonorReject::EventRevisionMissing: return "event-revision-missing";
    case BeforeSrDonorReject::EventRevisionExhausted: return "event-revision-exhausted";
    case BeforeSrDonorReject::SelectionRevisionMismatch: return "selection-event-revision-mismatch";
    case BeforeSrDonorReject::SnapshotsMissing: return "selection-snapshots-missing";
    case BeforeSrDonorReject::SnapshotsExhausted: return "selection-snapshots-exhausted";
    case BeforeSrDonorReject::GraphBuildsExhausted: return "selection-graph-builds-exhausted";
    }
    return "UNKNOWN";
}
enum BeforeSrDonorRead : unsigned
{
    DonorAppliedRead = 1, DonorDesiredRead = 2, DonorStatusRead = 4, DonorCacheRead = 8,
    DonorMutationRead = 16, DonorThreadRead = 32, DonorCanArmRead = 64, DonorSettingsEpochRead = 128,
    DonorEnabledTechniquesRead = 256, DonorEventWatchRead = 512,
    DonorFilesRead = 1024, DonorConfigRead = 2048, DonorPresetRead = 4096
};
struct BeforeSrDonorCandidate
{
    unsigned slot = 0, reads = 0;
    BeforeSrDonorReject firstFalse = BeforeSrDonorReject::None;
    bool admitted = false, live = false, armable = true;
    uint64_t settings = 0, applied = 0, desired = 0;
    bool statusIncomplete = false, cacheExhausted = false, mutationExhausted = false;
    uint64_t runtime = 0, lifetime = 0, copiedSerial = 0, sourceSettings = 0, settingsBarrier = 0;
    Identity identity;
    NaturalFrame frame;
    DWORD ownerThread = 0, currentThread = 0;
    bool canArm = false, settingsEpochReady = false, hasEnabledTechniques = false;
    bool selectionValid = false, selectionRefresh = false, selectionEnabled = false, selectionFiles = false;
    bool configEmpty = true, presetEmpty = true, graphKnown = false, graphValid = false;
    uint64_t selectionRuntime = 0, selectionLifetime = 0, revision = 0, snapshots = 0, graphBuilds = 0;
    Selection::EventState events;
};
struct BeforeSrDonorDiagnostics
{
    // Same finite capacity as State::natural. Empty slots never occupy a tuple.
    std::array<BeforeSrDonorCandidate, 32> candidates{};
    unsigned count = 0, nonempty = 0, live = 0, validSelection = 0, boundary = 0;
    bool truncated = false;
};
struct BeforeSrDonorChange
{
    uint64_t runtime = 0, lifetime = 0;
    BeforeSrDonorReject firstFalse = BeforeSrDonorReject::None;
    bool live = false, selectionValid = false;
    bool operator==(const BeforeSrDonorChange&) const = default;
};
struct StageObservation
{
    Reason reason = Reason::Disabled;
    SubmissionState submission = SubmissionState::Untouched;
    uint64_t stream = 0, generation = 0, naturalRuntime = 0, naturalLifetime = 0;
    bool naturalSuppressed = false;
    AssociationDiagnostics::Snapshot association;
    unsigned associationExit = 0;
    NaturalAssociation::Predicate mapping = NaturalAssociation::Predicate::Missing;
    uint64_t mappedOwner = 0;
    ColorMode colorMode = ColorMode::TransferQualified;
    NativeColorAdmission nativeColor = NativeColorAdmission::NotRequested;
    unsigned donorCount = 0;
    uint64_t donorRevision = 0, donorSnapshots = 0;
    bool depthBindingAtRender = false;
    unsigned persistentOwnedPhase = 0; // status changes, not producer-frame churn
    unsigned donorDiagnosticBoundary = 0;
    std::array<BeforeSrDonorChange, 32> donorCandidateChanges{};
    native::NativePictureColorObservation presentColor;
    bool operator==(const StageObservation&) const = default;
};
struct DomainCache
{
    DeviceDomain::Marker marker;
    DeviceDomain::Witness witness;
    uint64_t owner=0, buffer=0, lifetime=0;
};
// R061 cache metadata is fixed-size. Retirement is published only AFTER the
// SDK views and real native color owner have physically left pool custody.
struct DirectReceipt { std::atomic<OwnedRuntime::Retirement> state{OwnedRuntime::Retirement::Pending}; };
struct DirectKey
{
    uint64_t chain = 0, chainLifetime = 0, chainEpoch = 0;
    uint64_t stream = 0, generation = 0, naturalRuntime = 0, naturalLifetime = 0;
    uint64_t runtime = 0, sdkDevice = 0, sdkQueue = 0, nativeDevice = 0, nativeQueue = 0, resource = 0;
    D3D12_RESOURCE_DESC native{};
    reshade::api::resource_desc sdk{};
    reshade::api::format linear{}, srgb{}; // subresource zero / mip zero / one layer only
    bool operator==(const DirectKey& x) const noexcept
    {
        return chain == x.chain && chainLifetime == x.chainLifetime && chainEpoch == x.chainEpoch &&
            stream == x.stream && generation == x.generation && naturalRuntime == x.naturalRuntime &&
            naturalLifetime == x.naturalLifetime && runtime == x.runtime && sdkDevice == x.sdkDevice &&
            sdkQueue == x.sdkQueue && nativeDevice == x.nativeDevice && nativeQueue == x.nativeQueue &&
            resource == x.resource && linear == x.linear && srgb == x.srgb &&
            native.Dimension == x.native.Dimension && native.Alignment == x.native.Alignment &&
            native.Width == x.native.Width && native.Height == x.native.Height &&
            native.DepthOrArraySize == x.native.DepthOrArraySize && native.MipLevels == x.native.MipLevels &&
            native.Format == x.native.Format && native.SampleDesc.Count == x.native.SampleDesc.Count &&
            native.SampleDesc.Quality == x.native.SampleDesc.Quality && native.Layout == x.native.Layout &&
            native.Flags == x.native.Flags && sdk.type == x.sdk.type && sdk.heap == x.sdk.heap &&
            sdk.usage == x.sdk.usage && sdk.flags == x.sdk.flags &&
            sdk.texture.width == x.sdk.texture.width && sdk.texture.height == x.sdk.texture.height &&
            sdk.texture.depth_or_layers == x.sdk.texture.depth_or_layers &&
            sdk.texture.levels == x.sdk.texture.levels && sdk.texture.samples == x.sdk.texture.samples &&
            sdk.texture.format == x.sdk.texture.format;
    }
};
struct DirectSlot
{
    DirectKey key;
    std::shared_ptr<OwnedRuntime::Resource> owner;
    std::shared_ptr<DirectReceipt> receipt;
    bool occupied = false, failed = false, revoked = false;
};
struct DirectEntry
{
    OwnedRuntime::EntryReceipt entry;
    std::array<DirectSlot, 7> slots; // reserve pool slot eight for the old scratch
};
// A lock-free observation closes first-use-vs-resize races without refusing a
// scratch/disabled resize or consulting the GPU. Only Owners/Recording may add
// retirement constraints. High bits count scopes through every real/early exit.
constexpr uint64_t DirectRecording = 1, DirectOwners = 2, DirectClosed = 4, DirectResizeStep = 16;
struct DirectChain
{
    std::atomic<uint64_t> chain{0}, wrapper{0}, access{0}, lifetime{0};
    std::atomic<unsigned> resourceOnlyResize{0};
    uint64_t epoch = 1; // executionMutex only
    bool draining = false;
};
struct State
{
    std::mutex mutex;
    std::recursive_mutex executionMutex; // callbacks/destruction and stage calls serialize
    struct Natural
    {
        reshade::api::effect_runtime* runtime = nullptr;
        Identity id;
        bool live = false;
        uint64_t backbuffers[8]{};
        NaturalFrame frame;
        std::array<DomainCache,8> domains{}; // POD only, no BB/view/SDK ownership
        bool domainBuffersPending=false;
        uint64_t lifetime = 0, sourceSerial = 0, copiedSerial = 0, sourceSettings = 0;
        // Callback-local D3D11 COM identity, stored only as an immutable scalar.
        NaturalAssociation::CanonicalDeviceObservation nativeCanonical;
        DWORD canonicalThread = 0;
        // Pre-QI scalar diagnostics reset with this exact natural lifetime on init.
        uint8_t naturalQiGateReports = 0;
        NaturalQiGateReject naturalQiGateLastReason = NaturalQiGateReject::Ready;
        // Only actual recording/ticket admission participates in a transition.
        // These scalars belong to this exact runtime/lifetime (reset on init).
        struct SettingsUse {
            uint64_t settings = 0, stream = 0, generation = 0, epoch = 0;
        } settingsUse;
        uint64_t settingsBarrierEpoch = 0;
        DWORD thread = 0;
        Selection::Source selection;
        // Published only alongside existing Capture calls, under executionMutex.
        SelectionObservation selectionStatus;
        std::array<char, 32768> presetPath{};
        bool captureObserved = false;
        uint64_t idleSelectionMutation = 0, idleSelectionRevision = 0; // Complete live zero-selection Capture only.
        Diagnostics::ChangeReports<SelectionObservation> selectionReports;
    } natural[32]{};
    std::shared_ptr<OwnedRuntime::Resource> scratch;
    uint64_t scratchGeneration = 0;
    reshade::api::effect_runtime* scratchRuntime = nullptr;
    std::array<DirectEntry, OwnedRuntime::Capacity> directEntries;
    std::array<DirectChain, 32> directChains;
    uint64_t nextDirectLifetime = 0;
    Depth::Semantics::State depthSemantics; // idle private-runtime projection only
    Registry registry;
    // Sticky observation loss, not a render gate. Never infer a unique source
    // after a natural runtime could not be tracked in the bounded table.
    bool naturalStatusIncomplete = false;
    LatestStageStatus latestStage;
    AssociationDiagnostics::Callbacks naturalCallbacks;
    Diagnostics::ChangeReports<AssociationDiagnostics::CallbackSnapshot> naturalCallbackReports;
    struct MarkerRecord {DeviceDomain::Marker marker;uint64_t owner=0,buffer=0;};
    // Lifetime tags identify resources through their own public private-data
    // property. These bounded scalar records never retain a backbuffer.
    std::array<MarkerRecord,1024> domainMarkers{};
    uint64_t nextDomainMarker=0, nextDomainStamp=0;
    std::atomic<bool> domainSubscriptionRequested{false};
    uint64_t stageAttempts = 0, presentAttempts = 0; // Observations only; never retry budgets.
    unsigned canonicalStageReports = 0; // One diagnostic window, at most eight lines total.
    CoreApi api;
    HMODULE coreModule = nullptr;
    OwnedRuntime::Binding ownedBinding{};
    Microsoft::WRL::ComPtr<ID3D12Device> ownedDevice;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> ownedQueue;
    Microsoft::WRL::ComPtr<IDXGIFactory2> ownedFactory;
    std::string ownedConfigPath; // Host test source; product config comes from the natural runtime.
    std::string selectionConfigPath;
    reshade::api::effect_runtime* ownedNatural = nullptr;
    uint64_t ownedBindingOwner = 0, ownedBindingGeneration = 0;
    NaturalAssociation::SelectionDonor ownedSelectionDonor; // configuration identity only
    std::atomic<bool> started{false}, enabled{false}, accepted{false};
    std::atomic<unsigned> stage{2}; // Render-owner applied, not UI desire.
    std::atomic<uint64_t> desiredSettings{0}, appliedSettings{0};
    std::atomic<uint64_t> observationPublication{1}; // UINT64_MAX permanently denies lightweight demand.
    uint64_t observationSelectionRevision = 1; // executionMutex; BEFORE requests invalidate zero-selection evidence.
    std::atomic<HMODULE> self{nullptr};
    std::atomic<bool> connectStarted{false}, connecting{false};
    // Latest resolved admission attempt only, never a process-wide drain flag.
    struct EpochWait {
        uint64_t settings = 0, stream = 0, generation = 0, runtime = 0, lifetime = 0;
    } latestEpochWait;
    size_t renderOwners = 0; // Includes nested/cross-thread owner intervals.
    std::atomic<Reason> core{Reason::Disabled};
    // Fixed stage x output-kind streams, never partitioned by runtime/endpoint
    // address or reason. Alternating failures remain bounded to one/second.
    std::array<std::array<Diagnostics::ChangeReports<StageObservation>, 3>, 3> stageReports{};
    // Fixed site x stage x output streams; endpoint churn cannot grow storage
    // or bypass the one-second minimum / thirty-second stable heartbeat.
    std::array<std::array<std::array<Diagnostics::ChangeReports<DeviceValidation::Snapshot>, 3>, 3>,
        static_cast<unsigned>(DeviceValidation::Site::Count)> deviceReports{};
    OwnedRuntime::DiagnosticReports ownedReports{}; // Fixed-size, protected by mutex.
    Diagnostics::ExecutionReports executionReports; // Fixed scalar streams; logging only.
    std::atomic<uint64_t> callbacks{0}, overflow{0};
    std::atomic<uint64_t> renderCalls{0}, renderRecorded{0}, renderUnverified{0}, submitFailures{0};
    struct Transaction
    {
        uint64_t token = 0;
        uint64_t nativeQueue = 0, nativeList = 0, runtimeGeneration = 0;
        OwnedRuntime::Lease lease;
        OwnedRuntime::EntryReceipt poolEntry;
        reshade::api::effect_runtime* sdkRuntime = nullptr;
        reshade::api::device* sdkDevice = nullptr;
        reshade::api::command_queue* sdkQueue = nullptr;
        reshade::api::command_list* sdkList = nullptr;
        reshade::api::resource_view rtv = { 0 }, rtvSrgb = { 0 };
        std::shared_ptr<OwnedRuntime::Resource> rtvOwner;
        OwnedRuntime::Completion completion;
        uint32_t inFlight = 0;
        bool reserved = false, recorded = false, signalPending = false, submitted = false, signalFailed = false, orphaned = false;
    } transactions[32]{};
    uint64_t nextFenceValue = 0, nextTransactionToken = 0, nextNaturalLifetime = 0;
};
State& Get() { static State* state = new State; return *state; } // Process lifetime; no loader-lock destructors.
// Extend the existing demand token, not GPU admission. Mutators revoke before
// publishing owners; the claim uses existing locks, Current only atomic reads.
void RevokeObservationDemand() noexcept
{
    auto& publication = Get().observationPublication;
    auto previous = publication.load(std::memory_order_relaxed);
    while (previous != UINT64_MAX && !publication.compare_exchange_weak(previous, previous + 1,
        std::memory_order_acq_rel, std::memory_order_relaxed)) {}
}
// Natural effects remain independent when the product is effectively off.
// Only enabled-idle claims need the additional selection/execution revocations.
void RevokeEnabledObservationDemand() noexcept
{
    if (Get().enabled.load(std::memory_order_acquire)) RevokeObservationDemand();
}
void ObservationSelectionRequested() noexcept
{
    RevokeEnabledObservationDemand();
    auto& revision = Get().observationSelectionRevision; // existing executionMutex held
    if (revision != UINT64_MAX) ++revision;
}
bool ColorObservationIdle() noexcept;
struct OwnedRtvBundle final : OwnedRuntime::Resource
{
    reshade::api::resource_view linear{ 0 }, srgb{ 0 };
    reshade::api::resource color{ 0 };
    Depth::Cache depth; // same owned runtime and completion lifetime as the color target
    Microsoft::WRL::ComPtr<ID3D12Resource> externalColor;
    reshade::api::device* directDevice = nullptr;
    std::shared_ptr<DirectReceipt> retirement;
    bool GpuObjectsReleased() const noexcept override
    { return !retirement || retirement->state.load(std::memory_order_acquire) == OwnedRuntime::Retirement::Ready; }

    void DestroyGpuObjects(reshade::api::device* device) noexcept override
    {
        if (retirement) {
            if (retirement->state.load(std::memory_order_acquire) == OwnedRuntime::Retirement::Unknown) return;
            if (!device || device != directDevice || depth.quarantined || (depth.srv.handle && depth.device != device)) {
                retirement->state.store(OwnedRuntime::Retirement::Unknown, std::memory_order_release); return;
            }
            try {
                // Unlike the legacy best-effort scratch destructor, a failed
                // direct view destroy MUST retain the real resource and pool
                // object. Never SDK-destroy an external swapchain resource.
                if (depth.srv.handle) { device->destroy_resource_view(depth.srv); depth.srv = {0}; }
                depth.resource.Reset(); depth.device = nullptr; depth.typedFormat = DXGI_FORMAT_UNKNOWN;
                if (linear.handle) { device->destroy_resource_view(linear); linear = {0}; }
                if (srgb.handle) { device->destroy_resource_view(srgb); srgb = {0}; }
                externalColor.Reset(); color = {0};
                retirement->state.store(OwnedRuntime::Retirement::Ready, std::memory_order_release);
            } catch (...) { retirement->state.store(OwnedRuntime::Retirement::Unknown, std::memory_order_release); }
            return;
        }
        if (!device) return;
        depth.Destroy(device);
        try { if (linear.handle) device->destroy_resource_view(linear); } catch (...) {}
        try { if (srgb.handle) device->destroy_resource_view(srgb); } catch (...) {}
        try { if (color.handle) device->destroy_resource(color); } catch (...) {}
        linear = { 0 }; srgb = { 0 }; color = { 0 };
    }
};
// Caller must already own the transaction slot.  Lease release is deliberately
// excluded: OwnedRuntime::Lease::Release() takes State::mutex, so releasing a
// lease while that mutex is held deadlocks the first completed NR transaction.
void ClearTransaction(State::Transaction& transaction) noexcept
{
    transaction.token = 0;
    transaction.poolEntry = {};
    transaction.nativeQueue = transaction.nativeList = transaction.runtimeGeneration = 0;
    transaction.sdkRuntime = nullptr; transaction.sdkDevice = nullptr;
    transaction.sdkQueue = nullptr; transaction.sdkList = nullptr;
    transaction.rtv = { 0 }; transaction.rtvSrgb = { 0 }; transaction.completion = {};
    transaction.rtvOwner.reset();
    transaction.inFlight = 0;
    transaction.reserved = transaction.recorded = transaction.signalPending = transaction.submitted =
        transaction.signalFailed = transaction.orphaned = false;
}
thread_local bool stageActive = false;
thread_local bool inCallback = false;
thread_local bool ownedOperation = false;
struct OwnedOperation
{
    bool previous = ownedOperation;
    OwnedOperation() noexcept { ownedOperation = true; }
    ~OwnedOperation() { ownedOperation = previous; }
};
struct CallbackGuard
{
    std::unique_lock<std::recursive_mutex> lifetime{Get().executionMutex};
    bool entered = false;
    CallbackGuard() noexcept { RevokeEnabledObservationDemand(); if (!inCallback && Get().accepted.load(std::memory_order_acquire)) { inCallback = true; entered = true; ++Get().callbacks; } }
    ~CallbackGuard() { if (entered) inCallback = false; }
};
uintptr_t Key(const void* pointer) noexcept { return reinterpret_cast<uintptr_t>(pointer); }
bool OwnedStageRequested() noexcept
{
    try {
#ifndef HOYOSHADE_STAGE_TEST
    static const auto game = [] {
        const auto name = Util::ExePath().filename().wstring();
        return GameRouting::IdentifyExecutable(name.c_str());
    }();
    const auto& config = *Config::Instance();
    return game == GameRouting::Identity::Genshin &&
        (Selected(Stage::BeforeSr) || (Selected(Stage::BeforeNr) &&
         config.DlssNrEnabled.value_or_default() && config.DlssNrFinishedPicture.value_or_default() &&
         !config.DlssNrRunBeforeSr.value_or_default() && !config.DlssNrDeferredDlss.value_or_default()));
#else
    return false; // existing standalone hosts retain their legacy path
#endif
    } catch (...) { return false; }
}
uint64_t OwnedStageSettings() noexcept
{
    const auto desired = Get().desiredSettings.load(std::memory_order_acquire);
    return desired && desired == Get().appliedSettings.load(std::memory_order_acquire) &&
        OwnedStageRequested() ? desired : 0;
}
// A same-call TLS watch catches real donor callbacks even when the existing
// ownedOperation/CallbackGuard intentionally excludes natural bookkeeping.
// It never selects a donor and has no cross-frame admission authority.
struct BeforeSrDonorCall;
thread_local BeforeSrDonorCall* activeBeforeSrDonor = nullptr;
struct BeforeSrDonorCall
{
    NaturalAssociation::SelectionDonor token;
    State::Natural* natural = nullptr;
    BeforeSrDonorCall* previous = activeBeforeSrDonor;
    bool revoked = false, suppressing = false;
    BeforeSrDonorCall(const NaturalAssociation::SelectionDonor* value, State::Natural& source) noexcept {
        if (value) { token = *value; natural = &source; activeBeforeSrDonor = this; }
    }
    ~BeforeSrDonorCall() { if (natural) activeBeforeSrDonor = previous; }
    BeforeSrDonorCall(const BeforeSrDonorCall&) = delete;
    BeforeSrDonorCall& operator=(const BeforeSrDonorCall&) = delete;
    static bool Current(const NaturalAssociation::SelectionDonor& expected) noexcept {
        return !activeBeforeSrDonor || (!activeBeforeSrDonor->revoked && activeBeforeSrDonor->token == expected);
    }
    static void Observe(reshade::api::effect_runtime* runtime, AssociationDiagnostics::Event event) noexcept {
        auto* call = activeBeforeSrDonor;
        if (!call || Key(runtime) != call->token.runtime) return;
        // Only begin/finish emitted by our bounded empty-RTV suppression are
        // expected. A technique or Present here invalidates this selection.
        if (call->suppressing && (event == AssociationDiagnostics::Event::Begin || event == AssociationDiagnostics::Event::Finish)) return;
        call->revoked = true;
        Selection::Mutated(); // stop any reentrant Selection::Apply before its next SDK call
        if (call->natural->runtime == runtime && call->natural->lifetime == call->token.lifetime) {
            call->natural->frame.Invalidate();
            Selection::InvalidateNatural(call->natural->selection, false);
        }
    }
};
// executionMutex only. Resolving one endpoint never depends on another
// runtime's epoch, thread, CanArm window or outstanding source ticket.
bool SettingsEpochReady(const State::Natural& natural, uint64_t settings) noexcept
{
    if (natural.copiedSerial && natural.sourceSettings != settings) return false;
    return !natural.settingsBarrierEpoch ||
        (!natural.frame.invalid && natural.frame.epoch != UINT64_MAX &&
         natural.frame.epoch > natural.settingsBarrierEpoch);
}
bool AdmitSettingsForSource(State::Natural& natural, uint64_t stream, uint64_t generation) noexcept
{
    const auto settings = Get().appliedSettings.load(std::memory_order_acquire);
    if (!SettingsEpochReady(natural, settings)) {
        Get().latestEpochWait = {settings, stream, generation, Key(natural.runtime), natural.lifetime};
        return false;
    }
    return true;
}
void RememberSettingsUse(State::Natural& natural, uint64_t stream, uint64_t generation) noexcept
{
    // Only actual new-generation work/ticket replaces the old barrier.
    // A rejected/untouched admission must not discard inherited protection.
    natural.settingsBarrierEpoch = 0;
    natural.settingsUse = {Get().appliedSettings.load(std::memory_order_acquire),
        stream, generation, natural.frame.epoch};
}
// BeforeSR is a selection association, not a device/backbuffer association.
// All reads occur under executionMutex. No SDK call, file IO, capture, GPU
// object borrowing or cached admission in this resolver. Every boundary scans
// the bounded table again, so a newly eligible second donor revokes the call.
bool ReadBeforeSrSelectionDonor(const State::Natural& natural, uint64_t settings,
    NaturalAssociation::SelectionDonor& token, bool armable = true,
    BeforeSrDonorCandidate* diagnostic = nullptr) noexcept
{
    token = {};
    if (diagnostic) {
        *diagnostic = {};
        diagnostic->settings = settings; diagnostic->armable = armable;
        // Cached scalar tuple only; it does not evaluate later predicates.
        // Callable/atomic results below are UNKNOWN unless their read bit is set.
        diagnostic->live = natural.live; diagnostic->runtime = Key(natural.runtime);
        diagnostic->identity = natural.id; diagnostic->lifetime = natural.lifetime;
        diagnostic->frame = natural.frame; diagnostic->ownerThread = natural.thread;
        diagnostic->copiedSerial = natural.copiedSerial; diagnostic->sourceSettings = natural.sourceSettings;
        diagnostic->settingsBarrier = natural.settingsBarrierEpoch;
        const auto& selection = natural.selection;
        diagnostic->selectionValid = selection.valid; diagnostic->selectionRefresh = selection.refresh;
        diagnostic->selectionEnabled = selection.enabled;
        diagnostic->selectionRuntime = Key(selection.naturalRuntime); diagnostic->selectionLifetime = selection.naturalGeneration;
        diagnostic->graphKnown = selection.graphKnown; diagnostic->graphValid = selection.graphValid;
        diagnostic->revision = selection.naturalRevision; diagnostic->snapshots = selection.snapshots;
        diagnostic->graphBuilds = selection.graphBuilds;
    }
    const auto reject = [diagnostic](BeforeSrDonorReject reason) noexcept {
        if (diagnostic) diagnostic->firstFalse = reason;
        return false;
    };
    // Preserve the original left-to-right short circuit, including the exact
    // positions/counts of atomic loads, CacheEvents, thread, CanArm and Watch.
    if (!settings) return reject(BeforeSrDonorReject::SettingsMissing);
    if (!SettingsEnabled(settings)) return reject(BeforeSrDonorReject::SettingsDisabled);
    if (SettingsStage(settings) != static_cast<unsigned>(Stage::BeforeSr)) return reject(BeforeSrDonorReject::StageMismatch);
    const auto applied = Get().appliedSettings.load(std::memory_order_acquire);
    if (diagnostic) { diagnostic->applied = applied; diagnostic->reads |= DonorAppliedRead; }
    if (applied != settings) return reject(BeforeSrDonorReject::AppliedSettingsMismatch);
    const auto desired = Get().desiredSettings.load(std::memory_order_acquire);
    if (diagnostic) { diagnostic->desired = desired; diagnostic->reads |= DonorDesiredRead; }
    if (desired != settings) return reject(BeforeSrDonorReject::DesiredSettingsMismatch);
    const bool statusIncomplete = Get().naturalStatusIncomplete;
    if (diagnostic) { diagnostic->statusIncomplete = statusIncomplete; diagnostic->reads |= DonorStatusRead; }
    if (statusIncomplete) return reject(BeforeSrDonorReject::NaturalStatusIncomplete);
    const bool cacheExhausted = Selection::CacheEvents().exhausted;
    if (diagnostic) { diagnostic->cacheExhausted = cacheExhausted; diagnostic->reads |= DonorCacheRead; }
    if (cacheExhausted) return reject(BeforeSrDonorReject::EventCacheExhausted);
    const bool mutationExhausted = Selection::CacheEvents().mutationExhausted;
    if (diagnostic) { diagnostic->mutationExhausted = mutationExhausted; diagnostic->reads |= DonorMutationRead; }
    if (mutationExhausted) return reject(BeforeSrDonorReject::EventMutationExhausted);
    if (!natural.live) return reject(BeforeSrDonorReject::NotLive);
    if (!natural.runtime) return reject(BeforeSrDonorReject::RuntimeMissing);
    if (natural.id.runtime != Key(natural.runtime)) return reject(BeforeSrDonorReject::RuntimeIdentityMismatch);
    if (!natural.id.sdkDevice) return reject(BeforeSrDonorReject::SdkDeviceMissing);
    if (!natural.id.sdkQueue) return reject(BeforeSrDonorReject::SdkQueueMissing);
    if (!natural.id.nativeDevice) return reject(BeforeSrDonorReject::NativeDeviceMissing);
    if (!natural.id.nativeQueue) return reject(BeforeSrDonorReject::NativeQueueMissing);
    if (natural.id.api != 0xb000 && natural.id.api != 0xc000) return reject(BeforeSrDonorReject::ApiUnsupported);
    if (!natural.lifetime) return reject(BeforeSrDonorReject::LifetimeMissing);
    if (natural.lifetime == UINT64_MAX) return reject(BeforeSrDonorReject::LifetimeExhausted);
    if (!natural.frame.epoch) return reject(BeforeSrDonorReject::EpochMissing);
    if (natural.frame.epoch == UINT64_MAX) return reject(BeforeSrDonorReject::EpochExhausted);
    const auto currentThread = GetCurrentThreadId();
    if (diagnostic) { diagnostic->currentThread = currentThread; diagnostic->reads |= DonorThreadRead; }
    if (natural.thread != currentThread) return reject(BeforeSrDonorReject::ThreadMismatch);
    if (natural.frame.invalid) return reject(BeforeSrDonorReject::FrameInvalid);
    if (natural.copiedSerial) return reject(BeforeSrDonorReject::CopiedSerial);
    if (armable) {
        const bool canArm = natural.frame.CanArm();
        if (diagnostic) { diagnostic->canArm = canArm; diagnostic->reads |= DonorCanArmRead; }
        if (!canArm) return reject(BeforeSrDonorReject::FrameCannotArm);
    } else if (!natural.frame.armed) return reject(BeforeSrDonorReject::FrameNotArmed);
    const bool settingsEpochReady = SettingsEpochReady(natural, settings);
    if (diagnostic) { diagnostic->settingsEpochReady = settingsEpochReady; diagnostic->reads |= DonorSettingsEpochRead; }
    if (!settingsEpochReady) return reject(BeforeSrDonorReject::SettingsEpochNotReady);
    const auto& selection = natural.selection;
    const auto events = Selection::Watch(natural.runtime).Read();
    if (diagnostic) { diagnostic->events = events; diagnostic->reads |= DonorEventWatchRead; }
    if (!selection.valid) return reject(BeforeSrDonorReject::SelectionInvalid);
    if (selection.refresh) return reject(BeforeSrDonorReject::SelectionRefresh);
    const bool filesPresent = static_cast<bool>(selection.files);
    if (diagnostic) { diagnostic->selectionFiles = filesPresent; diagnostic->reads |= DonorFilesRead; }
    if (!filesPresent) return reject(BeforeSrDonorReject::SelectionFilesMissing);
    const bool configEmpty = selection.config.empty();
    if (diagnostic) { diagnostic->configEmpty = configEmpty; diagnostic->reads |= DonorConfigRead; }
    if (configEmpty) return reject(BeforeSrDonorReject::SelectionConfigEmpty);
    const bool presetEmpty = selection.preset.empty();
    if (diagnostic) { diagnostic->presetEmpty = presetEmpty; diagnostic->reads |= DonorPresetRead; }
    if (presetEmpty) return reject(BeforeSrDonorReject::SelectionPresetEmpty);
    if (selection.naturalRuntime != natural.runtime) return reject(BeforeSrDonorReject::SelectionRuntimeMismatch);
    if (selection.naturalGeneration != natural.lifetime) return reject(BeforeSrDonorReject::SelectionLifetimeMismatch);
    if (!selection.graphKnown) return reject(BeforeSrDonorReject::GraphUnknown);
    if (!selection.graphValid) return reject(BeforeSrDonorReject::GraphInvalid);
    const bool hasEnabledTechniques = Selection::HasEnabledTechniques(selection);
    if (diagnostic) { diagnostic->hasEnabledTechniques = hasEnabledTechniques; diagnostic->reads |= DonorEnabledTechniquesRead; }
    if (!hasEnabledTechniques) return reject(BeforeSrDonorReject::NoEnabledTechniques);
    if (events.runtime != natural.runtime) return reject(BeforeSrDonorReject::EventRuntimeMismatch);
    if (!events.revision) return reject(BeforeSrDonorReject::EventRevisionMissing);
    if (events.revision == UINT64_MAX) return reject(BeforeSrDonorReject::EventRevisionExhausted);
    if (selection.naturalRevision != events.revision) return reject(BeforeSrDonorReject::SelectionRevisionMismatch);
    if (!selection.snapshots) return reject(BeforeSrDonorReject::SnapshotsMissing);
    if (selection.snapshots == UINT64_MAX) return reject(BeforeSrDonorReject::SnapshotsExhausted);
    if (selection.graphBuilds == UINT64_MAX) return reject(BeforeSrDonorReject::GraphBuildsExhausted);
    token = {Key(natural.runtime), natural.lifetime, natural.frame.epoch, settings,
        events.revision, events.topology, selection.snapshots, selection.graphBuilds,
        Key(selection.files.get()), natural.thread};
    if (diagnostic) diagnostic->admitted = true;
    return true;
}
State::Natural* ResolveBeforeSrSelectionDonor(uint64_t settings,
    NaturalAssociation::SelectionDonor& token, unsigned& count,
    BeforeSrDonorDiagnostics* diagnostic = nullptr) noexcept
{
    token = {}; count = 0;
    if (diagnostic) *diagnostic = {};
    State::Natural* selected = nullptr;
    NaturalAssociation::SelectionDonor candidate;
    for (auto& entry : Get().natural) {
        BeforeSrDonorCandidate* probe = nullptr;
        // Selection-valid/live entries cannot be hidden behind the first empty
        // slot's not-live result. Record ALL nonempty entries, never pick one.
        if (diagnostic && (entry.live || entry.runtime || entry.id.runtime || entry.lifetime ||
                           entry.selection.naturalRuntime || entry.selection.valid)) {
            ++diagnostic->nonempty;
            if (entry.live) ++diagnostic->live;
            if (entry.selection.valid) ++diagnostic->validSelection;
            if (diagnostic->count < diagnostic->candidates.size())
                probe = &diagnostic->candidates[diagnostic->count++];
            else diagnostic->truncated = true;
        }
        const bool admitted = ReadBeforeSrSelectionDonor(entry, settings, candidate, true, probe);
        if (probe) probe->slot = static_cast<unsigned>(&entry - Get().natural);
        if (!admitted) continue;
        ++count; selected = &entry; token = candidate;
    }
    // No candidate is observable as a successful result until uniqueness holds.
    if (count != 1) { token = {}; return nullptr; }
    return selected;
}
bool BeforeSrSelectionDonorCurrent(const State::Natural& natural,
    const NaturalAssociation::SelectionDonor& expected) noexcept
{
    NaturalAssociation::SelectionDonor current;
    unsigned count = 0;
    return BeforeSrDonorCall::Current(expected) &&
        ResolveBeforeSrSelectionDonor(expected.settings, current, count) == &natural && current == expected;
}
void AdvanceSettingsAtOwnerBoundary()
{
    auto& state = Get();
    const auto desired = state.desiredSettings.load(std::memory_order_acquire);
    const auto applied = state.appliedSettings.load(std::memory_order_acquire);
    if (!desired || desired == applied) return;
    for (auto& natural : state.natural) {
        if (!natural.live || !natural.runtime) continue;
        const bool oldUse = natural.settingsUse.settings &&
            (natural.settingsUse.settings == applied || natural.settingsBarrierEpoch);
        const bool oldCopy = natural.copiedSerial && natural.sourceSettings != desired;
        if (!oldUse && !oldCopy) continue;
        // Snapshot the CURRENT real epoch at the CPU owner boundary. Merely
        // comparing to the recording epoch could admit Final after a parent
        // Present advanced that epoch while finishing the same old picture.
        const auto epoch = (std::max)(natural.frame.epoch, natural.settingsUse.epoch);
        natural.settingsBarrierEpoch = (std::max)(natural.settingsBarrierEpoch, epoch);
    }
    // Publish the configuration without waiting for unrelated natural events.
    // Each resolved source checks only its own retained epoch/ticket barrier.
    // No old work/ticket means first enable is independent of observed runtimes.
    // No GPU owner is reset: completion remains the lease pool's responsibility.
    state.stage.store(SettingsStage(desired), std::memory_order_relaxed);
    state.enabled.store(SettingsEnabled(desired), std::memory_order_release);
    state.appliedSettings.store(desired, std::memory_order_release);
    state.latestEpochWait = {};
    LOG_INFO("HoYoShade live settings applied generation={} enabled={} stage={} boundary=render-owner epoch_gate=resolved-source gpu_resources=completion-owned pixels=UNKNOWN",
        SettingsGeneration(desired), SettingsEnabled(desired), Text(static_cast<Stage>(SettingsStage(desired))));
}
using DeviceValidation::Predicate;
using DeviceValidation::Role;
using DeviceValidation::Site;
using LiveDevices = std::array<ID3D12Device*, static_cast<unsigned>(Role::Count)>;

DirectChain* FindDirectChain(uint64_t chain) noexcept
{
    if (!chain) return nullptr;
    for (auto& c : Get().directChains)
        if (c.chain.load(std::memory_order_acquire) == chain || c.wrapper.load(std::memory_order_acquire) == chain) return &c;
    return nullptr;
}
bool DirectPending(const DirectSlot& slot) noexcept
{
    return slot.receipt && slot.receipt->state.load(std::memory_order_acquire) != OwnedRuntime::Retirement::Ready;
}
void RefreshDirectOwners(DirectChain& chain) noexcept
{
    const auto key = chain.chain.load(std::memory_order_relaxed);
    const auto lifetime = chain.lifetime.load(std::memory_order_relaxed);
    for (const auto& e : Get().directEntries) for (const auto& slot : e.slots)
        if (slot.occupied && slot.key.chain == key && slot.key.chainLifetime == lifetime && DirectPending(slot)) return;
    chain.access.fetch_and(~DirectOwners, std::memory_order_acq_rel);
}
void InvalidateDirect(uint64_t chain = 0, uint64_t natural = 0) noexcept
{
    // executionMutex held; release ONLY cache references, never in-flight views.
    for (auto& e : Get().directEntries) for (auto& slot : e.slots)
        if (slot.occupied && (!chain || slot.key.chain == chain) && (!natural || slot.key.naturalRuntime == natural)) {
            slot.revoked = true; slot.owner.reset();
        }
}
struct DirectRecordingScope
{
    DirectChain* chain = nullptr;
    uint64_t lifetime = 0, epoch = 0;
    DirectRecordingScope() = default;
    void Enter(uint64_t key) noexcept
    {
        auto* c = FindDirectChain(key); if (!c) return;
        if (c->draining) {
            RefreshDirectOwners(*c);
            if (c->access.load(std::memory_order_acquire) & DirectOwners) return;
            c->draining = false;
        }
        if (c->epoch == UINT64_MAX) return;
        auto state = c->access.load(std::memory_order_acquire);
        if (state & DirectClosed || state >= DirectResizeStep || state & DirectRecording) return;
        if (!c->access.compare_exchange_strong(state, state | DirectRecording, std::memory_order_acq_rel)) return;
        chain = c; lifetime = c->lifetime.load(std::memory_order_acquire); epoch = c->epoch;
    }
    bool Current() const noexcept
    {
        return chain && chain->lifetime.load(std::memory_order_acquire) == lifetime &&
            chain->epoch == epoch && !chain->draining &&
            !(chain->access.load(std::memory_order_acquire) & DirectClosed);
        // A concurrent resize sees Recording and returns Pending. It cannot
        // enter real DXGI while this interval still owns GPU recording.
    }
    ~DirectRecordingScope()
    { if (chain) { RefreshDirectOwners(*chain); chain->access.fetch_and(~DirectRecording, std::memory_order_acq_rel); } }
};
OwnedRuntime::Retirement ReleaseDirectTransaction(const OwnedRuntime::EntryReceipt& entry) noexcept
{
    // Exact entry only; do not call the legacy global collector from resize.
    for (auto& t : Get().transactions) {
        if (!(t.poolEntry == entry)) continue;
        if (t.orphaned || t.signalFailed) return OwnedRuntime::Retirement::Unknown;
        if (t.inFlight || t.reserved || !t.submitted || !t.recorded || !t.lease)
            return OwnedRuntime::Retirement::Pending;
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        if (!t.completion.fence || !t.completion.value || t.completion.value == UINT64_MAX ||
            FAILED(t.completion.fence->GetDevice(IID_PPV_ARGS(&device))) || !device ||
            FAILED(device->GetDeviceRemovedReason())) {
            t.orphaned = true; return OwnedRuntime::Retirement::Unknown;
        }
        const auto completed = t.completion.fence->GetCompletedValue();
        if (completed == UINT64_MAX) { t.orphaned = true; return OwnedRuntime::Retirement::Unknown; }
        if (completed < t.completion.value) return OwnedRuntime::Retirement::Pending;
        // Same executionMutex as every transaction writer. Drop owners/lease
        // OUTSIDE State::mutex; final-release is only a retirement request.
        auto lease = std::move(t.lease);
        auto owner = std::move(t.rtvOwner);
        ClearTransaction(t);
        owner.reset(); lease = {};
    }
    return OwnedRuntime::Retirement::Ready;
}
DirectEntry* DirectCache(const OwnedRuntime::EntryReceipt& entry) noexcept
{
    DirectEntry* available = nullptr;
    for (auto& e : Get().directEntries) {
        if (e.entry == entry) return &e;
        bool retired = true;
        for (const auto& slot : e.slots) retired &= !slot.owner && !DirectPending(slot);
        if (retired && !available) available = &e;
    }
    if (available) { *available = {}; available->entry = entry; }
    return available;
}


// Only called with executionMutex held and with the call's COM references alive.
// QueryInterface(IUnknown) is sampled AFTER the rate gate, never every frame.
// It explains public COM identity only: no proxy unwrap/private IID or new alias
// admission. Even equal sampled identities leave the original refusal intact.
void ReportDeviceFailure(const ColorEndpoint& endpoint, Site site,
                         const DeviceValidation::Snapshot& evidence, const LiveDevices& devices = {},
                         uint64_t expectedDevice = 0) noexcept
{
    try {
        const auto stage = static_cast<unsigned>(endpoint.stage), output = static_cast<unsigned>(endpoint.output);
        if (evidence.first == Predicate::None || stage < 1 || stage > 3 || output >= 3) return;
        const auto report = Get().deviceReports[static_cast<unsigned>(site)][stage - 1][output].Next(evidence, GetTickCount64());
        if (!report.emit) return;
        std::array<Microsoft::WRL::ComPtr<IUnknown>, static_cast<unsigned>(Role::Count)> identities;
        std::string details;
        for (unsigned index = 0; index < devices.size(); ++index) {
            const bool queried = (evidence.queried & (1u << index)) != 0;
            const bool sample = devices[index] && (!queried || evidence.hrs[index] >= 0);
            HRESULT identityHr = E_PENDING;
            if (sample) identityHr = devices[index]->QueryInterface(IID_PPV_ARGS(&identities[index]));
            char row[256];
            std::snprintf(row, sizeof(row), " %s{get_device_queried=%u,hr=0x%08x,device=0x%llx,identity_sampled=%u,identity_hr=0x%08x,iunknown=0x%llx}",
                DeviceValidation::Text(static_cast<Role>(index)), unsigned(queried), unsigned(evidence.hrs[index]),
                static_cast<unsigned long long>(evidence.devices[index]), unsigned(sample), unsigned(identityHr),
                static_cast<unsigned long long>(Key(identities[index].Get())));
            details += row;
        }
        LOG_INFO("HoYoShade device validation site={} predicate={} hr_known={} hr=0x{:08x} null_success={} observation={} stage={} output_kind={} source_stream={} source_generation={} source_frame={} output_sequence={} color=0x{:x} consumer_queue=0x{:x} producer_queue=0x{:x} fence=0x{:x} producer_value={} expected_device=0x{:x} related_device=0x{:x} related_queue=0x{:x} natural_device=0x{:x} natural_backbuffer=0x{:x} removed_queried={} removed_hr=0x{:08x} completed_queried={} completed={} devices={} identity_policy=exact-pointer-required sampled_COM_identity=diagnostic-only pixels=UNKNOWN",
            DeviceValidation::Text(site), DeviceValidation::Text(evidence.first), evidence.hrKnown,
            static_cast<uint32_t>(evidence.failureHr), evidence.nullSuccess, report.count,
            Text(endpoint.stage), output, endpoint.stream, endpoint.generation, endpoint.frame, endpoint.outputToken.sequence,
            Key(endpoint.color), Key(endpoint.queue), Key(endpoint.ready.queue), Key(endpoint.ready.fence), endpoint.ready.value,
            expectedDevice, evidence.relatedDevice, evidence.relatedQueue, endpoint.naturalDevice, endpoint.naturalBackbuffer, evidence.removedKnown,
            static_cast<uint32_t>(evidence.removedHr), evidence.completedKnown, evidence.completed, details);
    } catch (...) {} // Diagnostics cannot change a returned reason or any safety gate.
}
struct StageDeviceAccess
{
    const ColorEndpoint& endpoint;
    std::array<Microsoft::WRL::ComPtr<ID3D12Device>, 4> devices;
    DeviceValidation::DeviceQuery GetDevice(Role role)
    {
        const auto index = static_cast<unsigned>(role);
        HRESULT hr = E_UNEXPECTED;
        switch (role) {
        case Role::Color: hr = endpoint.color->GetDevice(IID_PPV_ARGS(&devices[index])); break;
        case Role::ConsumerQueue: hr = endpoint.queue->GetDevice(IID_PPV_ARGS(&devices[index])); break;
        case Role::ProducerQueue: hr = endpoint.ready.queue->GetDevice(IID_PPV_ARGS(&devices[index])); break;
        case Role::Fence: hr = endpoint.ready.fence->GetDevice(IID_PPV_ARGS(&devices[index])); break;
        default: break;
        }
        return {hr, Key(devices[index].Get())};
    }
    bool HasSubmission() const { return bool(endpoint.ready.submission); }
    bool SubmittedFenceDomain() const {
        return endpoint.ready.submission && endpoint.ready.submission->Matches(endpoint.color,endpoint.queue,
            endpoint.ready.queue,endpoint.ready.fence,endpoint.ready.value,devices[0].Get(),devices[3].Get(),
            endpoint.frame,endpoint.ready.generation,endpoint.ready.batch);
    }
    HRESULT RemovedReason() const { return devices[0]->GetDeviceRemovedReason(); }
    uint64_t CompletedValue() const { return endpoint.ready.fence->GetCompletedValue(); }
    LiveDevices Live() const { return {devices[0].Get(), devices[1].Get(), devices[2].Get(), devices[3].Get(), nullptr}; }
};
bool Supported(unsigned api) noexcept { return api == 0xb000 || api == 0xc000; }
Identity ReadIdentity(reshade::api::effect_runtime* runtime)
{
    Identity id;
    if (!runtime) return id;
    auto* device = runtime->get_device();
    if (!device) return id;
    id.runtime = Key(runtime); id.sdkDevice = Key(device); id.api = static_cast<unsigned>(device->get_api());
    // SDK get_native is a native object in this API domain, not the game's
    // scheduling/proxy device. Never dereference this scalar or unwrap it.
    if (!Supported(id.api)) return {};
    auto* queue = runtime->get_command_queue();
    if (!queue || queue->get_device() != device) return {};
    id.sdkQueue = Key(queue); id.nativeDevice = device->get_native(); id.nativeQueue = queue->get_native();
    if (!id.nativeDevice || !id.nativeQueue) return {};
    return id;
}
void EnrollList(reshade::api::command_list* list)
{
    if (!list) return;
    auto* device = list->get_device();
    if (!device) return;
    const auto api = static_cast<unsigned>(device->get_api());
    if (!Supported(api)) return;
    const List snapshot{Key(list), Key(device), list->get_native(), device->get_native(), api};
    std::lock_guard lock(Get().mutex);
    // An owned immediate list is expected to be re-enrolled by the core's
    // begin/finish/technique callbacks while RenderBeforeNr is executing.  It
    // is not a stale-list event and must not invalidate the transaction that
    // is currently flushing that same list.  Actual list destruction is
    // handled by OnDestroyList, where the core has supplied a lifetime event.
    if (!Get().registry.EnrollList(snapshot)) ++Get().overflow;
}

// One RenderStage stack lifetime only. Strong references pin immutable COM
// allocation facts; no scope pointer or admission result is retained here.
// Live owner/receipt/transport predicates still run at every boundary.
struct NativeColorFacts
{
    // D3D12 allocation facts from this call's real stage validation, reused
    // only for the exact retained color/queue. Health/completion stay live.
    Microsoft::WRL::ComPtr<ID3D12Resource> gpuColor;
    Microsoft::WRL::ComPtr<ID3D12Device> gpuColorDevice, gpuQueueDevice;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> gpuQueue;
    D3D12_RESOURCE_DESC gpuDesc{};
    bool gpuCaptured = false;
    std::array<Microsoft::WRL::ComPtr<ID3D12Device>, 4> presentDevices;
    std::shared_ptr<const GenshinPresentNr::NativeColorCapability> presentPicture;
    native::NativeSubmission presentSubmission;
    Microsoft::WRL::ComPtr<ID3D12Fence> presentFence;
    uint64_t presentValue = 0, presentGeneration = 0, presentBatch = 0;
    bool presentCaptured = false;
#ifndef HOYOSHADE_STAGE_TEST
    GenshinGuides::Owner owner{};
    std::shared_ptr<GenshinTransport::ProducerFrame> producer;
    std::shared_ptr<GenshinTransport::Lease> transport;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<ID3D11Resource> sourceResource;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
    Microsoft::WRL::ComPtr<ID3D11Device> contextDevice, sourceDevice;
    Microsoft::WRL::ComPtr<ID3D12Resource> color;
    D3D11_TEXTURE2D_DESC input{};
    D3D12_RESOURCE_DESC imported{};
    D3D11_DEVICE_CONTEXT_TYPE contextType{};
    bool captured = false;
#endif
};

// API20 external-RTV processing does not require scene-linear/SDR facts.
// This exception authenticates the actual Genshin RGBA8 producer; all other
// endpoints retain their transfer-qualified path. No inference from UNORM,
// typeless storage, COMMON or a global output swapchain color-space value.
NativeColorAdmission InspectNativeBeforeSr(const ColorEndpoint& endpoint, NativeColorFacts& immutable) noexcept
{
    if (endpoint.colorMode != ColorMode::GenshinNativeTypedBeforeSr)
        return NativeColorAdmission::NotRequested;
    if (endpoint.stage != Stage::BeforeSr || endpoint.output != OutputKind::Original ||
        endpoint.naturalContract != NaturalSourceContract::BeforeSrOwnedRuntimeWithSelectionDonor ||
        !endpoint.stream || !endpoint.color || !endpoint.queue || endpoint.naturalSource || endpoint.naturalBackbuffer ||
        endpoint.colorX || endpoint.colorY)
        return NativeColorAdmission::EndpointMismatch;
#ifndef HOYOSHADE_STAGE_TEST
    try {
        static const auto game = [] {
            const auto name = Util::ExePath().filename().wstring();
            return GameRouting::IdentifyExecutable(name.c_str());
        }();
        if (game != GameRouting::Identity::Genshin) return NativeColorAdmission::WrongGame;
        const auto& binding = endpoint.nativeColor;
        const auto producer = GenshinTransport::ProducerInvocation::Current();
        const auto* scope = GenshinTransport::Scope::Current();
        if (!binding.producer || !binding.transport || !producer || !producer->Live() || !producer->Guides())
            return NativeColorAdmission::ProducerUnavailable;
        // A stale borrowed scope must never be dereferenced.
        if (!scope || scope != binding.scope || producer != binding.producer ||
            scope->lease != binding.transport || !scope->parameters)
            return NativeColorAdmission::InvocationMismatch;
        const auto& owner = scope->owner;
        const auto& facts = producer->Guides()->Get();
        if (!GenshinGuides::SameOwner(owner, producer->Owner()) || !GenshinGuides::SameOwner(owner, facts.owner) ||
            !owner.device || !owner.context || !owner.deviceEpoch || !owner.producerEpoch || !owner.configEpoch ||
            !owner.instance || !owner.historyEpoch || !owner.generation || owner.thread != GetCurrentThreadId() ||
            owner.device != endpoint.naturalDevice || owner.generation != endpoint.generation ||
            owner.sourceFrame != endpoint.frame)
            return NativeColorAdmission::OwnerMismatch;
        if (!producer->Guides()->CurrentResources(owner, scope->resources) ||
            facts.renderWidth.value != endpoint.width || facts.renderHeight.value != endpoint.height)
            return NativeColorAdmission::ReceiptMismatch;
        const auto observed = binding.transport->Snapshot();
        if (observed.revoked || observed.stage != GenshinTransport::Stage::InputsOrdered ||
            !GenshinGuides::SameOwner(owner, observed.owner) || observed.queue != Key(endpoint.queue) ||
            observed.parameters != Key(scope->parameters) || endpoint.ready.queue != endpoint.queue ||
            observed.imported[0] != Key(endpoint.color) || binding.transport->Resource(owner, 0) != endpoint.color)
            return NativeColorAdmission::TransportMismatch;
        for (size_t i = 0; i < scope->resources.size(); ++i)
            if (observed.source[i] != Key(scope->resources[i])) return NativeColorAdmission::TransportMismatch;
        auto* context = reinterpret_cast<ID3D11DeviceContext*>(owner.context);
        if (immutable.captured) {
            // Never reuse facts for replacement objects, even within this call.
            // The live four-resource and transport checks above are not cached.
            if (!GenshinGuides::SameOwner(immutable.owner, owner))
                return NativeColorAdmission::OwnerMismatch;
            if (immutable.producer != producer || immutable.transport != binding.transport ||
                immutable.context.Get() != context || immutable.sourceResource.Get() != scope->resources[0] ||
                immutable.color.Get() != endpoint.color)
                return NativeColorAdmission::ReceiptMismatch;
        } else {
            immutable.owner = owner;
            immutable.producer = producer; immutable.transport = binding.transport;
            immutable.context = context; immutable.sourceResource = scope->resources[0];
            immutable.color = endpoint.color;
            context->GetDevice(&immutable.contextDevice);
            if (!immutable.sourceResource ||
                FAILED(immutable.sourceResource.As(&immutable.source)) || !immutable.source)
                return NativeColorAdmission::ReceiptMismatch;
            immutable.source->GetDevice(&immutable.sourceDevice);
            immutable.contextType = context->GetType();
            immutable.source->GetDesc(&immutable.input);
            immutable.imported = endpoint.color->GetDesc();
            immutable.gpuDesc = immutable.imported;
            immutable.captured = true;
        }
        if (immutable.contextType != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
            Key(immutable.contextDevice.Get()) != owner.device ||
            immutable.contextDevice.Get() != immutable.sourceDevice.Get())
            return NativeColorAdmission::OwnerMismatch;
        const auto& input = immutable.input;
        const auto& color = immutable.imported;
        // Typed copy compatibility is not a declaration of game transfer.
        if (input.Format != DXGI_FORMAT_R8G8B8A8_UNORM || color.Format != input.Format ||
            facts.textures[0].format.value != input.Format || input.Width != endpoint.width ||
            input.Height != endpoint.height || input.MipLevels != 1 || input.ArraySize != 1 ||
            input.SampleDesc.Count != 1 || input.SampleDesc.Quality)
            return NativeColorAdmission::FormatMismatch;
        // Known HDR is unsupported. Unknown/false requested HDR never becomes SDR.
        if (binding.providerHdrKnown && binding.providerHdr) return NativeColorAdmission::HdrUnsupported;
        return NativeColorAdmission::ExactNativeTyped;
    } catch (...) { return NativeColorAdmission::ProducerUnavailable; }
#else
    // A standalone host has no authenticated game producer and cannot mint it.
    (void)immutable;
    return NativeColorAdmission::ProducerUnavailable;
#endif
}
// BeforeNR authorizes the final Present picture copied by the real native
// adapter, not the BeforeSR selected colour/guide/output receipt. Public scalar
// fields and a retained COM texture alone cannot grant or renew this contract.
NativeColorAdmission InspectNativeBeforeNr(const ColorEndpoint& endpoint, NativeColorFacts& immutable) noexcept
{
    if (endpoint.colorMode != ColorMode::GenshinNativeTypedBeforeNr)
        return NativeColorAdmission::NotRequested;
    if (endpoint.stage != Stage::BeforeNr || endpoint.output != OutputKind::Original ||
        endpoint.naturalContract != NaturalSourceContract::TypedBackbuffer || endpoint.sdr ||
        !endpoint.naturalSource || !endpoint.naturalDevice || !endpoint.naturalBackbuffer ||
        !endpoint.stream || !endpoint.generation || !endpoint.color || !endpoint.queue)
        return NativeColorAdmission::PresentEndpointMismatch;
#ifndef HOYOSHADE_STAGE_TEST
    try {
        static const auto game = [] {
            const auto name = Util::ExePath().filename().wstring();
            return GameRouting::IdentifyExecutable(name.c_str());
        }();
        if (game != GameRouting::Identity::Genshin) return NativeColorAdmission::WrongGame;
        const auto& binding = endpoint.beforeNrNativeColor;
        const auto* scope = GenshinPresentNr::Scope::Current();
        if (!scope || !binding.picture) return NativeColorAdmission::PresentScopeUnavailable;
        const auto& config = *Config::Instance();
        if (!config.DlssNrEnabled.value_or_default() || !config.DlssNrFinishedPicture.value_or_default() ||
            config.DlssNrRunBeforeSr.value_or_default() || config.DlssNrDeferredDlss.value_or_default())
            return NativeColorAdmission::PresentEndpointMismatch;
        if (!config.GenshinControlsCurrent(binding.picture->ConfigEpoch())) return NativeColorAdmission::OwnerMismatch;
        if (!scope->MatchesNativeColor(binding.picture, endpoint.color, endpoint.queue, endpoint.stream,
            endpoint.generation, endpoint.frame, endpoint.naturalDevice, endpoint.naturalBackbuffer,
            endpoint.width, endpoint.height, binding.observation)) return NativeColorAdmission::PresentPictureMismatch;
        const auto& observed = binding.picture->Observation();
        const auto& source = binding.picture->SourceDesc();
        const auto& picture = binding.picture->PictureDesc();
        if (observed.origin != native::NativePictureColorOrigin::SwapchainHelperDefaultOrPrivateUnknown ||
            observed.sourceFormat != source.Format || observed.sharedFormat != picture.Format)
            return NativeColorAdmission::CopyProvenanceMismatch;
        // Exact adapter CopyResource pairs, not a general format-family cast:
        // typed RGBA8 UNORM(28) or UNORM_SRGB(29) -> shared typed UNORM(28).
        // D3D11 permits this same-type-group copy without sRGB decode/encode.
        // Source/ticket/observation retain the actual format; only copy legality
        // is widened. Transfer stays UNKNOWN. No typeless/BGRA/10-bit/float/resolve.
        const bool nativeCopyFormats = picture.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
            (source.Format == DXGI_FORMAT_R8G8B8A8_UNORM || source.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        if (!nativeCopyFormats ||
            source.MipLevels != 1 || source.ArraySize != 1 || source.SampleDesc.Count != 1 || source.SampleDesc.Quality ||
            source.Width != endpoint.width || source.Height != endpoint.height ||
            picture.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || picture.MipLevels != 1 ||
            picture.DepthOrArraySize != 1 || picture.SampleDesc.Count != 1 || picture.SampleDesc.Quality ||
            picture.Width != endpoint.width || picture.Height != endpoint.height || !endpoint.width || !endpoint.height)
            return NativeColorAdmission::FormatMismatch;
        if (observed.knownHdr || observed.rawColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 ||
            observed.rawColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
            return NativeColorAdmission::HdrUnsupported;
        // G22 is only the narrow raw-value allowlist for native processing.
        // The helper may have returned its default: transfer/provenance UNKNOWN.
        if (observed.rawColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)
            return NativeColorAdmission::RawColorSpaceUnsupported;
        const auto& ready = endpoint.ready;
        if (!ready.submission || ready.queue != endpoint.queue || !ready.fence || !ready.value ||
            ready.value == UINT64_MAX || !ready.generation || ready.generation == UINT64_MAX ||
            !ready.batch || ready.batch == UINT64_MAX) return NativeColorAdmission::SubmissionMismatch;
        const bool firstBoundary = !immutable.presentCaptured;
        if (!firstBoundary) {
            if (immutable.presentPicture != binding.picture || immutable.presentSubmission != ready.submission ||
                immutable.presentFence.Get() != ready.fence || immutable.presentValue != ready.value ||
                immutable.presentGeneration != ready.generation || immutable.presentBatch != ready.batch)
                return NativeColorAdmission::SubmissionMismatch;
        } else {
            // Run the original StageDeviceAccess/ValidateStage gate HERE for
            // this mode, not a second time later in RenderStage. This checks
            // the private receipt (including allocator anchor), real device
            // health and UINT64_MAX. Only immutable interfaces are retained.
            StageDeviceAccess access{endpoint};
            DeviceValidation::Snapshot evidence;
            if (!DeviceValidation::ValidateStage(access, evidence)) {
                ReportDeviceFailure(endpoint, Site::Stage, evidence, access.Live());
                return NativeColorAdmission::SubmissionMismatch;
            }
            immutable.presentDevices = std::move(access.devices);
            immutable.presentPicture = binding.picture; immutable.presentSubmission = ready.submission;
            immutable.presentFence = ready.fence; immutable.presentValue = ready.value;
            immutable.presentGeneration = ready.generation; immutable.presentBatch = ready.batch;
            immutable.gpuDesc = picture; immutable.presentCaptured = true;
        }
        const auto& devices = immutable.presentDevices;
        if (devices[0].Get() != binding.picture->PictureOwner() || devices[0] != devices[1] || devices[0] != devices[2] ||
            (!firstBoundary && !ready.submission->Matches(endpoint.color, endpoint.queue, ready.queue, ready.fence, ready.value,
                devices[0].Get(), devices[3].Get(), endpoint.frame, ready.generation, ready.batch)))
            return NativeColorAdmission::SubmissionMismatch;
        return source.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
            ? NativeColorAdmission::ExactNativePresentSrgbCopy : NativeColorAdmission::ExactNativePresentPicture;
    } catch (...) { return NativeColorAdmission::PresentScopeUnavailable; }
#else
    (void)immutable;
    return NativeColorAdmission::PresentScopeUnavailable;
#endif
}
bool AdmitColor(const ColorEndpoint& endpoint, StageResult& result, NativeColorFacts& immutable) noexcept
{
    if (endpoint.colorMode == ColorMode::TransferQualified) {
        result.nativeColor = NativeColorAdmission::NotRequested;
        return endpoint.sdr;
    }
    if (endpoint.colorMode == ColorMode::GenshinNativeTypedBeforeSr) {
        result.nativeColor = InspectNativeBeforeSr(endpoint, immutable);
        return result.nativeColor == NativeColorAdmission::ExactNativeTyped;
    }
    if (endpoint.colorMode == ColorMode::GenshinNativeTypedBeforeNr) {
        result.nativeColor = InspectNativeBeforeNr(endpoint, immutable);
        return result.nativeColor == NativeColorAdmission::ExactNativePresentPicture ||
            result.nativeColor == NativeColorAdmission::ExactNativePresentSrgbCopy;
    }
    result.nativeColor = NativeColorAdmission::EndpointMismatch;
    return false;
}

bool ToResourceUsage(D3D12_RESOURCE_STATES state, reshade::api::resource_usage &usage) noexcept
{
    switch (state) {
    case D3D12_RESOURCE_STATE_COMMON:
        // D3D12_RESOURCE_STATE_PRESENT is the same zero-valued state as
        // COMMON.  A native NR input is not known to be a swap-chain
        // backbuffer here, so use the general barrier state conservatively.
        usage = reshade::api::resource_usage::general; return true;
    case D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:
        usage = reshade::api::resource_usage::shader_resource_non_pixel; return true;
    case D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:
        usage = reshade::api::resource_usage::shader_resource_pixel; return true;
    case D3D12_RESOURCE_STATE_RENDER_TARGET:
        usage = reshade::api::resource_usage::render_target; return true;
    default:
        return false;
    }
}

void DestroyUnsubmittedView(reshade::api::device* device, reshade::api::resource_view& view) noexcept
{
    if (!device || !view.handle) return;
    try { device->destroy_resource_view(view); } catch (...) {}
    view = { 0 };
}

// Only the authenticated source callsite supplies queue authority. A swapchain callback publishes
// observation metadata with a null queue; it cannot authorize AcquireForRender.
// Reuse is exact stream owner + device + DIRECT queue + SDR extent/format.
// A changed tuple retires the previous owner and advances the binding generation.
bool EnsureNativeBinding(uint64_t owner, ID3D12Device *device, ID3D12CommandQueue *queue,
                         const D3D12_RESOURCE_DESC &desc, const std::shared_ptr<void>& configLifetime,
                         const ColorEndpoint& endpoint, const NativeColorFacts& immutable,
                         const NaturalAssociation::SelectionDonor* donor) noexcept
{
    if (!owner || !device || !queue || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return false;
    DeviceValidation::Snapshot evidence;
    evidence.devices[0] = Key(device);
    evidence.removedKnown = true; evidence.removedHr = device->GetDeviceRemovedReason();
    if (!evidence.Api(Predicate::DeviceHealthy, evidence.removedHr)) {
        ReportDeviceFailure(endpoint, Site::Binding, evidence, {device}); return false;
    }
    Microsoft::WRL::ComPtr<ID3D12Device> queueDevice;
    HRESULT hr = S_OK;
    if ((donor || endpoint.colorMode == ColorMode::GenshinNativeTypedBeforeNr) && immutable.gpuCaptured && immutable.gpuQueue.Get() == queue && immutable.gpuColorDevice.Get() == device)
        queueDevice = immutable.gpuQueueDevice;
    else hr = queue->GetDevice(IID_PPV_ARGS(&queueDevice));
    if (!evidence.Device(Role::ConsumerQueue, hr, Key(queueDevice.Get())) ||
        !evidence.Check(queueDevice.Get() == device, Predicate::ConsumerDeviceEqual)) {
        ReportDeviceFailure(endpoint, Site::Binding, evidence, {device, queueDevice.Get()}); return false;
    }

    const auto format = ColorContract::Resolve(desc.Format).runtime;
    // BUFFER dimensions describe the pixels processed by this runtime, not
    // unused allocation padding surrounding the selected SR input.
    const auto width = endpoint.width, height = endpoint.height;
    if (format == DXGI_FORMAT_UNKNOWN || !width || !height)
        return false;

    {
        std::lock_guard lock(Get().mutex);
        const auto &binding = Get().ownedBinding;
        if (binding.owner == owner && binding.device == device && binding.queue == queue &&
            binding.width == width && binding.height == height && binding.format == format &&
            Get().ownedFactory && Get().ownedDevice && Get().ownedQueue &&
            Get().selectionConfigPath == Get().ownedConfigPath &&
            NaturalAssociation::SameSelectionConfiguration(Get().ownedSelectionDonor,
                donor ? *donor : NaturalAssociation::SelectionDonor{}))
            return true;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory4> factory4;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory4))) ||
        FAILED(factory4.As(&factory)))
        return false;

    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    const auto luid = device->GetAdapterLuid();
    if (FAILED(factory4->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) || !adapter)
        return false;

    uint64_t oldOwner = 0;
    {
        std::lock_guard lock(Get().mutex);
        const auto &binding = Get().ownedBinding;
        if (binding.owner == owner && binding.device == device && binding.queue == queue &&
            binding.width == width && binding.height == height && binding.format == format &&
            Get().ownedFactory && Get().ownedDevice && Get().ownedQueue &&
            Get().selectionConfigPath == Get().ownedConfigPath &&
            NaturalAssociation::SameSelectionConfiguration(Get().ownedSelectionDonor,
                donor ? *donor : NaturalAssociation::SelectionDonor{}))
            return true;
        if (donor && Get().ownedBindingGeneration >= UINT64_MAX - 1) return false;
        oldOwner = Get().ownedBindingOwner;
        Get().ownedDevice = device;
        Get().ownedQueue = queue;
        Get().ownedFactory = factory;
        ++Get().ownedBindingGeneration;
        Get().ownedBindingOwner = owner;
        Get().ownedBinding = {};
        Get().ownedBinding.owner = owner;
        Get().ownedBinding.generation = Get().ownedBindingGeneration;
        Get().ownedBinding.device = device;
        Get().ownedBinding.queue = queue;
        Get().ownedBinding.factory = factory.Get();
        Get().ownedBinding.width = width;
        Get().ownedBinding.height = height;
        Get().ownedBinding.format = format;
        Get().selectionConfigPath = Get().ownedConfigPath;
        Get().ownedBinding.configPathUtf8 = Get().selectionConfigPath.c_str();
        Get().ownedBinding.configLifetime = configLifetime;
        Get().ownedSelectionDonor = donor ? *donor : NaturalAssociation::SelectionDonor{};
    }
    if (oldOwner) {
        InvalidateDirect();
        Get().scratch.reset(); Get().scratchRuntime = nullptr; Get().scratchGeneration = 0;
        OwnedRuntime::Retire(oldOwner, 0);
        OwnedRuntime::TryCollect();
    }
    return true;
}

void RetireCompletedTransactions() noexcept
{
    struct Candidate
    {
        uint64_t token = 0;
        OwnedRuntime::Completion completion;
    };

    for (;;) {
        Candidate candidate;
        {
            std::lock_guard lock(Get().mutex);
            for (auto &transaction : Get().transactions) {
                if (!transaction.recorded || !transaction.submitted || transaction.inFlight ||
                    !transaction.lease || transaction.orphaned)
                    continue;
                transaction.inFlight = 1;
                candidate = { transaction.token, transaction.completion };
                break;
            }
        }
        if (!candidate.token) return;

        Microsoft::WRL::ComPtr<ID3D12Device> fenceDevice;
        const bool completionDeviceKnown = candidate.completion.fence &&
            SUCCEEDED(candidate.completion.fence->GetDevice(IID_PPV_ARGS(&fenceDevice))) && fenceDevice &&
            SUCCEEDED(fenceDevice->GetDeviceRemovedReason());
        const uint64_t completed = candidate.completion.fence ?
            candidate.completion.fence->GetCompletedValue() : (std::numeric_limits<uint64_t>::max)();
        if (!candidate.completion.fence || candidate.completion.value == 0 ||
            !completionDeviceKnown || completed == (std::numeric_limits<uint64_t>::max)()) {
            std::lock_guard lock(Get().mutex);
            for (auto &transaction : Get().transactions)
                if (transaction.token == candidate.token) {
                    transaction.inFlight = 0;
                    transaction.orphaned = true;
                    LOG_WARN("HoYoShade stage owned fence status unknown; resources retained token={} fence_value={} completed=UNKNOWN", transaction.token, transaction.completion.value);
                    break;
                }
            continue;
        }
        if (completed < candidate.completion.value) {
            std::lock_guard lock(Get().mutex);
            for (auto &transaction : Get().transactions)
                if (transaction.token == candidate.token) { transaction.inFlight = 0; break; }
            return;
        }

        OwnedRuntime::Lease completedLease;
        std::shared_ptr<OwnedRuntime::Resource> completedResources;
        {
            std::lock_guard lock(Get().mutex);
            for (auto &transaction : Get().transactions)
                if (transaction.token == candidate.token) {
                    if (!transaction.orphaned) {
                        transaction.inFlight = 2;
                        completedLease = std::move(transaction.lease);
                        completedResources = std::move(transaction.rtvOwner);
                    } else {
                        transaction.inFlight = 0;
                    }
                    break;
                }
        }
        if (completedLease)
        {
            // Resource owner release only queues pool cleanup. Drop the active
            // lease outside State::mutex, then let the owned pool destroy views
            // with its SDK device alive and the completion fence satisfied.
            completedResources.reset();
            completedLease = OwnedRuntime::Lease{};
            OwnedRuntime::TryCollect();
            std::lock_guard lock(Get().mutex);
            for (auto &transaction : Get().transactions)
                if (transaction.token == candidate.token && transaction.inFlight == 2) {
                    ClearTransaction(transaction);
                    break;
                }
        }
    }
}

// Only reads callback-owned counters. The helper cannot invent core readiness
// from macro getters, frames, a queued reload or a successful submission.
uint64_t OwnedDepthReloadSerial(reshade::api::effect_runtime* runtime) noexcept
{
    std::lock_guard lock(Get().mutex);
    if (const auto* entry = Get().registry.Find(Key(runtime))) return entry->reloads;
    return 0;
}
void InvalidateOwnedSelection(Selection::Source& source)
{
    Selection::InvalidateOwned(source);
}

// Only the donor's own empty-RTV path may touch its command queue/list. These
// objects are never returned to the owned GPU path or retained in a transaction.
// Recheck after EVERY virtual call before touching another borrowed object.
bool SuppressBeforeSrSelectionDonor(State::Natural& natural,
    const NaturalAssociation::SelectionDonor& token, Attempt& attempt)
{
    const Selection::CallBoundary boundary;
    const auto current = [&] { return boundary.Current() && BeforeSrSelectionDonorCurrent(natural, token); };
    if (!current()) return false;
    auto* runtime = natural.runtime;
    const auto identity = natural.id;
    auto* device = runtime->get_device();
    if (!current() || !device || Key(device) != identity.sdkDevice) return false;
    const auto api = static_cast<unsigned>(device->get_api());
    if (!current() || api != identity.api || (api != 0xb000 && api != 0xc000)) return false;
    const auto nativeDevice = device->get_native();
    if (!current() || !nativeDevice || nativeDevice != identity.nativeDevice) return false;
    auto* queue = runtime->get_command_queue();
    if (!current() || !queue || Key(queue) != identity.sdkQueue) return false;
    auto* queueDevice = queue->get_device();
    if (!current() || queueDevice != device) return false;
    const auto nativeQueue = queue->get_native();
    if (!current() || !nativeQueue || nativeQueue != identity.nativeQueue) return false;
    auto* list = queue->get_immediate_command_list();
    if (!current() || !list) return false;
    auto* listDevice = list->get_device();
    if (!current() || listDevice != device) return false;
    auto* interval = activeBeforeSrDonor;
    if (!interval || !BeforeSrDonorCall::Current(token) || !natural.frame.Arm()) return false;
    attempt.naturalSuppressed = true; // consume BEFORE invoking the real core
    struct SuppressionInterval {
        BeforeSrDonorCall& call;
        explicit SuppressionInterval(BeforeSrDonorCall& value) : call(value) { call.suppressing = true; }
        ~SuppressionInterval() { call.suppressing = false; }
    } suppression(*interval);
    runtime->render_effects(list, {0}, {0});
    NaturalAssociation::SelectionDonor after;
    return boundary.Current() && BeforeSrDonorCall::Current(token) &&
        ReadBeforeSrSelectionDonor(natural, token.settings, after, false) && after == token;
}
Reason RecordStage(const Request &request, Decision &decision, const ColorEndpoint& endpoint,
                   StageResult& result, OwnedRuntime::Detail& ownedDetail, NativeColorFacts& immutable,
                   State::Natural& natural, State::Natural* suppress,
                   reshade::api::command_list* naturalList,
                   const NaturalAssociation::SelectionDonor* selectionDonor) noexcept
{
    BeforeSrDonorCall donorCall(selectionDonor, natural);
    const auto naturalRuntime = natural.runtime;
    const auto naturalLifetime = natural.lifetime, naturalEpoch = natural.frame.epoch;
    const auto suppressRuntime = suppress ? suppress->runtime : nullptr;
    const uint64_t suppressLifetime = suppress ? suppress->lifetime : 0;
    const uint64_t suppressEpoch = suppress ? suppress->frame.epoch : 0;
    const Selection::CallBoundary sourceBoundary;
    const auto sourceCurrent = [&] {
        return natural.live && natural.runtime == naturalRuntime && natural.lifetime == naturalLifetime &&
            natural.frame.epoch == naturalEpoch && !natural.frame.invalid &&
            (selectionDonor || sourceBoundary.Current()) &&
            (!suppress || (suppress->live && suppress->runtime == suppressRuntime &&
                suppress->lifetime == suppressLifetime && suppress->frame.epoch == suppressEpoch &&
                !suppress->frame.invalid && suppress->thread == GetCurrentThreadId())) &&
            (!selectionDonor || BeforeSrSelectionDonorCurrent(natural, *selectionDonor));
    };
    if (!sourceCurrent() || !Selection::HasEnabledTechniques(natural.selection)) return Reason::SelectionUnavailable;
    if (!AdmitColor(endpoint, result, immutable)) return Reason::ColorSpaceUnknown;
    auto* color = endpoint.color;
    const auto knownState = endpoint.state;
    if (!endpoint.stateKnown) return Reason::StateUnknown;

    reshade::api::resource_usage oldUsage{};
    if (!ToResourceUsage(knownState, oldUsage)) return Reason::StateUnknown;
    if (!color || !request.nativeDevice || !request.nativeQueue) return Reason::DeviceMismatch;

    const bool reuseGpuFacts = (selectionDonor || endpoint.colorMode == ColorMode::GenshinNativeTypedBeforeNr) && immutable.gpuCaptured && immutable.gpuColor.Get() == color &&
        immutable.gpuQueue.Get() == endpoint.queue;
    const auto nativeDesc = reuseGpuFacts ? immutable.gpuDesc : color->GetDesc();
    const auto colorFormats = ColorContract::Resolve(nativeDesc.Format);
    if (!colorFormats) return Reason::UnsupportedFormat;
    const auto linearFormat = colorFormats.linear, srgbFormat = colorFormats.srgb;
    const bool partialColor = endpoint.colorX || endpoint.colorY ||
        nativeDesc.Width != endpoint.width || nativeDesc.Height != endpoint.height;

    Microsoft::WRL::ComPtr<ID3D12Device> nativeDevice;
    DeviceValidation::Snapshot deviceEvidence;
    HRESULT deviceHr = S_OK;
    if (reuseGpuFacts) nativeDevice = immutable.gpuColorDevice;
    else deviceHr = color->GetDevice(IID_PPV_ARGS(&nativeDevice));
    if (!deviceEvidence.Device(Role::Color, deviceHr, Key(nativeDevice.Get())) ||
        !deviceEvidence.Check(Key(nativeDevice.Get()) == request.nativeDevice, Predicate::RecordDeviceEqual)) {
        ReportDeviceFailure(endpoint, Site::Record, deviceEvidence, {nativeDevice.Get()}, request.nativeDevice);
        return Reason::DeviceMismatch;
    }
    auto *nativeQueue = reinterpret_cast<ID3D12CommandQueue*>(request.nativeQueue);
    if (!nativeQueue || !EnsureNativeBinding(request.stream, nativeDevice.Get(), nativeQueue, nativeDesc, natural.selection.files, endpoint, immutable, selectionDonor))
        return Reason::NoRuntime;

    if (selectionDonor && !sourceCurrent()) return Reason::SelectionPending;
    Get().ownedNatural = natural.runtime;
    OwnedRuntime::Binding binding{};
    CoreApi acceptedApi{};
    HMODULE coreModule = nullptr;
    {
        std::lock_guard lock(Get().mutex);
        binding = Get().ownedBinding;
        acceptedApi = Get().api;
        coreModule = Get().coreModule;
    }
    deviceEvidence.relatedDevice = Key(binding.device);
    deviceEvidence.relatedQueue = Key(binding.queue);
    if (!deviceEvidence.Check(binding.device != nullptr, Predicate::BindingDevicePresent) ||
        !deviceEvidence.Check(binding.queue != nullptr, Predicate::BindingQueuePresent) ||
        !deviceEvidence.Check(binding.factory != nullptr, Predicate::BindingFactoryPresent) ||
        !deviceEvidence.Check(binding.configPathUtf8 != nullptr, Predicate::BindingConfigPresent) ||
        !deviceEvidence.Check(binding.device == nativeDevice.Get(), Predicate::BindingDeviceEqual) ||
        !deviceEvidence.Check(binding.queue == nativeQueue, Predicate::BindingQueueEqual)) {
        ReportDeviceFailure(endpoint, Site::Record, deviceEvidence, {nativeDevice.Get()}, request.nativeDevice);
        return Reason::DeviceMismatch;
    }

    OwnedOperation ownedCall;
    OwnedRuntime::Lease lease;
    const auto ownedStatus = OwnedRuntime::AcquireForRender(binding, coreModule, acceptedApi, lease, &ownedDetail);
    if (ownedStatus != OwnedRuntime::Status::Ready || !lease)
        return ownedStatus == OwnedRuntime::Status::SignalFailed ? Reason::FenceUnavailable : Reason::NoRuntime;
    auto *runtime = lease.Runtime();
    auto *sdkDevice = lease.Device();
    auto *sdkQueue = lease.Queue();
    auto *sdkList = lease.List();
    if (!runtime || !sdkDevice || !sdkQueue || !sdkList ||
        lease.NativeList() == nullptr || sdkQueue->get_native() != request.nativeQueue)
        return Reason::QueueMismatch;

    // An owned init/reload/reorder may revoke its handle map while leaving
    // the natural token unchanged. Drain an idle owned lease via its real
    // Submit/update path instead of abandoning it into permanent quarantine.
    // This records no source-color command and never suppresses the donor.
    const auto finishUnrecorded = [&](OwnedRuntime::Lease& idle, Reason pending) noexcept {
        // NativeProducer BeforeSR and the other source contracts need the
        // same drain as a selection donor. Abandoning an idle lease after an
        // owned init/reload/order event permanently quarantines its runtime.
        OwnedRuntime::Completion warmup;
        const auto status = idle.Submit(warmup, &ownedDetail);
        return status == OwnedRuntime::Status::Ready && warmup.fence && warmup.value && warmup.value != UINT64_MAX &&
            warmup.fence->GetCompletedValue() != UINT64_MAX ? pending : Reason::FenceUnavailable;
    };
    // The owned runtime must never inherit the previous frame's depth, including warmup.
    try { Depth::Clear(runtime); } catch (...) { return Reason::InternalFailure; }
    // AcquireForRender has proved the preceding GPU batch complete and holds
    // this exact private runtime exclusively. No effects are recorded yet.
    const auto& depthInput = endpoint.depth;
    const bool depthEndpointMatches = depthInput.owner == endpoint.stream &&
        depthInput.generation == endpoint.generation && depthInput.frame == endpoint.frame &&
        depthInput.queue == nativeQueue && depthInput.readyFence == endpoint.ready.fence &&
        depthInput.readyValue == endpoint.ready.value;
    const auto quarantineDonorReader = [&]() noexcept {
        if (selectionDonor && depthEndpointMatches && depthInput.reader) depthInput.reader->Quarantine();
    };
    const auto depthSemantics = Depth::Semantics::Project(Get().depthSemantics, runtime,
        lease.Generation(), natural.selection, depthEndpointMatches ? &depthInput : nullptr, OwnedDepthReloadSerial);
    if (depthSemantics.invalidateSelection) InvalidateOwnedSelection(natural.selection);
    if (depthSemantics.outcome == Depth::Semantics::Outcome::Pending) {
        // Update the real core through the owned lease, with no render_effects
        // call, source color mutation or natural suppression while it compiles.
        return finishUnrecorded(lease, Reason::SelectionPending);
    }
    const bool depthSemanticsReady = depthSemantics.outcome == Depth::Semantics::Outcome::Ready;
    if (!sourceCurrent()) return finishUnrecorded(lease, Reason::SelectionPending);
    const Selection::CallBoundary selectionBoundary;
    bool selectionReady = false;
    if (selectionDonor) {
        try { selectionReady = Selection::Apply(natural.selection, runtime, lease.Generation()); }
        catch (...) { return Reason::InternalFailure; } // unsubmitted lease quarantines its real owners
    } else selectionReady = Selection::Apply(natural.selection, runtime, lease.Generation());
    // A reentrant lifecycle change (or an unconfirmed reorder request) must
    // not fall through to warmup using the revoked SDK runtime/handles.
    if (!selectionBoundary.Current() || !sourceCurrent())
        return finishUnrecorded(lease, Reason::SelectionPending);
    if (!selectionReady) {
        // Warm only the owned runtime. Never suppress the real game while its
        // selected effect is still missing in this extent/API permutation.
        if (selectionDonor) {
            try { runtime->render_effects(sdkList, {0}, {0}); }
            catch (...) { return Reason::InternalFailure; }
        } else runtime->render_effects(sdkList, {0}, {0});
        return finishUnrecorded(lease, Reason::SelectionPending);
    }

    decision.owned = true;
    decision.executionAllowed = true;
    decision.matched = false;
    decision.runtime = {};
    decision.runtime.id.runtime = Key(runtime);
    decision.runtime.id.sdkDevice = Key(sdkDevice);
    decision.runtime.id.sdkQueue = Key(sdkQueue);
    decision.runtime.id.nativeDevice = sdkDevice->get_native();
    decision.runtime.id.nativeQueue = sdkQueue->get_native();
    decision.runtime.id.api = static_cast<unsigned>(sdkDevice->get_api());
    decision.runtime.sdkRuntime = Key(runtime);
    decision.runtime.sdkDevice = Key(sdkDevice);
    decision.runtime.sdkQueue = Key(sdkQueue);
    decision.runtime.generation = lease.Generation();
    if (selectionDonor && (decision.runtime.id.api != 0xc000 ||
        decision.runtime.id.nativeDevice != request.nativeDevice || decision.runtime.id.nativeQueue != request.nativeQueue))
        return Reason::DeviceMismatch;

    const auto resource = reshade::api::resource{ reinterpret_cast<uint64_t>(color) };
    State::Transaction *transaction = nullptr;
    uint64_t transactionToken = 0;
    bool transitioned = false;
    try {
        const auto sdkDesc = sdkDevice->get_resource_desc(resource);
        if (sdkDesc.type != reshade::api::resource_type::texture_2d ||
            sdkDesc.texture.width != nativeDesc.Width || sdkDesc.texture.height != nativeDesc.Height ||
            sdkDesc.texture.depth_or_layers != nativeDesc.DepthOrArraySize ||
            sdkDesc.texture.levels != nativeDesc.MipLevels || sdkDesc.texture.samples != nativeDesc.SampleDesc.Count ||
            sdkDesc.texture.format != static_cast<reshade::api::format>(colorFormats.endpoint))
            return Reason::DeviceMismatch;

        RetireCompletedTransactions();
        OwnedRuntime::TryCollect();
        {
            std::lock_guard lock(Get().mutex);
            for (auto &candidate : Get().transactions) {
                if (!candidate.reserved && !candidate.recorded && !candidate.signalPending &&
                    !candidate.inFlight && !candidate.orphaned && !candidate.lease) {
                    ClearTransaction(candidate);
                    candidate.token = ++Get().nextTransactionToken;
                    if (!candidate.token) candidate.token = ++Get().nextTransactionToken;
                    candidate.reserved = true;
                    candidate.inFlight = 1;
                    candidate.nativeQueue = request.nativeQueue;
                    candidate.nativeList = reinterpret_cast<uint64_t>(lease.NativeList());
                    candidate.runtimeGeneration = lease.Generation();
                    candidate.sdkRuntime = runtime;
                    candidate.sdkDevice = sdkDevice;
                    candidate.sdkQueue = sdkQueue;
                    candidate.sdkList = sdkList;
                    candidate.poolEntry = lease.Receipt();
                    candidate.lease = std::move(lease);
                    transaction = &candidate;
                    transactionToken = candidate.token;
                    break;
                }
            }
        }
        if (!transaction) return Reason::Capacity;
        struct PreparedSlot {
            State::Transaction* transaction;
            uint64_t token;
            StageResult& result;
            ~PreparedSlot() {
                if (result.attempt.state != SubmissionState::Untouched) return;
                OwnedRuntime::Lease abandoned;
                { std::lock_guard lock(Get().mutex);
                  if (transaction->token == token && transaction->inFlight == 1) {
                      abandoned = std::move(transaction->lease); ClearTransaction(*transaction);
                  } }
                // Never release the pool lease while holding the transaction lock.
            }
        } prepared{transaction, transactionToken, result};

        // The SDK resource handle is only a typed view of the caller's native
        // object. Keep an owning COM reference in the owned batch until the
        // same-queue completion fence retires the transaction; do not rely on
        // an undocumented SDK view lifetime to keep the external NR color
        // alive. The ComPtr itself is the payload destructor, so it performs no
        // SDK work during lease retirement.
        try {
            auto colorOwner = std::make_shared<Microsoft::WRL::ComPtr<ID3D12Resource>>(color);
            if (!transaction->lease.KeepAlive(std::static_pointer_cast<void>(colorOwner))) {
                OwnedRuntime::Lease abandonedLease;
                {
                    std::lock_guard lock(Get().mutex);
                    if (transaction->token == transactionToken) {
                        abandonedLease = std::move(transaction->lease);
                        ClearTransaction(*transaction);
                    }
                }
                abandonedLease = {};
                return Reason::Capacity;
            }
        } catch (...) {
            OwnedRuntime::Lease abandonedLease;
            {
                std::lock_guard lock(Get().mutex);
                if (transaction->token == transactionToken) {
                    abandonedLease = std::move(transaction->lease);
                    ClearTransaction(*transaction);
                }
            }
            abandonedLease = {};
            return Reason::InternalFailure;
        }

        struct ProducerOwner {
            Microsoft::WRL::ComPtr<ID3D12Fence> fence;
            Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
            native::NativeSubmission submission; // exact creator/owner/color until owned completion
            std::shared_ptr<GenshinTransport::ProducerFrame> beforeSrProducer;
            std::shared_ptr<GenshinTransport::Lease> beforeSrTransport;
            std::shared_ptr<const GenshinPresentNr::NativeColorCapability> beforeNrPicture;
        };
        auto producerOwner = std::make_shared<ProducerOwner>();
        producerOwner->fence = endpoint.ready.fence; producerOwner->queue = endpoint.ready.queue;
        producerOwner->submission = endpoint.ready.submission;
        if (endpoint.colorMode == ColorMode::GenshinNativeTypedBeforeSr) {
            // Retain full contents/transport owners, not only imported color COM.
            producerOwner->beforeSrProducer = endpoint.nativeColor.producer;
            producerOwner->beforeSrTransport = endpoint.nativeColor.transport;
        }
        if (endpoint.colorMode == ColorMode::GenshinNativeTypedBeforeNr)
            producerOwner->beforeNrPicture = endpoint.beforeNrNativeColor.picture;
        // Same completion/quarantine as color/queue/receipt, never a borrowed
        // TLS Scope as a delayed GPU owner and no separate retirement system.
        if (!transaction->lease.KeepAlive(producerOwner)) return Reason::Capacity;

        // Partial views remain caller-owned until adoption. After adoption,
        // the owned pool controls their GPU lifetime and destruction.
        const auto abortUnsubmittedViews = [&]() noexcept {
            reshade::api::device *device = nullptr;
            reshade::api::resource_view linearView = { 0 }, srgbView = { 0 };
            OwnedRuntime::Lease abandonedLease;
            {
                std::lock_guard lock(Get().mutex);
                if (transaction->token != transactionToken) return;
                device = transaction->sdkDevice;
                linearView = transaction->rtv;
                srgbView = transaction->rtvSrgb;
                abandonedLease = std::move(transaction->lease);
                ClearTransaction(*transaction);
            }
            try {
                if (linearView.handle) device->destroy_resource_view(linearView);
                if (srgbView.handle) device->destroy_resource_view(srgbView);
            } catch (...) { ++Get().overflow; }
            abandonedLease = {};
        };

        const auto* proof = endpoint.naturalSource;
        const bool typed = endpoint.output == OutputKind::Original &&
            endpoint.naturalContract == NaturalSourceContract::TypedBackbuffer && proof &&
            proof->Observation().api == 0xc000 && proof->Observation().buffer == Key(color) &&
            proof->Observation().owner == Key(nativeDevice.Get()) &&
            proof->Endpoint(endpoint.stream, endpoint.generation, endpoint.frame,
                endpoint.naturalDevice, endpoint.naturalBackbuffer) == NaturalAssociation::Predicate::Captured;
        const auto exactFormat = static_cast<reshade::api::format>(colorFormats.endpoint);
        const bool directCompatible = typed && endpoint.ready.queue == nativeQueue &&
            (knownState == D3D12_RESOURCE_STATE_PRESENT || knownState == D3D12_RESOURCE_STATE_RENDER_TARGET) &&
            (nativeDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0 && !nativeDesc.SampleDesc.Quality &&
            sdkDesc.texture.format == exactFormat && sdkDesc.texture.levels == 1 &&
            sdkDesc.texture.depth_or_layers == 1 && sdkDesc.texture.samples == 1 &&
            (sdkDesc.usage & reshade::api::resource_usage::render_target) != reshade::api::resource_usage::undefined;
        DirectRecordingScope directRecording;
        DirectSlot* directSlot = nullptr;
        auto* enrolled = directCompatible ? FindDirectChain(proof->Observation().chain) : nullptr;
        if (enrolled && enrolled->epoch != UINT64_MAX && transaction->poolEntry) {
            DirectKey key;
            key.chain = enrolled->chain.load(std::memory_order_acquire);
            key.chainLifetime = enrolled->lifetime.load(std::memory_order_acquire);
            key.chainEpoch = enrolled->epoch; key.stream = endpoint.stream; key.generation = endpoint.generation;
            key.naturalRuntime = Key(natural.runtime); key.naturalLifetime = natural.lifetime;
            key.runtime = Key(runtime); key.sdkDevice = Key(sdkDevice); key.sdkQueue = Key(sdkQueue);
            key.nativeDevice = request.nativeDevice; key.nativeQueue = request.nativeQueue; key.resource = Key(color);
            key.native = nativeDesc; key.sdk = sdkDesc; key.linear = linearFormat; key.srgb = srgbFormat;
            if (auto* cache = DirectCache(transaction->poolEntry)) {
                DirectSlot* free = nullptr;
                for (auto& slot : cache->slots) {
                    if (slot.occupied && slot.key == key) { directSlot = &slot; break; }
                    if ((!slot.occupied || (slot.revoked && !slot.owner && !DirectPending(slot))) && !free) free = &slot;
                }
                // Stable negative results are scratch-only calls, without a
                // new Recording constraint or another create_view attempt.
                if ((!directSlot && free) || (directSlot && !directSlot->failed && !directSlot->revoked))
                    directRecording.Enter(key.chain);
                if (!directSlot && free && directRecording.Current()) {
                    *free = {}; free->occupied = free->failed = true; free->key = key;
                    directSlot = free; // reserve negative result BEFORE any SDK callback
                    auto receipt = std::make_shared<DirectReceipt>();
                    auto bundle = std::make_unique<OwnedRtvBundle>();
                    bundle->retirement = receipt; bundle->directDevice = sdkDevice;
                    // Publish Recording/Owners before retaining the backbuffer;
                    // a racing wrapper cannot enter real DXGI in this interval.
                    directRecording.chain->access.fetch_or(DirectOwners, std::memory_order_acq_rel);
                    bundle->externalColor = color; bundle->color = resource;
                    std::unique_ptr<OwnedRuntime::Resource> owned(std::move(bundle));
                    if (transaction->lease.AdoptResource(owned, directSlot->owner)) {
                        directSlot->receipt = std::move(receipt);
                        // Hold a local real owner across callbacks that may revoke
                        // the cache slot. Custody still belongs to the pool.
                        auto creating = directSlot->owner;
                        auto* target = static_cast<OwnedRtvBundle*>(creating.get());
                        const reshade::api::resource_view_desc linearView(linearFormat, 0, 1, 0, 1);
                        const reshade::api::resource_view_desc srgbView(srgbFormat, 0, 1, 0, 1);
                        const auto current = [&] { return selectionBoundary.Current() && directRecording.Current() && !directSlot->revoked; };
                        const bool linearCreated = sdkDevice->create_resource_view(resource,
                            reshade::api::resource_usage::render_target, linearView, &target->linear);
                        if (!current()) return Reason::SelectionPending;
                        const bool srgbCreated = linearCreated && target->linear.handle &&
                            sdkDevice->create_resource_view(resource, reshade::api::resource_usage::render_target, srgbView, &target->srgb);
                        if (!current()) return Reason::SelectionPending;
                        directSlot->failed = !(linearCreated && srgbCreated && target->linear.handle && target->srgb.handle);
                        if (directSlot->failed) directSlot->owner.reset(); // preserve the negative key, retire actual objects
                    } else {
                        // No views exist yet; adoption failure can drop this
                        // unsubmitted COM owner without any SDK destruction.
                        owned.reset();
                    }
                }
            }
        }
        const bool direct = directSlot && !directSlot->failed && !directSlot->revoked &&
            directSlot->owner && directRecording.Current();
        if (!direct) {
            if (Get().scratchGeneration != transaction->runtimeGeneration || Get().scratchRuntime != runtime) {
                Get().scratch.reset();
                Get().scratchGeneration = transaction->runtimeGeneration;
                Get().scratchRuntime = runtime;
            }
            if (!Get().scratch) {
                auto bundle = std::make_unique<OwnedRtvBundle>();
                reshade::api::resource_desc desc{};
                desc.type = reshade::api::resource_type::texture_2d;
                desc.texture.width = endpoint.width; desc.texture.height = endpoint.height;
                desc.texture.depth_or_layers = 1; desc.texture.levels = 1; desc.texture.samples = 1;
                desc.texture.format = colorFormats.working;
                desc.heap = reshade::api::memory_heap::default_;
                desc.usage = reshade::api::resource_usage::render_target | reshade::api::resource_usage::shader_resource |
                    reshade::api::resource_usage::copy_source | reshade::api::resource_usage::copy_dest;
                const reshade::api::resource_view_desc linearView(linearFormat, 0, 1, 0, 1);
                const reshade::api::resource_view_desc srgbView(srgbFormat, 0, 1, 0, 1);
                bool created = false;
                try {
                    created = sdkDevice->create_resource(desc, nullptr, reshade::api::resource_usage::copy_dest, &bundle->color) &&
                        sdkDevice->create_resource_view(bundle->color, reshade::api::resource_usage::render_target, linearView, &bundle->linear) &&
                        sdkDevice->create_resource_view(bundle->color, reshade::api::resource_usage::render_target, srgbView, &bundle->srgb);
                } catch (...) { bundle->DestroyGpuObjects(sdkDevice); throw; }
                if (!created) { bundle->DestroyGpuObjects(sdkDevice); abortUnsubmittedViews(); return Reason::UnsupportedFormat; }
                std::unique_ptr<OwnedRuntime::Resource> resourceOwner(std::move(bundle));
                if (!transaction->lease.AdoptResource(resourceOwner, Get().scratch)) {
                    resourceOwner->DestroyGpuObjects(sdkDevice); abortUnsubmittedViews(); return Reason::Capacity;
                }
            }
        }
        const auto targetOwner = direct ? directSlot->owner : Get().scratch;
        auto* target = static_cast<OwnedRtvBundle*>(targetOwner.get());
        transaction->rtvOwner = targetOwner;
        if (!transaction->lease.KeepAlive(targetOwner)) return Reason::Capacity;
        transaction->rtv = target->linear; transaction->rtvSrgb = target->srgb;
        // Optional admission has no SR/NR or color-filter failure gate. The
        // payload owns the producer lease, not merely a COM resource reference.
        std::shared_ptr<Depth::Prepared> depthRead;
        if (depthSemanticsReady && depthEndpointMatches)
            depthRead = Depth::Prepare(depthInput, nativeDevice.Get(), sdkDevice, target->depth);
        if (depthRead && !transaction->lease.KeepAlive(depthRead)) depthRead.reset();

        const auto recordingRevoked = [&] {
            return !selectionBoundary.Current() || !sourceCurrent() || transaction->orphaned ||
                (direct && (!directRecording.Current() || directSlot->revoked));
        };
        const auto retainRevokedRecording = [&] {
            quarantineDonorReader();
            // May already have recorded a color/depth barrier. Natural
            // suppression stays after submission; no scratch retry or release.
            result.attempt.state = SubmissionState::RecordedOrUncertain;
            OwnedRuntime::Lease abandoned;
            {
                std::lock_guard lock(Get().mutex);
                transaction->orphaned = true; transaction->reserved = false;
                transaction->inFlight = 0; abandoned = std::move(transaction->lease);
            }
            return Reason::InternalFailure; // lease destructor quarantines, pool retains owners
        };
        if (recordingRevoked()) return finishUnrecorded(transaction->lease, Reason::SelectionPending);

        // Consume the recording attempt, not the natural picture. Suppression
        // waits for actual techniques and a successful owned submission.
        if (!sourceCurrent()) return finishUnrecorded(transaction->lease, Reason::SelectionPending);
        if (!AdmitColor(endpoint, result, immutable)) return finishUnrecorded(transaction->lease, Reason::ColorSpaceUnknown);
        // Register both actual picture participants BEFORE the first command
        // can record or throw. Mere enumeration/selection/warmup is not a use.
        RememberSettingsUse(natural, endpoint.stream, endpoint.generation);
        if (suppress && suppress != &natural)
            RememberSettingsUse(*suppress, endpoint.stream, endpoint.generation);
        result.attempt.state = SubmissionState::RecordedOrUncertain;

        if (recordingRevoked()) return retainRevokedRecording();

        // PRESENT/COMMON share native zero: only the live typed DXGI proof
        // above interprets it as PRESENT, on the exact submitted DIRECT queue.
        const auto directUsage = knownState == D3D12_RESOURCE_STATE_RENDER_TARGET
            ? reshade::api::resource_usage::render_target : reshade::api::resource_usage::present;
        if (direct) {
            if (directUsage != reshade::api::resource_usage::render_target)
                sdkList->barrier(resource, directUsage, reshade::api::resource_usage::render_target);
        } else {
            sdkList->barrier(resource, oldUsage, reshade::api::resource_usage::copy_source);
            if (partialColor) {
                // Exact active SR texels; no resampling/transfer conversion.
                const reshade::api::subresource_box sourceBox{endpoint.colorX, endpoint.colorY, 0,
                    endpoint.colorX + endpoint.width, endpoint.colorY + endpoint.height, 1};
                sdkList->copy_texture_region(resource, 0, &sourceBox, target->color, 0, nullptr);
            } else sdkList->copy_resource(resource, target->color);
            sdkList->barrier(target->color, reshade::api::resource_usage::copy_dest, reshade::api::resource_usage::render_target);
        }
        transitioned = true;
        if (recordingRevoked()) return retainRevokedRecording();
        ++Get().renderCalls;
        uint64_t techniquesBefore = 0, callbackGeneration = 0;
        { std::lock_guard lock(Get().mutex);
          if (auto* entry = Get().registry.Find(Key(runtime))) {
              techniquesBefore = entry->techniques; callbackGeneration = entry->generation;
          } }
        // Begin records exact native subresource barriers and reflects the
        // binding before publishing bufready_depth. End clears and restores.
        result.depthAvailable = depthRead && depthRead->Begin(runtime, sdkList, transaction->lease.NativeList());
        if (recordingRevoked()) return retainRevokedRecording();
        // Native BeforeNR 29->28 copies retain encoded values. The existing
        // API20 view pair still follows preset SRGBTexture/SRGBWriteEnabled;
        // source format alone never forces a view or an adapter conversion.
        {
            if (endpoint.stage == Stage::BeforeSr) {
                Selection::RenderScope render(natural.selection, runtime, transaction->runtimeGeneration,
                    sdkList, target->linear, target->srgb);
                if (render.Ready()) runtime->render_effects(sdkList, target->linear, target->srgb);
                result.techniques = render.RecordedTechniques();
            } else runtime->render_effects(sdkList, target->linear, target->srgb);
        }
        if (recordingRevoked()) return retainRevokedRecording();
        if (depthRead) depthRead->End(runtime, sdkList, transaction->lease.NativeList());
        else Depth::Clear(runtime);
        if (recordingRevoked()) return retainRevokedRecording();
        if (endpoint.stage != Stage::BeforeSr) {
            std::lock_guard lock(Get().mutex);
            if (auto* entry = Get().registry.Find(Key(runtime)); entry && callbackGeneration &&
                entry->generation == callbackGeneration && entry->techniques >= techniquesBefore)
                result.techniques = entry->techniques - techniquesBefore;
        }
        // Dynamic producer/lease/four-resource checks are repeated at the
        // copyback boundary; only allocation descriptors/COM owners are reused.
        if (!AdmitColor(endpoint, result, immutable)) return retainRevokedRecording();
        const bool effectsRecorded = result.techniques != 0 && sourceCurrent();
        if (direct) {
            if (!effectsRecorded) result.techniques = 0;
            if (directUsage != reshade::api::resource_usage::render_target)
                sdkList->barrier(resource, reshade::api::resource_usage::render_target, directUsage);
        } else if (effectsRecorded) {
            sdkList->barrier(target->color, reshade::api::resource_usage::render_target, reshade::api::resource_usage::copy_source);
            sdkList->barrier(resource, reshade::api::resource_usage::copy_source, reshade::api::resource_usage::copy_dest);
            if (partialColor) {
                // Preserve every texel outside the selected SR input rectangle.
                const reshade::api::subresource_box destinationBox{endpoint.colorX, endpoint.colorY, 0,
                    endpoint.colorX + endpoint.width, endpoint.colorY + endpoint.height, 1};
                sdkList->copy_texture_region(target->color, 0, nullptr, resource, 0, &destinationBox);
            } else sdkList->copy_resource(target->color, resource);
            sdkList->barrier(resource, reshade::api::resource_usage::copy_dest, oldUsage);
            sdkList->barrier(target->color, reshade::api::resource_usage::copy_source, reshade::api::resource_usage::copy_dest);
        } else {
            // Empty permutation/warmup never writes output or hides natural
            // effects. Submit solely to retire the recorded scratch work.
            result.techniques = 0;
            sdkList->barrier(resource, reshade::api::resource_usage::copy_source, oldUsage);
            sdkList->barrier(target->color, reshade::api::resource_usage::render_target, reshade::api::resource_usage::copy_dest);
        }
        transitioned = false;
        if (recordingRevoked()) return retainRevokedRecording();

        if (selectionDonor && (!sourceCurrent() || !AdmitColor(endpoint, result, immutable)))
            return retainRevokedRecording();
        OwnedRuntime::Completion completion;
        const auto submitStatus = transaction->lease.Submit(completion, &ownedDetail);
        const bool unknownDonorCompletion = selectionDonor && submitStatus == OwnedRuntime::Status::Ready &&
            (!completion.fence || !completion.value || completion.value == UINT64_MAX ||
             completion.fence->GetCompletedValue() == UINT64_MAX);
        if (unknownDonorCompletion) {
            quarantineDonorReader();
            // Even a nominally successful Submit cannot certify MAX/device
            // removal. Keep the actual active pool lease/payloads indefinitely.
            std::lock_guard lock(Get().mutex);
            transaction->completion = completion; transaction->orphaned = transaction->signalFailed = true;
            transaction->reserved = false; transaction->inFlight = 0;
            return Reason::FenceUnavailable;
        }
        if (submitStatus != OwnedRuntime::Status::Ready || !completion.fence || !completion.value ||
            completion.value == UINT64_MAX || completion.fence->GetCompletedValue() == UINT64_MAX) {
            quarantineDonorReader();
            OwnedRuntime::Lease failedLease;
            {
                std::lock_guard lock(Get().mutex);
                transaction->completion = std::move(completion);
                transaction->signalFailed = true; transaction->orphaned = true;
                transaction->reserved = false; transaction->inFlight = 0;
                // Submit has revoked recording access even when it returns a
                // failure.  Release the active lease after leaving this mutex;
                // the quarantined owned entry and its views remain retained.
                failedLease = std::move(transaction->lease);
                ++Get().submitFailures;
                const auto fields = OwnedRuntime::Fields(ownedDetail);
                LOG_WARN("HoYoShade stage owned submission failed token={} status={} owned_schema={} owned_status={} owned_operation={} owned_code_kind={} owned_code=0x{:08x} resources=retained",
                    transactionToken, static_cast<unsigned>(submitStatus), OwnedRuntime::DiagnosticSchemaVersion,
                    fields.status, fields.operation, fields.codeKind, fields.code);
            }
            failedLease = OwnedRuntime::Lease{};
            return Reason::FenceUnavailable;
        }
        const bool readerCompleted = !depthRead || depthRead->Complete(completion.fence.Get(), completion.value);
        if (!readerCompleted) {
            LOG_WARN("HoYoShade depth reader completion refused; owner retained sentinel=RESHADE-REAL-DEPTH");
            if (selectionDonor) {
                // The reader quarantines its content lease. Keep THIS active
                // transaction/pool lease and all payloads, not just addresses;
                // do not certify a consumer or suppress the natural runtime.
                std::lock_guard lock(Get().mutex);
                transaction->completion = completion; transaction->orphaned = true;
                transaction->reserved = false; transaction->inFlight = 0;
                return Reason::FenceUnavailable;
            }
        }
        result.completion = completion;
        OwnedRuntime::Lease staleLease;
        bool stale = false;
        {
            std::lock_guard lock(Get().mutex);
            if (transaction->token != transactionToken || transaction->orphaned) {
                transaction->completion = std::move(completion);
                transaction->reserved = false; transaction->inFlight = 0;
                staleLease = std::move(transaction->lease);
                stale = true;
            }
            else {
                transaction->completion = std::move(completion);
                transaction->recorded = true;
                transaction->submitted = true;
                transaction->reserved = false;
                transaction->inFlight = 0;
                ++Get().renderRecorded;
            }
        }
        if (stale) {
            result.attempt.state = SubmissionState::RecordedOrUncertain;
            staleLease = OwnedRuntime::Lease{};
            LOG_WARN("HoYoShade stage owned render became stale token={} resources=retained", transactionToken);
            return Reason::InternalFailure;
        }
        if (result.techniques) {
            // A lifecycle change after recording is uncertain output, not a
            // successful application against a replacement natural runtime.
            if (!sourceCurrent()) return Reason::NaturalUnavailable;
            if (selectionDonor) {
                if (suppress != &natural || !AdmitColor(endpoint, result, immutable) ||
                    !SuppressBeforeSrSelectionDonor(natural, *selectionDonor, result.attempt))
                    return Reason::NaturalUnavailable;
            } else if (suppress) {
                if (!suppress->frame.Arm()) return Reason::NaturalFrameConsumed;
                result.attempt.naturalSuppressed = true;
                suppress->runtime->render_effects(naturalList, {0}, {0});
            }
        }
        result.attempt.state = SubmissionState::Submitted;
        const auto report = Get().executionReports.Next(Diagnostics::ExecutionStream::OwnedSubmit);
        if (report.emit) LOG_INFO("HoYoShade stage owned render submitted report_count={} runtime_generation={} format={} state={} techniques={} fence_value={} effects_executed={} natural_suppressed={}",
            report.count, transaction->runtimeGeneration, static_cast<unsigned>(nativeDesc.Format),
            static_cast<unsigned>(knownState), result.techniques, transaction->completion.value,
            result.EffectsExecuted(), result.attempt.naturalSuppressed);
        return result.techniques ? Reason::ExecutionUnverified : Reason::SelectionPending;
    } catch (...) {
        ++Get().overflow;
        (void)transitioned; // original state is uncertain; retain the whole batch
        if (result.attempt.state != SubmissionState::Untouched) quarantineDonorReader();
        OwnedRuntime::Lease abandonedLease;
        if (transaction) {
            std::lock_guard lock(Get().mutex);
            if (transaction->token == transactionToken) {
                transaction->orphaned = true;
                transaction->reserved = false;
                transaction->recorded = false;
                transaction->submitted = false;
                transaction->inFlight = 0;
                // The SDK list may already contain recorded commands.  Keep
                // its views and quarantine the owned runtime; only release
                // the active lease after the transaction mutex is unlocked.
                abandonedLease = std::move(transaction->lease);
            }
        }
        abandonedLease = OwnedRuntime::Lease{};
        return Reason::InternalFailure;
    }
}
// Only this TLS interval may consume a view event. Unrelated application/core
// resource creation pays a TLS null check and changes no integration state.
void OnDomainView(reshade::api::device* device,reshade::api::resource resource,
    reshade::api::resource_usage usage,const reshade::api::resource_view_desc&,reshade::api::resource_view view) noexcept
{
    auto* capture=DeviceDomain::Capture::Current();if(!capture)return;
    try {
        std::lock_guard lifetime(Get().executionMutex);
        if(!Get().accepted.load(std::memory_order_acquire) || !Get().domainSubscriptionRequested.load(std::memory_order_acquire) ||
            inCallback || ownedOperation || !device || usage!=reshade::api::resource_usage::render_target) {
            capture->Fail(DeviceDomain::Result::CallbackMismatch);return;
        }
        if(!capture->BeginCallback())return;
        struct Interval {Interval(){inCallback=true;}~Interval(){inCallback=false;}} interval;
        const auto api=static_cast<unsigned>(device->get_api());
        // get_native is compared as a scalar only. No cast/dereference/unwrap.
        capture->EndCallback(api,Key(device),device->get_native(),resource.handle,view.handle);
    } catch(...) {capture->Fail(DeviceDomain::Result::Exception);}
}
void OnList(reshade::api::command_list* list) noexcept
{
    try { CallbackGuard guard; if (guard.entered) EnrollList(list); } catch (...) { ++Get().overflow; }
}
void OnDestroyList(reshade::api::command_list* list) noexcept
{
    RevokeObservationDemand();
    try {
        CallbackGuard guard; if (!guard.entered) return;
        std::lock_guard lock(Get().mutex); Get().registry.DestroyList(Key(list));
        for (auto &transaction : Get().transactions)
                if (transaction.sdkList == list) transaction.orphaned = true;
    } catch (...) {}
}
void OnDestroyQueue(reshade::api::command_queue* queue) noexcept
{
    RevokeObservationDemand();
    try {
        CallbackGuard guard; if (!guard.entered) return;
        uint64_t owner = 0;
        void *nativeQueue = queue ? reinterpret_cast<void *>(queue->get_native()) : nullptr;
        {
            std::lock_guard lock(Get().mutex);
            Get().registry.DestroyQueue(Key(queue));
            for (auto &transaction : Get().transactions)
                if (transaction.sdkQueue == queue) { transaction.orphaned = true; owner = Get().ownedBindingOwner; }
            if (owner == 0 && nativeQueue && Get().ownedBinding.queue == nativeQueue)
                owner = Get().ownedBindingOwner;
            if (owner && nativeQueue && Get().ownedBinding.queue == nativeQueue) {
                InvalidateDirect();
                Get().ownedBinding = {};
                Get().ownedDevice.Reset();
                Get().ownedQueue.Reset();
                Get().ownedFactory.Reset();
                Get().ownedBindingOwner = 0;
            }
        }
        if (owner) OwnedRuntime::Retire(owner, 0);
    } catch (...) {}
}
void OnDestroyDevice(reshade::api::device* device) noexcept
{
    RevokeObservationDemand();
    try {
        CallbackGuard guard; if (!guard.entered) return;
        uint64_t owner = 0;
        void *nativeDevice = device ? reinterpret_cast<void *>(device->get_native()) : nullptr;
        {
            std::lock_guard lock(Get().mutex);
            Get().registry.DestroyDevice(Key(device));
            for(auto& natural:Get().natural)if(natural.id.sdkDevice==Key(device)) {
                natural.frame.Invalidate();natural.domains={};
            }
            if(auto* capture=DeviceDomain::Capture::Current())capture->Fail(DeviceDomain::Result::LifetimeChanged);
            for (auto &transaction : Get().transactions)
                if (transaction.sdkDevice == device) { transaction.orphaned = true; owner = Get().ownedBindingOwner; }
            if (owner == 0 && nativeDevice && Get().ownedBinding.device == nativeDevice)
                owner = Get().ownedBindingOwner;
            if (owner && nativeDevice && Get().ownedBinding.device == nativeDevice) {
                InvalidateDirect();
                Get().ownedBinding = {};
                Get().ownedDevice.Reset();
                Get().ownedQueue.Reset();
                Get().ownedFactory.Reset();
                Get().ownedBindingOwner = 0;
            }
        }
        if (owner) OwnedRuntime::Retire(owner, 0);
    } catch (...) {}
}
void OnSwapchainInit(reshade::api::swapchain* swapchain, bool) noexcept
{
    try {
        CallbackGuard guard; if (!swapchain) return;
        const auto chain = swapchain->get_native();
        if (auto* direct = FindDirectChain(chain)) {
            direct->draining = (direct->access.load(std::memory_order_acquire) & DirectOwners) != 0;
            if (direct->epoch != UINT64_MAX) ++direct->epoch;
            InvalidateDirect(direct->chain.load(std::memory_order_acquire));
        }
        if (!guard.entered || ownedOperation) return;
        OwnedStage::TopologyChanged();
        const auto device = Key(swapchain->get_device());
        std::lock_guard lock(Get().mutex);
        Get().registry.InvalidateDevice(device);
        for (auto& entry : Get().natural)
            if (entry.id.sdkDevice == device) {
                entry.frame.Invalidate();entry.domains={};entry.domainBuffersPending=true;
            }
        if(auto* capture=DeviceDomain::Capture::Current())capture->Fail(DeviceDomain::Result::LifetimeChanged);
    } catch (...) {}
}
void OnSwapchainDestroy(reshade::api::swapchain* swapchain, bool) noexcept
{ OnSwapchainInit(swapchain, false); }
// Publish data only; no SDK calls, allocation, logging or execution decisions.
// Path writes occur solely at the two pre-existing Capture observation sites.
void CacheSelection(State::Natural& natural, const SelectionObservation& value,
                    std::string_view preset) noexcept
{
    natural.captureObserved = true;
    natural.selectionStatus = value;
    const bool known = value.presetKnown && !preset.empty() && preset.size() < natural.presetPath.size();
    natural.selectionStatus.presetKnown = known;
    if (known) {
        std::memcpy(natural.presetPath.data(), preset.data(), preset.size());
        natural.presetPath[preset.size()] = 0;
    } else natural.presetPath[0] = 0;
}
// Reuse only facts collected by the existing Capture/Apply calls. Failed graph
// capture may still have complete current live technique states; retained OLD
// graph handles/values never become selection readiness or diagnostic counts.
void CacheCompleteSelection(State::Natural& natural, uint64_t selectionRevision) noexcept
{
    const auto& source = natural.selection;
    SelectionObservation value;
    value.capture = source.capture; value.applied = source.applied;
    value.snapshots = source.snapshots; value.graphBuilds = source.graphBuilds;
    value.captureReady = source.valid;
    value.selectionReady = source.valid && !source.config.empty();
    value.presetKnown = source.capture.presetKnown && !source.preset.empty();
    value.globalKnown = source.capture.globalKnown;
    value.globalEnabled = value.globalKnown && source.capture.globalEnabled;
    value.countsKnown = source.capture.techniqueEnumComplete && source.capture.statesComplete &&
        source.naturalRuntime == natural.runtime && source.naturalGeneration == natural.lifetime &&
        source.techniques.size() == source.capture.techniques && source.techniques.size() <= Selection::MaxItems;
    if (value.countsKnown) {
        value.total = source.techniques.size();
        value.enabled = source.capture.enabled;
        for (size_t i = 0; i < value.total; ++i)
            if (source.techniques[i].enabled) value.enabledBits[i / 64] |= uint64_t{1} << (i % 64);
    }
    CacheSelection(natural, value, source.preset);
    // Only the already performed live Capture can publish zero selection. A
    // failed private snapshot, loading/empty topology or an enabled-but-unready
    // technique is not idle. Requests revoke the observation revision before the setter.
    natural.idleSelectionMutation = natural.idleSelectionRevision = 0;
    const auto& events = Selection::CacheEvents();
    const auto watched = Selection::Watch(natural.runtime).Read();
    if (value.countsKnown && value.globalKnown && !value.enabled &&
        (source.capture.reason == Selection::CaptureReason::NoEnabledTechniques ||
         source.capture.reason == Selection::CaptureReason::GlobalDisabled) &&
        !source.refresh && watched.runtime == natural.runtime && watched.revision &&
        source.naturalRevision == watched.revision && events.mutation &&
        !events.exhausted && !events.mutationExhausted && selectionRevision &&
        selectionRevision != UINT64_MAX && selectionRevision == Get().observationSelectionRevision) {
        natural.idleSelectionMutation = events.mutation;
        natural.idleSelectionRevision = selectionRevision;
    }
}
// Called after the real callback Capture and its SDK-free Cache above. The
// bounded existing stream does not re-query the runtime or infer preset state.
// Called only AFTER the existing selection ChangeReports decision emits.
// No API getter/enumeration, preset inference, string construction or heap growth.
void ReportEnabledSelectionKeys(const State::Natural& natural, const SelectionObservation& value,
    uint64_t observation)
{
    constexpr size_t MaxKeys = 8, MaxNameBytes = 128, MaxEffectBytes = 192;
    const auto& metadata = natural.selection.techniques;
    const bool keysKnown = value.countsKnown && value.total == metadata.size() && value.total <= Selection::MaxItems;
    size_t reported = 0;
    bool textTruncated = false;
    if (keysKnown) for (size_t i = 0; i < value.total && reported < MaxKeys; ++i) {
        if (!(value.enabledBits[i / 64] & (uint64_t{1} << (i % 64)))) continue;
        const auto& item = metadata[i];
        const auto nameSize = item.name.size() < MaxNameBytes ? item.name.size() : MaxNameBytes;
        const auto effectSize = item.effect.size() < MaxEffectBytes ? item.effect.size() : MaxEffectBytes;
        const bool clipped = nameSize != item.name.size() || effectSize != item.effect.size();
        textTruncated = textTruncated || clipped;
        LOG_INFO("HoYoShade selection enabled key observation={} natural_runtime=0x{:x} natural_lifetime={} key_index={} technique_index={} enabled_key={}@{} name_bytes={} effect_bytes={} key_text_truncated={} live_enabled=callback_cache metadata=cached_api20 global_enabled={} pixels=UNKNOWN",
            observation, Key(natural.runtime), natural.lifetime, reported, i,
            std::string_view(item.name.data(), nameSize), std::string_view(item.effect.data(), effectSize),
            item.name.size(), item.effect.size(), clipped,
            value.globalKnown ? (value.globalEnabled ? "true" : "false") : "UNKNOWN");
        ++reported;
    }
    LOG_INFO("HoYoShade selection enabled keys observation={} natural_runtime=0x{:x} natural_lifetime={} keys_known={} keys_reported={} keys_limit=8 enabled_keys_truncated={} key_text_truncated={} source=complete_cached_live_enabled_bits_and_technique_metadata preset_inference=false pixels=UNKNOWN",
        observation, Key(natural.runtime), natural.lifetime, keysKnown, reported,
        keysKnown ? (value.enabled > reported ? "true" : "false") : "UNKNOWN", textTruncated);
}
// R196: only on an existing selection report or an actual owner Capture. No
// heartbeat, SDK getter, INI read, extra lock or admission decision is added.
void ReportSelectionEvidence(reshade::api::effect_runtime* runtime, uint64_t lifetime,
    uint64_t settings, DWORD initThread, const Selection::Source& source, bool ownerCapture) noexcept
{
    try {
        const auto& owner = OwnedStage::Get();
        struct Evidence {
            uintptr_t runtime = 0, core = 0, sourceRuntime = 0, ownerRuntime = 0;
            uint64_t lifetime = 0, sourceLifetime = 0, ownerLifetime = 0, settings = 0;
            DWORD initThread = 0, callbackThread = 0;
            Selection::CaptureFacts capture;
            bool valid = false, files = false, config = false, frozen = false, keysKnown = false;
            bool operator==(const Evidence&) const = default;
        } value;
        value.runtime = Key(runtime); value.core = Key(Get().coreModule);
        value.lifetime = lifetime; value.settings = settings;
        value.sourceRuntime = Key(source.naturalRuntime); value.sourceLifetime = source.naturalGeneration;
        value.ownerRuntime = owner.natural; value.ownerLifetime = owner.lifetime;
        value.initThread = initThread; value.callbackThread = GetCurrentThreadId();
        value.capture = source.capture; value.valid = source.valid;
        value.files = !!source.files; value.config = !source.config.empty(); value.frozen = !!owner.snapshot;
        value.keysKnown = source.capture.techniqueEnumComplete && source.capture.statesComplete &&
            source.naturalRuntime == runtime && source.naturalGeneration == lifetime &&
            source.techniques.size() == source.capture.techniques && source.techniques.size() <= Selection::MaxItems;
        const bool pathKnown = source.capture.presetKnown && !source.preset.empty();
        const std::string_view path = pathKnown ? std::string_view(source.preset) : "UNKNOWN";
        struct Stream {
            Evidence last;
            std::string path;
            uint64_t time = 0;
            unsigned emitted = 0;
        };
        static std::array<Stream, 2> streams;
        auto& stream = streams[ownerCapture ? 1 : 0];
        if (stream.emitted >= 16 || (stream.emitted && value == stream.last && path == stream.path)) return;
        const auto now = GetTickCount64();
        // An owner Capture is a one-shot attempt: do not lose its terminal
        // failure merely because the previous attempt was less than 1s ago.
        // Its process-lifetime 16-transition ceiling is still unconditional.
        if (!ownerCapture && stream.emitted && now - stream.time < 1000) return;
        const char* coreChanged = !stream.emitted ? "UNKNOWN" :
            value.core != stream.last.core ? "true" : "false";
        const char* runtimeChanged = !stream.emitted ? "UNKNOWN" :
            (value.runtime != stream.last.runtime || value.lifetime != stream.last.lifetime) ? "true" : "false";
        const char* ownerMatch = !owner.natural ? "unclaimed" :
            owner.natural != value.runtime ? "runtime-mismatch" :
            owner.lifetime != lifetime ? "lifetime-mismatch" : "matched";
        const char* keysState = !value.keysKnown ? "unknown" :
            !source.capture.enabled ? "known-empty-enabled" : "known-nonempty-enabled";
        const char* topology = !source.capture.techniqueEnumComplete ? "not-complete" :
            !source.capture.techniques ? "complete-empty" : "complete-nonempty";
        // Commit only diagnostic storage; exceptions cannot affect owner state.
        stream.path.assign(path); stream.last = value; stream.time = now; ++stream.emitted;
        LOG_INFO("HoYoShade selection source-evidence R196 origin={} observation={} report_limit=16 report_min_ms={} heartbeat=false runtime=0x{:x} lifetime={} source_runtime=0x{:x} source_lifetime={} core_module=0x{:x} core_module_changed={} runtime_lifetime_changed={} init_tid={} callback_tid={} init_thread_match={} owner_runtime=0x{:x} owner_lifetime={} owner_match={} config_settings={} capture_missing_reason={} private_snapshot_reason={} private_snapshot_runtime=0x{:x} private_snapshot_lifetime={} private_snapshot_reason_scope=last-attempt private_snapshot_files={} private_snapshot_config={} capture_snapshot_ready={} frozen_owner_snapshot_present={} capture_valid={} keys_state={} topology_state={} enabled_seen={} preset_path_read={} source_preset={} preset_known={} preset_path_source=existing_get_current_preset_path applied_preset=UNKNOWN enabled_source=existing_live_technique_states admission_unchanged=true pixels=UNKNOWN",
            ownerCapture ? "persistent-owner-capture" : "natural-callback-cache", stream.emitted, ownerCapture ? 0 : 1000,
            value.runtime, lifetime, value.sourceRuntime, value.sourceLifetime, value.core, coreChanged, runtimeChanged,
            initThread, value.callbackThread, !initThread ? "UNKNOWN" : initThread == value.callbackThread ? "true" : "false",
            owner.natural, owner.lifetime, ownerMatch, settings,
            Selection::Name(source.capture.reason), Selection::Name(source.capture.snapshotReason),
            source.capture.snapshotRuntime, source.capture.snapshotLifetime,
            value.files, value.config, source.capture.snapshotReady, value.frozen, value.valid, keysState, topology,
            source.capture.enabled, Selection::Name(source.capture.presetRead), path, pathKnown);
        // Only the owner's already captured metadata; eight bounded keys per
        // emitted transition, no API enumeration or per-draw lookup.
        if (ownerCapture && value.keysKnown) {
            size_t reported = 0;
            for (size_t i = 0; i < source.techniques.size() && reported < 8; ++i) {
                const auto& item = source.techniques[i];
                if (!item.enabled) continue;
                const auto name = std::string_view(item.name).substr(0, 128);
                const auto effect = std::string_view(item.effect).substr(0, 192);
                LOG_INFO("HoYoShade selection enabled key origin=persistent-owner-capture observation={} natural_runtime=0x{:x} natural_lifetime={} key_index={} enabled_key={}@{} key_text_truncated={} preset_inference=false pixels=UNKNOWN",
                    stream.emitted, value.runtime, lifetime, reported++, name, effect,
                    name.size() != item.name.size() || effect.size() != item.effect.size());
            }
        }
    } catch (...) {} // No effect on capture, suppression, owner acknowledgement or rendering.
}
void ReportOwnedSelectionCapture(reshade::api::effect_runtime* runtime, uint64_t lifetime,
    uint64_t settings, const Selection::Source& source) noexcept
{
    DWORD initThread = 0;
    for (const auto& natural : Get().natural)
        if (natural.runtime == runtime && natural.lifetime == lifetime) { initThread = natural.thread; break; }
    ReportSelectionEvidence(runtime, lifetime, settings, initThread, source, true);
}
void ReportSelection(State::Natural& natural, uint64_t generation) noexcept
{
    try {
        auto value = natural.selectionStatus;
        value.generation = generation; value.lifetime = natural.lifetime;
        const auto& capture = value.capture;
        const auto& applied = value.applied; // last actual Apply attempt, not current admission
        const std::string_view preset = value.presetKnown ? natural.presetPath.data() : "UNKNOWN";
        const auto report = natural.selectionReports.Next(value, GetTickCount64(), preset);
        if (report.emit) {
            LOG_INFO("HoYoShade selection diagnostics source=natural_api20 natural_runtime=0x{:x} natural_generation={} natural_lifetime={} observation={} source_preset={} preset_known={} global_enabled={} techniques_total={} techniques_enabled={} counts_known={} capture_ready={} selection_ready={} capture_reason={} snapshot_ready={} technique_enum_complete={} technique_states_complete={} capture_techniques_seen={} capture_enabled={} selected_effects={} uniform_enum_complete={} capture_uniforms_seen={} dynamic_uniforms={} selected_scalars={} graph_complete={} graph_reason={} topology_cached={} uniforms_cached={} count_limit={} seen_count_policy=saturated_lower_bound_until_enum_complete owned_last_apply_reason={} owned_last_techniques_seen={} owned_last_technique_enum_complete={} owned_last_uniforms_seen={} owned_last_uniform_enum_complete={} owned_last_graph_complete={} owned_last_mapping_complete={} owned_last_mapping_cached={} loading=UNKNOWN loading_observation=api20_not_exposed snapshot={} graph_builds={} natural_files=read_only live_values=callback_cache pixels=UNKNOWN",
                Key(natural.runtime), generation, natural.lifetime, report.count, preset, value.presetKnown,
                value.globalKnown ? (value.globalEnabled ? "true" : "false") : "UNKNOWN",
                value.countsKnown ? std::to_string(value.total) : "UNKNOWN",
                value.countsKnown ? std::to_string(value.enabled) : "UNKNOWN", value.countsKnown,
                value.captureReady, value.selectionReady, Selection::Name(capture.reason), capture.snapshotReady,
                capture.techniqueEnumComplete, capture.statesComplete, capture.techniques, capture.enabled, capture.effects,
                capture.graph.enumComplete, capture.graph.uniforms, capture.graph.dynamicUniforms, capture.graph.scalars,
                capture.graph.complete, Selection::Name(capture.graph.reason), capture.topologyCached, capture.uniformsCached,
                Selection::MaxItems, Selection::Name(applied.reason), applied.techniques, applied.techniqueEnumComplete,
                applied.graph.uniforms, applied.graph.enumComplete, applied.graph.complete, applied.mappingComplete, applied.cached,
                value.snapshots, value.graphBuilds);
            ReportEnabledSelectionKeys(natural, value, report.count);
            ReportSelectionEvidence(natural.runtime, natural.lifetime,
                Get().appliedSettings.load(std::memory_order_acquire), natural.thread, natural.selection, false);
        }
    } catch (...) {} // Diagnostics never change execution/suppression.
}
using RuntimeEvent = AssociationDiagnostics::Event;
// One fixed diagnostic stream, separate from owned callbacks and execution.
// The scope also covers invalid identity, tombstone and capacity early exits.
// All construction/destruction is under the caller's CallbackGuard executionMutex.
// Destroy counts only a previously tracked LIVE natural runtime: untracked owned
// collection outside OwnedOperation must not masquerade as a natural callback.
struct NaturalCallbackReportScope
{
    reshade::api::effect_runtime* runtime;
    RuntimeEvent event;
    bool natural = !ownedOperation;
    Identity id;
    NaturalCallbackReportScope(reshade::api::effect_runtime* value, RuntimeEvent kind) noexcept
        : runtime(value), event(kind)
    {
        if (natural && event == RuntimeEvent::Destroy) {
            natural = false;
            for (const auto& entry : Get().natural)
                if (runtime && entry.runtime == runtime && entry.live) { natural = true; break; }
        }
        Get().naturalCallbacks.Observe(event, !natural);
    }
    ~NaturalCallbackReportScope() noexcept
    {
        if (!natural) return;
        try {
            AssociationDiagnostics::CallbackSnapshot value;
            value.candidate = {Key(runtime), 0, id.nativeDevice, id.nativeQueue, 0, id.api};
            value.identityAccepted = id.runtime != 0;
            for (const auto& entry : Get().natural) if (entry.runtime == runtime && runtime) {
                value.enrolled = true; value.live = entry.live;
                value.enrolledLifetime = entry.lifetime;
                value.candidate.lifetime = entry.lifetime;
                value.candidate.backbuffer0 = entry.backbuffers[0];
                break;
            }
            const auto report = Get().naturalCallbackReports.Next(value, GetTickCount64());
            if (report.emit) {
                const auto& counts = Get().naturalCallbacks;
                LOG_INFO("HoYoShade natural callback event={} observation={} runtime=0x{:x} api=0x{:x} native_device=0x{:x} native_queue=0x{:x} backbuffer0=0x{:x} identity_accepted={} enrolled={} live={} lifetime={} init={} present={} destroy={} begin={} finish={} technique={} reload={} stage_attempts={} present_attempts={} owned_callbacks=excluded destroy_count_policy=tracked-live-natural-only pixels=UNKNOWN",
                    AssociationDiagnostics::Text(event), report.count, Key(runtime), id.api, id.nativeDevice, id.nativeQueue,
                    value.candidate.backbuffer0, value.identityAccepted, value.enrolled, value.live, value.enrolledLifetime,
                    counts.Count(RuntimeEvent::Init), counts.Count(RuntimeEvent::Present), counts.Count(RuntimeEvent::Destroy),
                    counts.Count(RuntimeEvent::Begin), counts.Count(RuntimeEvent::Finish), counts.Count(RuntimeEvent::Technique),
                    counts.Count(RuntimeEvent::Reload), Get().stageAttempts, Get().presentAttempts);
            }
        } catch (...) {} // Never change a callback's state transition or exception path.
    }
};
void ObserveRuntime(reshade::api::effect_runtime* runtime, RuntimeEvent event)
{
    // SDK virtual calls happen only inside the core's live-object callback, never
    // while our mutex is held, nor from an NR/resource-tracking hook.
    NaturalCallbackReportScope delivery{runtime, event};
    const auto id = ReadIdentity(runtime);
    delivery.id = id;
    if (!id.runtime) {
        // Losing natural GPU identity revokes publication. Restoration of its
        // remembered mode still uses this live owner callback/lifetime only.
        if (event == RuntimeEvent::Present && OwnedStage::OwnsNatural(runtime)) {
            const auto lifetime = OwnedStage::Get().lifetime;
            OwnedStage::OwnerPresent(runtime, lifetime, Get().api, 0, false, false);
        }
        return;
    }
    State::Natural* capturedNatural = nullptr;
    if (!ownedOperation) {
        State::Natural* natural = nullptr;
        for (auto& entry : Get().natural) if (entry.runtime == runtime) { natural = &entry; break; }
        // Only a live init callback can publish an executable pointer. An
        // observation delivered after destroy cannot resurrect a runtime.
        if (!natural && (event == RuntimeEvent::Init || event == RuntimeEvent::Present))
            for (auto& entry : Get().natural) if (!entry.runtime) { natural = &entry; break; }
        if (!natural) {
            Get().naturalStatusIncomplete = true;
            OwnedStage::TopologyChanged();
        }
        if (natural) {
            if (event == RuntimeEvent::Init || !natural->runtime) {
                OwnedStage::TopologyChanged();
                if (OwnedStage::OwnsNatural(runtime)) OwnedStage::Changed(runtime, true);
                *natural = {}; natural->runtime = runtime; natural->id = id; natural->live = true;
                natural->thread = GetCurrentThreadId();
                if (Get().nextNaturalLifetime == UINT64_MAX) {
                    natural->live = false; natural->frame.Invalidate(); Get().naturalStatusIncomplete = true; return;
                }
                natural->lifetime = ++Get().nextNaturalLifetime;
                const auto count = runtime->get_back_buffer_count();
                if (!count || count > 8) natural->frame.Invalidate();
                else for (uint32_t i = 0; i < count; ++i)
                    natural->backbuffers[i] = runtime->get_back_buffer(i).handle;
            }
            if (!natural->live) return; // destroy tombstone; only init can revive pointer reuse
            // Configuration ownership is independent of the producer picture.
            // This is the LIVE natural owner callback, not its cached init TID.
            if (event == RuntimeEvent::Present) {
                const bool sameDomain = natural->id == id;
                const bool requested = sameDomain && OwnedStageRequested();
                unsigned donors = 0;
                for (const auto& donor : Get().natural) donors += donor.live && donor.id.api == 0xb000;
                if (id.api == 0xb000 || OwnedStage::OwnsNatural(runtime)) {
                    const auto before = OwnedStage::Get().phase;
                    OwnedStage::OwnerPresent(runtime, natural->lifetime, Get().api,
                        OwnedStageSettings(), requested, donors == 1 && !Get().naturalStatusIncomplete,
                        ReportOwnedSelectionCapture);
                    const auto& persistent = OwnedStage::Get();
                    if (persistent.natural && (persistent.natural != Key(runtime) || persistent.lifetime != natural->lifetime))
                        ReportSelectionEvidence(runtime, natural->lifetime, OwnedStageSettings(),
                            natural->thread, natural->selection, false); // cached facts, no new Capture/getter
                    if (persistent.phase != before)
                        LOG_INFO("HoYoShade persistent owner transition from={} to={} runtime=0x{:x} lifetime={} config_revision={} private_generation={} restoration_debt={} completion_unknown={} persistent_mode=true config_same_frame=not-required pixels=UNKNOWN",
                            OwnedStage::Text(before), OwnedStage::Text(persistent.phase), Key(runtime), natural->lifetime,
                            persistent.revision, persistent.binding.generation, persistent.owesRestore, persistent.uncertain);
                }
            }
            if (!(natural->id == id)) {
                OwnedStage::Changed(runtime);
                natural->frame.Invalidate(); Selection::InvalidateNatural(natural->selection, true); return;
            } // changed SDK domain needs a new init
#ifndef HOYOSHADE_STAGE_TEST
            // API20 documents D3D11 device.get_native() as ID3D11Device*.
            // Query only in this live owner callback, never from the NR thread.
            // Observation is bounded to eight reason transitions per natural lifetime.
            // This reads scalar gates only; it does not authorize or move the QI.
            if (!natural->nativeCanonical.attempted && natural->naturalQiGateReports < 8) {
                try { // A diagnostic failure must not suppress the original QI or callback.
                    const DWORD callbackThread = GetCurrentThreadId();
                    const uint64_t desired = Get().desiredSettings.load(std::memory_order_acquire);
                    const uint64_t applied = Get().appliedSettings.load(std::memory_order_acquire);
                    const bool connecting = Get().connecting.load(std::memory_order_acquire);
                    const bool desiredEnabled = SettingsEnabled(desired);
                    const bool appliedEnabled = SettingsEnabled(applied);
                    const unsigned desiredStage = SettingsStage(desired);
                    const unsigned appliedStage = SettingsStage(applied);
                    const bool selectedSnapshot = desired && desired == applied && appliedEnabled &&
                        !connecting && appliedStage == static_cast<unsigned>(Stage::BeforeNr);
                    static const auto diagnosticGame = [] {
                        const auto name = Util::ExePath().filename().wstring();
                        return GameRouting::IdentifyExecutable(name.c_str());
                    }();
                    const auto& diagnosticConfig = *Config::Instance();
                    const bool genshin = diagnosticGame == GameRouting::Identity::Genshin;
                    const bool nrEnabled = diagnosticConfig.DlssNrEnabled.value_or_default();
                    const bool finishedPicture = diagnosticConfig.DlssNrFinishedPicture.value_or_default();
                    const bool beforeSr = diagnosticConfig.DlssNrRunBeforeSr.value_or_default();
                    const bool deferredDlss = diagnosticConfig.DlssNrDeferredDlss.value_or_default();
                    const auto firstReject = id.api != 0xb000 ? NaturalQiGateReject::ApiNotD3D11 :
                        !id.nativeDevice ? NaturalQiGateReject::NativeDeviceMissing :
                        natural->thread != callbackThread ? NaturalQiGateReject::CallbackThreadMismatch :
                        !desired ? NaturalQiGateReject::DesiredSettingsMissing :
                        desired != applied ? NaturalQiGateReject::SettingsMismatch :
                        !appliedEnabled ? NaturalQiGateReject::AppliedDisabled :
                        connecting ? NaturalQiGateReject::Connecting :
                        appliedStage != static_cast<unsigned>(Stage::BeforeNr) ? NaturalQiGateReject::StageNotBeforeNr :
                        !genshin ? NaturalQiGateReject::GameNotGenshin :
                        !nrEnabled ? NaturalQiGateReject::NrDisabled :
                        !finishedPicture ? NaturalQiGateReject::NotFinishedPicture :
                        beforeSr ? NaturalQiGateReject::BeforeSrEnabled :
                        deferredDlss ? NaturalQiGateReject::DeferredDlssEnabled : NaturalQiGateReject::Ready;
                    if (!natural->naturalQiGateReports || firstReject != natural->naturalQiGateLastReason) {
                        natural->naturalQiGateLastReason = firstReject;
                        const unsigned observation = ++natural->naturalQiGateReports;
                        LOG_INFO("HoYoShade natural pre-QI gate observation={} observation_limit=8 reason={} event={} runtime=0x{:x} lifetime={} api=0x{:x} native_device_present={} init_tid={} callback_tid={} desired_settings={} applied_settings={} desired_stage={} applied_stage={} desired_enabled={} applied_enabled={} connecting={} selected_snapshot={} genshin={} nr_enabled={} finished_picture={} before_sr={} deferred_dlss={} qi_attempted=false scope=live_natural_callback evidence=scalar_only admission_unchanged=true",
                            observation, NaturalQiGateRejectText(firstReject), AssociationDiagnostics::Text(event), Key(runtime),
                            natural->lifetime, id.api, id.nativeDevice != 0, natural->thread, callbackThread,
                            desired, applied, desiredStage, appliedStage, desiredEnabled, appliedEnabled,
                            connecting, selectedSnapshot, genshin, nrEnabled, finishedPicture, beforeSr, deferredDlss);
                    }
                } catch (...) {} // Preserve the original QI path on diagnostic failure.
            }
            if (!natural->nativeCanonical.attempted && id.api == 0xb000 && id.nativeDevice &&
                natural->thread == GetCurrentThreadId() && Selected(Stage::BeforeNr)) {
                static const auto game = [] {
                    const auto name = Util::ExePath().filename().wstring();
                    return GameRouting::IdentifyExecutable(name.c_str());
                }();
                const auto& config = *Config::Instance();
                if (game == GameRouting::Identity::Genshin && config.DlssNrEnabled.value_or_default() &&
                    config.DlssNrFinishedPicture.value_or_default() && !config.DlssNrRunBeforeSr.value_or_default() &&
                    !config.DlssNrDeferredDlss.value_or_default()) {
                    natural->nativeCanonical.attempted = true;
                    natural->canonicalThread = GetCurrentThreadId();
                    auto* native = reinterpret_cast<ID3D11Device*>(static_cast<uintptr_t>(id.nativeDevice));
                    Microsoft::WRL::ComPtr<IUnknown> canonical;
                    natural->nativeCanonical.hr = native->QueryInterface(IID_PPV_ARGS(&canonical));
                    if (SUCCEEDED(natural->nativeCanonical.hr) && canonical)
                        natural->nativeCanonical.identity = Key(canonical.Get());
                }
            }
#endif
            if (event == RuntimeEvent::Present) {
                if (natural->thread != GetCurrentThreadId()) { natural->frame.Invalidate(); return; }
                natural->frame.Presented();
                if (natural->frame.epoch != UINT64_MAX) {
                    const auto count = runtime->get_back_buffer_count();
                    bool same = count && count <= 8;
                    if(natural->domainBuffersPending && same) {
                        // An actual public init/destroy_swapchain signal revoked
                        // the old set. Refresh only in this live core callback;
                        // cache slots do not accumulate across same-life resize.
                        uint64_t fresh[8]{};
                        for(uint32_t i=0;i<count;++i) {
                            fresh[i]=runtime->get_back_buffer(i).handle;same&=fresh[i]!=0;
                        }
                        if(same) {
                            std::copy(std::begin(fresh),std::end(fresh),std::begin(natural->backbuffers));
                            natural->domainBuffersPending=false;
                        }
                    } else for (uint32_t i = 0; same && i < count; ++i)
                        same = natural->backbuffers[i] == runtime->get_back_buffer(i).handle;
                    if (same) natural->frame.invalid = false;
                }
            }
            if (event == RuntimeEvent::Begin) natural->frame.rendered = true;
            if (event == RuntimeEvent::Reload) { natural->frame.Invalidate(); natural->copiedSerial = 0; natural->domains={}; }
            // Read only during live callbacks; owned API mutations cannot feed
            // back into this natural selection. GUI edits are captured next frame.
            if (!OwnedStage::OwnsNatural(runtime) && !natural->copiedSerial &&
                (event == RuntimeEvent::Init || event == RuntimeEvent::Present || event == RuntimeEvent::Reload))
                {
                const auto snapshots = natural->selection.snapshots;
                const auto selectionRevision = Get().observationSelectionRevision;
                Selection::Capture(runtime, Get().api, natural->selection, event == RuntimeEvent::Reload,
                    natural->lifetime, Selection::CaptureAccess::OwnerCallback);
                CacheCompleteSelection(*natural, selectionRevision);
                capturedNatural = natural;
                if (snapshots != natural->selection.snapshots)
                    LOG_INFO("HoYoShade selection source=natural_api20 snapshot={} private_config={} source_preset={} natural_files=read_only live_values=callback_cache pixels=UNKNOWN",
                        natural->selection.snapshots, natural->selection.config, natural->selection.preset);
            }
        }
    }
    Runtime snapshot; bool changed = false;
    {
        std::lock_guard lock(Get().mutex);
        auto* previous = Get().registry.Find(id.runtime);
        changed = !previous || !(previous->id == id);
        auto* entry = Get().registry.Enroll(id);
        if (!entry) { ++Get().overflow; return; }
        entry->sdkRuntime = Key(runtime);
        entry->sdkDevice = Key(entry->id.sdkDevice ? reinterpret_cast<void *>(entry->id.sdkDevice) : nullptr);
        entry->sdkQueue = Key(entry->id.sdkQueue ? reinterpret_cast<void *>(entry->id.sdkQueue) : nullptr);
        switch (event) {
        case RuntimeEvent::Present: ++entry->presents; break;
        case RuntimeEvent::Begin: ++entry->begins; break;
        case RuntimeEvent::Finish: ++entry->finishes; break;
        case RuntimeEvent::Technique: ++entry->techniques; break;
        case RuntimeEvent::Reload: ++entry->reloads; entry->reloadObserved = true; break;
        default: break;
        }
        snapshot = *entry;
    }
    if (capturedNatural) ReportSelection(*capturedNatural, snapshot.generation);
    if (changed) {
        uint64_t naturalLifetime = 0;
        if (!ownedOperation) for (const auto& natural : Get().natural)
            if (natural.runtime == runtime && natural.live) { naturalLifetime = natural.lifetime; break; }
        LOG_INFO("HoYoShade runtime observed generation={} runtime=0x{:x} api=0x{:x} device=0x{:x} queue_context=0x{:x} late_enrollment={} callback_origin={} natural_lifetime={} generation_domain=registry_not_natural_lifetime core_module=0x{:x} loading=UNKNOWN stage_execution=false",
            snapshot.generation, id.runtime, id.api, id.nativeDevice, id.nativeQueue, event != RuntimeEvent::Init,
            ownedOperation ? "owned-operation" : naturalLifetime ? "tracked-natural" : "unclassified",
            naturalLifetime, Key(Get().coreModule));
    }
}
// Cache bookkeeping must also see callbacks nested in owned create/update and
// Apply. CallbackGuard intentionally drops their natural observation, but its
// recursive execution lock still serializes these SDK-free invalidations.
// Bounded, SDK-free callback evidence for the persistent selection only.
// Requests are BEFORE notifications, never an acknowledgement or author ID.
// No uniform payload, getters, allocations, waits or per-draw reporting here.
void ReportSelectionCallback(reshade::api::effect_runtime* runtime, const char* event,
                             reshade::api::effect_technique technique = {}, bool requested = false) noexcept
{
    try {
        const auto& s = OwnedStage::Get();
        if (!s.natural || !runtime) return;
        const bool natural = Key(runtime) == s.natural;
        const bool owned = runtime == s.owned;
        if (!natural && !owned && !ownedOperation) return;
        static uintptr_t owner = 0;
        static uint64_t lifetime = 0, generation = 0;
        static unsigned events = 0, requests = 0;
        if (owner != s.natural || lifetime != s.lifetime || generation != s.generation) {
            owner = s.natural; lifetime = s.lifetime; generation = s.generation; events = requests = 0;
        }
        auto& count = technique.handle ? requests : events;
        if (count >= 12) return; // no graph walk after the bounded quota
        const Selection::Technique* selected = nullptr;
        if (technique.handle) {
            const auto* graph = natural ? s.capturing.get() : &s.selection;
            if (!graph) return;
            for (const auto& item : graph->techniques)
                if ((natural ? item.natural : item.owned) == technique && item.enabled) { selected = &item; break; }
            if (!selected) return; // only techniques in the previously observed selected set
        }
        ++count;
        LOG_INFO("HoYoShade R548 selection callback event={} runtime=0x{:x} domain={} natural=0x{:x} lifetime={} private_generation={} reload={} phase={} observation={} per_kind_limit=12 technique=0x{:x} effect={} name={} requested={} changing_natural_mode={} writing_private_values={} restoration_debt={} owned_operation={} evidence=notification-not-readback pixels=UNKNOWN",
            event, Key(runtime), natural ? "natural" : owned ? "private" : "owned-operation-unbound",
            s.natural, s.lifetime, s.generation, s.reload, OwnedStage::Text(s.phase), count, technique.handle,
            selected ? selected->effect.c_str() : "none", selected ? selected->name.c_str() : "none", requested,
            s.changingState, s.writingValues, s.owesRestore, ownedOperation);
    } catch (...) {} // observation cannot bypass the original invalidation/callback
}
void SelectionEvent(reshade::api::effect_runtime* runtime, Selection::CacheEvent event) noexcept
{
    ObservationSelectionRequested(); // Existing executionMutex, before lifecycle/preset/order mutation.
    if (event != Selection::CacheEvent::Order) {
        const char* name = event == Selection::CacheEvent::Init ? "init" :
            event == Selection::CacheEvent::Reload ? "reload" : event == Selection::CacheEvent::Preset ? "preset-request" : "destroy";
        ReportSelectionCallback(runtime, name);
    }
    if (event == Selection::CacheEvent::Init || event == Selection::CacheEvent::Reload ||
        event == Selection::CacheEvent::Destroy) {
        // One precise lifecycle invalidation per enrolled chain, never per frame.
        for (auto& c : Get().directChains) {
            const auto key = c.chain.load(std::memory_order_acquire);
            if (!key) continue;
            bool related = false;
            for (const auto& e : Get().directEntries) for (const auto& slot : e.slots)
                related |= slot.occupied && slot.key.chain == key &&
                    (slot.key.runtime == Key(runtime) || slot.key.naturalRuntime == Key(runtime));
            if (related) { if (c.epoch != UINT64_MAX) ++c.epoch; InvalidateDirect(key); }
        }
    }
    if (!ownedOperation && event == Selection::CacheEvent::Destroy) OwnedStage::TopologyChanged();
    OwnedStage::Changed(runtime, event == Selection::CacheEvent::Destroy, event == Selection::CacheEvent::Reload);
    Selection::Observe(runtime, event);
    for (auto& entry : Get().natural) Selection::Invalidate(entry.selection, runtime, event);
}
void OnInit(reshade::api::effect_runtime* runtime) noexcept
{ try { CallbackGuard guard; SelectionEvent(runtime, Selection::CacheEvent::Init); if (guard.entered) ObserveRuntime(runtime, RuntimeEvent::Init); } catch (...) { ++Get().overflow; } }
void OnPresent(reshade::api::effect_runtime* runtime) noexcept
{ try { CallbackGuard guard; BeforeSrDonorCall::Observe(runtime, RuntimeEvent::Present); if (guard.entered) ObserveRuntime(runtime, RuntimeEvent::Present); } catch (...) { ++Get().overflow; } }
void OnReload(reshade::api::effect_runtime* runtime) noexcept
{ try { CallbackGuard guard; SelectionEvent(runtime, Selection::CacheEvent::Reload); if (guard.entered) ObserveRuntime(runtime, RuntimeEvent::Reload); } catch (...) { ++Get().overflow; } }
void OnPreset(reshade::api::effect_runtime* runtime, const char*) noexcept
{
    try { std::lock_guard lifetime(Get().executionMutex); SelectionEvent(runtime, Selection::CacheEvent::Preset); }
    catch (...) { ++Get().overflow; }
}
bool OnReorder(reshade::api::effect_runtime* runtime, size_t, reshade::api::effect_technique*) noexcept
{
    try { std::lock_guard lifetime(Get().executionMutex); SelectionEvent(runtime, Selection::CacheEvent::Order); }
    catch (...) { ++Get().overflow; }
    return false; // BEFORE only: may be vetoed/rewritten later; never certifies completion.
}
void OnDestroy(reshade::api::effect_runtime* runtime) noexcept
{
    RevokeObservationDemand();
    try {
        CallbackGuard guard; SelectionEvent(runtime, Selection::CacheEvent::Destroy); if (!guard.entered) return;
        NaturalCallbackReportScope delivery{runtime, RuntimeEvent::Destroy};
        uint64_t naturalLifetime = 0;
        // Destruction diagnostics reuse cached identity, never query a dying SDK object.
        for (const auto& entry : Get().natural)
            if (entry.runtime == runtime && runtime) { delivery.id = entry.id; naturalLifetime = entry.lifetime; break; }
        for (auto& entry : Get().natural) if (entry.runtime == runtime) {
            entry.live = false; entry.frame.Invalidate();entry.domains={};
            if (Get().ownedNatural == runtime) {
                InvalidateDirect(0, Key(runtime));
                OwnedRuntime::Retire(Get().ownedBinding.owner, Get().ownedBinding.generation);
                Get().ownedBinding = {}; Get().ownedNatural = nullptr;
                Get().scratch.reset(); Get().scratchRuntime = nullptr; Get().scratchGeneration = 0;
            }
            entry.selection = {};
        }
        uint64_t generation = 0;
        { std::lock_guard lock(Get().mutex);
            if (auto* entry = Get().registry.Find(Key(runtime))) generation = entry->generation;
            Get().registry.Destroy(Key(runtime));
            for (auto &transaction : Get().transactions)
                if (transaction.sdkRuntime == runtime) transaction.orphaned = true;
        }
        if (generation) LOG_INFO("HoYoShade runtime destroyed generation={} runtime=0x{:x} callback_origin={} natural_lifetime={} generation_domain=registry_not_natural_lifetime core_module=0x{:x} stage_execution=false",
            generation, Key(runtime), ownedOperation ? "owned-operation" : naturalLifetime ? "tracked-natural" : "unclassified",
            naturalLifetime, Key(Get().coreModule));
    } catch (...) {}
}
void Effects(reshade::api::effect_runtime* runtime, reshade::api::command_list* list, RuntimeEvent event,
             reshade::api::resource_view linear, reshade::api::resource_view srgb,
             reshade::api::effect_technique technique = {}) noexcept
{
    try {
        CallbackGuard guard;
        const auto rendered = event == RuntimeEvent::Begin ? Selection::RenderScope::Event::Begin :
            event == RuntimeEvent::Finish ? Selection::RenderScope::Event::Finish : Selection::RenderScope::Event::Technique;
        Selection::RenderScope::Observe(rendered, runtime, list, linear, srgb, technique);
        BeforeSrDonorCall::Observe(runtime, event);
        if (!guard.entered) return;
        EnrollList(list); ObserveRuntime(runtime, event);
    } catch (...) { ++Get().overflow; }
}
void OnBegin(reshade::api::effect_runtime* runtime, reshade::api::command_list* list, reshade::api::resource_view linear, reshade::api::resource_view srgb) noexcept
{
    try { std::lock_guard lock(Get().executionMutex); RevokeEnabledObservationDemand(); OwnedStage::Begin(runtime); }
    catch (...) { return; }
    Effects(runtime, list, RuntimeEvent::Begin, linear, srgb);
}
void OnFinish(reshade::api::effect_runtime* runtime, reshade::api::command_list* list, reshade::api::resource_view linear, reshade::api::resource_view srgb) noexcept
{ Effects(runtime, list, RuntimeEvent::Finish, linear, srgb); }
void OnTechnique(reshade::api::effect_runtime* runtime, reshade::api::effect_technique technique, reshade::api::command_list* list, reshade::api::resource_view linear, reshade::api::resource_view srgb) noexcept
{
    try { std::lock_guard lock(Get().executionMutex); RevokeEnabledObservationDemand(); OwnedStage::Technique(runtime, list); }
    catch (...) { return; }
    Effects(runtime, list, RuntimeEvent::Technique, linear, srgb, technique);
}
bool OnOwnedStageEffects(reshade::api::effect_runtime* runtime, bool enabled) noexcept
{
    try {
        std::lock_guard lock(Get().executionMutex);
        ObservationSelectionRequested(); // BEFORE request, not acknowledgement of the new state.
        ReportSelectionCallback(runtime, enabled ? "effects-enable-request" : "effects-disable-request");
        return OwnedStage::EffectsRequested(runtime, enabled);
    }
    catch (...) { return false; }
}
bool OnOwnedStageTechnique(reshade::api::effect_runtime* runtime, reshade::api::effect_technique technique, bool enabled) noexcept
{
    try {
        std::lock_guard lock(Get().executionMutex);
        ObservationSelectionRequested(); // Old zero-selection Capture cannot be claimed again.
        ReportSelectionCallback(runtime, "selected-technique-request", technique, enabled);
        OwnedStage::SelectionRequested(runtime);
    }
    catch (...) {}
    return false;
}
bool OnOwnedStageUniform(reshade::api::effect_runtime* runtime, reshade::api::effect_uniform_variable variable, const void*, size_t) noexcept
{
    try { std::lock_guard lock(Get().executionMutex); RevokeEnabledObservationDemand(); OwnedStage::UniformRequested(runtime, variable); }
    catch (...) {}
    return false;
}

bool KnownDiskIdentity(const wchar_t* path,HANDLE supplied) noexcept
{
    // This identifies the inspected file on disk, not a hash of the loaded image.
    constexpr unsigned char expected[] = {0x0c,0xee,0x63,0xf9,0xc9,0xf1,0x3f,0x3a,0xc9,0x09,0xc5,0xb4,0x90,0x3f,0x4d,0xbb,0x4b,0x71,0x9a,0x7a,0xb3,0xb4,0xf1,0x3b,0x0d,0xea,0xf8,0x3c,0x81,0x4b,0x94,0xf7};
    HANDLE file = supplied!=INVALID_HANDLE_VALUE?supplied:CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{}; bool ok = GetFileSizeEx(file, &size) && size.QuadPart == 5592064;
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    if (ok) ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    unsigned char buffer[65536], digest[32]{}; DWORD read = 0; uint64_t total = 0;
    while (ok) {
        if (!ReadFile(file, buffer, sizeof(buffer), &read, nullptr)) { ok = false; break; }
        if (!read) break;
        total += read; ok = BCryptHashData(hash, buffer, read, 0) >= 0;
    }
    if (ok) ok = total == 5592064 && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0 && !memcmp(digest, expected, sizeof(digest));
    if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if(supplied==INVALID_HANDLE_VALUE)CloseHandle(file); return ok;
}
Reason Connect(HMODULE self)
{
    RevokeObservationDemand();
    HMODULE modules[1024]{}; DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes) || bytes > sizeof(modules)) return Reason::MultipleCores;
    // Enumeration is only a snapshot. Acquire a real module reference before
    // touching each export table, so a concurrent unload cannot invalidate it.
    HMODULE held = nullptr; unsigned count = 0;
    struct Release { HMODULE& module; ~Release() { if (module) FreeLibrary(module); } } release{held};
    for (unsigned i = 0; i < bytes / sizeof(HMODULE); ++i) {
        HMODULE candidate = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(modules[i]), &candidate)) continue;
        if (GetProcAddress(candidate, "ReShadeRegisterAddon")) {
            ++count;
            if (!held) { held = candidate; continue; }
        }
        FreeLibrary(candidate);
    }
    if (!count) return Reason::CoreMissing;
    if (count != 1) return Reason::MultipleCores;
    // Hold the ordinary reference while checking identity; only an accepted core is pinned.
    wchar_t path[32768]{};
    const auto length = GetModuleFileNameW(held, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path)) return Reason::IdentityRejected;
    const std::filesystem::path diskPath(path);
    if (_wcsicmp(diskPath.filename().c_str(), L"ReShade64.dll") ||
        _wcsicmp(diskPath.parent_path().filename().c_str(), L"HoYoShade") || !KnownDiskIdentity(path)) return Reason::IdentityRejected;
    CoreApi api;
    api.registerAddon = reinterpret_cast<CoreApi::RegisterAddon>(GetProcAddress(held, "ReShadeRegisterAddon"));
    api.unregisterAddon = reinterpret_cast<CoreApi::UnregisterAddon>(GetProcAddress(held, "ReShadeUnregisterAddon"));
    api.registerEvent = reinterpret_cast<CoreApi::RegisterEvent>(GetProcAddress(held, "ReShadeRegisterEvent"));
    api.unregisterEvent = reinterpret_cast<CoreApi::UnregisterEvent>(GetProcAddress(held, "ReShadeUnregisterEvent"));
    api.createEffectRuntime = reinterpret_cast<CoreApi::CreateEffectRuntime>(GetProcAddress(held, "ReShadeCreateEffectRuntime"));
    api.destroyEffectRuntime = reinterpret_cast<CoreApi::DestroyEffectRuntime>(GetProcAddress(held, "ReShadeDestroyEffectRuntime"));
    api.getConfigValue = reinterpret_cast<CoreApi::GetConfigValue>(GetProcAddress(held, "ReShadeGetConfigValue"));
    api.getBasePath = reinterpret_cast<CoreApi::GetBasePath>(GetProcAddress(held, "ReShadeGetBasePath"));
    if (!api.registerAddon || !api.unregisterAddon || !api.registerEvent || !api.unregisterEvent ||
        !api.createEffectRuntime || !api.destroyEffectRuntime || !api.getConfigValue || !api.getBasePath) return Reason::ExportsMissing;
    if (!api.Handshake(self)) return Reason::RegistrationRejected;
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(held), &pinned)) {
        api.unregisterAddon(self); return Reason::PinFailed;
    }
    Get().api = api;
    Get().coreModule = held;
    Get().accepted.store(true, std::memory_order_release);
    using E = reshade::addon_event;
    api.Subscribe<E::init_effect_runtime>(OnInit); api.Subscribe<E::destroy_effect_runtime>(OnDestroy);
    api.Subscribe<E::init_command_list>(OnList); api.Subscribe<E::destroy_command_list>(OnDestroyList);
    api.Subscribe<E::destroy_command_queue>(OnDestroyQueue); api.Subscribe<E::destroy_device>(OnDestroyDevice);
    api.Subscribe<E::init_swapchain>(OnSwapchainInit); api.Subscribe<E::destroy_swapchain>(OnSwapchainDestroy);
    api.Subscribe<E::reshade_present>(OnPresent); api.Subscribe<E::reshade_begin_effects>(OnBegin);
    api.Subscribe<E::reshade_finish_effects>(OnFinish); api.Subscribe<E::reshade_render_technique>(OnTechnique);
    api.Subscribe<E::reshade_reloaded_effects>(OnReload);
    api.Subscribe<E::reshade_set_effects_state>(OnOwnedStageEffects);
    api.Subscribe<E::reshade_set_technique_state>(OnOwnedStageTechnique);
    api.Subscribe<E::reshade_set_uniform_value>(OnOwnedStageUniform);
    api.Subscribe<E::reshade_set_current_preset_path>(OnPreset);
    api.Subscribe<E::reshade_reorder_techniques>(OnReorder);
    api.Subscribe<E::init_resource_view>(OnDomainView);
    Get().domainSubscriptionRequested.store(true,std::memory_order_release);
    LOG_INFO("HoYoShade API20 registration accepted core_path={} disk_sha256=0cee63f9c9f13f3ac909c5b4903f4dbb4b719a7ab3b4f13b0deaf83c814b94f7 loaded_image_sha=UNKNOWN subscriptions_requested=19 callback_delivery=UNCONFIRMED stage_execution=false", diskPath.string());
    return Reason::ExecutionUnverified;
}
}

namespace {
void StartConnectionIfNeeded() noexcept
{
#ifndef HOYOSHADE_STAGE_TEST
    auto& state = Get();
    if (!state.started.load(std::memory_order_acquire) ||
        !SettingsEnabled(state.desiredSettings.load(std::memory_order_acquire)) ||
        state.accepted.load(std::memory_order_acquire) || state.connectStarted.exchange(true)) return;
    RevokeObservationDemand();
    state.connecting.store(true, std::memory_order_release);
    state.core.store(Reason::CorePending, std::memory_order_release);
    try {
        std::thread([] {
            auto& state = Get();
            try {
                const auto self = state.self.load(std::memory_order_acquire);
                HMODULE pinned = nullptr;
                Reason result = Reason::PinFailed;
                if (self && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(self), &pinned)) {
                    result = Reason::CoreMissing;
                    for (unsigned attempt = 0; attempt < 30; ++attempt) {
                        result = Connect(self);
                        if (result != Reason::CoreMissing) break;
                        if (attempt != 29) Sleep(1000);
                    }
                }
                state.core.store(result, std::memory_order_release);
                LOG_INFO("HoYoShade live connection finished result={} accepted={} core_load=existing-only requested_generation={} stage_execution=false",
                    Text(result), state.accepted.load(), SettingsGeneration(state.desiredSettings.load()));
            } catch (...) { state.core.store(Reason::InternalFailure, std::memory_order_release); }
            state.connecting.store(false, std::memory_order_release);
        }).detach();
    } catch (...) {
        state.core.store(Reason::InternalFailure, std::memory_order_release);
        state.connecting.store(false, std::memory_order_release);
    }
#endif
}
}
bool Reconfigure(bool enabled, unsigned stage) noexcept
{
    try {
        if (stage < 1 || stage > 3 || !Get().started.load(std::memory_order_acquire)) return false;
        const uint64_t bits = stage | (enabled ? EnabledBit : 0);
        auto previous = Get().desiredSettings.load(std::memory_order_acquire);
        for (;;) {
            if (previous && (previous & SettingsMask) == bits) { StartConnectionIfNeeded(); return true; }
            if (SettingsGeneration(previous) == (UINT64_MAX >> SettingsShift)) return false;
            const auto desired = ((SettingsGeneration(previous) + 1) << SettingsShift) | bits;
            RevokeObservationDemand();
            if (Get().desiredSettings.compare_exchange_weak(previous, desired,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
                StartConnectionIfNeeded();
                return true;
            }
        }
    } catch (...) { return false; }
}
RenderOwnerScope::RenderOwnerScope() noexcept
{
    try {
        auto& state = Get();
        if (!state.started.load(std::memory_order_acquire)) return;
        const auto desired = state.desiredSettings.load(std::memory_order_acquire);
        if (!SettingsEnabled(desired) && desired == state.appliedSettings.load(std::memory_order_acquire)) return;
        std::lock_guard lifetime(state.executionMutex);
        if (state.renderOwners == (std::numeric_limits<size_t>::max)()) return;
        if (!state.renderOwners && !inCallback && !ownedOperation && !stageActive)
            AdvanceSettingsAtOwnerBoundary();
        OwnedStage::SettingsChanged(OwnedStageSettings());
        RevokeObservationDemand();
        ++state.renderOwners;
        entered = true;
    } catch (...) {}
}
RenderOwnerScope::~RenderOwnerScope()
{
    if (!entered) return;
    try { std::lock_guard lifetime(Get().executionMutex); --Get().renderOwners; } catch (...) {}
}
bool ObservationDemand::Current() const noexcept
{
    if (!publication || publication == UINT64_MAX) return false;
#ifndef HOYOSHADE_STAGE_TEST
    const auto& option = Config::Instance()->HoYoShadeEnabled;
    if (option.value_or_default() != enabledIdle) return false;
#endif
    auto& state = Get();
    if (state.observationPublication.load(std::memory_order_acquire) != publication ||
        state.connecting.load(std::memory_order_acquire) ||
        state.enabled.load(std::memory_order_acquire) != enabledIdle) return false;
    if (settings) {
        if (!state.started.load(std::memory_order_acquire) || SettingsEnabled(settings) != enabledIdle ||
            (enabledIdle && !state.accepted.load(std::memory_order_acquire)) ||
            state.stage.load(std::memory_order_acquire) != SettingsStage(settings) ||
            state.desiredSettings.load(std::memory_order_acquire) != settings ||
            state.appliedSettings.load(std::memory_order_acquire) != settings) return false;
    } else {
        // This is a claimed empty lifetime, never an inference from !started.
        // started/connectStarted are one-way; a connection cannot disappear
        // behind a zero settings token or an off/on/off configuration cycle.
        if (enabledIdle || state.started.load(std::memory_order_acquire) ||
            state.connectStarted.load(std::memory_order_acquire) ||
            state.accepted.load(std::memory_order_acquire) ||
            state.desiredSettings.load(std::memory_order_acquire) ||
            state.appliedSettings.load(std::memory_order_acquire)) return false;
    }
    return ColorObservationIdle() &&
        state.observationPublication.load(std::memory_order_acquire) == publication;
}
ObservationDemand ReadObservationDemand() noexcept
{
    try {
        auto& state = Get();
        const auto settings = state.appliedSettings.load(std::memory_order_acquire);
        ObservationDemand result{settings, state.observationPublication.load(std::memory_order_acquire),
            SettingsEnabled(settings)};
        if (!result.Current() || inCallback || ownedOperation || stageActive) return {};
        std::unique_lock lifetime(state.executionMutex, std::try_to_lock);
        if (!lifetime.owns_lock()) return {};
        std::unique_lock lock(state.mutex, std::try_to_lock);
        if (!lock.owns_lock() || state.renderOwners ||
            state.overflow.load(std::memory_order_acquire)) return {};
        if (result.enabledIdle) {
            // Existing Present/claim/Poll boundary only. No API calls, per-draw
            // enumeration or cached enabled/readiness inference. Every live
            // natural lifetime needs a complete zero-selection Capture newer
            // than the last selection request/lifecycle mutation.
            const auto& events = Selection::CacheEvents();
            if (state.naturalStatusIncomplete || events.exhausted || events.mutationExhausted ||
                state.observationSelectionRevision == UINT64_MAX) return {};
            bool live = false;
            for (const auto& natural : state.natural) {
                if (!natural.live) continue;
                live = true;
                if (!natural.runtime || !natural.lifetime || !natural.captureObserved ||
                    !natural.frame.CanArm() || natural.copiedSerial ||
                    !natural.idleSelectionMutation || natural.idleSelectionMutation != events.mutation ||
                    natural.idleSelectionRevision != state.observationSelectionRevision) return {};
            }
            if (!live) return {}; // No runtime/selection knowledge is not an empty selection.
            // Retire may have withdrawn the public binding while the original
            // pool still owns a submitted batch, a lease or quarantine. Never
            // promote zero selection into GPU completion or force collection.
            if (!OwnedRuntime::ObservationOwnersIdle()) return {};
        }
        // Off/zero selection is only demand, never GPU retirement. Historical
        // CPU cache roots alone do not subscribe; actual custody still refuses.
        if (state.scratch || state.ownedBindingOwner) return {};
        // nextDirectLifetime is an enrollment serial, not a live consumer.
        // Check actual recording/retirement custody at this locked claim boundary;
        // scalar chain tombstones and cache keys alone do not subscribe draws.
        for (const auto& chain : state.directChains) {
            const auto access = chain.access.load(std::memory_order_acquire);
            if ((access & (DirectRecording | DirectOwners)) || access >= DirectResizeStep ||
                chain.resourceOnlyResize.load(std::memory_order_acquire)) return {};
        }
        for (const auto& entry : state.directEntries) for (const auto& slot : entry.slots)
            if (slot.owner || DirectPending(slot)) return {};
        for (const auto& t : state.transactions)
            if (t.token || t.lease || t.rtvOwner || t.completion.fence || t.inFlight ||
                t.reserved || t.recorded || t.signalPending || t.submitted ||
                t.signalFailed || t.orphaned) return {};
        // The persistent owned path has separate retained roots. Natural
        // selection/cache metadata alone does not subscribe an owned consumer.
        const auto& owned = OwnedStage::Get();
        if (owned.active || owned.owesRestore || owned.uncertain || owned.rendering ||
            owned.binding.owner || owned.device || owned.queue || owned.factory || owned.targets ||
            owned.latest.fence || owned.renderingDepth || owned.renderingList) return {};
        return result.Current() ? result : ObservationDemand{};
    } catch (...) { return {}; }
}
LiveSettingsStatus ReadLiveSettings() noexcept
{
    LiveSettingsStatus result;
    try {
        auto& state = Get();
        const auto desired = state.desiredSettings.load(std::memory_order_acquire);
        const auto applied = state.appliedSettings.load(std::memory_order_acquire);
        result.initialized = state.started.load(std::memory_order_acquire);
        result.connecting = state.connecting.load(std::memory_order_acquire);
        result.accepted = state.accepted.load(std::memory_order_acquire);
        result.connectionReason = state.core.load(std::memory_order_acquire);
        result.desiredEnabled = SettingsEnabled(desired); result.desiredStage = SettingsStage(desired);
        result.desiredGeneration = SettingsGeneration(desired);
        result.appliedKnown = applied != 0; result.appliedEnabled = SettingsEnabled(applied);
        result.appliedStage = SettingsStage(applied); result.appliedGeneration = SettingsGeneration(applied);
        result.pending = desired != applied;
        std::unique_lock lifetime(state.executionMutex, std::try_to_lock);
        if (!lifetime.owns_lock() || inCallback || ownedOperation || stageActive) return result;
        const auto& wait = state.latestEpochWait;
        if (!result.pending && result.appliedEnabled && wait.settings == applied) {
            for (const auto& natural : state.natural) {
                if (natural.live && Key(natural.runtime) == wait.runtime && natural.lifetime == wait.lifetime &&
                    !SettingsEpochReady(natural, applied)) {
                    result.waitingForEpoch = true;
                    result.waitingStream = wait.stream; result.waitingGeneration = wait.generation;
                    break;
                }
            }
        }
        for (const auto& transaction : state.transactions)
            if (transaction.reserved || transaction.inFlight || transaction.recorded || transaction.orphaned)
                ++result.retainedTransactions;
        result.readable = true;
    } catch (...) {}
    return result;
}
#ifndef HOYOSHADE_STAGE_TEST
void Initialize(HMODULE self, bool enabled, unsigned stage) noexcept
{
    try {
        RevokeObservationDemand();
        if (Get().started.exchange(true)) return;
        Get().self.store(self, std::memory_order_release);
        if (!Reconfigure(enabled, stage)) Get().core.store(Reason::InvalidStage);
    } catch (...) { Get().core.store(Reason::InternalFailure); }
}
#else
void Testing::EnableHost(bool enabled) noexcept {
    RevokeObservationDemand();
    Get().enabled.store(enabled, std::memory_order_release);
    const auto settings = (Get().desiredSettings.load() & ~EnabledBit) | (enabled ? EnabledBit : 0);
    Get().desiredSettings.store(settings); Get().appliedSettings.store(settings);
}
bool Testing::ConnectHost(HMODULE self, const char* config, Stage stage) noexcept
{
    RevokeObservationDemand();
    if (!self || !config || Get().started.exchange(true)) return false;
    try {
        Get().ownedConfigPath = config; Get().stage.store(static_cast<unsigned>(stage));
        const uint64_t settings = (uint64_t{1} << SettingsShift) | EnabledBit | static_cast<unsigned>(stage);
        Get().desiredSettings.store(settings); Get().appliedSettings.store(settings);
        Get().enabled.store(true); Get().core.store(Connect(self));
        return Get().accepted.load();
    } catch (...) { return false; }
}
#endif
StatusSnapshot ReadStatus() noexcept
{
    StatusSnapshot result;
    try {
        auto& state = Get();
        // Never take State::mutex, and never wait for executionMutex. Lifecycle
        // writers already serialize all natural/cache/latest data with this lock.
        std::unique_lock lifetime(state.executionMutex, std::try_to_lock);
        if (!lifetime.owns_lock()) { result.read = StatusRead::Busy; return result; }
        // Recursive acquisition on a callback thread would expose half-written
        // Capture data. Treat reentrant reads exactly like another busy writer.
        if (inCallback || ownedOperation) { result.read = StatusRead::Busy; return result; }
        result.initialized = state.started.load(std::memory_order_acquire);
        result.accepted = state.accepted.load(std::memory_order_acquire);
        result.enabled = state.enabled.load(std::memory_order_acquire);
        result.stage = state.stage.load(std::memory_order_relaxed);
        result.coreReason = state.core.load(std::memory_order_acquire);
        result.latest = state.latestStage;
        const State::Natural* source = nullptr;
        for (const auto& entry : state.natural) if (entry.runtime && entry.live) {
            ++result.natural.liveRuntimeCount;
            source = &entry;
        }
        auto& natural = result.natural;
        if (state.naturalStatusIncomplete) natural.status = NaturalStatus::CapacityExceeded;
        else if (natural.liveRuntimeCount > 1) natural.status = NaturalStatus::Ambiguous;
        else if (source) {
            natural.status = NaturalStatus::UniqueLive;
            natural.runtime = Key(source->runtime); natural.lifetime = source->lifetime;
            natural.api = source->id.api;
            natural.captureObserved = source->captureObserved;
            if (source->captureObserved) {
                const auto& value = source->selectionStatus;
                natural.captureReady = value.captureReady;
                natural.selectionReady = value.selectionReady;
                natural.presetKnown = value.presetKnown;
                if (value.presetKnown) natural.presetPath = source->presetPath;
                natural.globalEnabledKnown = value.globalKnown;
                if (value.globalKnown) natural.globalEnabled = value.globalEnabled;
                natural.countsKnown = value.countsKnown;
                if (value.countsKnown) {
                    natural.totalTechniques = value.total;
                    natural.enabledTechniques = value.enabled;
                }
            }
        }
        result.read = StatusRead::Ready;
        return result;
    } catch (...) { return {}; } // All fields UNKNOWN; never unwind into UI.
}
bool Selected(Stage stage) noexcept
{
    const auto desired = Get().desiredSettings.load(std::memory_order_acquire);
    const auto applied = Get().appliedSettings.load(std::memory_order_acquire);
    return desired && desired == applied && SettingsEnabled(applied) &&
        !Get().connecting.load(std::memory_order_acquire) &&
        SettingsStage(applied) == static_cast<unsigned>(stage);
}

bool NativeBeforeSrAvailable(const NaturalAssociation::Source* source) noexcept
{
    if (!source || !Selected(Stage::BeforeSr) || inCallback || ownedOperation || stageActive) return false;
    try {
        std::lock_guard lifetime(Get().executionMutex);
        if (!Selected(Stage::BeforeSr) || !Get().accepted.load(std::memory_order_acquire) ||
            Get().naturalStatusIncomplete || OwnedStage::RestorationPending()) return false;
        const auto& value = source->Observation();
        if (value.api != 0xc000 || source->Endpoint(value.stream, value.generation, value.frame,
            value.sourceDevice, value.buffer) != NaturalAssociation::Predicate::Captured ||
            source->BoundRuntime().predicate != NaturalAssociation::Predicate::Captured) return false;
        for (const auto& natural : Get().natural) {
            if (!natural.live || !natural.runtime || natural.thread != GetCurrentThreadId() ||
                source->Bound(Key(natural.runtime), natural.lifetime, natural.frame.epoch) !=
                    NaturalAssociation::Predicate::Captured) continue;
            return natural.frame.CanArm() && !natural.frame.invalid && !natural.copiedSerial &&
                natural.selection.valid && !natural.selection.config.empty() &&
                Selection::HasEnabledTechniques(natural.selection);
        }
    } catch (...) {}
    return false;
}

namespace {
struct ColorIsolation {
    std::mutex mutex;
    std::atomic<bool> present{false};
    ConsumerQuarantine rules;
    bool tainted = false;
    size_t activeWriters = 0;
    struct Owner {
        Microsoft::WRL::ComPtr<IUnknown> chain;
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        Microsoft::WRL::ComPtr<ID3D12Resource> color;
    } owners[ConsumerQuarantine::Capacity];
    struct Writer { uint64_t token = 0; Owner owner; } writers[ConsumerQuarantine::Capacity];
    uint64_t nextWriter = 0;
    struct Content {
        uint64_t token = 0, stream = 0, tail = 0;
        ConsumerEndpoint key;
        Owner owner;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        bool reserved = false;
    } contents[ConsumerQuarantine::Capacity];
    uint64_t nextContent = 0;
    size_t activeContents = 0;
};
ColorIsolation& Isolation() { static auto* state = new ColorIsolation; return *state; }
bool ColorObservationIdle() noexcept
{
    return !Isolation().present.load(std::memory_order_acquire);
}
// Existing isolation mutex held. Publish demand BEFORE retaining a new writer,
// content hold or quarantine owner. The epoch prevents ABA after a clean release;
// permanent quarantine still owns its objects and keeps present set.
void PublishColorObservation(ColorIsolation& state) noexcept
{
    state.present.store(true, std::memory_order_release);
    RevokeObservationDemand();
}
ConsumerEndpoint Endpoint(const ColorIsolation::Owner& o, uint64_t generation) noexcept {
    return {Key(o.chain.Get()), Key(o.device.Get()), Key(o.queue.Get()), Key(o.color.Get()), generation};
}
void QuarantineLocked(ColorIsolation& state, const ColorIsolation::Owner& owner, uint64_t generation) noexcept {
    PublishColorObservation(state);
    const auto key = Endpoint(owner, generation);
    const auto slot = state.rules.Add(key);
    if (slot < ConsumerQuarantine::Capacity && !state.owners[slot].color) {
        state.owners[slot] = owner;
        LOG_WARN("HoYoShade color quarantined chain=0x{:x} device=0x{:x} queue=0x{:x} resource=0x{:x} generation={} consumers=denied recovery=new_physical_endpoint",
            key.chain, key.device, key.queue, key.color, key.generation);
    }
    if (slot == ConsumerQuarantine::Capacity) {
        // Saturation is not permission to release unknown GPU owners.
        if (owner.chain) owner.chain->AddRef();
        if (owner.device) owner.device->AddRef();
        if (owner.queue) owner.queue->AddRef();
        if (owner.color) owner.color->AddRef();
    }
    state.tainted = true;
    state.present.store(true, std::memory_order_release);
}
ConsumerAccess CheckLocked(ColorIsolation& state, const ConsumerEndpoint& key, uint64_t discardToken = 0) noexcept {
    const auto taint = state.rules.Check(key);
    if (taint != ConsumerAccess::Safe) return taint;
    for (const auto& content : state.contents) {
        if (!content.token || content.token == discardToken) continue;
        if ((key.chain && key.chain == content.key.chain) ||
            (key.color && key.color == content.key.color && key.device == content.key.device))
            return ConsumerAccess::Unavailable;
    }
    for (const auto& writer : state.writers) {
        if (!writer.token) continue;
        const auto active = Endpoint(writer.owner, writer.token);
        if ((key.chain && active.chain == key.chain) ||
            (key.color && key.color == active.color && key.device == active.device))
            return ConsumerAccess::Unavailable;
    }
    return ConsumerAccess::Safe;
}

bool ContentReady(const ColorIsolation::Content& entry) noexcept {
    if (!entry.token || !entry.fence || !entry.tail || entry.tail == UINT64_MAX ||
        !entry.owner.device || FAILED(entry.owner.device->GetDeviceRemovedReason())) return false;
    const auto completed = entry.fence->GetCompletedValue();
    return completed != UINT64_MAX && completed >= entry.tail;
}
bool ContentMatches(const ColorIsolation::Content& entry, uint64_t token, IUnknown* chain,
                    uint64_t stream, uint64_t generation) noexcept {
    return token && entry.token == token && entry.stream == stream && entry.key.generation == generation &&
        entry.key.chain == Key(chain);
}
}
bool PhysicalContentHold::Publish(IDXGISwapChain* chain, ID3D12CommandQueue* queue,
        ID3D12Device* device, ID3D12Resource* color, uint64_t stream, uint64_t generation,
        ID3D12Fence* fence, uint64_t tail) noexcept
{
    if (token || !chain || !queue || !device || !color || !stream || !generation || !fence || !tail || tail == UINT64_MAX) return false;
    ColorIsolation::Owner owner;
    Microsoft::WRL::ComPtr<ID3D12Device> queueDevice, colorDevice, fenceDevice;
    if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&owner.chain))) ||
        FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) || FAILED(color->GetDevice(IID_PPV_ARGS(&colorDevice))) ||
        FAILED(fence->GetDevice(IID_PPV_ARGS(&fenceDevice))) || queueDevice.Get() != device ||
        colorDevice.Get() != device || fenceDevice.Get() != device ||
        FAILED(device->GetDeviceRemovedReason()) || fence->GetCompletedValue() == UINT64_MAX) return false;
    owner.device = device; owner.queue = queue; owner.color = color;
    auto& state = Isolation(); std::lock_guard lock(state.mutex);
    const auto key = Endpoint(owner, generation);
    if (CheckLocked(state, key) != ConsumerAccess::Safe || state.nextContent == UINT64_MAX) return false;
    for (auto& entry : state.contents) if (!entry.token) {
        PublishColorObservation(state);
        token = entry.token = ++state.nextContent; entry.stream = stream; entry.key = key;
        entry.owner = std::move(owner); entry.fence = fence; entry.tail = tail;
        ++state.activeContents; state.present.store(true, std::memory_order_release);
        return true;
    }
    return false;
}
bool PhysicalContentHold::ReserveDiscard(IDXGISwapChain* chain, uint64_t stream, uint64_t generation) noexcept
{
    Microsoft::WRL::ComPtr<IUnknown> identity;
    if (!token || !chain || FAILED(chain->QueryInterface(IID_PPV_ARGS(&identity)))) return false;
    auto& state = Isolation(); std::lock_guard lock(state.mutex);
    for (auto& entry : state.contents) if (ContentMatches(entry, token, identity.Get(), stream, generation)) {
        if (entry.reserved || !ContentReady(entry) || CheckLocked(state, entry.key, token) != ConsumerAccess::Safe) return false;
        // The exact tail is complete. Release only our old backbuffer reference
        // so DXGI can discard it; the chain/content denial remains published.
        entry.reserved = true; entry.owner.color.Reset(); return true;
    }
    return false;
}
bool PhysicalContentHold::ResizeAccess(IDXGISwapChain* chain, uint64_t stream, uint64_t generation) const noexcept
{
    Microsoft::WRL::ComPtr<IUnknown> identity;
    if (!token || !chain || FAILED(chain->QueryInterface(IID_PPV_ARGS(&identity)))) return false;
    auto& state = Isolation(); std::lock_guard lock(state.mutex);
    for (const auto& entry : state.contents) if (ContentMatches(entry, token, identity.Get(), stream, generation))
        return entry.reserved && ContentReady(entry) && CheckLocked(state, entry.key, token) == ConsumerAccess::Safe;
    return false;
}
bool PhysicalContentHold::CommitDiscard(IDXGISwapChain* chain, uint64_t stream, uint64_t generation,
        uint64_t nextGeneration, HRESULT hr) noexcept
{
    Microsoft::WRL::ComPtr<IUnknown> identity;
    if (!token || FAILED(hr) || generation == UINT64_MAX || nextGeneration != generation + 1 ||
        !chain || FAILED(chain->QueryInterface(IID_PPV_ARGS(&identity)))) return false;
    auto& state = Isolation(); std::lock_guard lock(state.mutex);
    for (auto& entry : state.contents) if (ContentMatches(entry, token, identity.Get(), stream, generation)) {
        if (!entry.reserved || !ContentReady(entry) || CheckLocked(state, entry.key, token) != ConsumerAccess::Safe) return false;
        // Completion was checked independently of the CPU resize HRESULT.
        // Only this content entry is retired; permanent rules have no clear API.
        entry = {}; token = 0; --state.activeContents;
        state.present.store(state.tainted || state.activeWriters || state.activeContents, std::memory_order_release);
        return true;
    }
    return false;
}
void PhysicalContentHold::CancelDiscard() noexcept
{
    if (!token) return;
    auto& state = Isolation(); std::lock_guard lock(state.mutex);
    for (auto& entry : state.contents) if (entry.token == token) { entry.reserved = false; break; }
}
void QuarantineColor(IDXGISwapChain* chain, ID3D12CommandQueue* queue,
                     ID3D12Device* device, ID3D12Resource* color, uint64_t generation) noexcept
{
    ColorIsolation::Owner owner;
    if (chain) chain->QueryInterface(IID_PPV_ARGS(&owner.chain));
    owner.device = device; owner.queue = queue; owner.color = color;
    auto& state = Isolation();
    std::lock_guard lock(state.mutex);
    QuarantineLocked(state, owner, generation);
}
ColorWriteScope::ColorWriteScope(bool enabled, ID3D12Device* device, ID3D12CommandQueue* queue,
                                 ID3D12Resource* color, IDXGISwapChain* chain) noexcept
{
    if (!enabled || !device || !queue || !color) return;
    ColorIsolation::Owner owner;
    if (chain && FAILED(chain->QueryInterface(IID_PPV_ARGS(&owner.chain)))) return;
    owner.device = device; owner.queue = queue; owner.color = color;
    auto& state = Isolation();
    std::lock_guard lock(state.mutex);
    if (CheckLocked(state, Endpoint(owner, 0)) != ConsumerAccess::Safe) return;
    for (auto& writer : state.writers) {
        if (writer.token) continue;
        if (state.nextWriter == UINT64_MAX) return;
        PublishColorObservation(state);
        token = writer.token = ++state.nextWriter;
        writer.owner = std::move(owner);
        ++state.activeWriters;
        state.present.store(true, std::memory_order_release);
        return;
    }
    // Ordinary writer contention/capacity is an untouched admission refusal,
    // not a global quarantine and never permission to suppress natural.
}
ColorWriteScope::~ColorWriteScope() { FinishAfterReturn(true); }
void ColorWriteScope::FinishAfterReturn(bool uncertain) noexcept
{
    if (!token) return;
    auto& state = Isolation();
    std::lock_guard lock(state.mutex);
    for (auto& writer : state.writers) {
        if (writer.token != token) continue;
        // Publish the persistent denial BEFORE withdrawing the active writer.
        // No resize/CPU submit/present notification can clear this quarantine.
        if (uncertain && state.rules.Check(Endpoint(writer.owner, token)) == ConsumerAccess::Safe)
            QuarantineLocked(state, writer.owner, token);
        writer = {};
        --state.activeWriters;
        state.present.store(state.tainted || state.activeWriters != 0 || state.activeContents != 0, std::memory_order_release);
        break;
    }
    token = 0;
}
ConsumerAccess ColorConsumers(IDXGISwapChain* chain) noexcept
{
    auto& state = Isolation();
    if (!state.present.load(std::memory_order_acquire)) return ConsumerAccess::Safe;
    Microsoft::WRL::ComPtr<IUnknown> identity;
    if (!chain || FAILED(chain->QueryInterface(IID_PPV_ARGS(&identity)))) return ConsumerAccess::Unavailable;
    std::lock_guard lock(state.mutex);
    ConsumerEndpoint key; key.chain = Key(identity.Get());
    const auto chainAccess = CheckLocked(state, key);
    if (chainAccess != ConsumerAccess::Safe) return chainAccess;
    // A different wrapper/queue cannot authorize an alias of a tainted or
    // concurrently written color. Unrelated API11/12 chains remain independent.
    Microsoft::WRL::ComPtr<ID3D12Device> chainDevice;
    // Keep the existing API12 probe: unrelated API11 consumers stay independent.
    if (FAILED(chain->GetDevice(IID_PPV_ARGS(&chainDevice)))) return ConsumerAccess::Safe;
    if (!chainDevice) return ConsumerAccess::Unavailable;
    Microsoft::WRL::ComPtr<ID3D12Resource> color;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> indexed;
    if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&indexed))) || !indexed ||
        FAILED(chain->GetBuffer(indexed->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&color))) || !color)
        return ConsumerAccess::Unavailable;
    // Alias checks use the actual resource device, as do writers/quarantine.
    // The chain's association device is not GPU execution authority.
    Microsoft::WRL::ComPtr<ID3D12Device> workDevice;
    if (FAILED(color->GetDevice(IID_PPV_ARGS(&workDevice))) || !workDevice) return ConsumerAccess::Unavailable;
    key.device = Key(workDevice.Get()); key.color = Key(color.Get());
    return CheckLocked(state, key);
}

namespace {
const char* OutputText(OutputKind output) noexcept
{
    switch (output) {
    case OutputKind::Original: return "original";
    case OutputKind::GeneratedUnavailable: return "generated-unavailable";
    case OutputKind::UnknownPhysical: return "unknown-physical";
    default: return "unknown";
    }
}
// Observes the actual result on EVERY exit after admission. It never assigns a
// reason, reads SDK/COM objects, changes a gate or owns/releases GPU resources.
// Declared after executionMutex's guard so logging stays serialized and all
// natural identities remain protected until this destructor returns.
// Emission stays inside StageReportScope's original ChangeReports allowance.
void ReportBeforeSrDonorDiagnostics(const BeforeSrDonorDiagnostics& diagnostic,
    unsigned donorCount, uint64_t observation)
{
    const char* boundary = diagnostic.boundary == 1 ? "initial-admission" :
        diagnostic.boundary == 2 ? "post-record-current-check" : "not-resolved";
    LOG_INFO("HoYoShade BeforeSR donor diagnostics observation={} diagnostic_boundary={} donor_count={} resolver_unique={} nonempty_candidates={} live_candidates={} cached_valid_selection_candidates={} diagnostic_tuples={} diagnostic_truncated={} empty_slots=not-sampled token_fields=separate-from-candidate-tuples count_authority=ResolveBeforeSrSelectionDonor tuple_authority=none depth_binding=UNKNOWN depth_completion=UNKNOWN pixels=UNKNOWN",
        observation, boundary, donorCount, donorCount == 1, diagnostic.nonempty, diagnostic.live,
        diagnostic.validSelection, diagnostic.count, diagnostic.truncated);
    for (unsigned i = 0; i < diagnostic.count; ++i) {
        const auto& candidate = diagnostic.candidates[i];
        const auto observed = [&candidate](unsigned bit, bool value) noexcept {
            return (candidate.reads & bit) ? (value ? "true" : "false") : "UNKNOWN";
        };
        LOG_INFO("HoYoShade BeforeSR donor firstfalse observation={} diagnostic_boundary={} diagnostic_slot={} firstfalse={} candidate_predicates_passed={} cached_live={} cached_runtime=0x{:x} cached_id_runtime=0x{:x} cached_api=0x{:x} cached_sdk_device=0x{:x} cached_sdk_queue=0x{:x} cached_native_device=0x{:x} cached_native_queue=0x{:x} cached_lifetime={} cached_epoch={} cached_owner_thread={} current_thread_known={} current_thread={} cached_armed={} cached_rendered={} cached_invalid={} can_arm={} armable={} cached_copied_serial={} tuple_authority=none",
            observation, boundary, candidate.slot, BeforeSrDonorRejectText(candidate.firstFalse), candidate.admitted, candidate.live,
            candidate.runtime, candidate.identity.runtime, candidate.identity.api, candidate.identity.sdkDevice,
            candidate.identity.sdkQueue, candidate.identity.nativeDevice, candidate.identity.nativeQueue,
            candidate.lifetime, candidate.frame.epoch, candidate.ownerThread, (candidate.reads & DonorThreadRead) != 0,
            candidate.currentThread, candidate.frame.armed, candidate.frame.rendered, candidate.frame.invalid,
            observed(DonorCanArmRead, candidate.canArm), candidate.armable, candidate.copiedSerial);
        LOG_INFO("HoYoShade BeforeSR donor settings observation={} diagnostic_boundary={} diagnostic_slot={} settings={} applied_settings_known={} applied_settings={} desired_settings_known={} desired_settings={} natural_status_incomplete={} event_cache_exhausted={} event_mutation_exhausted={} cached_source_settings={} cached_settings_barrier_epoch={} settings_epoch_ready={} read_mask=0x{:x} later_callable_predicates=not-evaluated-after-firstfalse tuple_authority=none",
            observation, boundary, candidate.slot, candidate.settings, (candidate.reads & DonorAppliedRead) != 0,
            candidate.applied, (candidate.reads & DonorDesiredRead) != 0, candidate.desired,
            observed(DonorStatusRead, candidate.statusIncomplete), observed(DonorCacheRead, candidate.cacheExhausted),
            observed(DonorMutationRead, candidate.mutationExhausted), candidate.sourceSettings, candidate.settingsBarrier,
            observed(DonorSettingsEpochRead, candidate.settingsEpochReady), candidate.reads);
        LOG_INFO("HoYoShade BeforeSR donor selection observation={} diagnostic_boundary={} diagnostic_slot={} cached_valid={} cached_refresh={} cached_global_enabled={} files_present={} config_empty={} preset_empty={} cached_selection_runtime=0x{:x} cached_selection_lifetime={} cached_graph_known={} cached_graph_valid={} has_enabled_techniques={} cached_selection_revision={} cached_snapshots={} cached_graph_builds={} event_watch_read={} event_runtime=0x{:x} event_revision={} event_topology={} event_reload_observed={} event_values=UNKNOWN-unless-event-watch-read tuple_authority=none depth_binding=UNKNOWN depth_completion=UNKNOWN",
            observation, boundary, candidate.slot, candidate.selectionValid, candidate.selectionRefresh, candidate.selectionEnabled,
            observed(DonorFilesRead, candidate.selectionFiles), observed(DonorConfigRead, candidate.configEmpty),
            observed(DonorPresetRead, candidate.presetEmpty), candidate.selectionRuntime, candidate.selectionLifetime,
            candidate.graphKnown, candidate.graphValid, observed(DonorEnabledTechniquesRead, candidate.hasEnabledTechniques),
            candidate.revision, candidate.snapshots, candidate.graphBuilds, (candidate.reads & DonorEventWatchRead) != 0,
            Key(candidate.events.runtime), candidate.events.revision, candidate.events.topology,
            observed(DonorEventWatchRead, candidate.events.reloadObserved));
    }
}
struct StageReportScope
{
    const ColorEndpoint& endpoint;
    const StageResult& result;
    uint64_t naturalRuntime = 0, naturalLifetime = 0;
    NaturalAssociation::SelectionDonor donor;
    Identity donorIdentity;
    unsigned donorCount = 0;
    BeforeSrDonorDiagnostics donorDiagnostics;
    uint64_t ownedDevice = 0, ownedQueue = 0, ownedGeneration = 0;
    AssociationDiagnostics::Snapshot association;
    // 0=before matching, 1=source matching, 2=suppression ambiguous,
    // 3=parent/child mismatch, 4=suppression queue/API mismatch, 5=natural list.
    unsigned associationExit = 0;
    NaturalAssociation::Predicate mapping = NaturalAssociation::Predicate::Missing;
    uint64_t mappedOwner = 0;
    ~StageReportScope() noexcept
    {
        try {
            // Commit before rate-limited diagnostics: status must not depend
            // on whether a log emits or its sink throws. executionMutex is held.
            Get().latestStage = {true, endpoint.stage, result.reason, result.attempt.state,
                result.techniques, naturalRuntime, naturalLifetime, endpoint.stream,
                endpoint.generation, endpoint.frame, endpoint.output, result.attempt.naturalSuppressed};
            const auto stage = static_cast<unsigned>(endpoint.stage);
            const auto output = static_cast<unsigned>(endpoint.output);
            if (stage < 1 || stage > 3 || output >= 3) return;
            StageObservation value{result.reason, result.attempt.state, endpoint.stream, endpoint.generation,
                naturalRuntime, naturalLifetime, result.attempt.naturalSuppressed, association, associationExit, mapping, mappedOwner,
                endpoint.colorMode, result.nativeColor};
            value.donorCount = donorCount; value.donorRevision = donor.revision; value.donorSnapshots = donor.snapshots;
            value.depthBindingAtRender = result.depthAvailable;
            if (OwnedStage::Handles(endpoint)) value.persistentOwnedPhase = static_cast<unsigned>(OwnedStage::Get().phase) + 1;
            value.donorDiagnosticBoundary = donorDiagnostics.boundary;
            // Compare only candidate identity/reason transitions. Epoch/thread
            // tuples themselves do not turn a stable heartbeat into frame churn.
            for (unsigned i = 0; i < donorDiagnostics.count; ++i) {
                const auto& candidate = donorDiagnostics.candidates[i];
                value.donorCandidateChanges[i] = {candidate.runtime, candidate.lifetime, candidate.firstFalse,
                    candidate.live, candidate.selectionValid};
            }
            value.presentColor = endpoint.beforeNrNativeColor.observation;
            const auto report = Get().stageReports[stage - 1][output].Next(value, GetTickCount64());
            if (OwnedStage::Handles(endpoint)) {
                if (report.emit) {
                    const auto& persistent = OwnedStage::Get();
                    const auto fields = OwnedRuntime::Fields(persistent.detail);
                    LOG_INFO("HoYoShade persistent owned stage={} phase={} result={} source_stream={} source_generation={} source_frame={} config_runtime=0x{:x} config_lifetime={} config_revision={} private_generation={} restoration_debt={} completion_unknown={} owned_status={} owned_operation={} owned_code_kind={} owned_code=0x{:08x} submission={} techniques={} depth_binding_at_render={} natural_sdk=owner-callback-only list=owned-immediate config_same_frame=not-required pixels=UNKNOWN",
                        Text(endpoint.stage), OwnedStage::Text(persistent.phase), Text(result.reason),
                        endpoint.stream, endpoint.generation, endpoint.frame, persistent.natural, persistent.lifetime,
                        persistent.revision, persistent.binding.generation, persistent.owesRestore, persistent.uncertain,
                        fields.status, fields.operation, fields.codeKind, fields.code,
                        static_cast<unsigned>(result.attempt.state), result.techniques, result.depthAvailable);
                }
                return; // old natural-frame/QI association is not configuration snapshot authority
            }
#ifndef HOYOSHADE_STAGE_TEST
                // One bounded diagnostic line per target stage attempt, at most eight total. A cached B
                // pointer narrows the diagnostic candidate, never proves a frame.
                if (Get().canonicalStageReports < 8 && endpoint.stage == Stage::BeforeNr &&
                    endpoint.output == OutputKind::Original &&
                    endpoint.colorMode == ColorMode::GenshinNativeTypedBeforeNr &&
                    endpoint.naturalContract == NaturalSourceContract::TypedBackbuffer && endpoint.naturalSource &&
                    endpoint.beforeNrNativeColor.picture) {
                    const auto& proof = *endpoint.naturalSource;
                    const auto& sourceEvidence = proof.Observation();
                    if (sourceEvidence.api == 0xb000 && sourceEvidence.predicate == NaturalAssociation::Predicate::Captured) {
                        // Capture already ran for this stage. The first line
                        // therefore has no source QI; arming samples a later
                        // Capture on this same stream/thread, never this B by age.
                        NaturalAssociation::ArmCanonicalSourceObservation(sourceEvidence.stream, sourceEvidence.thread);
                        State::Natural* candidate = nullptr;
                        unsigned candidates = 0;
                        for (auto& entry : Get().natural) {
                            if (!entry.live || entry.id.api != 0xb000) continue;
                            bool hasBuffer = false;
                            for (auto buffer : entry.backbuffers) hasBuffer |= buffer && buffer == sourceEvidence.buffer;
                            if (hasBuffer) { ++candidates; if (candidates == 1) candidate = &entry; }
                        }
                        if (candidates != 1) candidate = nullptr;
                        const auto& sourceCanonical = proof.CanonicalDevice();
                        const auto naturalCanonical = candidate ? candidate->nativeCanonical :
                            NaturalAssociation::CanonicalDeviceObservation{};
                        // The cached B match and independent QI samples do not bind the natural
                        // callback lifetime to this source capture or prove a common picture.
                        // Keep canonical_relation UNKNOWN even if ticketCurrent is independently true.
                        // Adapter generation/frame and ticket producer
                        // generation/sourceFrame are different clocks. Only a
                        // successful native admission has already matched both
                        // through the private capability/Scope; raw B reuse is
                        // never sufficient to expose ticket serials.
                        const auto* scope = GenshinPresentNr::Scope::Current();
                        const bool colorProven = result.nativeColor == NativeColorAdmission::ExactNativePresentPicture ||
                            result.nativeColor == NativeColorAdmission::ExactNativePresentSrgbCopy;
                        const bool ticketCurrent = colorProven &&
                            sourceEvidence.stream == endpoint.stream &&
                            sourceEvidence.generation == endpoint.generation &&
                            sourceEvidence.frame == endpoint.frame &&
                            scope && scope->MatchesNativeColor(endpoint.beforeNrNativeColor.picture,
                                endpoint.color, endpoint.queue, endpoint.stream, endpoint.generation, endpoint.frame,
                                endpoint.naturalDevice, endpoint.naturalBackbuffer, endpoint.width, endpoint.height,
                                endpoint.beforeNrNativeColor.observation) && scope->ticket.Known() &&
                            scope->ticket.source.sourceFrame != 0 &&
                            scope->ticket.source.thread == GetCurrentThreadId() &&
                            scope->ticket.source.thread == sourceEvidence.thread &&
                            scope->ticket.swapchain == sourceEvidence.chain &&
                            reinterpret_cast<uintptr_t>(scope->ticket.backbuffer.Get()) == sourceEvidence.buffer;
                        const bool firstSourceQiExpectedAbsent = Get().canonicalStageReports == 0 &&
                            !sourceCanonical.attempted;
                        const auto ticketField = [ticketCurrent](uint64_t value) {
                            return ticketCurrent ? std::to_string(value) : std::string("UNKNOWN");
                        };
                        ++Get().canonicalStageReports;
                        LOG_INFO("HoYoShade BeforeNR canonical device diagnostic sample={} candidate_count={} natural_runtime=0x{:x} natural_lifetime={} natural_raw=0x{:x} natural_canonical=0x{:x} natural_qi_attempted={} natural_qi_hr=0x{:08x} natural_callback_thread={} natural_epoch={} source_raw=0x{:x} source_canonical=0x{:x} source_qi_attempted={} source_qi_hr=0x{:08x} source_qi_first_sample_expected_absent={} source_capture_thread={} source_stream={} source_adapter_generation={} source_adapter_frame={} config_epoch={} ticket_current={} ticket_producer_generation={} ticket_producer_source_frame={} device_epoch={} present_epoch={} picture_serial={} canonical_relation=UNKNOWN simultaneous_device_liveness=UNKNOWN same_frame=UNKNOWN raw_backbuffer_match=observation-only admission=unchanged pixels=UNKNOWN",
                            Get().canonicalStageReports, candidates, candidate ? Key(candidate->runtime) : 0,
                            candidate ? candidate->lifetime : 0, candidate ? candidate->id.nativeDevice : 0,
                            naturalCanonical.identity, naturalCanonical.attempted, static_cast<uint32_t>(naturalCanonical.hr),
                            candidate ? candidate->canonicalThread : 0, candidate ? candidate->frame.epoch : 0,
                            sourceEvidence.owner, sourceCanonical.identity, sourceCanonical.attempted,
                            static_cast<uint32_t>(sourceCanonical.hr), firstSourceQiExpectedAbsent, sourceEvidence.thread,
                            sourceEvidence.stream, sourceEvidence.generation, sourceEvidence.frame,
                            endpoint.beforeNrNativeColor.picture->ConfigEpoch(), ticketCurrent,
                            ticketField(ticketCurrent ? scope->ticket.source.generation : 0),
                            ticketField(ticketCurrent ? scope->ticket.source.sourceFrame : 0),
                            ticketField(ticketCurrent ? scope->ticket.source.deviceEpoch : 0),
                            ticketField(ticketCurrent ? scope->ticket.presentEpoch : 0),
                            ticketField(ticketCurrent ? scope->ticket.pictureSerial : 0));
                        if (Get().canonicalStageReports == 8)
                            NaturalAssociation::DisarmCanonicalSourceObservation();
                    }
                }
#endif
            if (report.emit) {
                LOG_INFO("HoYoShade stage decision stage={} output_kind={} output_label={} result={} observation={} source_stream={} source_generation={} source_frame={} output_sequence={} natural_runtime=0x{:x} natural_lifetime={} submission={} natural_suppressed={} techniques={} extent={}x{} sdr={} state_known={} state={} producer_value={} depth_binding_at_render={} depth_reader_completion=UNKNOWN depth_gpu_completion=UNKNOWN pixels=UNKNOWN",
                    Text(endpoint.stage), output, OutputText(endpoint.output), Text(result.reason), report.count,
                    endpoint.stream, endpoint.generation, endpoint.frame, endpoint.outputToken.sequence,
                    naturalRuntime, naturalLifetime, static_cast<unsigned>(result.attempt.state),
                    result.attempt.naturalSuppressed, result.techniques, endpoint.width, endpoint.height,
                    endpoint.sdr, endpoint.stateKnown, static_cast<unsigned>(endpoint.state), endpoint.ready.value,
                     result.depthAvailable ? "reflected" : "not-observed");
                const auto& pictureColor = endpoint.beforeNrNativeColor.observation;
                LOG_INFO("HoYoShade color contract stage={} mode={} native_admission={} source_generation={} source_frame={} sdr={} native_transfer={} source_format={} shared_format={} raw_dxgi_space={} raw_provenance={} private_lookup_hr=UNKNOWN known_hdr_rejection={} adapter_color_conversion=none preset_srgb_states=preserved pixels=UNKNOWN",
                    Text(endpoint.stage), Text(endpoint.colorMode), Text(result.nativeColor), endpoint.generation,
                    endpoint.frame, endpoint.sdr, endpoint.colorMode == ColorMode::TransferQualified ? "not-requested" : "UNKNOWN",
                    static_cast<unsigned>(pictureColor.sourceFormat), static_cast<unsigned>(pictureColor.sharedFormat),
                    static_cast<unsigned>(pictureColor.rawColorSpace),
                    pictureColor.origin == native::NativePictureColorOrigin::SwapchainHelperDefaultOrPrivateUnknown ?
                        "exact-swapchain-helper-default-or-private-UNKNOWN" : "unobserved",
                    pictureColor.knownHdr);
                if (endpoint.naturalContract == NaturalSourceContract::BeforeSrOwnedRuntimeWithSelectionDonor) {
                    LOG_INFO("HoYoShade BeforeSR contracts observation={} source_api=0xb000 source_device=0x{:x} color_resource=0x{:x} consumer_queue=0x{:x} donor_count={} donor_predicate={} donor_runtime=0x{:x} donor_lifetime={} donor_epoch={} donor_selection_revision={} donor_config_generation={} donor_settings={} natural_api=0x{:x} natural_native_device=0x{:x} natural_sdk_device=0x{:x} owned_api=0xc000 owned_binding_device=0x{:x} owned_binding_queue=0x{:x} owned_binding_generation={} source_contract=exact-genshin-producer-four-resources gpu_contract=owned-exact-color-device-direct-queue donor_contract=selection-and-bounded-suppression-only typed_backbuffer=false native_transfer=UNKNOWN pixels=UNKNOWN",
                        report.count, endpoint.naturalDevice, Key(endpoint.color), Key(endpoint.queue), donorCount,
                        NaturalAssociation::Text(mapping), donor.runtime, donor.lifetime, donor.epoch, donor.revision,
                        donor.snapshots, donor.settings, donorIdentity.api, donorIdentity.nativeDevice, donorIdentity.sdkDevice,
                        ownedDevice, ownedQueue, ownedGeneration);
                    ReportBeforeSrDonorDiagnostics(donorDiagnostics, donorCount, report.count);
                    return; // Not a typed-backbuffer diagnostic or association.
                }
                const auto& counts = Get().naturalCallbacks;
                LOG_INFO("HoYoShade natural association stage={} output_kind={} observation={} stage_attempts={} present_attempts={} match={} exit={} live={} tombstones={} device_matches={} backbuffer_matches={} exact_matches={} ticket_runtime_matches={} ticket_lifetime_matches={} status_incomplete={} expected_device=0x{:x} expected_backbuffer=0x{:x} consumer_queue=0x{:x} source_stream={} source_runtime=0x{:x} source_lifetime={} source_serial={} copied_before_natural={} init={} present={} destroy={} begin={} finish={} technique={} reload={} retry=next-existing-stage-call report_min_ms=1000 report_heartbeat_ms=30000 runtime_existence=UNKNOWN pixels=UNKNOWN",
                    Text(endpoint.stage), output, report.count, Get().stageAttempts, Get().presentAttempts,
                    AssociationDiagnostics::Text(association.match), associationExit, association.live, association.tombstones,
                    association.devices, association.backbuffers, association.exact, association.ticketRuntime, association.ticketLifetime,
                    Get().naturalStatusIncomplete, endpoint.naturalDevice, endpoint.naturalBackbuffer, Key(endpoint.queue), endpoint.stream,
                    endpoint.physicalSource.runtime, endpoint.physicalSource.lifetime, endpoint.physicalSource.serial,
                    endpoint.physicalSource.copiedBeforeNatural, counts.Count(RuntimeEvent::Init), counts.Count(RuntimeEvent::Present),
                    counts.Count(RuntimeEvent::Destroy), counts.Count(RuntimeEvent::Begin), counts.Count(RuntimeEvent::Finish),
                    counts.Count(RuntimeEvent::Technique), counts.Count(RuntimeEvent::Reload));
                const auto source = endpoint.naturalSource ? endpoint.naturalSource->Observation() : NaturalAssociation::Evidence{};
                const auto binding = endpoint.naturalSource && source.thread == GetCurrentThreadId() ?
                    endpoint.naturalSource->BoundRuntime() : NaturalAssociation::Binding{};
                LOG_INFO("HoYoShade natural association candidates observation={} first_live_runtime=0x{:x} lifetime={} api=0x{:x} device=0x{:x} queue=0x{:x} backbuffer0=0x{:x} first_device_runtime=0x{:x} lifetime={} api=0x{:x} device=0x{:x} queue=0x{:x} backbuffer0=0x{:x} second_candidate_sample=NOT_RECORDED cached_only=true diagnostic_backbuffer_required=typed-source-only legacy_before_sr_backbuffer=absent-by-contract execution_policy=typed-owner-and-bound-frame legacy_before_sr_policy=unique-exact-api11-device parent_child_policy=existing-exact-ticket-only source_mapping={} source_api=0x{:x} source_chain=0x{:x} source_device=0x{:x} chain_device=0x{:x} resource_owner=0x{:x} capture_predicate={} capture_hr=0x{:08x} independent_buffer_matches={} api_matches={} authority=association-only runtime_epoch=pinned-lifetime-and-epoch source_runtime=0x{:x} source_lifetime={} source_epoch={} owner_query_has_hr={} pixels=UNKNOWN",
                    report.count, association.firstLive.runtime, association.firstLive.lifetime, association.firstLive.api,
                    association.firstLive.device, association.firstLive.queue, association.firstLive.backbuffer0,
                    association.firstDevice.runtime, association.firstDevice.lifetime, association.firstDevice.api,
                    association.firstDevice.device, association.firstDevice.queue, association.firstDevice.backbuffer0,
                    NaturalAssociation::Text(mapping), source.api, source.chain, source.sourceDevice, source.chainDevice, mappedOwner,
                    NaturalAssociation::Text(source.predicate), static_cast<uint32_t>(source.hr), association.independentBuffers, association.apis,
                    binding.runtime, binding.lifetime, binding.epoch, source.api == 0xc000);
            }
        } catch (...) {} // Reporting must never alter the returned StageResult.
    }
};
}

void CaptureNaturalDomain(const NaturalAssociation::Source* source) noexcept
{
    using namespace DeviceDomain;
    // Bind is called for every acquired source, including default-off frames.
    if(!source || !Get().enabled.load(std::memory_order_acquire))return;
    try {
        std::lock_guard lifetime(Get().executionMutex);
        if(auto* outer=Capture::Current()) {outer->Fail(Result::Nested);source->domain_.result=Result::Nested;return;}
        const auto& e=source->Observation();
        // One bounded process stream, not one logger per resource/runtime.
        // These observations cannot turn a failed gate into admission.
        const auto deferred=[&](std::string_view gate,DWORD runtimeThread=0) noexcept {
            try {
                static Diagnostics::ChangeReports<std::string_view> reports;
                const auto report=reports.Next(gate,GetTickCount64());
                if(report.emit)LOG_INFO("HoYoShade device domain deferred gate={} observation={} source_thread={} current_thread={} runtime_thread={} source_owner=0x{:x} resource=0x{:x} association_only=true gpu_submission=false gate_schema=1",
                    gate,report.count,e.thread,GetCurrentThreadId(),runtimeThread,e.owner,e.buffer);
            } catch(...) {}
        };
        // Temporary failures never become stable negative cache entries.
        if(inCallback) {deferred("callback-context");return;}
        if(ownedOperation) {deferred("owned-operation");return;}
        if(!Get().accepted.load(std::memory_order_acquire)) {deferred("core-not-accepted");return;}
        if(!Get().domainSubscriptionRequested.load(std::memory_order_acquire)) {deferred("subscription-not-requested");return;}
        if(Get().naturalStatusIncomplete) {deferred("observation-incomplete");return;}
        if(e.api!=0xb000) {deferred("api-not-d3d11");return;}
        if(!source->owner11_ || !source->buffer11_) {deferred("typed-source-missing");return;}
        if(source->Endpoint(e.stream,e.generation,e.frame,e.sourceDevice,e.buffer)!=NaturalAssociation::Predicate::Captured) {
            deferred("source-endpoint");return;
        }
        State::Natural* candidate=nullptr;
        for(auto& entry:Get().natural) {
            if(!entry.runtime || !entry.live || entry.id.api!=e.api)continue;
            bool buffer=false;for(auto b:entry.backbuffers)buffer|=b && b==e.buffer;
            if(!buffer)continue;
            // Exact native-owner matching needs no bootstrap. Buffer-only
            // narrowing below is never an admission predicate.
            if(entry.id.nativeDevice==e.owner)return;
            if(candidate) {deferred("candidate-ambiguous");return;}candidate=&entry;
        }
        if(!candidate) {deferred("candidate-missing");return;}
        if(candidate->thread!=GetCurrentThreadId()) {deferred("candidate-thread",candidate->thread);return;}
        if(!candidate->lifetime || !candidate->frame.epoch || candidate->frame.epoch==UINT64_MAX) {deferred("lifetime-epoch");return;}
        if(candidate->frame.invalid || candidate->domainBuffersPending) {deferred("resource-lifecycle");return;}
        // This typed Create submits no GPU work and consumes no core frame.
        // Discover the resource domain even after natural effects or before
        // selection loading completes. Bind below STILL requires CanArm; the
        // existing RenderStage/physical-source reader and selection gates stay.
        // Without this separation, a consumed frame prevents even establishing
        // the evidence needed to select its runtime on a later usable frame.
        const auto runtime=Key(candidate->runtime), life=candidate->lifetime, epoch=candidate->frame.epoch;
        const auto sdk=candidate->id.sdkDevice, native=candidate->id.nativeDevice;
        const auto current=[&]() noexcept {
            return candidate->live && Key(candidate->runtime)==runtime && candidate->lifetime==life &&
                candidate->id.sdkDevice==sdk && candidate->id.nativeDevice==native && candidate->id.api==e.api &&
                candidate->frame.epoch==epoch && !candidate->frame.invalid && !candidate->domainBuffersPending &&
                candidate->thread==GetCurrentThreadId();
        };
        Marker marker;
        const auto read=ReadMarker(source->buffer11_.Get(),marker);
        if(read==MarkerRead::Invalid) {source->domain_.result=Result::MarkerForeign;return;}
        if(read==MarkerRead::KnownShape) {
            bool issued=false;
            for(const auto& record:Get().domainMarkers)issued|=record.marker==marker && record.owner==e.owner && record.buffer==e.buffer;
            if(!issued) {source->domain_.result=Result::MarkerForeign;return;}
        } else {
            State::MarkerRecord* slot=nullptr;
            for(auto& record:Get().domainMarkers)if(!record.marker) {slot=&record;break;}
            if(!slot || Get().nextDomainMarker==UINT64_MAX) {source->domain_.result=Result::Capacity;return;}
            uint64_t nonce=0;
            if(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&nonce),sizeof(nonce),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0 || !nonce) {
                source->domain_.result=Result::MarkerIo;return;
            }
            marker={++Get().nextDomainMarker,nonce};
            if(FAILED(source->buffer11_->SetPrivateData(MarkerGuid,sizeof(marker),&marker))) {
                source->domain_.result=Result::MarkerIo;return;
            }
            *slot={marker,e.owner,e.buffer};
            Marker checked;
            if(ReadMarker(source->buffer11_.Get(),checked)!=MarkerRead::KnownShape || checked!=marker) {
                source->domain_.result=Result::MarkerForeign;return;
            }
        }
        if(!current()) {source->domain_.result=Result::LifetimeChanged;return;}
        DomainCache* cache=nullptr;
        for(auto& c:candidate->domains)if(c.marker==marker && c.buffer==e.buffer && c.owner==e.owner && c.lifetime==life) {
            source->domain_=c.witness;source->marker_=marker;
            source->domain_.runtime=runtime;source->domain_.lifetime=life;source->domain_.epoch=epoch;return;
        }
        for(auto& c:candidate->domains)if(!c.marker) {cache=&c;break;}
        // At most eight exact BBs between public lifecycle signals. Runtime
        // init/destroy, device destroy, swapchain init/destroy and reload clear
        // these POD slots. An ordinary Present/epoch never clears them.
        if(!cache || Get().nextDomainStamp==UINT64_MAX) {source->domain_.result=Result::Capacity;return;}
        Witness witness;
        // End the callback interval BEFORE releasing its real typed return.
        // Release-time callbacks cannot impersonate the Create return event.
        TypedRtv returned;
        {
            Capture capture(++Get().nextDomainStamp,e.buffer);
            if(!capture.Acquired()) {source->domain_=capture.value;return;}
            try {
                const auto hr=returned.Create(source->owner11_.Get(),source->buffer11_.Get());
                witness=capture.Returned(hr,returned,e.owner);
            } catch(...) {capture.Fail(Result::Exception);witness=capture.value;}
        }
        if(!current() || source->Endpoint(e.stream,e.generation,e.frame,e.sourceDevice,e.buffer)!=NaturalAssociation::Predicate::Captured)
            witness.result=Result::LifetimeChanged;
        Marker checked;
        if(witness.result==Result::Accepted &&
            (ReadMarker(source->buffer11_.Get(),checked)!=MarkerRead::KnownShape || checked!=marker))witness.result=Result::MarkerForeign;
        // Marker IO is also a public COM call and may deliver lifecycle events.
        if(!current())witness.result=Result::LifetimeChanged;
        if(witness.result==Result::Accepted && !Matches(witness,e.api,sdk,native,e.buffer,e.owner))witness.result=Result::CallbackMismatch;
        // Ordinary registration returns void. Missing delivery is a completed
        // failed probe, not a claim that the core supports this event. No retry
        // per Present/epoch; only a new resource marker/runtime lifetime probes.
        // A reentrant lifecycle change is temporary, not a completed stable
        // negative probe. Do not repopulate a slot that its callback revoked.
        if(witness.result!=Result::LifetimeChanged)*cache={marker,witness,e.owner,e.buffer,life};
        witness.runtime=runtime;witness.lifetime=life;witness.epoch=epoch;
        source->domain_=witness;source->marker_=marker;
        LOG_INFO("HoYoShade device domain probe result={} stamp={} api=0x{:x} sdk_device=0x{:x} native_device=0x{:x} resource=0x{:x} view=0x{:x} callbacks={} create_hr=0x{:08x} creator=0x{:x} returned_owner=0x{:x} association_only=true frame_available={} selection_ready={} runtime=0x{:x} lifetime={} epoch={} gpu_submission=false cache=resource-marker-and-runtime-lifetime cache_retains_BB_or_RTV=false",
            DeviceDomain::Text(witness.result),witness.stamp,witness.api,witness.sdkDevice,witness.nativeDevice,witness.resource,
            witness.view,witness.callbacks,static_cast<unsigned>(witness.createHr),witness.creatorDevice,witness.returnedOwner,
            candidate->frame.CanArm(),candidate->selectionStatus.selectionReady,runtime,life,epoch);
    } catch(...) {source->domain_.result=DeviceDomain::Result::Exception;}
}

void BindNaturalSource(const NaturalAssociation::Source* source) noexcept
{
    if (!source || !Get().enabled.load(std::memory_order_acquire) ||
        source->Observation().thread != GetCurrentThreadId()) return;
    try {
    std::lock_guard lifetime(Get().executionMutex);
    // Binding never samples a cached native pointer or an SDK virtual method.
    // Only live callback observations inside this lock are candidates.
    if (source->binding_.predicate != NaturalAssociation::Predicate::Unbound) return;
    auto& bound = source->binding_;
    const auto& value = source->Observation();
    bound.predicate = source->Endpoint(value.stream,value.generation,value.frame,value.sourceDevice,value.buffer);
    if (bound.predicate != NaturalAssociation::Predicate::Captured) return;
    if (!Get().accepted.load(std::memory_order_acquire) || Get().naturalStatusIncomplete) {
        bound.predicate = NaturalAssociation::Predicate::ObservationIncomplete; return;
    }
    CaptureNaturalDomain(source);
    bound.predicate = NaturalAssociation::Predicate::NoRuntime;
    const State::Natural* selected = nullptr;
    for (const auto& entry : Get().natural) {
        if (!entry.runtime || !entry.live || source->Runtime(entry.id.api,entry.id.nativeDevice,entry.backbuffers,entry.id.sdkDevice) !=
            NaturalAssociation::Predicate::Captured) continue;
        if (selected) { bound.predicate = NaturalAssociation::Predicate::Ambiguous; return; }
        selected = &entry;
    }
    if (!selected) return;
    // Typed calls may reenter lifecycle callbacks. Neither domain discovery
    // nor an accepted cached witness grants a frame or revives an endpoint.
    bound.predicate=source->Endpoint(value.stream,value.generation,value.frame,value.sourceDevice,value.buffer);
    if(bound.predicate!=NaturalAssociation::Predicate::Captured)return;
    if (!Get().accepted.load(std::memory_order_acquire) || Get().naturalStatusIncomplete) {
        bound.predicate=NaturalAssociation::Predicate::ObservationIncomplete;return;
    }
    const auto& domain=source->domain_;
    if(domain.result==DeviceDomain::Result::Accepted) {
        const NaturalAssociation::Binding pin{NaturalAssociation::Predicate::Captured,domain.runtime,domain.lifetime,domain.epoch};
        bound.predicate=NaturalAssociation::CheckBinding(pin,Key(selected->runtime),selected->lifetime,selected->frame.epoch);
        if(bound.predicate!=NaturalAssociation::Predicate::Captured)return;
    }
    if (selected->thread != GetCurrentThreadId() || !selected->frame.CanArm() ||
        !selected->lifetime || !selected->frame.epoch || selected->frame.epoch == UINT64_MAX) {
        bound.predicate = NaturalAssociation::Predicate::FrameUnavailable; return;
    }
    bound = {NaturalAssociation::Predicate::Captured,Key(selected->runtime),selected->lifetime,selected->frame.epoch};
    } catch (...) {} // No lock/SDK/allocation exception may terminate NativeDriver.
}

// R061: only wrappers enrolled here may retain direct backbuffers across calls.
// Raw/unrecognized chains retain the original scratch path.
void RegisterDirectChain(IDXGISwapChain* chain, const void* wrapper) noexcept
{
    if (!chain || !wrapper || ownedOperation) return;
    try {
        std::lock_guard lifetime(Get().executionMutex);
        auto* slot = FindDirectChain(Key(chain));
        if (slot) {
            RefreshDirectOwners(*slot);
            const auto access = slot->access.load(std::memory_order_acquire);
            if (!(access & DirectClosed) || (access & DirectOwners) || access >= DirectResizeStep) return;
        } else for (auto& c : Get().directChains) {
            if (c.access.load(std::memory_order_acquire) & DirectClosed) RefreshDirectOwners(c);
            const auto access = c.access.load(std::memory_order_acquire);
            if (!c.chain.load(std::memory_order_acquire) || access == DirectClosed) { slot = &c; break; }
        }
        if (!slot || Get().nextDirectLifetime == UINT64_MAX) return; // bounded scratch fallback
        RevokeObservationDemand();
        slot->access.store(DirectClosed, std::memory_order_release);
        slot->lifetime.store(++Get().nextDirectLifetime, std::memory_order_release);
        slot->epoch = 1; slot->draining = false;
        slot->wrapper.store(Key(wrapper), std::memory_order_release);
        slot->chain.store(Key(chain), std::memory_order_release);
        slot->access.store(0, std::memory_order_release);
    } catch (...) {} // enrollment never changes wrapper creation success
}
void ForgetDirectChain(IDXGISwapChain* chain, const void* wrapper) noexcept
{
    auto* c = FindDirectChain(Key(chain));
    if (!c || c->wrapper.load(std::memory_order_acquire) != Key(wrapper)) return;
    c->access.fetch_or(DirectClosed, std::memory_order_acq_rel);
    std::unique_lock lifetime(Get().executionMutex, std::try_to_lock);
    if (!lifetime.owns_lock()) return; // retain bounded old owners, never force release
    c->draining = true; InvalidateDirect(Key(chain));
    if (c->epoch != UINT64_MAX) ++c->epoch;
    RefreshDirectOwners(*c);
    // Keep a scalar tombstone; a reused pointer needs a new wrapper enrollment.
}
DirectResizeScope::DirectResizeScope(IDXGISwapChain* chain, const void* wrapper) noexcept
{
    auto* c = FindDirectChain(Key(chain));
    if (!c || c->wrapper.load(std::memory_order_acquire) != Key(wrapper)) return; // original scratch-only path
    chainSlot_ = static_cast<unsigned>(c - Get().directChains.data());
    lifetime_ = c->lifetime.load(std::memory_order_acquire);
    const auto prior = c->access.fetch_add(DirectResizeStep, std::memory_order_acq_rel);
    if (prior > UINT64_MAX - DirectResizeStep) {
        c->access.fetch_or(DirectClosed, std::memory_order_acq_rel);
        state_ = OwnedRuntime::Retirement::Unknown; return;
    }
    // Recording covers the very first cache publication as well as cache hits.
    // The observation itself never refuses an idle disabled/scratch-only chain.
    if (prior & DirectRecording) { state_ = OwnedRuntime::Retirement::Pending; return; }
    if (!(prior & DirectOwners)) return;
    resourceOnly_ = true;
    c->resourceOnlyResize.fetch_add(1, std::memory_order_acq_rel);
    std::unique_lock lifetime(Get().executionMutex, std::try_to_lock);
    if (!lifetime.owns_lock() || inCallback || ownedOperation) { state_ = OwnedRuntime::Retirement::Pending; return; }
    if (c->lifetime.load(std::memory_order_acquire) != lifetime_) { state_ = OwnedRuntime::Retirement::Unknown; return; }
    OwnedOperation collection;
    if (!c->draining && c->epoch != UINT64_MAX) ++c->epoch;
    c->draining = true;
    InvalidateDirect(Key(chain));
    for (unsigned index = 0; index < Get().directEntries.size(); ++index) {
        auto& e = Get().directEntries[index];
        bool needed = false;
        for (const auto& slot : e.slots)
            needed |= slot.occupied && slot.key.chain == Key(chain) &&
                slot.key.chainLifetime == lifetime_ && DirectPending(slot);
        if (!needed) continue;
        const auto transactions = ReleaseDirectTransaction(e.entry);
        if (transactions != OwnedRuntime::Retirement::Ready) { state_ = transactions; return; }
        const auto reserved = OwnedRuntime::ReserveResources(e.entry, collections_[index]);
        if (reserved != OwnedRuntime::Status::Ready) {
            state_ = reserved == OwnedRuntime::Status::Busy || reserved == OwnedRuntime::Status::GpuPending
                ? OwnedRuntime::Retirement::Pending : OwnedRuntime::Retirement::Unknown;
            return;
        }
        const auto collected = collections_[index].Poll();
        if (collected != OwnedRuntime::Retirement::Ready) { state_ = collected; return; }
        // Pool collection is not a physical retirement receipt. Every bundle
        // must confirm both SDK view destruction and actual native owner release.
        for (const auto& slot : e.slots) {
            if (!slot.occupied || slot.key.chain != Key(chain) || slot.key.chainLifetime != lifetime_ || !slot.receipt) continue;
            const auto actual = slot.receipt->state.load(std::memory_order_acquire);
            if (actual != OwnedRuntime::Retirement::Ready) { state_ = actual; return; }
        }
    }
    RefreshDirectOwners(*c);
    if (c->access.load(std::memory_order_acquire) & DirectOwners) state_ = OwnedRuntime::Retirement::Pending;
}
DirectResizeScope::~DirectResizeScope()
{
    if (chainSlot_ >= Get().directChains.size()) return;
    auto& c = Get().directChains[chainSlot_];
    // Drop exact pool pins before allowing a new direct admission. No GPU work.
    for (auto& collection : collections_) collection = {};
    if (resourceOnly_) c.resourceOnlyResize.fetch_sub(1, std::memory_order_acq_rel);
    c.access.fetch_sub(DirectResizeStep, std::memory_order_acq_rel);
}
HRESULT DirectResizeScope::Result() const noexcept
{
    switch (state_) {
    case OwnedRuntime::Retirement::Ready: return S_OK;
    case OwnedRuntime::Retirement::Pending: return DXGI_ERROR_WAS_STILL_DRAWING;
    default: return E_FAIL; // uncertainty never reaches DXGI with retained old views
    }
}
void DirectResizeScope::Complete(HRESULT result) noexcept
{
    if (!resourceOnly_ || state_ != OwnedRuntime::Retirement::Ready || chainSlot_ >= Get().directChains.size()) return;
    auto& c = Get().directChains[chainSlot_];
    if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
        c.access.fetch_or(DirectClosed, std::memory_order_acq_rel); return;
    }
    std::unique_lock lifetime(Get().executionMutex, std::try_to_lock);
    if (lifetime.owns_lock() && c.lifetime.load(std::memory_order_acquire) == lifetime_) {
        RefreshDirectOwners(c);
        if (!(c.access.load(std::memory_order_acquire) & DirectOwners)) c.draining = false;
    }
}

StageResult RenderStage(const ColorEndpoint& endpoint) noexcept
{
    StageResult result;
    NativeColorFacts immutable; // synchronous call only; never moved into a GPU owner
    if (!Selected(endpoint.stage) || inCallback || ownedOperation || stageActive) return result;
    // No natural pointer outlives this lease. Destruction/init/resize/reload
    // callbacks take the same recursive lock; synchronous effects callbacks
    // can reenter without publishing an owned runtime as a natural one.
    std::lock_guard lifetime(Get().executionMutex);
    if (!Selected(endpoint.stage) || stageActive) return result;
    struct StageGuard { StageGuard() { RevokeObservationDemand(); stageActive = true; } ~StageGuard() { stageActive = false; } } stageGuard;
    Get().latestEpochWait = {};
    AssociationDiagnostics::Increment(Get().stageAttempts);
    StageReportScope observation{endpoint, result};
    OwnedStage::Admission persistentAdmission{endpoint, result};
    OwnedStage::SettingsChanged(OwnedStageSettings());
    try {
        result.reason = Get().core.load(std::memory_order_acquire);
        if (!Get().accepted.load(std::memory_order_acquire)) return result;
        auto* directChain = endpoint.naturalSource
            ? FindDirectChain(endpoint.naturalSource->Observation().chain) : nullptr;
        if (directChain && directChain->resourceOnlyResize.load(std::memory_order_acquire)) {
            result.reason = Reason::NoRuntime; return result;
        }
        {
            OwnedOperation collection;
            RetireCompletedTransactions();
            OwnedRuntime::TryCollect();
        }
        if (directChain && directChain->draining) {
            RefreshDirectOwners(*directChain);
            if (directChain->access.load(std::memory_order_acquire) & DirectOwners) {
                result.reason = Reason::NoRuntime; return result;
            }
            directChain->draining = false;
        }
        const bool physical = endpoint.output == OutputKind::UnknownPhysical;
        if (endpoint.output != OutputKind::Original && !physical) { result.reason = Reason::GeneratedUnavailable; return result; }
        if (physical && (endpoint.stage != Stage::FinalOutput || !endpoint.outputToken ||
            endpoint.outputToken.owner != endpoint.stream || endpoint.outputToken.generation != endpoint.generation ||
            !endpoint.physicalSource)) { result.reason = Reason::FinalEndpointUnavailable; return result; }
        if (!AdmitColor(endpoint, result, immutable)) { result.reason = Reason::ColorSpaceUnknown; return result; }
        if (!endpoint.stateKnown) { result.reason = Reason::StateUnknown; return result; }
        if (!endpoint.color || !endpoint.queue) {
            DeviceValidation::Snapshot evidence;
            evidence.Fail(!endpoint.color ? Predicate::ColorPresent : Predicate::ConsumerQueuePresent);
            ReportDeviceFailure(endpoint, Site::Stage, evidence);
            result.reason = Reason::DeviceMismatch; return result;
        }
        if (endpoint.queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
            result.reason = Reason::QueueMismatch; return result;
        }
        const bool nativeProducerBeforeSr = endpoint.naturalContract == NaturalSourceContract::BeforeSrNativeProducer;
        // A natural source proves selection/suppression only. Native BeforeSR
        // additionally requires the unforgeable, currently active exact-color
        // receipt minted AFTER its own prefix Execute/Signal. External NGX
        // recordings have no such authority and are never closed/submitted here.
        if (nativeProducerBeforeSr && (endpoint.stage != Stage::BeforeSr ||
            endpoint.output != OutputKind::Original || endpoint.colorMode != ColorMode::TransferQualified ||
            !endpoint.naturalSource || endpoint.naturalSource->Observation().api != 0xc000 || !endpoint.ready.submission)) {
            result.reason = Reason::ProducerUnsubmitted; return result;
        }
        const bool selectionDonorMode = endpoint.naturalContract == NaturalSourceContract::BeforeSrOwnedRuntimeWithSelectionDonor;
        if (selectionDonorMode && result.nativeColor != NativeColorAdmission::ExactNativeTyped) {
            result.reason = Reason::ColorSpaceUnknown; return result;
        }
        const bool nativePresentColor = endpoint.colorMode == ColorMode::GenshinNativeTypedBeforeNr;
        const auto desc = (selectionDonorMode || nativePresentColor) ? immutable.gpuDesc : endpoint.color->GetDesc();
        // Only the existing legacy SR input contract admits a bounded active
        // rectangle. Native Genshin/picture/output contracts remain exact.
        const bool srRegion = endpoint.stage == Stage::BeforeSr &&
            endpoint.colorMode == ColorMode::TransferQualified &&
            endpoint.naturalContract == NaturalSourceContract::BeforeSrExactDevice;
        const bool fullColor = !endpoint.colorX && !endpoint.colorY &&
            desc.Width == endpoint.width && desc.Height == endpoint.height;
        const bool boundedColor = endpoint.colorX <= desc.Width && endpoint.colorY <= desc.Height &&
            endpoint.width <= desc.Width - endpoint.colorX && endpoint.height <= desc.Height - endpoint.colorY;
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 ||
            desc.MipLevels != 1 || desc.SampleDesc.Count != 1 || desc.Width > UINT32_MAX ||
            desc.Height > UINT32_MAX || !endpoint.width || !endpoint.height || !(srRegion ? boundedColor : fullColor)) {
            result.reason = Reason::ExtentMismatch; return result;
        }
        reshade::api::resource_usage usage{};
        if (!ColorContract::Resolve(desc.Format)) { result.reason = Reason::UnsupportedFormat; return result; }
        if (!ToResourceUsage(endpoint.state, usage)) { result.reason = Reason::StateUnknown; return result; }
        const auto& ready = endpoint.ready;
        if (!ready.queue || !ready.fence || !ready.value || ready.value == UINT64_MAX) {
            result.reason = Reason::ProducerUnsubmitted; return result;
        }
        StageDeviceAccess deviceAccess{endpoint};
        // Native Present color ran this exact stage validation in its first
        // AdmitColor above. Other modes retain the original location/order.
        // Later recording boundaries recheck the live private receipt; no
        // admission decision is reused across those boundaries.
        if (nativePresentColor) deviceAccess.devices = immutable.presentDevices;
        DeviceValidation::Snapshot deviceEvidence;
        if (!nativePresentColor && !DeviceValidation::ValidateStage(deviceAccess, deviceEvidence)) {
            ReportDeviceFailure(endpoint, Site::Stage, deviceEvidence, deviceAccess.Live());
            result.reason = Reason::DeviceMismatch; return result;
        }
        const auto& device = deviceAccess.devices[0];
        if (nativePresentColor) {
            immutable.gpuColor = endpoint.color; immutable.gpuQueue = endpoint.queue;
            immutable.gpuColorDevice = device; immutable.gpuQueueDevice = deviceAccess.devices[1];
            immutable.gpuDesc = desc; immutable.gpuCaptured = true;
        }
        // R148: only the already-authenticated Genshin native producer seams
        // use persistent owned effects. Other games/output contracts stay below.
        if (OwnedStage::Handles(endpoint)) {
            persistentAdmission.delegated = true;
            OwnedOperation ownedCall;
            auto admission = result.nativeColor;
            auto staged = OwnedStage::Render(endpoint, device.Get(), Get().coreModule, Get().api,
                OwnedStageSettings, [&](const ColorEndpoint& pinned) {
                    const bool admitted = AdmitColor(pinned, result, immutable);
                    admission = result.nativeColor; // retain the LAST actual predicate, including revocation
                    return admitted;
                });
            staged.nativeColor = admission;
            const auto& persistent = OwnedStage::Get();
            observation.naturalRuntime = persistent.natural;
            observation.naturalLifetime = persistent.lifetime;
            observation.ownedDevice = Key(device.Get());
            observation.ownedQueue = Key(endpoint.queue);
            observation.ownedGeneration = persistent.binding.generation;
            result = std::move(staged); // StageReportScope observes the actual returned transaction
            return result;
        }
        // A previous persistent mode must first restore on its natural owner.
        if (OwnedStage::RestorationPending()) { result.reason = Reason::SelectionPending; return result; }
        State::Natural* natural = nullptr;
        State::Natural* suppress = nullptr;
        NaturalAssociation::SelectionDonor donor;
        if (selectionDonorMode) {
            // Exact GPU facts come only from the color/queue/fence admission
            // above, never from the natural donor or an adapter relationship.
            immutable.gpuColor = endpoint.color; immutable.gpuQueue = endpoint.queue;
            immutable.gpuColorDevice = device; immutable.gpuQueueDevice = deviceAccess.devices[1];
            immutable.gpuDesc = desc; immutable.gpuCaptured = true;
            const auto settings = Get().appliedSettings.load(std::memory_order_acquire);
            natural = ResolveBeforeSrSelectionDonor(settings, donor, observation.donorCount, &observation.donorDiagnostics);
            observation.donorDiagnostics.boundary = 1;
            observation.donor = donor;
            observation.mapping = observation.donorCount > 1 ? NaturalAssociation::Predicate::Ambiguous :
                natural ? NaturalAssociation::Predicate::SelectionDonor : NaturalAssociation::Predicate::SelectionUnavailable;
            if (Get().naturalStatusIncomplete) observation.mapping = NaturalAssociation::Predicate::ObservationIncomplete;
            if (Get().desiredSettings.load(std::memory_order_acquire) != settings)
                observation.mapping = NaturalAssociation::Predicate::SettingsChanged;
            if (!natural) { result.reason = Reason::NaturalUnavailable; return result; }
            observation.donorIdentity = natural->id;
            observation.mappedOwner = Key(device.Get()); // GPU source, not donor device
        } else {
            const auto* sourceProof = endpoint.naturalSource;
            const bool legacyBeforeSr = NaturalAssociation::LegacyBeforeSr(
                endpoint.naturalContract == NaturalSourceContract::BeforeSrExactDevice,
                endpoint.stage == Stage::BeforeSr, !physical, sourceProof != nullptr,
                endpoint.naturalDevice, endpoint.naturalBackbuffer);
            unsigned sourceApi = legacyBeforeSr ? 0xb000 : 0;
            if (legacyBeforeSr) {
                observation.mapping = NaturalAssociation::Predicate::LegacyBeforeSr;
                observation.mappedOwner = endpoint.naturalDevice; // exact old device, no alias
            }
            if (!physical && sourceProof) {
                observation.mapping = sourceProof->Endpoint(endpoint.stream, endpoint.generation, endpoint.frame,
                    endpoint.naturalDevice, endpoint.naturalBackbuffer);
                if (observation.mapping == NaturalAssociation::Predicate::Captured) {
                    observation.mappedOwner = sourceProof->Observation().owner;
                    sourceApi = sourceProof->Observation().api;
                }
            }
            observation.association = AssociationDiagnostics::Inspect(Get().natural, Key,
                observation.mappedOwner, endpoint.naturalBackbuffer, physical, endpoint.physicalSource.runtime,
                endpoint.physicalSource.lifetime, endpoint.physicalSource.serial, sourceApi);
            observation.associationExit = 1;
            if (!physical && observation.mapping == NaturalAssociation::Predicate::Captured)
                observation.mapping = sourceProof->BoundRuntime().predicate;
            if (!physical && ((!legacyBeforeSr && observation.mapping != NaturalAssociation::Predicate::Captured) || Get().naturalStatusIncomplete)) {
                if (Get().naturalStatusIncomplete) observation.mapping = NaturalAssociation::Predicate::ObservationIncomplete;
                result.reason = Reason::NaturalUnavailable; return result;
            }
            for (auto& entry : Get().natural) {
                if (physical) {
                    const auto& source = endpoint.physicalSource;
                    if (entry.live && Key(entry.runtime) == source.runtime && entry.lifetime == source.lifetime &&
                        entry.sourceSerial == source.serial &&
                        entry.sourceSettings == Get().appliedSettings.load(std::memory_order_acquire))
                        natural = &entry;
                    continue;
                }
                if (!entry.runtime || !entry.live) continue;
                if (legacyBeforeSr) {
                    if (entry.id.api != 0xb000 || entry.id.nativeDevice != endpoint.naturalDevice) continue;
                } else if (sourceProof->Runtime(entry.id.api, entry.id.nativeDevice, entry.backbuffers, entry.id.sdkDevice) !=
                    NaturalAssociation::Predicate::Captured) continue;
                if (natural) {
                    observation.mapping = NaturalAssociation::Predicate::Ambiguous;
                    result.reason = Reason::NaturalUnavailable; return result;
                }
                natural = &entry;
            }
            if (!natural) { result.reason = Reason::NaturalUnavailable; return result; }
            if (!physical && !legacyBeforeSr) {
                observation.mapping = sourceProof->Bound(Key(natural->runtime),natural->lifetime,natural->frame.epoch);
                if (observation.mapping != NaturalAssociation::Predicate::Captured) {
                    result.reason = Reason::NaturalUnavailable; return result;
                }
            }
        }
        observation.naturalRuntime = Key(natural->runtime);
        observation.naturalLifetime = natural->lifetime;
        observation.associationExit = 0;
        const bool copiedSource = physical && endpoint.physicalSource.copiedBeforeNatural;
        if (physical && !copiedSource && natural->frame.epoch != endpoint.physicalSource.epoch) {
            result.reason = Reason::NaturalFrameConsumed; return result;
        }
        if (copiedSource) {
            const auto& source = endpoint.physicalSource;
            // The private copy precedes exactly ONE real parent core Present.
            // Reload/destruction/gap/reentry revoke the frozen source snapshot.
            if (natural->id.api != 0xb000 || natural->thread != GetCurrentThreadId() ||
                natural->copiedSerial != source.serial || source.epoch == UINT64_MAX ||
                natural->frame.epoch != source.epoch + 1 || natural->frame.invalid ||
                !source.copyOwner || !source.copyGeneration || !source.copySerial) {
                result.reason = Reason::NaturalFrameConsumed; return result;
            }
        }
        if (!natural->selection.valid || natural->selection.config.empty()) {
            result.reason = Reason::SelectionUnavailable; return result;
        }
        if (!Selection::HasEnabledTechniques(natural->selection)) {
            result.reason = Reason::SelectionUnavailable; return result;
        }
        Get().ownedConfigPath = natural->selection.config;
        if (physical) {
            // A physical DX12 runtime, when present, is a DIFFERENT runtime
            // from the source D3D11 parent. Suppress only its own current frame.
            for (auto& entry : Get().natural) {
                if (!entry.live || entry.id.nativeDevice != Key(device.Get())) continue;
                bool matches = false;
                for (auto buffer : entry.backbuffers) matches |= buffer == Key(endpoint.color);
                if (!matches) continue;
                if (suppress) { observation.associationExit = 2; result.reason = Reason::NaturalUnavailable; return result; }
                suppress = &entry;
            }
            // Independent pictures: the DX11 parent was naturally filtered
            // AFTER its private child copy. Only the output natural runtime
            // can duplicate effects on THIS physical DX12 color.
            if ((!copiedSource && suppress != natural) || (copiedSource && suppress == natural) ||
                (suppress && (suppress->id.api != 0xc000 || suppress->id.nativeQueue != Key(endpoint.queue)))) {
                observation.associationExit = ((!copiedSource && suppress != natural) || (copiedSource && suppress == natural)) ? 3 : 4;
                result.reason = Reason::NaturalUnavailable; return result;
            }
        } else suppress = natural;
        // Natural association above identifies the exact runtime/lifetime.
        // Shared source and suppression runtimes protect the same picture
        // across differing Evaluate/Present endpoint stream identities.
        if (!AdmitSettingsForSource(*natural, endpoint.stream, endpoint.generation) ||
            (suppress && suppress != natural &&
             !AdmitSettingsForSource(*suppress, endpoint.stream, endpoint.generation))) {
            result.reason = Reason::NaturalFrameConsumed; return result;
        }
        if (suppress && (!suppress->frame.CanArm() || suppress->thread != GetCurrentThreadId())) {
            result.reason = Reason::NaturalFrameConsumed; return result;
        }
        Request request{endpoint.stage, Key(device.Get()), Key(endpoint.queue), 1,
            endpoint.stream, endpoint.generation, physical ? endpoint.outputToken.sequence : endpoint.frame, 0xc000};
        Decision decision;
        { std::lock_guard lock(Get().mutex); decision = Get().registry.InspectOwned(request, Get().stage.load()); }
        result.reason = decision.reason;
        if (!decision.executionAllowed) return result;
        auto releaseUntouched = [&] {
            if (result.attempt.CanRetry()) {
                std::lock_guard lock(Get().mutex); Get().registry.ReleaseClaim(request, 0);
            }
        };
        if (ready.queue != endpoint.queue) {
            const auto waitHr = endpoint.queue->Wait(ready.fence, ready.value);
            if (FAILED(waitHr)) {
                deviceEvidence.Fail(Predicate::ProducerWait, waitHr, true);
                ReportDeviceFailure(endpoint, Site::Stage, deviceEvidence, deviceAccess.Live());
                result.reason = Reason::FenceUnavailable; releaseUntouched(); return result;
            }
        }
        reshade::api::command_list* naturalList = nullptr;
        if (!selectionDonorMode) {
            // Existing typed-backbuffer / physical paths retain their exact
            // source association and natural suppression preparation.
            auto* naturalQueue = suppress ? suppress->runtime->get_command_queue() : nullptr;
            naturalList = naturalQueue ? naturalQueue->get_immediate_command_list() : nullptr;
            if (suppress && (!naturalList || naturalList->get_device() != suppress->runtime->get_device())) {
                observation.associationExit = 5;
                result.reason = Reason::NaturalUnavailable; releaseUntouched(); return result;
            }
        }
        // Empty-RTV updates/suppresses the REAL game runtime, not the separate
        // owned runtime. Consume before the virtual call so exceptions cannot
        // authorize a second attempt. No persistent effects-state change.
        OwnedRuntime::Detail detail;
        result.reason = RecordStage(request, decision, endpoint, result, detail, immutable, *natural, suppress, naturalList,
            selectionDonorMode ? &donor : nullptr);
        if (selectionDonorMode && !result.attempt.naturalSuppressed &&
            !BeforeSrSelectionDonorCurrent(*natural, donor)) {
            NaturalAssociation::SelectionDonor current;
            ResolveBeforeSrSelectionDonor(donor.settings, current, observation.donorCount, &observation.donorDiagnostics);
            observation.donorDiagnostics.boundary = 2;
            observation.mapping = observation.donorCount > 1 ? NaturalAssociation::Predicate::Ambiguous :
                Get().desiredSettings.load(std::memory_order_acquire) != donor.settings
                    ? NaturalAssociation::Predicate::SettingsChanged : NaturalAssociation::Predicate::SelectionChanged;
        }
        if (selectionDonorMode && Get().ownedBinding.owner == endpoint.stream &&
            NaturalAssociation::SameSelectionConfiguration(Get().ownedSelectionDonor, donor)) {
            observation.ownedDevice = Key(Get().ownedBinding.device);
            observation.ownedQueue = Key(Get().ownedBinding.queue);
            observation.ownedGeneration = Get().ownedBinding.generation;
        }
        releaseUntouched();
        const auto report = Get().executionReports.Next(Diagnostics::ExecutionStream::OwnedSubmit);
        if (report.emit) LOG_INFO("HoYoShade stage={} source_generation={} source_frame={} producer_value={} natural_epoch={} natural_suppressed={} submission={} techniques={} owned_value={} extent={}x{} state={} result={} output_kind={} output_sequence={} pixels=UNKNOWN",
            Text(endpoint.stage), endpoint.generation, endpoint.frame, ready.value, natural->frame.epoch,
            result.attempt.naturalSuppressed, static_cast<unsigned>(result.attempt.state), result.techniques,
            result.completion.value, endpoint.width, endpoint.height, static_cast<unsigned>(endpoint.state), Text(result.reason),
            static_cast<unsigned>(endpoint.output), endpoint.outputToken.sequence);
    } catch (...) { result.reason = Reason::InternalFailure; }
    return result;
}

PhysicalSource PreparePhysicalSource(IDXGISwapChain* source, bool copiedBeforeNatural) noexcept
{
    if (!Selected(Stage::FinalOutput) || !source || inCallback || ownedOperation || stageActive) return {};
    std::lock_guard lifetime(Get().executionMutex);
    try {
        if (!Selected(Stage::FinalOutput) || !Get().accepted.load(std::memory_order_acquire)) return {};
        Get().latestEpochWait = {};
        // GetBuffer(0) is only identity matching, never a source/current clock.
        Microsoft::WRL::ComPtr<IUnknown> buffer;
        if (FAILED(source->GetBuffer(0, IID_PPV_ARGS(&buffer)))) return {};
        // COM identity is not necessarily the resource interface pointer used
        // by the SDK native resource handle. Query only this live owned buffer;
        // never dereference cached non-owning SDK handle values.
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer12;
        Microsoft::WRL::ComPtr<ID3D11Resource> buffer11;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture11;
        buffer.As(&buffer12); buffer.As(&buffer11); buffer.As(&texture11);
        Microsoft::WRL::ComPtr<ID3D11Device> owner11;
        Microsoft::WRL::ComPtr<ID3D12Device> owner12;
        if (buffer11) buffer11->GetDevice(&owner11);
        if (buffer12 && FAILED(buffer12->GetDevice(IID_PPV_ARGS(&owner12)))) owner12.Reset();
        // Narrow only to obtain an admission epoch; exact buffer by itself
        // never mints a ticket. Reuse the same typed public-event association.
        State::Natural* candidate=nullptr;
        for(auto& entry:Get().natural) {
            if(!entry.live)continue;
            bool matches=false;
            for(auto b:entry.backbuffers)if(b)matches|=
                (entry.id.api==0xb000 && texture11 && b==Key(texture11.Get())) ||
                (entry.id.api==0xc000 && buffer12 && b==Key(buffer12.Get()));
            if(!matches)continue;
            if(candidate)return {};candidate=&entry;
        }
        if(!candidate || !candidate->lifetime || !candidate->frame.epoch)return {};
        // This source's frame is the real natural admission epoch, not a
        // claimed physical/generated output frame. Ticket issuance stays below.
        auto proof=candidate->id.api==0xb000?
            NaturalAssociation::Source::Capture(source,owner11.Get(),texture11.Get(),0,Key(source),candidate->lifetime,candidate->frame.epoch):
            NaturalAssociation::Source::Capture(source,owner12.Get(),buffer12.Get(),0,Key(source),candidate->lifetime,candidate->frame.epoch);
        NaturalAssociation::Source::Scope scope(std::move(proof));
        BindNaturalSource(scope.Get());
        if(!scope.Get())return {};
        State::Natural* natural=nullptr;
        for(auto& entry:Get().natural) {
            if(!entry.live || scope.Get()->Runtime(entry.id.api,entry.id.nativeDevice,entry.backbuffers,entry.id.sdkDevice)!=
                NaturalAssociation::Predicate::Captured || scope.Get()->Bound(Key(entry.runtime),entry.lifetime,entry.frame.epoch)!=
                NaturalAssociation::Predicate::Captured)continue;
            if(natural)return {};natural=&entry;
        }
        if (Get().naturalStatusIncomplete || !natural || natural->thread != GetCurrentThreadId()) return {};
        const auto& admitted = scope.Get()->Observation();
        if (!AdmitSettingsForSource(*natural, admitted.stream, admitted.generation)) return {};
        if (!natural->selection.valid ||
            natural->selection.config.empty() || !natural->frame.CanArm() || natural->sourceSerial == UINT64_MAX ||
            !natural->selection.enabled || !std::any_of(natural->selection.techniques.begin(),
                natural->selection.techniques.end(), [](const auto& t) { return t.enabled; })) return {};
        if (copiedBeforeNatural) {
            if (natural->id.api != 0xb000 || natural->copiedSerial) return {};
            // Refresh current GUI selection under this existing non-callback
            // contract. Reuse only a still-matching immutable exported snapshot.
            // A new effect scope revokes this ticket until the next real owner
            // callback exports it; mutex/TID agreement is not export authority.
            RevokeEnabledObservationDemand();
            const auto selectionRevision = Get().observationSelectionRevision;
            Selection::Capture(natural->runtime, Get().api, natural->selection, false,
                natural->lifetime, Selection::CaptureAccess::ExistingSnapshot);
            CacheCompleteSelection(*natural, selectionRevision);
            if (!natural->selection.valid || !natural->selection.enabled ||
                !std::any_of(natural->selection.techniques.begin(), natural->selection.techniques.end(),
                    [](const auto& t) { return t.enabled; })) return {};
        }
        if(scope.Get()->Endpoint(admitted.stream,admitted.generation,admitted.frame,admitted.sourceDevice,admitted.buffer)!=
                NaturalAssociation::Predicate::Captured ||
            scope.Get()->Bound(Key(natural->runtime),natural->lifetime,natural->frame.epoch)!=NaturalAssociation::Predicate::Captured ||
            !natural->live || !natural->frame.CanArm())return {};
        PhysicalSource ticket{Key(natural->runtime), natural->lifetime, natural->frame.epoch, ++natural->sourceSerial};
        ticket.copiedBeforeNatural = copiedBeforeNatural;
        natural->sourceSettings = Get().appliedSettings.load(std::memory_order_acquire);
        if (copiedBeforeNatural) natural->copiedSerial = ticket.serial;
        RememberSettingsUse(*natural, admitted.stream, admitted.generation);
        return ticket;
    } catch (...) { return {}; }
}

void ReleasePhysicalSource(const PhysicalSource& source) noexcept
{
    if (!source.copiedBeforeNatural) return;
    std::lock_guard lifetime(Get().executionMutex);
    for (auto& entry : Get().natural)
        if (entry.live && Key(entry.runtime) == source.runtime && entry.lifetime == source.lifetime &&
            entry.copiedSerial == source.serial) entry.copiedSerial = 0;
}

void RetirePhysicalOutput(uint64_t stream) noexcept
{
    if (!stream) return;
    std::lock_guard lifetime(Get().executionMutex);
    OwnedOperation operation;
    for (auto& e : Get().directEntries) for (auto& slot : e.slots)
        if (slot.occupied && slot.key.stream == stream) { slot.revoked = true; slot.owner.reset(); }
    RetireCompletedTransactions();
    if (Get().ownedBindingOwner == stream) {
        Get().scratch.reset(); Get().scratchRuntime = nullptr; Get().scratchGeneration = 0;
        Get().ownedBinding = {}; Get().ownedBindingOwner = 0; Get().ownedNatural = nullptr;
    }
    OwnedRuntime::Retire(stream);
    OwnedRuntime::TryCollect();
}

StageResult RenderPresentOriginal(IDXGISwapChain* chain, ID3D12CommandQueue* queue,
                                  bool sdr, uint64_t frame) noexcept
{
    StageResult result;
    if (!Selected(Stage::FinalOutput) || !chain || !queue) return result;
    struct PresentProducer {
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        uint64_t value = 0;
    };
    // Reuse this endpoint's signal; no allocations on steady-state Presents.
    static auto* producer = new PresentProducer;
    std::lock_guard lifetime(Get().executionMutex);
    AssociationDiagnostics::Increment(Get().presentAttempts);
    Microsoft::WRL::ComPtr<ID3D12Device> chainDevice, workDevice, queueDevice, fenceDevice;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> indexed;
    Microsoft::WRL::ComPtr<ID3D12Resource> color;
    ColorEndpoint endpoint;
    endpoint.stage = Stage::FinalOutput; endpoint.queue = queue;
    endpoint.stream = Key(chain); endpoint.generation = 1; endpoint.frame = frame;
    DeviceValidation::Snapshot evidence;
    auto reportFailure = [&] {
        ReportDeviceFailure(endpoint, Site::PresentOriginal, evidence,
            {workDevice.Get(), queueDevice.Get(), queueDevice.Get(), fenceDevice.Get(), chainDevice.Get()}, Key(workDevice.Get()));
    };
    const auto deviceHr = chain->GetDevice(IID_PPV_ARGS(&chainDevice));
    if (!evidence.Device(Role::Chain, deviceHr, Key(chainDevice.Get()))) { reportFailure(); return result; }
    // Preserve the public scheduling device as observation. A typed chain /
    // backbuffer owner proof resolves natural identity separately from GPU work.
    endpoint.naturalDevice = Key(chainDevice.Get());
    const auto indexedHr = chain->QueryInterface(IID_PPV_ARGS(&indexed));
    if (!evidence.Api(Predicate::PresentIndexed, indexedHr, indexed != nullptr)) { reportFailure(); return result; }
    const auto bufferHr = chain->GetBuffer(indexed->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&color));
    if (!evidence.Api(Predicate::PresentBuffer, bufferHr, color != nullptr)) { reportFailure(); return result; }
    endpoint.color = color.Get(); endpoint.naturalBackbuffer = Key(color.Get());
    const auto sourceProof = NaturalAssociation::Source::Capture(chain, chainDevice.Get(), color.Get(),
        indexed->GetCurrentBackBufferIndex(), endpoint.stream, endpoint.generation, frame);
    NaturalAssociation::Source::Scope naturalSource(sourceProof);
    endpoint.naturalSource = naturalSource.Get();
    BindNaturalSource(endpoint.naturalSource);
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) { result.reason = Reason::QueueMismatch; return result; }
    const auto colorHr = color->GetDevice(IID_PPV_ARGS(&workDevice));
    if (!evidence.Device(Role::Color, colorHr, Key(workDevice.Get()))) {
        reportFailure(); result.reason = Reason::DeviceMismatch; return result;
    }
    const auto queueHr = queue->GetDevice(IID_PPV_ARGS(&queueDevice));
    if (!evidence.Device(Role::ConsumerQueue, queueHr, Key(queueDevice.Get())) ||
        !evidence.Check(workDevice.Get() == queueDevice.Get(), Predicate::ConsumerDeviceEqual)) {
        reportFailure(); result.reason = Reason::DeviceMismatch; return result;
    }
    evidence.removedKnown = true; evidence.removedHr = workDevice->GetDeviceRemovedReason();
    if (!evidence.Api(Predicate::DeviceHealthy, evidence.removedHr)) {
        reportFailure(); result.reason = Reason::DeviceMismatch; return result;
    }
    if (!producer->fence || producer->device != workDevice || producer->queue.Get() != queue) {
        // The old signal itself owns no color; stage batches hold all readers.
        producer->device = workDevice; producer->queue = queue; producer->value = 0;
        producer->fence.Reset();
        const auto fenceHr = workDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&producer->fence));
        if (!evidence.Api(Predicate::PresentCreateFence, fenceHr, producer->fence != nullptr)) {
            reportFailure(); producer->fence.Reset(); result.reason = Reason::FenceUnavailable; return result;
        }
    }
    endpoint.ready = {queue, producer->fence.Get(), producer->value};
    // Validate both new and reused fences before Signal. Public GetDevice must
    // return the exact work interface; COM identity sampling is not a bypass.
    const auto fenceHr = producer->fence->GetDevice(IID_PPV_ARGS(&fenceDevice));
    if (!evidence.Device(Role::Fence, fenceHr, Key(fenceDevice.Get())) ||
        !evidence.Check(workDevice.Get() == fenceDevice.Get(), Predicate::FenceDeviceEqual)) {
        reportFailure(); producer->fence.Reset(); result.reason = Reason::DeviceMismatch; return result;
    }
    evidence.completedKnown = true; evidence.completed = producer->fence->GetCompletedValue();
    if (!evidence.Check(evidence.completed != UINT64_MAX, Predicate::FenceCompletedValid)) {
        reportFailure(); producer->fence.Reset(); result.reason = Reason::FenceUnavailable; return result;
    }
    if (!evidence.Check(producer->value < UINT64_MAX - 1, Predicate::PresentFenceValue)) {
        reportFailure(); result.reason = Reason::FenceUnavailable; return result;
    }
    const auto signalHr = queue->Signal(producer->fence.Get(), ++producer->value);
    endpoint.ready.value = producer->value;
    if (!evidence.Api(Predicate::PresentSignal, signalHr)) {
        reportFailure(); result.reason = Reason::FenceUnavailable; return result;
    }
    ColorWriteScope writer(true, workDevice.Get(), queue, color.Get(), chain);
    if (!writer.Acquired()) { result.reason = Reason::NoRuntime; return result; }
    const auto desc = color->GetDesc();
    endpoint.width = static_cast<uint32_t>(desc.Width); endpoint.height = desc.Height;
    endpoint.state = D3D12_RESOURCE_STATE_PRESENT; endpoint.stateKnown = true; endpoint.sdr = sdr;
    result = RenderStage(endpoint);
    if (result.Uncertain()) QuarantineColor(chain, queue, workDevice.Get(), color.Get(), frame);
    writer.FinishAfterReturn(result.Uncertain());
    return result;
}

void NotifyNativeSubmit(ID3D12CommandQueue* queue, UINT count,
                        ID3D12CommandList* const* lists) noexcept
{
    try {
        if (!queue || !lists || !count || !Get().accepted.load(std::memory_order_acquire)) return;
        // Disable/transition cannot strand existing leases or GPU readers.
        std::lock_guard lifetime(Get().executionMutex);
        OwnedOperation collection;
        // OwnedRuntime::Lease::Submit already flushes the SDK immediate list
        // and signals the same native queue. This hook is collection-only;
        // signalling again here would create a second completion authority.
        RetireCompletedTransactions();
        const auto collected = OwnedRuntime::TryCollect();
        const auto report = Get().executionReports.Next(Diagnostics::ExecutionStream::NativeSubmit);
        if (report.emit) LOG_INFO("HoYoShade stage native submit observed report_count={} lists={} owned_collection_destroyed={} pending={} quarantined={} gpu_submission=owned_submit_only",
            report.count, count, collected.destroyed, collected.pending, collected.quarantined);
    } catch (...) { ++Get().overflow; }
}
}
