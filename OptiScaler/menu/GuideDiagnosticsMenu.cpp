// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "GuideDiagnosticsMenu.h"
#include <Config.h>
#include <Util.h>
#include <runtime/GuideCapture.h>
#include <runtime/GuideDiagnostics.h>
#include <runtime/GuideCalibrationRuntime.h>
#include <runtime/GuidePairExport.h>
#include <menu/UiLocalization.h>
#include <shaders/dlssnr/DlssNr_HistoryDebug.h>
#include <imgui/imgui.h>
#include <bcrypt.h>
#include <json.hpp>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#pragma comment(lib,"bcrypt.lib")
#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif

namespace GuideDiagnosticsMenu {
namespace {
bool frozen=false,sessionPreviewOff=false;
bool historyWindowOpen=false,historyTableFrozen=false;
unsigned historyView=0;
GuideCapture::Stream StreamFor(unsigned mode) {
    if(mode==1)return GuideCapture::Stream::Source;
    if(mode>=2&&mode<=4)return GuideCapture::Stream::NrModel;
    if(mode>=5&&mode<=7)return GuideCapture::Stream::Fg;
    if(mode==8)return GuideCapture::Stream::History;
    return GuideCapture::Stream::Count;
}
void SyncDemand(Config* c) {
    if(!c)return;
    GuideCapture::PreviewDemand d;
    const auto mode=sessionPreviewOff?0u:std::min(c->DlssNrGuidePreview.value_or_default(),8u);
    d.mode=static_cast<GuideCapture::PreviewMode>(mode);
    d.raw=c->DlssNrGuidePreviewRaw.value_or_default();
    d.flipY=c->DlssNrGuidePreviewFlipY.value_or_default();
    d.gain=c->DlssNrGuidePreviewMotionGain.value_or_default();d.frozen=frozen;
    d.historyView=historyView;
    if(mode)GuideDiagnostics::OnlyGenerated().store(false,std::memory_order_release);
    GuideCapture::SetPreviewDemand(d);
}
std::string DiskSha256(const std::filesystem::path& path){
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    struct Cleanup{BCRYPT_ALG_HANDLE& a;BCRYPT_HASH_HANDLE& h;~Cleanup(){if(h)BCryptDestroyHash(h);if(a)BCryptCloseAlgorithmProvider(a,0);}} cleanup{algorithm,hash};
    DWORD length=0,bytes=0;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0||
       BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&length),sizeof(length),&bytes,0)<0)return {};
    std::vector<unsigned char> object(length);
    if(BCryptCreateHash(algorithm,&hash,object.data(),length,nullptr,0,0)<0)return {};
    std::ifstream file(path,std::ios::binary);if(!file)return {};
    std::array<unsigned char,65536> block{};
    while(file){file.read(reinterpret_cast<char*>(block.data()),block.size());const auto got=file.gcount();
        if(got>0&&BCryptHashData(hash,block.data(),static_cast<ULONG>(got),0)<0)return {};}
    if(!file.eof())return {};
    std::array<unsigned char,32> digest{};if(BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)<0)return {};
    std::ostringstream out;out<<std::hex<<std::uppercase<<std::setfill('0');for(auto c:digest)out<<std::setw(2)<<unsigned(c);return out.str();
}

