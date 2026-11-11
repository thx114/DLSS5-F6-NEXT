// SPDX-License-Identifier: GPL-3.0-or-later
// Included in GenshinNativeQuery::detail. Supported contract is bounded signed
// PE64/v6 data + explicit default/importer aliases + independent hash selection.
// Unknown versions/flags/shape and empty selected hosts remain fail closed.
// R584 bounded v6 schema decoder. This is data parsing, never live PEB authority.
inline bool ApiContract(const char* name)noexcept {
    return name && (!_strnicmp(name,"api-",4) || !_strnicmp(name,"ext-",4));
}
inline char AsciiLower(char c)noexcept {return c>='A' && c<='Z'?char(c+('a'-'A')):c;}
inline bool BasenameToken(const char* name)noexcept {
    if(!name || !*name)return false;size_t length=0;
    for(;length<127 && name[length];++length){const auto c=name[length];
        if(c=='.'){if(!length || !name[length+1] || name[length+1]=='.')return false;}
        else if(!((c>='a' && c<='z') || (c>='A' && c<='Z') ||
                  (c>='0' && c<='9') || c=='_' || c=='-'))return false;
    }return length>0 && length<127;
}
struct ApiValue {std::array<char,128> alias{},host{};};
struct ApiEntry {std::array<char,128> name{};DWORD hashedChars=0;std::vector<ApiValue> values;};
struct ApiNamespace {
    std::vector<ApiEntry> entries;DWORD factor=0;
    // The supported profile is ASCII carried in strict counted UTF-16LE.
    // Embedded NUL, surrogates/non-ASCII, odd ranges and path tokens refuse.
    static bool Text(const unsigned char* data,size_t size,DWORD at,DWORD bytes,std::array<char,128>& out)noexcept {
        out.fill(0);if((at&1) || (bytes&1) || bytes>254 || !Span(at,bytes,size))return false;
        for(DWORD i=0;i<bytes/2;++i){const auto c=Read<WORD>(data+at+2*i);
            if(!c || c>127)return false;out[i]=AsciiLower(static_cast<char>(c));}
        return !bytes || BasenameToken(out.data());
    }
    static bool ContractKey(const char* token,std::array<char,128>& normalized,DWORD& chars)noexcept {
        if(!ApiContract(token) || !BasenameToken(token))return false;
        const auto n=std::strlen(token);size_t end=n;
        const auto* dot=std::strchr(token,'.');
        if(dot){if(_stricmp(dot,".dll"))return false;end=size_t(dot-token);}
        if(end>=normalized.size())return false;normalized.fill(0);
        for(size_t i=0;i<end;++i)normalized[i]=AsciiLower(token[i]);
        const auto* last=std::strrchr(normalized.data(),'-');
        if(!last || last==normalized.data() || !last[1])return false;
        for(const auto* p=last+1;*p;++p)if(*p<'0' || *p>'9')return false;
        chars=static_cast<DWORD>(last-normalized.data());return chars>4;
    }
    bool Parse(const unsigned char* data,size_t supplied) {
        entries.clear();factor=0;
        if(!data || supplied<28)return false;
        const auto version=Read<DWORD>(data),size=Read<DWORD>(data+4),flags=Read<DWORD>(data+8);
        const auto count=Read<DWORD>(data+12),table=Read<DWORD>(data+16),hashes=Read<DWORD>(data+20);
        const auto multiplier=Read<DWORD>(data+24);
        if(version!=6 || flags || size<28 || size>supplied || size>1024*1024 || !count || count>4096 ||
           !multiplier || !(multiplier&1) || table<28 || hashes<28 || (table&3) || (hashes&3) ||
           !Span(table,size_t(count)*24,size) || !Span(hashes,size_t(count)*8,size))return false;
        std::vector<ApiEntry> parsed;parsed.reserve(count);unsigned valueTotal=0;
        using Extent=std::pair<size_t,size_t>;
        std::vector<Extent> metadata{{0,28},{table,size_t(count)*24},{hashes,size_t(count)*8}},strings;
        for(DWORD i=0;i<count;++i){const auto* row=data+table+24*i;
            const auto entryFlags=Read<DWORD>(row),nameAt=Read<DWORD>(row+4),nameBytes=Read<DWORD>(row+8);
            const auto hashedBytes=Read<DWORD>(row+12),valuesAt=Read<DWORD>(row+16),values=Read<DWORD>(row+20);
            if((entryFlags&~DWORD(1)) || !values || values>32 || valueTotal>16384-values ||
               valuesAt<28 || (valuesAt&3) || !Span(valuesAt,size_t(values)*20,size))return false;
            valueTotal+=values;ApiEntry entry{};
            metadata.push_back({valuesAt,size_t(values)*20});strings.push_back({nameAt,nameBytes});
            if(!Text(data,size,nameAt,nameBytes,entry.name) || !nameBytes)return false;
            std::array<char,128> key{};DWORD prefix=0;
            if(!ContractKey(entry.name.data(),key,prefix) || std::strcmp(key.data(),entry.name.data()) ||
               hashedBytes!=prefix*2)return false;
            entry.hashedChars=prefix;
            for(const auto& old:parsed)if(old.hashedChars==prefix && !std::memcmp(old.name.data(),entry.name.data(),prefix))return false;
            for(DWORD j=0;j<values;++j){const auto* val=data+valuesAt+20*j;
                const auto vf=Read<DWORD>(val),aliasAt=Read<DWORD>(val+4),aliasBytes=Read<DWORD>(val+8);
                const auto hostAt=Read<DWORD>(val+12),hostBytes=Read<DWORD>(val+16);ApiValue value{};
                if(aliasBytes)strings.push_back({aliasAt,aliasBytes});if(hostBytes)strings.push_back({hostAt,hostBytes});
                if(vf || !Text(data,size,aliasAt,aliasBytes,value.alias) ||
                   !Text(data,size,hostAt,hostBytes,value.host) || (j==0?aliasBytes!=0:aliasBytes==0) ||
                   (hostBytes && ApiContract(value.host.data())))return false;
                // Empty hosts are a schema declaration of absence, not an
                // invitation to use observed bindings or another value.
                for(const auto& old:entry.values)if(!std::strcmp(old.alias.data(),value.alias.data()))return false;
                entry.values.push_back(value);
            }parsed.push_back(std::move(entry));
        }
        for(size_t i=0;i<metadata.size();++i){const auto& first=metadata[i];
            for(size_t j=0;j<i;++j){const auto& second=metadata[j];
                if(first.first<second.first+second.second && second.first<first.first+first.second)return false;}
            for(const auto& text:strings)if(text.first<first.first+first.second && first.first<text.first+text.second)return false;
        }
        std::vector<unsigned char> indices(count,0);DWORD previous=0;
        for(DWORD i=0;i<count;++i){const auto value=Read<DWORD>(data+hashes+8*i),index=Read<DWORD>(data+hashes+8*i+4);
            if(index>=count || indices[index] || (i && value<previous))return false;
            indices[index]=1;previous=value;DWORD expected=0;
            for(DWORD j=0;j<parsed[index].hashedChars;++j)expected=expected*multiplier+static_cast<unsigned char>(parsed[index].name[j]);
            if(expected!=value)return false;
        }
        entries.swap(parsed);factor=multiplier;return true;
    }
    bool Host(const char* contract,const char* importer,std::array<char,128>& host)const noexcept {
        host.fill(0);std::array<char,128> key{};DWORD prefix=0;
        if(entries.empty() || !ContractKey(contract,key,prefix) || !BasenameToken(importer))return false;
        const ApiEntry* found=nullptr;
        for(const auto& entry:entries)if(entry.hashedChars==prefix && !std::memcmp(entry.name.data(),key.data(),prefix)){
            if(found)return false;found=&entry;}
        if(!found || found->values.empty())return false;
        const ApiValue* selected=&found->values[0];
        for(size_t i=1;i<found->values.size();++i)if(!_stricmp(found->values[i].alias.data(),importer))selected=&found->values[i];
        if(!selected->host[0] || ApiContract(selected->host.data()))return false;
        host=selected->host;return true;
    }
};
inline bool Sha256(const unsigned char* bytes,size_t size,std::array<unsigned char,32>& digest)noexcept {
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    if(size>ULONG_MAX || BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return false;
    bool good=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0;
    if(good)good=BCryptHashData(hash,const_cast<PUCHAR>(bytes),static_cast<ULONG>(size),0)>=0 &&
                 BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)>=0;
    if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);return good;
}

