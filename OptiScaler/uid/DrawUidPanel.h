// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "DrawUid.h"
#include <menu/UiLocalization.h>

namespace DrawUid {
inline const char* ReasonText(Reason reason) noexcept {
    switch(reason){
    case Reason::None:return "尚未请求。";
    case Reason::WrongGame:return "当前仅支持原神 D3D11 的显式 draw 请求。";
    case Reason::ExplicitTargetRequired:return "未知：需要真实目标的 PS、VS 身份；不自动寻找或猜测 UID。";
    case Reason::InvalidRange:return "未就绪：精确 draw 参数或缓冲区范围无效。";
    case Reason::AwaitingPresent:return "请求已排队，等待现有 Present owner。";
    case Reason::AwaitingExactDraw:return "等待精确 draw；最多 120 次 Present 或 10 秒，不循环采集。";
    case Reason::NoMatch:return "未知：有界窗口内没有精确命中。";
    case Reason::SourceIdentityUnavailable:return "未知：实际着色器尚无共享创建身份，不补扫或猜签名。";
    case Reason::MissingBindings:return "未就绪：实际 owner、VB、IB、layout 或所选 CB 缺失。";
    case Reason::UnsupportedBinding:return "未知：当前绑定不支持安全的请求范围捕获。";
    case Reason::Ambiguous:return "未知：同帧多次精确命中，已撤销候选结果。";
    case Reason::DrawNotOriginal:return "未知：原 draw 未正常返回或配对不完整，候选已撤销。";
    case Reason::OwnerChanged:return "未知：context / owner 已改变，旧结果已撤销。";
    case Reason::Cancelled:return "已关闭；内存结果已清除，已提交资源按真实完成状态收尾。";
    case Reason::CpuContention:return "未知：请求期间回调竞争，不能证明唯一命中。";
    case Reason::Deadline:return "有界请求已超时；未确认 GPU 完成的资源仍保留。";
    case Reason::GpuPending:return "已捕获目标范围，等待后续 Present 的非阻塞完成查询。";
    case Reason::ReadbackFailed:return "未知：非阻塞读取失败；未确认完成的资源继续保留。";
    case Reason::GlyphContractMissing:return "UID 未知：候选原始字节已在内存中，但真实 UID draw / 顶点布局 / glyph→数字映射尚未证实，数字解码未实现。";
    }
    return "UID 未知。";
}
inline void RenderPanel() {
    MenuText::SeparatorText("UID与授权");
    MenuText::TextWrapped("UID 数字解码未实现；下方仅提供显式请求的一次 draw 候选捕获，结果只在内存中消费。不会通过注册表、缓存、网络或截图 OCR 获取 UID，不记录或保存 UID 明文。");
    MenuText::TextDisabled("授权状态：未验证（授权不可用）");
    MenuText::TextWrapped("尚未接入真实授权验证器，无法确认或授予付费权限。draw 命中、候选字节与 UID 请求均不代表当前账号验证或授权成功。");
    ImGui::Spacing();
    if(!MenuText::CollapsingHeader("Draw UID（按需、仅内存）##draw-uid"))return;
    auto status=Inspect();bool enabled=status.enabled;
    if(MenuText::Checkbox("允许按需获取 UID##draw-uid",&enabled))Enable(enabled);
    MenuText::TextWrapped("UID 数字解码尚未实现。需要已证实的游戏字形语义；本页不会将 draw 命中、颜色或纹理当作 UID。默认关闭，不自动后台采集，不保存账号数据。");
    static Selection selected{};
    MenuText::InputScalar("目标 PS 身份（16进制）##draw-uid",ImGuiDataType_U64,&selected.pixelShader,nullptr,nullptr,"%016llX",ImGuiInputTextFlags_CharsHexadecimal);
    MenuText::InputScalar("目标 VS 身份（16进制）##draw-uid",ImGuiDataType_U64,&selected.vertexShader,nullptr,nullptr,"%016llX",ImGuiInputTextFlags_CharsHexadecimal);
    MenuText::InputScalar("精确索引数##draw-uid",ImGuiDataType_U32,&selected.indexCount);
    MenuText::InputScalar("起始索引##draw-uid",ImGuiDataType_U32,&selected.startIndex);
    MenuText::InputScalar("基础顶点##draw-uid",ImGuiDataType_S32,&selected.baseVertex);
    MenuText::InputScalar("VB 槽##draw-uid",ImGuiDataType_U32,&selected.vertexSlot);
    MenuText::InputScalar("VB 字节偏移（相对绑定）##draw-uid",ImGuiDataType_U32,&selected.vertexByteOffset);
    MenuText::InputScalar("VB 请求字节数（1..4096）##draw-uid",ImGuiDataType_U32,&selected.vertexBytes);
    if(MenuText::TreeNode("可选完整 CB 范围##draw-uid")){
        MenuText::Checkbox("使用 PS 常量；否则为 VS##draw-uid",&selected.pixelConstants);
        MenuText::InputScalar("CB 槽##draw-uid",ImGuiDataType_U32,&selected.constantSlot);
        MenuText::InputScalar("CB 完整字节数（0 表示关闭）##draw-uid",ImGuiDataType_U32,&selected.constantBytes);
        MenuText::TextWrapped("CB 只接受完整缓冲区；D3D11.1 非零范围绑定不猜偏移。VB 范围由明确输入限定，不从 indexCount 猜顶点范围。");
        ImGui::TreePop();
    }
    ImGui::BeginDisabled(!enabled || status.pendingGpu || status.phase==Phase::Requested || status.phase==Phase::Armed || status.phase==Phase::Quarantined);
    if(MenuText::Button("请求获取 UID / 一次候选捕获##draw-uid"))Request(selected);
    ImGui::EndDisabled();
    status=Inspect();
    MenuText::TextWrapped("%s",ReasonText(status.reason));
    MenuText::Text("UID：未知");
    if(status.request){
        MenuText::Text("请求 %llu；精确命中 %u；Present %u",static_cast<unsigned long long>(status.request),status.matchingDraws,status.presents);
        MenuText::Text("内存候选 VB %u / IB %u / CB %u 字节；stride %u",status.vertexBytes,status.indexBytes,status.constantBytes,status.stride);
    }
    if(status.phase==Phase::Quarantined)
        MenuText::TextWrapped("GPU 完成状态未知：保留唯一未完成 owner，不再排入新请求。退出游戏后重新启动方可重试。");
}
} // namespace DrawUid
