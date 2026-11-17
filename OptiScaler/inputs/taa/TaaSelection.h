// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "TaaProfile.h"
#include <algorithm>
#include <array>
#include <vector>

namespace taa {
struct CandidateKey {
    Stage stage=Stage::Pixel;
    uint64_t hash=0;
    bool operator==(const CandidateKey&) const = default;
};
inline std::string KeyText(CandidateKey key) {
    std::array<char,17> hash{};
    auto result=std::to_chars(hash.data(),hash.data()+16,key.hash,16);
    return std::string(key.stage==Stage::Pixel?"PS:":"CS:")+std::string(16-size_t(result.ptr-hash.data()),'0')+std::string(hash.data(),result.ptr);
}
inline std::optional<CandidateKey> ParseKey(std::string_view value) {
    const auto lower=Lower(value);
    if(lower.empty()||lower=="auto")return CandidateKey{};
    if(lower.size()!=19||(lower.substr(0,3)!="ps:"&&lower.substr(0,3)!="cs:"))return {};
    CandidateKey key;key.stage=lower[0]=='p'?Stage::Pixel:Stage::Compute;
    const auto result=std::from_chars(lower.data()+3,lower.data()+lower.size(),key.hash,16);
    if(result.ec!=std::errc{}||result.ptr!=lower.data()+lower.size()||!key.hash)return {};
    return key;
}
struct Candidate {
    uint64_t hash=0,calls=0;
    Stage stage=Stage::Pixel;
    std::string bindings,reason;
    uint64_t lastSeen=0;
    uint64_t lastAnalysisLog=0;
    bool analysisLogged=false,loggedReady=false;
    std::string loggedReason;
    unsigned width=0,height=0,frameCalls=0;
    std::optional<Profile> inferred;
    CandidateKey Key() const {return {stage,hash};}
    bool AnalysisDue(uint64_t frame) const {
        return !analysisLogged||((loggedReason!=reason||loggedReady!=bool(inferred))&&frame>=lastAnalysisLog&&frame-lastAnalysisLog>=300);
    }
    void RecordAnalysis(uint64_t frame) {
        analysisLogged=true;lastAnalysisLog=frame;loggedReason=reason;loggedReady=bool(inferred);
    }
};
// Decisions use the completed source frame only. An absent/unsupported user
// pin never silently falls back to a different shader.
inline std::optional<CandidateKey> ChooseCandidate(const std::vector<Candidate>& candidates,uint64_t frame,
    CandidateKey pinned={},CandidateKey previous={}) {
    const Candidate* best=nullptr;
    for(const auto& c:candidates) {
        if(c.lastSeen!=frame||!c.inferred)continue;
        if(pinned.hash) {if(c.Key()==pinned)return pinned;continue;}
        if(c.frameCalls!=1)continue;
        const uint64_t area=uint64_t(c.width)*c.height;
        const uint64_t oldArea=best?uint64_t(best->width)*best->height:0;
        if(!best||area>oldArea||(area==oldArea&&c.Key()==previous))best=&c;
    }
    return best?std::optional(best->Key()):std::nullopt;
}
} // namespace taa
