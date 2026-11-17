#pragma once
#include <array>
namespace taa::test {
inline std::array<float,148> ProjectionConstants(unsigned width,unsigned height,float x=.25f,float y=-.125f) {
    std::array<float,148> cb{};cb[16]=float(width);cb[17]=float(height);
    cb[40]=.5f;cb[45]=-.3125f;cb[51]=10;cb[54]=-1;cb[55]=.0001f;
    cb[52]=-2*x/float(width)*cb[40];cb[53]=2*y/float(height)*cb[45];
    cb[72]=2;cb[77]=-3.2f;cb[82]=.00001f;cb[83]=-1;cb[86]=.1f;
    for(unsigned i=0;i<16;++i)cb[56+i]=cb[72+i];
    return cb;
}
}
