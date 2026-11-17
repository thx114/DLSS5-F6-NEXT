#include <framegen/FgRuntimeStatus.h>
#include <cstdlib>
#include <iostream>
#include <thread>
using namespace framegen;
static void Check(bool value, const char* name)
{ if (!value) { std::cerr << name << '\n'; std::exit(1); } }
struct Feature { virtual ~Feature() = default; uint64_t data = 0; };
struct DxFeature : virtual Feature { uintptr_t Identity() const { return reinterpret_cast<uintptr_t>(static_cast<const Feature*>(this)); } };
struct Provider : virtual DxFeature { uint64_t padding[8] {}; };
int main()
{
    RuntimeStatusInputs v;
    Check(SelectRuntimeStatus(v) == RuntimeStatus::Off, "disabled request");
    v.external = true; Check(SelectRuntimeStatus(v) == RuntimeStatus::External, "external ownership");
    v.external = false; v.requested = true; Check(SelectRuntimeStatus(v) == RuntimeStatus::NoRoute, "no route");
    v.route = true; Check(SelectRuntimeStatus(v) == RuntimeStatus::Initializing, "no provider");
    v.provider = true; Check(SelectRuntimeStatus(v) == RuntimeStatus::Waiting, "input warmup");
    v.active = true; Check(SelectRuntimeStatus(v) == RuntimeStatus::EnabledUnconfirmed, "activity is not generation proof");
    v.paused = true; Check(SelectRuntimeStatus(v) == RuntimeStatus::Paused, "pause before activity");
    Provider provider;
    Feature* canonical = &provider;
    const auto key = reinterpret_cast<uintptr_t>(canonical);
    Check(key == provider.Identity(), "virtual base identity");
    RuntimeObservations cache;
    RuntimeSnapshot in, out;
    in.provider = key; in.instance = NextRuntimeInstance(); in.epoch = 2;
    in.tickNs = 2000000000ull; in.status = RuntimeStatus::OutputObserved;
    in.outputRate = {1,0,1000000000ull,2000000000ull,120,30,120.0,true};
    cache.Publish(in);
    Check(cache.Read(key,in.tickNs,out) && out.outputRate.hz == 120.0, "output snapshot");
    Check(cache.Read(key,in.tickNs,out) && out.outputRate.frames == 120, "reads do not consume");
    Check(!cache.Read(key+1,in.tickNs,out), "wrong provider");
    cache.Invalidate(key,in.instance+1);
    Check(cache.Read(key,in.tickNs,out), "old instance cannot invalidate current");
    Check(cache.Read(key,5000000001ull,out) && !out.outputRate.known && out.status == RuntimeStatus::Stale, "stale snapshot");
    in.tickNs = 6000000000ull; cache.Publish(in);
    Check(cache.Read(key,in.tickNs,out) && !out.outputRate.known && out.status == RuntimeStatus::EnabledUnconfirmed,
          "new Present cannot renew an old SDK rate");
    cache.Invalidate(key,in.instance); Check(!cache.Read(key,in.tickNs,out), "provider retirement");
    in.instance = NextRuntimeInstance(); in.epoch = 3; in.outputRate = {}; in.status = RuntimeStatus::Waiting;
    cache.Publish(in); cache.Invalidate(key,in.instance-1);
    Check(cache.Read(key,in.tickNs,out) && out.epoch == 3 && !out.outputRate.known, "same address new lifetime");
    std::thread writer([&] {
        for (uint64_t n=4;n<10004;++n) { auto s=in; s.epoch=n; s.sdkStatus=static_cast<uint32_t>(n); cache.Publish(s); }
    });
    for (int n=0;n<10000;++n) if (cache.Read(key,in.tickNs,out) && out.epoch>=4)
        Check(out.epoch == out.sdkStatus, "coherent concurrent scalars");
    writer.join();
    cache.Invalidate(key,0); Check(!cache.Read(key,in.tickNs,out), "constructor clears reused identity");
    std::cout << "FgRuntimeStatus: PASS\n";
}
