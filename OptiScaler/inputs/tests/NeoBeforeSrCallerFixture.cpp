// Compile against a frozen Neo source tree; do not copy/reimplement its query.
#include "ngx/BeforeSr.h"
#include "NeoBeforeSrCallerFixture.h"
#include <cstdio>

extern "C" HRESULT WINAPI Dx11FsrBridge_QueryBeforeSrFrameV1(ID3D11DeviceContext*,
    ID3D11Resource*, ID3D11Resource*, neo::interop::BeforeSrFrameV1*) noexcept;

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV FixtureEvaluate(
    neo::ngx::F6EvaluateV1 evaluate, ID3D11DeviceContext* context, const NVSDK_NGX_Handle* handle,
    NVSDK_NGX_Parameter* parameters, std::uint64_t request, neo::ngx::F6EvalReceiptV1* receipt,
    const neo::interop::BeforeSrFrameV1* frame, ID3D11ShaderResourceView* view, NeoCallerCase test)
{
    using namespace neo::interop;
    using namespace neo::ngx;
    BeforeSrInput input { *frame, {frame->color, view, ColorTransfer::srgb} };
    switch (test) {
    case NeoCallerCase::Unknown: input.producer.transfer = ColorTransfer::unknown; break;
    case NeoCallerCase::Linear: input.producer.transfer = ColorTransfer::linear; break;
    case NeoCallerCase::Pq: input.producer.transfer = ColorTransfer::pq; break;
    case NeoCallerCase::WrongView: input.producer.texture = frame->depth; break;
    case NeoCallerCase::WrongColor: input.frame.color = frame->depth; break;
    case NeoCallerCase::WrongExtent: --input.frame.render_width; break;
    default: break;
    }
    NVSDK_NGX_Result result = NVSDK_NGX_Result_FAIL_InvalidParameter;
    {
        BeforeSrQueryScope scope(input);
        if (test != NeoCallerCase::ClosedScope)
            result = evaluate(context, handle, parameters, nullptr, request, receipt);
    }
    if (test == NeoCallerCase::ClosedScope)
        result = evaluate(context, handle, parameters, nullptr, request, receipt);
    BeforeSrFrameV1 outside;
    if (Dx11FsrBridge_QueryBeforeSrFrameV1(context, frame->color, frame->depth, &outside) != S_FALSE)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    std::printf("neo caller: case=%u request=%llu scope_closed=1 result=%u\n",
        static_cast<unsigned>(test), static_cast<unsigned long long>(request), static_cast<unsigned>(result));
    return result;
}
