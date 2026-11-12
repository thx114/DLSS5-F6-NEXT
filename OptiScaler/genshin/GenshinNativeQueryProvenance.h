// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinCpuStageTiming.h"
#include <runtime/F6InputPolicy.h>
// R533: optional, bounded x64 Windows native provenance. No game/depth facts.
// References and the supported trust boundary are in REPORT.md. Unsupported
// loader rewriting, prologues, foreign edits or capacity exhaustion refuse.
#include <Windows.h>
#include <wintrust.h>
#include <softpub.h>
#include <mscat.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <d3d11_4.h>
#include <detours/detours.h>
#include <array>
#include <atomic>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cstddef>
#include <cstdio>
#include <climits>
#include <type_traits>
#include <utility>
#include <source_location>
#include <memory>
#include <mutex>
#pragma comment(lib,"wintrust.lib")
#pragma comment(lib,"crypt32.lib")
#pragma comment(lib,"bcrypt.lib")

namespace GenshinQueryScope { class FactoryBirthIssuer; class CreationWitness; }
namespace HoYoShade {
struct ExistingFactoryOwner;
using ExistingFactoryOwnerRef=std::shared_ptr<const ExistingFactoryOwner>;
ExistingFactoryOwnerRef AcquireExistingFactoryOwner(const void*,const char*)noexcept;
using FactoryOwnerFreeze=bool(*)(void*,const void*,const void*,size_t,const MEMORY_BASIC_INFORMATION&);
bool ExistingFactoryOwnerIntact(const ExistingFactoryOwnerRef&,const void*,const char*,FactoryOwnerFreeze,void*)noexcept;
}
namespace GenshinNativeQuery {
namespace detail {
inline bool Span(size_t at,size_t n,size_t limit)noexcept {return at<=limit && n<=limit-at;}
template<class T> inline T Read(const unsigned char* p)noexcept {T v{};std::memcpy(&v,p,sizeof(v));return v;}
inline bool Memory(const void* p,void* out,size_t n)noexcept {
    SIZE_T got=0;return p && ReadProcessMemory(GetCurrentProcess(),p,out,n,&got) && got==n;
}
inline bool Equal(const void* p,const void* expected,size_t n)noexcept {
    std::array<unsigned char,4096> bytes{};
    const auto* address=static_cast<const unsigned char*>(p);
    const auto* source=static_cast<const unsigned char*>(expected);
    while(n){const auto take=(std::min)(n,bytes.size());
        if(!Memory(address,bytes.data(),take) || std::memcmp(bytes.data(),source,take))return false;
        address+=take;source+=take;n-=take;
    }return true;
}
// R193: startup-only observed overloads. Existing arities above remain byte-for-
// byte unchanged; ordinary reads/comparisons construct no receipt or observer.
// No failed read/comparison is repeated for diagnostics.
template<class Observation> inline bool Memory(const void* p,void* out,size_t n,Observation& o)noexcept {
    o.BeforeLeaf("memory-pointer-nonnull");
    if(!o.Leaf("memory-pointer-nonnull",p!=nullptr))return false;
    SIZE_T got=0;const BOOL result=ReadProcessMemory(GetCurrentProcess(),p,out,n,&got);
    o.requestedBytes=n;o.readBytes=got;o.apiValue=static_cast<uint64_t>(result);o.apiValid=true;
    if(!result){o.Win32("ReadProcessMemory",result,GetLastError());return false;}
    return o.Leaf("memory-complete-read",got==n);
}
template<class Observation> inline bool Equal(const void* p,const void* expected,size_t n,Observation& o)noexcept {
    std::array<unsigned char,4096> bytes{};
    const auto* address=static_cast<const unsigned char*>(p);
    const auto* source=static_cast<const unsigned char*>(expected);size_t offset=0;
    while(n){const auto take=(std::min)(n,bytes.size());o.offset=offset;
        if(!Memory(address,bytes.data(),take,o))return false;
        if(!o.Leaf("equal-bytes",std::memcmp(bytes.data(),source,take)==0))return false;
        address+=take;source+=take;n-=take;offset+=take;
    }return true;
}
struct Handle {
    HANDLE value=nullptr;
    ~Handle(){if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
// State data is closed for both success and failure. A trusted chain alone is
// insufficient: this bounded policy requires the Microsoft organization too.
struct TrustObservation {
    bool attempted=false,microsoft=false;LONG status=0;DWORD win32=0;
};
struct FileTrustObservation {
    TrustObservation embedded{},catalog{};
    unsigned catalogAttempts=0;const char* catalogStage="not-started";DWORD catalogWin32=0;
};
inline bool MicrosoftTrust(WINTRUST_DATA& data,TrustObservation* observation=nullptr)noexcept {
    GUID action=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    data.cbStruct=sizeof(data);data.dwUIChoice=WTD_UI_NONE;
    data.fdwRevocationChecks=WTD_REVOKE_NONE;
    data.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL;
    data.dwStateAction=WTD_STATEACTION_VERIFY;
    const auto result=WinVerifyTrust(reinterpret_cast<HWND>(static_cast<INT_PTR>(-1)),&action,&data);
    const auto verifyError=GetLastError();
    bool microsoft=false;
    if(result==ERROR_SUCCESS){
        auto* provider=WTHelperProvDataFromStateData(data.hWVTStateData);
        auto* signer=provider?WTHelperGetProvSignerFromChain(provider,0,FALSE,0):nullptr;
        if(signer && signer->csCertChain && signer->pasCertChain[0].pCert){
            wchar_t organization[128]{};
            const auto count=CertGetNameStringW(signer->pasCertChain[0].pCert,CERT_NAME_ATTR_TYPE,0,
                const_cast<char*>(szOID_ORGANIZATION_NAME),organization,128);
            microsoft=count>1 && std::wcscmp(organization,L"Microsoft Corporation")==0;
        }
    }
    if(observation)*observation={true,microsoft,result,verifyError};
    data.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(reinterpret_cast<HWND>(static_cast<INT_PTR>(-1)),&action,&data);
    return result==ERROR_SUCCESS && microsoft;
}
inline bool TrustedFile(HANDLE file,const wchar_t* path,FileTrustObservation* observation=nullptr)noexcept {
    if(observation)*observation={};
    WINTRUST_FILE_INFO info{};info.cbStruct=sizeof(info);info.pcwszFilePath=path;info.hFile=file;
    WINTRUST_DATA data{};data.dwUnionChoice=WTD_CHOICE_FILE;data.pFile=&info;
    if(MicrosoftTrust(data,observation?&observation->embedded:nullptr))return true;
    HCATADMIN admin=nullptr;
    const auto stage=[&](const char* name){if(observation){observation->catalogStage=name;observation->catalogWin32=GetLastError();}};
    if(!CryptCATAdminAcquireContext2(&admin,nullptr,BCRYPT_SHA256_ALGORITHM,nullptr,0)){stage("acquire-context");return false;}
    DWORD count=0;bool good=false;
    if(CryptCATAdminCalcHashFromFileHandle2(admin,file,&count,nullptr,0) && count && count<=64){
        unsigned char hash[64]{};
        if(CryptCATAdminCalcHashFromFileHandle2(admin,file,&count,hash,0)){
            wchar_t tag[129]{};constexpr wchar_t digits[]=L"0123456789ABCDEF";
            for(DWORD i=0;i<count;++i){tag[2*i]=digits[hash[i]>>4];tag[2*i+1]=digits[hash[i]&15];}
            HCATINFO catalog=nullptr;
            for(unsigned attempt=0;attempt<16 && !good;++attempt){
                catalog=CryptCATAdminEnumCatalogFromHash(admin,hash,count,0,&catalog);
                if(!catalog){stage("enumerate-catalog");break;}
                CATALOG_INFO catalogInfo{};catalogInfo.cbStruct=sizeof(catalogInfo);
                if(!CryptCATCatalogInfoFromContext(catalog,&catalogInfo,0)){stage("catalog-info");continue;}
                WINTRUST_CATALOG_INFO ci{};ci.cbStruct=sizeof(ci);
                ci.pcwszCatalogFilePath=catalogInfo.wszCatalogFile;ci.pcwszMemberTag=tag;
                ci.pcwszMemberFilePath=path;ci.hMemberFile=file;ci.pbCalculatedFileHash=hash;
                ci.cbCalculatedFileHash=count;ci.hCatAdmin=admin;
                WINTRUST_DATA cd{};cd.dwUnionChoice=WTD_CHOICE_CATALOG;cd.pCatalog=&ci;
                if(observation)++observation->catalogAttempts;
                good=MicrosoftTrust(cd,observation?&observation->catalog:nullptr);
                stage("verify-catalog");
            }
            if(catalog)CryptCATAdminReleaseCatalogContext(admin,catalog,0);
        }else stage("hash-bytes");
    }else stage("hash-size");
    CryptCATAdminReleaseContext(admin,0);return good;
}
struct Section {uint32_t rva=0,size=0,flags=0;};
// Phase40: decoded CLEAN import metadata only. No live bytes or admission result.
struct CleanSlotPlan {
    static constexpr size_t capacity=8192;
    struct Slot {DWORD dest=0,dll=0,name=0;bool delay=false;};
    std::vector<Slot> slots;std::vector<unsigned> regularNames;
};
struct CleanSlotRevision {
    const void* ledger=nullptr;const void* image=nullptr;const unsigned char* bytes=nullptr;
    size_t size=0;uint64_t opened=0,generation=0;
    std::array<uint64_t,13> shape{};
    bool operator==(const CleanSlotRevision&)const=default;
};
// R835: immutable signed export-name metadata, never live bytes/targets/Intact.
// Large or unsupported maps keep the complete original parser and live reads.
struct CleanExportPlan {
    static constexpr size_t capacity=8192,readCapacity=4096;
    struct Name {DWORD rva=0,index=0,size=0;};
    struct Run {DWORD rva=0,size=0,first=0,count=0;};
    IMAGE_EXPORT_DIRECTORY directory{};
    std::vector<Name> names;std::vector<Run> runs;
};
class ScpSource;
struct Image {
    // Published images are never copied/moved/replaced. Open invalidates before
    // its first write; only its complete authenticated success freezes clean.
private:
    uint64_t cleanRevision_=0;bool cleanFrozen_=false;
public:
    uint64_t CleanRevision()const noexcept {return cleanFrozen_?cleanRevision_:0;}
    mutable std::atomic_flag cleanPlanBusy=ATOMIC_FLAG_INIT;
    mutable CleanSlotRevision cleanPlanRevision{};
    mutable CleanSlotPlan cleanPlan;
    mutable std::atomic_flag exportPlanBusy=ATOMIC_FLAG_INIT;
    mutable CleanSlotRevision exportPlanRevision{};
    mutable CleanExportPlan exportPlan;
    HMODULE live=nullptr;Handle file,mapping;const unsigned char* raw=nullptr;
    size_t fileSize=0;std::vector<unsigned char> clean;
    std::array<Section,32> sections{};unsigned sectionCount=0;
    IMAGE_DATA_DIRECTORY imports{},delays{},exports{};
    // R27: one exact auxiliary process-entry detour, never a general foreign-code allowlist.
    uint32_t auxiliaryHookRva=0;uintptr_t auxiliaryHookTarget=0;std::array<unsigned char,5> auxiliaryHookBytes{};
    ~Image(){if(raw)UnmapViewOfFile(raw);}
    bool Range(uint32_t rva,size_t n)const noexcept {return Span(rva,n,clean.size());}
    bool SectionHas(uint32_t rva,size_t n,DWORD yes,DWORD no=0)const noexcept {
        for(unsigned i=0;i<sectionCount;++i){const auto& s=sections[i];
            if(rva>=s.rva && Span(rva-s.rva,n,s.size) && (s.flags&yes)==yes && !(s.flags&no))return true;
        }return false;
    }
    const char* String(uint32_t rva)const noexcept {
        if(!Range(rva,1))return nullptr;
        return std::memchr(clean.data()+rva,0,(std::min)(size_t(512),clean.size()-rva))?
            reinterpret_cast<const char*>(clean.data()+rva):nullptr;
    }
    // R611: an observation of this actual Open only; never a trust receipt,
    // issuer or alternate acceptance path. Offsets are image RVAs except
    // raw/file/header predicates, whose offsets are file positions.
    struct OpenObservation {
        const char* predicate="not-started";
        uint64_t offset=0,actual=0,expected=0;
        DWORD win32=0;
    } openObservation{};
    FileTrustObservation trustObservation{};
    bool OpenReject(const char* predicate,uint64_t offset=0,uint64_t actual=0,
                    uint64_t expected=0,DWORD win32=0)noexcept {
        openObservation={predicate,offset,actual,expected,win32};return false;
    }
    bool MatchLiveHeaders(size_t n)noexcept {
        // Same full byte equality and 4096-byte reads as Equal. Report a read
        // failure separately from the first differing byte; mask no fields.
        std::array<unsigned char,4096> bytes{};size_t offset=0;
        const auto* address=reinterpret_cast<const unsigned char*>(live);
        while(n){const auto take=(std::min)(n,bytes.size());
            SIZE_T got=0;
            if(!ReadProcessMemory(GetCurrentProcess(),address+offset,bytes.data(),take,&got))
                return OpenReject("live-header-read",offset,got,take,GetLastError());
            if(got!=take)return OpenReject("live-header-short-read",offset,got,take);
            if(std::memcmp(bytes.data(),clean.data()+offset,take)){
                for(size_t i=0;i<take;++i)if(bytes[i]!=clean[offset+i])
                    return OpenReject("live-header-byte-equality",offset+i,bytes[i],clean[offset+i]);
            }
            offset+=take;n-=take;
        }
        openObservation={"open-ok"};return true;
    }
    bool NormalizeLoaderImageBase(uint32_t nt,const IMAGE_OPTIONAL_HEADER64& opt,
                                  const std::vector<uint32_t>& relocations)noexcept {
        const size_t at=size_t(nt)+offsetof(IMAGE_NT_HEADERS64,OptionalHeader)+
            offsetof(IMAGE_OPTIONAL_HEADER64,ImageBase);
        if(!Span(at,sizeof(uint64_t),opt.SizeOfHeaders) || !Range(static_cast<uint32_t>(at),sizeof(uint64_t)))
            return OpenReject("loader-image-base-header-range",at);
        // This bounded header transform cannot override a signed relocation.
        for(const auto target:relocations)
            if(uint64_t(target)<at+sizeof(uint64_t) && at<uint64_t(target)+sizeof(uint64_t))
                return OpenReject("loader-image-base-relocation-overlap",at,target);
        uint64_t observed=0;SIZE_T got=0;
        if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const unsigned char*>(live)+at,
                              &observed,sizeof(observed),&got))
            return OpenReject("loader-image-base-read",at,got,sizeof(observed),GetLastError());
        if(got!=sizeof(observed))return OpenReject("loader-image-base-short-read",at,got,sizeof(observed));
        // R619 observed a loaded system header naming its actual pinned base.
        // Accept only the signed preferred base or this independently known
        // live module base. Never adopt an arbitrary observed header value.
        const auto actual=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(live));
        if(observed!=opt.ImageBase && observed!=actual)
            return OpenReject("loader-image-base-value",at,observed,actual);
        const uint64_t expected=observed==opt.ImageBase?opt.ImageBase:actual;
        std::memcpy(clean.data()+at,&expected,sizeof(expected));
        return true; // MatchLiveHeaders still compares EVERY header byte.
    }
    // Signed DVRT locations and reachable alternatives. A derived reference is
    // sealed once at Open; later checks require those exact five bytes again.
    // SCP additionally requires its signed source and complete extension page.
    struct DynamicFixup {
        uint32_t rva=0,original=0;
        std::vector<uint32_t> alternatives;
    };
    std::vector<DynamicFixup> dynamicFixups;
private:
    std::shared_ptr<ScpSource> scpSource;
public:
    const std::shared_ptr<ScpSource>& Scp()const noexcept {return scpSource;}
    bool NormalizeDynamicFixups();
    bool MatchScpSource()noexcept;
    HMODULE ScpDependency()const noexcept;
    size_t ScpReferenceBytes()const noexcept;
    bool RawBacked(uint32_t rva,size_t n,uint32_t nt)const noexcept {
        const size_t table=size_t(nt)+sizeof(IMAGE_NT_HEADERS64);
        if(!Span(table,size_t(sectionCount)*sizeof(IMAGE_SECTION_HEADER),fileSize))return false;
        for(unsigned i=0;i<sectionCount;++i){
            const auto s=Read<IMAGE_SECTION_HEADER>(raw+table+i*sizeof(IMAGE_SECTION_HEADER));
            if(rva>=s.VirtualAddress && Span(rva-s.VirtualAddress,n,s.SizeOfRawData) &&
               Span(rva-s.VirtualAddress,n,s.Misc.VirtualSize))return true;
        }return false;
    }
    bool MatchDynamicFixups()noexcept {
        if(!MatchScpSource())return false;
        for(const auto& f:dynamicFixups){
            if(!f.rva || !Range(f.rva-1,5))return OpenReject("dynamic-target-range",f.rva);
            std::array<unsigned char,5> bytes{};
            if(!Memory(reinterpret_cast<const unsigned char*>(live)+f.rva-1,bytes.data(),bytes.size()))
                return OpenReject("dynamic-target-read",f.rva,0,bytes.size(),GetLastError());
            // No recurring renormalization: changes to a previously selected
            // signed alternative or kernel-derived target revoke identity.
            for(size_t at=0;at<bytes.size();++at)if(bytes[at]!=clean[f.rva-1+at])
                return OpenReject("dynamic-target-byte-equality",f.rva-1+at,
                                  bytes[at],clean[f.rva-1+at]);
        }return true;
    }
    bool ValidateDynamicRelocations(uint32_t nt,const IMAGE_OPTIONAL_HEADER64& opt,
                                    const std::vector<uint32_t>& relocations) {
        static_assert(sizeof(IMAGE_DYNAMIC_RELOCATION_TABLE)==8 && sizeof(IMAGE_DYNAMIC_RELOCATION64)==12);
        static_assert(sizeof(IMAGE_FUNCTION_OVERRIDE_HEADER)==4 && sizeof(IMAGE_FUNCTION_OVERRIDE_DYNAMIC_RELOCATION)==16);
        static_assert(sizeof(IMAGE_BDD_INFO)==8 && sizeof(IMAGE_BDD_DYNAMIC_RELOCATION)==8);
        dynamicFixups.clear();
        const auto config=opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
        if(!config.Size)return true;
        if(!Range(config.VirtualAddress,config.Size))
            return OpenReject("load-config-range",config.VirtualAddress,config.Size,clean.size());
        constexpr size_t vaAt=offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64,DynamicValueRelocTable);
        constexpr size_t offAt=offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64,DynamicValueRelocTableOffset);
        constexpr size_t secAt=offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64,DynamicValueRelocTableSection);
        if((config.Size>vaAt && config.Size<vaAt+8) ||
           (config.Size>offAt && config.Size<offAt+4) ||
           (config.Size>secAt && config.Size<secAt+2))return OpenReject("dynamic-partial-locator",config.VirtualAddress);
        const auto* lc=clean.data()+config.VirtualAddress;
        if(config.Size>=vaAt+8 && Read<uint64_t>(lc+vaAt))
            return OpenReject("load-config-dynamic-va",config.VirtualAddress+vaAt,Read<uint64_t>(lc+vaAt));
        const auto overlaps=[](uint32_t a,size_t an,uint32_t b,size_t bn)noexcept {
            return an && bn && uint64_t(a)<uint64_t(b)+bn && uint64_t(b)<uint64_t(a)+an;
        };
        for(const auto at:relocations){
            for(const auto field:std::array<std::pair<size_t,size_t>,4>{{{0,4},{vaAt,8},{offAt,4},{secAt,2}}})
                if(Span(field.first,field.second,config.Size) &&
                   overlaps(at,8,config.VirtualAddress+static_cast<uint32_t>(field.first),field.second))
                    return OpenReject("dynamic-locator-ordinary-relocation",at);
        }
        const auto offset=config.Size>=offAt+4?Read<DWORD>(lc+offAt):0;
        const auto section=config.Size>=secAt+2?Read<WORD>(lc+secAt):WORD{0};
        if(!offset && !section)return true;
        if(!offset || !section || config.Size<secAt+2 || section>sectionCount)
            return OpenReject("dynamic-section-offset-pair",config.VirtualAddress+offAt,offset,section);
        if(config.Size<sizeof(DWORD) || Read<DWORD>(lc)!=config.Size ||
           !RawBacked(config.VirtualAddress,config.Size,nt))return OpenReject("dynamic-load-config-size",config.VirtualAddress);
        const size_t table=size_t(nt)+sizeof(IMAGE_NT_HEADERS64);
        if(!Span(table,size_t(sectionCount)*sizeof(IMAGE_SECTION_HEADER),fileSize))return OpenReject("dynamic-section-table-range",table);
        const auto s=Read<IMAGE_SECTION_HEADER>(raw+table+size_t(section-1)*sizeof(IMAGE_SECTION_HEADER));
        if(!(s.Characteristics&IMAGE_SCN_MEM_READ) ||
           (s.Characteristics&(IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_EXECUTE)) || (offset&3) ||
           !Span(offset,sizeof(IMAGE_DYNAMIC_RELOCATION_TABLE),s.SizeOfRawData) ||
           !Span(offset,sizeof(IMAGE_DYNAMIC_RELOCATION_TABLE),s.Misc.VirtualSize) ||
           uint64_t(s.VirtualAddress)+offset>UINT32_MAX)
            return OpenReject("dynamic-table-section-range",offset,section);
        const uint32_t rva=s.VirtualAddress+offset;
        if(!Range(rva,sizeof(IMAGE_DYNAMIC_RELOCATION_TABLE)))return OpenReject("dynamic-table-header-range",rva);
        const auto h=Read<IMAGE_DYNAMIC_RELOCATION_TABLE>(clean.data()+rva);
        if(h.Version!=1 || h.Size<sizeof(IMAGE_DYNAMIC_RELOCATION64) || h.Size>65536)
            return OpenReject("dynamic-table-version-size",rva,h.Version,h.Size);
        const size_t total=sizeof(h)+size_t(h.Size);
        if(!Span(offset,total,s.SizeOfRawData) || !Span(offset,total,s.Misc.VirtualSize) ||
           !Range(rva,total) || !RawBacked(rva,total,nt))return OpenReject("dynamic-table-payload-range",rva,total);
        if(overlaps(rva,total,0,opt.SizeOfHeaders) || overlaps(rva,total,config.VirtualAddress,config.Size))
            return OpenReject("dynamic-metadata-overlap",rva);
        // Metadata must be original signed bytes, never ASLR-adjusted metadata.
        for(const auto at:relocations)
            if(overlaps(at,8,rva,total))
                return OpenReject("dynamic-metadata-ordinary-relocation",at);
        const size_t record=size_t(rva)+sizeof(h);
        const auto e=Read<IMAGE_DYNAMIC_RELOCATION64>(clean.data()+record);
        if(e.Symbol!=IMAGE_DYNAMIC_RELOCATION_FUNCTION_OVERRIDE ||
           e.BaseRelocSize!=h.Size-sizeof(e) || e.BaseRelocSize<sizeof(IMAGE_FUNCTION_OVERRIDE_HEADER))
            return OpenReject("dynamic-symbol-or-record-size",record,e.Symbol,e.BaseRelocSize);
        const size_t payload=record+sizeof(e),end=payload+e.BaseRelocSize;
        const auto fh=Read<IMAGE_FUNCTION_OVERRIDE_HEADER>(clean.data()+payload);
        const size_t functions=payload+sizeof(fh);
        if(!Span(functions,fh.FuncOverrideSize,end) || fh.FuncOverrideSize<sizeof(IMAGE_FUNCTION_OVERRIDE_DYNAMIC_RELOCATION))
            return OpenReject("dynamic-function-region",functions,fh.FuncOverrideSize);
        const size_t functionsEnd=functions+fh.FuncOverrideSize,bddBase=functionsEnd;
        struct BddExtent {size_t begin,end;};
        std::vector<BddExtent> bddExtents;
        std::vector<DynamicFixup> pendingFixups;
        size_t functionAt=functions;unsigned functionCount=0;
        while(functionAt<functionsEnd){
            if(++functionCount>256 || !Span(functionAt,sizeof(IMAGE_FUNCTION_OVERRIDE_DYNAMIC_RELOCATION),functionsEnd))
                return OpenReject("dynamic-function-header-range",functionAt);
            const auto f=Read<IMAGE_FUNCTION_OVERRIDE_DYNAMIC_RELOCATION>(clean.data()+functionAt);
            if((f.BDDOffset&3) || !f.RvaSize || (f.RvaSize&3) || f.RvaSize/4>64 ||
               !f.BaseRelocSize || (f.BaseRelocSize&3) ||
               !Span(functionAt,uint64_t(sizeof(f))+f.RvaSize+f.BaseRelocSize,functionsEnd))
                return OpenReject("dynamic-function-shape",functionAt,f.RvaSize,f.BaseRelocSize);
            const auto codeRva=[&](uint32_t at){return at && RawBacked(at,1,nt) &&
                SectionHas(at,1,IMAGE_SCN_MEM_EXECUTE,IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_DISCARDABLE);};
            if(!codeRva(f.OriginalRva))return OpenReject("dynamic-original-function",f.OriginalRva);
            const size_t rvas=functionAt+sizeof(f);
            for(size_t i=0;i<f.RvaSize/4;++i){const auto at=Read<DWORD>(clean.data()+rvas+4*i);
                if(!codeRva(at))return OpenReject("dynamic-override-function",at);
                for(size_t j=0;j<i;++j)if(Read<DWORD>(clean.data()+rvas+4*j)==at)
                    return OpenReject("dynamic-duplicate-override",at);
            }
            // Each descriptor's BDD offset is relative to the BDD region.
            // Only reachable terminal alternatives enter the bounded set;
            // feature numbers are never emulated or used as trust receipts.
            if(!Span(bddBase,f.BDDOffset,end))return OpenReject("dynamic-bdd-offset-range",bddBase,f.BDDOffset);
            const size_t bdd=bddBase+f.BDDOffset;
            if(!Span(bdd,sizeof(IMAGE_BDD_INFO),end))return OpenReject("dynamic-bdd-header-range",bdd);
            const auto bh=Read<IMAGE_BDD_INFO>(clean.data()+bdd);
            if(bh.Version!=1 || !bh.BDDSize || (bh.BDDSize%sizeof(IMAGE_BDD_DYNAMIC_RELOCATION)) ||
               bh.BDDSize/sizeof(IMAGE_BDD_DYNAMIC_RELOCATION)>4096 ||
               !Span(bdd,sizeof(bh)+uint64_t(bh.BDDSize),end))
                return OpenReject("dynamic-bdd-version-size",bdd,bh.Version,bh.BDDSize);
            const size_t nodes=bdd+sizeof(bh),count=bh.BDDSize/sizeof(IMAGE_BDD_DYNAMIC_RELOCATION);
            std::vector<unsigned char> reached(count,0);reached[0]=1;
            std::vector<uint32_t> alternatives;
            // Supported serialized shape: forward decision edges, equal-edge
            // terminals (zero sentinel or self index); no unreachable records.
            // Feature IDs/terminal decisions remain opaque (never evaluated).
            for(size_t i=0;i<count;++i){const auto node=Read<IMAGE_BDD_DYNAMIC_RELOCATION>(clean.data()+nodes+i*sizeof(IMAGE_BDD_DYNAMIC_RELOCATION));
                if(!reached[i] || node.Left>=count || node.Right>=count)return OpenReject("dynamic-bdd-index",nodes+i*8);
                if(node.Left==node.Right){
                    if((node.Left && node.Left!=i) || node.Value>=f.RvaSize/4)
                        return OpenReject("dynamic-bdd-terminal",nodes+i*8,node.Value,f.RvaSize/4);
                    const auto target=Read<DWORD>(clean.data()+rvas+4*node.Value);
                    if(std::find(alternatives.begin(),alternatives.end(),target)==alternatives.end())
                        alternatives.push_back(target);
                }else {
                    if(node.Left<=i || node.Right<=i || node.Left==node.Right)return OpenReject("dynamic-bdd-edge",nodes+i*8);
                    reached[node.Left]=reached[node.Right]=1;
                }
            }
            bddExtents.push_back({bdd,bdd+sizeof(bh)+bh.BDDSize});
            size_t cursor=rvas+f.RvaSize,limit=cursor+f.BaseRelocSize;
            while(cursor<limit){
                if(!Span(cursor,sizeof(IMAGE_BASE_RELOCATION),limit))return OpenReject("dynamic-fixup-block-header",cursor);
                const auto block=Read<IMAGE_BASE_RELOCATION>(clean.data()+cursor);
                if((block.VirtualAddress&4095) || block.SizeOfBlock<sizeof(block)+4 ||
                   (block.SizeOfBlock&3) || !Span(cursor,block.SizeOfBlock,limit))
                    return OpenReject("dynamic-fixup-block-range",cursor);
                bool padding=false;
                for(size_t at=cursor+sizeof(block);at<cursor+block.SizeOfBlock;at+=2){
                    const auto entry=Read<WORD>(clean.data()+at);if(!entry){padding=true;continue;}
                    const auto type=entry>>12;const uint64_t dest=uint64_t(block.VirtualAddress)+(entry&4095);
                    if(padding || type!=IMAGE_FUNCTION_OVERRIDE_X64_REL32 || !dest || dest>UINT32_MAX ||
                       pendingFixups.size()==1024)return OpenReject("dynamic-fixup-type-capacity",at,type);
                    const auto target=static_cast<uint32_t>(dest);
                    if(target<=opt.SizeOfHeaders || !RawBacked(target-1,5,nt) ||
                       !SectionHas(target-1,5,IMAGE_SCN_MEM_EXECUTE,IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_DISCARDABLE) ||
                       overlaps(target-1,5,rva,total) || overlaps(target-1,5,config.VirtualAddress,config.Size))
                        return OpenReject("dynamic-fixup-target-range",target);
                    for(unsigned i=0;i<IMAGE_NUMBEROF_DIRECTORY_ENTRIES;++i){
                        if(i==IMAGE_DIRECTORY_ENTRY_SECURITY)continue; // file offset, not RVA
                        const auto d=opt.DataDirectory[i];
                        if(overlaps(target-1,5,d.VirtualAddress,d.Size))return OpenReject("dynamic-fixup-directory-overlap",target,i);
                    }
                    for(const auto ordinary:relocations)if(overlaps(target-1,5,ordinary,8))
                        return OpenReject("dynamic-fixup-ordinary-overlap",target,ordinary);
                    const auto opcode=clean[target-1];
                    const int64_t originalTarget=int64_t(target)+4+Read<int32_t>(clean.data()+target);
                    if((opcode!=0xe8 && opcode!=0xe9) || originalTarget!=f.OriginalRva)
                        return OpenReject("dynamic-fixup-opcode-destination",target);
                    for(const auto& old:pendingFixups)if(overlaps(target-1,5,old.rva-1,5))
                        return OpenReject("dynamic-fixup-overlap",target,old.rva);
                    pendingFixups.push_back({target,f.OriginalRva,alternatives});
                }cursor+=block.SizeOfBlock;
            }
            functionAt=rvas+f.RvaSize+f.BaseRelocSize;
        }
        // Fully consume both regions. Shared BDDs are allowed, but gaps,
        // partial overlaps, unreferenced tables and trailing bytes are not.
        std::sort(bddExtents.begin(),bddExtents.end(),[](const auto& a,const auto& b){
            return a.begin<b.begin || (a.begin==b.begin && a.end<b.end);});
        size_t bddEnd=bddBase;BddExtent previous{};
        for(const auto extent:bddExtents){
            if(extent.begin==previous.begin && extent.end==previous.end)continue;
            if(extent.begin!=bddEnd)return OpenReject("dynamic-bdd-coverage",extent.begin,bddEnd);
            bddEnd=extent.end;previous=extent;
        }
        if(bddEnd!=end)return OpenReject("dynamic-bdd-tail",bddEnd,end);
        if(pendingFixups.empty())return OpenReject("dynamic-fixup-empty",rva);
        dynamicFixups=std::move(pendingFixups);
        return true; // Parser alone grants no image/query authority.
    }
private:
    friend class ScpSource;
    // Reference-only preparation is inaccessible to callers and never freezes
    // CleanRevision. It breaks the signed ntdll/SCP bootstrap dependency.
    bool PrepareReference(HMODULE module) {
        cleanFrozen_=false;scpSource.reset();dynamicFixups.clear();
        cleanPlanRevision={};cleanPlan.slots.clear();cleanPlan.regularNames.clear();
        exportPlanRevision={};exportPlan.names.clear();exportPlan.runs.clear();
        // Exhaustion disables the memo rather than recycling a revision.
        if(cleanRevision_!=UINT64_MAX)++cleanRevision_;
        openObservation={"in-progress"};trustObservation={};
        wchar_t system[MAX_PATH]{},loaded[MAX_PATH]{},expected[MAX_PATH]{},finalPath[MAX_PATH+4]{};
        const auto sys=GetSystemDirectoryW(system,MAX_PATH);
        const auto sysError=sys?ERROR_SUCCESS:GetLastError();
        const auto len=GetModuleFileNameW(module,loaded,MAX_PATH);
        const auto pathError=len?ERROR_SUCCESS:GetLastError();
        if(!sys)return OpenReject("system-directory-query",0,0,0,sysError);
        if(sys>=MAX_PATH)return OpenReject("system-directory-length",0,sys,MAX_PATH);
        if(!len)return OpenReject("module-path-query",0,0,0,pathError);
        if(len>=MAX_PATH)return OpenReject("module-path-length",0,len,MAX_PATH);
        const auto* name=std::wcsrchr(loaded,L'\\');if(!name)return OpenReject("module-basename-separator");
        if(!name[1])return OpenReject("module-basename-empty");
        if(swprintf_s(expected,L"%ls\\%ls",system,name+1)<0)return OpenReject("system-path-format");
        if(_wcsicmp(expected,loaded))return OpenReject("system-path-equality");
        file.value=CreateFileW(expected,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(file.value==INVALID_HANDLE_VALUE)return OpenReject("file-open",0,0,0,GetLastError());
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if(!GetFileInformationByHandleEx(file.value,FileAttributeTagInfo,&attributes,sizeof(attributes)))
            return OpenReject("file-attributes-query",0,0,0,GetLastError());
        if(attributes.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)return OpenReject("file-reparse-point");
        const auto finalCount=GetFinalPathNameByHandleW(file.value,finalPath,MAX_PATH+4,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
        if(finalCount<5)return OpenReject("final-path-query-or-short",0,finalCount,5);
        if(finalCount>=MAX_PATH+4)return OpenReject("final-path-length",0,finalCount,MAX_PATH+4);
        if(std::wcsncmp(finalPath,L"\\\\?\\",4))return OpenReject("final-path-prefix");
        if(_wcsicmp(finalPath+4,expected))return OpenReject("same-handle-path-equality");
        LARGE_INTEGER size{};
        if(!GetFileSizeEx(file.value,&size))return OpenReject("file-size-query",0,0,0,GetLastError());
        if(size.QuadPart<4096)return OpenReject("file-size-minimum",0,size.QuadPart,4096);
        if(size.QuadPart>32*1024*1024)return OpenReject("file-size-maximum",0,size.QuadPart,32*1024*1024);
        fileSize=static_cast<size_t>(size.QuadPart);
        if(!TrustedFile(file.value,expected,&trustObservation))return OpenReject("same-handle-microsoft-trust");
        // Raw data mapping only: no SEC_IMAGE, loader, DllMain, executable view.
        mapping.value=CreateFileMappingW(file.value,nullptr,PAGE_READONLY,0,0,nullptr);
        if(!mapping.value)return OpenReject("raw-mapping-create",0,0,0,GetLastError());
        raw=static_cast<const unsigned char*>(MapViewOfFile(mapping.value,FILE_MAP_READ,0,0,0));
        if(!raw)return OpenReject("raw-mapping-view",0,0,0,GetLastError());
        if(Read<WORD>(raw)!=IMAGE_DOS_SIGNATURE)return OpenReject("dos-signature",0,Read<WORD>(raw),IMAGE_DOS_SIGNATURE);
        const auto nt=Read<DWORD>(raw+offsetof(IMAGE_DOS_HEADER,e_lfanew));
        if(!Span(nt,sizeof(IMAGE_NT_HEADERS64),fileSize))return OpenReject("nt-header-range",nt,sizeof(IMAGE_NT_HEADERS64),fileSize);
        const auto header=Read<IMAGE_NT_HEADERS64>(raw+nt);
        const auto& opt=header.OptionalHeader;
        if(header.Signature!=IMAGE_NT_SIGNATURE)return OpenReject("nt-signature",nt);
        if(header.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64)return OpenReject("pe-machine",nt);
        if(header.FileHeader.SizeOfOptionalHeader!=sizeof(IMAGE_OPTIONAL_HEADER64))return OpenReject("optional-header-size",nt);
        if(opt.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC)return OpenReject("optional-header-magic",nt);
        if(opt.NumberOfRvaAndSizes!=IMAGE_NUMBEROF_DIRECTORY_ENTRIES)return OpenReject("directory-count",nt);
        if(!opt.SizeOfImage)return OpenReject("image-size-zero",nt);
        if(opt.SizeOfImage>32*1024*1024)return OpenReject("image-size-maximum",nt);
        if(opt.SizeOfHeaders>opt.SizeOfImage)return OpenReject("headers-image-range",nt);
        if(!Span(0,opt.SizeOfHeaders,fileSize))return OpenReject("headers-file-range",nt);
        if(!opt.SectionAlignment)return OpenReject("section-alignment-zero",nt);
        if((opt.SectionAlignment&(opt.SectionAlignment-1)))return OpenReject("section-alignment-power-two",nt);
        if(!opt.FileAlignment)return OpenReject("file-alignment-zero",nt);
        if((opt.FileAlignment&(opt.FileAlignment-1)))return OpenReject("file-alignment-power-two",nt);
        if(!header.FileHeader.NumberOfSections)return OpenReject("section-count-zero",nt);
        if(header.FileHeader.NumberOfSections>sections.size())return OpenReject("section-count-capacity",nt);
        const size_t table=nt+sizeof(header);sectionCount=header.FileHeader.NumberOfSections;
        if(!Span(table,sectionCount*sizeof(IMAGE_SECTION_HEADER),opt.SizeOfHeaders))return OpenReject("section-table-range",table,sectionCount*sizeof(IMAGE_SECTION_HEADER),opt.SizeOfHeaders);
        clean.resize(opt.SizeOfImage);std::memcpy(clean.data(),raw,opt.SizeOfHeaders);
        for(unsigned i=0;i<sectionCount;++i){
            const auto s=Read<IMAGE_SECTION_HEADER>(raw+table+i*sizeof(IMAGE_SECTION_HEADER));
            const auto extent=(std::max)(s.Misc.VirtualSize,s.SizeOfRawData);
            if(!extent)return OpenReject("section-extent-zero",table+i*sizeof(IMAGE_SECTION_HEADER));
            if(s.VirtualAddress<opt.SizeOfHeaders)return OpenReject("section-overlaps-headers",s.VirtualAddress);
            if(!Range(s.VirtualAddress,extent))return OpenReject("section-image-range",s.VirtualAddress,extent,clean.size());
            if(!Span(s.PointerToRawData,s.SizeOfRawData,fileSize))return OpenReject("section-file-range",s.PointerToRawData,s.SizeOfRawData,fileSize);
            if(s.SizeOfRawData && s.PointerToRawData<opt.SizeOfHeaders)return OpenReject("section-raw-overlaps-headers",s.PointerToRawData);
            if((s.Characteristics&IMAGE_SCN_MEM_EXECUTE) && (s.Characteristics&IMAGE_SCN_MEM_WRITE))return OpenReject("section-write-execute",s.VirtualAddress,s.Characteristics);
            for(unsigned j=0;j<i;++j){const auto& p=sections[j];
                if(s.VirtualAddress<uint64_t(p.rva)+p.size && p.rva<uint64_t(s.VirtualAddress)+extent)return OpenReject("section-virtual-overlap",s.VirtualAddress,p.rva,p.size);
                const auto old=Read<IMAGE_SECTION_HEADER>(raw+table+j*sizeof(IMAGE_SECTION_HEADER));
                if(s.SizeOfRawData && old.SizeOfRawData && s.PointerToRawData<uint64_t(old.PointerToRawData)+old.SizeOfRawData &&
                   old.PointerToRawData<uint64_t(s.PointerToRawData)+s.SizeOfRawData)return OpenReject("section-raw-overlap",s.PointerToRawData,old.PointerToRawData,old.SizeOfRawData);
            }
            sections[i]={s.VirtualAddress,extent,s.Characteristics};
            std::memcpy(clean.data()+s.VirtualAddress,raw+s.PointerToRawData,s.SizeOfRawData);
        }
        // Pin the loaded image lifetime. A reference view never executes code.
        HMODULE held=nullptr;
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(module),&held))return OpenReject("live-module-pin",0,0,0,GetLastError());
        if(held!=module)return OpenReject("live-module-pin-identity",0,reinterpret_cast<uintptr_t>(held),reinterpret_cast<uintptr_t>(module));
        live=module;
        const auto relocation=opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        const uint64_t delta=reinterpret_cast<uintptr_t>(live)-opt.ImageBase;
        if(!relocation.Size && delta)return OpenReject("relocation-required",relocation.VirtualAddress,delta);
        std::vector<uint32_t> relocations;
        if(relocation.Size){
            if(!Range(relocation.VirtualAddress,relocation.Size))return OpenReject("relocation-directory-range",relocation.VirtualAddress,relocation.Size,clean.size());
            size_t cursor=relocation.VirtualAddress,end=cursor+relocation.Size;
            while(cursor<end){
                if(!Span(cursor,sizeof(IMAGE_BASE_RELOCATION),end))return OpenReject("relocation-block-header-range",cursor,sizeof(IMAGE_BASE_RELOCATION),end);
                const auto block=Read<IMAGE_BASE_RELOCATION>(clean.data()+cursor);
                if(block.SizeOfBlock<sizeof(block))return OpenReject("relocation-block-minimum",cursor,block.SizeOfBlock,sizeof(block));
                if(block.SizeOfBlock&1)return OpenReject("relocation-block-even-size",cursor,block.SizeOfBlock);
                if(!Span(cursor,block.SizeOfBlock,end))return OpenReject("relocation-block-range",cursor,block.SizeOfBlock,end);
                if(block.VirtualAddress&4095)return OpenReject("relocation-block-page-alignment",cursor,block.VirtualAddress);
                for(size_t at=cursor+sizeof(block);at<cursor+block.SizeOfBlock;at+=2){
                    const auto entry=Read<WORD>(clean.data()+at);const auto type=entry>>12;
                    if(type==IMAGE_REL_BASED_ABSOLUTE)continue;
                    const uint64_t target=uint64_t(block.VirtualAddress)+(entry&4095);
                    if(type!=IMAGE_REL_BASED_DIR64)return OpenReject("relocation-type",at,type,IMAGE_REL_BASED_DIR64);
                    if(target>UINT32_MAX)return OpenReject("relocation-target-u32",at,target,UINT32_MAX);
                    if(!Range(static_cast<uint32_t>(target),8))return OpenReject("relocation-target-range",at,target,clean.size());
                    relocations.push_back(static_cast<uint32_t>(target));
                }cursor+=block.SizeOfBlock;
            }
        }
        std::sort(relocations.begin(),relocations.end());
        if(!ValidateDynamicRelocations(nt,opt,relocations))return false;
        for(size_t i=0;i<relocations.size();++i){const auto at=relocations[i];
            if(i && at<relocations[i-1]+8)return OpenReject("relocation-target-overlap",at,relocations[i-1],8);
            const auto value=Read<uint64_t>(clean.data()+at)+delta;std::memcpy(clean.data()+at,&value,8);
        }
        imports=opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];delays=opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
        exports=opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        // Delay binding changes executable paths. This bounded version refuses
        // unresolved/dynamic delay dispatch rather than masking mutable slots.
        return NormalizeLoaderImageBase(nt,opt,relocations) && MatchLiveHeaders(opt.SizeOfHeaders);
    }
public:
    bool Open(HMODULE module) {
        const bool good=PrepareReference(module) && NormalizeDynamicFixups() && MatchDynamicFixups();
        cleanFrozen_=good && cleanRevision_!=UINT64_MAX;return good;
    }
};
#include "GenshinScpProvenance.inl"
// Detours 4.0.1 x64 layout, checked bytewise rather than dereferencing an opaque
// struct. It is only accepted when the library actually produces this shape.
struct NativeHop {
    unsigned char* entry=nullptr;void* target=nullptr;const void* iat=nullptr;
    unsigned size=0;std::array<unsigned char,8> code{};
};
// Input-only COM methods can be owned without becoming native query roots.
// This receipt grants no signed-image, import, callable or birth authority.
struct IndependentInputMethod {
    HMODULE module=nullptr;const void* tableSlot=nullptr; // Installation snapshot only; never dereferenced later.
    void* selected=nullptr;unsigned index=UINT_MAX;
};
// A frozen predecessor is not a native-code image or historical installer receipt.
// Only the factory installer can supply this before any write in its batch.
struct FactoryPredecessor {
    struct Mapping {
        uintptr_t allocation=0,region=0;size_t size=0;
        DWORD state=0,type=0,protect=0;
    };
    struct Node {
        uintptr_t address=0;Mapping mapping{};std::array<unsigned char,16> bytes{};
    };
    std::array<Node,3> nodes{};uintptr_t slot=0,terminal=0;Mapping slotMapping{};
    HoYoShade::ExistingFactoryOwnerRef owner;
    const char* symbol=nullptr;HMODULE module=nullptr;
    DWORD thread=0;uint64_t generation=0;bool captured=false;
};
inline bool FactoryMappingIntact(const FactoryPredecessor::Mapping& expected,uintptr_t address)noexcept {
    MEMORY_BASIC_INFORMATION m{};
    return VirtualQuery(reinterpret_cast<const void*>(address),&m,sizeof(m))==sizeof(m) &&
        expected.allocation==reinterpret_cast<uintptr_t>(m.AllocationBase) &&
        expected.region==reinterpret_cast<uintptr_t>(m.BaseAddress) && expected.size==m.RegionSize &&
        expected.state==m.State && expected.type==m.Type && expected.protect==m.Protect;
}
// Only Own::Batch can populate/activate this transaction-local page lease.
// Frozen mappings remain immutable; the lease describes their exact partition.
class FactoryProtectionLease final {
    friend class FactoryLeaseAccess;
    struct Page {uintptr_t address=0;FactoryPredecessor::Mapping mapping{};DWORD original=0;bool held=false;};
    std::array<Page,6> pages_{};unsigned count_=0;size_t pageSize_=0;
    const void* batch_=nullptr;DWORD thread_=0;uint64_t generation_=0;
public:
    FactoryProtectionLease()noexcept=default;
    FactoryProtectionLease(const FactoryProtectionLease&)=delete;
    FactoryProtectionLease& operator=(const FactoryProtectionLease&)=delete;
    bool Bound()const noexcept;
    bool MappingIntact(const FactoryPredecessor::Mapping& expected,uintptr_t address,size_t size)const noexcept {
        if(!Bound() || !size || address>UINTPTR_MAX-size || expected.region>UINTPTR_MAX-expected.size ||
           address<expected.region || address+size>expected.region+expected.size)return false;
        auto first=expected.region,last=expected.region+expected.size;bool writable=false;
        const auto affects=[&](const Page& p){
            // VirtualQuery snapshots may begin at different query pages of the
            // same RX run. Its frozen end and all attributes must still agree.
            return p.held && p.mapping.allocation==expected.allocation &&
                p.mapping.state==expected.state && p.mapping.type==expected.type &&
                p.mapping.protect==expected.protect && p.mapping.region<=UINTPTR_MAX-p.mapping.size &&
                p.mapping.region+p.mapping.size==expected.region+expected.size &&
                p.address>=expected.region && p.address<expected.region+expected.size;
        };
        for(unsigned i=0;i<count_;++i){const auto& p=pages_[i];if(!affects(p))continue;
            if(address>=p.address && address<p.address+pageSize_)writable=true;
        }
        // Adjacent held pages coalesce. Unleased fragments keep original RX.
        for(unsigned i=0;i<count_;++i){const auto& p=pages_[i];if(!affects(p))continue;
            if(writable){
                if(p.address<=address && p.address>=first)first=p.address;
                if(p.address+pageSize_>address && p.address+pageSize_<=last)last=p.address+pageSize_;
            }else {if(p.address+pageSize_<=address)first=(std::max)(first,p.address+pageSize_);
                if(p.address>address)last=(std::min)(last,p.address);}
        }
        if(writable){bool changed=true;while(changed){changed=false;
            for(unsigned i=0;i<count_;++i){const auto& p=pages_[i];if(!affects(p))continue;
                if(p.address+pageSize_==first){first=p.address;changed=true;}
                if(p.address==last){last+=pageSize_;changed=true;}
            }
        }}
        MEMORY_BASIC_INFORMATION m{};
        return address+size<=last && VirtualQuery(reinterpret_cast<void*>(address),&m,sizeof(m))==sizeof(m) &&
            reinterpret_cast<uintptr_t>(m.AllocationBase)==expected.allocation &&
            reinterpret_cast<uintptr_t>(m.BaseAddress)==first && m.RegionSize==last-first &&
            m.State==expected.state && m.Type==expected.type &&
            m.Protect==(writable?PAGE_EXECUTE_READWRITE:expected.protect);
    }
};
inline bool FactoryPredecessorIntact(const FactoryPredecessor& r,unsigned ownSize=0,
                                    const FactoryProtectionLease* lease=nullptr)noexcept {
    if(!r.captured || !r.owner || ownSize>r.nodes[0].bytes.size() ||
       r.nodes[0].bytes[0]!=0xe9 || r.nodes[1].bytes[0]!=0xff || r.nodes[1].bytes[1]!=0x25 ||
       Read<int32_t>(r.nodes[1].bytes.data()+2)!=0 || r.slot!=r.nodes[1].address+6 ||
       r.nodes[2].address!=r.terminal || Read<uintptr_t>(r.nodes[1].bytes.data()+6)!=r.terminal ||
       !HoYoShade::ExistingFactoryOwnerIntact(r.owner,reinterpret_cast<void*>(r.terminal),r.symbol,nullptr,nullptr))return false;
    for(unsigned i=0;i<r.nodes.size();++i){const auto& n=r.nodes[i];const auto skip=i==0?ownSize:0;
        if(!(lease?lease->MappingIntact(n.mapping,n.address,n.bytes.size()):FactoryMappingIntact(n.mapping,n.address)) ||
           !Equal(reinterpret_cast<void*>(n.address+skip),n.bytes.data()+skip,n.bytes.size()-skip))return false;
    }
    uintptr_t target=0;
    return (lease?lease->MappingIntact(r.slotMapping,r.slot,sizeof(target)):FactoryMappingIntact(r.slotMapping,r.slot)) &&
        Memory(reinterpret_cast<void*>(r.slot),&target,sizeof(target)) && target==r.terminal;
}
struct Patch {
    void** original=nullptr;unsigned char* entry=nullptr;unsigned char* trampoline=nullptr;void* hook=nullptr;
    // R560: transaction storage and every published executing alias are persistent.
    void** executed=nullptr;void** mirror=nullptr;
    HMODULE declaredModule=nullptr;const char* exportName=nullptr;
    // Diagnostic source of enrollment, never a code/trust capability.
    std::source_location enrollment{};
    void* requested=nullptr;std::array<NativeHop,4> hops{};unsigned hopCount=0;
    bool ownSetup=false;IndependentInputMethod input{};
    unsigned image=UINT_MAX;uint32_t rva=0;unsigned size=0;bool straight=false;
    std::array<unsigned char,30> edit{};
    // Pre-commit snapshot plus the six exactly constructed rbCodeIn bytes.
    std::array<unsigned char,96> code{};
    std::array<unsigned char,32> ownEntry{};
    // Immutable installation preimage, independent of Detours' rbRestore.
    // It agrees with rbRestore AND clean bytes or a clean-start composition.
    // Absent only in non-enrolled detach/CPU model receipts; never synthesized.
    std::array<unsigned char,24> installPreimage{};
    bool installPreimageCaptured=false;uint64_t installGeneration=0;
    FactoryPredecessor predecessor;
    // First signed Index proof freezes the clean start separately from the
    // predecessor install bytes. Bind/rebuild must prove both exact operands.
    std::array<unsigned char,16> factoryCleanStart{};bool factoryCleanCaptured=false;
};
// R041: keep the producer and exact collection edge with the ORIGINAL refusal.
// Addresses/roles are observations only. No resolver, gate or patch verifier
// consumes this context; a private-core hook never becomes system code here.
struct ImageCollectionOrigin {
    bool patch=false,ownSetup=false,exportNameTruncated=false;
    const char* role="image-root-or-import";std::array<char,128> exportName{'n','o','n','e'};
    std::source_location enrollment{},collection{},request{};
    unsigned hopIndex=UINT_MAX,depth=0;
    HMODULE parent=nullptr,current=nullptr,declaredModule=nullptr;
    const void* address=nullptr;const void* requested=nullptr;const void* entry=nullptr;
    const void* hook=nullptr;const void* trampoline=nullptr;
    const void* original=nullptr;const void* executed=nullptr;const void* mirror=nullptr;
};
inline thread_local ImageCollectionOrigin imageCollectionOrigin;
struct ImageRequestScope final {
    ImageCollectionOrigin previous=imageCollectionOrigin;
    ImageRequestScope(HMODULE module,const std::source_location& caller)noexcept {
        imageCollectionOrigin.parent=previous.current;imageCollectionOrigin.current=module;
        imageCollectionOrigin.request=caller;imageCollectionOrigin.depth=previous.depth+1;
    }
    ~ImageRequestScope(){imageCollectionOrigin=previous;}
    ImageRequestScope(const ImageRequestScope&)=delete;
};
struct PatchCollectionScope final {
    ImageCollectionOrigin previous=imageCollectionOrigin;
    PatchCollectionScope(const Patch& p,const std::source_location& caller)noexcept {
        auto& o=imageCollectionOrigin;o={};o.patch=true;o.ownSetup=p.ownSetup;
        o.enrollment=p.enrollment;o.collection=caller;o.declaredModule=p.declaredModule;
        const auto count=std::snprintf(o.exportName.data(),o.exportName.size(),"%s",p.exportName?p.exportName:"none");
        o.exportNameTruncated=count<0 || static_cast<size_t>(count)>=o.exportName.size();
        o.requested=p.requested;o.entry=p.entry;o.hook=p.hook;o.trampoline=p.trampoline;
        o.original=p.original;o.executed=p.executed;o.mirror=p.mirror;
    }
    ~PatchCollectionScope(){imageCollectionOrigin=previous;}
    PatchCollectionScope(const PatchCollectionScope&)=delete;
    void Role(const char* role,const void* address,unsigned hop=UINT_MAX)noexcept {
        imageCollectionOrigin.role=role;imageCollectionOrigin.address=address;imageCollectionOrigin.hopIndex=hop;
    }
};
// Deliberately small instruction language: pushes, register/stack MOV/LEA,
// register arithmetic and stack immediate arithmetic. No branch, return,
// call, RIP-relative operand, SIB base-less address or transformed relocation.
inline unsigned Instruction(const unsigned char* b,size_t n)noexcept {
    size_t at=0;if(!n)return 0;
    if(b[at]>=0x40 && b[at]<=0x4f){if(++at==n)return 0;}
    const auto op=b[at++];
    if(op>=0x50 && op<=0x57)return static_cast<unsigned>(at);
    if(op==0x90)return static_cast<unsigned>(at);
    if(op!=0x89 && op!=0x8b && op!=0x8d && op!=0x33 && op!=0x31 && op!=0x85 && op!=0x83 && op!=0x81)return 0;
    if(at==n)return 0;const auto modrm=b[at++];const auto mod=modrm>>6,rm=modrm&7;
    if(mod==0 && rm==5)return 0;
    if(mod!=3 && rm==4){if(at==n)return 0;const auto sib=b[at++];if(mod==0 && (sib&7)==5)return 0;}
    if(mod==1)++at;if(mod==2)at+=4;
    if(op==0x83)++at;if(op==0x81)at+=4;
    return at<=n?static_cast<unsigned>(at):0;
}
inline bool Straight(const unsigned char* bytes,unsigned n)noexcept {
    unsigned at=0;while(at<n){const auto size=Instruction(bytes+at,n-at);if(!size)return false;at+=size;}
    return at==n;
}
// R716: our hook entry is executed in place, not copied into the native
// trampoline. A compiler-generated RIP-relative MOV from our own data is safe
// here but must NEVER enter Instruction/Straight's native-original language.
// R776: LEA uses the identical own-image data-range proof; it reads no payload.
// Actual R774 hkGetMessagePos begins SUB RSP,0x28 then this seven-byte LEA.
// The mutable data value is not an import, callable target or birth receipt.
inline bool OwnImageRange(const void* address,size_t n,DWORD yes,DWORD no)noexcept {
    if(!address || !n)return false;
    MEMORY_BASIC_INFORMATION self{},first{};
    if(!VirtualQuery(reinterpret_cast<const void*>(&Instruction),&self,sizeof(self)) ||
       !VirtualQuery(address,&first,sizeof(first)) || self.Type!=MEM_IMAGE ||
       first.Type!=MEM_IMAGE || self.AllocationBase!=first.AllocationBase)return false;
    const auto base=reinterpret_cast<uintptr_t>(self.AllocationBase);
    const auto at=reinterpret_cast<uintptr_t>(address);
    if(at<base || at-base>UINT32_MAX)return false;
    IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
    if(!Memory(self.AllocationBase,&dos,sizeof(dos)) || dos.e_magic!=IMAGE_DOS_SIGNATURE ||
       dos.e_lfanew<static_cast<LONG>(sizeof(dos)) || dos.e_lfanew>1024*1024 ||
       !Memory(reinterpret_cast<const void*>(base+dos.e_lfanew),&nt,sizeof(nt)) ||
       nt.Signature!=IMAGE_NT_SIGNATURE || nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 ||
       nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
       nt.FileHeader.SizeOfOptionalHeader!=sizeof(IMAGE_OPTIONAL_HEADER64) ||
       !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections>96 ||
       !Span(at-base,n,nt.OptionalHeader.SizeOfImage))return false;
    const size_t table=size_t(dos.e_lfanew)+sizeof(nt);
    if(!Span(table,size_t(nt.FileHeader.NumberOfSections)*sizeof(IMAGE_SECTION_HEADER),
             nt.OptionalHeader.SizeOfHeaders))return false;
    bool section=false;
    for(unsigned i=0;i<nt.FileHeader.NumberOfSections;++i){IMAGE_SECTION_HEADER s{};
        if(!Memory(reinterpret_cast<const void*>(base+table+i*sizeof(s)),&s,sizeof(s)))return false;
        if(at-base>=s.VirtualAddress && Span(at-base-s.VirtualAddress,n,s.Misc.VirtualSize) &&
           (s.Characteristics&yes)==yes && !(s.Characteristics&no))section=true;
    }
    if(!section)return false;
    // Every covered page must still belong to this image, with the declared
    // code/data protection. No foreign allocation or executable data operand.
    for(size_t done=0;done<n;){MEMORY_BASIC_INFORMATION page{};
        const auto current=at+done;
        if(!VirtualQuery(reinterpret_cast<const void*>(current),&page,sizeof(page)) ||
           page.AllocationBase!=self.AllocationBase || page.Type!=MEM_IMAGE || page.State!=MEM_COMMIT ||
           (page.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
        const DWORD protection=page.Protect&0xff;
        if(yes&IMAGE_SCN_MEM_EXECUTE){if(protection!=PAGE_EXECUTE_READ)return false;}
        else if(protection!=PAGE_READONLY && protection!=PAGE_READWRITE && protection!=PAGE_WRITECOPY)return false;
        const auto region=reinterpret_cast<uintptr_t>(page.BaseAddress);
        if(current<region || current-region>=page.RegionSize)return false;
        done+=(std::min)(n-done,page.RegionSize-size_t(current-region));
    }
    return true;
}
// R175: a finite decoder miss is distinct from a failed own-image/operand proof.
enum class OwnEntryStatus { Supported, UnsupportedEncoding, IntegrityFailure };
struct OwnEntryDecode {OwnEntryStatus status=OwnEntryStatus::IntegrityFailure;unsigned length=0;};
inline OwnEntryDecode OwnEntryInstruction(const unsigned char* bytes,size_t n,const void* live)noexcept {
    if(!bytes || !live || !n)return {};
    if(const auto length=Instruction(bytes,n))return {OwnEntryStatus::Supported,length};
    // R784: compiler MOV r64,imm64 in our in-place hook entry. The immediate
    // is only a register constant, never a memory/import/callable authority.
    // Native-original Instruction/Straight remains unchanged.
    if(n>=2 && (bytes[0]==0x48 || bytes[0]==0x49) && bytes[1]>=0xb8 && bytes[1]<=0xbf){
        if(n<10 || !OwnImageRange(live,10,IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE,
                                  IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_DISCARDABLE))return {};
        return {OwnEntryStatus::Supported,10};
    }

    // Only MOV/LEA r64,[RIP+disp32] into our own non-executable data. No store,
    // indirect call/jump, operand-
    // size/address-size prefix, baseless SIB or other RIP-relative operation.
    if(n<7)return {};
    if((bytes[0]!=0x48 && bytes[0]!=0x4c) || (bytes[1]!=0x8b && bytes[1]!=0x8d) ||
       (bytes[2]&0xc7)!=0x05)return {OwnEntryStatus::UnsupportedEncoding};
    const auto at=reinterpret_cast<uintptr_t>(live);
    if(at>UINTPTR_MAX-7)return {};
    const auto next=at+7;
    const int64_t displacement=Read<int32_t>(bytes+3);
    if(displacement<0 && next<static_cast<uint64_t>(-displacement))return {};
    if(displacement>=0 && next>UINTPTR_MAX-static_cast<uint64_t>(displacement))return {};
    const auto operand=displacement<0?next-static_cast<uintptr_t>(-displacement):
        next+static_cast<uintptr_t>(displacement);
    if(!OwnImageRange(live,7,IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE,
                      IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_DISCARDABLE) ||
       !OwnImageRange(reinterpret_cast<const void*>(operand),8,IMAGE_SCN_MEM_READ,
                      IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_DISCARDABLE))return {};
    return {OwnEntryStatus::Supported,7};
}
inline OwnEntryStatus OwnEntryPrefix(const unsigned char* bytes,size_t n,const void* live)noexcept {
    // Every decode has at least 28 bytes: a short/truncated input is not a soft miss.
    if(!bytes || !live || n<32)return OwnEntryStatus::IntegrityFailure;
    unsigned prefix=0;
    while(prefix<5){const auto decoded=OwnEntryInstruction(bytes+prefix,n-prefix,
                                                          static_cast<const unsigned char*>(live)+prefix);
        if(decoded.status!=OwnEntryStatus::Supported)return decoded.status;
        prefix+=decoded.length;
    }
    return OwnEntryStatus::Supported;
}
// R580: binding categories are NOT executable-call capabilities. A data
// receipt authenticates only an address/owner, never mutable payload/semantics.
enum class ImportKind : unsigned char { None, CodeBinding, DataBinding, DelayThunk };
struct CanonicalBinding {
    HMODULE owner=nullptr;uint32_t rva=0;ImportKind kind=ImportKind::None;
};
struct Import {
    const void* slot=nullptr;uintptr_t target=0;
    // Only canonical, signed native delay thunks can transition once during
    // the actual first factory call. After its setup seal all slots are exact.
    const char* dll=nullptr;const char* name=nullptr;WORD ordinal=0;bool lazy=false;
    CanonicalBinding binding{};HMODULE importer=nullptr;
};
enum class ImageCollection : unsigned char { Absent, Building, Complete, Refused };
enum class RepresentationState : unsigned char { Absent, Building, Validated, Sealed, Refused };
// R753: roles identify exact signed slots, not a symbol-wide exception.
enum class ShimRole : unsigned char { Outer, Self, SelfAlias, BaseNt, BaseAlias };
enum class ShimForm : unsigned char {
    OuterRepresented, SelfCanonical, SelfRepresented,
    SelfAliasCanonical, SelfAliasRepresented,
    BaseNtCanonical, BaseNtRepresented, BaseAliasCanonical, BaseAliasRepresented,
    BaseNtNative, BaseAliasNative
};
struct RoleForm { ShimRole role=ShimRole::Outer;ShimForm form=ShimForm::OuterRepresented; };
constexpr bool RepresentedForm(ShimRole role,ShimForm form)noexcept {
    switch(role){
    case ShimRole::Outer:return form==ShimForm::OuterRepresented;
    case ShimRole::Self:return form==ShimForm::SelfCanonical || form==ShimForm::SelfRepresented;
    case ShimRole::SelfAlias:return form==ShimForm::SelfAliasCanonical || form==ShimForm::SelfAliasRepresented;
    case ShimRole::BaseNt:return form==ShimForm::BaseNtCanonical || form==ShimForm::BaseNtRepresented;
    case ShimRole::BaseAlias:return form==ShimForm::BaseAliasCanonical || form==ShimForm::BaseAliasRepresented;
    }return false;
}
constexpr bool RoleFormValid(ShimRole role,ShimForm form,uintptr_t iat,uintptr_t resolver,
                             uintptr_t canonical,uintptr_t wrapper)noexcept {
    if(!canonical || !wrapper || canonical==wrapper)return false;
    if(role==ShimRole::BaseNt && form==ShimForm::BaseNtNative)
        return iat==canonical && resolver==canonical;
    if(role==ShimRole::BaseAlias && form==ShimForm::BaseAliasNative)
        return iat==canonical && resolver==canonical;
    if(!RepresentedForm(role,form) || resolver!=wrapper)return false;
    if(form==ShimForm::SelfCanonical || form==ShimForm::SelfAliasCanonical ||
       form==ShimForm::BaseNtCanonical || form==ShimForm::BaseAliasCanonical)return iat==canonical;
    return iat==wrapper;
}
constexpr bool RoleSetValid(const RoleForm* forms,size_t count)noexcept {
    if(!forms || count!=5)return false;
    unsigned mask=0;
    for(size_t i=0;i<count;++i){const auto role=forms[i].role;const auto form=forms[i].form;
        if(!RepresentedForm(role,form) &&
           !(role==ShimRole::BaseNt && form==ShimForm::BaseNtNative) &&
           !(role==ShimRole::BaseAlias && form==ShimForm::BaseAliasNative))return false;
        const auto bit=1u<<static_cast<unsigned>(role);
        if(mask&bit)return false;mask|=bit;
    }return mask==31;
}
struct ImportCoverage {
    uint32_t regularDescriptors=0,regularSlots=0,delayDescriptors=0,delaySlots=0;
    bool regularTerminated=false,delayTerminated=false;
};
constexpr bool CoverageComplete(const ImportCoverage& c)noexcept {
    return c.regularTerminated && c.delayTerminated && c.regularSlots<=8192 && c.delaySlots<=8192;
}
struct RepresentedEdge {
    ShimRole role=ShimRole::Outer;ShimForm form=ShimForm::OuterRepresented;
    const void* slot=nullptr;uintptr_t iatTarget=0,resolverTarget=0;
    const char* dll=nullptr;const char* name=nullptr;WORD ordinal=0;
    HMODULE importer=nullptr,canonicalOwner=nullptr;DWORD canonicalRva=0;ImportKind canonicalKind=ImportKind::None;
};
// Noncallable file-family observations never become Import/CanonicalBinding.
struct FileRepresentedEdge {
    unsigned selector=UINT_MAX;const void* slot=nullptr;HMODULE importer=nullptr;
    uintptr_t iatTarget=0,resolverTarget=0;CanonicalBinding canonical{};const char* dll=nullptr;
};
// R794: a signed lazy Import remains an inert DelayThunk forever when its
// loader chooses an already profiled system wrapper. This separate receipt
// changes observation only; it cannot produce a callable CanonicalBinding.
struct DelayedRepresentation {unsigned importIndex=UINT_MAX;FileRepresentedEdge edge{};};
struct ShimRepresentation {
    RepresentationState state=RepresentationState::Absent;
    unsigned recipe=0; // finite file profile; never selected from live wrapper bytes
    HMODULE apphelp=nullptr,native=nullptr;
    uintptr_t table=0,allocation=0,region=0;size_t regionSize=0;DWORD protect=0;
    // Exact represented edges are never Import/CanonicalBinding capabilities.
    std::array<RepresentedEdge,5> edges{};unsigned edgeCount=0;
    std::array<FileRepresentedEdge,128> fileEdges{};unsigned fileCount=0,frozenFileCount=0;
    std::array<ImageCollection,96> frozenImageStates{};
    std::array<ImportCoverage,96> frozenCoverage{};
    unsigned frozenImageCount=0,frozenImportCount=0,frozenPatchCount=0;
    size_t frozenImageBytes=0;uint32_t frozenRegularDescriptors=0,frozenRegularSlots=0;
    uint32_t frozenDelayDescriptors=0,frozenDelaySlots=0;
};
// R041: one original IndexPatch failure, owned by the unpublished draft.
// Atomic busy/generation samples are observations, never a coherent lock receipt
// or an admission condition. Only the existing winning publication exposes it.
struct SetupPatchObservation {
    const char* predicate="none";bool captured=false,busyAtFailure=false;
    unsigned attempt=0,index=UINT_MAX,patchCount=0,imageCount=0,size=0;
    int owner=-1;uint32_t rva=0;unsigned mismatchOffset=UINT_MAX;
    unsigned savedByte=0,cleanByte=0,installByte=0;
    bool installPreimageCaptured=false,restoreMatchesInstall=false;
    uint64_t installGeneration=0,savedPrefix=0,cleanPrefix=0;
    uint64_t snapshotGeneration=0,failureGeneration=0,publicationGeneration=0;
};
// R761: unpublished setup owns mutable image/import state on its worker.
struct NativeValidationPlan;
struct LedgerData {
    // R1009: user-provided so make_unique<LedgerData>() skips preliminary zero-init.
    LedgerData() {}
    uint64_t validationRevision=1;
    std::shared_ptr<const NativeValidationPlan> validationPlan;
    std::array<Image,96> images{};unsigned imageCount=0;
    std::array<ImageCollection,96> imageCollection{};ShimRepresentation shim{};
    std::array<ImportCoverage,96> imageCoverage{};
    size_t imageBytes=0;std::array<Patch,128> patches{};unsigned patchCount=0;
    // Import initializes every field; default construction avoids a huge brace expansion.
    std::array<Import,32768> imports;unsigned importCount=0;
    // R795: append-only, noncallable regular-import observations for newly
    // authenticated dependency images. The sealed setup prefix is immutable.
    std::array<FileRepresentedEdge,128> runtimeFileEdges{};unsigned runtimeFileCount=0;
    std::array<DelayedRepresentation,128> delayedEdges{};unsigned delayedCount=0;
    std::array<unsigned char,32768> delayedIndex{}; // zero or one-based receipt index
    HMODULE factoryModule=nullptr;void* factoryEntry=nullptr;void* swapFactoryEntry=nullptr;
    struct Transition {uint64_t from=0,to=0;DWORD thread=0;};
    std::array<Transition,128> setupTransitions{};unsigned transitionCount=0;
    std::atomic<bool> refused{false};
    const char* refusal=nullptr;HMODULE refusalModule=nullptr;
    std::source_location refusalCaller{};DWORD refusalThread=0;
    bool refusalHasImage=false;Image::OpenObservation refusalImage{};FileTrustObservation refusalTrust{};
    ImageCollectionOrigin refusalOrigin{};SetupPatchObservation setupPatchFailure{};
};
inline LedgerData initialLedger;
struct Store {
    std::atomic<unsigned> initialized{0};std::atomic<bool> revoked{false};
    std::atomic_flag busy=ATOMIC_FLAG_INIT;std::atomic<uint64_t> generation{2};
    std::atomic<LedgerData*> published{&initialLedger};
};
inline Store store;
// R206 scheduling notice only; never native authority. Published outside Guard.
inline std::atomic<bool> optionalSetupPublished{false};
inline const char* PublishedFailure()noexcept;
inline uint64_t ScpValidationEpoch()noexcept {return store.generation.load();}
inline thread_local LedgerData* draftLedger=nullptr;
inline LedgerData& Ledger()noexcept {return draftLedger?*draftLedger:*store.published.load(std::memory_order_acquire);}
inline unsigned RepresentationCount()noexcept {return Ledger().shim.edgeCount+Ledger().shim.fileCount+Ledger().runtimeFileCount;}
inline bool ExpectedImportTarget(unsigned index,uintptr_t& target)noexcept {
    const auto& l=Ledger();if(index>=l.importCount || l.delayedCount>l.delayedEdges.size())return false;
    const auto& base=l.imports[index];const unsigned link=l.delayedIndex[index];
    if(!link){target=base.target;return true;}
    if(link>l.delayedCount)return false;const auto& receipt=l.delayedEdges[link-1];
    if(receipt.importIndex!=index || receipt.edge.slot!=base.slot || !base.lazy ||
       base.binding.kind!=ImportKind::DelayThunk || base.binding.owner!=base.importer)return false;
    target=receipt.edge.iatTarget;return target!=0;
}
inline bool Revoked()noexcept {return store.revoked.load(std::memory_order_acquire) || (draftLedger && draftLedger->refused.load());}
// Only the explicit startup observer overload samples these original operands.
template<class Observation> inline bool Revoked(Observation& o)noexcept {
    const bool revoked=store.revoked.load(std::memory_order_acquire);
    if(!o.Leaf("native-store-not-revoked",!revoked))return true;
    if(draftLedger){const bool refused=draftLedger->refused.load();
        o.Leaf("native-draft-not-refused",!refused);return refused;
    }
    return false;
}
struct LiveLedgerScope final {
    LedgerData* prior=draftLedger;
    LiveLedgerScope()noexcept {draftLedger=nullptr;}
    ~LiveLedgerScope(){draftLedger=prior;}
    LiveLedgerScope(const LiveLedgerScope&)=delete;
};
// R728: observation only. Fixed storage is published once during the winning
// Setup invocation and is never reset or consulted by any acceptance gate.
struct ImportFailureReceipt {
    const char* stage="none";const char* priorReason="none";
    DWORD pid=0,thread=0;HMODULE importer=nullptr,nominal=nullptr,resolverModule=nullptr;
    const void* slot=nullptr;WORD ordinal=0,resolverOrdinal=0;
    unsigned hop=0;DWORD exportIndex=UINT32_MAX;
    std::array<char,512> dll{},symbol{},resolverSymbol{},forward{};
    std::array<char,128> importerName{},resolverDll{},expectedOwnerName{};
    struct Address {
        uintptr_t value=0,owner=0,rva=0;DWORD memoryType=0;
        bool ownerKnown=false;std::array<char,128> ownerName{};
    } actual{},resolverActual{};
    HMODULE expectedOwner=nullptr,queriedActualOwner=nullptr;
    uintptr_t expected=0;uint32_t expectedRva=0;ImportKind expectedKind=ImportKind::None;
    bool expectedKnown=false,actualKnown=false,resolverActualKnown=false,textTruncated=false;
    // R739: snapshots of a rejected binding, never authentication evidence.
    // Keep the original stage token. R739 classifications have separate logs.
    const char* baseStage="none";
    struct Corroboration {
        const char* eatSlot="unknown";const char* iatSample="unknown";const char* iatSlot="absent";
        uintptr_t eatSlotAddress=0,iatNow=0;DWORD cleanEatRva=0,liveEatRva=0;
        bool eatSlotRead=false,iatSlotRead=false;
    } corroboration{};

    // R740: fixed-capacity first-rejection observations, NEVER capabilities.
    struct ShimModule {
        const char* status="not-read";uintptr_t base=0;uint64_t fileSize=0;
        DWORD timestamp=0,imageSize=0,versionMS=0,versionLS=0,error=0;
        bool diskRead=false,diskMatch=false,liveRead=false,liveMatch=false;
    };
    struct ShimSlot {
        const char* status="not-read";uintptr_t address=0,value=0;
        bool read=false;DWORD error=0;
    };
    struct ShimCode {
        const char* status="not-read";Address address{};
        const char* comparisonSource="none";unsigned comparisonBytes=0;
        DWORD protect=0,error=0;bool read=false,diskRead=false,comparisonMatch=false;
        std::array<char,129> bytes{}; // exactly 64 sampled bytes, hex, or empty
    };
    struct Shim {
        const char* status="not-applicable";
        // Profile names refer to VS_FIXEDFILEINFO, not stale version strings.
        const char* profile="apphelp-fixed-10.0.22621.5983-r740-v1";
        ShimModule apphelp{},ntdll{};
        bool wrapperDiskRead=false,wrapperDiskMatch=false,wrapperRead=false,wrapperMatch=false;
        bool locatorsRead=false,locatorsMatch=false,thunksRead=false,thunksMatch=false;
        std::array<char,129> wrapperBytes{};
        ShimSlot tableRoot{},xfgSlot{},cfSlot{};
        uintptr_t rowAddress=0,tableAllocation=0;DWORD tableType=0,tableState=0,tableProtect=0;
        bool tableRegionRead=false,rowRead=false,rowSelectorsMatch=false;
        DWORD rowError=0;std::array<uintptr_t,6> row{}; // no string/pointer traversal
        ShimCode original{},xfg{},cf{};
        bool xfgUsesDeclaredCf=false;
    } shim{};
};
struct ImportFailureContext {
    const char* dll=nullptr;const char* symbol=nullptr;const char* resolverSymbol=nullptr;
    const char* forward=nullptr;HMODULE importer=nullptr,nominal=nullptr,resolverModule=nullptr;
    const void* slot=nullptr;WORD ordinal=0,resolverOrdinal=0;unsigned hop=0;
    DWORD exportIndex=UINT32_MAX;std::array<char,128> resolverDll{};
    uintptr_t actual=0,resolverActual=0,expected=0;
    HMODULE expectedOwner=nullptr,queriedActualOwner=nullptr;
    uint32_t expectedRva=0;ImportKind expectedKind=ImportKind::None;
    bool actualKnown=false,expectedKnown=false,resolverActualKnown=false,textTruncated=false;
};
inline ImportFailureReceipt firstImportFailure;
inline std::atomic<unsigned> firstImportFailureState{0}; // 0 absent, 1 writing, 2 immutable
inline thread_local bool observeSetupImports=false;
template<size_t N> inline bool ImportText(std::array<char,N>& out,const char* value)noexcept {
    out.fill(0);if(!value)return false;size_t i=0;
    for(;i+1<N && value[i];++i)out[i]=value[i];
    return value[i]!=0;
}
inline bool ImportModuleName(HMODULE module,std::array<char,128>& out)noexcept {
    // A bounded observed basename, not file trust or canonical selection.
    out.fill(0);if(!module)return false;wchar_t path[MAX_PATH]{};
    const auto count=GetModuleFileNameW(module,path,MAX_PATH);
    if(!count || count>=MAX_PATH)return true;
    const auto* name=std::wcsrchr(path,L'\\');name=name?name+1:path;
    size_t i=0;bool truncated=false;
    for(;i+1<out.size() && name[i];++i){
        if(name[i]>127){out[i]='?';truncated=true;}else out[i]=static_cast<char>(name[i]);
    }
    return truncated || name[i]!=0;
}
inline void ObserveImportAddress(uintptr_t value,ImportFailureReceipt::Address& out,bool& truncated)noexcept {
    out.value=value;if(!value)return;MEMORY_BASIC_INFORMATION memory{};
    if(!VirtualQuery(reinterpret_cast<const void*>(value),&memory,sizeof(memory)))return;
    out.memoryType=memory.Type;
    const auto base=reinterpret_cast<uintptr_t>(memory.AllocationBase);
    if(memory.Type!=MEM_IMAGE || !base || value<base)return;
    out.ownerKnown=true;out.owner=base;out.rva=value-base;
    truncated=ImportModuleName(static_cast<HMODULE>(memory.AllocationBase),out.ownerName) || truncated;
}
// Read only after the first-failure writer has claimed fixed storage. All
// addresses come from the already selected clean hop or the observed IAT slot.
// No live EAT parsing, extra resolver call, target execution or jump following.
inline void ObserveRejectedCorroboration(const ImportFailureContext& context,ImportFailureReceipt& out)noexcept {
    auto& snapshot=out.corroboration;
    if(context.actualKnown && context.expectedKnown && context.resolverActualKnown)
        snapshot.iatSample=context.actual==context.expected?"equals_expected":
            (context.actual==context.resolverActual?"equals_resolver":"differs_both");
    if(context.slot){
        snapshot.iatSlot="unreadable";
        if(Memory(context.slot,&snapshot.iatNow,sizeof(snapshot.iatNow))){
            snapshot.iatSlotRead=true;
            snapshot.iatSlot=!context.actualKnown?"unknown":
                (snapshot.iatNow==context.actual?"same_sample":"changed");
        }
    }
    for(unsigned i=0;i<Ledger().imageCount;++i){const auto& image=Ledger().images[i];
        if(image.live!=context.resolverModule)continue;
        const auto d=image.exports;
        if(d.Size<sizeof(IMAGE_EXPORT_DIRECTORY) || !image.Range(d.VirtualAddress,sizeof(IMAGE_EXPORT_DIRECTORY)))break;
        const auto ex=Read<IMAGE_EXPORT_DIRECTORY>(image.clean.data()+d.VirtualAddress);
        if(context.exportIndex>=ex.NumberOfFunctions || !ex.AddressOfFunctions)break;
        const uint64_t cell=uint64_t(ex.AddressOfFunctions)+4ull*context.exportIndex;
        if(cell>UINT32_MAX || !image.Range(static_cast<uint32_t>(cell),sizeof(DWORD)))break;
        const auto base=reinterpret_cast<uintptr_t>(image.live);
        if(cell>UINTPTR_MAX-base)break;
        snapshot.eatSlotAddress=base+static_cast<uintptr_t>(cell);
        snapshot.cleanEatRva=Read<DWORD>(image.clean.data()+static_cast<size_t>(cell));
        snapshot.eatSlot="unreadable";
        if(Memory(reinterpret_cast<const void*>(snapshot.eatSlotAddress),&snapshot.liveEatRva,sizeof(DWORD))){
            snapshot.eatSlotRead=true;
            snapshot.eatSlot=snapshot.liveEatRva==snapshot.cleanEatRva?"match":"mismatch";
        }
        break;
    }
    // These sequential reads are not an atomic snapshot or an immutability
    // proof. A matching EAT cell says nothing about the live name map or code.
}

// R740 diagnostics only. These helpers do not use AddImage/ResolveCanonicalImport,
// trust APIs, the loader, an instruction scanner, or ANY acceptance/epoch state.
// Disk and live equality below cover explicitly selected bytes, NOT whole images.
namespace shim_observation {
inline constexpr std::array<unsigned char,64> apphelpDos{{
    0x4d,0x5a,0x90,0x00,0x03,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0xff,0xff,0x00,0x00,
    0xb8,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xf0,0x00,0x00,0x00,
}};
inline constexpr std::array<unsigned char,264> apphelpNt{{
    0x50,0x45,0x00,0x00,0x64,0x86,0x07,0x00,0xb3,0x9d,0x2a,0x80,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xf0,0x00,0x22,0x20,0x0b,0x02,0x0e,0x1e,0x00,0x20,0x05,0x00,
    0x00,0x40,0x04,0x00,0x00,0x00,0x00,0x00,0xf0,0x49,0x01,0x00,0x00,0x10,0x00,0x00,
    0x00,0x00,0x00,0x80,0x01,0x00,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x10,0x00,0x00,
    0x0a,0x00,0x00,0x00,0x0a,0x00,0x00,0x00,0x0a,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x70,0x09,0x00,0x00,0x10,0x00,0x00,0x2c,0x81,0x0a,0x00,0x02,0x00,0x60,0x41,
    0x00,0x00,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x40,0x32,0x07,0x00,0xe4,0x1a,0x00,0x00,
    0x24,0x4d,0x07,0x00,0xe0,0x01,0x00,0x00,0x00,0xf0,0x07,0x00,0x88,0x67,0x01,0x00,
    0x00,0xa0,0x07,0x00,0x88,0x35,0x00,0x00,0x00,0x60,0x09,0x00,0x20,0x3b,0x00,0x00,
    0x00,0x60,0x09,0x00,0xe0,0x07,0x00,0x00,0x64,0x7e,0x05,0x00,0x70,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x70,0x31,0x05,0x00,0x40,0x01,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x78,0x50,0x05,0x00,0x38,0x08,0x00,0x00,
    0xc0,0x31,0x07,0x00,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
}};
inline constexpr std::array<unsigned char,52> apphelpVersion{{
    0xbd,0x04,0xef,0xfe,0x00,0x00,0x01,0x00,0x00,0x00,0x0a,0x00,0x5f,0x17,0x5d,0x58,
    0x00,0x00,0x0a,0x00,0x5f,0x17,0x5d,0x58,0x3f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x04,0x00,0x04,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,
}};
inline constexpr std::array<unsigned char,64> ntdllDos{{
    0x4d,0x5a,0x90,0x00,0x03,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0xff,0xff,0x00,0x00,
    0xb8,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xf0,0x00,0x00,0x00,
}};
inline constexpr std::array<unsigned char,264> ntdllNt{{
    0x50,0x45,0x00,0x00,0x64,0x86,0x0b,0x00,0xa3,0xda,0x55,0xac,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xf0,0x00,0x22,0x20,0x0b,0x02,0x0e,0x1e,0x00,0x20,0x13,0x00,
    0x00,0x50,0x0e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x00,0x00,
    0x00,0x00,0x00,0x80,0x01,0x00,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x10,0x00,0x00,
    0x0a,0x00,0x00,0x00,0x0a,0x00,0x00,0x00,0x0a,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x80,0x21,0x00,0x00,0x10,0x00,0x00,0xe1,0x43,0x22,0x00,0x03,0x00,0x60,0x41,
    0x00,0x00,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0xc0,0xcc,0x16,0x00,0xb2,0x36,0x01,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x1a,0x00,0xa8,0x59,0x07,0x00,
    0x00,0xd0,0x18,0x00,0x6c,0xed,0x00,0x00,0x00,0x00,0x21,0x00,0x50,0x70,0x00,0x00,
    0x00,0x70,0x21,0x00,0x50,0x05,0x00,0x00,0xf0,0xda,0x13,0x00,0x70,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x3e,0x13,0x00,0x40,0x01,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
}};
inline constexpr std::array<unsigned char,52> ntdllVersion{{
    0xbd,0x04,0xef,0xfe,0x00,0x00,0x01,0x00,0x00,0x00,0x0a,0x00,0xac,0x17,0x5d,0x58,
    0x00,0x00,0x0a,0x00,0xac,0x17,0x5d,0x58,0x3f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x04,0x00,0x04,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,
}};
inline constexpr std::array<unsigned char,137> apphelpWrapper{{
    0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x40,0x48,
    0x8b,0x05,0x2a,0x0a,0x03,0x00,0x48,0x8b,0xda,0x48,0x8b,0xf1,0x49,0xba,0x70,0xb2,
    0x54,0x76,0xd9,0x27,0xcc,0xdf,0x44,0x8b,0x5c,0x24,0x78,0x48,0x8b,0x40,0x78,0x44,
    0x89,0x5c,0x24,0x28,0x4c,0x8b,0x5c,0x24,0x70,0x4c,0x89,0x5c,0x24,0x20,0xff,0x15,
    0xa4,0xce,0x00,0x00,0x8b,0xf8,0x85,0xc0,0x78,0x2d,0x48,0x85,0xdb,0x74,0x28,0x66,
    0x83,0x3b,0x1e,0x75,0x22,0x48,0x8b,0x4b,0x08,0x48,0x8d,0x15,0xa0,0x11,0x02,0x00,
    0x41,0xb8,0x0f,0x00,0x00,0x00,0xe8,0x1e,0xab,0xfc,0xff,0x85,0xc0,0x75,0x08,0x48,
    0x8b,0xce,0xe8,0x2d,0x01,0x00,0x00,0x48,0x8b,0x5c,0x24,0x50,0x8b,0xc7,0x48,0x8b,
    0x74,0x24,0x58,0x48,0x83,0xc4,0x40,0x5f,0xc3,
}};
inline constexpr std::array<unsigned char,6> apphelpXfgThunk{{
    0xff,0x25,0xe2,0x04,0x04,0x00,
}};
inline constexpr std::array<unsigned char,2> apphelpCfThunk{{
    0xff,0xe0,
}};
struct Profile {
    const wchar_t* name;uint64_t fileSize;DWORD versionRaw,versionRva;
    const std::array<unsigned char,64>& dos;
    const std::array<unsigned char,264>& nt;
    const std::array<unsigned char,52>& version;
};
inline const Profile apphelpProfile{L"apphelp.dll",629536,0x7e188,0x7f188,apphelpDos,apphelpNt,apphelpVersion};
inline const Profile ntdllProfile{L"ntdll.dll",2191440,0x199118,0x1a1118,ntdllDos,ntdllNt,ntdllVersion};
} // namespace shim_observation
#include "GenshinNativeShimRecipes.inl"
namespace shim_observation {
template<size_t N> inline void Hex(const std::array<unsigned char,N>& in,std::array<char,2*N+1>& out)noexcept {
    constexpr char digits[]="0123456789abcdef";
    for(size_t i=0;i<N;++i){out[2*i]=digits[in[i]>>4];out[2*i+1]=digits[in[i]&15];}out[2*N]=0;
}
template<size_t N> inline bool Disk(HANDLE file,DWORD offset,std::array<unsigned char,N>& out)noexcept {
    LARGE_INTEGER at{};at.QuadPart=offset;DWORD got=0;
    return SetFilePointerEx(file,at,nullptr,FILE_BEGIN) &&
        ReadFile(file,out.data(),static_cast<DWORD>(N),&got,nullptr) && got==N;
}
template<size_t N> inline bool Live(uintptr_t base,DWORD rva,std::array<unsigned char,N>& out)noexcept {
    return base && rva<=UINTPTR_MAX-base && N<=UINTPTR_MAX-(base+rva) &&
        Memory(reinterpret_cast<const void*>(base+rva),out.data(),N);
}
inline bool OpenProfile(HMODULE module,const Profile& p,ImportFailureReceipt::ShimModule& out,Handle& file)noexcept {
    out.base=reinterpret_cast<uintptr_t>(module);out.status="system-path-refused";
    wchar_t system[MAX_PATH]{},loaded[MAX_PATH]{},expected[MAX_PATH]{},finalPath[MAX_PATH+4]{};
    const auto sys=GetSystemDirectoryW(system,MAX_PATH);
    const auto len=GetModuleFileNameW(module,loaded,MAX_PATH);
    if(!sys || sys>=MAX_PATH || !len || len>=MAX_PATH ||
       swprintf_s(expected,L"%ls\\%ls",system,p.name)<0 || _wcsicmp(expected,loaded))return false;
    file.value=CreateFileW(expected,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    out.status="disk-open-refused";
    if(file.value==INVALID_HANDLE_VALUE){out.error=GetLastError();return false;}
    FILE_ATTRIBUTE_TAG_INFO attributes{};LARGE_INTEGER size{};
    const auto finalCount=GetFinalPathNameByHandleW(file.value,finalPath,MAX_PATH+4,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    out.status="disk-identity-refused";
    if(!GetFileInformationByHandleEx(file.value,FileAttributeTagInfo,&attributes,sizeof(attributes)) ||
       (attributes.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) || finalCount<5 || finalCount>=MAX_PATH+4 ||
       std::wcsncmp(finalPath,L"\\\\?\\",4) || _wcsicmp(finalPath+4,expected) || !GetFileSizeEx(file.value,&size))return false;
    out.fileSize=static_cast<uint64_t>(size.QuadPart);if(out.fileSize!=p.fileSize)return false;
    std::array<unsigned char,64> dos{};std::array<unsigned char,264> nt{};std::array<unsigned char,52> version{};
    const auto ntRva=Read<DWORD>(p.dos.data()+offsetof(IMAGE_DOS_HEADER,e_lfanew));
    out.status="disk-profile-unreadable";
    if(!Disk(file.value,0,dos) || !Disk(file.value,ntRva,nt) || !Disk(file.value,p.versionRaw,version)){
        out.error=GetLastError();return false;}
    out.diskRead=true;out.timestamp=Read<DWORD>(nt.data()+offsetof(IMAGE_NT_HEADERS64,FileHeader)+offsetof(IMAGE_FILE_HEADER,TimeDateStamp));
    out.imageSize=Read<DWORD>(nt.data()+offsetof(IMAGE_NT_HEADERS64,OptionalHeader)+offsetof(IMAGE_OPTIONAL_HEADER64,SizeOfImage));
    out.versionMS=Read<DWORD>(version.data()+8);out.versionLS=Read<DWORD>(version.data()+12);
    out.diskMatch=dos==p.dos && nt==p.nt && version==p.version;
    out.status="disk-profile-mismatch";if(!out.diskMatch)return false;
    out.status="live-profile-unreadable";
    if(!Live(out.base,0,dos) || !Live(out.base,ntRva,nt) || !Live(out.base,p.versionRva,version)){
        out.error=GetLastError();return false;}
    out.liveRead=true;
    constexpr size_t imageBaseAt=offsetof(IMAGE_NT_HEADERS64,OptionalHeader)+offsetof(IMAGE_OPTIONAL_HEADER64,ImageBase);
    const auto liveBase=Read<uint64_t>(nt.data()+imageBaseAt);
    auto expectedNt=p.nt;
    // Same two bounded loader representations as Image::Open; no header masking.
    if(liveBase==out.base)std::memcpy(expectedNt.data()+imageBaseAt,&liveBase,sizeof(liveBase));
    out.liveMatch=dos==p.dos && nt==expectedNt && version==p.version;
    out.status=out.liveMatch?"selected-profile-match":"live-profile-mismatch";return out.liveMatch;
}
inline void Slot(uintptr_t base,DWORD rva,ImportFailureReceipt::ShimSlot& out)noexcept {
    out.status="range-refused";if(!base || rva>UINTPTR_MAX-base)return;
    out.address=base+rva;out.read=Memory(reinterpret_cast<const void*>(out.address),&out.value,sizeof(out.value));
    out.status=out.read?"read":"unreadable";if(!out.read)out.error=GetLastError();
}
// Only this observed, already-profiled module's .text or two exact local thunks
// can supply code samples. No imported pointer, branch or arbitrary owner is followed.
inline void Code(uintptr_t value,uintptr_t apphelp,uintptr_t ntdll,HANDLE nativeFile,
                 bool nativeProfile,const shim_recipe::Recipe& p,ImportFailureReceipt::ShimCode& out,bool& truncated)noexcept {
    ObserveImportAddress(value,out.address,truncated);out.status="owner-or-range-refused";
    DWORD raw=0;const unsigned char* exact=nullptr;size_t exactSize=0;
    if(out.address.ownerKnown && out.address.owner==apphelp && value==apphelp+p.xfg.rva){
        exact=p.xfg.bytes;exactSize=p.xfg.size;
    }else if(out.address.ownerKnown && out.address.owner==apphelp && value==apphelp+p.cf.rva){
        exact=p.cf.bytes;exactSize=p.cf.size;
    }else if(nativeProfile && out.address.ownerKnown && out.address.owner==ntdll &&
        out.address.rva>=0x1000 && Span(out.address.rva-0x1000,64,&p==&shim_recipe::System26100?0x16aebc:0x12e0c2)){
        raw=static_cast<DWORD>(out.address.rva); // pinned ntdll .text raw offset equals RVA
    }else return;
    MEMORY_BASIC_INFORMATION memory{};
    if(!VirtualQuery(reinterpret_cast<const void*>(value),&memory,sizeof(memory)) ||
       memory.Type!=MEM_IMAGE || memory.State!=MEM_COMMIT ||
       reinterpret_cast<uintptr_t>(memory.AllocationBase)!=out.address.owner ||
       (memory.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return;
    out.protect=memory.Protect;
    if((memory.Protect&0xff)!=PAGE_EXECUTE_READ && (memory.Protect&0xff)!=PAGE_EXECUTE_WRITECOPY)return;
    std::array<unsigned char,64> live{},disk{};
    out.read=Memory(reinterpret_cast<const void*>(value),live.data(),live.size());
    if(!out.read){out.status="code-unreadable";out.error=GetLastError();return;}Hex(live,out.bytes);
    if(exact){
        out.comparisonSource="compiled_profile";out.comparisonBytes=static_cast<unsigned>(exactSize);
        out.comparisonMatch=!std::memcmp(live.data(),exact,exactSize);
        out.status=out.comparisonMatch?"profile-code-match":"code-mismatch";
    }else {
        out.comparisonSource="disk_window";out.comparisonBytes=static_cast<unsigned>(disk.size());
        out.diskRead=Disk(nativeFile,raw,disk);out.comparisonMatch=out.diskRead && disk==live;
        out.status=!out.diskRead?"disk-code-unreadable":(out.comparisonMatch?"selected-code-match":"code-mismatch");
    }
    // A sample is not a trusted execution chain. ALWAYS stop here, even on match.
}
inline void Capture(const ImportFailureContext& context,ImportFailureReceipt& out)noexcept {
    auto& s=out.shim;
    const auto& p=context.expectedRva==shim_recipe::System26100.families[2].nativeRva?
        shim_recipe::System26100:shim_recipe::Legacy;
    const auto nativeRva=p.families[2].nativeRva;
    if(!context.expectedKnown || !context.actualKnown || !context.resolverActualKnown ||
       context.actual!=context.resolverActual || !context.symbol || std::strcmp(context.symbol,"NtSetValueKey") ||
       !context.dll || _stricmp(context.dll,"ntdll.dll") || context.expectedRva!=nativeRva ||
       context.expectedKind!=ImportKind::CodeBinding || !out.actual.ownerKnown ||
       out.actual.rva!=p.wrapper.rva || !context.expectedOwner ||
       context.expected!=reinterpret_cast<uintptr_t>(context.expectedOwner)+nativeRva)return;
    s.profile=p.name;s.status="apphelp-profile-refused";Handle appFile,nativeFile;
    const auto appBase=out.actual.owner,nativeBase=reinterpret_cast<uintptr_t>(context.expectedOwner);
    if(!OpenProfile(reinterpret_cast<HMODULE>(appBase),*p.app,s.apphelp,appFile))return;
    std::array<unsigned char,137> disk{},live{};
    s.wrapperDiskRead=Disk(appFile.value,p.wrapper.rva,disk);s.wrapperDiskMatch=s.wrapperDiskRead && !std::memcmp(disk.data(),p.wrapper.bytes,p.wrapper.size);
    s.status="disk-wrapper-refused";if(!s.wrapperDiskMatch)return;
    s.wrapperRead=Live(appBase,p.wrapper.rva,live);s.wrapperMatch=s.wrapperRead && !std::memcmp(live.data(),p.wrapper.bytes,p.wrapper.size);
    if(s.wrapperRead){std::array<unsigned char,64> prefix{};std::memcpy(prefix.data(),live.data(),prefix.size());Hex(prefix,s.wrapperBytes);}
    s.status="live-wrapper-refused";if(!s.wrapperMatch)return;
    // Prove the exact load-config locators and the local two-hop default shape
    // before reading ANY dispatcher slot or mutable table pointer.
    std::array<unsigned char,8> dx{},dc{},lx{},lc{};
    const bool locatorDisk=Disk(appFile.value,p.loadConfig+0x120,dx) && Disk(appFile.value,p.loadConfig+0x78,dc);
    s.locatorsRead=locatorDisk && Live(appBase,p.loadConfig+0x120,lx) && Live(appBase,p.loadConfig+0x78,lc);
    s.locatorsMatch=s.locatorsRead && Read<uint64_t>(dx.data())==0x180000000ull+p.xfgCell &&
        Read<uint64_t>(dc.data())==0x180000000ull+p.cfCell && Read<uint64_t>(lx.data())==appBase+p.xfgCell &&
        Read<uint64_t>(lc.data())==appBase+p.cfCell;
    s.status="guard-locators-refused";if(!s.locatorsMatch)return;
    std::array<unsigned char,6> dxt{},lxt{};std::array<unsigned char,2> dct{},lct{};
    s.thunksRead=Disk(appFile.value,p.xfg.rva,dxt) && Disk(appFile.value,p.cf.rva,dct) &&
        Live(appBase,p.xfg.rva,lxt) && Live(appBase,p.cf.rva,lct);
    s.thunksMatch=s.thunksRead && !std::memcmp(dxt.data(),p.xfg.bytes,p.xfg.size) && !std::memcmp(lxt.data(),p.xfg.bytes,p.xfg.size) &&
        !std::memcmp(dct.data(),p.cf.bytes,p.cf.size) && !std::memcmp(lct.data(),p.cf.bytes,p.cf.size);
    s.status="local-thunks-refused";if(!s.thunksMatch)return;
    const bool nativeProfile=OpenProfile(context.expectedOwner,*p.native,s.ntdll,nativeFile);
    Slot(appBase,p.tableCell,s.tableRoot);Slot(appBase,p.xfgCell,s.xfgSlot);Slot(appBase,p.cfCell,s.cfSlot);
    if(s.xfgSlot.read)Code(s.xfgSlot.value,appBase,nativeBase,nativeFile.value,nativeProfile,p,s.xfg,out.textTruncated);
    s.xfgUsesDeclaredCf=s.xfgSlot.read && s.xfgSlot.value==appBase+p.xfg.rva && s.xfg.comparisonMatch;
    // cf is the declared CF slot. It is a successor ONLY when xfgUsesDeclaredCf.
    if(s.cfSlot.read)Code(s.cfSlot.value,appBase,nativeBase,nativeFile.value,nativeProfile,p,s.cf,out.textTruncated);
    s.status="table-root-unreadable";if(!s.tableRoot.read)return;
    const auto table=s.tableRoot.value;s.status="table-range-refused";
    if(!table || (table&7) || table>UINTPTR_MAX-0x90)return;
    s.rowAddress=table+0x60;MEMORY_BASIC_INFORMATION memory{};
    s.tableRegionRead=VirtualQuery(reinterpret_cast<const void*>(s.rowAddress),&memory,sizeof(memory))==sizeof(memory);
    if(!s.tableRegionRead)return;
    s.tableAllocation=reinterpret_cast<uintptr_t>(memory.AllocationBase);s.tableType=memory.Type;
    s.tableState=memory.State;s.tableProtect=memory.Protect;
    const auto region=reinterpret_cast<uintptr_t>(memory.BaseAddress);const auto protection=memory.Protect&0xff;
    if(memory.Type!=MEM_PRIVATE || memory.State!=MEM_COMMIT || (memory.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
       (protection!=PAGE_READONLY && protection!=PAGE_READWRITE && protection!=PAGE_WRITECOPY) ||
       s.rowAddress<region || !Span(s.rowAddress-region,sizeof(s.row),memory.RegionSize))return;
    s.rowRead=Memory(reinterpret_cast<const void*>(s.rowAddress),s.row.data(),sizeof(s.row));
    s.status="table-row-unreadable";if(!s.rowRead){s.rowError=GetLastError();return;}
    s.rowSelectorsMatch=s.row[0]==appBase+p.dllName && s.row[1]==appBase+p.families[2].symbol && s.row[2]==appBase+p.wrapper.rva;
    s.status="table-selectors-mismatch";if(!s.rowSelectorsMatch)return;
    // A different original value is recorded, but NEVER dereferenced.
    if(s.row[3]==context.expected && nativeProfile)
        Code(s.row[3],appBase,nativeBase,nativeFile.value,true,p,s.original,out.textTruncated);
    else {ObserveImportAddress(s.row[3],s.original.address,out.textTruncated);s.original.status="noncanonical-or-native-profile-refused";}
    s.status="bounded-snapshot-complete";
    // Sequential reads only; no retry, state freeze, ABA exclusion or continuity claim.
}
} // namespace shim_observation

inline thread_local ImportFailureReceipt draftImportFailure;
inline thread_local bool draftImportFailureSet=false;
inline void PinImportFailure(const char* stage,const ImportFailureContext& context)noexcept {
    if(!observeSetupImports)return;unsigned absent=0;
    if(draftLedger){if(draftImportFailureSet)return;draftImportFailureSet=true;}
    else if(!firstImportFailureState.compare_exchange_strong(absent,1))return;
    struct KeepLastError {DWORD value;~KeepLastError(){SetLastError(value);}} keepLastError{GetLastError()};
    auto& out=draftLedger?draftImportFailure:firstImportFailure;out.stage=stage;out.baseStage=stage;
    const auto* reason=draftLedger?draftLedger->refusal:PublishedFailure();out.priorReason=reason?reason:"none";
    out.pid=GetCurrentProcessId();out.thread=GetCurrentThreadId();
    out.importer=context.importer;out.nominal=context.nominal;out.slot=context.slot;
    out.ordinal=context.ordinal;out.resolverOrdinal=context.resolverOrdinal;
    out.hop=context.hop;out.exportIndex=context.exportIndex;out.resolverModule=context.resolverModule;
    out.expected=context.expected;out.expectedOwner=context.expectedOwner;out.expectedRva=context.expectedRva;
    out.expectedKind=context.expectedKind;out.expectedKnown=context.expectedKnown;
    out.actualKnown=context.actualKnown;out.resolverActualKnown=context.resolverActualKnown;
    out.queriedActualOwner=context.queriedActualOwner;
    out.textTruncated=ImportText(out.dll,context.dll) || context.textTruncated;
    out.textTruncated=ImportText(out.symbol,context.symbol) || out.textTruncated;
    out.textTruncated=ImportText(out.resolverDll,context.resolverDll.data()) || out.textTruncated;
    if(!out.resolverDll[0])out.textTruncated=ImportModuleName(context.resolverModule,out.resolverDll) || out.textTruncated;
    out.textTruncated=ImportText(out.resolverSymbol,context.resolverSymbol) || out.textTruncated;
    out.textTruncated=ImportText(out.forward,context.forward) || out.textTruncated;
    out.textTruncated=ImportModuleName(context.importer,out.importerName) || out.textTruncated;
    out.textTruncated=ImportModuleName(context.expectedOwner,out.expectedOwnerName) || out.textTruncated;
    if(context.actualKnown)ObserveImportAddress(context.actual,out.actual,out.textTruncated);
    if(context.resolverActualKnown)ObserveImportAddress(context.resolverActual,out.resolverActual,out.textTruncated);
    // Observation owners come only from VirtualQuery MEM_IMAGE allocation
    // bases. They are explicitly untrusted and never adopted into a binding.
    if(!std::strcmp(stage,"getprocaddress-corroboration")){
        ObserveRejectedCorroboration(context,out);
        shim_observation::Capture(context,out);
    }
    if(!draftLedger)firstImportFailureState.store(2,std::memory_order_release);
}
// R796: terminal validation failures revoke regardless of setup scheduling.
// Retain their exact original context even outside the optional setup window.
// This scoped diagnostic preserves caller TLS and grants no admission/retry.
inline void PinTerminalImportFailure(const char* stage,const ImportFailureContext& context)noexcept {
    const bool previous=observeSetupImports;observeSetupImports=true;
    PinImportFailure(stage,context);observeSetupImports=previous;
}
// R748: original NativeContext traversal only; never queried by admission.
// The two alternatives retain separate leaves so fallback cannot erase a failure.
struct ContextTableLeaf {
    const char* scope="not-evaluated";const char* predicate="not-evaluated";
    const char* statusKind="none";LONG status=0;
    int slot=-1,entry=-1,mask=-1,mode=-1,image=-1,patch=-1,section=-1;
    int modeMismatch[2]{-1,-1};uint32_t rva=0;
    size_t offset=0,spanOffset=0,requestedBytes=0,readBytes=0;
    uint64_t apiValue=0;DWORD win32=0;bool apiValid=false,win32Valid=false;
    std::array<uintptr_t,2> actual{},expected{};
    void BeforeLeaf(const char* next)noexcept {
        predicate=next;statusKind="none";status=0;apiValid=false;apiValue=0;
        win32Valid=false;win32=0;requestedBytes=0;readBytes=0;
    }
    void At(const char* next,uint32_t nextRva=0)noexcept {
        scope=next;rva=nextRva;slot=-1;entry=-1;mask=-1;mode=-1;image=-1;patch=-1;section=-1;
        offset=0;spanOffset=0;BeforeLeaf("not-evaluated");
    }
    bool Leaf(const char* next,bool value)noexcept {
        predicate=next;statusKind="bool";status=value?TRUE:FALSE;return value;
    }
    void Win32(const char* next,BOOL value,DWORD error)noexcept {
        predicate=next;statusKind="win32_bool";status=value;win32=error;win32Valid=true;
    }
    void EntryMismatch(unsigned index,uintptr_t value,uintptr_t wanted)noexcept {
        actual[index]=value;expected[index]=wanted;
    }
};
struct NativeContextFailure {
    uintptr_t object=0,table=0,factory=0;unsigned slots=0;int owner=-2;
    uint64_t generation=0;DWORD thread=0;
    ContextTableLeaf tableLeaf{},inlineLeaf{},swapLeaf{},bindingLeaf{};
    bool bindingShape=false,executionOnly=false;
};
// Shape refusal is not executable-image corruption. Keep a separate sticky GPU
// context-capability denial so CPU binding closure can still authenticate its
// own setters. Never clear store.revoked or grant any GPU context authority.
inline std::atomic<bool> contextExecutionRefused{false};
inline NativeContextFailure firstContextFailure;
inline std::atomic<unsigned> firstContextFailureState{0};
inline void PinContextFailure(const NativeContextFailure& failure)noexcept {
    unsigned absent=0;
    if(firstContextFailureState.compare_exchange_strong(absent,1)){
        firstContextFailure=failure;firstContextFailureState.store(2,std::memory_order_release);
    }
}
// R760: immutable observation only; no acceptance gate reads this receipt.
struct FirstRevocationReceipt {
    const char* file="none";const char* function="none";const char* reason="none";
    unsigned line=0;DWORD thread=0;HMODULE module=nullptr;
    bool hasImage=false;Image::OpenObservation image{};FileTrustObservation trust{};
    ImageCollectionOrigin origin{};SetupPatchObservation setupPatchFailure{};
    bool hasContextTable=false;NativeContextFailure contextTable{};
};
inline FirstRevocationReceipt firstRevocation;
inline std::atomic<unsigned> firstRevocationState{0}; // 0 absent, 1 writing, 2 immutable
using FirstRevocationObserver=void(*)()noexcept;
inline std::atomic<FirstRevocationObserver> firstRevocationObserver{nullptr};
inline std::atomic<bool> firstRevocationReported{false};
inline const char* PublishedFailure()noexcept {
    const auto state=firstRevocationState.load(std::memory_order_acquire);
    return state==2?firstRevocation.reason:(state==1?"publication-pending":"none");
}
inline void NotifyFirstRevocation()noexcept {
    struct KeepLastError {DWORD value=GetLastError();~KeepLastError(){SetLastError(value);}} keep;
    if(firstRevocationState.load(std::memory_order_acquire)!=2)return;
    const auto observer=firstRevocationObserver.load();if(!observer)return;
    bool absent=false;if(!firstRevocationReported.compare_exchange_strong(absent,true))return;
    // Synchronous: Setup drains after its guards, and the existing Genshin
    // Present owner drains runtime refusals outside collector/native locks.
    // Successful Setup's pre-existing GET_MODULE_HANDLE_EX_FLAG_PIN holds Opt.
    // No queued callback or independent logging lifetime is created.
    observer();
}
inline void Revoke(const char* reason="receipt-or-generation",HMODULE module=nullptr,
                   const std::source_location& caller=std::source_location::current(),
                   const Image::OpenObservation* image=nullptr,const FileTrustObservation* trust=nullptr,
                   DWORD originalThread=0,const ImageCollectionOrigin* origin=nullptr,
                   const SetupPatchObservation* setupPatchFailure=nullptr,
                   const NativeContextFailure* contextTable=nullptr)noexcept {
    struct KeepLastError {DWORD value=GetLastError();~KeepLastError(){SetLastError(value);}} keep;
    const auto thread=originalThread?originalThread:GetCurrentThreadId();
    if(draftLedger){
        if(!draftLedger->refused.load()){
            draftLedger->refusal=reason;draftLedger->refusalModule=module;
            draftLedger->refusalCaller=caller;draftLedger->refusalThread=thread;
            draftLedger->refusalHasImage=image!=nullptr;
            if(image)draftLedger->refusalImage=*image;if(trust)draftLedger->refusalTrust=*trust;
            draftLedger->refusalOrigin=origin?*origin:imageCollectionOrigin;
            if(setupPatchFailure)draftLedger->setupPatchFailure=*setupPatchFailure;
            draftLedger->refused.store(true,std::memory_order_release);
        }
        return;
    }
    // Fail closed immediately, but publish diagnostics only as one complete receipt.
    store.revoked.store(true,std::memory_order_release);
    unsigned absent=0;
    if(firstRevocationState.compare_exchange_strong(absent,1)){
        firstRevocation.file=caller.file_name();firstRevocation.function=caller.function_name();
        firstRevocation.line=caller.line();firstRevocation.thread=thread;
        firstRevocation.reason=reason;firstRevocation.module=module;firstRevocation.hasImage=image!=nullptr;
        if(image)firstRevocation.image=*image;if(trust)firstRevocation.trust=*trust;
        firstRevocation.origin=origin?*origin:imageCollectionOrigin;
        if(setupPatchFailure)firstRevocation.setupPatchFailure=*setupPatchFailure;
        if(contextTable){firstRevocation.contextTable=*contextTable;firstRevocation.hasContextTable=true;}
        firstRevocationState.store(2,std::memory_order_release);
    }
}
} // namespace detail
inline void ResumeSetup()noexcept;
namespace detail {
struct Guard {
    LiveLedgerScope live;bool held=false;
    Guard()noexcept {held=!store.busy.test_and_set(std::memory_order_acquire);}
    ~Guard(){if(held){store.busy.clear(std::memory_order_release);ResumeSetup();}}
    Guard(const Guard&)=delete;
};
inline int Owner(const void* pointer,size_t n=1)noexcept {
    const auto address=reinterpret_cast<uintptr_t>(pointer);
    for(unsigned i=0;i<Ledger().imageCount;++i){const auto base=reinterpret_cast<uintptr_t>(Ledger().images[i].live);
        if(address>=base && Span(address-base,n,Ledger().images[i].clean.size()))return static_cast<int>(i);
    }return -1;
}
inline bool Executable(const void* pointer)noexcept {
    const auto i=Owner(pointer);return i>=0 && Ledger().images[i].SectionHas(
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pointer)-reinterpret_cast<uintptr_t>(Ledger().images[i].live)),1,IMAGE_SCN_MEM_EXECUTE);
}
// Classify the code we actually edit, separately from the COM table storage.
// Other cores may put their method in a system/heap/writable table. We only
// borrow the held instance/table during installation; no COM lifetime is extended.
inline bool InputImageRange(const void* address,size_t size,HMODULE module,bool code)noexcept {
    const auto at=reinterpret_cast<uintptr_t>(address);MEMORY_BASIC_INFORMATION m{};
    if(!at || !size || at>UINTPTR_MAX-size ||
       VirtualQuery(address,&m,sizeof(m))!=sizeof(m) || m.State!=MEM_COMMIT ||
       m.Type!=MEM_IMAGE || m.AllocationBase!=module || (m.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
    const auto base=reinterpret_cast<uintptr_t>(m.BaseAddress);const auto protect=m.Protect&0xff;
    if(at<base || !Span(at-base,size,m.RegionSize))return false;
    return code?(protect==PAGE_EXECUTE || protect==PAGE_EXECUTE_READ ||
        protect==PAGE_EXECUTE_READWRITE || protect==PAGE_EXECUTE_WRITECOPY):protect==PAGE_READONLY;
}
inline bool InputMethodIndex(unsigned index)noexcept {return index==2 || index==3 || index==9 || index==10;}
inline bool SelectIndependentInputMethod(IUnknown* object,unsigned index,void* entry,IndependentInputMethod& result)noexcept {
    result={};if(!object || !InputMethodIndex(index) || !entry || Owner(entry)>=0)return false;
    void** table=nullptr;if(!Memory(object,&table,sizeof(table)) || !table)return false;
    const auto at=reinterpret_cast<uintptr_t>(table);
    if(at>UINTPTR_MAX-(index+1)*sizeof(void*))return false;
    const auto* slot=reinterpret_cast<const void*>(at+index*sizeof(void*));
    // This is an observed callable method, not an immutable native table proof.
    // The input patch changes code only; table contents grant no trust authority.
    MEMORY_BASIC_INFORMATION storage{};
    if(VirtualQuery(slot,&storage,sizeof(storage))!=sizeof(storage) || storage.State!=MEM_COMMIT ||
       (storage.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
       ((storage.Protect&0xff)!=PAGE_READONLY && (storage.Protect&0xff)!=PAGE_READWRITE &&
        (storage.Protect&0xff)!=PAGE_WRITECOPY))return false;
    void* current=nullptr;void** currentTable=nullptr;
    if(!Memory(slot,&current,sizeof(current)) || current!=entry ||
       !Memory(object,&currentTable,sizeof(currentTable)) || currentTable!=table)return false;
    HMODULE module=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(entry),&module) || !InputImageRange(entry,32,module,true))return false;
    wchar_t system[MAX_PATH]{},loaded[MAX_PATH]{};
    const auto sys=GetSystemDirectoryW(system,MAX_PATH);const auto len=GetModuleFileNameW(module,loaded,MAX_PATH);
    if(!sys || sys>=MAX_PATH || !len || len>=MAX_PATH)return false;
    if(!std::wcsrchr(loaded,L'\\') ||
       (!_wcsnicmp(system,loaded,sys) && (loaded[sys]==L'\\' || loaded[sys]==L'\0')))return false;
    // System code always takes the old complete authentication path, regardless
    // of who owns its table. Outside code is pinned only for patch lifetime.
    HMODULE pinned=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(entry),&pinned) || pinned!=module)return false;
    result={module,slot,entry,index};return true;
}
inline bool IndependentInputIntact(const Patch& p)noexcept {
    if(!p.input.module)return true;
    // The borrowed object/table may already have been released or rehooked.
    // Validate our entire code edit, trampoline and aliases in PatchIntact;
    // never keep a table pointer alive merely to validate unrelated code.
    return p.ownSetup && !p.declaredModule && !p.exportName && !p.hopCount && p.requested==p.entry &&
        p.input.selected==p.entry && p.input.tableSlot && InputMethodIndex(p.input.index) &&
        p.image==UINT_MAX && !p.rva && Owner(p.entry)<0 &&
        InputImageRange(p.entry,32,p.input.module,true);
}
inline bool ImportTable(Image& image); // Setup-only recursion, cycles bounded by inserted images.
inline bool AddImage(HMODULE module,const std::source_location& caller=std::source_location::current()) {
    ImageRequestScope request(module,caller);
    for(unsigned i=0;i<Ledger().imageCount;++i)if(Ledger().images[i].live==module)return true;
    if(Ledger().imageCount==Ledger().images.size()){Revoke("module-capacity",module);return false;}
    auto& image=Ledger().images[Ledger().imageCount];
    if(!image.Open(module)){
        Revoke("system-file-trust-or-PE",module,std::source_location::current(),&image.openObservation,&image.trustObservation);
        return false;
    }
    Ledger().imageBytes+=image.clean.size();if(Ledger().imageBytes>128*1024*1024){Revoke("image-byte-capacity",module);return false;}
    const auto index=Ledger().imageCount++;Ledger().imageCollection[index]=ImageCollection::Building;
    // A derived SCP target is a real code dependency, never an owner exception.
    // Insert first to break ntdll's self-cycle, then collect ALL source imports.
    if(const auto dependency=image.ScpDependency()){
        bool counted=false;
        for(unsigned i=0;i<index;++i)counted|=Ledger().images[i].Scp()==image.Scp();
        if(!counted)Ledger().imageBytes+=image.ScpReferenceBytes();
        if(Ledger().imageBytes>128*1024*1024 || !AddImage(dependency)){
            Ledger().imageCollection[index]=ImageCollection::Refused;Revoke("scp-source-closure",module);return false;
        }
    }
    // Returning an inserted image only breaks collection cycles. It publishes
    // neither a complete closure nor a new representation capability.
    if(!ImportTable(image)){Ledger().imageCollection[index]=ImageCollection::Refused;Revoke("import-or-delay-binding",module);return false;}
    Ledger().imageCollection[index]=ImageCollection::Complete;return true;
}
// Pure clean-image export decoding; neither live EAT nor GetProcAddress can
// select an address here. Deliberately bounded PE64 policy, not loader emulation.
struct ExportStep {
    DWORD index=0,rva=0;const char* forward=nullptr;ImportKind kind=ImportKind::None;
};
inline ImportKind ExportKind(const Image& image,DWORD rva)noexcept {
    if(!rva || !image.Range(rva,1))return ImportKind::None;
    for(unsigned i=0;i<image.sectionCount;++i){const auto& section=image.sections[i];
        if(rva<section.rva || !Span(rva-section.rva,1,section.size))continue;
        const auto flags=section.flags;
        if(flags&IMAGE_SCN_MEM_DISCARDABLE)return ImportKind::None;
        if(flags&IMAGE_SCN_MEM_EXECUTE)
            return (flags&IMAGE_SCN_MEM_WRITE)?ImportKind::None:ImportKind::CodeBinding;
        if((flags&IMAGE_SCN_MEM_READ) &&
           (flags&(IMAGE_SCN_CNT_INITIALIZED_DATA|IMAGE_SCN_CNT_UNINITIALIZED_DATA)))
            return ImportKind::DataBinding;
        return ImportKind::None;
    }return ImportKind::None;
}
inline bool CanonicalExportStepFull(const Image& image,const char* symbol,WORD ordinal,ExportStep& result)noexcept {
    result={};const auto d=image.exports;
    if(!d.VirtualAddress || d.Size<sizeof(IMAGE_EXPORT_DIRECTORY) || !image.Range(d.VirtualAddress,d.Size))return false;
    const auto ex=Read<IMAGE_EXPORT_DIRECTORY>(image.clean.data()+d.VirtualAddress);
    if(!ex.NumberOfFunctions || ex.NumberOfFunctions>65536 || ex.NumberOfNames>65536 ||
       !ex.AddressOfFunctions || !image.Range(ex.AddressOfFunctions,size_t(ex.NumberOfFunctions)*4) ||
       (ex.NumberOfNames && (!ex.AddressOfNames || !ex.AddressOfNameOrdinals ||
        !image.Range(ex.AddressOfNames,size_t(ex.NumberOfNames)*4) ||
        !image.Range(ex.AddressOfNameOrdinals,size_t(ex.NumberOfNames)*2))))return false;
    DWORD selected=UINT32_MAX;const char* previous=nullptr;
    // Validate the entire bounded name map, including ordinal range/order.
    // Duplicate names, malformed unrelated entries and ambiguous selection fail.
    for(DWORD i=0;i<ex.NumberOfNames;++i){
        const auto* name=image.String(Read<DWORD>(image.clean.data()+ex.AddressOfNames+4*i));
        const auto index=Read<WORD>(image.clean.data()+ex.AddressOfNameOrdinals+2*i);
        if(!name || !*name || index>=ex.NumberOfFunctions || (previous && std::strcmp(previous,name)>=0))return false;
        previous=name;if(symbol && !std::strcmp(symbol,name))selected=index;
    }
    if(!symbol){if(ordinal<ex.Base || uint64_t(ordinal)-ex.Base>=ex.NumberOfFunctions)return false;
        selected=static_cast<DWORD>(uint64_t(ordinal)-ex.Base);}
    if(selected==UINT32_MAX)return false;
    const auto rva=Read<DWORD>(image.clean.data()+ex.AddressOfFunctions+4*selected);
    if(!rva || !image.Range(rva,1))return false;
    result.index=selected;result.rva=rva;
    if(rva>=d.VirtualAddress && rva<uint64_t(d.VirtualAddress)+d.Size){
        const auto remaining=size_t(uint64_t(d.VirtualAddress)+d.Size-rva);
        if(!std::memchr(image.clean.data()+rva,0,(std::min)(size_t(512),remaining)))return false;
        result.forward=reinterpret_cast<const char*>(image.clean.data()+rva);return *result.forward!=0;
    }
    result.kind=ExportKind(image,rva);return result.kind!=ImportKind::None;
}
// The full name-map proof is performed once per exact frozen-image revision.
// Selected function RVA/kind/forwarder checks remain the original predicates.
inline bool BuildCleanExportPlan(const Image& image,CleanExportPlan& plan) {
    plan={};const auto d=image.exports;
    if(!d.VirtualAddress || d.Size<sizeof(IMAGE_EXPORT_DIRECTORY) || !image.Range(d.VirtualAddress,d.Size))return false;
    const auto ex=Read<IMAGE_EXPORT_DIRECTORY>(image.clean.data()+d.VirtualAddress);
    if(!ex.NumberOfFunctions || ex.NumberOfFunctions>65536 || ex.NumberOfNames>CleanExportPlan::capacity ||
       !ex.AddressOfFunctions || !image.Range(ex.AddressOfFunctions,size_t(ex.NumberOfFunctions)*4) ||
       (ex.NumberOfNames && (!ex.AddressOfNames || !ex.AddressOfNameOrdinals ||
        !image.Range(ex.AddressOfNames,size_t(ex.NumberOfNames)*4) ||
        !image.Range(ex.AddressOfNameOrdinals,size_t(ex.NumberOfNames)*2))))return false;
    plan.directory=ex;plan.names.reserve(ex.NumberOfNames);plan.runs.reserve(ex.NumberOfNames);
    const char* previous=nullptr;
    for(DWORD i=0;i<ex.NumberOfNames;++i){
        const auto rva=Read<DWORD>(image.clean.data()+ex.AddressOfNames+4*i);
        const auto* name=image.String(rva);
        const auto index=Read<WORD>(image.clean.data()+ex.AddressOfNameOrdinals+2*i);
        if(!name || !*name || index>=ex.NumberOfFunctions || (previous && std::strcmp(previous,name)>=0))return false;
        previous=name;const auto size=static_cast<DWORD>(std::strlen(name)+1);
        plan.names.push_back({rva,index,size});
        // Exact adjacency only: no holes, added byte obligations or sorting of
        // live checks. Each run preserves the original name-table read order.
        if(!plan.runs.empty() && uint64_t(plan.runs.back().rva)+plan.runs.back().size==rva &&
           size<=CleanExportPlan::readCapacity-plan.runs.back().size){
            plan.runs.back().size+=size;++plan.runs.back().count;
        }else plan.runs.push_back({rva,size,i,1});
    }return true;
}
inline CleanSlotRevision CleanExportKey(const Image& image)noexcept {
    const auto& l=Ledger();
    return {&l,&image,image.clean.data(),image.clean.size(),image.CleanRevision(),store.generation.load(),
        {l.imageCount,l.importCount,l.patchCount,l.transitionCount,l.runtimeFileCount,l.delayedCount,
         l.shim.edgeCount,l.shim.fileCount,static_cast<uint64_t>(l.shim.state),
         image.exports.VirtualAddress,image.exports.Size,image.sectionCount,reinterpret_cast<uintptr_t>(image.live)}};
}
struct CleanExportScope {
    const Image& image;const CleanExportPlan* plan=nullptr;bool locked=false;
    explicit CleanExportScope(const Image& value)noexcept:image(value) {
        // A plan contains no mutable import/delay bindings. Same-count target
        // transitions still execute every original live check at admission.
        if(draftLedger || store.initialized.load()!=2 || Revoked() || !image.CleanRevision())return;
        const auto& l=Ledger();if(l.imageCount>l.images.size())return;
        bool member=false;for(unsigned i=0;i<l.imageCount;++i)if(&l.images[i]==&image){member=true;break;}
        if(!member)return;
        const auto revision=CleanExportKey(image);if(revision.generation&1)return;
        if(image.exportPlanBusy.test_and_set(std::memory_order_acquire))return;
        locked=true;
        if(!(image.exportPlanRevision==revision)){
            image.exportPlanRevision={};
            try{
                CleanExportPlan next;if(!BuildCleanExportPlan(image,next))return;
                if(!(CleanExportKey(image)==revision) || Revoked())return;
                image.exportPlan=std::move(next);image.exportPlanRevision=revision;
            }catch(...){return;} // Allocation/unsupported shape: full fallback.
        }
        plan=&image.exportPlan;
    }
    CleanExportScope(const CleanExportScope&)=delete;
    CleanExportScope& operator=(const CleanExportScope&)=delete;
    ~CleanExportScope(){if(locked)image.exportPlanBusy.clear(std::memory_order_release);}
};
inline bool CanonicalExportStepPlanned(const Image& image,const CleanExportPlan& plan,
                                       const char* symbol,WORD ordinal,ExportStep& result)noexcept {
    result={};const auto& ex=plan.directory;DWORD selected=UINT32_MAX;
    if(symbol){
        const auto found=std::lower_bound(plan.names.begin(),plan.names.end(),symbol,[&](const auto& name,const char* text){
            return std::strcmp(reinterpret_cast<const char*>(image.clean.data()+name.rva),text)<0;
        });
        if(found!=plan.names.end() && !std::strcmp(reinterpret_cast<const char*>(image.clean.data()+found->rva),symbol))
            selected=found->index;
    }else{
        if(ordinal<ex.Base || uint64_t(ordinal)-ex.Base>=ex.NumberOfFunctions)return false;
        selected=static_cast<DWORD>(uint64_t(ordinal)-ex.Base);
    }
    if(selected==UINT32_MAX)return false;
    const auto rva=Read<DWORD>(image.clean.data()+ex.AddressOfFunctions+4*selected);
    if(!rva || !image.Range(rva,1))return false;
    result.index=selected;result.rva=rva;const auto d=image.exports;
    if(rva>=d.VirtualAddress && rva<uint64_t(d.VirtualAddress)+d.Size){
        const auto remaining=size_t(uint64_t(d.VirtualAddress)+d.Size-rva);
        if(!std::memchr(image.clean.data()+rva,0,(std::min)(size_t(512),remaining)))return false;
        result.forward=reinterpret_cast<const char*>(image.clean.data()+rva);return *result.forward!=0;
    }
    result.kind=ExportKind(image,rva);return result.kind!=ImportKind::None;
}
inline bool CanonicalExportStep(const Image& image,const char* symbol,WORD ordinal,ExportStep& result)noexcept {
    CleanExportScope scope(image);
    return scope.plan?CanonicalExportStepPlanned(image,*scope.plan,symbol,ordinal,result):
        CanonicalExportStepFull(image,symbol,ordinal,result);
}
// Read/consume one bounded run at a time. No live bytes survive this function,
// a Win32 call, a new run or an admission boundary. Region-query/read failures
// use the original per-name reads; a successful read with any mismatch refuses.
inline bool ExportNamesIntact(const Image& image,const CleanExportPlan& plan)noexcept {
    for(const auto& run:plan.runs){
        const auto* address=reinterpret_cast<const unsigned char*>(image.live)+run.rva;
        if(run.count>1){
            MEMORY_BASIC_INFORMATION region{};
            const auto at=reinterpret_cast<uintptr_t>(address);
            if(VirtualQuery(address,&region,sizeof(region))==sizeof(region) && region.State==MEM_COMMIT &&
               region.AllocationBase==image.live && !(region.Protect&(PAGE_GUARD|PAGE_NOACCESS)) &&
               (region.Protect&(PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READ|
                                PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)) &&
               at>=reinterpret_cast<uintptr_t>(region.BaseAddress) &&
               Span(at-reinterpret_cast<uintptr_t>(region.BaseAddress),run.size,region.RegionSize)){
                std::array<unsigned char,CleanExportPlan::readCapacity> bytes{};
                if(Memory(address,bytes.data(),run.size)){
                    if(std::memcmp(bytes.data(),image.clean.data()+run.rva,run.size))return false;
                    continue;
                }
            }
        }
        for(DWORD i=0;i<run.count;++i){const auto& name=plan.names[run.first+i];
            if(!Equal(reinterpret_cast<const unsigned char*>(image.live)+name.rva,
                      image.clean.data()+name.rva,name.size))return false;
        }
    }return true;
}
// R762: corroborate the address the imported slot ACTUALLY calls. A dynamic
// GetProcAddress shim is a different call path and supplies no authority here.
// Every byte of each live export directory must equal the signed clean image;
// each selected function/name/ordinal table cell is independently compared too.
inline bool CanonicalExportIntactFull(const Image& image,const char* symbol,WORD ordinal,
                                  const ExportStep& selected)noexcept {
    ExportStep clean{};if(!image.live || !CanonicalExportStepFull(image,symbol,ordinal,clean) ||
       clean.index!=selected.index || clean.rva!=selected.rva || clean.kind!=selected.kind ||
       (clean.forward==nullptr)!=(selected.forward==nullptr))return false;
    const auto d=image.exports;
    if(!Equal(reinterpret_cast<const unsigned char*>(image.live)+d.VirtualAddress,
              image.clean.data()+d.VirtualAddress,d.Size))return false;
    const auto ex=Read<IMAGE_EXPORT_DIRECTORY>(image.clean.data()+d.VirtualAddress);
    const auto equal=[&](DWORD at,size_t count){return image.Range(at,count) &&
        Equal(reinterpret_cast<const unsigned char*>(image.live)+at,image.clean.data()+at,count);};
    if(!equal(ex.AddressOfFunctions,size_t(ex.NumberOfFunctions)*4) ||
       !equal(ex.AddressOfNames,size_t(ex.NumberOfNames)*4) ||
       !equal(ex.AddressOfNameOrdinals,size_t(ex.NumberOfNames)*2))return false;
    for(DWORD i=0;i<ex.NumberOfNames;++i){
        const auto at=Read<DWORD>(image.clean.data()+ex.AddressOfNames+4*i);
        const auto* name=image.String(at);if(!name || !equal(at,std::strlen(name)+1))return false;
    }
    return !clean.forward || equal(clean.rva,std::strlen(clean.forward)+1);
}
inline bool CanonicalExportIntact(const Image& image,const char* symbol,WORD ordinal,
                                  const ExportStep& selected)noexcept {
    CleanExportScope scope(image);
    if(!scope.plan)return CanonicalExportIntactFull(image,symbol,ordinal,selected);
    ExportStep clean{};
    if(!image.live || !CanonicalExportStepPlanned(image,*scope.plan,symbol,ordinal,clean) ||
       clean.index!=selected.index || clean.rva!=selected.rva || clean.kind!=selected.kind ||
       (clean.forward==nullptr)!=(selected.forward==nullptr))return false;
    const auto d=image.exports;
    if(!Equal(reinterpret_cast<const unsigned char*>(image.live)+d.VirtualAddress,
              image.clean.data()+d.VirtualAddress,d.Size))return false;
    const auto& ex=scope.plan->directory;
    const auto equal=[&](DWORD at,size_t count){return image.Range(at,count) &&
        Equal(reinterpret_cast<const unsigned char*>(image.live)+at,image.clean.data()+at,count);};
    if(!equal(ex.AddressOfFunctions,size_t(ex.NumberOfFunctions)*4) ||
       !equal(ex.AddressOfNames,size_t(ex.NumberOfNames)*4) ||
       !equal(ex.AddressOfNameOrdinals,size_t(ex.NumberOfNames)*2))return false;
    return ExportNamesIntact(image,*scope.plan) && (!clean.forward || equal(clean.rva,std::strlen(clean.forward)+1));
}
inline bool ExactCanonicalIat(const ImportFailureContext* c,uintptr_t target)noexcept {
    if(!c || !c->slot || !c->importer || !c->actualKnown || c->actual!=target)return false;
    const auto owner=Owner(c->slot,sizeof(uintptr_t));uintptr_t current=0;
    return owner>=0 && Ledger().images[owner].live==c->importer &&
        Memory(c->slot,&current,sizeof(current)) && current==target;
}
#include "GenshinApiSetSchema.inl"
inline bool ImporterBasename(HMODULE importer,std::array<char,128>& result)noexcept {
    result.fill(0);const auto owner=Owner(importer);
    if(owner<0 || Ledger().images[owner].live!=importer)return false;
    wchar_t path[MAX_PATH]{};const auto count=GetModuleFileNameW(importer,path,MAX_PATH);
    if(!count || count>=MAX_PATH)return false;const auto* name=std::wcsrchr(path,L'\\');if(!name)return false;++name;
    size_t length=0;
    for(;length<127 && name[length];++length){if(name[length]>127)return false;result[length]=AsciiLower(static_cast<char>(name[length]));}
    return length<127 && BasenameToken(result.data());
}
// Literal hosts still require exact loaded basename + AddImage. Contracts
// select a host only through the fixed, authenticated clean schema and the
// authenticated importing image; a clean forwarder's own module supplies the
// importer context of its next schema edge (e.g. kernel32 alias -> kernelbase).
inline HMODULE CanonicalModule(const char* token,HMODULE observed=nullptr,HMODULE importer=nullptr) {
    if(!token)return nullptr;
    std::array<char,128> mapped{};
    const bool api=ApiContract(token);
    if(api){std::array<char,128> importingName{};
        if(!ImporterBasename(importer,importingName) || !apiSchema.Open() ||
           !apiSchema.Host(token,importingName.data(),mapped)){
            Revoke("canonical-apiset-schema-or-selection",importer);return nullptr;
        }
        token=mapped.data();
    }
    size_t length=0;while(length<127 && token[length])++length;
    if(!length || length==127)return nullptr;
    const char* dot=nullptr;
    for(size_t i=0;i<length;++i){const auto c=static_cast<unsigned char>(token[i]);
        if(c=='.'){if(!i || !token[i+1] || token[i+1]=='.')return nullptr;dot=token+i;}
        else if(!((c>='a' && c<='z') || (c>='A' && c<='Z') ||
                  (c>='0' && c<='9') || c=='_' || c=='-'))return nullptr;
    }
    if(dot && (dot==token || _stricmp(dot,".dll")))return nullptr;
    wchar_t wide[132]{};
    for(size_t i=0;i<length;++i)wide[i]=static_cast<unsigned char>(token[i]);
    if(!dot)std::memcpy(wide+length,L".dll",5*sizeof(wchar_t));
    const auto module=GetModuleHandleW(wide);if(!module || (observed && module!=observed))return nullptr;
    wchar_t path[MAX_PATH]{};const auto count=GetModuleFileNameW(module,path,MAX_PATH);
    if(!count || count>=MAX_PATH)return nullptr;
    const auto* basename=std::wcsrchr(path,L'\\');
    return basename && !_wcsicmp(basename+1,wide)?module:nullptr;
}
inline bool ForwardSelector(const char* text,char* module,size_t capacity,const char*& name,WORD& ordinal)noexcept {
    if(!text)return false;
    const auto* dot=std::strrchr(text,'.');
    if(!dot || dot==text || !dot[1] || size_t(dot-text)>=capacity)return false;
    const auto length=size_t(dot-text);std::memcpy(module,text,length);module[length]=0;
    name=dot+1;ordinal=0;
    if(*name=='#'){
        const auto* digit=name+1;if(!*digit)return false;unsigned value=0;
        for(;*digit;++digit){if(*digit<'0' || *digit>'9' || value>(65535u-unsigned(*digit-'0'))/10u)return false;
            value=value*10+unsigned(*digit-'0');}
        ordinal=static_cast<WORD>(value);name=nullptr;
    }return true;
}
// R753: finite representation of exact profiled import slots, never a generic
// export alias. Collection outcomes cannot be consumed as callable bindings.
enum class ImportResolution : unsigned char { Refused, Canonical, Represented, Deferred };
namespace file_binding {inline bool Intact()noexcept;inline bool Seal()noexcept;}
namespace shim_binding {
inline const shim_recipe::Recipe& Recipe()noexcept {
    return Ledger().shim.recipe==1?shim_recipe::System26100:shim_recipe::Legacy;
}
inline DWORD NativeRva()noexcept {return Recipe().families[2].nativeRva;}
struct Layout {
    uintptr_t apphelp=0,native=0,tableCell=0,table=0,rowAddress=0;
    uintptr_t allocation=0,region=0;size_t regionSize=0;
    DWORD type=0,state=0,protect=0;
    std::array<uintptr_t,6> row{};
    uintptr_t xfgCell=0,xfgTarget=0,cfCell=0,cfTarget=0;
    std::array<unsigned char,137> wrapper{};
    std::array<unsigned char,6> xfg{};std::array<unsigned char,2> cf{};
};
inline bool LayoutValid(const Layout& f)noexcept {
    const auto& p=Recipe();
    const auto appSize=Read<IMAGE_NT_HEADERS64>(p.app->nt.data()).OptionalHeader.SizeOfImage;
    const auto nativeSize=Read<IMAGE_NT_HEADERS64>(p.native->nt.data()).OptionalHeader.SizeOfImage;
    if(!f.apphelp || !f.native || f.apphelp>UINTPTR_MAX-appSize ||
       f.native>UINTPTR_MAX-nativeSize || f.tableCell!=f.apphelp+p.tableCell ||
       !f.table || (f.table&7) || f.table>UINTPTR_MAX-0x90 || f.rowAddress!=f.table+0x60 ||
       !f.allocation || f.region<f.allocation || f.rowAddress<f.region ||
       !Span(f.rowAddress-f.region,sizeof(f.row),f.regionSize) ||
       f.type!=MEM_PRIVATE || f.state!=MEM_COMMIT || (f.protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
    const auto protect=f.protect&0xff;
    if(protect!=PAGE_READONLY && protect!=PAGE_READWRITE && protect!=PAGE_WRITECOPY)return false;
    return p.wrapper.size<=f.wrapper.size() && p.xfg.size==f.xfg.size() && p.cf.size==f.cf.size() &&
        f.row[0]==f.apphelp+p.dllName && f.row[1]==f.apphelp+p.families[2].symbol &&
        f.row[2]==f.apphelp+p.wrapper.rva && f.row[3]==f.native+NativeRva() &&
        f.xfgCell==f.apphelp+p.xfgCell && f.xfgTarget==f.apphelp+p.xfg.rva &&
        f.cfCell==f.apphelp+p.cfCell && f.cfTarget==f.apphelp+p.cf.rva &&
        !std::memcmp(f.wrapper.data(),p.wrapper.bytes,p.wrapper.size) &&
        !std::memcmp(f.xfg.data(),p.xfg.bytes,p.xfg.size) && !std::memcmp(f.cf.data(),p.cf.bytes,p.cf.size);
    // q4/q5 remain opaque data. Neither is followed, called or certified.
}
inline bool ProfileImage(const Image& image,const shim_observation::Profile& p)noexcept {
    if(!image.live || !image.raw || image.fileSize!=p.fileSize ||
       !Span(0,p.dos.size(),image.fileSize) ||
       std::memcmp(image.raw,p.dos.data(),p.dos.size()))return false;
    const auto nt=Read<DWORD>(p.dos.data()+offsetof(IMAGE_DOS_HEADER,e_lfanew));
    if(!Span(nt,p.nt.size(),image.fileSize) || !Span(p.versionRaw,p.version.size(),image.fileSize) ||
       std::memcmp(image.raw+nt,p.nt.data(),p.nt.size()) ||
       std::memcmp(image.raw+p.versionRaw,p.version.data(),p.version.size()) ||
       !image.Range(p.versionRva,p.version.size()) ||
       std::memcmp(image.clean.data()+p.versionRva,p.version.data(),p.version.size()))return false;
    const auto header=Read<IMAGE_NT_HEADERS64>(image.raw+nt);
    return image.Range(0,header.OptionalHeader.SizeOfHeaders) &&
        Equal(image.live,image.clean.data(),header.OptionalHeader.SizeOfHeaders) &&
        Equal(reinterpret_cast<const unsigned char*>(image.live)+p.versionRva,p.version.data(),p.version.size());
}
inline bool ImageData(const Image& image,DWORD rva,size_t bytes)noexcept {
    if(!image.SectionHas(rva,bytes,IMAGE_SCN_MEM_READ,IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_DISCARDABLE))return false;
    const auto base=reinterpret_cast<uintptr_t>(image.live);if(rva>UINTPTR_MAX-base)return false;
    MEMORY_BASIC_INFORMATION m{};const auto at=base+rva;
    if(VirtualQuery(reinterpret_cast<const void*>(at),&m,sizeof(m))!=sizeof(m) ||
       m.Type!=MEM_IMAGE || m.State!=MEM_COMMIT || m.AllocationBase!=image.live ||
       (m.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
    const auto p=m.Protect&0xff;
    const auto region=reinterpret_cast<uintptr_t>(m.BaseAddress);
    return (p==PAGE_READONLY || p==PAGE_READWRITE || p==PAGE_WRITECOPY) &&
        at>=region && Span(at-region,bytes,m.RegionSize);
}
// Selection only chooses a finite candidate from an already authenticated,
// frozen native image. It grants no wrapper/call authority and cannot change
// a Building/Validated/Sealed receipt or use diagnostic post-refusal samples.
inline void PrepareRecipe(HMODULE native)noexcept {
    auto& s=Ledger().shim;
    if(!draftLedger || !observeSetupImports || store.initialized.load()!=1 || Revoked() ||
       s.state!=RepresentationState::Absent || RepresentationCount() || native!=GetModuleHandleW(L"ntdll.dll"))return;
    const auto ni=Owner(native);
    if(ni>=0 && ProfileImage(Ledger().images[ni],shim_recipe::Nt26100))s.recipe=1;
}
inline bool Pinned(const Image& image,const shim_recipe::Code& code)noexcept {
    return code.bytes && code.size && image.Range(code.rva,code.size) &&
        image.SectionHas(code.rva,code.size,IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE,
            IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_DISCARDABLE) &&
        !std::memcmp(image.clean.data()+code.rva,code.bytes,code.size) &&
        Equal(reinterpret_cast<const unsigned char*>(image.live)+code.rva,code.bytes,code.size);
}
// Only this recipe-named REL32 may use Open's signed, frozen SCP reference.
// Never normalize again, learn a live destination, or certify the data cell as
// callable. Both the original CFG slot and the whole SCP source remain proven.
inline bool DispatchIntact(const Image& image,const shim_recipe::Recipe& p)noexcept {
    const auto& code=p.dispatch;const auto& original=p.dispatchOriginal;
    if(!code.rva)return true;
    if(!image.CleanRevision() || code.size!=5 || code.bytes[0]!=0xe9 ||
       int64_t(code.rva)+5+Read<int32_t>(code.bytes+1)!=original.rva || !Pinned(image,original) ||
       scp::OriginalRole(image,original.rva)!=scp::Role::Dispatch ||
       !image.Range(code.rva,code.size) || !image.SectionHas(code.rva,code.size,
           IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE,IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_DISCARDABLE))return false;
    const Image::DynamicFixup* selected=nullptr;
    for(const auto& fix:image.dynamicFixups)if(fix.rva==code.rva+1){if(selected)return false;selected=&fix;}
    if(!selected || selected->original!=original.rva || selected->alternatives.size()!=1 ||
       selected->alternatives[0]!=original.rva)return false;
    const auto* sealed=image.clean.data()+code.rva;
    if(sealed[0]!=code.bytes[0])return false;
    if(std::memcmp(sealed,code.bytes,code.size)){
        const auto& source=image.Scp();
        if(!source || !ScpValidationPass::Check(*source))return false;
        const auto base=reinterpret_cast<uintptr_t>(image.live);
        const int64_t rva=int64_t(code.rva)+5+Read<int32_t>(sealed+1);uintptr_t target=0,derived=0;
        if(rva>=0){if(uint64_t(rva)>UINTPTR_MAX-base)return false;target=base+static_cast<uintptr_t>(rva);}
        else {if(uint64_t(-rva)>base)return false;target=base-static_cast<uintptr_t>(-rva);}
        if(!source->Destination(scp::Role::Dispatch,target,derived) || target!=derived)return false;
    }
    return Equal(reinterpret_cast<const unsigned char*>(image.live)+code.rva,sealed,code.size);
}
inline bool ReadLayout(HMODULE apphelp,HMODULE native,Layout& f)noexcept {
    f={};const auto ai=Owner(apphelp),ni=Owner(native);if(ai<0 || ni<0)return false;
    const auto& a=Ledger().images[ai];const auto& n=Ledger().images[ni];const auto& p=Recipe();
    if(!ProfileImage(a,*p.app) || !ProfileImage(n,*p.native) ||
       !ImageData(a,p.tableCell,8) || !ImageData(a,p.xfgCell,8) || !ImageData(a,p.cfCell,8) ||
       !ImageData(a,p.loadConfig+0x120,8) || !ImageData(a,p.loadConfig+0x78,8) ||
       !Pinned(a,p.wrapper) || !Pinned(a,p.xfg) || !Pinned(a,p.cf) ||
       (p.dispatch.rva && (!Pinned(a,p.initializer) || !DispatchIntact(a,p))))return false;
    f.apphelp=reinterpret_cast<uintptr_t>(apphelp);f.native=reinterpret_cast<uintptr_t>(native);
    const auto appSize=Read<IMAGE_NT_HEADERS64>(p.app->nt.data()).OptionalHeader.SizeOfImage;
    const auto nativeSize=Read<IMAGE_NT_HEADERS64>(p.native->nt.data()).OptionalHeader.SizeOfImage;
    if(f.apphelp>UINTPTR_MAX-appSize || f.native>UINTPTR_MAX-nativeSize)return false;
    // Relocated clean load-config cells select these exact data cells.
    if(Read<uint64_t>(a.clean.data()+p.loadConfig+0x120)!=f.apphelp+p.xfgCell ||
       Read<uint64_t>(a.clean.data()+p.loadConfig+0x78)!=f.apphelp+p.cfCell ||
       !Equal(reinterpret_cast<const void*>(f.apphelp+p.loadConfig+0x120),a.clean.data()+p.loadConfig+0x120,8) ||
       !Equal(reinterpret_cast<const void*>(f.apphelp+p.loadConfig+0x78),a.clean.data()+p.loadConfig+0x78,8))return false;
    f.tableCell=f.apphelp+p.tableCell;f.xfgCell=f.apphelp+p.xfgCell;f.cfCell=f.apphelp+p.cfCell;
    if(!Memory(reinterpret_cast<const void*>(f.tableCell),&f.table,8) || !f.table ||
       (f.table&7) || f.table>UINTPTR_MAX-0x90)return false;
    f.rowAddress=f.table+0x60;MEMORY_BASIC_INFORMATION m{};
    if(VirtualQuery(reinterpret_cast<const void*>(f.rowAddress),&m,sizeof(m))!=sizeof(m))return false;
    f.allocation=reinterpret_cast<uintptr_t>(m.AllocationBase);f.region=reinterpret_cast<uintptr_t>(m.BaseAddress);
    f.regionSize=m.RegionSize;f.type=m.Type;f.state=m.State;f.protect=m.Protect;
    const auto protect=m.Protect&0xff;
    if(m.Type!=MEM_PRIVATE || m.State!=MEM_COMMIT || (m.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
       (protect!=PAGE_READONLY && protect!=PAGE_READWRITE && protect!=PAGE_WRITECOPY) ||
       f.rowAddress<f.region || !Span(f.rowAddress-f.region,sizeof(f.row),m.RegionSize) ||
       p.wrapper.size>f.wrapper.size())return false;
    return Memory(reinterpret_cast<const void*>(f.rowAddress),f.row.data(),sizeof(f.row)) &&
        Memory(reinterpret_cast<const void*>(f.xfgCell),&f.xfgTarget,8) &&
        Memory(reinterpret_cast<const void*>(f.cfCell),&f.cfTarget,8) &&
        Memory(reinterpret_cast<const void*>(f.apphelp+p.wrapper.rva),f.wrapper.data(),p.wrapper.size) &&
        Memory(reinterpret_cast<const void*>(f.apphelp+p.xfg.rva),f.xfg.data(),f.xfg.size()) &&
        Memory(reinterpret_cast<const void*>(f.apphelp+p.cf.rva),f.cf.data(),f.cf.size()) && LayoutValid(f);
}
inline const char* RoleName(ShimRole role)noexcept {
    return role==ShimRole::SelfAlias || role==ShimRole::BaseAlias?"ZwSetValueKey":"NtSetValueKey";
}
inline DWORD RoleRva(ShimRole role)noexcept {
    const auto i=static_cast<unsigned>(role);return i<Recipe().roles.size()?Recipe().roles[i]:0;
}
inline HMODULE RoleModule(ShimRole role,HMODULE apphelp)noexcept {
    if(role==ShimRole::Outer)return GetModuleHandleW(L"kernel32.dll");
    if(role==ShimRole::Self || role==ShimRole::SelfAlias)return apphelp;
    if(role==ShimRole::BaseNt || role==ShimRole::BaseAlias)return GetModuleHandleW(L"kernelbase.dll");
    return nullptr;
}
// Independent clean EAT proof: both names, two distinct cells, identical code
// RVA, and NO third named or ordinal-only alias at that RVA. Never consult GPA.
inline bool AliasFacts(const Image& image)noexcept {
    ExportStep nt{},zw{};
    if(!ProfileImage(image,*Recipe().native) ||
       !CanonicalExportStep(image,"NtSetValueKey",0,nt) ||
       !CanonicalExportStep(image,"ZwSetValueKey",0,zw) ||
       nt.forward || zw.forward || nt.kind!=ImportKind::CodeBinding || zw.kind!=nt.kind ||
       nt.rva!=NativeRva() || zw.rva!=nt.rva || nt.index!=Recipe().families[2].ntIndex || zw.index!=Recipe().families[2].zwIndex)return false;
    const auto ex=Read<IMAGE_EXPORT_DIRECTORY>(image.clean.data()+image.exports.VirtualAddress);
    if(ex.Base!=8)return false;
    unsigned cells=0,names=0;
    for(DWORD i=0;i<ex.NumberOfFunctions;++i)
        if(Read<DWORD>(image.clean.data()+ex.AddressOfFunctions+4*i)==nt.rva){
            if(i!=nt.index && i!=zw.index)return false;++cells;
        }
    for(DWORD i=0;i<ex.NumberOfNames;++i){
        const auto index=Read<WORD>(image.clean.data()+ex.AddressOfNameOrdinals+2*i);
        if(index!=nt.index && index!=zw.index)continue;
        const auto* name=image.String(Read<DWORD>(image.clean.data()+ex.AddressOfNames+4*i));
        if(!name || std::strcmp(name,index==nt.index?"NtSetValueKey":"ZwSetValueKey"))return false;
        ++names;
    }return cells==2 && names==2;
}
// Full reference/fallback parser of the COMPLETE clean descriptor/slot tables.
// A selector comes from signed INT bytes, never the mutable IAT contents.
// Other Nt/Zw imports are still traversed by ImportTable with canonical-only
// policy; this function grants none of them representation authority.
inline bool CleanSelectedSlotFull(const Image& image,DWORD expected,const char* wanted,const char* wantedDll="ntdll.dll",bool allowResolvedDelay=false)noexcept {
    if(!expected || !wanted || !wantedDll || !image.SectionHas(expected,8,IMAGE_SCN_MEM_READ,IMAGE_SCN_MEM_EXECUTE))return false;
    unsigned matches=0;
    for(unsigned delay=0;delay<2;++delay){const auto d=delay?image.delays:image.imports;
        if(!d.Size)continue;
        if(!image.Range(d.VirtualAddress,d.Size))return false;
        const size_t stride=delay?32:sizeof(IMAGE_IMPORT_DESCRIPTOR);bool ended=false;
        for(size_t at=0;Span(at,stride,d.Size);at+=stride){
            const auto* desc=image.clean.data()+d.VirtualAddress+at;
            const std::array<unsigned char,32> zero{};
            if(!std::memcmp(desc,zero.data(),stride)){ended=true;break;}
            if(delay && Read<DWORD>(desc)!=1)return false;
            const auto nameRva=Read<DWORD>(desc+(delay?4:12));
            const auto names=Read<DWORD>(desc+(delay?16:0)),iat=Read<DWORD>(desc+(delay?12:16));
            const auto* dll=image.String(nameRva);if(!dll || !names || !iat)return false;
            bool slotsEnded=false;
            for(unsigned j=0;j<8192;++j){const uint64_t source=uint64_t(names)+8ull*j,dest=uint64_t(iat)+8ull*j;
                if(source>UINT32_MAX || dest>UINT32_MAX || !image.Range(static_cast<DWORD>(source),8) ||
                   !image.Range(static_cast<DWORD>(dest),8))return false;
                const auto thunk=Read<uint64_t>(image.clean.data()+source);
                if(!thunk){slotsEnded=true;break;}
                const char* name=nullptr;
                if(thunk&IMAGE_ORDINAL_FLAG64){if(thunk&~(IMAGE_ORDINAL_FLAG64|uint64_t(0xffff)))return false;}
                else {if(thunk>UINT32_MAX-2)return false;name=image.String(static_cast<DWORD>(thunk)+2);if(!name)return false;}
                if(dest==expected){
                    if((delay && !allowResolvedDelay) || _stricmp(dll,wantedDll) || !name || std::strcmp(name,wanted))return false;
                    ++matches;
                }
                // No duplicate matching name in this importer's signed graph.
                if(!delay && !_stricmp(dll,wantedDll) && name && !std::strcmp(name,wanted) && dest!=expected)
                    return false;
            }if(!slotsEnded)return false;
        }if(!ended)return false;
    }return matches==1;
}
// Build once from the exact frozen clean revision. Failure/capacity/allocation
// never changes admission: the caller uses the full parser instead. No runtime
// Memory/Equal/VirtualQuery/GPA, IAT value, alias, scope or ownership is recorded.
inline bool BuildCleanSlotPlan(const Image& image,CleanSlotPlan& plan) {
    plan.slots.clear();plan.regularNames.clear();
    for(unsigned delay=0;delay<2;++delay){const auto d=delay?image.delays:image.imports;
        if(!d.Size)continue;
        if(!image.Range(d.VirtualAddress,d.Size))return false;
        const size_t stride=delay?32:sizeof(IMAGE_IMPORT_DESCRIPTOR);bool ended=false;
        for(size_t at=0;Span(at,stride,d.Size);at+=stride){
            const auto* desc=image.clean.data()+d.VirtualAddress+at;
            const std::array<unsigned char,32> zero{};
            if(!std::memcmp(desc,zero.data(),stride)){ended=true;break;}
            if(delay && Read<DWORD>(desc)!=1)return false;
            const auto nameRva=Read<DWORD>(desc+(delay?4:12));
            const auto names=Read<DWORD>(desc+(delay?16:0)),iat=Read<DWORD>(desc+(delay?12:16));
            if(!image.String(nameRva) || !names || !iat)return false;
            bool slotsEnded=false;
            for(unsigned j=0;j<8192;++j){const uint64_t source=uint64_t(names)+8ull*j,dest=uint64_t(iat)+8ull*j;
                if(source>UINT32_MAX || dest>UINT32_MAX || !image.Range(static_cast<DWORD>(source),8) ||
                   !image.Range(static_cast<DWORD>(dest),8))return false;
                const auto thunk=Read<uint64_t>(image.clean.data()+source);
                if(!thunk){slotsEnded=true;break;}
                DWORD name=0;
                if(thunk&IMAGE_ORDINAL_FLAG64){if(thunk&~(IMAGE_ORDINAL_FLAG64|uint64_t(0xffff)))return false;}
                else {if(thunk>UINT32_MAX-2)return false;name=static_cast<DWORD>(thunk)+2;if(!image.String(name))return false;}
                if(plan.slots.size()==CleanSlotPlan::capacity)return false;
                plan.slots.push_back({static_cast<DWORD>(dest),nameRva,name,delay!=0});
            }if(!slotsEnded)return false;
        }if(!ended)return false;
    }
    std::sort(plan.slots.begin(),plan.slots.end(),[](const auto& a,const auto& b){return a.dest<b.dest;});
    for(unsigned i=0;i<plan.slots.size();++i)if(!plan.slots[i].delay && plan.slots[i].name)plan.regularNames.push_back(i);
    const auto text=[&](DWORD at){return reinterpret_cast<const char*>(image.clean.data()+at);};
    std::sort(plan.regularNames.begin(),plan.regularNames.end(),[&](unsigned a,unsigned b){
        const auto& x=plan.slots[a];const auto& y=plan.slots[b];
        const auto name=std::strcmp(text(x.name),text(y.name));
        return name?name<0:x.dest<y.dest;
    });return true;
}
// Requires a complete plan from this unchanged clean revision; never callable
// proof. Preserve the full parser's regular-only duplicate-name rule, including
// overlapping destinations, ordinal rejection and optional resolved delay slots.
inline bool CleanSelectedSlotPlanned(const Image& image,const CleanSlotPlan& plan,DWORD expected,
                                    const char* wanted,const char* wantedDll,bool allowResolvedDelay)noexcept {
    if(!expected || !wanted || !wantedDll || !image.SectionHas(expected,8,IMAGE_SCN_MEM_READ,IMAGE_SCN_MEM_EXECUTE))return false;
    const auto text=[&](DWORD at){return reinterpret_cast<const char*>(image.clean.data()+at);};
    auto at=std::lower_bound(plan.slots.begin(),plan.slots.end(),expected,[](const auto& slot,DWORD dest){return slot.dest<dest;});
    unsigned matches=0;
    for(;at!=plan.slots.end() && at->dest==expected;++at){
        if((at->delay && !allowResolvedDelay) || _stricmp(text(at->dll),wantedDll) || !at->name || std::strcmp(text(at->name),wanted))return false;
        ++matches;
    }if(matches!=1)return false;
    // Symbol ordering is locale-independent. DLL equality deliberately uses
    // the original current-locale comparator afresh for every matching symbol.
    const auto compare=[&](unsigned index){return std::strcmp(text(plan.slots[index].name),wanted);};
    const auto first=std::lower_bound(plan.regularNames.begin(),plan.regularNames.end(),0,
        [&](unsigned index,int){return compare(index)<0;});
    for(auto i=first;i!=plan.regularNames.end() && !compare(*i);++i){
        const auto& slot=plan.slots[*i];
        if(slot.dest!=expected && !_stricmp(text(slot.dll),wantedDll))return false;
    }
    return true;
}
inline CleanSlotRevision CleanSlotKey(const Image& image)noexcept {
    const auto& l=Ledger();
    return {&l,&image,image.clean.data(),image.clean.size(),image.CleanRevision(),store.generation.load(),
        {l.imageCount,l.importCount,l.patchCount,l.transitionCount,l.runtimeFileCount,l.delayedCount,
         l.shim.edgeCount,l.shim.fileCount,static_cast<uint64_t>(l.shim.state),
         image.imports.VirtualAddress,image.imports.Size,image.delays.VirtualAddress,image.delays.Size}};
}
inline bool CleanSelectedSlot(const Image& image,DWORD expected,const char* wanted,const char* wantedDll="ntdll.dll",bool allowResolvedDelay=false)noexcept {
    if(!expected || !wanted || !wantedDll || !image.SectionHas(expected,8,IMAGE_SCN_MEM_READ,IMAGE_SCN_MEM_EXECUTE))return false;
    // Drafts (including adversarial clean-byte fixtures) retain the full path.
    // Published image storage survives for the process lifetime. No replacement
    // or copy of its clean vector occurs after Open. Existing caller guards and
    // before/after epoch checks remain responsible for all runtime admission.
    const auto full=[&](){return CleanSelectedSlotFull(image,expected,wanted,wantedDll,allowResolvedDelay);};
    if(draftLedger || store.initialized.load()!=2 || !image.CleanRevision())return full();
    const auto& l=Ledger();bool member=false;
    if(l.imageCount>l.images.size())return full();
    for(unsigned i=0;i<l.imageCount;++i)if(&l.images[i]==&image){member=true;break;}
    if(!member)return full(); // Not an ownership or binding receipt.
    const auto revision=CleanSlotKey(image);
    if(revision.generation&1)return full();
    // Never wait/reenter a plan lock. Contention runs the unchanged full parser.
    if(image.cleanPlanBusy.test_and_set(std::memory_order_acquire))return full();
    struct Release {std::atomic_flag& flag;~Release(){flag.clear(std::memory_order_release);}} release{image.cleanPlanBusy};
    if(!(image.cleanPlanRevision==revision)){
        image.cleanPlanRevision={};
        try{if(!BuildCleanSlotPlan(image,image.cleanPlan))return full();}catch(...){return full();}
        if(!(CleanSlotKey(image)==revision))return full();
        image.cleanPlanRevision=revision;
    }
    return CleanSelectedSlotPlanned(image,image.cleanPlan,expected,wanted,wantedDll,allowResolvedDelay);
}
inline bool CleanRoleSlot(const Image& image,ShimRole role)noexcept {
    return CleanSelectedSlot(image,RoleRva(role),RoleName(role));
}
inline bool SelectRole(const char* dll,const char* name,WORD ordinal,HMODULE importer,const void* slot,
                       HMODULE apphelp,ShimRole& selected)noexcept {
    if(!dll || !name || ordinal || !importer)return false;
    const auto base=reinterpret_cast<uintptr_t>(importer),at=reinterpret_cast<uintptr_t>(slot);
    if(at<base)return false;
    // R834: reject unrelated imports by their exact fixed slot before any
    // string comparison, module resolution or Owner scan. A matching offset
    // still requires every original DLL/name/module/owner predicate.
    const auto offset=at-base;ShimRole role{};
    if(offset==RoleRva(ShimRole::Outer))role=ShimRole::Outer;
    else if(offset==RoleRva(ShimRole::Self))role=ShimRole::Self;
    else if(offset==RoleRva(ShimRole::SelfAlias))role=ShimRole::SelfAlias;
    else if(offset==RoleRva(ShimRole::BaseNt))role=ShimRole::BaseNt;
    else if(offset==RoleRva(ShimRole::BaseAlias))role=ShimRole::BaseAlias;
    else return false;
    if(importer!=RoleModule(role,apphelp) || _stricmp(dll,"ntdll.dll") ||
       std::strcmp(name,RoleName(role)) || Owner(slot,8)<0)return false;
    selected=role;return true;
}
inline bool Selector(const char* dll,const char* name,WORD ordinal,HMODULE importer,const void* slot,
                     const CanonicalBinding& canonical,HMODULE apphelp,HMODULE native)noexcept {
    ShimRole role{};
    if(canonical.kind!=ImportKind::CodeBinding || canonical.owner!=native || canonical.rva!=NativeRva() ||
       !native || native!=GetModuleHandleW(L"ntdll.dll") || !apphelp || apphelp!=GetModuleHandleW(L"apphelp.dll") ||
       !SelectRole(dll,name,ordinal,importer,slot,apphelp,role))return false;
    const auto owner=Owner(importer);return owner>=0 && CleanRoleSlot(Ledger().images[owner],role);
}
inline bool ObligationInventory(HMODULE apphelp,HMODULE native)noexcept {
    const auto ni=Owner(native);if(ni<0 || !AliasFacts(Ledger().images[ni]))return false;
    // All five mandatory roles must exist independently in clean signed tables.
    for(unsigned i=0;i<5;++i){const auto role=static_cast<ShimRole>(i);
        const auto module=RoleModule(role,apphelp);const auto owner=Owner(module);
        if(!module || owner<0 || !CleanRoleSlot(Ledger().images[owner],role))return false;
    }return true;
}
inline bool RoleSet(std::array<RoleForm,5>& roles)noexcept {
    const auto& s=Ledger().shim;roles={};unsigned mask=0;
    if(s.edgeCount<3 || s.edgeCount>s.edges.size())return false;
    for(unsigned i=0;i<s.edgeCount;++i){const auto& e=s.edges[i];ShimRole role{};
        const CanonicalBinding binding{e.canonicalOwner,e.canonicalRva,e.canonicalKind};
        if(!SelectRole(e.dll,e.name,e.ordinal,e.importer,e.slot,s.apphelp,role) || role!=e.role ||
           !RepresentedForm(role,e.form) || !Selector(e.dll,e.name,e.ordinal,e.importer,e.slot,binding,s.apphelp,s.native))return false;
        const auto index=static_cast<unsigned>(role),bit=1u<<index;
        if(mask&bit)return false;mask|=bit;roles[index]={role,e.form};
    }
    // Only the two Base obligations can be supplied by ordinary canonical
    // receipts. Read the real IAT and GPA; never predict Base GPA observations.
    for(unsigned i=0;i<Ledger().importCount;++i){const auto& e=Ledger().imports[i];ShimRole role{};
        if(!SelectRole(e.dll,e.name,e.ordinal,e.importer,e.slot,s.apphelp,role))continue;
        if(role!=ShimRole::BaseNt && role!=ShimRole::BaseAlias)return false;
        const auto index=static_cast<unsigned>(role),bit=1u<<index;
        const auto form=role==ShimRole::BaseNt?ShimForm::BaseNtNative:ShimForm::BaseAliasNative;
        uintptr_t now=0;
        if((mask&bit) || e.lazy || !Selector(e.dll,e.name,e.ordinal,e.importer,e.slot,e.binding,s.apphelp,s.native) ||
           !Memory(e.slot,&now,sizeof(now)) || now!=e.target ||
           !RoleFormValid(role,form,now,reinterpret_cast<uintptr_t>(GetProcAddress(s.native,RoleName(role))),
               reinterpret_cast<uintptr_t>(s.native)+NativeRva(),reinterpret_cast<uintptr_t>(s.apphelp)+Recipe().wrapper.rva))return false;
        mask|=bit;roles[index]={role,form};
    }return mask==31 && RoleSetValid(roles.data(),roles.size());
}
inline bool ProfileIntact(bool draft)noexcept {
    const auto& s=Ledger().shim;
    if(s.state==RepresentationState::Absent)return !RepresentationCount();
    if((draft?s.state!=RepresentationState::Building:
        (s.state!=RepresentationState::Validated && s.state!=RepresentationState::Sealed)) ||
       s.edgeCount<3 || s.edgeCount>s.edges.size())return false;
    Layout f{};
    if(!ReadLayout(s.apphelp,s.native,f) || f.table!=s.table || f.allocation!=s.allocation ||
       f.region!=s.region || f.regionSize!=s.regionSize || f.protect!=s.protect)return false;
    if(!ObligationInventory(s.apphelp,s.native))return false;
    std::array<RoleForm,5> roles{};bool rolesIntact=false;
    {GenshinCpuStageTiming::Scope cost(GenshinCpuStageTiming::Stage::RoleSet);rolesIntact=RoleSet(roles);}
    if(!rolesIntact)return false;
    for(unsigned i=0;i<s.edgeCount;++i){const auto& e=s.edges[i];uintptr_t now=0;
        CanonicalBinding canonical{e.canonicalOwner,e.canonicalRva,e.canonicalKind};
        if(!Selector(e.dll,e.name,e.ordinal,e.importer,e.slot,canonical,s.apphelp,s.native) ||
           e.canonicalOwner!=s.native || e.canonicalRva!=NativeRva() || e.canonicalKind!=ImportKind::CodeBinding ||
           !RoleFormValid(e.role,e.form,e.iatTarget,e.resolverTarget,
               reinterpret_cast<uintptr_t>(s.native)+NativeRva(),reinterpret_cast<uintptr_t>(s.apphelp)+Recipe().wrapper.rva) ||
           !Memory(e.slot,&now,sizeof(now)) || now!=e.iatTarget ||
           reinterpret_cast<uintptr_t>(GetProcAddress(s.native,e.name))!=e.resolverTarget)return false;
    }
    if(!RoleSetValid(roles.data(),roles.size()))return false;
    return true;
}
inline bool Intact()noexcept {return ProfileIntact(false) && file_binding::Intact();}
inline bool DraftIntact()noexcept {return ProfileIntact(true) && file_binding::Intact();}
inline ImportResolution Resolve(const ImportFailureContext& c,const CanonicalBinding& canonical,
                                uintptr_t resolver,bool collect) {
    auto& s=Ledger().shim;const auto apphelp=GetModuleHandleW(L"apphelp.dll");
    const auto native=GetModuleHandleW(L"ntdll.dll");
    if(Revoked() || !c.actualKnown || !Selector(c.dll,c.symbol,c.ordinal,c.importer,c.slot,canonical,apphelp,native))return ImportResolution::Refused;
    const auto a=reinterpret_cast<uintptr_t>(apphelp),n=reinterpret_cast<uintptr_t>(native);
    if(a>UINTPTR_MAX-Recipe().wrapper.rva || n>UINTPTR_MAX-NativeRva() || resolver!=a+Recipe().wrapper.rva)return ImportResolution::Refused;
    ShimRole role{};
    if(!SelectRole(c.dll,c.symbol,c.ordinal,c.importer,c.slot,apphelp,role))return ImportResolution::Refused;
    const bool canonicalIat=c.actual==n+NativeRva();
    ShimForm form=ShimForm::OuterRepresented;
    switch(role){
    case ShimRole::Outer:break;
    case ShimRole::Self:form=canonicalIat?ShimForm::SelfCanonical:ShimForm::SelfRepresented;break;
    case ShimRole::SelfAlias:form=canonicalIat?ShimForm::SelfAliasCanonical:ShimForm::SelfAliasRepresented;break;
    case ShimRole::BaseNt:form=canonicalIat?ShimForm::BaseNtCanonical:ShimForm::BaseNtRepresented;break;
    case ShimRole::BaseAlias:form=canonicalIat?ShimForm::BaseAliasCanonical:ShimForm::BaseAliasRepresented;break;
    }
    if(!RepresentedForm(role,form))return ImportResolution::Refused;
    if(!RoleFormValid(role,form,c.actual,resolver,n+NativeRva(),a+Recipe().wrapper.rva))return ImportResolution::Refused;
    HMODULE actualOwner=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(c.actual),&actualOwner) || actualOwner!=(c.actual==resolver?apphelp:native))return ImportResolution::Refused;
    if(s.state==RepresentationState::Sealed){
        if(!Intact())return ImportResolution::Refused;
        for(unsigned i=0;i<s.edgeCount;++i){const auto& e=s.edges[i];
            if(e.importer==c.importer && e.slot==c.slot && e.iatTarget==c.actual &&
               e.role==role && e.form==form && e.ordinal==c.ordinal &&
               e.dll && c.dll && !_stricmp(e.dll,c.dll) && e.name && c.symbol && !std::strcmp(e.name,c.symbol) &&
               canonical.owner==native && canonical.rva==NativeRva() &&
               canonical.kind==ImportKind::CodeBinding && e.canonicalOwner==canonical.owner &&
               e.canonicalRva==canonical.rva && e.canonicalKind==canonical.kind)
                return ImportResolution::Represented;
        }return ImportResolution::Refused;
    }
    // Collection is confined to the first winning Setup while no factory can
    // be certified. Strict resolver/call sites never see Deferred as success.
    if(!collect || !observeSetupImports || store.initialized.load()!=1 ||
       s.state==RepresentationState::Refused || s.edgeCount==s.edges.size() ||
       Ledger().importCount+RepresentationCount()>=Ledger().imports.size())return ImportResolution::Refused;
    if(s.state==RepresentationState::Absent){s.state=RepresentationState::Building;s.apphelp=apphelp;s.native=native;}
    if(s.state!=RepresentationState::Building || s.apphelp!=apphelp || s.native!=native)return ImportResolution::Refused;
    for(unsigned i=0;i<s.edgeCount;++i)if(s.edges[i].slot==c.slot || s.edges[i].role==role)return ImportResolution::Refused;
    RepresentedEdge e{};e.role=role;e.form=form;e.slot=c.slot;e.iatTarget=c.actual;e.resolverTarget=resolver;
    e.dll=c.dll;e.name=c.symbol;e.ordinal=c.ordinal;e.importer=c.importer;
    e.canonicalOwner=canonical.owner;e.canonicalRva=canonical.rva;e.canonicalKind=canonical.kind;s.edges[s.edgeCount++]=e;
    // Insertion-before-recursion is only graph collection. Apphelp self-import
    // is another staged edge; it cannot borrow the pending representation.
    if(!AddImage(apphelp))return ImportResolution::Refused;
    return ImportResolution::Deferred;
}
} // namespace shim_binding
// R774: exact committed transactions may overlay the signed wrapper preimage.
// Same-module destinations, observed branch chains and unregistered bytes grant
// no authority. This also runs while a private setup draft is collecting images,
// before IndexPatch publishes final image indices.
inline bool PatchIntact(const Patch& p)noexcept;
inline bool OwnedWrapperCode(const Image& image,DWORD rva,size_t size)noexcept {
    std::array<unsigned char,64> expected{};std::array<bool,64> covered{};
    if(!size || size>expected.size() || !image.Range(rva,size) ||
       Ledger().patchCount>Ledger().patches.size())return false;
    std::memcpy(expected.data(),image.clean.data()+rva,size);
    const auto base=reinterpret_cast<uintptr_t>(image.live);
    if(base>UINTPTR_MAX-rva || base+rva>UINTPTR_MAX-size)return false;
    const auto first=base+rva,last=first+size;
    for(unsigned i=0;i<Ledger().patchCount;++i){const auto& p=Ledger().patches[i];
        const auto entry=reinterpret_cast<uintptr_t>(p.entry);
        if(entry>UINTPTR_MAX-p.size)return false;
        if(entry>=last || entry+p.size<=first)continue;
        if(p.input.module || !p.ownSetup || p.size<5 || p.size>24 || entry<first || p.size>last-entry ||
           !image.Range(static_cast<uint32_t>(entry-base),p.size) ||
           std::memcmp(image.clean.data()+entry-base,p.code.data()+32,p.size) || !PatchIntact(p))return false;
        const auto offset=entry-first;
        for(unsigned j=0;j<p.size;++j){if(covered[offset+j])return false;covered[offset+j]=true;}
        std::memcpy(expected.data()+offset,p.edit.data(),p.size);
    }
    return Equal(reinterpret_cast<const void*>(first),expected.data(),size);
}

namespace file_binding {
using Family=shim_recipe::Family;
using InstallerFamily=shim_recipe::InstallerFamily;
inline const auto& Families()noexcept {return shim_binding::Recipe().families;}
inline const auto& Installers()noexcept {return shim_binding::Recipe().installers;}
inline const auto& Slots()noexcept {return shim_binding::Recipe().slots;}
inline constexpr unsigned nativeSelectors=6;
constexpr bool Values(const Family& f,uintptr_t app,uintptr_t native,uintptr_t iat,uintptr_t resolver)noexcept {
    return app && native && app<=UINTPTR_MAX-f.wrapper && native<=UINTPTR_MAX-f.nativeRva &&
        resolver==app+f.wrapper && (iat==resolver || iat==native+f.nativeRva);
}
inline bool Alias(const Image& image,const Family& f)noexcept {
    ExportStep nt{},zw{};
    if(!shim_binding::ProfileImage(image,*shim_binding::Recipe().native) ||
       !CanonicalExportStep(image,f.nt,0,nt) || !CanonicalExportStep(image,f.zw,0,zw) ||
       nt.forward || zw.forward || nt.kind!=ImportKind::CodeBinding || zw.kind!=nt.kind ||
       nt.rva!=f.nativeRva || zw.rva!=nt.rva || nt.index!=f.ntIndex || zw.index!=f.zwIndex)return false;
    const auto ex=Read<IMAGE_EXPORT_DIRECTORY>(image.clean.data()+image.exports.VirtualAddress);
    if(ex.Base!=8)return false;unsigned cells=0,names=0;
    for(DWORD i=0;i<ex.NumberOfFunctions;++i)if(Read<DWORD>(image.clean.data()+ex.AddressOfFunctions+4*i)==nt.rva){
        if(i!=nt.index && i!=zw.index)return false;++cells;}
    for(DWORD i=0;i<ex.NumberOfNames;++i){const auto index=Read<WORD>(image.clean.data()+ex.AddressOfNameOrdinals+2*i);
        if(index!=nt.index && index!=zw.index)continue;
        const auto* name=image.String(Read<DWORD>(image.clean.data()+ex.AddressOfNames+4*i));
        if(!name || std::strcmp(name,index==nt.index?f.nt:f.zw))return false;++names;}
    return cells==2 && names==2;
}
// The selector is derived from the complete signed import table of an image
// already admitted to this ledger. Only the three native and three system rows
// qualify; a module name, mutable IAT value or GPA result grants no authority.
inline const char* SelectorName(unsigned selected)noexcept {
    if(selected<nativeSelectors)return selected&1?Families()[selected/2].zw:Families()[selected/2].nt;
    selected-=nativeSelectors;return selected<Installers().size()?Installers()[selected].symbol:nullptr;
}
inline bool Selector(const char* dll,const char* symbol,WORD ordinal,HMODULE importer,
                     const void* slot,const CanonicalBinding& canonical,unsigned& selected)noexcept {
    const auto native=GetModuleHandleW(L"ntdll.dll");
    if(!native || !dll || !symbol || ordinal || !importer || !slot ||
       !canonical.owner || canonical.kind!=ImportKind::CodeBinding)return false;
    const auto at=reinterpret_cast<uintptr_t>(slot),base=reinterpret_cast<uintptr_t>(importer);
    const auto owner=Owner(importer);
    if(at<base || at-base>UINT32_MAX || owner<0 || Ledger().images[owner].live!=importer || Owner(slot,8)!=owner)return false;
    for(unsigned i=0;i<nativeSelectors;++i){
        if(_stricmp(dll,"ntdll.dll") || canonical.owner!=native || std::strcmp(symbol,SelectorName(i)) || canonical.rva!=Families()[i/2].nativeRva)continue;
        // The five original SetValueKey roles keep their stricter paired-role
        // proof. This extension cannot supply a substitute for a refused role.
        if(i/2==2){ShimRole role{};
            if(shim_binding::SelectRole(dll,symbol,ordinal,importer,slot,GetModuleHandleW(L"apphelp.dll"),role))return false;}
        if(!shim_binding::CleanSelectedSlot(Ledger().images[owner],static_cast<DWORD>(at-base),symbol))return false;
        selected=i;return true;
    }
    for(unsigned i=0;i<Installers().size();++i){const auto& f=Installers()[i];
        if(std::strcmp(symbol,f.symbol) || canonical.owner!=GetModuleHandleW(f.owner))continue;
        const auto target=Owner(canonical.owner);ExportStep step{};
        if(target<0 || !CanonicalExportStep(Ledger().images[target],symbol,0,step) || step.forward ||
           step.kind!=ImportKind::CodeBinding || step.rva!=canonical.rva ||
           !shim_binding::CleanSelectedSlot(Ledger().images[owner],static_cast<DWORD>(at-base),symbol,dll,true))return false;
        selected=nativeSelectors+i;return true;
    }
    return false;
}
inline bool SelectedValues(unsigned selected,uintptr_t app,uintptr_t native,const CanonicalBinding& canonical,
                           uintptr_t iat,uintptr_t resolver)noexcept {
    if(selected<nativeSelectors)return Values(Families()[selected/2],app,native,iat,resolver);
    const auto index=selected-nativeSelectors;if(index>=Installers().size())return false;
    const auto owner=reinterpret_cast<uintptr_t>(canonical.owner);const auto wrapper=Installers()[index].wrapper;
    return app && owner && canonical.kind==ImportKind::CodeBinding && canonical.rva &&
        app<=UINTPTR_MAX-wrapper && owner<=UINTPTR_MAX-canonical.rva && resolver==app+wrapper &&
        (iat==resolver || iat==owner+canonical.rva);
}
inline bool RowValid(const shim_binding::Layout& layout,const Family& f,uintptr_t address,
                     const MEMORY_BASIC_INFORMATION& m,const std::array<uintptr_t,6>& row)noexcept {
    return shim_binding::LayoutValid(layout) && Span(f.row,sizeof(row),0x150) &&
        layout.table<=UINTPTR_MAX-0x150 && address==layout.table+f.row &&
        reinterpret_cast<uintptr_t>(m.AllocationBase)==layout.allocation &&
        reinterpret_cast<uintptr_t>(m.BaseAddress)==layout.region && m.RegionSize==layout.regionSize &&
        m.Type==MEM_PRIVATE && m.State==MEM_COMMIT && m.Protect==layout.protect && address>=layout.region &&
        Span(address-layout.region,sizeof(row),layout.regionSize) &&
        row[0]==layout.apphelp+shim_binding::Recipe().dllName && row[1]==layout.apphelp+f.symbol &&
        row[2]==layout.apphelp+f.wrapper && row[3]==layout.native+f.nativeRva;
}
inline bool Layout(shim_binding::Layout& layout)noexcept {
    const auto app=GetModuleHandleW(L"apphelp.dll"),native=GetModuleHandleW(L"ntdll.dll");
    if(!shim_binding::ReadLayout(app,native,layout))return false;
    const auto ai=Owner(app),ni=Owner(native);if(ai<0 || ni<0)return false;
    const auto& a=Ledger().images[ai];const auto& n=Ledger().images[ni];
    const auto& p=shim_binding::Recipe();
    if(!shim_binding::Pinned(a,p.create) || !shim_binding::Pinned(a,p.setInfo) || !shim_binding::Pinned(a,p.initializer))return false;
    for(const auto& f:Families()){
        if(f.end<=f.wrapper || !a.SectionHas(f.wrapper,f.end-f.wrapper,IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE,IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_DISCARDABLE) ||
           !Equal(reinterpret_cast<const unsigned char*>(app)+f.wrapper,a.clean.data()+f.wrapper,f.end-f.wrapper) ||
           !Alias(n,f) || !shim_binding::ImageData(a,f.symbol,std::strlen(f.nt)+1) ||
           !a.Range(f.symbol,std::strlen(f.nt)+1) || !a.String(f.symbol) || std::strcmp(a.String(f.symbol),f.nt) ||
           !Equal(reinterpret_cast<const unsigned char*>(app)+f.symbol,f.nt,std::strlen(f.nt)+1) ||
           !Span(f.row,sizeof(std::array<uintptr_t,6>),0x150) || layout.table>UINTPTR_MAX-f.row)return false;
        const auto address=layout.table+f.row;MEMORY_BASIC_INFORMATION m{};
        if(VirtualQuery(reinterpret_cast<const void*>(address),&m,sizeof(m))!=sizeof(m) ||
           reinterpret_cast<uintptr_t>(m.AllocationBase)!=layout.allocation ||
           reinterpret_cast<uintptr_t>(m.BaseAddress)!=layout.region || m.RegionSize!=layout.regionSize ||
           m.Type!=MEM_PRIVATE || m.State!=MEM_COMMIT || m.Protect!=layout.protect || address<layout.region ||
           !Span(address-layout.region,sizeof(std::array<uintptr_t,6>),layout.regionSize))return false;
        std::array<uintptr_t,6> row{};
        if(!Memory(reinterpret_cast<const void*>(address),row.data(),sizeof(row)) ||
           !RowValid(layout,f,address,m,row))return false;
    }
    return true;
}
inline bool InstallerRowValid(const shim_binding::Layout& layout,const InstallerFamily& f,
                              const CanonicalBinding& canonical,uintptr_t address,
                              const MEMORY_BASIC_INFORMATION& m,const std::array<uintptr_t,6>& row)noexcept {
    const auto target=reinterpret_cast<uintptr_t>(canonical.owner);
    return shim_binding::LayoutValid(layout) && target && canonical.kind==ImportKind::CodeBinding &&
        canonical.rva && target<=UINTPTR_MAX-canonical.rva && Span(f.row,sizeof(row),0x150) &&
        layout.table<=UINTPTR_MAX-f.row && address==layout.table+f.row &&
        reinterpret_cast<uintptr_t>(m.AllocationBase)==layout.allocation &&
        reinterpret_cast<uintptr_t>(m.BaseAddress)==layout.region && m.RegionSize==layout.regionSize &&
        m.Type==MEM_PRIVATE && m.State==MEM_COMMIT && m.Protect==layout.protect && address>=layout.region &&
        Span(address-layout.region,sizeof(row),layout.regionSize) &&
        row[0]==layout.apphelp+f.dllRva && row[1]==layout.apphelp+f.symbolRva &&
        row[2]==layout.apphelp+f.wrapper && row[3]==target+canonical.rva;
}
inline const char* InstallerFailure(const shim_binding::Layout& layout,unsigned selected,const CanonicalBinding& canonical)noexcept {
    if(selected<nativeSelectors || selected-nativeSelectors>=Installers().size())return "installer-selector";
    const auto& f=Installers()[selected-nativeSelectors];const auto owner=Owner(reinterpret_cast<void*>(layout.apphelp));
    if(owner<0 || canonical.owner!=GetModuleHandleW(f.owner))return "installer-canonical-owner";
    const auto& image=Ledger().images[owner];
    if(f.end<=f.wrapper || !image.SectionHas(f.wrapper,f.end-f.wrapper,IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE,IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_DISCARDABLE) ||
       !OwnedWrapperCode(image,f.wrapper,f.end-f.wrapper))return "installer-wrapper-code";
    const auto string=[&](DWORD at,const char* value){const auto size=std::strlen(value)+1;
        return shim_binding::ImageData(image,at,size) && image.String(at) && !std::strcmp(image.String(at),value) &&
            Equal(reinterpret_cast<const void*>(layout.apphelp+at),value,size);};
    if(!string(f.dllRva,f.dll) || !string(f.symbolRva,f.symbol))return "installer-row-selectors";
    if(layout.table>UINTPTR_MAX-f.row)return "installer-row-range";
    const auto address=layout.table+f.row;MEMORY_BASIC_INFORMATION m{};std::array<uintptr_t,6> row{};
    if(VirtualQuery(reinterpret_cast<const void*>(address),&m,sizeof(m))!=sizeof(m) ||
       !Memory(reinterpret_cast<const void*>(address),row.data(),sizeof(row)))return "installer-row-read";
    if(!InstallerRowValid(layout,f,canonical,address,m,row))return "installer-row-original-or-layout";
    return nullptr;
}
inline bool Intact()noexcept {
    const auto& l=Ledger();const auto& s=l.shim;
    if(l.runtimeFileCount>l.runtimeFileEdges.size() || s.fileCount>s.fileEdges.size() ||
       l.runtimeFileCount>s.fileEdges.size()-s.fileCount ||
       (l.runtimeFileCount && s.state!=RepresentationState::Sealed))return false;
    const auto total=s.fileCount+l.runtimeFileCount;if(!total)return true;
    if((s.state!=RepresentationState::Building && s.state!=RepresentationState::Validated && s.state!=RepresentationState::Sealed) ||
       s.fileCount>s.fileEdges.size())return false;shim_binding::Layout f{};if(!Layout(f))return false;
    if(s.state==RepresentationState::Sealed && (s.fileCount!=s.frozenFileCount ||
       f.table!=s.table || f.allocation!=s.allocation || f.region!=s.region || f.regionSize!=s.regionSize || f.protect!=s.protect))return false;
    const auto edge=[&](unsigned i)->const FileRepresentedEdge& {return i<s.fileCount?s.fileEdges[i]:l.runtimeFileEdges[i-s.fileCount];};
    for(unsigned i=0;i<total;++i){const auto& e=edge(i);
        const auto* symbol=SelectorName(e.selector);if(!symbol)return false;
        for(unsigned j=0;j<i;++j)if(edge(j).slot==e.slot)return false;
        unsigned selected=UINT_MAX;uintptr_t current=0;
        if(!e.dll || !Selector(e.dll,symbol,0,e.importer,e.slot,e.canonical,selected) || selected!=e.selector ||
           !SelectedValues(e.selector,f.apphelp,f.native,e.canonical,e.iatTarget,e.resolverTarget) ||
           !Memory(e.slot,&current,8) || current!=e.iatTarget ||
           reinterpret_cast<uintptr_t>(GetProcAddress(e.canonical.owner,symbol))!=e.resolverTarget ||
           (e.selector>=nativeSelectors && InstallerFailure(f,e.selector,e.canonical)))return false;
    }return true;
}
inline bool Seal()noexcept {
    const auto& s=Ledger().shim;if(!s.fileCount)return true;
    if(!Intact())return false;const auto native=GetModuleHandleW(L"ntdll.dll");
    // Every signed family slot participates exactly once, canonical or represented.
    for(unsigned i=0;i<Slots().size();++i){const auto& spec=Slots()[i];const auto module=GetModuleHandleW(spec.module);
        const auto owner=Owner(module);if(owner<0 || !shim_binding::CleanSelectedSlot(Ledger().images[owner],spec.rva,spec.symbol))return false;
        const auto slot=reinterpret_cast<const unsigned char*>(module)+spec.rva;unsigned matches=0;
        for(unsigned j=0;j<s.fileCount;++j)if(s.fileEdges[j].slot==slot)++matches;
        for(unsigned j=0;j<Ledger().importCount;++j){const auto& e=Ledger().imports[j];if(e.slot!=slot)continue;
            if(e.importer!=module || !e.dll || _stricmp(e.dll,"ntdll.dll") || !e.name || std::strcmp(e.name,spec.symbol) ||
               e.ordinal || e.lazy || e.binding.owner!=native || e.binding.kind!=ImportKind::CodeBinding ||
               e.binding.rva!=Families()[spec.family].nativeRva || e.target!=reinterpret_cast<uintptr_t>(native)+e.binding.rva)return false;
            ++matches;}
        if(matches!=1)return false;
    }return true;
}
// Full existing installer/profile proof for an exact, previously recorded
// signed lazy slot. Sealed base representation state/counts are never reopened.
inline bool DelayedProof(unsigned index,const FileRepresentedEdge& e)noexcept {
    const auto& l=Ledger();const auto& sealed=l.shim;
    if(Revoked() || store.initialized.load()!=2 || sealed.state!=RepresentationState::Sealed ||
       index>=l.importCount || e.selector<nativeSelectors ||
       e.selector-nativeSelectors>=Installers().size())return false;
    const auto& base=l.imports[index];unsigned selected=UINT_MAX;uintptr_t current=0;
    if(!base.lazy || !base.slot || !base.dll || !base.name || base.ordinal ||
       base.binding.kind!=ImportKind::DelayThunk || base.binding.owner!=base.importer ||
       e.slot!=base.slot || e.importer!=base.importer || e.dll!=base.dll ||
       e.iatTarget!=e.resolverTarget ||
       !Selector(base.dll,base.name,base.ordinal,base.importer,base.slot,e.canonical,selected) || selected!=e.selector)return false;
    const auto owner=Owner(base.slot,sizeof(uintptr_t));if(owner<0)return false;
    const auto& image=l.images[owner];const auto at=reinterpret_cast<uintptr_t>(base.slot),start=reinterpret_cast<uintptr_t>(image.live);
    if(image.live!=base.importer || at<start || at-start>UINT32_MAX ||
       !image.Range(static_cast<DWORD>(at-start),sizeof(uintptr_t)) ||
       Read<uintptr_t>(image.clean.data()+at-start)!=base.target ||
       base.target!=reinterpret_cast<uintptr_t>(base.binding.owner)+base.binding.rva ||
       !Executable(reinterpret_cast<void*>(base.target)) ||
       !Memory(base.slot,&current,sizeof(current)) || current!=e.iatTarget)return false;
    shim_binding::Layout layout{};
    if(!Layout(layout) || layout.table!=sealed.table || layout.allocation!=sealed.allocation ||
       layout.region!=sealed.region || layout.regionSize!=sealed.regionSize || layout.protect!=sealed.protect ||
       !SelectedValues(e.selector,layout.apphelp,layout.native,e.canonical,e.iatTarget,e.resolverTarget) ||
       InstallerFailure(layout,e.selector,e.canonical) ||
       reinterpret_cast<uintptr_t>(GetProcAddress(e.canonical.owner,base.name))!=e.resolverTarget)return false;
    HMODULE actualOwner=nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(e.iatTarget),&actualOwner) && reinterpret_cast<uintptr_t>(actualOwner)==layout.apphelp;
}
inline bool DelayedIntact()noexcept {
    const auto& l=Ledger();if(l.delayedCount>l.delayedEdges.size())return false;
    for(unsigned i=0;i<l.delayedCount;++i){const auto& item=l.delayedEdges[i];
        if(item.importIndex>=l.importCount || l.delayedIndex[item.importIndex]!=i+1 ||
           !DelayedProof(item.importIndex,item.edge))return false;
    }return true;
}
// R797: collection authority comes from the exact in-progress signed image
// graph, never from the optional diagnostic TLS. Selector still independently
// reparses the complete clean descriptor/name/slot graph and proves its target.
// This check alone grants no representation, native call or history authority.
inline bool RuntimeCollectionContext(const ImportFailureContext& c,bool collect)noexcept {
    if(!collect || store.initialized.load()!=2 || Revoked() || !c.importer || !c.slot)return false;
    const auto owner=Owner(c.slot,sizeof(uintptr_t));
    return owner>=0 && Ledger().images[owner].live==c.importer &&
        Ledger().imageCollection[owner]==ImageCollection::Building;
}
inline ImportResolution Resolve(const ImportFailureContext& c,const CanonicalBinding& canonical,
                                uintptr_t resolver,bool collect,FileRepresentedEdge* delayed=nullptr) {
    auto& s=Ledger().shim;unsigned selected=UINT_MAX;
    if(Revoked() || !c.actualKnown || !Selector(c.dll,c.symbol,c.ordinal,c.importer,c.slot,canonical,selected))return ImportResolution::Refused;
    const auto app=GetModuleHandleW(L"apphelp.dll");const auto a=reinterpret_cast<uintptr_t>(app),n=reinterpret_cast<uintptr_t>(canonical.owner);
    if(!SelectedValues(selected,a,n,canonical,c.actual,resolver))return ImportResolution::Refused;
    HMODULE actualOwner=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(c.actual),&actualOwner) || actualOwner!=(c.actual==resolver?app:canonical.owner))return ImportResolution::Refused;
    if(s.state==RepresentationState::Sealed){
        if(!Intact())return ImportResolution::Refused;
        auto& runtime=Ledger();
        for(unsigned i=0;i<s.fileCount+runtime.runtimeFileCount;++i){const auto& e=i<s.fileCount?s.fileEdges[i]:runtime.runtimeFileEdges[i-s.fileCount];
            if(e.selector==selected && e.slot==c.slot && e.importer==c.importer && e.iatTarget==c.actual &&
               e.resolverTarget==resolver && e.canonical.owner==canonical.owner && e.canonical.rva==canonical.rva &&
               e.canonical.kind==canonical.kind)return ImportResolution::Represented;}
        if(RuntimeCollectionContext(c,collect)){
            // Only CollectImport reached from a newly authenticated signed
            // descriptor graph may append a noncallable observation. An already
            // recorded Import cannot be reclassified by this collection path.
            if(runtime.runtimeFileCount>=runtime.runtimeFileEdges.size() ||
               runtime.runtimeFileCount>=s.fileEdges.size()-s.fileCount ||
               runtime.importCount+RepresentationCount()>=runtime.imports.size() || Owner(app)<0)return ImportResolution::Refused;
            for(unsigned i=0;i<runtime.importCount;++i)if(runtime.imports[i].slot==c.slot)return ImportResolution::Refused;
            for(unsigned i=0;i<s.fileCount;++i)if(s.fileEdges[i].slot==c.slot)return ImportResolution::Refused;
            for(unsigned i=0;i<runtime.runtimeFileCount;++i)if(runtime.runtimeFileEdges[i].slot==c.slot)return ImportResolution::Refused;
            const auto refuse=[&](const char* reason){PinTerminalImportFailure(reason,c);return ImportResolution::Refused;};
            shim_binding::Layout layout{};
            if(!Layout(layout))return refuse("runtime-file-base-layout");
            if(layout.table!=s.table || layout.allocation!=s.allocation || layout.region!=s.region ||
               layout.regionSize!=s.regionSize || layout.protect!=s.protect)return refuse("runtime-file-sealed-layout-changed");
            if(selected>=nativeSelectors)if(const auto* reason=InstallerFailure(layout,selected,canonical))return refuse(reason);
            runtime.runtimeFileEdges[runtime.runtimeFileCount++]={selected,c.slot,c.importer,c.actual,resolver,canonical,c.dll};
            // A failed collection remains a refusal. No canonical binding is
            // returned, and the caller still seals every image/import/code edge.
            if(!Intact())return ImportResolution::Refused;
            return ImportResolution::Deferred;
        }
        if(delayed && observeSetupImports){
            // Output is staged only. The guarded lifecycle validates the entire
            // code/import closure and all pending targets before publication.
            auto& l=Ledger();unsigned index=UINT_MAX;
            for(unsigned i=0;i<l.importCount;++i)if(l.imports[i].slot==c.slot){if(index!=UINT_MAX)return ImportResolution::Refused;index=i;}
            FileRepresentedEdge edge{selected,c.slot,c.importer,c.actual,resolver,canonical,c.dll};
            if(index<l.importCount && !l.delayedIndex[index] && l.delayedCount<l.delayedEdges.size() &&
               DelayedProof(index,edge)){*delayed=edge;return ImportResolution::Deferred;}
        }
        return ImportResolution::Refused;
    }
    if(!collect || !observeSetupImports || store.initialized.load()!=1 ||
       (s.state!=RepresentationState::Absent && s.state!=RepresentationState::Building) ||
       s.fileCount>=s.fileEdges.size() || Ledger().importCount+RepresentationCount()>=Ledger().imports.size())return ImportResolution::Refused;
    for(unsigned i=0;i<s.fileCount;++i)if(s.fileEdges[i].slot==c.slot)return ImportResolution::Refused;
    s.fileEdges[s.fileCount++]={selected,c.slot,c.importer,c.actual,resolver,canonical,c.dll};
    if(!AddImage(app))return ImportResolution::Refused;
    if(selected>=nativeSelectors){shim_binding::Layout layout{};
        if(!Layout(layout)){PinImportFailure("installer-base-layout",c);return ImportResolution::Refused;}
        if(const auto* reason=InstallerFailure(layout,selected,canonical)){PinImportFailure(reason,c);return ImportResolution::Refused;}}
    return ImportResolution::Deferred;
}
} // namespace file_binding
inline ImportResolution ResolveImport(const char* dll,const char* name,WORD ordinal,HMODULE nominal,CanonicalBinding& binding,HMODULE importer=nullptr,ImportFailureContext* observation=nullptr,bool collect=false,FileRepresentedEdge* delayed=nullptr) {

    const auto fail=[&](const char* stage){binding={};if(observation)PinImportFailure(stage,*observation);return ImportResolution::Refused;};
    if(observation){observation->textTruncated=ImportText(observation->resolverDll,dll) || observation->textTruncated;observation->resolverSymbol=name;observation->resolverOrdinal=ordinal;}
    binding={};auto module=CanonicalModule(dll,nominal,importer);if(!module)return fail("canonical-module");
    struct Hop {HMODULE module=nullptr;const char* name=nullptr;WORD ordinal=0;DWORD index=0;const char* forward=nullptr;};
    std::array<Hop,16> chain{};unsigned count=0;
    for(;;){
        if(observation){observation->hop=count;observation->resolverModule=module;
            observation->resolverSymbol=name;observation->resolverOrdinal=ordinal;
            observation->exportIndex=UINT32_MAX;observation->forward=nullptr;}
        if(count==chain.size())return fail("resolver-hop-capacity");
        if(!AddImage(module))return fail("resolver-image-authentication");
        if(Revoked())return fail("resolver-ledger-revoked");
        const auto owner=Owner(module);if(owner<0)return fail("resolver-image-owner");
        ExportStep step{};if(!CanonicalExportStep(Ledger().images[owner],name,ordinal,step))return fail("clean-export-step");
        if(observation){observation->exportIndex=step.index;observation->forward=step.forward;}
        for(unsigned i=0;i<count;++i)if(chain[i].module==module && chain[i].index==step.index)return fail("resolver-cycle");
        chain[count++]={module,name,ordinal,step.index,step.forward};
        if(!step.forward){
            const auto base=reinterpret_cast<uintptr_t>(module);
            if(step.rva>UINTPTR_MAX-base)return fail("canonical-target-overflow");
            const auto target=base+step.rva;
            if(observation){observation->expected=target;observation->expectedOwner=module;
                observation->expectedRva=step.rva;observation->expectedKind=step.kind;observation->expectedKnown=true;}
            // An exact ordinary IAT already selects the clean canonical target.
            // Prove every clean forwarder/name/ordinal edge directly against the
            // live signed image instead of requiring a DIFFERENT dynamic resolver
            // path to return the same address. Noncanonical IATs and non-import
            // callers retain the existing finite representation/GPA proof below.
            shim_binding::PrepareRecipe(module);
            ShimRole profiledRole{};
            const bool profiledSlot=observation && module==GetModuleHandleW(L"ntdll.dll") &&
                shim_binding::SelectRole(observation->dll,observation->symbol,observation->ordinal,
                    observation->importer,observation->slot,GetModuleHandleW(L"apphelp.dll"),profiledRole);
            if(!profiledSlot && ExactCanonicalIat(observation,target)){
                for(unsigned i=0;i<count;++i){const auto& hop=chain[i];
                    const auto at=Owner(hop.module);ExportStep edge{};
                    if(at<0 || !CanonicalExportStep(Ledger().images[at],hop.name,hop.ordinal,edge) ||
                       edge.index!=hop.index || !CanonicalExportIntact(Ledger().images[at],hop.name,hop.ordinal,edge))
                        return fail("canonical-live-export-directory");
                }
                binding={module,step.rva,step.kind};return ImportResolution::Canonical;
            }
            // Remaining GPA evidence never selects an address or admits a wrapper.
            for(unsigned i=0;i<count;++i){const auto& hop=chain[i];
                const auto actual=reinterpret_cast<uintptr_t>(GetProcAddress(hop.module,hop.name?hop.name:MAKEINTRESOURCEA(hop.ordinal)));
                if(actual!=target){
                    if(observation){observation->hop=i;observation->resolverModule=hop.module;
                        observation->resolverSymbol=hop.name;observation->resolverOrdinal=hop.ordinal;
                        observation->exportIndex=hop.index;observation->resolverActual=actual;observation->resolverActualKnown=true;
                        // This hop can precede the terminal module. Its exact
                        // module/symbol/index identify it; the final token must
                        // not be mislabeled as the failed corroboration hop.
                        observation->resolverDll.fill(0);observation->forward=hop.forward;}
                    // Only a direct clean native selector with the exact import
                    // context can request a separate representation receipt.
                    if(observation){
                        const bool direct=count==1 && !hop.forward;
                        bool installer=false;
                        for(const auto& f:file_binding::Installers())
                            installer=installer || (observation->symbol && !std::strcmp(observation->symbol,f.symbol));
                        if(direct || installer){
                            // A represented forward path must have every signed
                            // name/ordinal/EAT edge independently revalidated.
                            for(unsigned j=0;j<count;++j){const auto& edge=chain[j];const auto at=Owner(edge.module);ExportStep live{};
                                if(at<0 || !CanonicalExportStep(Ledger().images[at],edge.name,edge.ordinal,live) ||
                                   live.index!=edge.index || !CanonicalExportIntact(Ledger().images[at],edge.name,edge.ordinal,live))
                                    return fail("represented-live-export-directory");}
                            const CanonicalBinding canonical{module,step.rva,step.kind};
                            auto result=direct?shim_binding::Resolve(*observation,canonical,actual,collect):ImportResolution::Refused;
                            if(result==ImportResolution::Refused)result=file_binding::Resolve(*observation,canonical,actual,collect,delayed);
                            if(result==ImportResolution::Represented || result==ImportResolution::Deferred){binding={};return result;}
                        }
                    }
                    return fail("getprocaddress-corroboration");
                }
            }
            binding={module,step.rva,step.kind};return ImportResolution::Canonical;
        }
        char nextModule[128]{};const char* nextName=nullptr;WORD nextOrdinal=0;
        if(!ForwardSelector(step.forward,nextModule,sizeof(nextModule),nextName,nextOrdinal))return fail("forward-selector");
        module=CanonicalModule(nextModule,nullptr,module);if(!module)return fail("forward-module");
        if(observation)observation->textTruncated=ImportText(observation->resolverDll,nextModule) || observation->textTruncated;
        name=nextName;ordinal=nextOrdinal;
    }
}
inline bool ResolveCanonicalImport(const char* dll,const char* name,WORD ordinal,HMODULE nominal,CanonicalBinding& binding,HMODULE importer=nullptr,ImportFailureContext* observation=nullptr) {
    const auto result=ResolveImport(dll,name,ordinal,nominal,binding,importer,observation);
    if(result!=ImportResolution::Canonical)binding={};
    return result==ImportResolution::Canonical;
}
inline bool ImportOwner(uintptr_t value,const CanonicalBinding& binding)noexcept {
    const auto expected=reinterpret_cast<uintptr_t>(binding.owner)+binding.rva;
    if(value!=expected)return false;
    HMODULE actualOwner=nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(value),&actualOwner) && actualOwner==binding.owner &&
        (binding.kind==ImportKind::CodeBinding || binding.kind==ImportKind::DataBinding);
}
inline bool ImportTarget(uintptr_t value,const char* name,WORD ordinal,HMODULE nominal,const char* dll,CanonicalBinding& binding,HMODULE importer=nullptr,const void* slot=nullptr) {
    ImportFailureContext observation{};observation.dll=dll;observation.symbol=name;observation.ordinal=ordinal;
    observation.nominal=nominal;observation.importer=importer;observation.slot=slot;
    observation.actual=value;observation.actualKnown=true;
    const auto fail=[&](const char* stage){PinTerminalImportFailure(stage,observation);Revoke("canonical-import-binding",nominal);return false;};
    if(!value)return fail("iat-zero-target");
    const auto resolved=ResolveImport(dll,name,ordinal,nominal,binding,importer,&observation);
    if(resolved!=ImportResolution::Canonical)return fail("canonical-resolution-or-represented-edge");
    const auto expected=reinterpret_cast<uintptr_t>(binding.owner)+binding.rva;
    HMODULE actualOwner=nullptr;
    if(value!=expected)return fail("iat-target-mismatch");
    const auto ownerResolved=GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(value),&actualOwner);
    observation.queriedActualOwner=actualOwner;
    if(!ownerResolved)return fail("iat-owner-query");
    if(actualOwner!=binding.owner)return fail("iat-owner-mismatch");
    if(binding.kind!=ImportKind::CodeBinding && binding.kind!=ImportKind::DataBinding)return fail("iat-binding-kind");
    return true;
}
enum class ImportCollection : unsigned char { Refused, Canonical, Staged };
inline ImportCollection CollectImport(const void* slot,const char* name,WORD ordinal,HMODULE nominal,const char* dll,HMODULE importer=nullptr) {
    uintptr_t value=0;CanonicalBinding binding{};
    ImportFailureContext c{};c.dll=dll;c.symbol=name;c.ordinal=ordinal;c.nominal=nominal;c.importer=importer;c.slot=slot;
    const auto fail=[&](const char* reason){PinTerminalImportFailure(reason,c);Revoke("canonical-import-receipt",nominal);return ImportCollection::Refused;};
    if(!Memory(slot,&value,sizeof(value)))return fail("iat-read");
    c.actual=value;c.actualKnown=true;if(!value)return fail("iat-zero-target");
    const auto result=ResolveImport(dll,name,ordinal,nominal,binding,importer,&c,true);
    if(result==ImportResolution::Deferred)return ImportCollection::Staged;
    if(result==ImportResolution::Refused)return fail("collected-import-resolution");
    if(Ledger().importCount+RepresentationCount()>=Ledger().imports.size())return fail("import-ledger-capacity");
    if(result!=ImportResolution::Canonical)return fail("collected-import-kind");
    if(!ImportOwner(value,binding))return fail("collected-iat-owner");
    Import item{};item.slot=slot;item.target=value;item.dll=dll;item.name=name;item.ordinal=ordinal;item.binding=binding;item.importer=importer;
    Ledger().imports[Ledger().importCount++]=item;return ImportCollection::Canonical;
}
inline bool DelayTable(Image& image) {
    const auto imageIndex=Owner(image.live);if(imageIndex<0)return false;
    auto& coverage=Ledger().imageCoverage[static_cast<unsigned>(imageIndex)];
    const auto d=image.delays;if(!d.Size){coverage.delayTerminated=true;return true;}
    if(!image.Range(d.VirtualAddress,d.Size))return false;
    // ImgDelayDescr, RVA form. No loader execution or delay-helper invocation.
    for(size_t at=0;at+32<=d.Size;at+=32){const auto* descriptor=image.clean.data()+d.VirtualAddress+at;
        const auto attributes=Read<DWORD>(descriptor),nameRva=Read<DWORD>(descriptor+4);
        const std::array<unsigned char,32> zeroDescriptor{};
        if(!std::memcmp(descriptor,zeroDescriptor.data(),zeroDescriptor.size())){coverage.delayTerminated=true;return true;}
        const auto iat=Read<DWORD>(descriptor+12),names=Read<DWORD>(descriptor+16);
        const auto* dll=image.String(nameRva);if(attributes!=1 || !dll || !iat || !names)return false;
        ++coverage.delayDescriptors;
        wchar_t wide[512]{};if(!MultiByteToWideChar(CP_ACP,MB_ERR_INVALID_CHARS,dll,-1,wide,512))return false;
        auto nominal=ApiContract(dll)?nullptr:GetModuleHandleW(wide);
        if(!nominal && _strnicmp(dll,"api-",4) && _strnicmp(dll,"ext-",4)){
            // A delay-only dependency need not be loaded yet. It will be
            // independently authenticated if the native loader resolves it.
            nominal=nullptr;
        }
        bool ended=false;
        for(unsigned j=0;j<8192;++j){const uint64_t source=uint64_t(names)+8ull*j,dest=uint64_t(iat)+8ull*j;
            if(source>UINT32_MAX || dest>UINT32_MAX || !image.Range(static_cast<uint32_t>(source),8) ||
               !image.Range(static_cast<uint32_t>(dest),8))return false;
            const auto thunk=Read<uint64_t>(image.clean.data()+source);if(!thunk){ended=true;break;}
            ++coverage.delaySlots;
            const char* name=nullptr;WORD ordinal=0;
            if(thunk&IMAGE_ORDINAL_FLAG64){if(thunk&~(IMAGE_ORDINAL_FLAG64|uint64_t(0xffff)))return false;ordinal=static_cast<WORD>(thunk);}
            else {if(thunk>UINT32_MAX-2)return false;name=image.String(static_cast<uint32_t>(thunk)+2);if(!name)return false;}
            const auto* slot=reinterpret_cast<unsigned char*>(image.live)+dest;
            uintptr_t value=0;if(!Memory(slot,&value,sizeof(value)))return false;
            const auto initial=Read<uintptr_t>(image.clean.data()+dest);
            if(value==initial){
                if(value<reinterpret_cast<uintptr_t>(image.live) || value-reinterpret_cast<uintptr_t>(image.live)>UINT32_MAX ||
                   !image.SectionHas(static_cast<uint32_t>(value-reinterpret_cast<uintptr_t>(image.live)),1,IMAGE_SCN_MEM_EXECUTE))return false;
                if(Ledger().importCount+RepresentationCount()>=Ledger().imports.size()){
                    Revoke("delay-import-ledger-capacity",image.live);return false;}
                Ledger().imports[Ledger().importCount++]={slot,value,dll,name,ordinal,true,
                    {image.live,static_cast<uint32_t>(value-reinterpret_cast<uintptr_t>(image.live)),ImportKind::DelayThunk},image.live};
            }else if(CollectImport(slot,name,ordinal,nominal,dll,image.live)==ImportCollection::Refused)return false;
        }if(!ended)return false;
    }return false;
}
inline bool ImportTable(Image& image) {
    if(!DelayTable(image))return false;
    const auto imageIndex=Owner(image.live);if(imageIndex<0)return false;
    auto& coverage=Ledger().imageCoverage[static_cast<unsigned>(imageIndex)];
    const auto d=image.imports;if(!d.Size){coverage.regularTerminated=true;return true;}
    if(!image.Range(d.VirtualAddress,d.Size))return false;
    for(size_t at=0;at+sizeof(IMAGE_IMPORT_DESCRIPTOR)<=d.Size;at+=sizeof(IMAGE_IMPORT_DESCRIPTOR)){
        const auto desc=Read<IMAGE_IMPORT_DESCRIPTOR>(image.clean.data()+d.VirtualAddress+at);
        if(!desc.Name && !desc.FirstThunk && !desc.OriginalFirstThunk && !desc.TimeDateStamp && !desc.ForwarderChain){
            coverage.regularTerminated=true;return true;
        }
        const auto* dll=image.String(desc.Name);
        if(!dll || !desc.OriginalFirstThunk || !desc.FirstThunk)return false;
        ++coverage.regularDescriptors;
        wchar_t wide[512]{};if(!MultiByteToWideChar(CP_ACP,MB_ERR_INVALID_CHARS,dll,-1,wide,512))return false;
        auto nominal=ApiContract(dll)?nullptr:GetModuleHandleW(wide);
        if(!nominal && _strnicmp(dll,"api-",4) && _strnicmp(dll,"ext-",4))return false;
        bool ended=false;
        for(unsigned j=0;j<8192;++j){
            const uint64_t source=uint64_t(desc.OriginalFirstThunk)+8ull*j,dest=uint64_t(desc.FirstThunk)+8ull*j;
            if(source>UINT32_MAX || dest>UINT32_MAX || !image.Range(static_cast<uint32_t>(source),8) ||
               !image.Range(static_cast<uint32_t>(dest),8))return false;
            const auto thunk=Read<uint64_t>(image.clean.data()+source);
            if(!thunk){ended=true;break;}
            ++coverage.regularSlots;
            const char* name=nullptr;WORD ordinal=0;
            if(thunk&IMAGE_ORDINAL_FLAG64){if(thunk&~(IMAGE_ORDINAL_FLAG64|uint64_t(0xffff)))return false;ordinal=static_cast<WORD>(thunk);}
            else {if(thunk>UINT32_MAX-2)return false;name=image.String(static_cast<uint32_t>(thunk)+2);if(!name)return false;}
            if(CollectImport(reinterpret_cast<unsigned char*>(image.live)+dest,name,ordinal,nominal,dll,image.live)==ImportCollection::Refused)return false;
        }if(!ended)return false;
    }return false;
}
inline bool PrepareNativeBirthLocked()noexcept;
inline bool FinishNativeDelaySetup() {return PrepareNativeBirthLocked();}
// Validate normalization against trusted ORIGINAL bytes and authenticated IAT
// bindings. AttachEx's normalized target is an observation, never authority.
inline bool AddAddressImage(const void* address,const std::source_location& caller=std::source_location::current()) {
    HMODULE module=nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address),&module) && AddImage(module,caller);
}
inline bool AddPatchImages(const Patch& p,const std::source_location& caller=std::source_location::current()) {
    // Keep the patch/slot/byte/lifetime receipt, but do not collect an unrelated
    // private input image as a system root. Any overlap fails, never overlays.
    if(p.input.module)return IndependentInputIntact(p);
    PatchCollectionScope origin(p,caller);
    origin.Role("patch-normalized-entry",p.entry);
    if(!AddAddressImage(p.entry))return false;
    origin.Role("patch-requested-entry",p.requested);
    if(!AddAddressImage(p.requested))return false;
    origin.Role("patch-declared-export-module",p.declaredModule);
    if(p.declaredModule && !AddImage(p.declaredModule))return false;
    for(unsigned i=0;i<p.hopCount;++i){
        origin.Role("patch-native-hop-entry",p.hops[i].entry,i);
        if(!AddAddressImage(p.hops[i].entry))return false;
        origin.Role("patch-native-hop-target",p.hops[i].target,i);
        if(!AddAddressImage(p.hops[i].target))return false;
    }
    return true;
}
// Executable consumer of the shared canonical resolver. A DataBinding
// receipt can never produce a callable export for a detour/thunk request.
inline void* CleanExport(HMODULE module,const char* symbol) {
    if(!symbol || !AddImage(module))return nullptr;
    std::array<char,128> basename{};CanonicalBinding binding{};
    if(!ImporterBasename(module,basename) ||
       !ResolveCanonicalImport(basename.data(),symbol,0,module,binding,module) ||
       binding.kind!=ImportKind::CodeBinding)return nullptr;
    auto* target=reinterpret_cast<unsigned char*>(binding.owner)+binding.rva;
    return Executable(target)?target:nullptr;
}
// Keep the signed start and its original bytes as a separate obligation.
// A composed receipt replaces only an authenticated same-export predecessor.
inline bool FactoryCleanStart(const Patch& p,const Image& image)noexcept {
    const auto& r=p.predecessor;
    if(!r.captured || p.input.module || !p.ownSetup || p.hopCount || p.size!=5 ||
       r.generation==UINT64_MAX ||
       p.requested!=p.entry || r.nodes[0].address!=reinterpret_cast<uintptr_t>(p.entry) ||
       p.declaredModule!=r.module || image.live!=r.module || !p.exportName || !r.symbol ||
       std::strcmp(p.exportName,r.symbol) || p.installGeneration!=r.generation+1 ||
       !p.installPreimageCaptured || std::memcmp(r.nodes[0].bytes.data(),p.installPreimage.data(),p.size) ||
       std::memcmp(p.code.data()+32,p.installPreimage.data(),p.size))return false;
    ExportStep step{};
    return CanonicalExportStep(image,p.exportName,0,step) && !step.forward &&
        step.kind==ImportKind::CodeBinding && step.rva==p.rva &&
        image.SectionHas(p.rva,r.nodes[0].bytes.size(),IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE,
                         IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_DISCARDABLE) &&
        (!p.factoryCleanCaptured || !std::memcmp(p.factoryCleanStart.data(),image.clean.data()+p.rva,p.factoryCleanStart.size())) &&
        !std::memcmp(image.clean.data()+p.rva+p.size,r.nodes[0].bytes.data()+p.size,
                     r.nodes[0].bytes.size()-p.size);
}
inline bool PatchPreimage(const Patch& p,const Image& image,bool installed=true)noexcept {
    if(!p.predecessor.captured)return !std::memcmp(p.code.data()+32,image.clean.data()+p.rva,p.size);
    return FactoryCleanStart(p,image) && FactoryPredecessorIntact(p.predecessor,installed?p.size:0);
}
inline bool BindPatch(Patch& p) {
    if(p.input.module)return false; // Input ownership is never callable native authority.
    if(p.predecessor.captured && !p.factoryCleanCaptured)return false;
    if(!p.installPreimageCaptured || p.size>p.installPreimage.size() ||
       std::memcmp(p.installPreimage.data(),p.code.data()+32,p.size))return false;
    const auto owner=Owner(p.entry,p.size);if(owner<0 || static_cast<unsigned>(owner)!=p.image)return false;
    if(p.rva!=static_cast<uint32_t>(p.entry-reinterpret_cast<unsigned char*>(Ledger().images[p.image].live)))return false;
    if(!Ledger().images[p.image].SectionHas(p.rva,p.size,IMAGE_SCN_MEM_EXECUTE) ||
       !PatchPreimage(p,Ledger().images[p.image]))return false;
    if(p.declaredModule && CleanExport(p.declaredModule,p.exportName)!=p.requested)return false;
    void* next=p.requested;
    for(unsigned i=0;i<p.hopCount;++i){const auto& h=p.hops[i];
        if(next!=h.entry || !Executable(h.target))return false;
        const auto imageIndex=Owner(h.entry,h.size);if(imageIndex<0)return false;
        const auto& image=Ledger().images[imageIndex];
        const auto rva=static_cast<uint32_t>(h.entry-reinterpret_cast<unsigned char*>(image.live));
        if(!image.SectionHas(rva,h.size,IMAGE_SCN_MEM_EXECUTE) ||
           std::memcmp(image.clean.data()+rva,h.code.data(),h.size))return false;
        if(h.iat){bool known=false;
            for(unsigned j=0;j<Ledger().importCount;++j)if(Ledger().imports[j].slot==h.iat &&
                Ledger().imports[j].target==reinterpret_cast<uintptr_t>(h.target) && !Ledger().imports[j].lazy &&
                Ledger().imports[j].binding.kind==ImportKind::CodeBinding){known=true;break;}
            if(!known)return false;
        }next=h.target;
    }
    return next==p.entry;
}
inline bool IndexPatch(Patch& p,SetupPatchObservation* observation=nullptr)noexcept {
    const auto owner=Owner(p.entry,p.size);
    const auto fail=[&](const char* predicate,unsigned offset=UINT_MAX){
        if(observation && !observation->captured){
            observation->predicate=predicate;observation->captured=true;
            observation->owner=owner;observation->size=p.size;
            observation->installPreimageCaptured=p.installPreimageCaptured;
            observation->installGeneration=p.installGeneration;
            observation->restoreMatchesInstall=p.installPreimageCaptured && p.size<=p.installPreimage.size() &&
                !std::memcmp(p.installPreimage.data(),p.code.data()+32,p.size);
            std::memcpy(&observation->savedPrefix,p.code.data()+32,(std::min)(size_t(p.size),sizeof(observation->savedPrefix)));
            const auto prefixSize=(std::min)(size_t(p.size),sizeof(observation->cleanPrefix));
            if(owner>=0 && Ledger().images[p.image].Range(p.rva,prefixSize))
                std::memcpy(&observation->cleanPrefix,Ledger().images[p.image].clean.data()+p.rva,prefixSize);
            if(offset<p.installPreimage.size() && p.installPreimageCaptured)
                observation->installByte=p.installPreimage[offset];
            observation->rva=owner>=0?p.rva:0;observation->mismatchOffset=offset;
            if(offset!=UINT_MAX){observation->savedByte=p.code[32+offset];
                observation->cleanByte=Ledger().images[p.image].clean[p.rva+offset];}
            observation->failureGeneration=store.generation.load();
            observation->busyAtFailure=store.busy.test(std::memory_order_acquire);
        }
        return false;
    };
    if(owner<0)return fail("setup-patch-owner-range");
    p.image=static_cast<unsigned>(owner);
    p.rva=static_cast<uint32_t>(p.entry-reinterpret_cast<unsigned char*>(Ledger().images[p.image].live));
    const auto& image=Ledger().images[p.image];
    if(!image.SectionHas(p.rva,p.size,IMAGE_SCN_MEM_EXECUTE))return fail("setup-patch-executable-section");
    if(!p.installPreimageCaptured)return fail("setup-patch-install-preimage-missing");
    if(p.size>p.installPreimage.size() || std::memcmp(p.installPreimage.data(),p.code.data()+32,p.size))
        return fail("setup-patch-install-restore-mismatch");
    if(!PatchPreimage(p,image)){
        if(p.predecessor.captured)return fail("setup-factory-composition-changed");
        // Both operands belong to this private draft, not a later live reread.
        unsigned offset=0;while(offset<p.size && p.code[32+offset]==image.clean[p.rva+offset])++offset;
        return fail("setup-patch-preimage-mismatch",offset);
    }
    if(p.predecessor.captured && !p.factoryCleanCaptured){
        std::memcpy(p.factoryCleanStart.data(),image.clean.data()+p.rva,p.factoryCleanStart.size());
        p.factoryCleanCaptured=true;
    }
    return true;
}
inline bool CaptureNativeHops(Patch& p,void* requested,void* target)noexcept {
    p.requested=requested;void* next=requested;
    while(next!=target){
        if(p.hopCount==p.hops.size())return false;
        auto& h=p.hops[p.hopCount++];h.entry=static_cast<unsigned char*>(next);
        if(!Memory(h.entry,h.code.data(),h.code.size()))return false;
        if(h.code[0]==0xff && h.code[1]==0x25){
            h.size=6;h.iat=h.entry+6+Read<int32_t>(h.code.data()+2);
            if(!Memory(h.iat,&h.target,sizeof(h.target)))return false;
        }else if(h.code[0]==0xe9){h.size=5;h.target=h.entry+5+Read<int32_t>(h.code.data()+1);}
        else if(h.code[0]==0xeb){h.size=2;h.target=h.entry+2+static_cast<int8_t>(h.code[1]);}
        else return false;
        if(!h.target || h.target==next)return false;next=h.target;
    }return true;
}
inline bool PatchIntact(const Patch& p)noexcept {
    if(!IndependentInputIntact(p))return false;
    if(p.predecessor.captured && !FactoryPredecessorIntact(p.predecessor,p.size))return false;
    void* current=nullptr;
    if(!Memory(p.original,&current,sizeof(current)) || current!=p.trampoline)return false;
    for(auto* slot:{p.executed,p.mirror})if(slot &&
        (!Memory(slot,&current,sizeof(current)) || current!=p.trampoline))return false;
    for(unsigned i=0;i<p.hopCount;++i){const auto& h=p.hops[i];
        if(!Equal(h.entry,h.code.data(),h.size) ||
           (h.iat && (!Memory(h.iat,&current,sizeof(current)) || current!=h.target)))return false;
    }
    return
        (!p.installPreimageCaptured || (p.size<=p.installPreimage.size() &&
         !std::memcmp(p.installPreimage.data(),p.code.data()+32,p.size))) &&
        Equal(p.entry,p.edit.data(),p.size) && Equal(p.trampoline,p.code.data(),p.code.size()) &&
        Equal(p.hook,p.ownEntry.data(),p.ownEntry.size());
}
inline bool FinalizeOwnPatch(Patch& p)noexcept {
    const auto displacement=reinterpret_cast<intptr_t>(p.trampoline+88)-reinterpret_cast<intptr_t>(p.entry+5);
    if(displacement<INT32_MIN || displacement>INT32_MAX)return false;
    p.edit.fill(0xcc);p.edit[0]=0xe9;const auto relative=static_cast<int32_t>(displacement);
    std::memcpy(p.edit.data()+1,&relative,4);
    // Commit changes only rbCodeIn[0..5] in this supported layout.
    // All other trampoline bytes must still equal the validated
    // precommit image; never adopt a postcommit rbCode snapshot.
    p.code[88]=0xff;p.code[89]=0x25;const int32_t relay=-14;
    std::memcpy(p.code.data()+90,&relay,4);
    const auto priorRemainder=Read<uintptr_t>(p.code.data()+72);
    if(priorRemainder!=reinterpret_cast<uintptr_t>(p.entry+p.size) ||
       Read<uintptr_t>(p.code.data()+80)!=reinterpret_cast<uintptr_t>(p.hook)) return false;
    return PatchIntact(p);
}
inline bool StableLocked()noexcept {
    return store.initialized.load()==2 && !Revoked() && !(store.generation.load()&1);
}
// Original no-argument StableLocked is intentionally unchanged.
template<class Observation> inline bool StableLocked(Observation& o)noexcept {
    const auto initialized=store.initialized.load();o.Initialized(initialized);
    if(!o.Leaf("native-initialized-2",initialized==2))return false;
    if(Revoked(o))return false;
    return o.Leaf("native-generation-even",!(store.generation.load()&1));
}
inline bool QuickLockedFull()noexcept {
    ScpValidationPass scpPass;
    using CpuStage=GenshinCpuStageTiming::Stage;
    if(!StableLocked())return false;
    {GenshinCpuStageTiming::Scope cost(CpuStage::Representation);
     bool profile=false,file=false,delayed=false;
     {GenshinCpuStageTiming::Scope subcost(CpuStage::Profile);profile=shim_binding::ProfileIntact(false);}
     {GenshinCpuStageTiming::Scope subcost(CpuStage::File);file=profile && file_binding::Intact();}
     {GenshinCpuStageTiming::Scope subcost(CpuStage::Delayed);delayed=profile && file && file_binding::DelayedIntact();}
     if(!delayed){Revoke("native-shim-changed");return false;}}
    // R634: no own-patch exception or later override may cover dynamic operands.
    {GenshinCpuStageTiming::Scope cost(CpuStage::Fixups);
     for(unsigned i=0;i<Ledger().imageCount;++i)if(!Ledger().images[i].MatchDynamicFixups()){
        Revoke("dynamic-target-changed",Ledger().images[i].live);return false;
     }}
    {GenshinCpuStageTiming::Scope cost(CpuStage::Patches);
     for(unsigned i=0;i<Ledger().patchCount;++i)if(!PatchIntact(Ledger().patches[i]))return false;}
    GenshinCpuStageTiming::Scope importCost(CpuStage::Imports);
    // Coalesce contiguous IAT receipts: bounded stack buffers, no allocation,
    // disk, signature validation, hashing or repeated per-alias global scan.
    for(unsigned i=0;i<Ledger().importCount;){
        std::array<uintptr_t,256> expected{},current{};const auto* first=Ledger().imports[i].slot;unsigned count=0;
        do {if(!ExpectedImportTarget(i,expected[count]))return false;++count;++i;}
        while(i<Ledger().importCount && count<expected.size() && reinterpret_cast<uintptr_t>(Ledger().imports[i].slot)==
            reinterpret_cast<uintptr_t>(first)+count*sizeof(uintptr_t));
        if(!Memory(first,current.data(),count*sizeof(uintptr_t)) ||
           std::memcmp(expected.data(),current.data(),count*sizeof(uintptr_t)))return false;
    }return true;
}
// The existing compiled plan compares the sealed five-byte references but has
// no kernel-query operation. Keep that optimization intact behind an explicit
// derived-source guard, including on the cached fast path.
#define QuickLocked QuickLockedPlan
#include "GenshinNativeValidationPlan.inl"
#undef QuickLocked
inline bool QuickLocked()noexcept {
    GenshinCpuStageTiming::Scope cost(GenshinCpuStageTiming::Stage::Native);
    if(!StableLocked() || Ledger().imageCount>Ledger().images.size())return false;
    ScpValidationPass scpPass;
    const ScpSource* previous=nullptr;
    for(unsigned i=0;i<Ledger().imageCount;++i){auto& image=Ledger().images[i];
        if(image.Scp() && image.Scp().get()!=previous){
            if(!image.MatchScpSource()){Revoke("scp-source-or-extension-changed",image.live);return false;}
            previous=image.Scp().get();
        }
    }
    // The compiled byte plan already retains every input edit/trampoline/alias.
    // Recheck its outside-closure/table/lifetime boundary even on the fast path.
    for(unsigned i=0;i<Ledger().patchCount;++i)if(!IndependentInputIntact(Ledger().patches[i]))return false;
    return QuickLockedPlan();
}
// The launcher/ReShade chain may already hook CreateProcessInternalW before Opt is
// injected. This is not a trusted callable target and is never invoked by this
// helper. Only that exported auxiliary process entry's existing five-byte jump
// is recorded; all other bytes and any later target change remain fatal. Native
// factory, D3D11 query and resource gates are unchanged.
inline size_t FirstCodeDifference(const unsigned char* live,const unsigned char* expected,size_t size)noexcept {
    std::array<unsigned char,4096> actual{};size_t offset=0;
    while(offset<size){const auto take=(std::min)(actual.size(),size-offset);
        if(!Memory(live+offset,actual.data(),take))return SIZE_MAX;
        for(size_t i=0;i<take;++i)if(actual[i]!=expected[offset+i])return offset+i;
        offset+=take;
    }
    return size;
}
inline bool AuxiliaryProcessHookLocked(Image& image,uintptr_t entry,const unsigned char* live,
                                       const unsigned char* expected,size_t size,size_t offset)noexcept {
    if(!image.live || entry<sizeof(void*) || offset>size || size-offset<5 ||
       GetProcAddress(image.live,"CreateProcessInternalW")!=reinterpret_cast<const void*>(entry))return false;
    const auto rva=static_cast<uint32_t>(entry-reinterpret_cast<uintptr_t>(image.live));
    if(image.auxiliaryHookRva && (image.auxiliaryHookRva!=rva || image.auxiliaryHookTarget==0))return false;
    if(image.auxiliaryHookRva && std::memcmp(live+offset,image.auxiliaryHookBytes.data(),5))return false;
    for(unsigned i=0;i<Ledger().patchCount;++i)if(Ledger().patches[i].entry==reinterpret_cast<unsigned char*>(entry))return false;
    if(FirstCodeDifference(live,expected,offset)!=offset ||
       FirstCodeDifference(live+offset+5,expected+offset+5,size-offset-5)!=size-offset-5)return false;
    std::array<unsigned char,5> bytes{};if(!Memory(live+offset,bytes.data(),bytes.size()) || bytes[0]!=0xe9)return false;
    const auto target=entry+5+Read<int32_t>(bytes.data()+1);
    if(image.auxiliaryHookRva && image.auxiliaryHookTarget!=target)return false;
    MEMORY_BASIC_INFORMATION memory{};
    if(!VirtualQuery(reinterpret_cast<const void*>(target),&memory,sizeof(memory)) ||
       memory.Type!=MEM_PRIVATE || memory.State!=MEM_COMMIT || (memory.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
    const auto protection=memory.Protect&0xff;
    if(protection!=PAGE_EXECUTE && protection!=PAGE_EXECUTE_READ && protection!=PAGE_EXECUTE_READWRITE &&
       protection!=PAGE_EXECUTE_WRITECOPY)return false;
    if(!image.auxiliaryHookRva){
        image.auxiliaryHookRva=rva;image.auxiliaryHookTarget=target;image.auxiliaryHookBytes=bytes;
    }
    return true;
}
inline bool WholeCodeLocked() {
    ScpValidationPass scpPass;
    for(unsigned i=0;i<Ledger().patchCount;++i)if(!IndependentInputIntact(Ledger().patches[i]))return false;
    for(unsigned i=0;i<Ledger().imageCount;++i){auto& image=Ledger().images[i];
        if(!image.MatchDynamicFixups()){Revoke("dynamic-target-changed",image.live);return false;}
        for(unsigned j=0;j<image.sectionCount;++j){const auto& section=image.sections[j];
            if(!(section.flags&IMAGE_SCN_MEM_EXECUTE))continue;
            std::vector<unsigned char> expected(image.clean.begin()+section.rva,image.clean.begin()+section.rva+section.size);
            for(unsigned k=0;k<Ledger().patchCount;++k){const auto& p=Ledger().patches[k];if(p.image!=i)continue;
                if(p.rva>=section.rva && Span(p.rva-section.rva,p.size,section.size))
                    std::memcpy(expected.data()+p.rva-section.rva,p.edit.data(),p.size);
            }
            const auto* live=reinterpret_cast<unsigned char*>(image.live)+section.rva;
            if(!Equal(live,expected.data(),expected.size())){
                const auto offset=FirstCodeDifference(live,expected.data(),expected.size());
                if(offset==SIZE_MAX || offset==expected.size() ||
                   !AuxiliaryProcessHookLocked(image,reinterpret_cast<uintptr_t>(image.live)+section.rva+offset,
                                               live,expected.data(),expected.size(),offset)){
                    Revoke("untracked-executable-bytes",image.live);return false;
                }
            }
        }
    }return true;
}
inline bool WholeLocked() {
    if(!shim_binding::Intact() || !file_binding::DelayedIntact()){Revoke("native-shim-changed");return false;}
    return WholeCodeLocked();
}
// R193 observed overloads are selected ONLY by PrepareCoverage's explicit
// startup receipt. The original Whole/WholeCode and their callers stay above.
// Deep native authentication helpers retain their actual bool boundaries;
// they are never re-run or replaced by sampled interpretations.
template<class Observation> inline bool WholeCodeLocked(Observation& o) {
    ScpValidationPass scpPass;
    for(unsigned i=0;i<Ledger().patchCount;++i){
        o.At("WholeCodeLocked");o.patch=static_cast<int>(i);
        if(!o.Leaf("whole-IndependentInputIntact",IndependentInputIntact(Ledger().patches[i])))return false;
    }
    for(unsigned i=0;i<Ledger().imageCount;++i){auto& image=Ledger().images[i];
        o.At("WholeCodeLocked");o.image=static_cast<int>(i);
        if(!o.Leaf("whole-MatchDynamicFixups",image.MatchDynamicFixups())){
            Revoke("dynamic-target-changed",image.live);return false;
        }
        for(unsigned j=0;j<image.sectionCount;++j){const auto& section=image.sections[j];
            if(!(section.flags&IMAGE_SCN_MEM_EXECUTE))continue;
            o.At("WholeCodeLocked",section.rva);o.image=static_cast<int>(i);o.section=static_cast<int>(j);
            o.BeforeLeaf("whole-expected-section-allocation");
            std::vector<unsigned char> expected(image.clean.begin()+section.rva,image.clean.begin()+section.rva+section.size);
            for(unsigned k=0;k<Ledger().patchCount;++k){const auto& p=Ledger().patches[k];if(p.image!=i)continue;
                if(p.rva>=section.rva && Span(p.rva-section.rva,p.size,section.size))
                    std::memcpy(expected.data()+p.rva-section.rva,p.edit.data(),p.size);
            }
            const auto* live=reinterpret_cast<unsigned char*>(image.live)+section.rva;
            if(!Equal(live,expected.data(),expected.size(),o)){
                // These are the ORIGINAL fallback calls, not diagnostic retries.
                o.BeforeLeaf("whole-FirstCodeDifference");
                const auto offset=FirstCodeDifference(live,expected.data(),expected.size());o.offset=offset;
                if(!o.Leaf("whole-first-difference-readable",offset!=SIZE_MAX)){
                    Revoke("untracked-executable-bytes",image.live);return false;
                }
                if(!o.Leaf("whole-first-difference-present",offset!=expected.size())){
                    Revoke("untracked-executable-bytes",image.live);return false;
                }
                o.BeforeLeaf("whole-AuxiliaryProcessHookLocked");
                if(!o.Leaf("whole-AuxiliaryProcessHookLocked",AuxiliaryProcessHookLocked(image,
                    reinterpret_cast<uintptr_t>(image.live)+section.rva+offset,
                    live,expected.data(),expected.size(),offset))){
                    Revoke("untracked-executable-bytes",image.live);return false;
                }
            }
        }
    }return true;
}
template<class Observation> inline bool WholeLocked(Observation& o) {
    o.At("WholeLocked");
    if(!o.Leaf("whole-shim-Intact",shim_binding::Intact())){
        Revoke("native-shim-changed");return false;
    }
    if(!o.Leaf("whole-file-DelayedIntact",file_binding::DelayedIntact())){
        Revoke("native-shim-changed");return false;
    }
    return WholeCodeLocked(o);
}
inline bool SealRepresentations() {
    auto& s=Ledger().shim;
    if(!file_binding::Seal()){Revoke("native-file-family-seal");return false;}
    if(s.state==RepresentationState::Absent)return !RepresentationCount();
    const auto fail=[](){Revoke("native-shim-final-seal");return false;};
    if(Revoked() || store.initialized.load()!=1 || s.state!=RepresentationState::Building ||
       s.edgeCount<3 || s.edgeCount>s.edges.size() || s.fileCount>s.fileEdges.size() ||
       Ledger().importCount>Ledger().imports.size() || RepresentationCount()>Ledger().imports.size()-Ledger().importCount)return fail();
    std::array<RoleForm,5> roles{};
    if(!shim_binding::ObligationInventory(s.apphelp,s.native) || !shim_binding::RoleSet(roles))return fail();
    for(unsigned i=0;i<Ledger().imageCount;++i)
        if(Ledger().imageCollection[i]!=ImageCollection::Complete || !CoverageComplete(Ledger().imageCoverage[i]))return fail();
    // Complete clean code covers wrapper side helpers/cookie failure paths and
    // every trusted image. Pending edges remain noncallable until this returns.
    if(!WholeCodeLocked())return fail();
    for(unsigned i=0;i<Ledger().importCount;++i){const auto& e=Ledger().imports[i];uintptr_t now=0;
        if(!Memory(e.slot,&now,sizeof(now)) || now!=e.target)return fail();
    }
    shim_binding::Layout f{};if(!shim_binding::ReadLayout(s.apphelp,s.native,f))return fail();
    for(unsigned i=0;i<s.edgeCount;++i){const auto& e=s.edges[i];uintptr_t now=0;
        const CanonicalBinding canonical{s.native,shim_binding::NativeRva(),ImportKind::CodeBinding};
        if(!shim_binding::Selector(e.dll,e.name,e.ordinal,e.importer,e.slot,canonical,s.apphelp,s.native) ||
           !RoleFormValid(e.role,e.form,e.iatTarget,e.resolverTarget,f.native+shim_binding::NativeRva(),f.apphelp+shim_binding::Recipe().wrapper.rva) ||
           !Memory(e.slot,&now,sizeof(now)) || now!=e.iatTarget ||
           reinterpret_cast<uintptr_t>(GetProcAddress(s.native,e.name))!=e.resolverTarget)return fail();
    }
    s.table=f.table;s.allocation=f.allocation;s.region=f.region;s.regionSize=f.regionSize;s.protect=f.protect;
    if(!shim_binding::DraftIntact() || !WholeCodeLocked())return fail();
    s.state=RepresentationState::Validated;
    return true;
}
inline bool FreezeClosure()noexcept {
    auto& s=Ledger().shim;
    if(Revoked() || Ledger().imageCount>Ledger().images.size() ||
       Ledger().importCount+RepresentationCount()>Ledger().imports.size() || Ledger().imageBytes>128*1024*1024 ||
       Ledger().patchCount>Ledger().patches.size())return false;
    uint64_t regularDescriptors=0,regularSlots=0,delayDescriptors=0,delaySlots=0;
    for(unsigned i=0;i<Ledger().imageCount;++i){
        if(Ledger().imageCollection[i]!=ImageCollection::Complete || !CoverageComplete(Ledger().imageCoverage[i]))return false;
        const auto& c=Ledger().imageCoverage[i];s.frozenImageStates[i]=Ledger().imageCollection[i];s.frozenCoverage[i]=c;
        regularDescriptors+=c.regularDescriptors;regularSlots+=c.regularSlots;
        delayDescriptors+=c.delayDescriptors;delaySlots+=c.delaySlots;
    }
    if(regularDescriptors>UINT32_MAX || regularSlots>UINT32_MAX ||
       delayDescriptors>UINT32_MAX || delaySlots>UINT32_MAX)return false;
    if(regularSlots+delaySlots!=uint64_t(Ledger().importCount)+RepresentationCount())return false;
    s.frozenFileCount=s.fileCount;
    s.frozenImageCount=Ledger().imageCount;s.frozenImportCount=Ledger().importCount;
    s.frozenPatchCount=Ledger().patchCount;s.frozenImageBytes=Ledger().imageBytes;
    s.frozenRegularDescriptors=static_cast<uint32_t>(regularDescriptors);
    s.frozenRegularSlots=static_cast<uint32_t>(regularSlots);
    s.frozenDelayDescriptors=static_cast<uint32_t>(delayDescriptors);
    s.frozenDelaySlots=static_cast<uint32_t>(delaySlots);
    return true;
}
inline bool FrozenClosureIntact()noexcept {
    const auto& s=Ledger().shim;
    if(s.frozenFileCount!=s.fileCount || s.fileCount>s.fileEdges.size() ||
       s.frozenImageCount!=Ledger().imageCount || s.frozenImportCount!=Ledger().importCount ||
       s.frozenPatchCount!=Ledger().patchCount || s.frozenImageBytes!=Ledger().imageBytes ||
       Ledger().importCount+RepresentationCount()>Ledger().imports.size())return false;
    uint64_t rd=0,rs=0,dd=0,ds=0;
    for(unsigned i=0;i<Ledger().imageCount;++i){const auto& c=Ledger().imageCoverage[i];
        const auto& f=s.frozenCoverage[i];
        if(Ledger().imageCollection[i]!=ImageCollection::Complete || s.frozenImageStates[i]!=Ledger().imageCollection[i] ||
           !CoverageComplete(c) || c.regularDescriptors!=f.regularDescriptors || c.regularSlots!=f.regularSlots ||
           c.delayDescriptors!=f.delayDescriptors || c.delaySlots!=f.delaySlots ||
           c.regularTerminated!=f.regularTerminated || c.delayTerminated!=f.delayTerminated)return false;
        rd+=c.regularDescriptors;rs+=c.regularSlots;dd+=c.delayDescriptors;ds+=c.delaySlots;
    }
    return rd==s.frozenRegularDescriptors && rs==s.frozenRegularSlots &&
        dd==s.frozenDelayDescriptors && ds==s.frozenDelaySlots;
}
inline bool PublishRepresentations() {
    auto& s=Ledger().shim;
    if(Revoked() || store.initialized.load()!=1 ||
       !FrozenClosureIntact() || !WholeCodeLocked())return false;
    for(unsigned i=0;i<Ledger().importCount;++i){const auto& e=Ledger().imports[i];uintptr_t now=0;
        if(!Memory(e.slot,&now,sizeof(now)) || now!=e.target || e.binding.kind==ImportKind::None)return false;
    }
    if(s.state==RepresentationState::Absent){if(RepresentationCount())return false;return true;}
    if(s.state!=RepresentationState::Validated || s.edgeCount<3 || s.edgeCount>s.edges.size() || !shim_binding::Intact())return false;
    for(unsigned i=0;i<s.edgeCount;++i){uintptr_t now=0;const auto& e=s.edges[i];
        if(!Memory(e.slot,&now,sizeof(now)) || now!=e.iatTarget)return false;
    }
    // This is the final non-atomic profile write. All fallible validation has completed.
    s.state=RepresentationState::Sealed;
    return true;
}
// R701/R792: only a previously recorded signed native lazy selector can
// advance once to its independently canonical authenticated code binding.
// The same full proof is used at birth, hook-transaction entry and runtime
// receipt checks. It never issues a birth witness or promotes query history.
// Guard is held by the caller. Failure is sticky; no revoked ledger reopens.
// The unchanged fast path performs no file/signature/resolver work. A genuine
// one-way delay transition takes the full existing code/closure/seal proof.
inline bool PrepareNativeBirthLocked()noexcept {
    if(!StableLocked())return false;
    if(QuickLocked())return true;
    if(!StableLocked())return false; // In particular, never recover a revoked ledger.
    // R793: this rare slow path retains a first failure observation from
    // the actual resolver invocation, including its exact imported IAT slot.
    // It is not consulted by any acceptance gate and restores caller TLS.
    struct ObserveDelayImports {
        const bool previous=observeSetupImports;
        ObserveDelayImports()noexcept {observeSetupImports=true;}
        ~ObserveDelayImports(){observeSetupImports=previous;}
    } observeDelayImports;
    const auto fail=[](const char* reason){Revoke(reason);return false;};
    try {
        const auto generation=store.generation.load();
        // First authenticate existing code/patches. QuickLocked may have failed
        // because of an IAT mismatch; it is not permission to ignore any other edit.
        for(unsigned i=0;i<Ledger().patchCount;++i)if(!PatchIntact(Ledger().patches[i]))
            return fail("birth-entry-patch-changed");
        if(!WholeLocked())return fail("birth-entry-whole-before");
        struct Advance {unsigned index;uintptr_t before,after;CanonicalBinding binding;FileRepresentedEdge represented{};bool noncallable=false;};
        std::vector<Advance> pending;
        const auto failIat=[&](const char* reason,const auto& item,uintptr_t expected,uintptr_t actual,bool read){
            ImportFailureContext c{};c.dll=item.dll;c.symbol=item.name;c.ordinal=item.ordinal;
            c.importer=item.importer;c.slot=item.slot;c.expected=expected;c.expectedKnown=true;
            c.actual=actual;c.actualKnown=read;PinTerminalImportFailure(reason,c);return fail(reason);
        };
        // R827 phase17: one monotonic transaction, including transitions that
        // arrive during complete proof. Pending after-values never retarget;
        // only an unstaged original signed lazy selector can be authenticated.
        const auto collect=[&](bool& expanded)->bool {
            for(unsigned i=0;i<Ledger().importCount;++i){const auto& item=Ledger().imports[i];
                uintptr_t live=0;if(!Memory(item.slot,&live,sizeof(live)))return fail("birth-entry-IAT-read");
                const auto staged=std::lower_bound(pending.begin(),pending.end(),i,
                    [](const Advance& change,unsigned index){return change.index<index;});
                if(staged!=pending.end() && staged->index==i){
                    if(live!=staged->after)return failIat("birth-entry-staged-IAT-changed",item,staged->after,live,true);
                    continue; // Never retarget a binding already authenticated in this transaction.
                }
                uintptr_t expected=0;if(!ExpectedImportTarget(i,expected))return fail("delay-observation-index");
                if(live==expected)continue;
                if(Ledger().delayedIndex[i])return fail("represented-delay-changed");
                if(!item.lazy || !item.dll || !*item.dll || (!item.name && !item.ordinal) ||
                   item.binding.kind!=ImportKind::DelayThunk || item.binding.owner!=item.importer)
                    return fail("birth-entry-nonlazy-or-selector");
                const auto owner=Owner(item.slot,sizeof(uintptr_t));if(owner<0)return fail("birth-entry-slot-owner");
                const auto& image=Ledger().images[owner];
                const auto slotRva=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(item.slot)-reinterpret_cast<uintptr_t>(image.live));
                if(image.live!=item.importer || Read<uintptr_t>(image.clean.data()+slotRva)!=item.target ||
                   item.target!=reinterpret_cast<uintptr_t>(item.binding.owner)+item.binding.rva ||
                   !Executable(reinterpret_cast<void*>(item.target)))return fail("birth-entry-lazy-preimage");
                wchar_t wide[512]{};
                if(!MultiByteToWideChar(CP_ACP,MB_ERR_INVALID_CHARS,item.dll,-1,wide,512))return fail("birth-entry-selector-encoding");
                auto nominal=ApiContract(item.dll)?nullptr:GetModuleHandleW(wide);
                if(!nominal && _strnicmp(item.dll,"api-",4) && _strnicmp(item.dll,"ext-",4))
                    return fail("birth-entry-selector-module");
                CanonicalBinding binding{};FileRepresentedEdge represented{};
                ImportFailureContext observation{};observation.dll=item.dll;observation.symbol=item.name;observation.ordinal=item.ordinal;
                observation.nominal=nominal;observation.importer=item.importer;observation.slot=item.slot;observation.actual=live;observation.actualKnown=true;
                // Canonical code and the separate exact system-wrapper observation
                // are distinct outcomes. Neither an alias nor an arbitrary IAT can
                // create a callable binding through the represented branch.
                const auto result=ResolveImport(item.dll,item.name,item.ordinal,nominal,binding,item.importer,&observation,false,&represented);
                const bool noncallable=result==ImportResolution::Deferred;
                if(noncallable){if(!file_binding::DelayedProof(i,represented))return fail("represented-delay-proof");}
                else if(result!=ImportResolution::Canonical || binding.kind!=ImportKind::CodeBinding || !ImportOwner(live,binding))
                    return fail("birth-entry-canonical-code");
                pending.insert(std::lower_bound(pending.begin(),pending.end(),i,
                    [](const Advance& change,unsigned index){return change.index<index;}),
                    Advance{i,item.target,live,binding,represented,noncallable});
                expanded=true;
            }
            return true;
        };
        const auto prove=[&]()->bool {
            for(unsigned i=0;i<Ledger().patchCount;++i)if(!PatchIntact(Ledger().patches[i]))
                return fail("birth-entry-patch-after-resolution");
            // ResolveCanonicalImport may have authenticated additional images and
            // their imports. Seal that complete closure BEFORE publishing any advance.
            if(!WholeLocked())return fail("birth-entry-whole-after-resolution");
            uint64_t covered=0;
            for(unsigned i=0;i<Ledger().imageCount;++i){const auto& c=Ledger().imageCoverage[i];
                if(Ledger().imageCollection[i]!=ImageCollection::Complete || !CoverageComplete(c))return fail("runtime-import-closure-incomplete");
                covered+=uint64_t(c.regularSlots)+c.delaySlots;
            }
            if(covered!=uint64_t(Ledger().importCount)+RepresentationCount())return fail("runtime-import-coverage-conservation");
            unsigned additional=0;
            for(const auto& change:pending)if(change.noncallable){
                if(!file_binding::DelayedProof(change.index,change.represented))return fail("represented-delay-final-proof");++additional;}
            if(Ledger().delayedCount>Ledger().delayedEdges.size() || additional>Ledger().delayedEdges.size()-Ledger().delayedCount)return fail("represented-delay-capacity");
            return true;
        };
        bool settled=false;
        for(unsigned pass=0;pass<16;++pass){
            const auto images=Ledger().imageCount,imports=Ledger().importCount,
                representations=RepresentationCount(),delayed=Ledger().delayedCount;
            bool expanded=false;if(!collect(expanded))return false;
            if(!StableLocked() || store.generation.load()!=generation)return fail("birth-entry-epoch-changed");
            // A failed proof is terminal. It is never reclassified as a lazy advance.
            if(!prove())return false;
            if(!StableLocked() || store.generation.load()!=generation)return fail("birth-entry-epoch-changed");
            // Full code/profile proof may overlap a genuine loader transition.
            // Authenticate that new edge through the very same original proof,
            // then require another complete round before any publication.
            bool afterProof=false;if(!collect(afterProof))return false;
            if(!StableLocked() || store.generation.load()!=generation)return fail("birth-entry-epoch-changed");
            if(!expanded && !afterProof && images==Ledger().imageCount && imports==Ledger().importCount &&
               representations==RepresentationCount() && delayed==Ledger().delayedCount){settled=true;break;}
        }
        if(!settled)return fail("birth-entry-delay-closure-did-not-settle");
        if(!StableLocked() || store.generation.load()!=generation)return fail("birth-entry-epoch-changed");
        unsigned next=0;
        for(unsigned i=0;i<Ledger().importCount;++i){const auto& item=Ledger().imports[i];
            const auto* change=next<pending.size() && pending[next].index==i?&pending[next]:nullptr;
            uintptr_t expected=0;if(change)expected=change->after;
            else if(!ExpectedImportTarget(i,expected))return fail("delay-final-observation-index");
            uintptr_t live=0;
            const bool read=Memory(item.slot,&live,sizeof(live));
            if(!read || live!=expected)return failIat("birth-entry-IAT-changed-during-resolution",item,expected,live,read);
            if(change){if(!item.lazy || item.target!=change->before)return fail("birth-entry-receipt-changed");++next;}
        }
        if(next!=pending.size())return fail("birth-entry-advance-count");
        ++Ledger().validationRevision;Ledger().validationPlan.reset();
        for(const auto& change:pending){auto& item=Ledger().imports[change.index];
            if(change.noncallable){auto& l=Ledger();const auto slot=l.delayedCount++;
                l.delayedEdges[slot]={change.index,change.represented};l.delayedIndex[change.index]=static_cast<unsigned char>(slot+1);
            }else {item.target=change.after;item.binding=change.binding;item.lazy=false;}
        }
        // No success is returned from a partial publication. If this final seal
        // fails the ledger remains permanently revoked, including new images.
        // WholeLocked is fallible proof work too: exact equality must also hold
        // after its last read, with no resolver or baseline adoption afterward.
        if(!QuickLocked() || !WholeLocked() || !QuickLocked() || !StableLocked() || store.generation.load()!=generation)
            return fail("birth-entry-final-seal");
        return true;
    }catch(...){return fail("birth-entry-exception");}
}

inline const Patch* Find(const void* entry)noexcept {
    for(unsigned i=0;i<Ledger().patchCount;++i)if(Ledger().patches[i].entry==entry)return &Ledger().patches[i];return nullptr;
}
struct Pending {std::array<Patch,64> patches{};unsigned count=0;bool held=false;uint64_t from=0;
    bool optional=false; // Isolated optimization install; never changes provenance refusal policy.
    FactoryProtectionLease* factoryLease=nullptr;
};
inline thread_local Pending pending;
inline bool FactoryProtectionLease::Bound()const noexcept {
    return batch_ && thread_==GetCurrentThreadId() && pending.held && pending.factoryLease==this &&
        generation_==store.generation.load() && (generation_&1);
}
class FactoryLeaseAccess final {
public:
    static bool Prepare(FactoryProtectionLease& lease,const void* batch,
                        const FactoryPredecessor* receipts,unsigned count)noexcept {
        if(!count)return true;
        if(!receipts || count>3)return false;
        bool needed=false;for(unsigned i=0;i<count;++i)needed=needed || receipts[i].captured;
        if(!needed)return true;
        if(!pending.held || pending.factoryLease || pending.count || Revoked())return false;
        SYSTEM_INFO info{};GetSystemInfo(&info);const auto size=size_t(info.dwPageSize);
        if(!size || (size&(size-1)))return false;
        lease.pageSize_=size;lease.batch_=batch;lease.thread_=GetCurrentThreadId();
        lease.generation_=store.generation.load();
        // Validate the whole batch before the first page changes/splits.
        for(unsigned i=0;i<count;++i){const auto& r=receipts[i];if(!r.captured)continue;
            const auto& n=r.nodes[0];const auto& m=n.mapping;
            if(r.thread!=lease.thread_ || r.generation==UINT64_MAX || r.generation+1!=lease.generation_ ||
               m.protect!=PAGE_EXECUTE_READ || m.state!=MEM_COMMIT || m.type!=MEM_IMAGE ||
               m.allocation!=reinterpret_cast<uintptr_t>(r.module) ||
               r.nodes[2].mapping.allocation==m.allocation || !FactoryPredecessorIntact(r) ||
               n.address>UINTPTR_MAX-n.bytes.size() || m.region>UINTPTR_MAX-m.size)return false;
            const auto end=n.address+n.bytes.size();
            for(auto at=n.address&~uintptr_t(size-1);at<end;at+=size){
                if(at<m.region || at>UINTPTR_MAX-size || at+size>m.region+m.size)return false;
                bool found=false;for(unsigned j=0;j<lease.count_;++j)if(lease.pages_[j].address==at)found=true;
                if(found)continue;
                if(lease.count_==lease.pages_.size())return false;
                lease.pages_[lease.count_++]={at,m,0,false};
            }
        }
        pending.factoryLease=&lease;
        for(unsigned i=0;i<lease.count_;++i){auto& p=lease.pages_[i];DWORD old=0;
            if(!VirtualProtect(reinterpret_cast<void*>(p.address),size,PAGE_EXECUTE_READWRITE,&old))return false;
            p.original=old;p.held=true;
            if(old!=p.mapping.protect)return false;
        }
        for(unsigned i=0;i<count;++i)if(receipts[i].captured &&
            !FactoryPredecessorIntact(receipts[i],0,&lease))return false;
        return true;
    }
    static bool Restore(FactoryProtectionLease& lease)noexcept {
        bool good=true;
        // Detours may have saved RWX as oldProtect. Always restore OUR old value
        // after its Commit/Abort, once per deduplicated page, even on failure.
        for(unsigned i=0;i<lease.count_;++i){auto& p=lease.pages_[i];if(!p.held)continue;DWORD ignored=0;
            if(!VirtualProtect(reinterpret_cast<void*>(p.address),lease.pageSize_,p.original,&ignored))good=false;
            p.held=false;
        }
        for(unsigned i=0;i<lease.count_;++i){const auto& p=lease.pages_[i];
            if(p.original && !FactoryMappingIntact(p.mapping,p.address))good=false;
        }
        if(pending.factoryLease==&lease)pending.factoryLease=nullptr;
        lease.batch_=nullptr;return good;
    }
};
inline thread_local unsigned factoryDepth=0;
inline thread_local bool nativeSetupWindow=false;
inline thread_local uint64_t reentryEpoch=0;
} // namespace detail

inline void Revoke()noexcept {detail::LiveLedgerScope live;detail::Revoke();}
inline const char* Failure()noexcept {
    if(!detail::store.revoked.load(std::memory_order_acquire) &&
       detail::contextExecutionRefused.load(std::memory_order_acquire))return "native-context-execution-state";
    return detail::PublishedFailure();
}
inline HMODULE FailureModule()noexcept {
    return detail::firstRevocationState.load(std::memory_order_acquire)==2?detail::firstRevocation.module:nullptr;
}
inline const detail::FirstRevocationReceipt* FirstRevocation()noexcept {
    return detail::firstRevocationState.load(std::memory_order_acquire)==2?&detail::firstRevocation:nullptr;
}
inline void SetFirstRevocationObserver(detail::FirstRevocationObserver observer)noexcept {
    detail::firstRevocationObserver.store(observer);
}
inline const detail::ImportFailureReceipt* FirstImportFailure()noexcept {
    return detail::firstImportFailureState.load(std::memory_order_acquire)==2?&detail::firstImportFailure:nullptr;
}
inline std::atomic<unsigned> startupInstallScopes{0};
inline std::atomic<HMODULE> deferredSetupModule{nullptr};
inline std::atomic<bool> setupRunning{false},setupScheduled{false};
inline bool BuildSetupDraft(HMODULE module)noexcept {
    using namespace detail;
    // Capture belongs to this private draft; discarded epochs publish no receipt.
    struct ImportObservationWindow {
        ImportObservationWindow()noexcept {observeSetupImports=true;}
        ~ImportObservationWindow(){observeSetupImports=false;}
    } importObservationWindow;
    try {
        wchar_t path[MAX_PATH]{};
        if(!GetModuleFileNameW(module,path,MAX_PATH) || !std::wcsrchr(path,L'\\') ||
           _wcsicmp(std::wcsrchr(path,L'\\')+1,L"d3d11.dll")){detail::Revoke();return false;}
        if(!AddImage(module)){detail::Revoke();return false;}
        // Earlier own edits were recorded without granting any certificate.
        // Only now may they be excluded from a trusted executable comparison.
        // Known proxy export/thunk modules are proof dependencies of ACTUAL
        // registered native edits, not a module whitelist. They receive the
        // same file trust, import recursion and complete executable comparison.
        for(unsigned i=0;i<Ledger().patchCount;++i){const auto& p=Ledger().patches[i];
            if(!AddPatchImages(p)){detail::Revoke();return false;}
        }
        for(unsigned i=0;i<Ledger().patchCount;++i){auto& p=Ledger().patches[i];
            auto& observation=Ledger().setupPatchFailure;observation.index=i;
            PatchCollectionScope origin(p,std::source_location::current());
            origin.Role("setup-patch-index",p.entry);
            if(p.input.module){if(!IndependentInputIntact(p)){detail::Revoke("input-method-boundary");return false;}continue;}
            if(!IndexPatch(p,&observation)){
                detail::Revoke(observation.predicate,observation.owner>=0?
                    Ledger().images[static_cast<unsigned>(observation.owner)].live:p.declaredModule);
                return false;
            }
        }
        if(!WholeCodeLocked() || !SealRepresentations()){detail::Revoke();return false;}
        for(unsigned i=0;i<Ledger().patchCount;++i){auto& p=Ledger().patches[i];
            if((!p.input.module && !BindPatch(p)) || !PatchIntact(p)){detail::Revoke();return false;}
        }
        if(!WholeLocked()){detail::Revoke();return false;}
        auto* image=&Ledger().images[0];const auto d=image->exports;
        if(!image->Range(d.VirtualAddress,d.Size) || d.Size<sizeof(IMAGE_EXPORT_DIRECTORY)){detail::Revoke();return false;}
        const auto ex=detail::Read<IMAGE_EXPORT_DIRECTORY>(image->clean.data()+d.VirtualAddress);
        if(ex.NumberOfNames>65536 || !image->Range(ex.AddressOfNames,size_t(ex.NumberOfNames)*4) ||
           !image->Range(ex.AddressOfNameOrdinals,size_t(ex.NumberOfNames)*2) ||
           !image->Range(ex.AddressOfFunctions,size_t(ex.NumberOfFunctions)*4)){detail::Revoke();return false;}
        for(DWORD i=0;i<ex.NumberOfNames;++i){const auto* name=image->String(detail::Read<DWORD>(image->clean.data()+ex.AddressOfNames+4*i));
            if(!name){detail::Revoke();return false;}const bool deviceExport=!std::strcmp(name,"D3D11CreateDevice");
            const bool swapExport=!std::strcmp(name,"D3D11CreateDeviceAndSwapChain");
            if(!deviceExport && !swapExport)continue;
            const auto ordinal=detail::Read<WORD>(image->clean.data()+ex.AddressOfNameOrdinals+2*i);
            if(ordinal>=ex.NumberOfFunctions){detail::Revoke();return false;}
            const auto rva=detail::Read<DWORD>(image->clean.data()+ex.AddressOfFunctions+4*ordinal);
            if((rva>=d.VirtualAddress && rva<uint64_t(d.VirtualAddress)+d.Size) ||
               !image->SectionHas(rva,32,IMAGE_SCN_MEM_EXECUTE)){detail::Revoke();return false;}
            auto* entry=reinterpret_cast<unsigned char*>(module)+rva;
            if(deviceExport)Ledger().factoryEntry=entry;else Ledger().swapFactoryEntry=entry;
        }
        if(!Ledger().factoryEntry || !Ledger().swapFactoryEntry ||
           reinterpret_cast<void*>(GetProcAddress(module,"D3D11CreateDevice"))!=Ledger().factoryEntry ||
           reinterpret_cast<void*>(GetProcAddress(module,"D3D11CreateDeviceAndSwapChain"))!=Ledger().swapFactoryEntry){detail::Revoke();return false;}
        Ledger().factoryModule=module;
        if(!FreezeClosure() || !PublishRepresentations()){detail::Revoke("setup-final-publication");return false;}
        if(Revoked())return false;
        return true;
    }catch(...){detail::Revoke();return false;}
}
// One call-local observation; not readiness, revocation or a scheduling policy.
struct SetupObservation {
    const char* predicate="not-entered";unsigned attempts=0,generationConflicts=0;
    unsigned state=0,installScopes=0;uint64_t snapshotGeneration=0,returnGeneration=0;
    bool running=false,busy=false,deferred=false;
};
// Fresh unpublished drafts may retry; no revoked or published ledger is reset.
inline bool Setup(HMODULE module,SetupObservation* observation=nullptr)noexcept {
    if constexpr (!F6InputPolicy::FsrBridgeEnabled) return false;
    using namespace detail;LiveLedgerScope live;
    const auto outcome=[&](bool ready,const char* predicate){
        if(observation){observation->predicate=predicate;observation->state=store.initialized.load();
            observation->returnGeneration=store.generation.load();observation->running=setupRunning.load();
            observation->busy=store.busy.test(std::memory_order_acquire);
            observation->deferred=deferredSetupModule.load()!=nullptr;observation->installScopes=startupInstallScopes.load();}
        return ready;
    };
    if(store.initialized.load()==2)return outcome(!Revoked(),Revoked()?"already-revoked":"already-ready");
    if(Revoked() || !module)return outcome(false,Revoked()?"already-revoked":"missing-runtime");
    HMODULE runtime=nullptr,self=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(module),&runtime) || runtime!=module ||
       !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&Setup),&self)){detail::Revoke("setup-module-pin");return outcome(false,"module-pin");}
    bool absent=false;if(!setupRunning.compare_exchange_strong(absent,true))return outcome(false,"setup-already-running");
    struct Finish {~Finish(){NotifyFirstRevocation();setupRunning.store(false);ResumeSetup();}} finish;
    const auto defer=[&](const char* predicate){HMODULE empty=nullptr;deferredSetupModule.compare_exchange_strong(empty,module);return outcome(false,predicate);};
    for(unsigned attempt=0;attempt<8;++attempt){
        if(observation)observation->attempts=attempt+1;
        std::unique_ptr<LedgerData> draft;
        try{draft=std::make_unique<LedgerData>();}catch(...){detail::Revoke("setup-draft-allocation");return outcome(false,"draft-allocation");}
        uint64_t generation=0;
        {Guard guard;if(!guard.held)return defer("snapshot-lock-busy");
            if(Revoked())return outcome(false,"concurrent-revocation");
            if(store.initialized.load()==2)return outcome(true,"ready-before-snapshot");
            generation=store.generation.load();
            if(observation)observation->snapshotGeneration=generation;
            if(generation&1)return defer("snapshot-generation-odd");
            auto& current=Ledger();draft->patchCount=current.patchCount;
            draft->patches=current.patches;draft->transitionCount=current.transitionCount;
            draft->setupTransitions=current.setupTransitions;
            draft->setupPatchFailure.attempt=attempt+1;
            draft->setupPatchFailure.snapshotGeneration=generation;
            draft->setupPatchFailure.patchCount=current.patchCount;
            store.initialized.store(1,std::memory_order_release);
        }
        draftImportFailureSet=false;draftLedger=draft.get();
        const bool good=BuildSetupDraft(module);
        if(good)RebuildNativeValidationPlan(true); // Private sealed draft: no publication/loader lock.
        draftLedger=nullptr;
        // No signature/file/loader work occurs inside this publication lock.
        {Guard guard;if(!guard.held)return defer("publication-lock-busy");
            if(Revoked())return outcome(false,"concurrent-revocation");
            if(store.generation.load()!=generation){
                if(observation)++observation->generationConflicts;
                continue;
            }
            if(!good || draft->refused.load()){
                if(draft->setupPatchFailure.captured){
                    draft->setupPatchFailure.imageCount=draft->imageCount;
                    draft->setupPatchFailure.publicationGeneration=store.generation.load();
                }
                if(draftImportFailureSet){unsigned empty=0;
                    if(firstImportFailureState.compare_exchange_strong(empty,1)){
                        firstImportFailure=draftImportFailure;firstImportFailureState.store(2,std::memory_order_release);}}
                detail::Revoke(draft->refusal?draft->refusal:"setup-draft-refused",draft->refusalModule,draft->refusalCaller,
                    draft->refusalHasImage?&draft->refusalImage:nullptr,draft->refusalHasImage?&draft->refusalTrust:nullptr,
                    draft->refusalThread,&draft->refusalOrigin,&draft->setupPatchFailure);
                return outcome(false,"winning-draft-refused");
            }
            // Pin successful image/file ownership for the process lifetime.
            store.published.store(draft.release(),std::memory_order_release);
            store.initialized.store(2,std::memory_order_release);
        }
        // The winning draft was released and the publication Guard is gone.
        // No callback, installer, wait or new Setup owner runs on this path.
        optionalSetupPublished.store(true,std::memory_order_release);
        return outcome(true,"published-ready");
    }
    return defer("generation-conflict-budget");
}
inline void ScheduleSetup(HMODULE module)noexcept;
inline void ResumeSetup()noexcept {
    if constexpr (!F6InputPolicy::FsrBridgeEnabled) return;
    using namespace detail;
    if(!deferredSetupModule.load() || startupInstallScopes.load() || setupRunning.load() ||
       store.busy.test(std::memory_order_acquire) || store.initialized.load()==2 || Revoked())return;
    bool absent=false;if(!setupScheduled.compare_exchange_strong(absent,true))return;
    if(!TrySubmitThreadpoolCallback([](PTP_CALLBACK_INSTANCE,void*){
        const auto module=deferredSetupModule.exchange(nullptr);if(module)Setup(module);
        setupScheduled.store(false);ResumeSetup();
    },nullptr,nullptr)){setupScheduled.store(false);detail::Revoke("setup-worker-submit");}
}
inline void ScheduleSetup(HMODULE module)noexcept {
    if constexpr (!F6InputPolicy::FsrBridgeEnabled) return;
    detail::LiveLedgerScope live;
    if(!module || detail::Revoked() || detail::store.initialized.load()==2)return;
    HMODULE runtime=nullptr,self=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(module),&runtime) || runtime!=module ||
       !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&ScheduleSetup),&self)){detail::Revoke("setup-module-pin");return;}
    HMODULE empty=nullptr;deferredSetupModule.compare_exchange_strong(empty,module);ResumeSetup();
}
// Covers the existing CheckWorkingMode startup sequence, including its late
// Kernel32 hooks. Deferral only postpones setup: it cannot admit an early or
// previously observed context. The callback remains outside the loader lock.
class StartupInstallScope final {
public:
    StartupInstallScope()noexcept {startupInstallScopes.fetch_add(1);}
    ~StartupInstallScope(){
        if(startupInstallScopes.fetch_sub(1)==1){
            const auto module=deferredSetupModule.exchange(nullptr);
            if(module)ScheduleSetup(module);
        }
    }
    StartupInstallScope(const StartupInstallScope&)=delete;
    StartupInstallScope& operator=(const StartupInstallScope&)=delete;
};
// These wrappers retain Detours' return values and original behavior when
// optional authentication is unavailable. They NEVER make hooking a fallback
// requirement for the existing SR route.
enum class InstallRequirement { Mandatory, Optional };
// A transaction-local snapshot, not native authority. Both optional setters are
// prepared before either detour; Attach compares these exact bytes again.
struct OwnEntryPreflight {PVOID hook=nullptr;std::array<unsigned char,32> bytes{};};
inline LONG PrepareOptionalOwnEntry(PVOID hook,OwnEntryPreflight& prepared)noexcept {
    using namespace detail;LiveLedgerScope live;prepared={};
    if(!pending.held || !pending.optional || Revoked())return ERROR_INVALID_OPERATION;
    OwnEntryPreflight candidate{};
    if(!OwnImageRange(hook,candidate.bytes.size(),IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE,
                      IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_DISCARDABLE) ||
       !Memory(hook,candidate.bytes.data(),candidate.bytes.size()) ||
       !Equal(hook,candidate.bytes.data(),candidate.bytes.size())){
        detail::Revoke("optional-own-hook-entry-integrity");return ERROR_INVALID_DATA;
    }
    const auto status=OwnEntryPrefix(candidate.bytes.data(),candidate.bytes.size(),hook);
    if(status==OwnEntryStatus::IntegrityFailure){
        detail::Revoke("own-hook-entry-integrity");return ERROR_INVALID_DATA;
    }
    if(status==OwnEntryStatus::UnsupportedEncoding)return ERROR_NOT_SUPPORTED;
    candidate.hook=hook;prepared=candidate;return NO_ERROR;
}
inline LONG BeginDetourTransaction()noexcept {
    const auto result=DetourTransactionBegin();
    // The linked Detours acquires ownership before making trampoline pages writable.
    // ERROR_INVALID_OPERATION alone rejects Begin without acquiring that ownership.
    if(result!=NO_ERROR && result!=ERROR_INVALID_OPERATION){
        if(DetourTransactionAbort()!=NO_ERROR)GenshinNativeQuery::Revoke();
    }
    return result;
}
inline LONG Begin(InstallRequirement requirement=InstallRequirement::Mandatory)noexcept {
    if constexpr (!F6InputPolicy::FsrBridgeEnabled) return BeginDetourTransaction();
    using namespace detail;LiveLedgerScope live;
    if(pending.held)return ERROR_INVALID_OPERATION;
    if(Revoked())return BeginDetourTransaction();
    if(store.busy.test_and_set(std::memory_order_acquire))return ERROR_RETRY;
    if(requirement==InstallRequirement::Optional &&
       Ledger().transitionCount==Ledger().setupTransitions.size()){
        store.busy.clear(std::memory_order_release);ResumeSetup();return ERROR_NOT_ENOUGH_MEMORY;
    }
    const auto result=BeginDetourTransaction();
    if(result!=NO_ERROR){store.busy.clear(std::memory_order_release);ResumeSetup();
        if(requirement==InstallRequirement::Mandatory)detail::Revoke("detour-begin-refused");return result;}
    pending.held=true;pending.count=0;pending.from=0;
    pending.optional=requirement==InstallRequirement::Optional;
    try {
        const auto generation=store.generation.load();
        bool good=generation<=UINT64_MAX-2 && !(generation&1);
        if(store.initialized.load()==2){
            // A native factory can resolve its canonical signed delay imports
            // before calling a known own DXGI installer on this same thread.
            // This is the existing native delay proof, not a reset/adoption of
            // arbitrary bindings; executable bytes are still checked in full.
            if(nativeSetupWindow)good=good && FinishNativeDelaySetup();
            good=good && PrepareNativeBirthLocked() && WholeLocked();
        }
        else for(unsigned i=0;i<Ledger().patchCount;++i)good=good && PatchIntact(Ledger().patches[i]);
        if(!good)detail::Revoke();else {pending.from=generation;store.generation=generation+1;}
    }catch(...){detail::Revoke();}
    return result;
}
inline LONG Attach(PVOID* original,PVOID hook,PVOID* executed=nullptr,PVOID* mirror=nullptr,
                   HMODULE declaredModule=nullptr,const char* exportName=nullptr,bool ownSetup=false,
                   PVOID* observedTarget=nullptr,PDETOUR_TRAMPOLINE* observedTrampoline=nullptr,
                   PVOID* observedDetour=nullptr,
                   const std::source_location& caller=std::source_location::current(),
                   const detail::IndependentInputMethod* input=nullptr,
                   const OwnEntryPreflight* preparedOwnEntry=nullptr,
                   const detail::FactoryPredecessor* predecessor=nullptr)noexcept {
    if constexpr (!F6InputPolicy::FsrBridgeEnabled)
        return DetourAttachEx(original,hook,observedTrampoline,observedTarget,observedDetour);
    using namespace detail;LiveLedgerScope live;
    if(preparedOwnEntry && (!pending.held || !pending.optional || Revoked()))return ERROR_INVALID_OPERATION;
    if(!pending.held || Revoked()){
        return DetourAttachEx(original,hook,observedTrampoline,observedTarget,observedDetour);
    }
    // A full receipt array is an optional-install capacity miss, not damaged
    // native provenance. Refuse BEFORE staging a detour; the caller aborts only
    // this transaction. Mandatory enrollment retains its existing policy.
    if(pending.optional && (pending.count==pending.patches.size() ||
       Ledger().patchCount+pending.count==Ledger().patches.size()))return ERROR_NOT_ENOUGH_MEMORY;
    OwnEntryPreflight localOwnEntry{};
    if(pending.optional){
        if(!preparedOwnEntry){
            const auto preflight=PrepareOptionalOwnEntry(hook,localOwnEntry);
            if(preflight!=NO_ERROR)return preflight;
            preparedOwnEntry=&localOwnEntry;
        }
        if(preparedOwnEntry->hook!=hook ||
           !OwnImageRange(hook,preparedOwnEntry->bytes.size(),IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE,
                          IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_DISCARDABLE) ||
           !Equal(hook,preparedOwnEntry->bytes.data(),preparedOwnEntry->bytes.size())){
            detail::Revoke("optional-own-hook-preimage-mismatch");return ERROR_INVALID_DATA;
        }
    }
    void* before=original?*original:nullptr;PDETOUR_TRAMPOLINE trampoline=nullptr;void* target=nullptr;void* detour=nullptr;
    if(predecessor && predecessor->captured &&
       (!ownSetup || !declaredModule || declaredModule!=predecessor->module || !exportName ||
        !predecessor->symbol || std::strcmp(exportName,predecessor->symbol) ||
        predecessor->nodes[0].address!=reinterpret_cast<uintptr_t>(before) ||
        predecessor->thread!=GetCurrentThreadId() || predecessor->generation==UINT64_MAX ||
        predecessor->generation+1!=store.generation.load() || !pending.factoryLease ||
        !FactoryPredecessorIntact(*predecessor,0,pending.factoryLease))){
        detail::Revoke("factory-predecessor-before-attach");return ERROR_INVALID_DATA;
    }
    const auto result=DetourAttachEx(original,hook,&trampoline,&target,&detour);
    if(observedTarget)*observedTarget=target;
    if(observedTrampoline)*observedTrampoline=trampoline;
    if(observedDetour)*observedDetour=detour;
    if(result!=NO_ERROR){if(!pending.optional)detail::Revoke();return result;}
    if(input && input->module && target!=before){
        detail::Revoke("input-method-normalization-changed");return ERROR_INVALID_OPERATION;
    }
    const auto owner=Owner(target,30);
    if((store.initialized.load()==2 && owner<0 && !ownSetup) || (!ownSetup && target!=before) || detour!=hook || !trampoline || pending.count==pending.patches.size() ||
       Ledger().patchCount+pending.count==Ledger().patches.size()){detail::Revoke();return result;}
    Patch p{};p.original=original;p.entry=static_cast<unsigned char*>(target);p.enrollment=caller;
    if(predecessor && predecessor->captured)p.predecessor=*predecessor;
    if(input)p.input=*input;
    p.executed=executed;p.mirror=mirror;p.declaredModule=declaredModule;p.exportName=exportName;p.ownSetup=ownSetup;
    if(!CaptureNativeHops(p,before,target)){detail::Revoke();return result;}
    p.trampoline=reinterpret_cast<unsigned char*>(trampoline);p.hook=hook;
    if(owner>=0){p.image=static_cast<unsigned>(owner);
        p.rva=static_cast<uint32_t>(p.entry-reinterpret_cast<unsigned char*>(Ledger().images[p.image].live));}
    if(!Memory(trampoline,p.code.data(),p.code.size()) || !Memory(hook,p.ownEntry.data(),p.ownEntry.size())){
        detail::Revoke();return ERROR_INVALID_DATA;
    }
    if(preparedOwnEntry && p.ownEntry!=preparedOwnEntry->bytes){
        detail::Revoke("optional-own-hook-preimage-mismatch");return ERROR_INVALID_DATA;
    }
    const auto ownEntryStatus=OwnEntryPrefix(p.ownEntry.data(),p.ownEntry.size(),hook);
    if(ownEntryStatus!=OwnEntryStatus::Supported){
        // Optional snapshots were already Supported before DetourAttachEx.
        // Any post-staging rejection, and every mandatory rejection, stays hard.
        detail::Revoke("own-hook-entry-instruction");
        return ownEntryStatus==OwnEntryStatus::UnsupportedEncoding?ERROR_NOT_SUPPORTED:ERROR_INVALID_DATA;
    }
    p.size=p.code[62];const auto copied=p.code[30];
    if(p.size<5 || p.size>24 || copied>24 || !Equal(target,p.code.data()+32,p.size) ||
       (owner>=0 && (!Ledger().images[p.image].SectionHas(p.rva,p.size,IMAGE_SCN_MEM_EXECUTE) ||
        (!p.predecessor.captured && std::memcmp(p.code.data()+32,Ledger().images[p.image].clean.data()+p.rva,p.size)))) ||
       Read<uintptr_t>(p.code.data()+72)!=reinterpret_cast<uintptr_t>(p.entry+p.size) ||
       Read<uintptr_t>(p.code.data()+80)!=reinterpret_cast<uintptr_t>(hook) ||
       p.code[copied]!=0xff || p.code[copied+1]!=0x25 || Read<int32_t>(p.code.data()+copied+2)!=72-int(copied)-6){detail::Revoke();return result;}
    if(!Memory(target,p.installPreimage.data(),p.size) ||
       std::memcmp(p.installPreimage.data(),p.code.data()+32,p.size)){
        detail::Revoke("attach-install-preimage-mismatch");return result;
    }
    p.installPreimageCaptured=true;p.installGeneration=store.generation.load();
    if(p.predecessor.captured && (target!=before || p.size!=5 ||
       std::memcmp(p.installPreimage.data(),p.predecessor.nodes[0].bytes.data(),p.size) ||
       !FactoryPredecessorIntact(p.predecessor,0,pending.factoryLease))){detail::Revoke("factory-predecessor-install-restore");return ERROR_INVALID_DATA;}
    if(p.predecessor.captured){
        const auto trampolineAt=reinterpret_cast<uintptr_t>(p.trampoline);
        const auto distance=int64_t(Read<int32_t>(p.code.data()+1));
        if(trampolineAt>UINTPTR_MAX-5){detail::Revoke("factory-predecessor-trampoline-range");return ERROR_INVALID_DATA;}
        const auto next=trampolineAt+5;
        if((distance<0 && uint64_t(-distance)>next) ||
           (distance>=0 && uint64_t(distance)>UINTPTR_MAX-next)){
            detail::Revoke("factory-predecessor-trampoline-range");return ERROR_INVALID_DATA;}
        const auto branch=distance<0?next-static_cast<uintptr_t>(-distance):next+static_cast<uintptr_t>(distance);
        if(copied!=5 || p.code[0]!=0xe9 || branch!=p.predecessor.nodes[1].address){
            detail::Revoke("factory-predecessor-trampoline-target");return ERROR_INVALID_DATA;}
        // A pre-existing native closure also needs its signed start proved now;
        // the initial closure repeats the same proof in IndexPatch/BindPatch.
        if(owner>=0 && !FactoryCleanStart(p,Ledger().images[p.image])){
            detail::Revoke("factory-predecessor-clean-start");return ERROR_INVALID_DATA;}
    }
    for(unsigned i=0;i<Ledger().patchCount+pending.count;++i){const auto& old=i<Ledger().patchCount?Ledger().patches[i]:pending.patches[i-Ledger().patchCount];
        const auto first=reinterpret_cast<uintptr_t>(p.entry),second=reinterpret_cast<uintptr_t>(old.entry);
        if(first<second+old.size && second<first+p.size){detail::Revoke();return result;}
    }
    if(p.input.module && !IndependentInputIntact(p)){
        detail::Revoke("input-method-normalization-or-boundary");return ERROR_INVALID_OPERATION;
    }
    try {
        if(store.initialized.load()==2 && !p.input.module){
            if(!AddPatchImages(p)){detail::Revoke("attach-patch-image",declaredModule);return result;}
            // R798: an own late installer may introduce its signed target image.
            // Derive its index/RVA only AFTER that image is authenticated, using
            // the same original-byte proof as initial setup. BindPatch still
            // verifies the declared export, complete normalization and IAT edges.
            // Composed factories retain strict Index/Bind mapping. Complete
            // those proofs after our lease restores RX, before publication.
            if(!p.predecessor.captured){
                if(!IndexPatch(p)){detail::Revoke("attach-patch-index",declaredModule);return result;}
                if(!BindPatch(p)){detail::Revoke("attach-patch-bind",declaredModule);return result;}
            }
        }
    }catch(...){detail::Revoke();return result;}
    p.straight=copied==p.size && Straight(p.code.data()+32,p.size) && !std::memcmp(p.code.data(),p.code.data()+32,p.size);
    pending.patches[pending.count++]=p;return result;
}
// Only explicit input COM-method callsites can request this noncallable role.
// Exports, system methods and unsupported tables retain ordinary enrollment.
inline LONG AttachInputMethod(PVOID* original,PVOID hook,IUnknown* object,unsigned index,
    const std::source_location& caller=std::source_location::current())noexcept {
    detail::IndependentInputMethod input{};
    if(detail::pending.held && !detail::Revoked())
        detail::SelectIndependentInputMethod(object,index,original?*original:nullptr,input);
    return Attach(original,hook,original,nullptr,nullptr,nullptr,true,nullptr,nullptr,nullptr,caller,
        input.module?&input:nullptr);
}
template<class Function,typename std::enable_if<std::is_function<Function>::value,int>::type=0>
inline LONG Attach(PVOID* original,Function* hook,
                   const std::source_location& caller=std::source_location::current())noexcept {
    return Attach(original,reinterpret_cast<void*>(hook),nullptr,nullptr,nullptr,nullptr,false,
                  nullptr,nullptr,nullptr,caller);
}
inline void EndTransaction(bool committed)noexcept {
    if constexpr (!F6InputPolicy::FsrBridgeEnabled) return;
    using namespace detail;LiveLedgerScope live;if(!pending.held)return;
    // An optional transaction is separate from the already committed Bridge.
    // Detours reports an all-or-nothing rollback on failure/abort. Do not publish
    // its staged patches, or revoke sound existing patches for an install error.
    // Authentication failures in Begin/Attach/Finalize still revoke normally.
    if(!committed && !pending.optional)detail::Revoke();
    bool setupOnly=pending.count!=0;
    if(committed && !Revoked()){
        for(unsigned i=0;i<pending.count;++i){auto p=pending.patches[i];
            setupOnly=setupOnly && p.ownSetup && !p.input.module;
            if(p.predecessor.captured && store.initialized.load()==2 &&
               (!IndexPatch(p) || !BindPatch(p))){detail::Revoke("factory-postcommit-index-bind");break;}
            // Derive exactly Detours' relay/edit from the saved preimage.
            if(!FinalizeOwnPatch(p)){detail::Revoke();break;}
            Ledger().patches[Ledger().patchCount++]=p;
        }
        try {if(!Revoked() && store.initialized.load()==2 && !WholeLocked())detail::Revoke();}catch(...){detail::Revoke();}
    }
    const auto generation=store.generation.load();if(generation&1)store.generation=generation+1;
    // A native factory may itself create a DXGI factory, installing known own
    // methods before returning. Accept ONLY a contiguous same-thread sequence
    // of these committed, byte-verified setup transactions, never an epoch reset.
    // A successfully rolled-back optional install is an authenticated no-op,
    // not a gap in the enclosing native factory's observed epoch chain. Keep
    // generation monotonic; never roll it back or clear an actual revocation.
    if(!committed && pending.optional && !Revoked()){
        try {if(store.initialized.load()==2 && !WholeLocked())detail::Revoke();}
        catch(...){detail::Revoke();}
    }
    if(!Revoked() && ((committed && setupOnly) || (!committed && pending.optional && pending.from))){
        if(Ledger().transitionCount==Ledger().setupTransitions.size())detail::Revoke();
        else Ledger().setupTransitions[Ledger().transitionCount++]={pending.from,store.generation.load(),GetCurrentThreadId()};
    }
    ++Ledger().validationRevision;Ledger().validationPlan.reset();
    if(!Revoked() && store.initialized.load()==2)RebuildNativeValidationPlan();
    pending.count=0;pending.held=false;pending.optional=false;
    store.busy.clear(std::memory_order_release);ResumeSetup();
}
inline LONG Commit()noexcept {const auto result=DetourTransactionCommit();EndTransaction(result==NO_ERROR);return result;}
inline LONG Abort()noexcept {const auto result=DetourTransactionAbort();
    // A failed rollback has unknown live state, including for optional installs.
    if(result!=NO_ERROR)detail::Revoke("detour-abort-refused");EndTransaction(false);return result;}
inline bool Intact()noexcept {
    if(detail::contextExecutionRefused.load(std::memory_order_acquire))return false;
    detail::Guard guard;if(!guard.held)return false;
    if(detail::store.initialized.load()!=2)return false;
    const bool good=detail::PrepareNativeBirthLocked();if(!good)detail::Revoke();return good;
}
// R701 explicit entry gate for standalone/native callers; not a witness issuer.
inline bool PrepareNativeBirth()noexcept {
    if(detail::contextExecutionRefused.load(std::memory_order_acquire))return false;
    const DWORD lastError=GetLastError();bool good=false;
    {detail::Guard guard;if(guard.held){
        try {good=detail::PrepareNativeBirthLocked() && detail::WholeLocked();}
        catch(...){detail::Revoke("birth-entry-public-exception");}
    }}
    SetLastError(lastError);return good;
}
inline bool NativeObject(const void* object,unsigned slots)noexcept {
    using namespace detail;Guard guard;if(!guard.held || !StableLocked() || contextExecutionRefused.load(std::memory_order_acquire))return false;
    const auto fail=[](){detail::Revoke("native-interface-table");return false;};
    void* table=nullptr;if(!Memory(object,&table,sizeof(table)) || !slots || slots>150)return fail();
    const auto owner=Owner(table,size_t(slots)*sizeof(void*));if(owner<0)return fail();
    const auto& image=Ledger().images[owner];const auto rva=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(table)-reinterpret_cast<uintptr_t>(image.live));
    if(!image.SectionHas(rva,size_t(slots)*8,IMAGE_SCN_MEM_READ,IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_EXECUTE) ||
       !Equal(table,image.clean.data()+rva,size_t(slots)*8))return fail();
    for(unsigned i=0;i<slots;++i)if(!Executable(reinterpret_cast<void*>(Read<uintptr_t>(image.clean.data()+rva+8*i))))return fail();
    return StableLocked();
}
#include "GenshinNativeContextRecipe.h"
#include "GenshinNativeContextStateSwapRecipe.h"
// Only typed D3D11 context consumers use the native inline constructor recipe.
// NativeObject remains strict for devices, adapters, factories and all others.
// A table match never mints a creation witness or promotes UnknownPast history.
inline bool NativeContext(const ID3D11DeviceContext* object,unsigned slots)noexcept {
    using namespace detail;Guard guard;if(!guard.held || !StableLocked())return false;
    if(contextExecutionRefused.load(std::memory_order_acquire))return false;
    NativeContextFailure failure;failure.object=reinterpret_cast<uintptr_t>(object);failure.slots=slots;
    failure.generation=store.generation.load();failure.thread=GetCurrentThreadId();failure.factory=reinterpret_cast<uintptr_t>(Ledger().factoryModule);
    const auto fail=[&](){
        if(failure.executionOnly)contextExecutionRefused.store(true,std::memory_order_release);
        PinContextFailure(failure);
        // Only a complete signed table/code match plus a refused execution
        // FIELD takes this branch. GPU admission stays permanently false; CPU
        // code integrity still has Stable/Whole/PatchIntact and its own proof.
        if(!failure.executionOnly)detail::Revoke("native-context-table",nullptr,std::source_location::current(),
            nullptr,nullptr,0,nullptr,nullptr,&failure);
        return false;
    };
    auto& o=failure.tableLeaf;o.At("NativeContext");
    void* table=nullptr;
    if(!Memory(object,&table,sizeof(table),o))return fail();
    failure.table=reinterpret_cast<uintptr_t>(table);
    if(!o.Leaf("context-requested-slots",slots && slots<=150))return fail();
    const auto owner=Owner(table,size_t(slots)*sizeof(void*));failure.owner=owner;
    if(owner>=0){
        const auto& image=Ledger().images[owner];const auto rva=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(table)-reinterpret_cast<uintptr_t>(image.live));
        o.rva=rva;o.image=owner;
        if(!o.Leaf("context-image-table-section",image.SectionHas(rva,size_t(slots)*8,IMAGE_SCN_MEM_READ,IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_EXECUTE)) ||
           !Equal(table,image.clean.data()+rva,size_t(slots)*8,o))return fail();
        for(unsigned i=0;i<slots;++i){o.slot=static_cast<int>(i);
            if(!o.Leaf("context-image-entry-Executable",Executable(reinterpret_cast<void*>(Read<uintptr_t>(image.clean.data()+rva+8*i)))))return fail();}
    }else {
        context_recipe::TableSnapshot snapshot;
        if(!context_recipe::InlineTableLocked(object,table,slots,failure.inlineLeaf,&snapshot) &&
           !context_recipe::StateSwapObservedTableLocked(object,table,slots,failure.swapLeaf,true,&failure.bindingShape,&snapshot,&failure.bindingLeaf,&failure.executionOnly)){
            // Only a successfully READ, known field value mismatch is separate
            // from image integrity. Unknown reads/ranges/pointers still revoke.
            failure.executionOnly=failure.bindingShape && failure.executionOnly;return fail();
        }
    }
    return StableLocked();
}
inline bool StateSwapHookTargets(const ID3D11DeviceContext* object,std::array<void*,5>& targets,
    std::array<uintptr_t,2>& rangeReturns,detail::ContextTableLeaf* observation=nullptr)noexcept {
    targets.fill(nullptr);rangeReturns.fill(0);
    using namespace detail;ContextTableLeaf local;auto& o=observation?*observation:local;
    o.At("StateSwapHookTargets");
    Guard guard;if(!o.Leaf("swap-target-guard",guard.held) || !o.Leaf("swap-target-stable",StableLocked()))return false;
    void* table=nullptr;
    if(!Memory(object,&table,sizeof(table),o) || !o.Leaf("swap-target-image-count",Ledger().imageCount!=0) ||
       !context_recipe::BindingTableLocked(object,table,132,o))return false;
    const auto& image=Ledger().images[0];
    const auto* profile=context_recipe::SelectStateSwapProfile(image);
    o.At("StateSwapHookTargets");
    if(!o.Leaf("swap-target-factory-image",image.live==Ledger().factoryModule) ||
       !o.Leaf("swap-target-profile-supported",profile!=nullptr) ||
       !context_recipe::StateSwapObservedRecipeIntact(image,*profile,o) ||
       !context_recipe::StateSwapObservedForwardCodeIntact(image,*profile,o))return false;
    constexpr unsigned slots[]{27,28,58,110,114};const auto base=reinterpret_cast<uintptr_t>(image.live);
    std::array<void*,5> exact{};std::array<uintptr_t,2> returns{};
    for(unsigned i=0;i<exact.size();++i){
        const auto rva=(*profile->expected)[slots[i]];o.At("StateSwapHookTargets",rva);o.slot=slots[i];
        if(!o.Leaf("swap-target-address-range",base<=UINTPTR_MAX-rva) ||
           !o.Leaf("swap-target-executable",Executable(reinterpret_cast<const void*>(base+rva))))return false;
        exact[i]=reinterpret_cast<void*>(base+rva);
    }
    // Exact return PCs after each pinned wrapper's native query call. They are
    // profile data, not an arithmetic shift from the legacy Begin address.
    for(unsigned i=0;i<returns.size();++i){
        const auto rva=profile->rangeReturns[i];o.At("StateSwapHookTargets",rva);
        if(!o.Leaf("swap-return-address-range",base<=UINTPTR_MAX-rva) ||
           !o.Leaf("swap-return-executable",Executable(reinterpret_cast<const void*>(base+rva))))return false;
        returns[i]=base+rva;
    }
    o.At("StateSwapHookTargets");
    if(!o.Leaf("swap-target-final-stable",StableLocked()))return false;
    targets=exact;rangeReturns=returns;return true;
}
inline bool Required(void* entry)noexcept {
    using namespace detail;Guard guard;if(!guard.held || !StableLocked() || contextExecutionRefused.load(std::memory_order_acquire))return false;
    const auto* patch=Find(entry);
    if(!patch || patch->input.module || !patch->straight || !PatchIntact(*patch)){detail::Revoke("required-original-path");return false;}
    return StableLocked();
}
namespace detail {
// R701: original FF25 may address a signed immutable local function-pointer
// constant rather than an IAT receipt. A known import NEVER takes this fallback.
inline bool RepresentationSlot(const void* slot)noexcept {
    const auto address=reinterpret_cast<uintptr_t>(slot);
    const auto kernel=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"kernel32.dll"));
    const auto apphelp=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"apphelp.dll"));
    const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"kernelbase.dll"));
    if((kernel && kernel<=UINTPTR_MAX-0x85bb0 && address==kernel+0x85bb0) ||
       (apphelp && apphelp<=UINTPTR_MAX-0x55470 && (address==apphelp+0x55410 || address==apphelp+0x55470)) ||
       (base && base<=UINTPTR_MAX-0x286778 && (address==base+0x285f88 || address==base+0x286778)))return true;
    const auto& s=Ledger().shim;
    for(unsigned i=0;i<s.edgeCount && i<s.edges.size();++i)if(s.edges[i].slot==slot)return true;
    for(unsigned i=0;i<s.fileCount && i<s.fileEdges.size();++i)if(s.fileEdges[i].slot==slot)return true;
    const auto& l=Ledger();
    for(unsigned i=0;i<l.runtimeFileCount && i<l.runtimeFileEdges.size();++i)if(l.runtimeFileEdges[i].slot==slot)return true;
    for(unsigned i=0;i<l.delayedCount && i<l.delayedEdges.size();++i)if(l.delayedEdges[i].edge.slot==slot)return true;
    return false;
}
inline bool ReadOnlyNativeForward(const void* slot,void*& target)noexcept {
    const auto owner=Owner(slot,sizeof(uintptr_t));if(owner<0)return false;
    const auto& image=Ledger().images[owner];
    const auto rva=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(slot)-reinterpret_cast<uintptr_t>(image.live));
    if(!image.SectionHas(rva,sizeof(uintptr_t),IMAGE_SCN_MEM_READ,
        IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_DISCARDABLE))return false;
    const auto expected=Read<uintptr_t>(image.clean.data()+rva);
    if(!expected || !Executable(reinterpret_cast<void*>(expected)) ||
       !Equal(slot,image.clean.data()+rva,sizeof(uintptr_t)))return false;
    target=reinterpret_cast<void*>(expected);return true;
}
}
// R692: methods may execute only clean native entries or recorded straight Own originals.
inline void* NativeMethod(const void* object,unsigned slots,unsigned index)noexcept {
    if(index>=slots || !NativeObject(object,slots))return nullptr;
    using namespace detail;Guard guard;if(!guard.held || !PrepareNativeBirthLocked())return nullptr;
    const auto accept=[&](void* method)noexcept->void* {
        if(!method || !QuickLocked())return nullptr;
        return StableLocked()?method:nullptr;
    };
    void* table=nullptr;void* entry=nullptr;
    if(!Memory(object,&table,sizeof(table)) ||
       !Memory(static_cast<unsigned char*>(table)+index*sizeof(void*),&entry,sizeof(entry)))return nullptr;
    std::array<void*,5> visited{};
    for(unsigned depth=0;depth<visited.size();++depth){
        for(unsigned i=0;i<depth;++i)if(visited[i]==entry)return nullptr;
        visited[depth]=entry;
        // Resolve registered aliases at EVERY clean hop; never enter an Own hook.
        Patch* selected=nullptr;
        for(unsigned i=0;i<Ledger().patchCount;++i){auto& patch=Ledger().patches[i];
            bool matches=patch.entry==entry || patch.requested==entry;
            for(unsigned j=0;j<patch.hopCount;++j)matches=matches || patch.hops[j].entry==entry;
            if(!matches)continue;
            if(selected && selected!=&patch)return nullptr;
            selected=&patch;
        }
        if(selected)return accept(selected->ownSetup && selected->straight && PatchIntact(*selected) &&
            BindPatch(*selected)?selected->trampoline:nullptr);
        const auto owner=Owner(entry,2);if(owner<0)return nullptr;
        const auto& image=Ledger().images[owner];
        const auto rva=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(entry)-reinterpret_cast<uintptr_t>(image.live));
        if(!image.SectionHas(rva,2,IMAGE_SCN_MEM_EXECUTE) || !Equal(entry,image.clean.data()+rva,2))return nullptr;
        const auto* code=image.clean.data()+rva;
        const unsigned size=code[0]==0xe9?5:code[0]==0xeb?2:(code[0]==0xff && code[1]==0x25)?6:0;
        if(!size)return accept(Executable(entry)?entry:nullptr);
        // A clean native forwarding entry is not itself an unregistered hook.
        // Authenticate its complete original instruction and, for FF25, the
        // existing canonical CodeBinding IAT receipt before following it.
        if(depth+1==visited.size() || !image.SectionHas(rva,size,IMAGE_SCN_MEM_EXECUTE) ||
           !Equal(entry,code,size))return nullptr;
        auto* address=static_cast<unsigned char*>(entry);void* next=nullptr;
        if(code[0]==0xe9)next=address+5+Read<int32_t>(code+1);
        else if(code[0]==0xeb)next=address+2+static_cast<int8_t>(code[1]);
        else {
            auto* slot=address+6+Read<int32_t>(code+2);bool known=false;
            if(RepresentationSlot(slot))return nullptr;
            for(unsigned i=0;i<Ledger().importCount;++i){const auto& item=Ledger().imports[i];
                if(item.slot!=slot)continue;
                if(item.lazy || item.binding.kind!=ImportKind::CodeBinding || !item.target)return nullptr;
                void* live=nullptr;if(!Memory(slot,&live,sizeof(live)) || live!=reinterpret_cast<void*>(item.target))return nullptr;
                if(known && next!=live)return nullptr;known=true;next=live;
            }
            if(!known && !ReadOnlyNativeForward(slot,next))return nullptr;
        }
        if(!next || !Executable(next))return nullptr;entry=next;
    }
    return nullptr;
}
namespace birth_detail {
using ReleaseFn=ULONG(STDMETHODCALLTYPE*)(IUnknown*);
using AddRefFn=ULONG(STDMETHODCALLTYPE*)(IUnknown*);
using QueryFn=HRESULT(STDMETHODCALLTYPE*)(IUnknown*,REFIID,void**);
using ParentFn=HRESULT(STDMETHODCALLTYPE*)(IDXGIObject*,REFIID,void**);
using EnumFn=HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*,UINT,IDXGIAdapter**);
using AdapterFn=HRESULT(STDMETHODCALLTYPE*)(IDXGIDevice*,IDXGIAdapter**);
// Stack-only bounded strong refs. Never Release a rejected/foreign result.
// A malformed result from a supposedly native method refuses rather than calling foreign code.
class Ref final {
    IUnknown* object_=nullptr;ReleaseFn release_=nullptr;
public:
    ~Ref(){if(object_ && release_){
        // Do not execute a stale original after an intervening ledger change.
        const auto current=reinterpret_cast<ReleaseFn>(NativeMethod(object_,3,2));
        if(current==release_)current(object_);else detail::Revoke("birth-release-receipt-changed");
    }}
    Ref()=default;Ref(const Ref&)=delete;Ref& operator=(const Ref&)=delete;
    IUnknown* Get()const noexcept{return object_;}
    bool Adopt(IUnknown* object,unsigned slots)noexcept {
        if(object_ || !object)return false;
        const auto release=reinterpret_cast<ReleaseFn>(NativeMethod(object,slots,2));
        if(!release)return false;object_=object;release_=release;return true;
    }
    bool Hold(IUnknown* object,unsigned slots)noexcept {
        const auto add=reinterpret_cast<AddRefFn>(NativeMethod(object,slots,1));
        if(!add || !Adopt(object,slots))return false;add(object_);return true;
    }
};
inline bool Query(IUnknown* source,unsigned slots,REFIID iid,Ref& out,unsigned outSlots)noexcept {
    const auto query=reinterpret_cast<QueryFn>(NativeMethod(source,slots,0));
    IUnknown* raw=nullptr;
    return query && query(source,iid,reinterpret_cast<void**>(&raw))==S_OK && out.Adopt(raw,outSlots);
}
inline bool Identity(IUnknown* source,unsigned slots,Ref& out)noexcept {
    return Query(source,slots,IID_IUnknown,out,3);
}
inline bool SameIdentity(IUnknown* source,unsigned slots,IUnknown* expected)noexcept {
    Ref identity;return Identity(source,slots,identity) && identity.Get()==expected;
}
inline bool Parent(IDXGIAdapter* adapter,Ref& out)noexcept {
    const auto parent=reinterpret_cast<ParentFn>(NativeMethod(adapter,10,6));
    IDXGIFactory* raw=nullptr;
    return parent && parent(adapter,__uuidof(IDXGIFactory),reinterpret_cast<void**>(&raw))==S_OK &&
        out.Adopt(raw,12);
}
// R702: COM identity belongs to an instance. Re-enumeration need not return
// the caller's instance. The authenticated native create invocation binds its
// exact selected-adapter argument to its device output; this helper checks the
// native parent chain and pins each participating COM identity independently.
class AdapterProof final {
    Ref adapter_,factory_,adapterId_,factoryId_;
    void* adapterTable_=nullptr;void* factoryTable_=nullptr;
    bool attempted_=false,ready_=false,afterAttempted_=false;
    bool SelectedIntact()noexcept {
        void* at=nullptr;void* ft=nullptr;Ref parent;
        return detail::Memory(adapter_.Get(),&at,sizeof(at)) && at==adapterTable_ &&
            detail::Memory(factory_.Get(),&ft,sizeof(ft)) && ft==factoryTable_ &&
            SameIdentity(adapter_.Get(),10,adapterId_.Get()) &&
            SameIdentity(factory_.Get(),12,factoryId_.Get()) &&
            Parent(reinterpret_cast<IDXGIAdapter*>(adapter_.Get()),parent) &&
            SameIdentity(parent.Get(),12,factoryId_.Get());
    }
public:
    bool Before(IDXGIAdapter* adapter)noexcept {
        if(attempted_)return false;attempted_=true;
        ready_=adapter_.Hold(adapter,10) && Identity(adapter,10,adapterId_) && Parent(adapter,factory_) &&
            Identity(factory_.Get(),12,factoryId_) &&
            detail::Memory(adapter,&adapterTable_,sizeof(adapterTable_)) &&
            detail::Memory(factory_.Get(),&factoryTable_,sizeof(factoryTable_)) && SelectedIntact();
        return ready_;
    }
    // This is an output relation check, not a certificate for an arbitrary
    // device. NativeCall alone authenticates the actual invocation, parameters,
    // output address, generation, nested receipt and once-only witness.
    bool After(IDXGIAdapter* executedAdapter,ID3D11Device* device)noexcept {
        if(!ready_ || afterAttempted_)return false;afterAttempted_=true;ready_=false;
        if(!device || executedAdapter!=reinterpret_cast<IDXGIAdapter*>(adapter_.Get()) || !SelectedIntact())return false;
        Ref devicePin,deviceId,dxgi,owner,ownerId,parent;
        if(!devicePin.Hold(device,43) || !Identity(device,43,deviceId) ||
           !Query(device,43,__uuidof(IDXGIDevice),dxgi,12) ||
           !SameIdentity(dxgi.Get(),12,deviceId.Get()))return false;
        const auto getAdapter=reinterpret_cast<AdapterFn>(NativeMethod(dxgi.Get(),12,7));
        IDXGIAdapter* raw=nullptr;
        if(!getAdapter || getAdapter(reinterpret_cast<IDXGIDevice*>(dxgi.Get()),&raw)!=S_OK ||
           !owner.Adopt(raw,10) || !Identity(owner.Get(),10,ownerId) ||
           !Parent(reinterpret_cast<IDXGIAdapter*>(owner.Get()),parent) ||
           !SameIdentity(parent.Get(),12,factoryId_.Get()))return false;
        // Different adapter instances may describe the same native selection.
        // No LUID, ordinal, description or cross-instance IUnknown substitution.
        return SameIdentity(owner.Get(),10,ownerId.Get()) &&
            SameIdentity(device,43,deviceId.Get()) && SelectedIntact();
    }
    uintptr_t AdapterIdentity()const noexcept{return reinterpret_cast<uintptr_t>(adapterId_.Get());}
    uintptr_t FactoryIdentity()const noexcept{return reinterpret_cast<uintptr_t>(factoryId_.Get());}
};
// A nested call must actually originate at a clean, direct E8 call in the
// pinned d3d11 image. An arbitrary signed module or mutable indirect operand
// cannot certify a caller. Other instruction shapes deliberately refuse.
inline bool NestedCallSiteLocked(const void* returned,const detail::Patch& child)noexcept {
    using namespace detail;const auto end=reinterpret_cast<uintptr_t>(returned);if(end<5)return false;
    const auto* call=reinterpret_cast<const unsigned char*>(end-5);const auto owner=Owner(call,5);
    if(owner<0 || Ledger().images[owner].live!=Ledger().factoryModule)return false;
    const auto& image=Ledger().images[owner];const auto rva=static_cast<uint32_t>(end-5-reinterpret_cast<uintptr_t>(image.live));
    const auto* code=image.clean.data()+rva;
    if(!image.SectionHas(rva,5,IMAGE_SCN_MEM_EXECUTE) || code[0]!=0xe8 || !Equal(call,code,5))return false;
    const auto target=end+static_cast<intptr_t>(Read<int32_t>(code+1));
    return target==reinterpret_cast<uintptr_t>(child.requested) || target==reinterpret_cast<uintptr_t>(child.entry);
}
} // namespace birth_detail

inline void NoteFactoryEntry()noexcept {
    if(detail::factoryDepth && detail::reentryEpoch!=UINT64_MAX)++detail::reentryEpoch;
}
inline bool OwnSetupAdvanceLocked(uint64_t from,uint64_t to,DWORD thread)noexcept {
    if(from==to)return true;
    for(unsigned i=0;i<detail::Ledger().transitionCount;++i){const auto& step=detail::Ledger().setupTransitions[i];
        if(step.from!=from)continue;if(step.thread!=thread || step.to<=from)return false;
        from=step.to;if(from==to)return true;
    }return false;
}
// The issuer alone can mint a witness. Each actual invocation owns a local
// once-only capability; an earlier failed/overlay device consumes no global claim.
class NativeCall final {
    friend class GenshinQueryScope::FactoryBirthIssuer;
    friend class GenshinQueryScope::CreationWitness;
    struct Arguments {
        IDXGIAdapter* adapter=nullptr;D3D_DRIVER_TYPE type{};HMODULE software=nullptr;UINT flags=0;
        const D3D_FEATURE_LEVEL* levels=nullptr;UINT count=0,sdk=0;
        ID3D11Device** device=nullptr;D3D_FEATURE_LEVEL* feature=nullptr;ID3D11DeviceContext** context=nullptr;
        std::array<D3D_FEATURE_LEVEL,16> levelValues{};
        bool LevelsIntact()const noexcept {
            std::array<D3D_FEATURE_LEVEL,16> current{};
            return count<=levelValues.size() && (!count || (levels &&
                detail::Memory(levels,current.data(),count*sizeof(current[0])) &&
                !std::memcmp(current.data(),levelValues.data(),count*sizeof(current[0]))));
        }
    } args_;
    struct ChildReceipt {
        bool started=false,completed=false,inCall=false;DWORD thread=0;
        uint64_t generationBefore=0,generationAfter=0;const void* returned=nullptr;
        PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN executed=nullptr;
        HRESULT result=E_UNEXPECTED;ID3D11Device* device=nullptr;
        ID3D11DeviceContext* context=nullptr;D3D_FEATURE_LEVEL feature{};
    } child_;
    PFN_D3D11_CREATE_DEVICE executed_;
    PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN executedNullSwap_=nullptr;
    bool authentic_=false,ran_=false,executing_=false;
    // First failed native-call predicate is a per-invocation observation only.
    // Check returns its operand unchanged and preserves short-circuit ordering.
    mutable const char* failedPredicate_=nullptr;
    bool Check(bool value,const char* predicate)const noexcept {
        if(!value && !failedPredicate_)failedPredicate_=predicate;return value;
    }
    const char* FailureObservation()const noexcept {return failedPredicate_?failedPredicate_:"none";}
    uint64_t reentry_=0,generation_=0;DWORD thread_=GetCurrentThreadId();
    // Retained with the one-use issuer through capture. These facts describe
    // the executed owner proof, never an alternate way to mint a certificate.
    uintptr_t adapterIdentity_=0,factoryIdentity_=0;
    uint64_t completedGeneration_=0;
    inline static thread_local NativeCall* active_=nullptr;
    NativeCall* prior_=nullptr;
    NativeCall(PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN executed)noexcept:NativeCall(nullptr,executed) {}
    NativeCall(PFN_D3D11_CREATE_DEVICE executed,PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN nullSwap=nullptr)noexcept:
        executed_(executed),executedNullSwap_(nullSwap),prior_(active_) {
        using namespace detail;
        if(!Check(factoryDepth++==0,"factory-reentry") || !Check(reentryEpoch!=UINT64_MAX,"factory-reentry-exhausted")){
            if(reentryEpoch!=UINT64_MAX)++reentryEpoch;return;
        }
        active_=this;reentry_=reentryEpoch;
        if(!Check(store.initialized.load()==2,"native-not-initialized") || !Check(!Revoked(),"native-revoked"))return;
        Guard guard;if(!Check(guard.held,"factory-entry-busy"))return;
        const auto* p=Find(executedNullSwap_?Ledger().swapFactoryEntry:Ledger().factoryEntry);
        try {
            if(!Check(PrepareNativeBirthLocked(),"factory-entry-native-proof")){detail::Revoke();return;}
            authentic_=Check(WholeLocked(),"factory-entry-wholecode") && Check(p!=nullptr,"factory-patch-missing") &&
                Check(!p->input.module,"factory-patch-input-only") &&
                Check(p->straight || (p->predecessor.captured && p->image<Ledger().imageCount &&
                    FactoryCleanStart(*p,Ledger().images[p->image]) && PatchIntact(*p)),"factory-patch-not-straight") &&
                Check(p->trampoline==(executedNullSwap_?reinterpret_cast<void*>(executedNullSwap_):
                    reinterpret_cast<void*>(executed_)),"factory-trampoline-mismatch");
        }catch(...){Check(false,"factory-entry-exception");detail::Revoke();}
        generation_=store.generation.load();
    }
    ~NativeCall(){if(active_==this)active_=prior_;--detail::factoryDepth;}
    NativeCall(const NativeCall&)=delete;
    bool SnapshotOutputs(ID3D11Device*& device,ID3D11DeviceContext*& context,D3D_FEATURE_LEVEL& feature)const noexcept {
        return args_.device && detail::Memory(args_.device,&device,sizeof(device)) &&
            (!args_.context || detail::Memory(args_.context,&context,sizeof(context))) &&
            (!args_.feature || detail::Memory(args_.feature,&feature,sizeof(feature)));
    }
    bool ChildMatches(HRESULT outer)const noexcept {
        if(!child_.started)return true; // Some native images do not use the child export.
        ID3D11Device* device=nullptr;ID3D11DeviceContext* context=nullptr;D3D_FEATURE_LEVEL feature{};
        return child_.completed && !child_.inCall && child_.thread==thread_ && outer==child_.result &&
            SnapshotOutputs(device,context,feature) && device==child_.device && context==child_.context &&
            (!args_.feature || feature==child_.feature);
    }
    HRESULT Run(IDXGIAdapter* adapter,D3D_DRIVER_TYPE type,HMODULE software,UINT flags,const D3D_FEATURE_LEVEL* levels,
        UINT count,UINT sdk,ID3D11Device** device,D3D_FEATURE_LEVEL* feature,ID3D11DeviceContext** context) {
        using namespace detail;
        if(!Check(!ran_,"factory-called-twice") || !Check(executed_!=nullptr || executedNullSwap_!=nullptr,"factory-original-missing")){
            authentic_=false;return E_UNEXPECTED;
        }ran_=true;
        args_.adapter=adapter;args_.type=type;args_.software=software;args_.flags=flags;args_.levels=levels;
        args_.count=count;args_.sdk=sdk;args_.device=device;args_.feature=feature;args_.context=context;
        const bool copied=count<=args_.levelValues.size() && (!count ||
            (levels && Memory(levels,args_.levelValues.data(),count*sizeof(D3D_FEATURE_LEVEL))));
        authentic_=authentic_ && Check(copied,"factory-levels-copy") && Check(device!=nullptr,"factory-device-output") &&
            Check(!software,"factory-software-driver") && Check(sdk==D3D11_SDK_VERSION,"factory-sdk") &&
            Check(!(flags&(D3D11_CREATE_DEVICE_DEBUG|D3D11_CREATE_DEVICE_DEBUGGABLE)),"factory-debug-flags") &&
            Check(adapter?type==D3D_DRIVER_TYPE_UNKNOWN:type==D3D_DRIVER_TYPE_HARDWARE,"factory-driver-type");
        birth_detail::AdapterProof adapterProof;
        if(authentic_ && adapter){
            authentic_=Check(adapterProof.Before(adapter),"factory-adapter-before");
            if(authentic_){adapterIdentity_=adapterProof.AdapterIdentity();
                factoryIdentity_=adapterProof.FactoryIdentity();}
        }
        // No lock spans a COM/native call. Epoch/generation and full byte checks
        // bracket the proof and execution; unexpected hooks still poison history.
        if(authentic_){Guard guard;
            try {authentic_=Check(guard.held,"factory-before-call-busy") && Check(thread_==GetCurrentThreadId(),"factory-before-call-thread") &&
                Check(reentry_==reentryEpoch,"factory-before-call-reentry") && Check(generation_==store.generation.load(),"factory-before-call-epoch") &&
                Check(QuickLocked(),"factory-before-call-quick") && Check(WholeLocked(),"factory-before-call-wholecode");}
            catch(...){Check(false,"factory-before-call-exception");authentic_=false;detail::Revoke();}}
        const bool priorWindow=nativeSetupWindow;nativeSetupWindow=authentic_;executing_=authentic_;
        struct CloseWindow {NativeCall* call;bool prior;~CloseWindow(){call->executing_=false;detail::nativeSetupWindow=prior;}} closeWindow{this,priorWindow};
        // Same attested factory, exactly once. The null-swap entry cannot expose
        // a swapchain or run its callbacks before the one-use birth preparation.
        const auto result=executedNullSwap_?executedNullSwap_(adapter,type,software,flags,levels,count,sdk,
            nullptr,nullptr,device,feature,context):executed_(adapter,type,software,flags,levels,count,sdk,device,feature,context);
        executing_=false;nativeSetupWindow=priorWindow;
        if(!authentic_)return result;
        {Guard guard;
            try {authentic_=Check(result==S_OK,"factory-result") && Check(guard.held,"factory-return-busy") &&
                Check(thread_==GetCurrentThreadId(),"factory-return-thread") && Check(reentry_==reentryEpoch,"factory-return-reentry") &&
                Check(OwnSetupAdvanceLocked(generation_,store.generation.load(),thread_),"factory-return-epoch-chain") &&
                Check(FinishNativeDelaySetup(),"factory-return-delay-imports") && Check(QuickLocked(),"factory-return-quick") &&
                Check(WholeLocked(),"factory-return-wholecode") && Check(args_.LevelsIntact(),"factory-return-levels") &&
                Check(ChildMatches(result),"factory-return-child-receipt");}
            catch(...){Check(false,"factory-return-exception");authentic_=false;detail::Revoke();}}
        ID3D11Device* created=nullptr;
        if(authentic_ && adapter)authentic_=Check(Memory(device,&created,sizeof(created)),"factory-created-device-read") &&
            Check(created!=nullptr,"factory-created-device-null") && Check(adapterProof.After(adapter,created),"factory-adapter-after");
        {Guard guard;
            try {authentic_=authentic_ && Check(guard.held,"factory-final-busy") && Check(thread_==GetCurrentThreadId(),"factory-final-thread") &&
                Check(reentry_==reentryEpoch,"factory-final-reentry") &&
                Check(OwnSetupAdvanceLocked(generation_,store.generation.load(),thread_),"factory-final-epoch-chain") &&
                Check(QuickLocked(),"factory-final-quick") && Check(WholeLocked(),"factory-final-wholecode");}
            catch(...){Check(false,"factory-final-exception");authentic_=false;detail::Revoke();}}
        if(authentic_)completedGeneration_=store.generation.load();
        return result;
    }
    bool Authenticated()const noexcept {return authentic_ && Check(ran_,"factory-not-run") &&
        Check(thread_==GetCurrentThreadId(),"factory-authentication-thread") &&
        Check(reentry_==detail::reentryEpoch,"factory-authentication-reentry") && Check(Intact(),"factory-authentication-intact");}
public:
    // Called BEFORE logging/ordinary skip handlers. False means ordinary
    // forwarding must still call NoteFactoryEntry and revoke unexpected reentry.
    static bool TryNestedSwap(PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN executed,const void* returned,
        IDXGIAdapter* adapter,D3D_DRIVER_TYPE type,HMODULE software,UINT flags,const D3D_FEATURE_LEVEL* levels,
        UINT count,UINT sdk,const DXGI_SWAP_CHAIN_DESC* swapDesc,IDXGISwapChain** swap,
        ID3D11Device** device,D3D_FEATURE_LEVEL* feature,ID3D11DeviceContext** context,HRESULT& result) {
        using namespace detail;auto* call=active_;
        if(!call || call->executedNullSwap_ || !call->authentic_ || !call->executing_ || call->child_.started ||
           call->thread_!=GetCurrentThreadId() || factoryDepth!=1 || call->reentry_!=reentryEpoch ||
           swapDesc || swap)return false;
        const auto& a=call->args_;
        if(adapter!=a.adapter || type!=a.type || software!=a.software || flags!=a.flags || levels!=a.levels ||
           count!=a.count || sdk!=a.sdk || device!=a.device || feature!=a.feature || context!=a.context ||
           !a.LevelsIntact())return false;
        {Guard guard;if(!guard.held)return false;
            const auto* child=Find(Ledger().swapFactoryEntry);
            try {if(!child || !child->straight || child->trampoline!=reinterpret_cast<void*>(executed) ||
                !PatchIntact(*child) || !NestedCallSiteLockedForR692(returned,*child))return false;}
            catch(...){detail::Revoke();return false;}}
        auto& receipt=call->child_;receipt.started=true;receipt.inCall=true;receipt.thread=GetCurrentThreadId();
        receipt.generationBefore=store.generation.load();receipt.executed=executed;receipt.returned=returned;
        result=executed(adapter,type,software,flags,levels,count,sdk,swapDesc,swap,device,feature,context);
        receipt.inCall=false;receipt.result=result;receipt.generationAfter=store.generation.load();
        receipt.completed=call->SnapshotOutputs(receipt.device,receipt.context,receipt.feature);
        {Guard guard;
            try {call->authentic_=call->authentic_ && guard.held && receipt.thread==GetCurrentThreadId() &&
                call->reentry_==reentryEpoch && OwnSetupAdvanceLocked(receipt.generationBefore,receipt.generationAfter,receipt.thread) &&
                FinishNativeDelaySetup() && QuickLocked() && WholeLocked() && a.LevelsIntact();}
            catch(...){call->authentic_=false;detail::Revoke();}}
        return true; // Executed exactly once even on HRESULT failure; never forward again.
    }
private:
    static bool NestedCallSiteLockedForR692(const void* returned,const detail::Patch& child) {
        return detail::QuickLocked() && detail::WholeLocked() && birth_detail::NestedCallSiteLocked(returned,child);
    }
};
} // namespace GenshinNativeQuery
