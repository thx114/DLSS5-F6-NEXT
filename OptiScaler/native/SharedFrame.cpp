// Not built with the precompiled header: Direct3D only, so tests/nr_shared_frame_gpu.cpp compiles it alone.
#include "SharedFrame.h"

#include <format>

using Microsoft::WRL::ComPtr;

namespace native
{

DXGI_FORMAT SharedPictureFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return format;
    }
}

bool SharedDepthFormats(DXGI_FORMAT depth, DXGI_FORMAT* typeless, DXGI_FORMAT* view)
{
    switch (depth)
    {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
        *typeless = DXGI_FORMAT_R32_TYPELESS;
        *view = DXGI_FORMAT_R32_FLOAT;
        return true;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        *typeless = DXGI_FORMAT_R32G8X24_TYPELESS;
        *view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        return true;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
        *typeless = DXGI_FORMAT_R24G8_TYPELESS;
        *view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        return true;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:
        *typeless = DXGI_FORMAT_R16_TYPELESS;
        *view = DXGI_FORMAT_R16_UNORM;
        return true;
    default:
        return false;
    }
}

bool CreatePublicConsumerDomain(ID3D12Device* creationEntry, ComPtr<ID3D12Device>& device,
    ComPtr<ID3D12CommandQueue>& queue, std::string& diagnostic)
{
    diagnostic.clear();
    const auto record = [&](const char* step, HRESULT hr, const void* object) {
        diagnostic += std::format("{} hr={:08X} object=0x{:x}; ", step, static_cast<UINT>(hr),
                                  reinterpret_cast<uintptr_t>(object));
    };
    const auto fail = [&](const char* step, HRESULT hr) {
        record(step, hr, nullptr); return false;
    };
    if (!creationEntry || device || queue) return fail("unpublished-inputs-required", E_INVALIDARG);
    ComPtr<ID3D12CommandAllocator> anchor;
    auto hr = creationEntry->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&anchor));
    record("CreateCommandAllocator", hr, anchor.Get());
    if (FAILED(hr) || !anchor) return fail("allocator-create-failed-or-null", FAILED(hr) ? hr : E_POINTER);
    ComPtr<ID3D12Device> owner;
    hr = anchor->GetDevice(IID_PPV_ARGS(&owner));
    record("allocator-publicOwner", hr, owner.Get());
    if (FAILED(hr) || !owner) return fail("allocator-owner-failed-or-null", FAILED(hr) ? hr : E_POINTER);
    ComPtr<IUnknown> identity;
    hr = owner.As(&identity);
    record("publicOwner-IUnknown", hr, identity.Get());
    if (FAILED(hr) || !identity) return fail("owner-identity-failed-or-null", FAILED(hr) ? hr : E_POINTER);

    ComPtr<ID3D12CommandQueue> createdQueue;
    D3D12_COMMAND_QUEUE_DESC desc {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = owner->CreateCommandQueue(&desc, IID_PPV_ARGS(&createdQueue));
    record("publicOwner-CreateCommandQueue", hr, createdQueue.Get());
    if (FAILED(hr) || !createdQueue) return fail("queue-create-failed-or-null", FAILED(hr) ? hr : E_POINTER);
    ComPtr<ID3D12Device> queueOwner;
    hr = createdQueue->GetDevice(IID_PPV_ARGS(&queueOwner));
    record("queue-publicOwner", hr, queueOwner.Get());
    if (FAILED(hr) || !queueOwner) return fail("queue-owner-failed-or-null", FAILED(hr) ? hr : E_POINTER);
    ComPtr<IUnknown> queueIdentity;
    hr = queueOwner.As(&queueIdentity);
    record("queue-owner-IUnknown", hr, queueIdentity.Get());
    if (FAILED(hr) || !queueIdentity) return fail("queue-identity-failed-or-null", FAILED(hr) ? hr : E_POINTER);
    if (identity != queueIdentity || createdQueue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return fail("queue-owner-or-type-mismatch", E_INVALIDARG);
    hr = owner->GetDeviceRemovedReason();
    record("publicOwner-health", hr, owner.Get());
    if (FAILED(hr)) return false;

    // No GPU work has been submitted; the probe allocator can retire now.
    // The newly constructed pair is the only publication, never an alias for
    // the creation entry's previous resources or a different existing queue.
    device = owner; queue = createdQueue;
    return true;
}

bool SharedTexture::Create(ID3D11Device* device11, uint32_t width, uint32_t height, DXGI_FORMAT format, UINT bindFlags)
{
    Reset();

    if (device11 == nullptr)
    {
        _error = "no D3D11 device";
        return false;
    }

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bindFlags;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    HRESULT hr = device11->CreateTexture2D(&desc, nullptr, &_tex11);

    if (FAILED(hr))
    {
        _error = std::format("creating a shared texture {}x{} format {}: {:X}", width, height, (int) format, (UINT) hr);
        _tex11.Reset();
        return false;
    }

    ComPtr<IDXGIResource1> dxgi;
    hr = _tex11.As(&dxgi);

    if (SUCCEEDED(hr))
        hr = dxgi->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &_handle);

    if (FAILED(hr) || _handle == nullptr)
    {
        _error = std::format("making the shared handle: {:X}", (UINT) hr);
        Reset();
        return false;
    }

    _width = width;
    _height = height;
    _format = format;
    return true;
}

