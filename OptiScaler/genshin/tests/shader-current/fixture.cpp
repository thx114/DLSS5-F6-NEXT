// SPDX-License-Identifier: GPL-3.0-or-later
// Executes the complete production implementation against real WARP COM objects.
// Coverage receipts and NR private records are seeded fixture inputs; this does
// not test hook installation, game bytecode, SDK work or GPU completion.
#define NOMINMAX
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include "../../GenshinFamilySkip.cpp"

namespace F = GenshinLegacy::Family;
namespace P = GenshinLegacy::PsState;
using Microsoft::WRL::ComPtr;
namespace {
unsigned checks = 0;
void Check(bool value, const char* why) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", why); std::abort(); }
}
void Hr(HRESULT value, const char* why) { Check(SUCCEEDED(value), why); }
bool Controls(uint64_t epoch) { return epoch == 17; }
struct Fixture {
    static constexpr uint64_t Epoch = 11;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    F::CreationOwner owner;
    ComPtr<ID3D11PixelShader> a, b, unmarked;
    std::array<unsigned char, 30> edit{};
    std::array<unsigned char, 96> trampoline{};
    std::array<unsigned char, 32> hook{};
    void* executed = trampoline.data();
    Fixture() {
        Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &device, nullptr, &context), "create WARP device");
        Check(owner.Bind(device.Get(), Epoch), "bind actual creation owner");
        a = Shader("float4 main():SV_Target{return float4(1,0,0,1);}", true);
        b = Shader("float4 main():SV_Target{return float4(0,1,0,1);}", true);
        unmarked = Shader("float4 main():SV_Target{return float4(0,0,1,1);}", false);
    }
    ~Fixture() { P::RetireOwner(); P::coverage.store({}); context->ClearState(); owner.Reset(); }
    ComPtr<ID3D11PixelShader> Shader(const char* source, bool mark) {
        ComPtr<ID3DBlob> blob, errors;
        Hr(D3DCompile(source, std::strlen(source), "fixture", nullptr, nullptr, "main", "ps_5_0",
            0, 0, &blob, &errors), "compile fixture shader");
        ComPtr<ID3D11PixelShader> shader;
        Hr(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader), "create real shader");
        if (mark) Check(owner.RecordReturned(device.Get(), shader.Get(), blob->GetBufferPointer(), blob->GetBufferSize()), "mark real creation return");
        return shader;
    }
    void Set(ID3D11PixelShader* shader) { context->PSSetShader(shader, nullptr, 0); }
    F::Shader Compare(F::CurrentComparison& c, F::ReadObservation& o) {
        return F::CompareCurrent(context.Get(), device.Get(), Epoch, o, c);
    }
    void Coverage() {
        // Supply only the installer's OUTPUT as a fixture. No code is patched or
        // executed through these synthetic receipt buffers. Consumers stay real.
        P::RetireOwner(); P::Coverage::refused.store(false);
        GenshinLegacy::PublicPsBinding::stopped.store(false);
        GenshinLegacy::PublicPsBinding::refreshState.store(0);
        auto c = std::make_shared<P::Coverage>();
        c->context = context; c->device = device;
        Hr(context.As(&c->context1), "query actual context1");
        c->dispatch = GenshinLegacy::PublicPsBinding::ReadDispatch(context.Get());
        c->publicPsMutationCovered = true;
        for (size_t i = 0; i < c->dispatch.size(); ++i) {
            auto& e = c->endpoints[i];
            e.requested = c->dispatch[i]; e.hook = hook.data(); e.executed = &executed;
            e.entry = edit.data(); e.trampoline = trampoline.data(); e.size = 5;
            c->variants[i][0] = e; c->variantCounts[i] = 1;
        }
        Check(P::PublicCoverageIntact(*c), "prepared receipt fixture passes production boundary");
        P::coverage.store(c);
    }
    F::StoredShader Record(ID3D11PixelShader* shader) {
        F::StoredShader r{}; UINT size = sizeof(r);
        Hr(shader->GetPrivateData(F::ShaderKey, &size, &r), "read actual private record");
        Check(F::Valid(r, size), "production validates fixture record"); return r;
    }
    void Put(ID3D11PixelShader* shader, F::StoredShader r) {
        r.seal = F::Seal(r);
        Hr(shader->SetPrivateData(F::ShaderKey, sizeof(r), &r), "write test mutation bypassing cache notification");
    }
    void NrRecord(ID3D11PixelShader* shader, size_t variant=0) {
        const auto& profile=GenshinFsrProfiles::Reviewed[variant];
        auto r = Record(shader); r.hash = profile.hash;
        r.bytes = profile.bytes; r.digest = profile.digest;
        Put(shader, r);
        F::MetadataMutation invalidate(shader);
    }
};
void Metadata() {
    Fixture f; f.Set(f.a.Get()); F::CurrentComparison c; F::ReadObservation o;
    const auto first = f.Compare(c, o);
    Check(first.Known() && c.stable && !c.mismatch && !c.coverageRefused, "cold stable equal");
    const auto second = f.Compare(c, o);
    Check(second.hash == first.hash && c.cached.metadataCached && c.stable && !c.mismatch, "warm metadata equal");
    auto changed = f.Record(f.a.Get()); changed.hash ^= 1; f.Put(f.a.Get(), changed);
    const auto corrected = f.Compare(c, o);
    Check(c.stable && c.mismatch && c.coverageRefused && corrected.hash == changed.hash, "same-object metadata mismatch corrected");
    Check(!corrected.nrProfile, "ordinary correction invents no NR profile");
    auto reread = F::Read(f.device.Get(), f.a.Get(), Fixture::Epoch, &o);
    Check(reread.hash == changed.hash && !o.metadataCached, "mismatched metadata slot actually invalidated");
    f.Set(f.unmarked.Get()); const auto missing = f.Compare(c, o);
    Check(!missing.Known() && !missing.nrProfile && c.stable && o.state == F::ReadState::MissingRecord, "missing mark never authenticates");
    f.Set(f.a.Get());
    const auto wrong = F::CompareCurrent(f.context.Get(), f.device.Get(), Fixture::Epoch + 1, o, c);
    Check(!wrong.Known() && o.state == F::ReadState::WrongEpoch, "wrong epoch never authenticates marked shader");
    changed.seal ^= 1;
    Hr(f.a->SetPrivateData(F::ShaderKey, sizeof(changed), &changed), "inject invalid record seal");
    const auto invalid = f.Compare(c, o);
    Check(c.stable && c.mismatch && c.coverageRefused && !invalid.Known() && o.state == F::ReadState::InvalidRecord,
        "invalid current record cannot retain old cached classification");
}
void BindingAndUnknown() {
    Fixture f; f.Coverage(); f.Set(f.a.Get());
    F::CurrentComparison c; F::ReadObservation o;
    {
        P::DrawScope draw(&f.owner, f.context.Get(), f.device.Get(), Fixture::Epoch, 17, true, Controls);
        const auto first = f.Compare(c, o);
        Check(first.Known() && c.stable && !c.mismatch, "real getter seeds binding consumer");
        const auto equal = f.Compare(c, o);
        Check(equal.Known() && c.cached.bindingCached && !c.mismatch, "binding consumer actually hits");
        f.Set(f.b.Get()); // deliberately omit mutation notification
        const auto fixed = f.Compare(c, o);
        Check(c.cached.bindingCached && c.stable && c.mismatch && c.coverageRefused, "unobserved setter mismatch refuses coverage");
        Check(fixed.Known() && c.actual.shader == reinterpret_cast<uintptr_t>(f.b.Get()) && fixed.hash == c.actualHash, "correction uses actual current shader");
        Check(!P::current.load() && P::Coverage::refused.load(), "optional domain really retired");
    }
    const auto fallback = f.Compare(c, o);
    Check(fallback.Known() && !c.cached.bindingCached, "later invocation uses getter fallback");
    f.Coverage(); f.Set(f.a.Get());
    {
        P::DrawScope draw(&f.owner, f.context.Get(), f.device.Get(), Fixture::Epoch, 17, true, Controls);
        Check(f.Compare(c, o).Known(), "seed second independent lifetime");
        P::Mutation pending(f.context.Get()); f.Set(f.b.Get());
        Check(!f.Compare(c, o).Known() && !c.stable && o.state == F::ReadState::UnstableComparison,
            "in-flight mutation is Unknown rather than cache fault evidence");
        Check(!c.coverageRefused, "unstable comparison does not terminally refuse coverage");
    }
}
void Profiles() {
    Fixture f; f.NrRecord(f.b.Get()); f.Set(f.b.Get());
    F::CurrentComparison c; F::ReadObservation o;
    const auto first = f.Compare(c, o);
    Check(first.nrProfile && first.nrProfile->Matches(Fixture::Epoch, first.identity), "full production Read creates exact profile from seeded record");
    const auto equal = f.Compare(c, o);
    Check(c.stable && !c.mismatch && equal.nrProfile == first.nrProfile, "equal comparison preserves cached profile");
    f.Coverage(); f.Set(f.a.Get());
    {
        P::DrawScope draw(&f.owner, f.context.Get(), f.device.Get(), Fixture::Epoch, 17, true, Controls);
        Check(f.Compare(c, o).Known(), "seed ordinary binding before NR correction"); f.Set(f.b.Get());
        const auto fixed = f.Compare(c, o);
        Check(c.stable && c.mismatch && c.coverageRefused && fixed.nrProfile &&
            fixed.nrProfile->Matches(Fixture::Epoch, fixed.identity), "binding correction retains full NR capability");
    }
    auto record = f.Record(f.b.Get()); record.digest[0] ^= 1; f.Put(f.b.Get(), record);
    const auto invalidProfile = f.Compare(c, o);
    Check(c.stable && c.mismatch && c.coverageRefused && invalidProfile.Known() && !invalidProfile.nrProfile,
        "digest mutation cannot retain cached NR profile despite same scalar identity");
}
}
void OverlayProfile() {
    Fixture f;f.NrRecord(f.b.Get(),1);f.Set(f.b.Get());
    const auto profile=F::Read(f.device.Get(),f.b.Get(),Fixture::Epoch);
    Check(profile.nrProfile && profile.nrProfile->MatchesShader(Fixture::Epoch,profile.identity,
        GenshinFsrProfiles::Reviewed[1].hash),"new variant exact authenticated numerical capability");
    Check(!profile.nrProfile->MatchesShader(Fixture::Epoch,profile.identity,
        GenshinFsrProfiles::Reviewed[0].hash),"new variant cannot impersonate old bytecode");
    auto r=f.Record(f.b.Get());r.digest=GenshinFsrProfiles::Reviewed[0].digest;f.Put(f.b.Get(),r);
    F::MetadataMutation invalidate(f.b.Get());
    const auto mixed=F::Read(f.device.Get(),f.b.Get(),Fixture::Epoch);
    Check(mixed.Known() && !mixed.nrProfile,"new hash with old digest grants no numerical capability");
}
void ActualBytecode(const char* directory) {
    Fixture f;
    for(const auto& reviewed:GenshinFsrProfiles::Reviewed) {
        char name[64]{};std::snprintf(name,sizeof(name),"/fsr-%016llX.dxbc",static_cast<unsigned long long>(reviewed.hash));
        std::ifstream file(std::string(directory)+name,std::ios::binary);
        Check(bool(file),"actual captured bytecode available");
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
        Check(bytes.size()==reviewed.bytes,"actual creation bytes match reviewed size");
        ComPtr<ID3D11PixelShader> ps;
        Hr(f.device->CreatePixelShader(bytes.data(),bytes.size(),nullptr,&ps),"real captured shader creation");
        Check(f.owner.RecordReturned(f.device.Get(),ps.Get(),bytes.data(),bytes.size()),"digest real creation bytes");
        const auto actual=F::Read(f.device.Get(),ps.Get(),Fixture::Epoch);
        Check(actual.hash==reviewed.hash && actual.nrProfile &&
            actual.nrProfile->MatchesShader(Fixture::Epoch,actual.identity,reviewed.hash),"actual captured shader certificate issued");
    }
}
int main(int argc,char** argv) {
    Metadata(); BindingAndUnknown(); Profiles(); OverlayProfile();
    if(argc>1)ActualBytecode(argv[1]);
    std::printf("PASS %u production CompareCurrent checks; real WARP COM, seeded coverage/profile records; no hook install/SDK/game proof\n", checks);
}
