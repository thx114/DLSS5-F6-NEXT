#include <framegen/FrameOutputRate.h>
#include <cmath>
#include <cstdlib>
#include <iostream>

using framegen::OutputRateWindow;
static void Check(bool value, const char* message)
{
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
int main()
{
    constexpr uint64_t step = 10000000, start = 1000000000;
    OutputRateWindow window;
    for (uint64_t i = 1; i <= 100; ++i)
        window.Observe(1, 0, i + 1, i, start + (i-1)*step, start+i*step, 2, true);
    auto rate = window.Read(start + 100*step);
    Check(rate.known && std::abs(rate.hz - 200.0) < 0.001 && rate.frames == 200, "continuous interval count");
    Check(window.Read(start + 100*step).frames == rate.frames, "read consumed samples");
    Check(!window.Read(start + 100*step + 3000000001ull).known, "stale sample");
    window.Observe(1, 0, 103, 102, start + 101*step, start+102*step, 100, true);
    Check(!window.Read(start+102*step).known, "sequence gap retained old rate");
    window.Reset();
    for (uint64_t i = 104; i < 134; ++i)
        window.Observe(1, 0, i, i-1, start+(i-3)*step, start+(i-2)*step, 0, true);
    rate = window.Read(start+132*step);
    Check(rate.known && rate.frames == 0 && rate.hz == 0, "zero-output interval");
    window.Observe(1, 1, 134, 133, start+132*step, start+133*step, 1, true);
    Check(!window.Read(start+133*step).known, "viewport switch retained rate");
    window.Observe(1, 1, 135, 134, start+133*step, start+134*step, 1, false);
    Check(!window.Read(start+134*step).known, "failed query retained rate");
    window.Reset();
    for (uint64_t i=1;i<=30;++i)
        window.Observe(1,0,i+1,i,start+(i-1)*step,start+i*step,3,true);
    Check(window.Read(start+30*step).known, "warm output window");
    window.Observe(1,0,31,30,start+29*step,start+30*step,3,true);
    Check(!window.Read(start+30*step).known, "duplicate interval cannot double count");
    window.Observe(2,0,32,31,start+30*step,start+31*step,3,true);
    Check(!window.Read(start+31*step).known, "API switch invalidates old rate");
    window.Observe(2,0,33,32,start+31*step,start+31*step,3,true);
    Check(!window.Read(start+31*step).known, "zero interval is unknown");
    std::cout << "FrameOutputRate: PASS\n";
}
