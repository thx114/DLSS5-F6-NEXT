// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Included inside GenshinNativeQuery after its authenticated ledger machinery.
// Context-role table shape only; no factory/birth/resource/camera authority.
namespace context_recipe {
inline constexpr uint32_t TableRva=0x1ccd40,Stride=0x4b8;
inline constexpr unsigned Count=151;
// One actual read for the two existing table alternatives. This stack object
// never escapes the guarded traversal or becomes a persistent authority token.
struct TableSnapshot {std::array<uintptr_t,Count> entries{};bool read=false;};
inline constexpr std::array<unsigned char,0x131> CopyRecipe={0x48,0x8b,0xc4,0x48,0x89,0x58,0x10,0x48,0x89,0x70,0x18,0x48,0x89,0x78,0x20,0x48,0x89,0x48,0x08,0x55,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0x68,0xa1,0x48,0x81,0xec,0xb0,0x00,0x00,0x00,0x48,0x8b,0xda,0x48,0x8b,0xf9,0x48,0x8b,0x42,0x10,0x45,0x33,0xed,0x44,0x89,0xa9,0x98,0x00,0x00,0x00,0x48,0x89,0x81,0xa0,0x00,0x00,0x00,0xe8,0x41,0x08,0x00,0x00,0x48,0x8d,0x4f,0x20,0xe8,0x3c,0x0d,0x02,0x00,0x4c,0x89,0xaf,0xa8,0x00,0x00,0x00,0x83,0x8f,0xb4,0x00,0x00,0x00,0xff,0x4c,0x89,0xaf,0xb8,0x00,0x00,0x00,0x48,0x8d,0x87,0xc0,0x00,0x00,0x00,0x48,0x89,0x00,0x48,0x89,0x40,0x08,0x45,0x8d,0x75,0x01,0x44,0x39,0x6b,0x28,0x75,0x10,0x48,0x8b,0x43,0x10,0x44,0x38,0xa8,0x44,0x05,0x00,0x00,0x41,0x8a,0xc6,0x75,0x03,0x41,0x8a,0xc5,0x88,0x87,0x00,0x01,0x00,0x00,0x48,0x8b,0xcb,0xe8,0x62,0x07,0x00,0x00,0x44,0x0f,0xb6,0xc0,0x49,0x69,0xc0,0xb8,0x04,0x00,0x00,0x48,0x8d,0x0d,0x20,0xb5,0x1b,0x00,0x48,0x03,0xc1,0x48,0x8d,0x8f,0x10,0x01,0x00,0x00,0x48,0x89,0x8f,0x08,0x01,0x00,0x00,0xba,0x09,0x00,0x00,0x00,0x44,0x8d,0x42,0x77,0x0f,0x10,0x00,0x0f,0x11,0x01,0x0f,0x10,0x48,0x10,0x0f,0x11,0x49,0x10,0x0f,0x10,0x40,0x20,0x0f,0x11,0x41,0x20,0x0f,0x10,0x48,0x30,0x0f,0x11,0x49,0x30,0x0f,0x10,0x40,0x40,0x0f,0x11,0x41,0x40,0x0f,0x10,0x48,0x50,0x0f,0x11,0x49,0x50,0x0f,0x10,0x40,0x60,0x0f,0x11,0x41,0x60,0x49,0x03,0xc8,0x0f,0x10,0x48,0x70,0x0f,0x11,0x49,0xf0,0x49,0x03,0xc0,0x49,0x2b,0xd6,0x75,0xb7,0x0f,0x10,0x00,0x0f,0x11,0x01,0x0f,0x10,0x48,0x10,0x0f,0x11,0x49,0x10,0x0f,0x10,0x40,0x20,0x0f,0x11,0x41,0x20,0x48,0x8b,0x40,0x30,0x48,0x89,0x41,0x30};
inline constexpr std::array<unsigned char,0x1c> PatchRecipe={0x48,0x8d,0x05,0xa8,0xb5,0x0e,0x00,0x48,0x89,0x87,0x88,0x04,0x00,0x00,0x48,0x8d,0x05,0x3a,0xfb,0x19,0x00,0x48,0x89,0x87,0x90,0x05,0x00,0x00};
// R1024: finite constructor profile for the locally observed d3d11 10.0.26100.9549.
// This constructor puts the context at base+d8 and its 151-entry inline table
// at base+e0. The two stores at base+458/base+560 therefore override slots
// 111/144. Runtime authority still requires the authenticated factory image,
// exact clean AND live constructor/store bytes, a complete immutable source
// table and all 151 actual entries. No runtime observation becomes a recipe.
inline constexpr std::array<unsigned char,0x112> CopyRecipe26100_9549={
    0x48,0x89,0x4c,0x24,0x08,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,
    0x57,0x48,0x8d,0x6c,0x24,0xe1,0x48,0x81,0xec,0xb8,0x00,0x00,0x00,0x48,0x8b,0xfa,
    0x48,0x8b,0xd9,0x48,0x8b,0x42,0x10,0x45,0x33,0xff,0x44,0x89,0x79,0x68,0x48,0x89,
    0x41,0x70,0xe8,0x25,0x0f,0x00,0x00,0x48,0x8d,0x4b,0x20,0xe8,0xa4,0xbc,0x01,0x00,
    0x4c,0x89,0x7b,0x78,0x83,0x8b,0x84,0x00,0x00,0x00,0xff,0x4c,0x89,0xbb,0x88,0x00,
    0x00,0x00,0x48,0x8d,0x83,0x90,0x00,0x00,0x00,0x48,0x89,0x00,0x48,0x89,0x40,0x08,
    0x45,0x8d,0x6f,0x01,0x44,0x39,0x7f,0x28,0x0f,0x84,0x03,0x06,0x00,0x00,0x41,0x8a,
    0xc7,0x88,0x83,0xd0,0x00,0x00,0x00,0x48,0x8b,0xcf,0xe8,0xc5,0x0c,0x00,0x00,0x44,
    0x0f,0xb6,0xc0,0x49,0x69,0xc0,0xb8,0x04,0x00,0x00,0x48,0x8d,0x0d,0x43,0xa3,0x1c,
    0x00,0x48,0x03,0xc1,0x48,0x8d,0x8b,0xe0,0x00,0x00,0x00,0x48,0x89,0x8b,0xd8,0x00,
    0x00,0x00,0xba,0x09,0x00,0x00,0x00,0x44,0x8d,0x62,0x77,0x0f,0x10,0x00,0x0f,0x11,
    0x01,0x0f,0x10,0x48,0x10,0x0f,0x11,0x49,0x10,0x0f,0x10,0x40,0x20,0x0f,0x11,0x41,
    0x20,0x0f,0x10,0x48,0x30,0x0f,0x11,0x49,0x30,0x0f,0x10,0x40,0x40,0x0f,0x11,0x41,
    0x40,0x0f,0x10,0x48,0x50,0x0f,0x11,0x49,0x50,0x0f,0x10,0x40,0x60,0x0f,0x11,0x41,
    0x60,0x49,0x03,0xcc,0x0f,0x10,0x48,0x70,0x0f,0x11,0x49,0xf0,0x49,0x03,0xc4,0x49,
    0x2b,0xd5,0x75,0xb7,0x0f,0x10,0x00,0x0f,0x11,0x01,0x0f,0x10,0x48,0x10,0x0f,0x11,
    0x49,0x10,0x0f,0x10,0x40,0x20,0x0f,0x11,0x41,0x20,0x48,0x8b,0x40,0x30,0x48,0x89,
    0x41,0x30,
};
inline constexpr std::array<unsigned char,0x1c> PatchRecipe26100_9549={
    0x48,0x8d,0x05,0xe6,0xa9,0x16,0x00,0x48,0x89,0x83,0x58,0x04,0x00,0x00,0x48,0x8d,
    0x05,0x68,0x9b,0x1b,0x00,0x48,0x89,0x83,0x60,0x05,0x00,0x00,
};
struct InlineProfile {
    uint32_t tableRva,copyRva;
    const unsigned char* copy;
    size_t copySize;
    uint32_t patchRva;
    const unsigned char* patch;
    size_t patchSize;
    std::array<uint32_t,2> overrides;
};
inline constexpr std::array<InlineProfile,2> InlineProfiles={
    InlineProfile{TableRva,0x11770,CopyRecipe.data(),CopyRecipe.size(),
        0x11c31,PatchRecipe.data(),PatchRecipe.size(),{0xfd1e0,0x1b1780}},
    InlineProfile{0x1d3040,0x8c6c,CopyRecipe26100_9549.data(),CopyRecipe26100_9549.size(),
        0x90f3,PatchRecipe26100_9549.data(),PatchRecipe26100_9549.size(),{0x173ae0,0x1c2c70}},
};
inline const InlineProfile* SelectInlineProfile(const detail::Image& image)noexcept {
    const InlineProfile* selected=nullptr;
    for(const auto& profile:InlineProfiles){
        if(!image.SectionHas(profile.copyRva,profile.copySize,IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE,
                IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_DISCARDABLE) ||
           !image.SectionHas(profile.patchRva,profile.patchSize,IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE,
                IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_DISCARDABLE) ||
           std::memcmp(image.clean.data()+profile.copyRva,profile.copy,profile.copySize) ||
           std::memcmp(image.clean.data()+profile.patchRva,profile.patch,profile.patchSize))continue;
        if(selected)return nullptr; // Ambiguous profiles never mix source tables or overrides.
        selected=&profile;
    }
    return selected;
}
inline bool RecipeRange(const detail::Image& image,uint32_t rva,const unsigned char* bytes,size_t size)noexcept {
    return image.SectionHas(rva,size,IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE,IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_DISCARDABLE) &&
        !std::memcmp(image.clean.data()+rva,bytes,size) &&
        detail::Equal(reinterpret_cast<const unsigned char*>(image.live)+rva,bytes,size);
}
inline bool PrivateInline(const void* object,const void* table)noexcept {
    const auto start=reinterpret_cast<uintptr_t>(object);constexpr size_t bytes=(Count+1)*sizeof(uintptr_t);
    if(!start || start>UINTPTR_MAX-bytes || reinterpret_cast<uintptr_t>(table)!=start+sizeof(uintptr_t))return false;
    const auto end=start+bytes;auto at=start;void* allocation=nullptr;
    while(at<end){
        MEMORY_BASIC_INFORMATION info{};
        if(VirtualQuery(reinterpret_cast<const void*>(at),&info,sizeof(info))!=sizeof(info) ||
           info.State!=MEM_COMMIT || info.Type!=MEM_PRIVATE ||
           (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)) || (info.Protect&0xff)!=PAGE_READWRITE ||
           (allocation && allocation!=info.AllocationBase))return false;
        allocation=info.AllocationBase;const auto base=reinterpret_cast<uintptr_t>(info.BaseAddress);
        if(base>at || info.RegionSize>UINTPTR_MAX-base || base+info.RegionSize<=at)return false;
        at=(std::min)(end,base+info.RegionSize);
    }
    return true;
}
inline bool InlineTableLocked(const void* object,const void* table,unsigned requestedSlots)noexcept {
    // No pointer or observed runtime method can become a recipe. Both complete
    // signed read-only sources, exact constructor bytes, two fixed stores and
    // every one of151 actual entries must agree. Unsupported native builds fail.
    if(!requestedSlots || requestedSlots>150 || !PrivateInline(object,table) || !detail::Ledger().imageCount)return false;
    const auto& image=detail::Ledger().images[0];
    if(!image.live || image.live!=detail::Ledger().factoryModule)return false;
    const auto* profile=SelectInlineProfile(image);
    if(!profile || !RecipeRange(image,profile->copyRva,profile->copy,profile->copySize) ||
       !RecipeRange(image,profile->patchRva,profile->patch,profile->patchSize))return false;
    std::array<uintptr_t,Count> actual{};
    if(!detail::Memory(table,actual.data(),sizeof(actual)))return false;
    for(unsigned mode=0;mode<2;++mode){
        const auto rva=profile->tableRva+mode*Stride;
        if(!image.SectionHas(rva,sizeof(actual),IMAGE_SCN_MEM_READ,IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_DISCARDABLE) ||
           !detail::Equal(reinterpret_cast<const unsigned char*>(image.live)+rva,image.clean.data()+rva,sizeof(actual)))return false;
        bool same=true;
        for(unsigned slot=0;slot<Count;++slot){
            uintptr_t expected=detail::Read<uintptr_t>(image.clean.data()+rva+slot*sizeof(uintptr_t));
            // The selected constructor profile proves both fixed overrides;
            // never combine one profile's source table with another's targets.
            if(slot==111)expected=reinterpret_cast<uintptr_t>(image.live)+profile->overrides[0];
            if(slot==144)expected=reinterpret_cast<uintptr_t>(image.live)+profile->overrides[1];
            if(!detail::Executable(reinterpret_cast<const void*>(expected)))return false;
            if(actual[slot]!=expected)same=false;
        }
        if(same)return true;
    }
    return false;
}
}
// R193: startup-only observation overloads retain the same predicates and
// selected finite constructor profile as the observer-free implementations.
namespace context_recipe {
template<class Observation> inline bool RecipeRange(const detail::Image& image,uint32_t rva,
    const unsigned char* bytes,size_t size,Observation& o)noexcept {
    o.At("RecipeRange",rva);
    if(!o.Leaf("recipe-section",image.SectionHas(rva,size,IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE,
        IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_DISCARDABLE)))return false;
    if(!o.Leaf("recipe-clean-bytes",std::memcmp(image.clean.data()+rva,bytes,size)==0))return false;
    return detail::Equal(reinterpret_cast<const unsigned char*>(image.live)+rva,bytes,size,o);
}
template<class Observation> inline bool PrivateInline(const void* object,const void* table,Observation& o)noexcept {
    o.At("PrivateInline");
    const auto start=reinterpret_cast<uintptr_t>(object);constexpr size_t bytes=(Count+1)*sizeof(uintptr_t);
    if(!o.Leaf("inline-object-nonnull",start!=0))return false;
    if(!o.Leaf("inline-object-address-range",start<=UINTPTR_MAX-bytes))return false;
    if(!o.Leaf("inline-table-at-object-plus-pointer",reinterpret_cast<uintptr_t>(table)==start+sizeof(uintptr_t)))return false;
    const auto end=start+bytes;auto at=start;void* allocation=nullptr;
    while(at<end){
        MEMORY_BASIC_INFORMATION info{};o.offset=at-start;o.BeforeLeaf("inline-VirtualQuery");
        const auto queried=VirtualQuery(reinterpret_cast<const void*>(at),&info,sizeof(info));
        o.apiValue=queried;o.apiValid=true;
        if(queried!=sizeof(info)){
            if(!queried){o.win32=GetLastError();o.win32Valid=true;}
            o.Leaf("inline-VirtualQuery-size",queried==sizeof(info));return false;
        }
        if(!o.Leaf("inline-memory-committed",info.State==MEM_COMMIT))return false;
        if(!o.Leaf("inline-memory-private",info.Type==MEM_PRIVATE))return false;
        if(!o.Leaf("inline-memory-no-guard-noaccess",!(info.Protect&(PAGE_GUARD|PAGE_NOACCESS))))return false;
        if(!o.Leaf("inline-memory-readwrite",(info.Protect&0xff)==PAGE_READWRITE))return false;
        if(!o.Leaf("inline-memory-one-allocation",!allocation || allocation==info.AllocationBase))return false;
        allocation=info.AllocationBase;const auto base=reinterpret_cast<uintptr_t>(info.BaseAddress);
        if(!o.Leaf("inline-region-base",base<=at))return false;
        if(!o.Leaf("inline-region-address-range",info.RegionSize<=UINTPTR_MAX-base))return false;
        if(!o.Leaf("inline-region-progress",base+info.RegionSize>at))return false;
        at=(std::min)(end,base+info.RegionSize);
    }
    return true;
}
template<class Observation> inline bool InlineTableLocked(const void* object,const void* table,
    unsigned requestedSlots,Observation& o,TableSnapshot* snapshot=nullptr)noexcept {
    o.At("InlineTableLocked");
    if(!o.Leaf("inline-requested-slots-nonzero",requestedSlots!=0))return false;
    if(!o.Leaf("inline-requested-slots-supported",requestedSlots<=150))return false;
    if(!PrivateInline(object,table,o))return false;
    o.At("InlineTableLocked");
    if(!o.Leaf("inline-image-count-nonzero",detail::Ledger().imageCount!=0))return false;
    const auto& image=detail::Ledger().images[0];
    if(!o.Leaf("inline-image-live",image.live!=nullptr))return false;
    if(!o.Leaf("inline-factory-image-identity",image.live==detail::Ledger().factoryModule))return false;
    const auto* profile=SelectInlineProfile(image);
    if(!o.Leaf("inline-constructor-profile-supported",profile!=nullptr))return false;
    if(!RecipeRange(image,profile->copyRva,profile->copy,profile->copySize,o))return false;
    if(!RecipeRange(image,profile->patchRva,profile->patch,profile->patchSize,o))return false;
    o.At("InlineTableLocked");
    std::array<uintptr_t,Count> local{};auto& actual=snapshot?snapshot->entries:local;
    if((!snapshot || !snapshot->read) && !detail::Memory(table,actual.data(),sizeof(actual),o))return false;
    if(snapshot)snapshot->read=true;
    bool same=false;
    for(unsigned mode=0;mode<2;++mode){
        const auto rva=profile->tableRva+mode*Stride;o.rva=rva;o.mode=static_cast<int>(mode);o.slot=-1;
        o.BeforeLeaf("inline-source-table-section");
        if(!o.Leaf("inline-source-table-section",image.SectionHas(rva,sizeof(actual),IMAGE_SCN_MEM_READ,
            IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_DISCARDABLE)))return false;
        if(!detail::Equal(reinterpret_cast<const unsigned char*>(image.live)+rva,image.clean.data()+rva,sizeof(actual),o))return false;
        same=true;
        for(unsigned slot=0;slot<Count;++slot){
            o.slot=static_cast<int>(slot);
            uintptr_t expected=detail::Read<uintptr_t>(image.clean.data()+rva+slot*sizeof(uintptr_t));
            // The same selected profile and two overrides as the observer-free path.
            if(slot==111)expected=reinterpret_cast<uintptr_t>(image.live)+profile->overrides[0];
            if(slot==144)expected=reinterpret_cast<uintptr_t>(image.live)+profile->overrides[1];
            o.BeforeLeaf("inline-target-Executable");
            if(!o.Leaf("inline-target-Executable",detail::Executable(reinterpret_cast<const void*>(expected))))return false;
            if(actual[slot]!=expected){same=false;
                if(o.modeMismatch[mode]<0){o.modeMismatch[mode]=static_cast<int>(slot);
                    if constexpr(requires {o.EntryMismatch(mode,actual[slot],expected);})
                        o.EntryMismatch(mode,actual[slot],expected);
                }
            }
        }
        if(same)return true;
        // An ordinary first-mode mismatch is NOT a failure and does not stop
        // the second mode. Capture both existing comparisons in this one pass.
    }
    o.slot=-1;o.BeforeLeaf("inline-any-mode-match");
    return o.Leaf("inline-any-mode-match",same);
}
} // namespace context_recipe