// Selection proof is independent of the materialized vector: follow the signed
// v6 hash/index table and wire value array, then compare with the linear parsed
// selection. These bytes must first pass the complete strict Parse contract.
namespace api_schema {
inline bool WireHost(const unsigned char* bytes,size_t supplied,const char* contract,
                     const char* importer,std::array<char,128>& host)noexcept {
    host.fill(0);std::array<char,128> key{};DWORD prefix=0;
    if(!bytes || supplied<28 || Read<DWORD>(bytes)!=6 || Read<DWORD>(bytes+8) ||
       !ApiNamespace::ContractKey(contract,key,prefix) || !BasenameToken(importer))return false;
    const auto size=Read<DWORD>(bytes+4),count=Read<DWORD>(bytes+12),table=Read<DWORD>(bytes+16),
        hashes=Read<DWORD>(bytes+20),factor=Read<DWORD>(bytes+24);
    if(size<28 || size>supplied || size>1024*1024 || !count || count>4096 || !factor || !(factor&1) ||
       table<28 || hashes<28 || (table&3) || (hashes&3) ||
       !Span(table,size_t(count)*24,size) || !Span(hashes,size_t(count)*8,size))return false;
    DWORD wanted=0;for(DWORD i=0;i<prefix;++i)wanted=wanted*factor+static_cast<unsigned char>(key[i]);
    DWORD low=0,high=count;
    while(low<high){const auto mid=low+(high-low)/2;
        if(Read<DWORD>(bytes+hashes+8*mid)<wanted)low=mid+1;else high=mid;}
    DWORD row=UINT32_MAX;
    for(auto i=low;i<count && Read<DWORD>(bytes+hashes+8*i)==wanted;++i){
        const auto index=Read<DWORD>(bytes+hashes+8*i+4);if(index>=count)return false;
        const auto at=table+24*index;std::array<char,128> name{};
        if((Read<DWORD>(bytes+at)&~DWORD(1)) ||
           !ApiNamespace::Text(bytes,size,Read<DWORD>(bytes+at+4),Read<DWORD>(bytes+at+8),name))return false;
        if(Read<DWORD>(bytes+at+12)!=prefix*2 || std::memcmp(name.data(),key.data(),prefix))continue;
        if(row!=UINT32_MAX)return false;row=at;
    }
    if(row==UINT32_MAX)return false;
    const auto values=Read<DWORD>(bytes+row+16),n=Read<DWORD>(bytes+row+20);
    if(!n || n>32 || values<28 || (values&3) || !Span(values,size_t(n)*20,size))return false;
    std::array<char,128> selected{};bool aliasSelected=false;
    for(DWORD i=0;i<n;++i){const auto at=values+20*i;std::array<char,128> alias{},target{};
        const auto aliasBytes=Read<DWORD>(bytes+at+8);
        if(Read<DWORD>(bytes+at) || (i==0?aliasBytes!=0:aliasBytes==0) ||
           !ApiNamespace::Text(bytes,size,Read<DWORD>(bytes+at+4),aliasBytes,alias) ||
           !ApiNamespace::Text(bytes,size,Read<DWORD>(bytes+at+12),Read<DWORD>(bytes+at+16),target) ||
           ApiContract(target.data()))return false;
        if(i==0)selected=target;
        else if(!_stricmp(alias.data(),importer)){
            if(aliasSelected)return false;aliasSelected=true;selected=target;
        }
    }
    if(!selected[0])return false;host=selected;return true;
}
inline bool SelectionsMatch(const ApiNamespace& space,const unsigned char* bytes,size_t size)noexcept {
    if(space.entries.empty())return false;
    for(const auto& entry:space.entries){
        if(entry.values.empty())return false;
        // Derive a valid importer absent from this bounded alias set, so that
        // the default is checked as well as every explicit importer alias.
        std::array<char,128> unmatched{};bool found=false;
        for(unsigned i=0;i<=entry.values.size();++i){
            sprintf_s(unmatched.data(),unmatched.size(),"schema-proof-%u.dll",i);bool collision=false;
            for(const auto& value:entry.values)collision|=!_stricmp(value.alias.data(),unmatched.data());
            if(!collision){found=true;break;}
        }
        if(!found)return false;
        for(unsigned i=0;i<entry.values.size();++i){const auto& value=entry.values[i];
            const auto* importer=i?value.alias.data():unmatched.data();std::array<char,128> parsed{},wire{};
            const bool expected=value.host[0]!=0;
            if(space.Host(entry.name.data(),importer,parsed)!=expected ||
               WireHost(bytes,size,entry.name.data(),importer,wire)!=expected ||
               (expected && (parsed!=wire || parsed!=value.host)))return false;
        }
    }
    return true;
}
}

