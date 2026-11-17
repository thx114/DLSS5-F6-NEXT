// SPDX-License-Identifier: GPL-3.0-or-later
#include <shaders/dlssnr/DlssNr_ColourEncoding.h>
#include <shaders/dlssnr/DlssNr_SurfaceFormats.h>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
unsigned checks=0;
void Check(bool ok,const char* label) { ++checks; if(!ok) throw std::runtime_error(label); }
}
int main() {
    try {
        using namespace DlssNrColourEncoding;
        for(bool hdr:{false,true}) for(bool floating:{false,true}) {
            const auto old=Resolve(kAuto,hdr,floating);
            const auto fallback=Resolve(kAuto,hdr,floating,false);
            Check(old.automatic&&old.flagKnown,"ordinary SR flag keeps legacy auto semantics");
            Check(fallback.automatic&&!fallback.flagKnown&&fallback.encoding==Encoding::Unspecified&&
                      !fallback.LinearHdr()&&ShaderConversion(fallback.encoding)==0,
                  "unknown synthetic flags cannot select a transfer or an HDR proxy");
            Check(Describe(fallback,"test").find("unverified")!=std::string::npos,
                  "the auto fallback is visible in status");
            for(unsigned forced=1;forced<kSettingCount;++forced) {
                const auto choice=Resolve(forced,hdr,floating,false);
                Check(!choice.automatic&&choice.encoding==static_cast<Encoding>(forced),
                      "explicit NR choice overrides the synthetic flag");
            }
        }
        Check(Resolve(kAuto,false,true,false).encoding==Resolve(kAuto,true,true,false).encoding,
              "synthetic SR HDR flag changes do not change the NR colour domain");
        Check(Resolve(kAuto,true,true).LinearHdr()&&!Resolve(kAuto,false,true).LinearHdr(),
              "declared native SR flags retain their existing interpretation");
        const auto screen=ResolveFinished(kAuto,true,Screen::Pq,false,true);
        Check(screen.supported&&screen.choice.encoding==Encoding::Pq,"finished-picture auto retains screen colour authority");
        Check(DlssNr::WorkingColourFormat(26)==10,"R11 uses signed FP16 intermediates");
        Check(DlssNr::WorkingColourFormat(10)==10,"FP16 remains FP16");
        Check(DlssNr::WorkingColourFormat(87)==28,"BGRA retains canonical shader channel order");
        Check(!DlssNr::SameColourFamily(26,10),"R11 and FP16 sharing allocations does not merge source identity");
        std::printf("PASS NR automatic colour provenance: %u checks\n",checks);
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"FAIL %s\n",e.what());return 1; }
}
