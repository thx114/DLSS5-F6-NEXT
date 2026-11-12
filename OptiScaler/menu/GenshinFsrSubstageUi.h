// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <genshin/GenshinFsrSubstageProgress.h>
#include "UiLocalization.h"
#include <imgui/imgui.h>
#include <cstdio>

namespace CriticalUi::Substage
{
inline const char* StageText(GenshinSubstage::Stage stage) noexcept {
    constexpr const char* names[]={
        "目标绘制", "输入绑定", "私有输入准备", "SR准入", "SR上下文注册",
        "DX11 → DX12资源", "参数检查", "DLSS调用", "DLSS实际返回",
        "队列Execute", "内部回写完成", "SR结果验收", "原生输出认领", "原生输出完成",
        "NR输入准入", "NR模型实际返回", "FG输入", "FG实际执行"
    };
    return GenshinSubstage::Index(stage)<GenshinSubstage::Index(GenshinSubstage::Stage::Count)
        ?names[GenshinSubstage::Index(stage)]:"未知阶段";
}
inline const char* StateText(GenshinSubstage::State state) noexcept {
    using GenshinSubstage::State;
    switch(state) {
    case State::Waiting:return "WAITING";
    case State::Running:return "RUNNING";
    case State::Succeeded:return "SUCCEEDED";
    case State::Failed:return "FAILED";
    default:return "UNKNOWN";
    }
}
inline const char* ResourceText(GenshinSubstage::Resource resource) noexcept {
    using GenshinSubstage::Resource;
    switch(resource) {
    case Resource::Color:return "Color";
    case Resource::Depth:return "Depth";
    case Resource::Motion:return "Motion";
    case Resource::Output:return "Output";
    case Resource::Inputs:return "输入";
    case Resource::Context:return "上下文";
    case Resource::Queue:return "队列";
    case Resource::NativeOutput:return "原生输出";
    case Resource::Nr:return "NR";
    case Resource::Fg:return "FG";
    default:return "资源未知";
    }
}
inline const char* DomainText(GenshinSubstage::Domain domain) noexcept {
    using GenshinSubstage::Domain;
    switch(domain) {
    case Domain::Bindings:return "绑定";
    case Domain::Fsr:return "FSR";
    case Domain::Mapping:return "DX11→DX12";
    case Domain::Parameters:return "参数";
    case Domain::Ngx:return "NGX";
    case Domain::Hresult:return "HRESULT";
    case Domain::NativeClaim:return "输出认领";
    case Domain::Nr:return "NR";
    case Domain::Fg:return "FG";
    default:return "未知";
    }
}
inline const char* IssueText(GenshinSubstage::Issue issue) noexcept {
    using GenshinSubstage::Issue;
    switch(issue) {
    case Issue::NonTex2D:return "非Tex2D";
    case Issue::Rejected:return "拒绝";
    case Issue::Pending:return "等待";
    case Issue::NoProvider:return "提供器不可用";
    case Issue::Exception:return "异常";
    default:return "";
    }
}
// Display-only freshness budget: four UI sampling intervals without a source
// update become history. This is not an FSR setting or execution/admission gate.
inline constexpr std::uint64_t SampleIntervalMs=250, FreshnessBudgetMs=1000;
inline constexpr const char* WaitingHeading="等待新鲜 producer 事件";
struct Freshness {
    std::uint64_t updated=0,age=0;
    bool ageKnown=false,fresh=false;
};
inline Freshness Age(std::uint64_t updated,std::uint64_t now,bool valid) noexcept {
    Freshness result;result.updated=updated;
    // A future timestamp/clock rollback is unknown, never a fresh success.
    if(!valid || !updated || now<updated)return result;
    result.ageKnown=true;result.age=now-updated;
    result.fresh=result.age<FreshnessBudgetMs;
    return result;
}
inline Freshness Observe(const GenshinSubstage::Snapshot& s,std::uint64_t now) noexcept {
    auto updated=s.started;
    for(const auto& e:s.stages) {
        if(e.started>updated)updated=e.started;
        if(e.updated>updated)updated=e.updated;
    }
    return Age(updated,now,s.known && s.key.Known());
}
struct SnapshotCache {
    GenshinSubstage::Snapshot cached{};
    GenshinLegacy::Ui::Session requested{};
    std::uint64_t sampled=0;
    bool sampledKnown=false;
    template<class Source>
    bool Read(const GenshinLegacy::Ui::Session& session,std::uint64_t now,
              GenshinSubstage::Snapshot& result,Source& source) noexcept {
        // Discard the old session even when the new source read contends. A
        // later switch back must not resurrect an earlier session's success.
        if(requested!=session) {*this={};requested=session;}
        if(!session.Known())return false;
        if(!sampledKnown || now<sampled || now-sampled>=SampleIntervalMs) {
            sampled=now;sampledKnown=true; // Bound failed reads as well as successes.
            GenshinSubstage::Snapshot next;
            if(source.Read(next))cached=next;
        }
        if(!cached.known || !cached.key.Known() || cached.key.session!=session)return false;
        result=cached;return true;
    }
};
inline bool Read(const GenshinLegacy::Ui::Session& session,std::uint64_t now,
                 GenshinSubstage::Snapshot& result) noexcept {
    static SnapshotCache cache;
    return cache.Read(session,now,result,GenshinSubstage::Runtime());
}
inline void FreshnessText(const Freshness& freshness) {
    if(freshness.ageKnown)
        MenuText::Text("源快照更新距今 %.2fs（%s）",double(freshness.age)/1000.0,
                    freshness.fresh?"近期观测":"历史记录；等待新鲜 producer 事件");
    else
        MenuText::TextUnformatted("源快照年龄未知（时钟/时间戳不可验证）；等待新鲜 producer 事件");
}
inline void Summary(const GenshinSubstage::Snapshot& s,const GenshinSubstage::Progress& p,
                    const Freshness& freshness) {
    using namespace GenshinSubstage;
    FreshnessText(freshness);
    MenuText::Text("%s%s：%s%s",freshness.fresh?"":"历史记录：",StageText(p.current),
                StateText(p.state),p.timedOut?"（计时提示）":"");
    MenuText::Text("本阶段 %.1fs；本次 %.1fs",double(p.elapsed)/1000.0,double(p.total)/1000.0);
    if(s.firstFailure.known) {
        const auto& f=s.firstFailure;
        MenuText::TextWrapped("首次失败%s：%s / %s %s / %s 0x%llX",
            f.recovered?"（已恢复）":"",StageText(f.stage),ResourceText(f.event.resource),
            IssueText(f.event.issue),DomainText(f.event.domain),static_cast<unsigned long long>(f.event.code));
    }
    MenuText::Text("%sSR：%s；NR：%s；FG：%s",freshness.fresh?"近期观测：":"历史记录：",
        p.srReady?"就绪":"未就绪",
        s.nr==Mode::Disabled?"已关闭":p.nrReady?"模型已返回":"待观测",
        s.fg==Mode::Disabled?"已关闭":p.fgReady?"已执行":"待观测");
}
inline void Rows(const GenshinSubstage::Snapshot& s,std::uint64_t now) {
    for(std::size_t i=0;i<s.stages.size();++i) {
        const auto stage=static_cast<GenshinSubstage::Stage>(i);
        const auto& e=s.stages[i];
        if(!GenshinSubstage::Applicable(s,stage)) {
            MenuText::TextDisabled("%s：已关闭（可选）",StageText(stage));continue;
        }
        const auto end=e.finished?e.finished:now;
        const auto duration=e.started && end>=e.started?end-e.started:0;
        MenuText::Text("%s：%s  %.1fs",StageText(stage),StateText(e.state),double(duration)/1000.0);
    }
}
}