struct SchemaReceipt {
    FILE_ID_INFO identity{};FILE_BASIC_INFO basic{};ULONGLONG bytes=0;
    std::array<unsigned char,32> fileSHA256{},namespaceSHA256{};
    DWORD namespaceSize=0,count=0;wchar_t path[MAX_PATH]{};
};
struct CanonicalApiSchema {
private:
    // Snapshot only signed file data, never a live process namespace. It is
    // private and immutable after the one successful publication of good.
    std::vector<unsigned char> namespace_;
    ApiNamespace space;
public:
    Handle file,mapping;const unsigned char* raw=nullptr;bool attempted=false,good=false;
    SchemaReceipt receipt{};
    size_t EntryCount()const noexcept {return space.entries.size();}
    ~CanonicalApiSchema(){if(raw)UnmapViewOfFile(raw);}
    bool Identity(SchemaReceipt& value)const noexcept {
        LARGE_INTEGER size{};
        if(!GetFileInformationByHandleEx(file.value,FileIdInfo,&value.identity,sizeof(value.identity)) ||
           !GetFileInformationByHandleEx(file.value,FileBasicInfo,&value.basic,sizeof(value.basic)) ||
           !GetFileSizeEx(file.value,&size) || size.QuadPart<4096 || size.QuadPart>4*1024*1024)return false;
        value.bytes=static_cast<ULONGLONG>(size.QuadPart);return true;
    }
    bool Open() {
        if(attempted)return good;attempted=true;
        wchar_t system[MAX_PATH]{},path[MAX_PATH]{},finalPath[MAX_PATH+4]{};
        const auto sys=GetSystemDirectoryW(system,MAX_PATH);
        if(!sys || sys>=MAX_PATH || swprintf_s(path,L"%ls\\apisetschema.dll",system)<0)return false;
        file.value=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(file.value==INVALID_HANDLE_VALUE)return false;
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if(!GetFileInformationByHandleEx(file.value,FileAttributeTagInfo,&attributes,sizeof(attributes)) ||
           (attributes.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))return false;
        const auto count=GetFinalPathNameByHandleW(file.value,finalPath,MAX_PATH+4,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
        if(count<5 || count>=MAX_PATH+4 || std::wcsncmp(finalPath,L"\\\\?\\",4) || _wcsicmp(finalPath+4,path) || !Identity(receipt))return false;
        mapping.value=CreateFileMappingW(file.value,nullptr,PAGE_READONLY,0,0,nullptr);if(!mapping.value)return false;
        raw=static_cast<const unsigned char*>(MapViewOfFile(mapping.value,FILE_MAP_READ,0,0,0));if(!raw)return false;
        std::array<unsigned char,32> before{};
        if(!Sha256(raw,static_cast<size_t>(receipt.bytes),before) || !TrustedFile(file.value,path))return false;
        if(Read<WORD>(raw)!=IMAGE_DOS_SIGNATURE)return false;
        const auto nt=Read<DWORD>(raw+offsetof(IMAGE_DOS_HEADER,e_lfanew));
        if(nt<sizeof(IMAGE_DOS_HEADER) || !Span(nt,sizeof(IMAGE_NT_HEADERS64),static_cast<size_t>(receipt.bytes)))return false;
        const auto header=Read<IMAGE_NT_HEADERS64>(raw+nt);const auto& opt=header.OptionalHeader;
        if(header.Signature!=IMAGE_NT_SIGNATURE || header.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 ||
           header.FileHeader.SizeOfOptionalHeader!=sizeof(IMAGE_OPTIONAL_HEADER64) || opt.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
           opt.NumberOfRvaAndSizes!=IMAGE_NUMBEROF_DIRECTORY_ENTRIES || !opt.SizeOfImage || opt.SizeOfImage>4*1024*1024 ||
           !opt.SectionAlignment || (opt.SectionAlignment&(opt.SectionAlignment-1)) ||
           !opt.FileAlignment || (opt.FileAlignment&(opt.FileAlignment-1)) ||
           !header.FileHeader.NumberOfSections || header.FileHeader.NumberOfSections>32 ||
           opt.SizeOfHeaders>opt.SizeOfImage || !Span(0,opt.SizeOfHeaders,static_cast<size_t>(receipt.bytes)))return false;
        const size_t table=nt+sizeof(header);const unsigned char* namespaceBytes=nullptr;DWORD namespaceLimit=0;
        if(!Span(table,size_t(header.FileHeader.NumberOfSections)*sizeof(IMAGE_SECTION_HEADER),opt.SizeOfHeaders))return false;
        for(unsigned i=0;i<header.FileHeader.NumberOfSections;++i){
            const auto section=Read<IMAGE_SECTION_HEADER>(raw+table+i*sizeof(IMAGE_SECTION_HEADER));
            const auto extent=(std::max)(section.Misc.VirtualSize,section.SizeOfRawData);
            if(!extent || section.VirtualAddress<opt.SizeOfHeaders || !Span(section.VirtualAddress,extent,opt.SizeOfImage) ||
               !Span(section.PointerToRawData,section.SizeOfRawData,static_cast<size_t>(receipt.bytes)) ||
               (section.SizeOfRawData && section.PointerToRawData<opt.SizeOfHeaders))return false;
            for(unsigned j=0;j<i;++j){const auto old=Read<IMAGE_SECTION_HEADER>(raw+table+j*sizeof(IMAGE_SECTION_HEADER));
                const auto oldExtent=(std::max)(old.Misc.VirtualSize,old.SizeOfRawData);
                if(section.VirtualAddress<uint64_t(old.VirtualAddress)+oldExtent && old.VirtualAddress<uint64_t(section.VirtualAddress)+extent)return false;
                if(section.SizeOfRawData && old.SizeOfRawData && section.PointerToRawData<uint64_t(old.PointerToRawData)+old.SizeOfRawData &&
                   old.PointerToRawData<uint64_t(section.PointerToRawData)+section.SizeOfRawData)return false;
            }
            if(!std::memcmp(section.Name,".apiset\0",8)){
                if(namespaceBytes || !section.Misc.VirtualSize || !section.SizeOfRawData ||
                   (section.Characteristics&(IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_DISCARDABLE)) ||
                   !(section.Characteristics&IMAGE_SCN_MEM_READ))return false;
                namespaceBytes=raw+section.PointerToRawData;
                namespaceLimit=(std::min)(section.Misc.VirtualSize,section.SizeOfRawData);
            }
        }
        if(!namespaceBytes || namespaceLimit<28)return false;
        const auto namespaceSize=Read<DWORD>(namespaceBytes+4);
        if(namespaceSize<28 || namespaceSize>namespaceLimit || namespaceSize>1024*1024)return false;
        namespace_.assign(namespaceBytes,namespaceBytes+namespaceSize);
        if(!space.Parse(namespace_.data(),namespace_.size()) ||
           !api_schema::SelectionsMatch(space,namespace_.data(),namespace_.size()))return false;
        SchemaReceipt after{};
        if(!Identity(after) || after.bytes!=receipt.bytes ||
           std::memcmp(&after.identity,&receipt.identity,sizeof(receipt.identity)) ||
           after.basic.LastWriteTime.QuadPart!=receipt.basic.LastWriteTime.QuadPart ||
           after.basic.ChangeTime.QuadPart!=receipt.basic.ChangeTime.QuadPart ||
           !Sha256(raw,static_cast<size_t>(receipt.bytes),receipt.fileSHA256) || before!=receipt.fileSHA256)return false;
        receipt.namespaceSize=Read<DWORD>(namespaceBytes+4);receipt.count=Read<DWORD>(namespaceBytes+12);
        // SHA values remain provenance and continuity receipts, never an OS
        // revision whitelist. Strict v6/PE, both independent selectors, trusted
        // same-handle source and the immutable snapshot establish support.
        if(std::memcmp(namespaceBytes,namespace_.data(),namespace_.size()) ||
           !Sha256(namespace_.data(),namespace_.size(),receipt.namespaceSHA256))return false;
        std::memcpy(receipt.path,path,sizeof(path));good=true;return true;
    }
    bool Host(const char* contract,const char* importer,std::array<char,128>& host)const noexcept {
        host.fill(0);std::array<char,128> parsed{},wire{};
        if(!good || !space.Host(contract,importer,parsed) ||
           !api_schema::WireHost(namespace_.data(),namespace_.size(),contract,importer,wire) || parsed!=wire)return false;
        host=parsed;return true;
    }
};
inline CanonicalApiSchema apiSchema;