bool SharedTexture::Open(ID3D12Device* device12, bool requireExactOwner)
{
    if (_res12 != nullptr && _opened == device12 && (!requireExactOwner || _ownerVerified))
        return true;

    if (_handle == nullptr || device12 == nullptr)
        return false;

    _res12.Reset();
    _opened = nullptr;
    _ownerVerified = false;
    const HRESULT hr = device12->OpenSharedHandle(_handle, IID_PPV_ARGS(&_res12));

    if (FAILED(hr) || !_res12)
    {
        _error = std::format("opening the shared texture on D3D12: {:X}", (UINT) hr);
        _res12.Reset();
        return false;
    }

    if (requireExactOwner)
    {
        ComPtr<ID3D12Device> owner;
        ComPtr<IUnknown> expectedIdentity, ownerIdentity;
        const auto ownerHr = _res12->GetDevice(IID_PPV_ARGS(&owner));
        if (FAILED(ownerHr) || !owner || FAILED(device12->QueryInterface(IID_PPV_ARGS(&expectedIdentity))) ||
            !expectedIdentity || FAILED(owner.As(&ownerIdentity)) || !ownerIdentity || expectedIdentity != ownerIdentity)
        {
            _error = "shared texture consumer-owner-mismatch or unknown public owner";
            _res12.Reset();
            return false;
        }
        _ownerVerified = true;
    }
    _opened = device12;
    return true;
}

void SharedTexture::Reset()
{
    _res12.Reset();
    _tex11.Reset();

    if (_handle != nullptr)
    {
        CloseHandle(_handle);
        _handle = nullptr;
    }

    _opened = nullptr;
    _ownerVerified = false;
    _width = _height = 0;
    _format = DXGI_FORMAT_UNKNOWN;
}