struct ExportState {
    std::atomic<bool> busy{false};
    std::mutex mutex;
    std::string result;
};
ExportState& Exports(){static auto* state=new ExportState;return *state;}
struct ExportJob {
    GuideCapture::Stream stream;
    std::filesystem::path dll,output;
    GuidePairExport::ExportIdentity identity;
    HMODULE module=nullptr;
    bool pair=true,viewKnown=false;
    GuideDiagnostics::View view;
};
nlohmann::ordered_json FactsJson(const FrameTrace::Facts& f) {
    nlohmann::ordered_json j;
    j["kind"]=static_cast<unsigned>(f.kind);j["kindName"]=FrameTrace::KindName(f.kind);
    j["api"]=static_cast<unsigned>(f.api);
#define F6_FACT(name) j[#name]=f.name
    F6_FACT(evaluateOrdinal);F6_FACT(apiFeature);F6_FACT(epoch);F6_FACT(feature);F6_FACT(configuration);F6_FACT(owner);
    F6_FACT(context);F6_FACT(colour);F6_FACT(depth);F6_FACT(motion);F6_FACT(queue);F6_FACT(fenceValue);
    F6_FACT(token);F6_FACT(tokenKnown);F6_FACT(querySequence);F6_FACT(queryBeginTick);F6_FACT(queryEndTick);
    F6_FACT(presentedInterval);F6_FACT(sdkStatus);F6_FACT(queryIntervalKnown);
    F6_FACT(scaleX);F6_FACT(scaleY);F6_FACT(jitterX);F6_FACT(jitterY);F6_FACT(constantsKnown);
    F6_FACT(depthInverted);F6_FACT(reset);F6_FACT(nrResult);F6_FACT(nrReason);F6_FACT(nrOutcomeKnown);
    F6_FACT(pixelPhaseVerified);F6_FACT(generatedIdentityKnown);F6_FACT(displayIdentityKnown);
#undef F6_FACT
    return j;
}
nlohmann::ordered_json SnapshotJson(const GuideDiagnostics::Snapshot& v) {
    nlohmann::ordered_json j={{"source",v.source},{"epoch",v.epoch},{"feature",v.feature},
        {"owner",v.owner},{"configuration",v.configuration},{"tick",v.tick},
        {"stage",GuideDiagnostics::StageName(v.stage)},{"reason",v.reason}};
#define F6_SNAP(name) j[#name]=v.name
    F6_SNAP(sdkQueryObserved);F6_SNAP(sdkQueryResult);F6_SNAP(sdkQuerySequence);F6_SNAP(sdkPreviousQueryTick);
    F6_SNAP(sdkQueryTick);F6_SNAP(sdkQueryBeginNs);F6_SNAP(sdkQueryEndNs);F6_SNAP(sdkPreviousQueryEndNs);
    F6_SNAP(sdkPresentedInterval);F6_SNAP(sdkIntervalKnown);F6_SNAP(sdkStatus);F6_SNAP(sdkStatusKnown);
    F6_SNAP(sdkOptionsKnown);F6_SNAP(sdkOptionsCalled);F6_SNAP(sdkOptionsResult);F6_SNAP(sdkOptionsMode);
    F6_SNAP(scaleX);F6_SNAP(scaleY);F6_SNAP(jitterX);F6_SNAP(jitterY);
    F6_SNAP(generatedIdentityKnown);F6_SNAP(phaseVerified);
#undef F6_SNAP
    return j;
}
void WriteSnapshot(const ExportJob& job,const std::filesystem::path& path) {
    nlohmann::ordered_json report={{"schema","f6.guide-trace.v2"},{"pid",job.identity.pid},
        {"diskDllSHA",job.identity.diskDllSHA},{"loadedMemoryKnown",false},{"loadedMemorySHA",nullptr},
        {"observationOnly",true},{"traceSnapshotKnown",job.viewKnown},
        {"droppedEvents",job.view.droppedEvents},{"revision",job.view.revision}};
    for(const auto& lane:job.view.lanes)report["consumers"].push_back({
        {"observations",lane.observations},{"latest",SnapshotJson(lane.latest)},{"completed",SnapshotJson(lane.completed)}});
    auto events=job.view.events;
    std::sort(events.begin(),events.end(),[](const auto& a,const auto& b){return a.sequence<b.sequence;});
    for(const auto& e:events)if(e.sequence)report["events"].push_back({
        {"sequence",e.sequence},{"tick",e.tick},{"tickNs",e.tickNs},{"source",e.source},{"sourceTick",e.sourceTick},
        {"picture",e.picture},{"providerFrame",e.providerFrame},{"consumer",static_cast<unsigned>(e.consumer)},
        {"stage",GuideDiagnostics::StageName(e.stage)},{"reason",e.reason},{"resultKnown",e.resultKnown},
        {"apiResult",e.apiResult},{"presentReturned",e.presentReturned},{"facts",FactsJson(e.facts)}});
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path,std::ios::binary);out<<report.dump(2);out.flush();
    if(!out)throw std::runtime_error("snapshot write failed");
}
DWORD WINAPI PairWorker(void* context) noexcept {
    std::unique_ptr<ExportJob> job(static_cast<ExportJob*>(context));
    const auto module=job->module;
    std::string result="Pair export failed";
    try {
        const auto deadline=GuideDiagnostics::Now()+15000;
        std::vector<GuideCapture::Image> images;
        while(job->pair&&GuideDiagnostics::Now()<deadline){
            images=GuideCapture::DrainPairs(job->stream);
            if(!images.empty())break;
            Sleep(20); // this explicit offline worker never blocks a source/render callback
        }
        job->identity.diskDllSHA=DiskSha256(job->dll);
        auto tracePath=job->output;tracePath.replace_extension(".trace.json");
        WriteSnapshot(*job,tracePath);
        if(!job->pair)result="Snapshot exported: "+tracePath.string();
        else if(images.size()!=2)result="Pair unavailable: two adjacent completed source frames were not observed; trace exported";
        else {
            job->identity.diskDllSHA=DiskSha256(job->dll);
            SYSTEMTIME utc{};GetSystemTime(&utc);
            std::ostringstream time;time<<std::setfill('0')<<std::setw(4)<<utc.wYear<<'-'
                <<std::setw(2)<<utc.wMonth<<'-'<<std::setw(2)<<utc.wDay<<'T'
                <<std::setw(2)<<utc.wHour<<':'<<std::setw(2)<<utc.wMinute<<':'<<std::setw(2)<<utc.wSecond<<'Z';
            job->identity.capturedUtc=time.str();
            if(const char* why=GuidePairExport::Validate(images,job->identity))result=std::string("Pair unavailable: ")+why;
            else {
                std::filesystem::create_directories(job->output.parent_path());
                std::ofstream out(job->output,std::ios::binary);
                if(out&&GuidePairExport::Write(out,images,job->identity)){
                    out.flush();result=out?"Pair exported: "+job->output.string():"Pair export write failed";
                }else result="Pair export write failed";
            }
        }
    }catch(const std::exception& e){result=std::string("Pair export failed: ")+e.what();}
    catch(...){result="Pair export failed";}
    auto& state=Exports();
    {std::lock_guard lock(state.mutex);state.result=std::move(result);}
    state.busy.store(false,std::memory_order_release);
    job.reset();
    if(module)FreeLibraryAndExitThread(module,0);
    return 0;
}
void ExportPair(unsigned mode,bool pair=true) {
    auto& state=Exports();bool expected=false;
    if(!state.busy.compare_exchange_strong(expected,true,std::memory_order_acq_rel))return;
    auto job=std::make_unique<ExportJob>();
    job->stream=StreamFor(mode);job->dll=Util::DllPath();job->pair=pair;
    job->viewKnown=GuideDiagnostics::Read(job->view);
    job->identity.pid=GetCurrentProcessId();
    GuideCapture::PreviewSnapshot displayed;
    job->identity.configuration=GuideCapture::ReadDisplayed(displayed)?std::to_string(displayed.configuration):"unobserved";
    job->output=job->dll.parent_path()/"diagnostics"/
        ("guide-pair-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GuideDiagnostics::Now())+".json");
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(&PairWorker),&job->module)){
        state.busy.store(false);return;}
    if(pair&&!GuideCapture::RequestPair(job->stream)){FreeLibrary(job->module);state.busy.store(false);return;}
    {std::lock_guard lock(state.mutex);state.result=pair?"Waiting for two adjacent actual source frames (15 s maximum)":"Exporting immutable event snapshot";}
    auto* context=job.release();const auto thread=CreateThread(nullptr,0,PairWorker,context,0,nullptr);
    if(thread)CloseHandle(thread);else{
        std::unique_ptr<ExportJob> failed(context);FreeLibrary(failed->module);state.busy.store(false);}
}
void SnapshotStatus() {
    GuideCapture::PreviewSnapshot displayed;
    const bool have=GuideCapture::ReadDisplayed(displayed);
    // Fixed three-line region: identity always belongs to the displayed snapshot.
    ImGui::BeginChild("GuideDisplayedStatus",ImVec2(0,ImGui::GetTextLineHeightWithSpacing()*3.3f),false);
    if(have){
        const auto now=GuideDiagnostics::Now();
        MenuText::Text("显示源帧 %llu／代次 %llu／快照 %llu",
            displayed.source,displayed.epoch,displayed.serial);
        MenuText::Text("feature %llu／配置 %llu／owner %llu",
            displayed.feature,displayed.configuration,displayed.owner);
        MenuText::Text("%u × %u／源帧年龄 %llu ms／%s",
            displayed.width,displayed.height,now>=displayed.tick?now-displayed.tick:0,frozen?"已冻结":"实时预览");
    }else{
        MenuText::TextUnformatted("显示源帧／代次：尚不可用");
        MenuText::TextUnformatted("显示 feature／配置／owner：尚不可用");
        MenuText::TextUnformatted("等待所选 GPU 快照完成、Reset 并提交显示");
    }
    ImGui::EndChild();
}
}
void Visibility(bool visible) noexcept {
    if(!visible){NrHistoryDebug::Request(false);GuideCapture::RequestHistoryMeasurement(false);}
    GuideDiagnostics::SetConsumerObservationDemand(visible);
    try {
        if(GuideCalibrationRuntime::Active())GuideCalibrationRuntime::Stop();
        if(!visible)GuideDiagnostics::OnlyGenerated().store(false,std::memory_order_release);
        SyncDemand(Config::Instance()); // menu closure does not stop the GPU preview
    }catch(...){}
}
// Shared by the ordinary settings page and the per-source inspector. These
// values drive mode 24 directly; none of the legacy interval-reuse knobs leak in.
void RenderHistoryTuning(Config* config) {
    if (!config) return;
    MenuText::SeparatorText("每源帧持续保留：可信范围");
    float depth=config->DlssNrPresentedTemporalDepthTolerance.value_or_default();
    if(MenuText::SliderFloat("持续保留：深度容差",&depth,0.0f,1.0f,"%.3f"))config->DlssNrPresentedTemporalDepthTolerance=depth;
    float colour=config->DlssNrPresentedTemporalColourTolerance.value_or_default();
    if(MenuText::SliderFloat("持续保留：颜色容差",&colour,0.005f,0.5f,"%.3f"))config->DlssNrPresentedTemporalColourTolerance=colour;
    float motion=config->DlssNrPresentedTemporalMotionTolerance.value_or_default();
    if(MenuText::SliderFloat("持续保留：运动容差",&motion,0.05f,32.0f,"%.2f 工作像素"))config->DlssNrPresentedTemporalMotionTolerance=motion;
    MenuText::TextWrapped("调高可减少拒绝：深度 1 忽略像素深度差；颜色控制原画与出生纹理差异；运动控制局部运动场差异。与间隔复用的旧调试参数独立。");
    MenuText::SeparatorText("最终历史稳定（NR 噪点与光照跳变）");
    int stability=int(std::min(config->DlssNrPresentedTemporalStabilityMs.value_or_default(),2000u));
    if(MenuText::SliderInt("最终历史稳定时间",&stability,0,2000,"%d ms"))config->DlssNrPresentedTemporalStabilityMs=unsigned(stability);
    float floor=config->DlssNrPresentedTemporalStabilityFloor.value_or_default();
    if(MenuText::SliderFloat("低可信区域稳定比例",&floor,0.0f,1.0f,"%.2f"))config->DlssNrPresentedTemporalStabilityFloor=floor;
    float limit=config->DlssNrPresentedTemporalStabilityClamp.value_or_default();
    if(MenuText::SliderFloat("稳定变化容差",&limit,0.001f,0.5f,"%.3f"))config->DlssNrPresentedTemporalStabilityClamp=limit;
    MenuText::TextWrapped("0 ms 关闭；可先试 100～250 ms。仅平滑 NR 增加的变化，游戏原画保持当前帧。深度或运动不可信时仍按上面的比例稳定；越高越稳定，也可能拖尾。时间是衰减常数与平均滞后上限，不是旧整帧冻结时长。切场景或源帧断链会重置。");
    if(MenuText::TreeNode("持续保留高级参数")) {
        float noise=config->DlssNrPresentedTemporalNoiseFloor.value_or_default();
        if(MenuText::SliderFloat("持续保留：噪声下限",&noise,0.0f,0.1f,"%.3f"))config->DlssNrPresentedTemporalNoiseFloor=noise;
        ImGui::TreePop();
    }
}
void OpenHistoryWindow() noexcept {historyWindowOpen=true;}
void RenderHistoryWindow(Config* config) {
    if(!historyWindowOpen||!config){NrHistoryDebug::Request(false);GuideCapture::RequestHistoryMeasurement(false);return;}
    NrHistoryDebug::Request(true);
    ImGui::SetNextWindowSize(ImVec2(860,570),ImGuiCond_FirstUseEver);
    if(!ImGui::Begin("NR 每帧细节保留调试###NrHistoryPerFrame",&historyWindowOpen))
    {NrHistoryDebug::Request(false);GuideCapture::RequestHistoryMeasurement(false);ImGui::End();return;}
    static NrHistoryDebug::View view;
    GuideCapture::RequestHistoryMeasurement(config->DlssNrPresentedTemporalDetail.value_or_default());
    if(!historyTableFrozen)NrHistoryDebug::Read(view);
    MenuText::TextWrapped("每个真实源帧记录准入和实际提交/完成状态。调节下方参数直接生效；图像只读观察，按需最多 30 Hz，忙时跳过。");
    if(MenuText::TreeNode("调节历史参数（与 NR 设置同步）")) {
        ImGui::BeginDisabled(!config->DlssNrPresentedTemporalDetail.value_or_default());
        RenderHistoryTuning(config);
        ImGui::EndDisabled();
        ImGui::TreePop();
    }
    MenuText::Checkbox("冻结帧记录（图像冻结用原预览条）",&historyTableFrozen);
    constexpr const char* images[]{"关闭","实际贡献","拒绝原因","历史年龄","重投影出生颜色","当前 NR（模型域）","融合结果（模型域）","最终稳定权重"};
    int image=config->DlssNrGuidePreview.value_or_default()==8&&!sessionPreviewOff?int(historyView+1):0;
    if(MenuText::Segmented("细节保留调试图",&image,images,IM_ARRAYSIZE(images))) {
        sessionPreviewOff=false;
        config->DlssNrGuidePreview=image?8u:0u;
        if(image)GuideDiagnostics::OnlyGenerated().store(false,std::memory_order_release);
        historyView=image?unsigned(image-1):0u;
        SyncDemand(config);
    }
    MenuText::TextWrapped("拒绝图：绿=模型域非零增量；黄绿=低可信稳定生效；蓝=当前细节更强/无增量；灰=无相邻历史；黄=过期；橙=深度；紫=运动；红=越界；青=出生颜色/结构不匹配；暗红=无效输入。");
    SnapshotStatus();
    const auto now=NrHistoryDebug::Now();
    if(view.count) {
        const auto& f=view.frames[(view.next+view.frames.size()-1)%view.frames.size()];
        MenuText::Text("显示源 %llu／历史源 %llu／配置 %llu／%llu ms 前",f.source,f.historySource,f.configuration,now>=f.tick?now-f.tick:0);
        MenuText::Text("深度 %s %u×%u／运动 %s %u×%u／工作 %u×%u／相位 %s",
            f.depth?"收到":"缺失",f.depthWidth,f.depthHeight,f.motion?"收到":"缺失",f.motionWidth,f.motionHeight,
            f.width,f.height,f.phase?"验证":"未验证");
        MenuText::Text("MV scale %.6g, %.6g／MV phase %.6g, %.6g／Z phase %.6g, %.6g",
            f.scaleX,f.scaleY,f.motionPhaseX,f.motionPhaseY,f.depthPhaseX,f.depthPhaseY);
        MenuText::Text("源间隔 %.2f ms／衰减 %.2f s／最长 %.2f s／强度 %.3f",f.delta*1000,f.duration,f.maxAge,f.strength);
        MenuText::Text("实际阈值：深度 %.3f／颜色 %.3f／运动 %.2f px／稳定 %.0f ms／低可信 %.2f／变化 %.3f",
            f.depthTolerance,f.colourTolerance,f.motionTolerance,f.stabilitySeconds*1000,f.stabilityFloor,f.stabilityClamp);
        static GuideCapture::ContributionSample measured;
        GuideCapture::ReadHistoryMeasurement(measured);
        const auto sampleNow=GuideDiagnostics::Now();
        if(measured.Matches(f.owner,f.configuration,f.source,sampleNow))
            MenuText::Text("已完成贡献采样：%.2f%% 采样点／源 %llu／%llu ms 前（独立异步采样，非每帧读回）",
                measured.changed*100,measured.source,sampleNow-measured.tick);
        else MenuText::TextUnformatted("已完成贡献采样：等待本配置结果；录制/准入不代表实际贡献。");
    }else MenuText::TextUnformatted("等待当前源帧；NR 或持续保留关闭时不会伪造历史。");
    if(ImGui::BeginTable("NrHistoryFrames",8,ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollY,ImVec2(0,0))) {
        constexpr const char* columns[]{"源帧","历史源","准入/历史","录制","提交","GPU完成","Reset","原因"};
        for(auto name:columns)ImGui::TableSetupColumn(name);
        ImGui::TableSetupScrollFreeze(0,1);ImGui::TableHeadersRow();
        for(unsigned n=0;n<view.count;++n) {
            const auto& f=view.frames[(view.next+view.frames.size()-1-n)%view.frames.size()];
            ImGui::TableNextRow();ImGui::TableNextColumn();MenuText::Text("%llu",f.source);
            ImGui::TableNextColumn();MenuText::Text("%llu",f.historySource);
            ImGui::TableNextColumn();MenuText::Text("%s / %s",f.admitted?"是":"否",f.history?"可用":"不可用");
            ImGui::TableNextColumn();MenuText::TextUnformatted(f.recorded?"是":"否");
            ImGui::TableNextColumn();MenuText::TextUnformatted(f.submitted?"是":"未证明");
            ImGui::TableNextColumn();MenuText::TextUnformatted(f.completed?"是":"未证明");
            ImGui::TableNextColumn();MenuText::TextUnformatted(f.reset?"已闭合":"未闭合");
            ImGui::TableNextColumn();MenuText::TextUnformatted(f.quarantined?"quarantined":f.reason);
        }
        ImGui::EndTable();
    }
    ImGui::End();
    if(!historyWindowOpen){NrHistoryDebug::Request(false);GuideCapture::RequestHistoryMeasurement(false);}
}
void Render(Config*) {
    MenuText::SeparatorText("深度与运动诊断");
    static GuideDiagnostics::View view;
    static uint64_t refreshed=0;
    const auto now=GuideDiagnostics::Now();
    if(!refreshed||now-refreshed>=250){if(GuideDiagnostics::Read(view))refreshed=now;}
    constexpr const char* consumers[]{"NR 模型","细节持续反馈","FG"};
    constexpr const char* stages[]{"未知","收到","拒绝","已录制","已提交","GPU 完成"};
    if(ImGui::BeginTable("GuideConsumerStates",3,ImGuiTableFlags_BordersInnerV)){
        for(unsigned n=0;n<3;++n){const auto& lane=view.lanes[n];const auto& s=lane.latest;
            ImGui::TableNextRow();ImGui::TableNextColumn();
            MenuText::Text("%s：%s%s",consumers[n],stages[static_cast<unsigned>(s.stage)],
                GuideDiagnostics::Stale(s,now)?"（陈旧／未观测）":"");
            ImGui::TableNextColumn();
            MenuText::Text("源 %llu；深度 %s／运动 %s；准入 %s",s.source,
                s.depth.present?"收到":"缺失",s.motion.present?"收到":"缺失",s.accepted?"接受":"未接受");
            ImGui::TableNextColumn();
            if(lane.completed.tick)MenuText::Text("最近完成 %llu；年龄 %llu ms",lane.completed.source,
                GuideDiagnostics::Age(lane.completed,now));
            else MenuText::TextUnformatted("最近完成：未知");
        }
        ImGui::EndTable();
    }
    if(ImGui::TreeNode("消费者详情与拒绝原因")){
        for(unsigned n=0;n<3;++n){ImGui::PushID(static_cast<int>(n));const auto& lane=view.lanes[n];const auto& s=lane.latest;
            MenuText::SeparatorText(consumers[n]);MenuText::TextWrapped("原因：%s",s.reason.c_str());
            MenuText::Text("源 %llu；代次 %llu；配置 %llu；相位 %s",s.source,s.epoch,s.configuration,s.phaseVerified?"已验证":"未知");
            MenuText::Text("深度行翻转 %d／运动行翻转 %d／运动 Y 取反 %d；SDK scale %.7g, %.7g；jitter %.7g, %.7g",
                s.depthFlip,s.motionFlip,s.yNegated,s.scaleX,s.scaleY,s.jitterX,s.jitterY);
            if(n==0)MenuText::Text("模型实际成功：%s",s.modelSuccess?"是":"未证明");
            if(n==1){MenuText::Text("历史源 %llu；可用 %d；融合已录制 %d；重置：%s",
                s.historySource,s.historyAvailable,s.fusionRecorded,s.resetReason.c_str());
                const auto& c=lane.completed;
                if(c.contributionKnown)MenuText::Text("源 %llu 的实际非零贡献比例 %.3f%%／有效覆盖 %.3f%%",
                    c.source,c.contribution*100.f,c.coverage*100.f);
                else MenuText::TextUnformatted("实际历史贡献：未知（录制融合不等于非零贡献）");
            }
            if(n==2){MenuText::Text("SDK 配置确认 %d；查询 result %d／status %u；生成帧身份：未知",
                s.sdkOptionsKnown,s.sdkQueryResult,s.sdkStatus);
                MenuText::TextUnformatted(s.uiProtected?"UI 已提供保护输入":"UI 未保护");
            }
            if(ImGui::BeginTable("GuideFields",3,ImGuiTableFlags_BordersInnerV)){
                for(unsigned f=0;f<static_cast<unsigned>(GuideDiagnostics::Field::Count);++f){
                    const auto& v=s.fields[f];ImGui::TableNextRow();ImGui::TableNextColumn();
                    MenuText::TextUnformatted(GuideDiagnostics::FieldNames[f]);ImGui::TableNextColumn();
                    if(v.valid)MenuText::Text("%.9g",v.value);else MenuText::TextUnformatted("未知");
                    ImGui::TableNextColumn();MenuText::TextUnformatted(GuideDiagnostics::OriginName(v.origin));
                }
                ImGui::EndTable();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
}
void RenderPreviewStrip(Config* config) {
    if(!config)return;
    ImGui::PushID("UnifiedGuidePreview");
    if(MenuText::Button("打开每帧细节保留调试"))OpenHistoryWindow();
    constexpr const char* names[]={"关闭","NR 编码前颜色","NR 输入颜色","NR 深度","NR 运动",
        "FG 颜色","FG 深度","FG 运动","历史贡献"};
    int mode=sessionPreviewOff?0:static_cast<int>(std::min(config->DlssNrGuidePreview.value_or_default(),8u));
    if(MenuText::Segmented("输入资源预览",&mode,names,IM_ARRAYSIZE(names))){
        sessionPreviewOff=false;config->DlssNrGuidePreview=static_cast<uint32_t>(mode);
        if(mode)GuideDiagnostics::OnlyGenerated().store(false,std::memory_order_release);
    }
    MenuText::Checkbox("冻结最后完成的快照",&frozen);
    bool only=GuideDiagnostics::OnlyGenerated().load(std::memory_order_acquire);
    if(MenuText::Checkbox("临时只看生成帧（关闭菜单恢复）",&only)){
        if(only){sessionPreviewOff=true;mode=0;}
        GuideDiagnostics::OnlyGenerated().store(only,std::memory_order_release);
    }
    if(mode){
        bool raw=config->DlssNrGuidePreviewRaw.value_or_default();
        if(MenuText::Checkbox("原始资源坐标和值",&raw))config->DlssNrGuidePreviewRaw=raw;
        bool flip=config->DlssNrGuidePreviewFlipY.value_or_default();
        if(MenuText::Checkbox("仅预览上下翻转",&flip))config->DlssNrGuidePreviewFlipY=flip;
        if(mode==4||mode==7||mode==8){
            float gain=config->DlssNrGuidePreviewMotionGain.value_or_default();
            if(MenuText::SliderFloat("显示增益",&gain,.01f,4096.f,"%.2f",ImGuiSliderFlags_Logarithmic))
                config->DlssNrGuidePreviewMotionGain=gain;
        }
    }
    SyncDemand(config);
    SnapshotStatus();
    if(ImGui::TreeNode("预览详情／离线双帧")){
        const auto status=GuideCapture::PreviewStatus();
        MenuText::TextWrapped("%s",status.c_str());
        MenuText::TextWrapped("关闭菜单继续全屏预览；选择关闭才停止新增采样。当前不支持 HDR 输入／输出。显示变换不修改 NR／FG 输入。");
        MenuText::TextWrapped("双帧导出仅采集两张连续真实源帧；只有该请求使用 CPU 读回。缺少 guides 或来源身份时保持不可用。");
        ImGui::BeginDisabled(!mode||Exports().busy.load(std::memory_order_acquire));
        if(MenuText::Button("导出一次连续源帧对"))ExportPair(static_cast<unsigned>(mode));
        ImGui::EndDisabled();
        ImGui::BeginDisabled(Exports().busy.load(std::memory_order_acquire));
        if(MenuText::Button("导出当前诊断快照"))ExportPair(static_cast<unsigned>(mode),false);
        ImGui::EndDisabled();
        {std::unique_lock lock(Exports().mutex,std::try_to_lock);if(lock.owns_lock()&&!Exports().result.empty())
            MenuText::TextWrapped("%s",Exports().result.c_str());}
        ImGui::TreePop();
    }
    ImGui::PopID();
}
} // namespace GuideDiagnosticsMenu
