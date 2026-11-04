#pragma once
#include <cstdint>
namespace DlssNrTemporalDetail
{
struct Decision { bool enabled=false, forceFull=false, allowHistory=false, noAdvance=false; unsigned missingDepthRun=0; };
class Policy
{
    bool active=false, hadDepth=false, hasFrame=false;
    uint64_t lastFrame=0;
    unsigned missingRun=0;
public:
    Decision Next(bool requested, bool depth, bool currentGuides, bool baseIdentity, bool reset,
                  uint64_t frame, unsigned maxMissing=2, bool pictureFlowOnly=false)
    {
        Decision d;
        if (!requested) { *this={}; return d; }
        d.enabled=true;
        const bool duplicate=hasFrame && frame<=lastFrame;
        if (duplicate) { d.forceFull=true; d.noAdvance=true; return d; }
        const bool fresh=!active || reset;
        const bool returning=active && !hadDepth && depth;
        if (fresh) missingRun=0;
        if (depth) missingRun=0;
        else if (!duplicate && missingRun<~0u) ++missingRun;
        // The caller separately authenticates interval-1 feedback or the ordinary
        // source-by-source flow/confidence chain. Depth stays genuinely absent;
        // native/depth feedback still requires depth.
        const bool authority=currentGuides && baseIdentity && (depth || pictureFlowOnly);
        d.missingDepthRun=missingRun;
        d.forceFull=fresh || duplicate || returning || !authority ||
            (!depth && !pictureFlowOnly && missingRun>maxMissing);
        d.allowHistory=!fresh && authority; // only the explicitly authenticated current guide branch carries history
        active=true; hadDepth=depth; hasFrame=true; lastFrame=frame;
        return d;
    }
};
}
