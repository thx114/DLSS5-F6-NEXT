// SPDX-License-Identifier: GPL-3.0-or-later
// Included inside GenshinNativeQuery::detail after Image. References, observed
// layouts and negative fixtures: audit/v3-r041-reshade-completion-20261027/
// native-trust-fix/. Unknown templates/ABI/syscall encodings fail closed.
namespace scp {
struct Extension {
    ULONG type=0,flags=0;
    uintptr_t rva=0;
    SIZE_T size=0;
    bool operator==(const Extension&)const=default;
};
static_assert(sizeof(Extension)==24 && offsetof(Extension,rva)==8);
using Query=LONG(NTAPI*)(HANDLE,PVOID,ULONG,PVOID,SIZE_T,PSIZE_T);
struct Bounds {uintptr_t begin=0,end=0;};
struct Header {std::array<DWORD,6> offset{};};
enum class Role {Unknown,Dispatch,Check};
// A finite signed export lookup, independent of live EAT/GetProcAddress. No
// forwarders, name aliases, guessed addresses or external code are called.
inline DWORD Export(const Image& image,const char* wanted)noexcept {
    const auto directory=image.exports;
    if(!directory.Size || !image.Range(directory.VirtualAddress,directory.Size) ||
       directory.Size<sizeof(IMAGE_EXPORT_DIRECTORY))return 0;
    const auto e=Read<IMAGE_EXPORT_DIRECTORY>(image.clean.data()+directory.VirtualAddress);
    if(!e.NumberOfNames || e.NumberOfNames>65536 || e.NumberOfFunctions>65536 ||
       !image.Range(e.AddressOfNames,size_t(e.NumberOfNames)*4) ||
       !image.Range(e.AddressOfNameOrdinals,size_t(e.NumberOfNames)*2) ||
       !image.Range(e.AddressOfFunctions,size_t(e.NumberOfFunctions)*4))return 0;
    DWORD result=0;
    for(DWORD i=0;i<e.NumberOfNames;++i){
        const auto name=Read<DWORD>(image.clean.data()+e.AddressOfNames+4*i);
        const auto* text=image.String(name);if(!text)return 0;
        if(std::strcmp(text,wanted))continue;
        const auto ordinal=Read<WORD>(image.clean.data()+e.AddressOfNameOrdinals+2*i);
        if(result || ordinal>=e.NumberOfFunctions)return 0;
        result=Read<DWORD>(image.clean.data()+e.AddressOfFunctions+4*ordinal);
        if(result>=directory.VirtualAddress && result-directory.VirtualAddress<directory.Size)return 0;
    }
    return result;
}
inline bool ExtensionShape(const Extension& e,size_t imageSize,size_t pageSize)noexcept {
    return e.type==0 && e.flags==0 && pageSize && !(pageSize&(pageSize-1)) &&
        pageSize<=65536 && e.rva>=imageSize && !(e.rva&(pageSize-1)) &&
        e.size==pageSize && e.rva<=UINT32_MAX && e.size<=UINT32_MAX-e.rva;
}
// The syscall ID comes from the signed file, never an OS-version table. The
// supported complete leaf includes both syscall and int2e paths and both RETs;
// a different/hooked stub refuses before invoking it. No generated code.
inline bool QueryStub(const unsigned char* b,size_t size)noexcept {
    constexpr unsigned char prefix[]={0x4c,0x8b,0xd1,0xb8};
    constexpr unsigned char tail[]={0xf6,0x04,0x25,0x08,0x03,0xfe,0x7f,0x01,
        0x75,0x03,0x0f,0x05,0xc3,0xcd,0x2e,0xc3};
    return size==24 && !std::memcmp(b,prefix,sizeof(prefix)) &&
        !std::memcmp(b+8,tail,sizeof(tail));
}
// Decode only complete, signed CFG adapter forms tied to the signed load
// config slots. A jmprax sequence anywhere else is never a role certificate.
inline Role OriginalRole(const Image& image,uint32_t original)noexcept {
    if(!image.raw || image.fileSize<64)return Role::Unknown;
    const auto nt=Read<DWORD>(image.raw+offsetof(IMAGE_DOS_HEADER,e_lfanew));
    if(!Span(nt,sizeof(IMAGE_NT_HEADERS64),image.fileSize))return Role::Unknown;
    const auto opt=Read<IMAGE_NT_HEADERS64>(image.raw+nt).OptionalHeader;
    const auto lc=opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    constexpr size_t needed=offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64,GuardCFDispatchFunctionPointer)+8;
    if(lc.Size<needed || !image.Range(lc.VirtualAddress,lc.Size) ||
       !image.RawBacked(lc.VirtualAddress,lc.Size,nt) || !image.RawBacked(original,6,nt) ||
       !image.SectionHas(original,6,IMAGE_SCN_MEM_EXECUTE,IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_DISCARDABLE))return Role::Unknown;
    const auto* config=image.clean.data()+lc.VirtualAddress;
    const auto base=reinterpret_cast<uintptr_t>(image.live);
    const auto check=Read<uint64_t>(config+offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64,GuardCFCheckFunctionPointer));
    const auto dispatch=Read<uint64_t>(config+offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64,GuardCFDispatchFunctionPointer));
    const auto* code=image.clean.data()+original;
    const auto validSlot=[&](uint64_t slot){return slot>=base && slot-base<=UINT32_MAX &&
        image.RawBacked(static_cast<DWORD>(slot-base),8,nt) &&
        image.SectionHas(static_cast<DWORD>(slot-base),8,IMAGE_SCN_MEM_READ,IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_DISCARDABLE);};
    if(code[0]==0xff && code[1]==0x25 && validSlot(dispatch) &&
       int64_t(original)+6+Read<int32_t>(code+2)==int64_t(dispatch-base))return Role::Dispatch;
    constexpr unsigned char start[]={0x48,0x83,0xec,0x28,0x48,0x8b,0x05};
    constexpr unsigned char end[]={0xff,0xd0,0x0f,0x1f,0x00,0x48,0x83,0xc4,0x28,0xc3};
    if(image.RawBacked(original,sizeof(start)+4+sizeof(end),nt) &&
       image.SectionHas(original,sizeof(start)+4+sizeof(end),IMAGE_SCN_MEM_EXECUTE,IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_DISCARDABLE) &&
       !std::memcmp(code,start,sizeof(start)) && !std::memcmp(code+11,end,sizeof(end)) && validSlot(check) &&
       int64_t(original)+11+Read<int32_t>(code+7)==int64_t(check-base))return Role::Check;
    return Role::Unknown;
}
inline bool Relative(uint32_t operand,uintptr_t base,uintptr_t destination,int32_t& result)noexcept {
    if(base>UINTPTR_MAX-operand-4)return false;
    const auto after=base+operand+4;
    if(destination>=after){const auto n=destination-after;if(n>INT32_MAX)return false;result=static_cast<int32_t>(n);}
    else {const auto n=after-destination;if(n>uint64_t(INT32_MAX)+1)return false;
        result=static_cast<int32_t>(-static_cast<int64_t>(n));}
    return true;
}
}
class ScpSource final {
    std::unique_ptr<Image> native_;
    std::vector<unsigned char> page_;
    scp::Extension extension_{};
    scp::Header header_{};
    uint32_t templateRva_=0,templateSize_=0,descriptorRva_=0,queryRva_=0;
    std::array<unsigned char,24> syscall_{};
    bool Query(scp::Extension& result)const noexcept {
        // Kernel query bootstrap: compare the entire signed leaf BEFORE and
        // AFTER invocation; never obtain the callable address from live EAT.
        const auto* entry=reinterpret_cast<const unsigned char*>(Module())+queryRva_;
        if(!queryRva_ || !Equal(entry,syscall_.data(),syscall_.size()))return false;
        SIZE_T returned=0;result={};
        const auto status=reinterpret_cast<scp::Query>(const_cast<unsigned char*>(entry))(
            GetCurrentProcess(),Module(),14,&result,sizeof(result),&returned);
        return status==0 && returned==sizeof(result) && Equal(entry,syscall_.data(),syscall_.size());
    }
    bool Initialize() {
        native_=std::make_unique<Image>();
        const auto module=GetModuleHandleW(L"ntdll.dll");
        if(!module || !native_->PrepareReference(module))return false;
        const auto& image=*native_;const auto base=reinterpret_cast<uintptr_t>(module);
        const auto nt=Read<DWORD>(image.raw+offsetof(IMAGE_DOS_HEADER,e_lfanew));
        descriptorRva_=scp::Export(image,"RtlpScpCfgNtdllExports");
        queryRva_=scp::Export(image,"NtQueryVirtualMemory");
        constexpr size_t exportSize=sizeof(scp::Bounds)*4+sizeof(uintptr_t)*5;
        if(!descriptorRva_ || !image.Range(descriptorRva_,exportSize) ||
           !image.RawBacked(descriptorRva_,exportSize,nt) ||
           !image.SectionHas(descriptorRva_,exportSize,IMAGE_SCN_MEM_READ,IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_EXECUTE) ||
           !queryRva_ || !image.RawBacked(queryRva_,syscall_.size(),nt) ||
           !image.SectionHas(queryRva_,syscall_.size(),IMAGE_SCN_MEM_EXECUTE,IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_DISCARDABLE))return false;
        std::memcpy(syscall_.data(),image.clean.data()+queryRva_,syscall_.size());
        if(!scp::QueryStub(syscall_.data(),syscall_.size()))return false;
        SYSTEM_INFO system{};GetSystemInfo(&system);
        if(!Query(extension_) || !scp::ExtensionShape(extension_,image.clean.size(),system.dwPageSize) ||
           base>UINTPTR_MAX-extension_.rva-extension_.size)return false;
        std::array<scp::Bounds,4> regions{};
        std::memcpy(regions.data(),image.clean.data()+descriptorRva_,sizeof(regions));
        for(unsigned i=0;i<regions.size();++i){const auto region=regions[i];
            if(region.begin<base || region.end<=region.begin || region.end-base>UINT32_MAX)return false;
            const auto rva=static_cast<uint32_t>(region.begin-base);
            const auto size=region.end-region.begin;
            if(size<sizeof(scp::Header) || size>extension_.size || !image.RawBacked(rva,size,nt) ||
               !image.SectionHas(rva,size,IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_READ,IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_DISCARDABLE))return false;
            for(unsigned j=0;j<i;++j)if(region.begin<regions[j].end && regions[j].begin<region.end)return false;
            // Verify each exported bound is exactly a signed virtual section,
            // rather than trusting the spelling of a section or a live header.
            bool exact=false;
            for(unsigned j=0;j<image.sectionCount;++j){
                const auto s=Read<IMAGE_SECTION_HEADER>(image.raw+nt+sizeof(IMAGE_NT_HEADERS64)+j*sizeof(IMAGE_SECTION_HEADER));
                if(rva==s.VirtualAddress && size==s.Misc.VirtualSize)exact=true;
            }
            if(!exact)return false;
            const auto h=Read<scp::Header>(image.clean.data()+rva);
            for(unsigned j=0;j<h.offset.size();++j)
                if(h.offset[j]<sizeof(h) || h.offset[j]>=size || (j && h.offset[j]<=h.offset[j-1]))return false;
            if(i==0){templateRva_=rva;templateSize_=static_cast<uint32_t>(size);header_=h;}
        }
        // Initial supported profile is the unmodified NOP template. No CFG
        // bitmap address or function-pointer placeholder is ever read/adopted.
        // Copy only signed Begin..End; the remainder must be exact INT3 fill.
        // Both template and padding are compared, never masked or skipped.
        page_.assign(extension_.size,0xcc);
        std::memcpy(page_.data(),image.clean.data()+templateRva_,templateSize_);
        return Intact();
    }
public:
    HMODULE Module()const noexcept {return native_?native_->live:nullptr;}
    size_t ReferenceBytes()const noexcept {return native_?native_->clean.size()+page_.size():0;}
    // Byte-comparison primitive shared with private-buffer tamper fixtures.
    // This alone has no range/kernel/source identity or issuing semantics.
    bool PageMatches(const void* bytes)const noexcept {
        return !page_.empty() && Equal(bytes,page_.data(),page_.size());
    }
    bool Intact()const noexcept {
        if(!native_ || page_.empty())return false;
        scp::Extension current{};
        if(!Query(current) || current!=extension_)return false;
        const auto* base=reinterpret_cast<const unsigned char*>(Module());
        constexpr size_t exportSize=sizeof(scp::Bounds)*4+sizeof(uintptr_t)*5;
        if(!Equal(base+descriptorRva_,native_->clean.data()+descriptorRva_,exportSize) ||
           !Equal(base+templateRva_,native_->clean.data()+templateRva_,templateSize_))return false;
        const auto address=reinterpret_cast<uintptr_t>(Module())+extension_.rva;
        MEMORY_BASIC_INFORMATION memory{};
        if(!VirtualQuery(reinterpret_cast<const void*>(address),&memory,sizeof(memory)) ||
           memory.AllocationBase!=Module() || memory.State!=MEM_COMMIT || memory.Type!=MEM_IMAGE ||
           memory.Protect!=PAGE_EXECUTE_READ || address<reinterpret_cast<uintptr_t>(memory.BaseAddress) ||
           !Span(address-reinterpret_cast<uintptr_t>(memory.BaseAddress),page_.size(),memory.RegionSize))return false;
        // MEM_IMAGE/RX are range guards only. Exact derived page equality is
        // mandatory; neither property authenticates arbitrary live code.
        return PageMatches(reinterpret_cast<const void*>(address));
    }
    bool Destination(scp::Role role,uintptr_t observed,uintptr_t& derived)const noexcept {
        if(role==scp::Role::Unknown)return false;
        // NOP template has the same role with/without export suppression.
        // Accept only that role's two signed header entrypoints, never another
        // role or an interior instruction. No process-feature guess is needed.
        const auto first=role==scp::Role::Dispatch?0u:2u;
        for(unsigned i=first;i<first+2;++i){
            const auto target=reinterpret_cast<uintptr_t>(Module())+extension_.rva+header_.offset[i];
            if(observed==target){derived=target;return true;}
        }
        return false;
    }
    static std::shared_ptr<ScpSource> Acquire() {
        // One signed reference per active ledger, retained by every dependent
        // Image. Weak cache releases file/mapping data when no image uses it.
        static std::mutex mutex;static std::weak_ptr<ScpSource> cache;
        const std::lock_guard<std::mutex> lock(mutex);
        if(auto source=cache.lock())return source->Intact()?source:nullptr;
        auto source=std::make_shared<ScpSource>();
        if(!source->Initialize())return nullptr;
        cache=source;return source;
    }
};
// One validation pass may encounter the same retained source through many
// Images and the existing compiled-plan fallback. Reuse only within that
// lexical pass and the unchanged generation. No result survives its destructor.
inline uint64_t ScpValidationEpoch()noexcept;
class ScpValidationPass final {
    inline static thread_local ScpValidationPass* active_=nullptr;
    ScpValidationPass* prior_=nullptr;
    ScpValidationPass* owner_=nullptr;
    uint64_t epoch_=0;
    std::array<const ScpSource*,96> checked_{};unsigned count_=0;
public:
    ScpValidationPass()noexcept:prior_(active_),epoch_(ScpValidationEpoch()) {
        // Nested work remains part of its original pass. A generation change
        // must reject that work, never start a fresh nested success cache.
        owner_=prior_?prior_->owner_:this;
        active_=this;
    }
    ~ScpValidationPass(){active_=prior_;}
    ScpValidationPass(const ScpValidationPass&)=delete;
    ScpValidationPass& operator=(const ScpValidationPass&)=delete;
    static bool Check(const ScpSource& source)noexcept {
        if(!active_)return source.Intact();
        auto& pass=*active_->owner_;
        if(pass.epoch_!=ScpValidationEpoch())return false;
        for(unsigned i=0;i<pass.count_;++i)if(pass.checked_[i]==&source)return true;
        if(pass.count_==pass.checked_.size() || !source.Intact() || pass.epoch_!=ScpValidationEpoch())return false;
        pass.checked_[pass.count_++]=&source;return true;
    }
};
inline bool Image::MatchScpSource()noexcept {
    return !scpSource || ScpValidationPass::Check(*scpSource) || OpenReject("scp-source-or-extension-changed");
}
inline HMODULE Image::ScpDependency()const noexcept {return scpSource?scpSource->Module():nullptr;}
inline size_t Image::ScpReferenceBytes()const noexcept {return scpSource?scpSource->ReferenceBytes():0;}
inline bool Image::NormalizeDynamicFixups() {
    if(cleanFrozen_)return OpenReject("dynamic-reference-already-frozen");
    struct Edit {uint32_t rva;int32_t displacement;};
    std::vector<Edit> edits;
    std::shared_ptr<ScpSource> source;
    const auto base=reinterpret_cast<uintptr_t>(live);
    for(const auto& fixup:dynamicFixups){
        if(!fixup.rva || !Range(fixup.rva-1,5))return OpenReject("dynamic-target-range",fixup.rva);
        std::array<unsigned char,5> bytes{};
        if(!Memory(reinterpret_cast<const unsigned char*>(live)+fixup.rva-1,bytes.data(),bytes.size()))
            return OpenReject("dynamic-target-read",fixup.rva,0,bytes.size(),GetLastError());
        if(!std::memcmp(bytes.data(),clean.data()+fixup.rva-1,bytes.size()))continue;
        const auto reject=[&](){
            for(unsigned i=0;i<bytes.size();++i)if(bytes[i]!=clean[fixup.rva-1+i])
                return OpenReject("dynamic-target-byte-equality",fixup.rva-1+i,bytes[i],clean[fixup.rva-1+i]);
            return false;
        };
        if(bytes[0]!=clean[fixup.rva-1])return reject();
        const int64_t rva=int64_t(fixup.rva)+4+Read<int32_t>(bytes.data()+1);
        uintptr_t derived=0;bool found=false;
        for(const auto target:fixup.alternatives)if(rva==target){
            derived=base+target;found=true;break;
        }
        if(!found){
            const auto role=scp::OriginalRole(*this,fixup.original);
            if(role==scp::Role::Unknown)return reject();
            if(!source)source=ScpSource::Acquire();
            if(!source)return OpenReject("scp-signed-template-or-kernel-extension",fixup.rva);
            uintptr_t observed=0;
            if(rva>=0){if(uint64_t(rva)>UINTPTR_MAX-base)return reject();observed=base+static_cast<uintptr_t>(rva);}
            else {if(uint64_t(-rva)>base)return reject();observed=base-static_cast<uintptr_t>(-rva);}
            if(!source->Destination(role,observed,derived))return reject();
        }
        int32_t displacement=0;
        if(!scp::Relative(fixup.rva,base,derived,displacement))return OpenReject("dynamic-rel32-range",fixup.rva);
        edits.push_back({fixup.rva,displacement});
    }
    if(source && !source->Intact())return OpenReject("scp-source-or-extension-changed");
    // Commit a complete derived reference only after every descriptor passed.
    // No expected byte ever comes from the observed instruction buffer.
    for(const auto edit:edits)std::memcpy(clean.data()+edit.rva,&edit.displacement,sizeof(edit.displacement));
    scpSource=std::move(source);
    return true;
}
