#pragma once
#include <nvsdk_ngx.h>
#include "../F6EvalReceiptV1.h"

struct ID3D11ShaderResourceView;
namespace neo::interop { struct BeforeSrFrameV1; }

// Test-only caller DLL, compiled with Neo's unmodified production BeforeSr.cpp.
// These controls never enter either product DLL or a game configuration.
enum class NeoCallerCase : unsigned { Sdr, Unknown, Linear, Pq, WrongView, WrongColor, WrongExtent, ClosedScope };
using NeoCallerEvaluate = NVSDK_NGX_Result (NVSDK_CONV *)(neo::ngx::F6EvaluateV1,
    ID3D11DeviceContext*, const NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*, std::uint64_t,
    neo::ngx::F6EvalReceiptV1*, const neo::interop::BeforeSrFrameV1*, ID3D11ShaderResourceView*, NeoCallerCase);
