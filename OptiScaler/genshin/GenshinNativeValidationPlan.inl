// SPDX-License-Identifier: GPL-3.0-or-later
// Included inside detail, after the complete reference validator. This is a
// compiled list of proof obligations, never a cache of an admission result or
// live memory. All expectations come from authenticated clean data and sealed
// receipts. Resolver calls precede EVERY fresh memory comparison on admission.
// Equality only, with bounded intrinsic loads. Unlike lexicographic memcmp,
// this needs no CRT dispatch and never reads beyond either checked span.
__forceinline bool NativePlanEqual(const void* left,const void* right,size_t size)noexcept {
    auto* a=static_cast<const unsigned char*>(left);auto* b=static_cast<const unsigned char*>(right);
    while(size>=64){
        const auto c0=_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a)),_mm_loadu_si128(reinterpret_cast<const __m128i*>(b)));
        const auto c1=_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a+16)),_mm_loadu_si128(reinterpret_cast<const __m128i*>(b+16)));
        const auto c2=_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a+32)),_mm_loadu_si128(reinterpret_cast<const __m128i*>(b+32)));
        const auto c3=_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a+48)),_mm_loadu_si128(reinterpret_cast<const __m128i*>(b+48)));
        if(_mm_movemask_epi8(_mm_and_si128(_mm_and_si128(c0,c1),_mm_and_si128(c2,c3)))!=0xffff)return false;
        a+=64;b+=64;size-=64;
    }
    while(size>=16){if(_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a)),
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(b))))!=0xffff)return false;a+=16;b+=16;size-=16;}
    while(size--){if(*a++!=*b++)return false;}return true;
}
struct NativeValidationPlan {
    struct Key {
        const LedgerData* ledger=nullptr;uint64_t revision=0,epoch=0;
        std::array<uint64_t,12> shape{};
        std::vector<CleanSlotRevision> images;
    } key;
    struct MetadataCheck {const void* address=nullptr;size_t offset=0,size=0;};
    enum class Failure : unsigned char {Representation,Dynamic,Patch,Import};
    struct ReadCheck {uintptr_t address=0;size_t offset=0,size=0;Failure failure=Failure::Import;size_t extent=0;size_t batchNext=0;uintptr_t batchEnd=0;};
    struct RegionCheck {
        uintptr_t address=0;size_t size=0;HMODULE image=nullptr;
        uintptr_t allocation=0,region=0;size_t regionSize=0;DWORD protect=0;
        bool privateRow=false;
        // Only Patches() may declare authenticated frozen predecessor code.
        bool code=false;DWORD type=0;
        bool operator==(const RegionCheck&)const=default;
    };
    struct Module {const wchar_t* name=nullptr;HMODULE expected=nullptr;};
    struct Resolver {HMODULE module=nullptr;const char* name=nullptr;uintptr_t expected=0;};
    struct OwnerCheck {uintptr_t address=0;HMODULE expected=nullptr;};
    std::vector<unsigned char> expected;
    std::vector<unsigned char> metadata;
    std::vector<MetadataCheck> metadataChecks;
    std::vector<ReadCheck> reads;
    std::vector<RegionCheck> regions;
    std::vector<Module> modules;
    std::vector<Resolver> resolvers;
    std::vector<OwnerCheck> owners;
    bool ready=false;
};
inline NativeValidationPlan::Key NativePlanKey() {
    const auto& l=Ledger();NativeValidationPlan::Key key{};
    key.ledger=&l;key.revision=l.validationRevision;key.epoch=store.generation.load();
    key.shape={l.imageCount,l.importCount,l.patchCount,l.transitionCount,l.runtimeFileCount,l.delayedCount,
        l.shim.edgeCount,l.shim.fileCount,static_cast<uint64_t>(l.shim.state),l.imageBytes,
        l.shim.frozenFileCount,RepresentationCount()};
    for(unsigned i=0;i<l.imageCount;++i)key.images.push_back(shim_binding::CleanSlotKey(l.images[i]));
    return key;
}
inline bool NativePlanMatches(const NativeValidationPlan& plan,bool metadata=true)noexcept {
    const auto& l=Ledger();const auto& k=plan.key;
    if(!plan.ready || !StableLocked() || k.ledger!=&l || k.revision!=l.validationRevision ||
       k.epoch!=store.generation.load() || l.imageCount>l.images.size() ||
       k.images.size()!=l.imageCount ||
       k.shape!=std::array<uint64_t,12>{l.imageCount,l.importCount,l.patchCount,l.transitionCount,
           l.runtimeFileCount,l.delayedCount,l.shim.edgeCount,l.shim.fileCount,
           static_cast<uint64_t>(l.shim.state),l.imageBytes,l.shim.frozenFileCount,RepresentationCount()})return false;
    for(unsigned i=0;i<l.imageCount;++i)
        if(!l.images[i].CleanRevision() || !(k.images[i]==shim_binding::CleanSlotKey(l.images[i])))return false;
    // Sealed receipt storage is internal metadata, not game live bytes. Check
    // it as well so a stale same-count ledger can never reuse a compiled plan.
    if(metadata){GenshinCpuStageTiming::Scope cost(GenshinCpuStageTiming::Stage::PlanMetadata);
        for(const auto& m:plan.metadataChecks)
            if(!NativePlanEqual(m.address,plan.metadata.data()+m.offset,m.size))return false;
    }
    return true;
}
class NativePlanCompiler final {
    NativeValidationPlan& p;
    using Failure=NativeValidationPlan::Failure;
public:
    explicit NativePlanCompiler(NativeValidationPlan& plan):p(plan){}
    void Metadata(const void* address,size_t size) {
        if(!size)return;const auto* bytes=static_cast<const unsigned char*>(address);
        const auto offset=p.metadata.size();p.metadata.insert(p.metadata.end(),bytes,bytes+size);
        p.metadataChecks.push_back({address,offset,size});
    }
    void Metadata() {
        const auto& l=Ledger();Metadata(l.imports.data(),l.importCount*sizeof(Import));
        Metadata(l.patches.data(),l.patchCount*sizeof(Patch));Metadata(&l.shim,sizeof(l.shim));
        Metadata(l.runtimeFileEdges.data(),l.runtimeFileCount*sizeof(FileRepresentedEdge));
        Metadata(l.delayedEdges.data(),l.delayedCount*sizeof(DelayedRepresentation));
        Metadata(l.delayedIndex.data(),l.importCount);
        for(unsigned i=0;i<l.imageCount;++i){const auto& image=l.images[i];
            Metadata(&image.live,sizeof(image.live));Metadata(&image.raw,sizeof(image.raw));
            Metadata(&image.fileSize,sizeof(image.fileSize));Metadata(image.sections.data(),sizeof(image.sections));
            Metadata(&image.sectionCount,sizeof(image.sectionCount));Metadata(&image.imports,sizeof(image.imports));
            Metadata(&image.delays,sizeof(image.delays));Metadata(&image.exports,sizeof(image.exports));
            Metadata(&image.dynamicFixups,sizeof(image.dynamicFixups));
            Metadata(image.dynamicFixups.data(),image.dynamicFixups.size()*sizeof(Image::DynamicFixup));
        }
    }
    bool Bytes(const void* address,const void* expected,size_t n,Failure f) {
        auto at=reinterpret_cast<uintptr_t>(address);const auto* bytes=static_cast<const unsigned char*>(expected);
        if(!at || !bytes || !n || at>UINTPTR_MAX-n)return false;
        while(n){const auto take=(std::min)(n,size_t(4096));
            const auto offset=p.expected.size();p.expected.insert(p.expected.end(),bytes,bytes+take);
            p.reads.push_back({at,offset,take,f,take});at+=take;bytes+=take;n-=take;
        }return true;
    }
    template<class T> bool Value(const void* at,const T& value,Failure f=Failure::Representation) {
        return Bytes(at,&value,sizeof(value),f);
    }
    bool Clean(const Image& image,DWORD rva,size_t size,Failure f=Failure::Representation) {
        return image.Range(rva,size) && Bytes(reinterpret_cast<const unsigned char*>(image.live)+rva,
            image.clean.data()+rva,size,f);
    }
    void Module(const wchar_t* name,HMODULE expected) {
        for(const auto& m:p.modules)if(!std::wcscmp(m.name,name)){
            if(m.expected!=expected)throw false;return;}
        if(!expected)throw false;p.modules.push_back({name,expected});
    }
    void Resolver(HMODULE module,const char* name,uintptr_t expected) {
        if(!module || !name || !expected)throw false;
        for(const auto& r:p.resolvers)if(r.module==module && !std::strcmp(r.name,name)){
            if(r.expected!=expected)throw false;return;}
        p.resolvers.push_back({module,name,expected});
    }
    void Owner(uintptr_t address,HMODULE expected) {
        for(const auto& o:p.owners)if(o.address==address){if(o.expected!=expected)throw false;return;}
        p.owners.push_back({address,expected});
    }
    bool Data(const Image& image,DWORD rva,size_t size) {
        if(!image.SectionHas(rva,size,IMAGE_SCN_MEM_READ,IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_DISCARDABLE))return false;
        p.regions.push_back({reinterpret_cast<uintptr_t>(image.live)+rva,size,image.live});return true;
    }
    bool Profile(const Image& image,const shim_observation::Profile& profile) {
        // The full reference proof above authenticates the raw profile/clean
        // version facts. Retain both original live byte obligations here.
        const auto nt=Read<DWORD>(profile.dos.data()+offsetof(IMAGE_DOS_HEADER,e_lfanew));
        if(!Span(nt,sizeof(IMAGE_NT_HEADERS64),image.fileSize))return false;
        const auto header=Read<IMAGE_NT_HEADERS64>(image.raw+nt);
        return Clean(image,0,header.OptionalHeader.SizeOfHeaders) &&
            Bytes(reinterpret_cast<const unsigned char*>(image.live)+profile.versionRva,
                profile.version.data(),profile.version.size(),Failure::Representation);
    }
    bool Row(DWORD offset,const std::array<uintptr_t,4>& values) {
        const auto& s=Ledger().shim;if(s.table>UINTPTR_MAX-offset)return false;
        const auto at=s.table+offset;
        p.regions.push_back({at,6*sizeof(uintptr_t),nullptr,s.allocation,s.region,s.regionSize,s.protect,true});
        // q4/q5 remain opaque. Readability of all six words is still checked.
        if(!Bytes(reinterpret_cast<void*>(at),values.data(),sizeof(values),Failure::Representation))return false;
        p.reads.back().extent=6*sizeof(uintptr_t);return true;
    }
    bool Layout(bool file) {
        const auto& l=Ledger();const auto& s=l.shim;const auto& recipe=shim_binding::Recipe();
        const auto ai=detail::Owner(s.apphelp),ni=detail::Owner(s.native);if(ai<0 || ni<0)return false;
        const auto& a=l.images[ai];const auto& n=l.images[ni];
        const auto app=reinterpret_cast<uintptr_t>(s.apphelp),native=reinterpret_cast<uintptr_t>(s.native);
        Module(L"apphelp.dll",s.apphelp);Module(L"ntdll.dll",s.native);
        if(!Profile(a,*recipe.app) || !Profile(n,*recipe.native))return false;
        for(const auto rva:{recipe.tableCell,recipe.xfgCell,recipe.cfCell,recipe.loadConfig+0x120,recipe.loadConfig+0x78})
            if(!Data(a,rva,8))return false;
        if(!Clean(a,recipe.loadConfig+0x120,8) || !Clean(a,recipe.loadConfig+0x78,8) ||
           !Value(reinterpret_cast<void*>(app+recipe.tableCell),s.table) ||
           !Value(reinterpret_cast<void*>(app+recipe.xfgCell),app+recipe.xfg.rva) ||
           !Value(reinterpret_cast<void*>(app+recipe.cfCell),app+recipe.cf.rva) ||
           !Bytes(reinterpret_cast<void*>(app+recipe.wrapper.rva),recipe.wrapper.bytes,recipe.wrapper.size,Failure::Representation) ||
           !Bytes(reinterpret_cast<void*>(app+recipe.xfg.rva),recipe.xfg.bytes,recipe.xfg.size,Failure::Representation) ||
           !Bytes(reinterpret_cast<void*>(app+recipe.cf.rva),recipe.cf.bytes,recipe.cf.size,Failure::Representation) ||
           !Row(0x60,{app+recipe.dllName,app+recipe.families[2].symbol,app+recipe.wrapper.rva,native+shim_binding::NativeRva()}))return false;
        // Reference seal already proved the named DVRT/CFG role and derived
        // SCP source. Compile its exact frozen bytes, never the disk REL32.
        // QuickLocked's existing source guard remains ahead of this plan.
        if(recipe.dispatch.rva && (!Clean(a,recipe.initializer.rva,recipe.initializer.size) ||
           !Clean(a,recipe.dispatchOriginal.rva,recipe.dispatchOriginal.size) ||
           !Clean(a,recipe.dispatch.rva,recipe.dispatch.size,Failure::Dynamic)))return false;
        if(!file)return true;
        if(!Clean(a,recipe.create.rva,recipe.create.size) ||
           !Clean(a,recipe.setInfo.rva,recipe.setInfo.size) ||
           !Clean(a,recipe.initializer.rva,recipe.initializer.size))return false;
        for(const auto& f:file_binding::Families()){const auto size=std::strlen(f.nt)+1;
            if(!Clean(a,f.wrapper,f.end-f.wrapper) || !Data(a,f.symbol,size) ||
               !Bytes(reinterpret_cast<void*>(app+f.symbol),f.nt,size,Failure::Representation) ||
               !Row(f.row,{app+recipe.dllName,app+f.symbol,app+f.wrapper,native+f.nativeRva}))return false;
        }return true;
    }
    bool Installer(unsigned selected,const CanonicalBinding& canonical) {
        if(selected<file_binding::nativeSelectors)return true;
        const auto index=selected-file_binding::nativeSelectors;if(index>=file_binding::Installers().size())return false;
        const auto& f=file_binding::Installers()[index];const auto& l=Ledger();
        const auto& s=l.shim;const auto owner=detail::Owner(s.apphelp);if(owner<0)return false;
        const auto& image=l.images[owner];const auto app=reinterpret_cast<uintptr_t>(s.apphelp);
        Module(f.owner,canonical.owner);
        const auto size=f.end-f.wrapper;if(!size || size>64 || !image.Range(f.wrapper,size))return false;
        std::array<unsigned char,64> expected{};std::memcpy(expected.data(),image.clean.data()+f.wrapper,size);
        const auto first=app+f.wrapper,last=first+size;
        for(unsigned i=0;i<l.patchCount;++i){const auto& patch=l.patches[i];const auto at=reinterpret_cast<uintptr_t>(patch.entry);
            if(at>=last || at+patch.size<=first)continue;
            // OwnedWrapperCode in the full compiler proof validates the exact
            // signed preimage, ownSetup, overlap and patch receipt predicates.
            if(at<first || patch.size>last-at)return false;
            std::memcpy(expected.data()+at-first,patch.edit.data(),patch.size);
        }
        const auto dllSize=std::strlen(f.dll)+1,nameSize=std::strlen(f.symbol)+1;
        return Bytes(reinterpret_cast<void*>(first),expected.data(),size,Failure::Representation) &&
            Data(image,f.dllRva,dllSize) && Data(image,f.symbolRva,nameSize) &&
            Bytes(reinterpret_cast<void*>(app+f.dllRva),f.dll,dllSize,Failure::Representation) &&
            Bytes(reinterpret_cast<void*>(app+f.symbolRva),f.symbol,nameSize,Failure::Representation) &&
            Row(f.row,{app+f.dllRva,app+f.symbolRva,app+f.wrapper,reinterpret_cast<uintptr_t>(canonical.owner)+canonical.rva});
    }
    bool Representations() {
        const auto& l=Ledger();const auto& s=l.shim;
        const bool file=s.fileCount || l.runtimeFileCount || l.delayedCount;
        if(s.state==RepresentationState::Absent)return !file && !s.edgeCount;
        if(s.state!=RepresentationState::Sealed || !Layout(file))return false;
        for(unsigned i=0;i<5;++i){const auto role=static_cast<ShimRole>(i);
            const auto expected=shim_binding::RoleModule(role,s.apphelp);
            if(role==ShimRole::Outer)Module(L"kernel32.dll",expected);
            if(role==ShimRole::BaseNt || role==ShimRole::BaseAlias)Module(L"kernelbase.dll",expected);
        }
        for(unsigned i=0;i<s.edgeCount;++i){const auto& e=s.edges[i];
            Resolver(s.native,e.name,e.resolverTarget);if(!Value(e.slot,e.iatTarget))return false;
        }
        for(unsigned i=0;i<l.importCount;++i){const auto& e=l.imports[i];ShimRole role{};
            if(!shim_binding::SelectRole(e.dll,e.name,e.ordinal,e.importer,e.slot,s.apphelp,role))continue;
            if(role!=ShimRole::BaseNt && role!=ShimRole::BaseAlias)return false;
            Resolver(s.native,shim_binding::RoleName(role),reinterpret_cast<uintptr_t>(s.native)+shim_binding::NativeRva());
        }
        const auto edge=[&](const FileRepresentedEdge& e){
            Resolver(e.canonical.owner,file_binding::SelectorName(e.selector),e.resolverTarget);
            return Value(e.slot,e.iatTarget) && Installer(e.selector,e.canonical);
        };
        for(unsigned i=0;i<s.fileCount;++i)if(!edge(s.fileEdges[i]))return false;
        for(unsigned i=0;i<l.runtimeFileCount;++i)if(!edge(l.runtimeFileEdges[i]))return false;
        for(unsigned i=0;i<l.delayedCount;++i){const auto& r=l.delayedEdges[i];
            if(r.importIndex>=l.importCount || l.delayedIndex[r.importIndex]!=i+1 || !edge(r.edge))return false;
            Owner(r.edge.iatTarget,s.apphelp);
        }return true;
    }
    bool Patches() {
        const auto& l=Ledger();if(l.patchCount>l.patches.size())return false;
        for(unsigned i=0;i<l.patchCount;++i){const auto& x=l.patches[i];
            if(x.hopCount>x.hops.size() || !Value(x.original,x.trampoline,Failure::Patch))return false;
            for(auto* slot:{x.executed,x.mirror})if(slot && !Value(slot,x.trampoline,Failure::Patch))return false;
            for(unsigned j=0;j<x.hopCount;++j){const auto& h=x.hops[j];
                if(!Bytes(h.entry,h.code.data(),h.size,Failure::Patch) || (h.iat && !Value(h.iat,h.target,Failure::Patch)))return false;
            }
            if(x.predecessor.captured){const auto& r=x.predecessor;
                if(!x.factoryCleanCaptured || x.image>=l.imageCount || !FactoryCleanStart(x,l.images[x.image]) ||
                   !FactoryPredecessorIntact(r,x.size))return false;
                // Same proof as Index/Bind, compiled once per sealed revision.
                // Admission reuses the existing batched byte/region operations.
                for(unsigned n=0;n<r.nodes.size();++n){const auto& node=r.nodes[n];const auto skip=n==0?x.size:0;
                    if(!Bytes(reinterpret_cast<void*>(node.address+skip),node.bytes.data()+skip,
                              node.bytes.size()-skip,Failure::Patch))return false;
                    const auto& m=node.mapping;
                    p.regions.push_back({node.address,node.bytes.size(),nullptr,m.allocation,m.region,m.size,m.protect,false,true,m.type});
                }
                if(!Value(reinterpret_cast<void*>(r.slot),r.terminal,Failure::Patch))return false;
                const auto& m=r.slotMapping;
                p.regions.push_back({r.slot,sizeof(uintptr_t),nullptr,m.allocation,m.region,m.size,m.protect,false,true,m.type});
                const auto freeze=+[](void* context,const void* address,const void* expected,size_t size,
                                      const MEMORY_BASIC_INFORMATION& m)->bool {
                    auto& compiler=*static_cast<NativePlanCompiler*>(context);
                    if(!compiler.Bytes(address,expected,size,Failure::Patch))return false;
                    compiler.p.regions.push_back({reinterpret_cast<uintptr_t>(address),size,nullptr,
                        reinterpret_cast<uintptr_t>(m.AllocationBase),reinterpret_cast<uintptr_t>(m.BaseAddress),
                        m.RegionSize,m.Protect,false,true,m.Type});return true;
                };
                if(!HoYoShade::ExistingFactoryOwnerIntact(r.owner,reinterpret_cast<void*>(r.terminal),r.symbol,freeze,this))return false;
            }
            if(!Bytes(x.entry,x.edit.data(),x.size,Failure::Patch) ||
               !Bytes(x.trampoline,x.code.data(),x.code.size(),Failure::Patch) ||
               !Bytes(x.hook,x.ownEntry.data(),x.ownEntry.size(),Failure::Patch))return false;
        }return true;
    }
    bool ImportsAndDynamic() {
        const auto& l=Ledger();if(l.importCount>l.imports.size())return false;
        for(unsigned i=0;i<l.importCount;++i){uintptr_t target=0;
            if(!ExpectedImportTarget(i,target) || !Value(l.imports[i].slot,target,Failure::Import))return false;
        }
        for(unsigned i=0;i<l.imageCount;++i){const auto& image=l.images[i];
            for(const auto& f:image.dynamicFixups)if(!f.rva || !Clean(image,f.rva-1,5,Failure::Dynamic))return false;
        }return true;
    }
    bool Finish() {
        std::sort(p.reads.begin(),p.reads.end(),[](const auto& a,const auto& b){
            return a.address<b.address || (a.address==b.address && a.size<b.size);});
        // Remove identical obligations only; conflicting expectations stay and
        // necessarily fail. No live read or policy result is retained.
        p.reads.erase(std::unique(p.reads.begin(),p.reads.end(),[&](const auto& a,const auto& b){
            return a.address==b.address && a.size==b.size && a.extent==b.extent && a.failure==b.failure &&
                !std::memcmp(p.expected.data()+a.offset,p.expected.data()+b.offset,a.size);
        }),p.reads.end());
        // Compile adjacent comparisons too: the hot loop compares one bounded
        // clean expected span, rather than thousands of eight-byte memcmps.
        std::vector<unsigned char> expected;std::vector<NativeValidationPlan::ReadCheck> merged;
        expected.reserve(p.expected.size());merged.reserve(p.reads.size());
        for(const auto& r:p.reads){
            if(!merged.empty()){auto& previous=merged.back();
                if(previous.address+previous.size==r.address && previous.failure==r.failure &&
                   previous.extent==previous.size && r.extent==r.size && previous.size+r.size<=4096){
                    expected.insert(expected.end(),p.expected.data()+r.offset,p.expected.data()+r.offset+r.size);
                    previous.size+=r.size;previous.extent=previous.size;continue;
                }
            }
            auto copy=r;copy.offset=expected.size();merged.push_back(copy);
            expected.insert(expected.end(),p.expected.data()+r.offset,p.expected.data()+r.offset+r.size);
        }
        p.expected=std::move(expected);p.reads=std::move(merged);
        // Compile each possible region-split entry point. Region boundaries
        // stay live: admission clips these address-only windows to fresh VQ.
        for(size_t first=0;first<p.reads.size();++first){auto& check=p.reads[first];
            auto end=check.address+check.extent;size_t next=first+1;
            while(next<p.reads.size()){const auto& r=p.reads[next];
                if(r.address<check.address || r.address+r.extent<r.address ||
                   r.address+r.extent-check.address>65536 || (r.address>end && r.address-end>128))break;
                end=(std::max)(end,r.address+r.extent);++next;
            }
            check.batchNext=next;check.batchEnd=end;
        }
        std::sort(p.regions.begin(),p.regions.end(),[](const auto& a,const auto& b){return a.address<b.address;});
        p.regions.erase(std::unique(p.regions.begin(),p.regions.end()),p.regions.end());
        return true;
    }
};
inline bool RebuildNativeValidationPlan(bool sealedDraft=false)noexcept {
    auto& l=Ledger();l.validationPlan.reset();
    if((sealedDraft?(!draftLedger || store.initialized.load()!=1 || Revoked() || (store.generation.load()&1)):
        (!StableLocked() || draftLedger)) || l.imageCount>l.images.size())return false;
    for(unsigned i=0;i<l.imageCount;++i)if(!l.images[i].CleanRevision())return false;
    try {
        // Complete static selector/alias/role/layout/patch proof is compiled at
        // publication/revision, not repeated on every admission. A failed or
        // unsupported compile uses the unchanged complete reference validator.
        auto p=std::make_shared<NativeValidationPlan>();p->key=NativePlanKey();
        if(!sealedDraft && !QuickLockedFull())return false;
        NativePlanCompiler compiler(*p);
        compiler.Metadata();
        if(!compiler.Representations() || !compiler.Patches() || !compiler.ImportsAndDynamic() || !compiler.Finish())return false;
        p->ready=true;
        if(sealedDraft){if(Revoked() || p->key.epoch!=store.generation.load())return false;}
        else if(!NativePlanMatches(*p))return false;
        l.validationPlan=std::move(p);return true;
    }catch(...){return false;}
}
inline bool NativePlanFailure(NativeValidationPlan::Failure f)noexcept {
    if(f==NativeValidationPlan::Failure::Representation)Revoke("native-shim-changed");
    if(f==NativeValidationPlan::Failure::Dynamic)Revoke("dynamic-target-changed");
    return false; // IAT mismatch still enters the existing one-way lazy proof.
}
inline bool NativePlanReadable(DWORD protection)noexcept {
    if(protection&(PAGE_GUARD|PAGE_NOACCESS))return false;
    switch(protection&0xff){
    case PAGE_READONLY:case PAGE_READWRITE:case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:case PAGE_EXECUTE_READWRITE:case PAGE_EXECUTE_WRITECOPY:return true;
    default:return false;
    }
}
// Self-process snapshot: the same byte obligations as RPM, without a kernel
// round trip. The caller proves a fresh single readable region and a bounded
// extent. A concurrent unmap/protection/in-page fault fails closed. __movsb is
// intrinsic: no external call can intervene between the live read and compare.
__declspec(noinline) inline bool NativePlanSnapshot(const void* address,void* out,size_t size)noexcept {
    __try {
        auto* target=static_cast<unsigned char*>(out);auto* source=static_cast<const unsigned char*>(address);
        // REP MOVSB has disproportionate setup cost on the many short IAT/
        // wrapper spans. All SIMD loads/stores are bounded to the exact span.
        if(size>=256){__movsb(target,source,size);return true;}
        while(size>=16){_mm_storeu_si128(reinterpret_cast<__m128i*>(target),
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(source)));source+=16;target+=16;size-=16;}
        while(size--)*target++=*source++;return true;
    }
    __except((GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR)
        ?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){return false;}
}
inline bool NativePlanIntact(const NativeValidationPlan& p)noexcept {
    if(!NativePlanMatches(p,false))return false;
    // No memory snapshot exists across any of these external calls. Their
    // results are immediately compared with revision-bound sealed expectations.
    {GenshinCpuStageTiming::Scope cost(GenshinCpuStageTiming::Stage::PlanResolve);
    for(const auto& m:p.modules)if(GetModuleHandleW(m.name)!=m.expected)return NativePlanFailure(NativeValidationPlan::Failure::Representation);
    for(const auto& r:p.resolvers)if(reinterpret_cast<uintptr_t>(GetProcAddress(r.module,r.name))!=r.expected)
        return NativePlanFailure(NativeValidationPlan::Failure::Representation);
    for(const auto& o:p.owners){HMODULE module=nullptr;
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(o.address),&module) || module!=o.expected)return NativePlanFailure(NativeValidationPlan::Failure::Representation);
    }
    }
    if(!NativePlanMatches(p))return false;
    // Live metadata is checked every time as well. No cached VirtualQuery fact.
    {GenshinCpuStageTiming::Scope cost(GenshinCpuStageTiming::Stage::PlanRegions);
    for(const auto& r:p.regions){MEMORY_BASIC_INFORMATION m{};
        if(VirtualQuery(reinterpret_cast<void*>(r.address),&m,sizeof(m))!=sizeof(m))return NativePlanFailure(NativeValidationPlan::Failure::Representation);
        const auto base=reinterpret_cast<uintptr_t>(m.BaseAddress);const auto protection=m.Protect&0xff;
        if(m.State!=MEM_COMMIT || (m.Protect&(PAGE_GUARD|PAGE_NOACCESS)) || r.address<base ||
           !Span(r.address-base,r.size,m.RegionSize) ||
           (!r.code && protection!=PAGE_READONLY && protection!=PAGE_READWRITE && protection!=PAGE_WRITECOPY))return NativePlanFailure(NativeValidationPlan::Failure::Representation);
        if(r.code){
            // Exact frozen mappings only. This is not a general executable-page
            // exception and leaves every pre-existing data obligation unchanged.
            if(m.Type!=r.type || reinterpret_cast<uintptr_t>(m.AllocationBase)!=r.allocation ||
               base!=r.region || m.RegionSize!=r.regionSize || m.Protect!=r.protect ||
               (m.Type!=MEM_IMAGE && m.Type!=MEM_PRIVATE) ||
               (protection!=PAGE_READONLY && protection!=PAGE_EXECUTE_READ && protection!=PAGE_EXECUTE_READWRITE &&
                protection!=PAGE_EXECUTE_WRITECOPY))return NativePlanFailure(NativeValidationPlan::Failure::Representation);
            continue;
        }
        if(r.privateRow){if(m.Type!=MEM_PRIVATE || reinterpret_cast<uintptr_t>(m.AllocationBase)!=r.allocation ||
            base!=r.region || m.RegionSize!=r.regionSize || m.Protect!=r.protect)return NativePlanFailure(NativeValidationPlan::Failure::Representation);
        }else if(m.Type!=MEM_IMAGE || m.AllocationBase!=r.image)return NativePlanFailure(NativeValidationPlan::Failure::Representation);
    }
    }
    // The sorted immutable obligations are batched using a FRESH readable
    // region. Never bridge an unreadable hole; compare immediately after copy,
    // before the next Win32/COM/NGX call. Scratch storage is admission-local.
    // Each region is queried once per admission. Inside that region there are
    // no external calls: bounded adjacent snapshots are immediately compared
    // before the scratch bytes are overwritten. Only region metadata spans
    // these copies, never a live byte baseline. Sparse gaps are not copied.
    {GenshinCpuStageTiming::Scope cost(GenshinCpuStageTiming::Stage::PlanReads);
    std::array<unsigned char,65536> live;
    uint64_t queries=0,snapshots=0,liveBytes=0,proofBytes=0;
    for(size_t first=0;first<p.reads.size();){
        const auto& initial=p.reads[first];MEMORY_BASIC_INFORMATION m{};++queries;
        if(VirtualQuery(reinterpret_cast<void*>(initial.address),&m,sizeof(m))!=sizeof(m) ||
           m.State!=MEM_COMMIT || !NativePlanReadable(m.Protect))return NativePlanFailure(initial.failure);
        const auto region=reinterpret_cast<uintptr_t>(m.BaseAddress);
        if(initial.address<region || !Span(initial.address-region,initial.extent,m.RegionSize))return NativePlanFailure(initial.failure);
        do {const auto& check=p.reads[first];
            if(check.address<region || !Span(check.address-region,check.extent,m.RegionSize))break;
            size_t next=first;uintptr_t end=check.address+check.extent;
            if(check.batchNext<=first || check.batchNext>p.reads.size())return NativePlanFailure(check.failure);
            if(check.batchEnd>=check.address && Span(check.address-region,check.batchEnd-check.address,m.RegionSize)){
                next=check.batchNext;end=check.batchEnd;
            }else{
                // A current region split may cut a compiled batch. Preserve
                // every obligation and fresh-query the next region as before.
                while(next<check.batchNext){const auto& r=p.reads[next];
                    if(r.address<region || !Span(r.address-region,r.extent,m.RegionSize))break;
                    end=(std::max)(end,r.address+r.extent);++next;
                }
            }
            if(end-check.address>live.size() || !NativePlanSnapshot(reinterpret_cast<void*>(check.address),live.data(),end-check.address))return NativePlanFailure(check.failure);
            for(size_t i=first;i<next;++i){const auto& r=p.reads[i];
                if(!NativePlanEqual(live.data()+r.address-check.address,p.expected.data()+r.offset,r.size))return NativePlanFailure(r.failure);
                proofBytes+=r.size;
            }
            ++snapshots;liveBytes+=end-check.address;first=next;
        }while(first<p.reads.size());
    }
    if(cost.Sampled())GenshinCpuStageTiming::RecordReadWork(queries,snapshots,liveBytes,proofBytes);
    }
    return NativePlanMatches(p); // Sticky revocation and the epoch remain live.
}
inline bool QuickLocked()noexcept {
    if(!StableLocked())return false;
    auto& l=Ledger();
    if(!l.validationPlan || !NativePlanMatches(*l.validationPlan,false))RebuildNativeValidationPlan();
    const auto plan=l.validationPlan; // Keep immutable storage alive across resolver reentry.
    if(plan)return NativePlanIntact(*plan);
    return QuickLockedFull();
}