bool SharedFence::Create(ID3D12Device* device12, ID3D11Device* device11)
{
    Reset();

    // Only this handle's imports may represent the timeline. There is no second
    // CreateFence, copied counter, private unwrap or synthetic owner interface.
    struct SharedHandle
    {
        HANDLE value = nullptr;
        ~SharedHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    } handle;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D12Fence> created, consumer;
    ComPtr<ID3D11Fence> fence11;
    ComPtr<ID3D12Device> publicOwner, consumerOwner;
    ComPtr<IUnknown> creatorIdentity, publicOwnerIdentity, consumerOwnerIdentity;
    const auto record = [&](const char* step, HRESULT hr, const void* object) {
        _diagnostic += std::format("{} hr={:08X} object=0x{:x}; ", step, static_cast<UINT>(hr),
                                   reinterpret_cast<uintptr_t>(object));
    };
    const auto fail = [&](const char* step, HRESULT hr) {
        _error = std::format("shared fence {}: {:08X}", step, static_cast<UINT>(hr));
        return false; // locals release on every failure; members stay unpublished
    };
    const auto owner = [&](const char* step, ID3D12Fence* fence, ComPtr<ID3D12Device>& device,
                           ComPtr<IUnknown>& identity) {
        const auto hr = fence->GetDevice(IID_PPV_ARGS(&device));
        record(step, hr, device.Get());
        if (FAILED(hr) || !device) return fail(step, FAILED(hr) ? hr : E_POINTER);
        const auto identityHr = device.As(&identity);
        record("owner-IUnknown", identityHr, identity.Get());
        return SUCCEEDED(identityHr) && identity ? true : fail("owner-IUnknown", FAILED(identityHr) ? identityHr : E_POINTER);
    };

    if (!device12 || !device11) return fail("no-device", E_POINTER);
    auto hr = device12->QueryInterface(IID_PPV_ARGS(&creatorIdentity));
    record("creator-IUnknown", hr, creatorIdentity.Get());
    if (FAILED(hr) || !creatorIdentity) return fail("creator-IUnknown", FAILED(hr) ? hr : E_POINTER);
    hr = device11->QueryInterface(IID_PPV_ARGS(&device5));
    record("D3D11Device5", hr, device5.Get());
    if (FAILED(hr) || !device5) return fail("D3D11Device5", FAILED(hr) ? hr : E_POINTER);

    hr = device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&created));
    record("CreateFence", hr, created.Get());
    if (FAILED(hr) || !created) return fail("CreateFence", FAILED(hr) ? hr : E_POINTER);
    if (!owner("created-publicOwner", created.Get(), publicOwner, publicOwnerIdentity)) return false;
    // This comparison records exact COM identity only. It does not authorize
    // using a forwarded fence on the consumer or relaxing any downstream gate.
    _diagnostic += std::format("creator-publicOwner-same={}; ", creatorIdentity == publicOwnerIdentity);

    hr = device12->CreateSharedHandle(created.Get(), nullptr, GENERIC_ALL, nullptr, &handle.value);
    record("CreateSharedHandle", hr, handle.value);
    if (FAILED(hr) || !handle.value || handle.value == INVALID_HANDLE_VALUE)
        return fail("CreateSharedHandle", FAILED(hr) ? hr : E_HANDLE);

    // Import through the selected consumer's public typed API, not through the
    // returned fence's owner. OpenSharedHandle preserves the exported timeline.
    hr = device12->OpenSharedHandle(handle.value, IID_PPV_ARGS(&consumer));
    record("consumer-OpenSharedHandle-ID3D12Fence", hr, consumer.Get());
    if (FAILED(hr) || !consumer) return fail("consumer-OpenSharedHandle", FAILED(hr) ? hr : E_POINTER);
    if (!owner("consumer-publicOwner", consumer.Get(), consumerOwner, consumerOwnerIdentity)) return false;
    const bool sameConsumer = creatorIdentity == consumerOwnerIdentity;
    _diagnostic += std::format("consumer-owner-same={}; ", sameConsumer);
    if (!sameConsumer) return fail("consumer-owner-mismatch", E_INVALIDARG);

    hr = device5->OpenSharedFence(handle.value, IID_PPV_ARGS(&fence11));
    record("D3D11-OpenSharedFence", hr, fence11.Get());
    if (FAILED(hr) || !fence11) return fail("D3D11-OpenSharedFence", FAILED(hr) ? hr : E_POINTER);

    // Commit only the complete pair. Keep the export object and both actual
    // owners alive alongside the consumer fence until the existing Reset gate.
    _creator12 = device12;
    _publicOwner12 = publicOwner;
    _consumerOwner12 = consumerOwner;
    _createdFence12 = created;
    _fence12 = consumer;
    _fence11 = fence11;
    return true;
}

void SharedFence::Reset()
{
    _fence11.Reset();
    _fence12.Reset();
    _createdFence12.Reset();
    _consumerOwner12.Reset();
    _publicOwner12.Reset();
    _creator12.Reset();
    _value = 0;
    _error.clear();
    _diagnostic.clear();
}

} // namespace native
