// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone production-header tests. No arguments runs both suites; --cpu uses
// fake native COM interfaces; --warp uses a real D3D11 WARP device.
// No hooks, SDK, game or deployment. Link d3d11.lib (no project/stub includes).
#include "../../GenshinDepthRetention.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <thread>
#include <type_traits>
#include <vector>

namespace AllocationTest {
std::atomic<bool> failNext{false};
std::atomic<size_t> calls{0};
}
void* operator new(size_t size) {
    ++AllocationTest::calls;
    if(AllocationTest::failNext.exchange(false))throw std::bad_alloc{};
    if(auto* result=std::malloc(size?size:1))return result;
    throw std::bad_alloc{};
}
void operator delete(void* pointer)noexcept {std::free(pointer);}
void operator delete(void* pointer,size_t)noexcept {std::free(pointer);}
void* operator new[](size_t size) {return ::operator new(size);}
void operator delete[](void* pointer)noexcept {std::free(pointer);}
void operator delete[](void* pointer,size_t)noexcept {std::free(pointer);}

namespace Retention=GenshinDepthRetention;
using Microsoft::WRL::ComPtr;
namespace {
unsigned checks=0,groups=0;
void Check(bool ok,const char* why) {
    ++checks;
    if(!ok){std::fprintf(stderr,"FAILED: %s\n",why);std::abort();}
}
void Clean() {
    const auto t=Retention::ReadTelemetry();
    Check(!t.bytes && !t.resources && !t.payloadBytes && !t.payloads,"all registry charges retired");
    Check(!Retention::ReadSourceTelemetry().allocations,"all source charges retired");
}
template<class F> void Group(const char* name,F&& test) {
    Clean();const auto before=checks;test();Clean();++groups;
    std::printf("PASS %s (%u checks)\n",name,checks-before);
}

struct Calls {
    unsigned desc=0,query=0,add=0,release=0;
    bool operator==(const Calls&)const=default;
};
// Two distinct ID3D11Buffer interfaces share one separate canonical IUnknown.
// Each face also tracks its own references to catch an unretained tear-off.
// Storage stays on the CPU stack; zero refs marks logical COM destruction.
struct Fake {
    struct Face final : ID3D11Buffer {
        Fake* owner;UINT width;ULONG refs=0;
        Face(Fake* object,UINT bytes):owner(object),width(bytes){}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override {return owner->Query(this,iid,out);}
        ULONG STDMETHODCALLTYPE AddRef()override {++owner->calls.add;++owner->refs;return ++refs;}
        ULONG STDMETHODCALLTYPE Release()override {++owner->calls.release;Check(refs!=0,"face release has matching retain");--refs;return owner->Release();}
        void STDMETHODCALLTYPE GetDevice(ID3D11Device** out)override {*out=nullptr;}
        HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,UINT*,void*)override {return E_NOTIMPL;}
        HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,UINT,const void*)override {return E_NOTIMPL;}
        HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,const IUnknown*)override {return E_NOTIMPL;}
        void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* out)override {*out=D3D11_RESOURCE_DIMENSION_BUFFER;}
        void STDMETHODCALLTYPE SetEvictionPriority(UINT)override {}
        UINT STDMETHODCALLTYPE GetEvictionPriority()override {return 0;}
        void STDMETHODCALLTYPE GetDesc(D3D11_BUFFER_DESC* out)override {
            Check(!owner->dead,"GetDesc never uses a destroyed fake");++owner->calls.desc;
            *out={};out->ByteWidth=width;out->Usage=D3D11_USAGE_DEFAULT;out->BindFlags=D3D11_BIND_VERTEX_BUFFER;
        }
    };
    struct Identity final : IUnknown {
        Fake* owner;ULONG refs=0;
        explicit Identity(Fake* object):owner(object){}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override {return owner->Query(&owner->first,iid,out);}
        ULONG STDMETHODCALLTYPE AddRef()override {++owner->calls.add;++owner->refs;return ++refs;}
        ULONG STDMETHODCALLTYPE Release()override {++owner->calls.release;Check(refs!=0,"identity release has matching retain");--refs;return owner->Release();}
    };
    ULONG refs=1; // external owner of the fake, independent of the two faces
    Calls calls;
    Face first,alias;
    Identity identity{this};
    bool failQuery=false,nullIdentity=false,dead=false,inspectFinal=false;
    bool finalCharged=false,finalRegistryFree=false,finalFacesReleased=false;
    unsigned finalCalls=0;
    explicit Fake(UINT bytes=16):first(this,bytes),alias(this,bytes){}
    ~Fake() {
        Check((refs==1 || refs==0) && !first.refs && !alias.refs && !identity.refs,"fake has no leaked COM interfaces");
    }
    HRESULT Query(Face* face,REFIID iid,void** out) {
        ++calls.query;if(!out)return E_POINTER;*out=nullptr;
        if(iid==__uuidof(IUnknown)) {
            if(failQuery)return E_NOINTERFACE;
            if(nullIdentity)return S_OK;
            *out=&identity;identity.AddRef();return S_OK;
        }
        if(iid==__uuidof(ID3D11Buffer) || iid==__uuidof(ID3D11Resource) || iid==__uuidof(ID3D11DeviceChild)) {
            *out=static_cast<ID3D11Buffer*>(face);face->AddRef();return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG Release() {
        Check(refs!=0,"fake cannot release below zero");
        if(--refs)return refs;
        dead=true;++finalCalls;
        if(inspectFinal) {
            const auto t=Retention::ReadTelemetry();
            finalCharged=t.bytes>=first.width && t.resources!=0;
            finalFacesReleased=!first.refs && !alias.refs;
            // A different thread avoids try_lock on a mutex owned by this one.
            std::thread observer([&]{auto* registry=Retention::detail::Store();
                finalRegistryFree=registry->mutex.try_lock();if(finalRegistryFree)registry->mutex.unlock();});
            observer.join();
        }
        return 0;
    }
};

void ExactFastPath() {
    Fake object;
    Retention::Buffers<1> set;
    Check(set.Add(nullptr) && !set.count,"null buffer is an uncharged success");
    Check(set.Add(&object.first) && set.count==1,"first exact interface admitted");
    Check(set.tokens[0]->object.Get()==static_cast<IUnknown*>(&object.identity),"registry retains canonical IUnknown");
    Check(set.tokens[0]->buffer.Get()==&object.first && object.first.refs==1,"lease owns its exact buffer interface");
    const auto before=object.calls;
    const auto allocations=AllocationTest::calls.load();
    bool accepted=true;
    for(unsigned i=0;i<100000;++i)accepted=set.Add(&object.first)&&accepted;
    Check(accepted && set.count==1,"100000 repeats preserve set capacity");
    Check(object.calls==before,"100000 repeats make zero GetDesc/QI/AddRef/Release calls");
    Check(allocations==AllocationTest::calls.load(),"100000 repeats allocate no metadata");
    const auto t=Retention::ReadTelemetry();
    Check(t.bytes==16 && t.resources==1,"one canonical resource charge");
    std::printf("  exact repeats=100000, additional desc/query/add/release/allocations=0\n");
}

void CanonicalAliases() {
    Fake object,other;
    Retention::Buffers<1> set;
    Check(&object.first!=&object.alias,"fake exposes different physical buffer interfaces");
    Check(set.Add(&object.first),"admit first interface");
    const auto before=object.calls;
    Check(set.Add(&object.alias) && set.count==1,"full set accepts canonical interface alias");
    Check(object.calls.desc==before.desc+1 && object.calls.query==before.query+1,"unknown alias follows canonical lookup");
    Check(!object.alias.refs && object.first.refs==1,"unknown alias is not recorded as a raw-address witness");
    Retention::Buffers<1> aliasSet;
    Check(aliasSet.Add(&object.alias) && aliasSet.tokens[0]==set.tokens[0],"sets share a canonical lease regardless of interface");
    Check(aliasSet.Add(&object.first),"shared lease carries its retained exact-interface witness into another set");
    Check(Retention::ReadTelemetry().resources==1,"interface aliases never double charge");
    const auto released=Retention::ReadTelemetry().releases;
    const auto refused=Retention::ReadTelemetry().refused;
    Check(!set.Add(&other.first),"same descriptor with a different canonical identity fails full N capacity");
    Check(set.count==1 && Retention::ReadTelemetry().resources==1,"full-set failure leaves previous tokens intact");
    Check(Retention::ReadTelemetry().refused==refused+1 && Retention::ReadTelemetry().releases==released+1,
        "full-set miss retains original admit-then-release refusal semantics");
    other.alias.width=32;
    Retention::Buffers<2> two;
    Check(two.Add(&object.first) && two.Add(&other.first) && two.count==2,"different identities with equal descriptors remain distinct");
    const auto conflict=Retention::ReadRefusalTelemetry().conflict;
    Check(!two.Add(&other.alias) && Retention::ReadRefusalTelemetry().conflict==conflict+1,"canonical alias size conflict still refuses");
}

void CopiesAndMoves() {
    static_assert(std::is_nothrow_copy_constructible_v<Retention::Buffers<2>>);
    static_assert(std::is_nothrow_copy_assignable_v<Retention::Buffers<2>>);
    static_assert(std::is_nothrow_move_constructible_v<Retention::Buffers<2>>);
    static_assert(std::is_nothrow_move_assignable_v<Retention::Buffers<2>>);
    struct OriginalShape {std::array<Retention::Token,2> tokens{};size_t count=0;};
    static_assert(sizeof(OriginalShape)==sizeof(Retention::Buffers<2>));
    Fake first,second;
    Retention::Buffers<2> source;
    Check(source.Add(&first.first) && source.Add(&second.first),"populate copy/move source");
    const auto before=first.calls;
    auto copy=source;
    Retention::Buffers<2> assigned;assigned=copy;
    auto moved=std::move(copy);
    Retention::Buffers<2> moveAssigned;moveAssigned=std::move(assigned);
    source={};
    Check(moved.count==2 && moveAssigned.count==2 && !copy.tokens[0] && !assigned.tokens[0],"default move transfers token ownership");
    Check(moved.Add(&first.first) && moveAssigned.Add(&first.first),"both moved destinations keep fast-path ownership");
    Check(first.calls==before,"copy/move and exact hits make no COM calls while lease stays alive");
    // Default moves intentionally retain the existing scalar count semantics.
    // No stale interface may be accepted through the moved-from empty tokens.
    const auto query=first.calls.query;
    Check(!copy.Add(&first.first) && first.calls.query==query+1,"full moved-from set canonicalizes rather than accepting a stale witness");
    copy={};assigned={};
    Check(copy.Add(&first.first) && assigned.Add(&second.first),"reset moved-from sets are reusable");
    moveAssigned=moveAssigned;
    Check(moveAssigned.Add(&second.first),"self copy assignment preserves exact ownership");
}

template<size_t N> void Capacity() {
    std::vector<std::unique_ptr<Fake>> objects;
    for(size_t i=0;i<N+1;++i)objects.push_back(std::make_unique<Fake>());
    Retention::Buffers<N> set;
    Check(set.Add(nullptr),"null succeeds at every N including zero");
    for(size_t i=0;i<N;++i)Check(set.Add(&objects[i]->first),"exactly N distinct buffers admitted");
    Check(set.count==N,"no set capacity expansion");
    const auto before=Retention::ReadTelemetry();
    Check(!set.Add(&objects.back()->first),"N plus one refuses");
    const auto after=Retention::ReadTelemetry();
    Check(after.bytes==before.bytes && after.resources==before.resources && after.refused==before.refused+1,"capacity failure rolls back transient new charge");
    if constexpr(N>0)Check(set.Add(&objects.front()->alias) && set.count==N,"alias duplicate succeeds at full N");
}

void BudgetAndFailures() {
    Fake whole(static_cast<UINT>(Retention::BufferBudget)),extra,oversized(static_cast<UINT>(Retention::BufferBudget+1)),zero(0);
    Retention::Buffers<2> set;
    Check(set.Add(&whole.first),"exact buffer budget admitted");
    Check(set.Add(&whole.alias) && set.count==1,"canonical alias accepted at full global byte budget");
    auto bytesRefused=Retention::ReadRefusalTelemetry().bytes;
    Check(!set.Add(&extra.first) && Retention::ReadRefusalTelemetry().bytes==bytesRefused+1,"next distinct buffer exceeds original global byte budget");
    set={};bytesRefused=Retention::ReadRefusalTelemetry().bytes;
    Check(!set.Add(&oversized.first) && Retention::ReadRefusalTelemetry().bytes==bytesRefused+1,"oversized buffer refuses without expanding budget");
    auto refused=Retention::ReadTelemetry().refused;
    Check(!set.Add(&zero.first) && Retention::ReadTelemetry().refused==refused+1,"zero-byte descriptor retains rejection semantics");
    extra.failQuery=true;refused=Retention::ReadTelemetry().refused;
    Check(!set.Add(&extra.first) && Retention::ReadTelemetry().refused==refused+1,"failed canonical QI refuses");
    extra.failQuery=false;extra.nullIdentity=true;refused=Retention::ReadTelemetry().refused;
    Check(!set.Add(&extra.first) && Retention::ReadTelemetry().refused==refused+1,"successful QI with null identity still refuses");
    extra.nullIdentity=false;
    const auto allocationRefused=Retention::ReadRefusalTelemetry().allocation;
    AllocationTest::failNext=true;
    const bool admitted=set.Add(&extra.first);
    const bool allocationConsumed=!AllocationTest::failNext.exchange(false);
    Check(!admitted && allocationConsumed && Retention::ReadRefusalTelemetry().allocation==allocationRefused+1,"make_shared allocation failure fails closed");
    Check(extra.refs==1 && !extra.first.refs && !extra.identity.refs && !set.count,"all rejected owned interfaces are released");
    Check(set.Add(&extra.first),"refusal cannot poison later admission");
}

void RegistryCapacity() {
    std::vector<std::unique_ptr<Fake>> objects;
    std::vector<Retention::Token> tokens;
    for(size_t i=0;i<2049;++i)objects.push_back(std::make_unique<Fake>());
    for(size_t i=0;i<2048;++i) {
        auto token=Retention::Buffer(&objects[i]->first);
        Check(bool(token),"original registry admits 2048 canonical resources");
        tokens.push_back(std::move(token));
    }
    Check(Retention::ReadTelemetry().resources==2048,"registry capacity remains exactly 2048");
    const auto refused=Retention::ReadRefusalTelemetry().capacity;
    Check(!Retention::Buffer(&objects.back()->first) && Retention::ReadRefusalTelemetry().capacity==refused+1,"2049th registry identity refuses");
    Retention::Buffers<1> set;
    Check(set.Add(&objects.front()->alias) && set.tokens[0]==tokens[0],"existing canonical lease survives full registry capacity");
    tokens.pop_back();
    Check(bool(Retention::Buffer(&objects.back()->first)),"released registry slot can be reused after actual lease retirement");
}

void Contention() {
    Fake object,other;
    Retention::Buffers<1> set;
    Check(set.Add(&object.first),"admit interface before contention");
    const auto before=object.calls;
    std::promise<void> locked,finish;
    auto wait=finish.get_future();auto ready=locked.get_future();
    std::thread holder([&]{std::lock_guard lock(Retention::detail::Store()->mutex);locked.set_value();wait.wait();});
    ready.wait();
    bool accepted=true;
    for(unsigned i=0;i<10000;++i)accepted=set.Add(&object.first)&&accepted;
    const auto refused=Retention::ReadRefusalTelemetry().contention;
    const bool alias=set.Add(&object.alias);
    Retention::Buffers<1> distinct;
    const bool otherAccepted=distinct.Add(&other.first);
    const auto after=Retention::ReadRefusalTelemetry().contention;
    finish.set_value();holder.join();
    Check(accepted && object.calls.desc==before.desc+1 && object.calls.query==before.query+1,
        "10000 exact hits bypass held registry mutex; only unknown alias calls COM");
    Check(!alias && !otherAccepted && after==refused+2,"unrecognized alias and new identity retain nonblocking contention refusal");
    Check(set.count==1 && !distinct.count,"contention never changes set membership");
    Check(set.Add(&object.alias) && distinct.Add(&other.first),"canonical admission resumes after contention");
}

void DestructionAndReuse() {
    alignas(Fake) unsigned char storage[sizeof(Fake)];
    auto* first=new(storage) Fake(32);
    const auto firstAddress=&first->first;
    Retention::Buffers<1> set;
    Check(set.Add(firstAddress),"first lifetime admitted");
    auto copy=set;
    first->inspectFinal=true;first->Release(); // release external root
    set={};
    Check(!first->dead && Retention::ReadTelemetry().bytes==32,"copied token keeps COM object and charge alive");
    copy={};
    Check(first->dead && first->finalCalls==1 && first->finalCharged,"final COM release happens once while charge remains live");
    Check(first->finalRegistryFree && first->finalFacesReleased,"exact interface released before canonical identity and outside registry lock");
    Clean();first->~Fake();
    auto* next=new(storage) Fake(64);
    Check(&next->first==firstAddress,"new COM lifetime deliberately reuses exact interface address");
    Check(set.Add(&next->first) && set.tokens[0]->size==64 && next->calls.desc==1 && next->calls.query==1,
        "address reuse must perform fresh descriptor and canonical admission");
    set={};next->~Fake();
}

void OtherCharges() {
    int key=0;
    auto payload=Retention::Payload(&key,Retention::PayloadBudget);
    Check(bool(payload) && payload==Retention::Payload(&key,Retention::PayloadBudget),"payload canonical allocation sharing unchanged");
    int other=0;
    Check(!Retention::Payload(&other,1),"payload budget unchanged");
    auto source=Retention::Source(Retention::SourceBudget-Retention::SourceReserve);
    Check(bool(source) && !Retention::Source(1),"ordinary source reserve unchanged");
    auto priority=Retention::Source(Retention::SourceReserve,true);
    Check(bool(priority) && !Retention::Source(1,true),"priority source ceiling unchanged");
}

constexpr GUID LifetimeKey={0x13ae79c2,0xe8b1,0x4f13,{0xa7,0xad,0x3b,0xe4,0x51,0xc3,0xc7,0xf0}};
struct LifetimeProbe final : IUnknown {
    ULONG refs=1;bool* charged;bool* destroyed;
    LifetimeProbe(bool& charge,bool& dead):charged(&charge),destroyed(&dead){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override {
        if(!out)return E_POINTER;*out=nullptr;if(iid!=__uuidof(IUnknown))return E_NOINTERFACE;
        *out=static_cast<IUnknown*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override {return ++refs;}
    ULONG STDMETHODCALLTYPE Release()override {
        const auto remaining=--refs;
        if(!remaining) {
            const auto t=Retention::ReadTelemetry();*charged=t.resources!=0 && t.bytes>=64;
            *destroyed=true;delete this;
        }
        return remaining;
    }
};

void Warp() {
    Clean();
    ComPtr<ID3D11Device> device;
    const auto hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,nullptr);
    Check(SUCCEEDED(hr) && device,"create actual software D3D11 WARP device");
    D3D11_BUFFER_DESC desc{};desc.ByteWidth=64;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    ComPtr<ID3D11Buffer> buffer,other;
    Check(SUCCEEDED(device->CreateBuffer(&desc,nullptr,&buffer)),"create native buffer");
    Check(SUCCEEDED(device->CreateBuffer(&desc,nullptr,&other)),"create second equal-descriptor native buffer");
    bool charged=false,destroyed=false;
    auto* observer=new LifetimeProbe(charged,destroyed);
    Check(SUCCEEDED(buffer->SetPrivateDataInterface(LifetimeKey,observer)),"native buffer owns a private lifetime probe");
    observer->Release();
    Retention::Buffers<1> set;
    Check(set.Add(buffer.Get()),"admit native interface");
    ComPtr<IUnknown> identity;
    Check(SUCCEEDED(buffer.As(&identity)) && set.tokens[0]->object.Get()==identity.Get(),"native registry identity is queried canonical IUnknown");
    Check(set.tokens[0]->buffer.Get()==buffer.Get(),"native exact interface retained by real lease");
    bool accepted=true;
    for(unsigned i=0;i<10000;++i)accepted=set.Add(buffer.Get())&&accepted;
    Check(accepted && set.count==1 && Retention::ReadTelemetry().bytes==64,"10000 native repeats keep one charge");
    ComPtr<ID3D11Resource> resource;ComPtr<ID3D11Buffer> roundTrip;
    Check(SUCCEEDED(buffer.As(&resource)) && SUCCEEDED(resource.As(&roundTrip)),"native resource-to-buffer interface round trip");
    Check(set.Add(roundTrip.Get()) && set.count==1,"native interface round trip deduplicates without consuming capacity");
    Check(!set.Add(other.Get()),"different native buffer refuses full set");
    auto copy=set;
    auto moved=std::move(set);
    identity.Reset();resource.Reset();roundTrip.Reset();buffer.Reset();
    Check(!destroyed,"token-held native allocation survives all external interface releases");
    auto* borrowed=moved.tokens[0]->buffer.Get();
    Check(moved.Add(borrowed) && copy.Add(borrowed),"copied/moved native tokens retain valid exact interfaces");
    moved={};
    Check(!destroyed && Retention::ReadTelemetry().bytes==64,"last copied native token retains allocation and charge");
    copy={};
    Check(destroyed && charged,"actual native final release precedes resource uncharge");
    Clean();
    Retention::Buffers<0> zero;
    Check(zero.Add(nullptr) && !zero.Add(other.Get()),"native zero-capacity semantics unchanged");
    Clean();
    std::printf("DepthRetention WARP: %u checks passed; native software D3D11 interfaces, no SDK/game validation\n",checks);
}
}

int main(int argc,char** argv) {
    if(argc==2 && std::strcmp(argv[1],"--warp")==0) {Warp();return 0;}
    if(argc!=1 && (argc!=2 || std::strcmp(argv[1],"--cpu")!=0)) {
        std::fprintf(stderr,"usage: DepthRetentionTests.exe [--cpu | --warp]\n");return 2;
    }
    Group("token-owned exact interface fast path",ExactFastPath);
    Group("canonical aliases and local refusal",CanonicalAliases);
    Group("default copy/move semantics",CopiesAndMoves);
    Group("N=0",Capacity<0>);
    Group("N=1",Capacity<1>);
    Group("N=4",Capacity<4>);
    Group("N=90",Capacity<90>);
    Group("N=254",Capacity<254>);
    Group("budgets, canonical failure, allocation failure",BudgetAndFailures);
    Group("fixed global registry capacity",RegistryCapacity);
    Group("exact hit during registry contention",Contention);
    Group("release ordering and exact-address reuse",DestructionAndReuse);
    Group("payload and source charge invariants",OtherCharges);
    std::printf("DepthRetention CPU: %u groups / %u checks passed; native COM fakes, no GPU or game\n",groups,checks);
    if(argc==1){checks=0;Warp();}
}
