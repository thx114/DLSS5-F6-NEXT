#include <pch.h>

#include "with_dx12.h"

#include <proxies/DXGI_Proxy.h>
#include <proxies/D3D12_Proxy.h>
#include <inputs/GenshinFsrContract.h>
#include <native/SharedFrame.h>
#include <hooks/D3D12_Hooks.h>

HRESULT CreateD3D12DeviceOnAdapter(IDXGIAdapter* adapter, D3D_FEATURE_LEVEL featureLevel, ID3D12Device** device)
{
    if (device == nullptr)
        return E_POINTER;

    *device = nullptr;

    if(GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring())) {
        Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
        const auto hr=D3D12GetDebugInterface(IID_PPV_ARGS(&dred));
        if(SUCCEEDED(hr) && dred) {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            LOG_WARN("R24 Genshin DRED auto-breadcrumbs/page-fault enabled before interop device creation");
        } else LOG_WARN("R24 Genshin DRED settings unavailable: {:X}",static_cast<UINT>(hr));
    }

    if (D3d12Proxy::Module() == nullptr)
        return D3D12CreateDevice(adapter, featureLevel, IID_PPV_ARGS(device));

    return D3d12Proxy::D3D12CreateDevice_()(adapter, featureLevel, IID_PPV_ARGS(device));
}

HRESULT TestD3D12DeviceOnAdapter(IDXGIAdapter* adapter, D3D_FEATURE_LEVEL featureLevel)
{
    if (D3d12Proxy::Module() == nullptr)
        return D3D12CreateDevice(adapter, featureLevel, _uuidof(ID3D12Device), nullptr);

    return D3d12Proxy::D3D12CreateDevice_()(adapter, featureLevel, _uuidof(ID3D12Device), nullptr);
}

namespace
{
bool GenshinConsumerPair()
{
    static const bool selected = GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring());
    return selected;
}

bool PublicPairMatches(ID3D12Device* device, ID3D12CommandQueue* queue, D3D12_COMMAND_LIST_TYPE type)
{
    if (!device || !queue || type != D3D12_COMMAND_LIST_TYPE_DIRECT || queue->GetDesc().Type != type)
        return false;
    Microsoft::WRL::ComPtr<ID3D12Device> owner;
    Microsoft::WRL::ComPtr<IUnknown> expected, actual;
    return SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&owner))) && owner &&
        SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&expected))) && expected &&
        SUCCEEDED(owner.As(&actual)) && actual && expected == actual &&
        SUCCEEDED(device->GetDeviceRemovedReason());
}
}

WithDx12::PairReservation::~PairReservation()
{
    std::lock_guard lock(_publicationMutex);
    _constructingPair = false;
}

bool WithDx12::IsInited()
{
    if (!GenshinConsumerPair()) return _d3d12Device && _d3d12CommandQueue;
    std::lock_guard lock(_publicationMutex);
    return _d3d12Device && _d3d12CommandQueue;
}

bool WithDx12::PrepareGenshinPair(ID3D11Device* device11, D3D_FEATURE_LEVEL level, IDXGIAdapter* requestedAdapter)
{
    ID3D12Device* externalDevice = nullptr;
    ID3D12CommandQueue* externalQueue = nullptr;
    {
        std::lock_guard lock(_publicationMutex);
        if (_d3d12Device && _d3d12CommandQueue) return true;
        if (_constructingPair || _d3d12Device || _d3d12CommandQueue) return false;
        auto& state = State::Instance();
        if (state.currentD3D12Device || state.currentCommandQueue)
        {
            // An existing foreign pair is never an unpublished seed. Adopt its
            // EXACT supplied queue only after public validation, or leave it alone.
            if (!state.currentD3D12Device || !state.currentCommandQueue) return false;
            externalDevice = state.currentD3D12Device;
            externalQueue = state.currentCommandQueue;
        }
        else
        {
            _constructingPair = true;
            _publicationInterrupted = false;
        }
    }
    if (externalDevice)
    {
        SetD3D12Objects(externalDevice, externalQueue);
        return IsInited();
    }
    // All COM locals die before this guard clears the reservation. Reentrant
    // Prepare/Request returns not-ready; Set interrupts, without waiting.
    PairReservation reservation;
    ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
    ScopedSkipVulkanHooks skipVulkanHooks {};
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter = requestedAdapter;
    if (device11)
    {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
        if (FAILED(device11->QueryInterface(IID_PPV_ARGS(&dxgi))) || !dxgi ||
            FAILED(dxgi->GetAdapter(adapter.ReleaseAndGetAddressOf())) || !adapter) return false;
    }
    else if (!adapter)
    {
        Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
        const auto hr = DxgiProxy::Module() == nullptr
            ? CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))
            : DxgiProxy::CreateDxgiFactory2_()(0, __uuidof(IDXGIFactory2), factory.GetAddressOf());
        if (FAILED(hr) || !factory) return false;
        GetHardwareAdapter(factory.Get(), adapter.GetAddressOf(), level, true);
    }
    Microsoft::WRL::ComPtr<ID3D12Device> seed, device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    const auto hr = [&] {
        // The ONLY scope construction site. Covers the actual seed factory
        // invocation and its synchronous factory nesting, never adapter probes,
        // public-owner construction, final hooks, or any later consumer work.
        InternalPairCreationScope internalCreation;
        return CreateD3D12DeviceOnAdapter(adapter.Get(), level, seed.GetAddressOf());
    }();
    if (FAILED(hr) || !seed) return false;
    std::string diagnostic;
    const bool ready = native::CreatePublicConsumerDomain(seed.Get(), device, queue, diagnostic);
    LOG_INFO("Genshin first consumer pair: ready={} seed={} device={} queue={} {}", ready,
        static_cast<void*>(seed.Get()), static_cast<void*>(device.Get()), static_cast<void*>(queue.Get()), diagnostic);
    if (!ready) return false;
    {
        std::lock_guard lock(_publicationMutex);
        if (_publicationInterrupted || State::Instance().currentD3D12Device ||
            State::Instance().currentCommandQueue) return false;
    }
    // Setup is outside the mutex and before ANY pair/State publication.
    // Callbacks and other WithDx12 readers see not-ready and never wait.
    if (!InitializeInternalPairDevice(device.Get())) return false;
    {
        std::lock_guard lock(_publicationMutex);
        auto& state = State::Instance();
        // No seed allowance: hooks must have kept State completely empty.
        // A concurrent foreign publication cancels this transaction.
        if (_publicationInterrupted || _d3d12Device || _d3d12CommandQueue ||
            state.currentD3D12Device || state.currentCommandQueue) return false;
        state.d3d12Devices.push_back(device.Get()); // may allocate; still no pointer/pair published on throw
        _d3d12Device = device.Detach();
        _d3d12CommandQueue = queue.Detach();
        _commandListType = D3D12_COMMAND_LIST_TYPE_DIRECT;
        // Only WithDx12 readers share this mutex. State's existing raw fields
        // are compatibility mirrors, not an atomic pair for arbitrary readers.
        UpdateStateObjects();
    }
    return true;
}

void WithDx12::UpdateStateObjects()
{
    State::Instance().currentD3D12Device = _d3d12Device;
    State::Instance().currentCommandQueue = _d3d12CommandQueue;
}

void WithDx12::SetD3D12Objects(ID3D12Device* InDevice, ID3D12CommandQueue* InCommandQueue,
                               D3D12_COMMAND_LIST_TYPE InCommandListType)
{
    if (GenshinConsumerPair())
    {
        {
            std::lock_guard lock(_publicationMutex);
            if (_d3d12Device || _d3d12CommandQueue)
                return; // immutable complete pair; FG off/on reuses this exact queue
            if (_constructingPair)
            {
                _publicationInterrupted = true;
                return; // never wait inside a driver/third-party callback
            }
            if (!InDevice || !InCommandQueue) return;
            auto& state = State::Instance();
            if ((state.currentD3D12Device && state.currentD3D12Device != InDevice) ||
                (state.currentCommandQueue && state.currentCommandQueue != InCommandQueue)) return;
            _constructingPair = true;
            _publicationInterrupted = false;
        }
        PairReservation reservation;
        Microsoft::WRL::ComPtr<ID3D12Device> device = InDevice;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue = InCommandQueue;
        if (!PublicPairMatches(device.Get(), queue.Get(), InCommandListType)) return;
        {
            std::lock_guard lock(_publicationMutex);
            auto& state = State::Instance();
            if (_publicationInterrupted || _d3d12Device || _d3d12CommandQueue ||
                (state.currentD3D12Device && state.currentD3D12Device != InDevice) ||
                (state.currentCommandQueue && state.currentCommandQueue != InCommandQueue)) return;
            // Preserve an already supplied pair, including its exact queue. No
            // owner normalization, new queue, or migration of existing resources.
            _d3d12Device = device.Detach();
            _d3d12CommandQueue = queue.Detach();
            _commandListType = InCommandListType;
            UpdateStateObjects();
        }
        return;
    }
    _d3d12Device = InDevice;
    _d3d12CommandQueue = InCommandQueue;
    _commandListType = InCommandListType;

    UpdateStateObjects();

    LOG_DEBUG("Using D3D12 objects, device: {:X}, queue: {:X}", (size_t) _d3d12Device, (size_t) _d3d12CommandQueue);
}

void WithDx12::ForgetD3D12Device(ID3D12Device* device)
{
    if (GenshinConsumerPair()) return; // Forget is a Vulkan-only private-domain teardown
    if (device == nullptr || _d3d12Device != device)
        return;

    auto& state = State::Instance();

    if (state.currentD3D12Device == _d3d12Device)
        state.currentD3D12Device = nullptr;

    if (state.currentCommandQueue == _d3d12CommandQueue)
        state.currentCommandQueue = nullptr;

    _d3d12Device = nullptr;
    _d3d12CommandQueue = nullptr;
}

bool WithDx12::PrepareD3D12ForD3D11(ID3D11Device* InDx11Device, D3D_FEATURE_LEVEL InFeatureLevel)
{
    if (GenshinConsumerPair()) return PrepareGenshinPair(InDx11Device, InFeatureLevel, nullptr);
    if (IsInited())
    {
        UpdateStateObjects();
        return true;
    }

    if (State::Instance().currentCommandQueue != nullptr)
    {
        ID3D12Device* queueDevice = nullptr;
        auto deviceResult = State::Instance().currentCommandQueue->GetDevice(IID_PPV_ARGS(&queueDevice));

        if (SUCCEEDED(deviceResult) && queueDevice != nullptr)
        {
            SetD3D12Objects(queueDevice, State::Instance().currentCommandQueue, D3D12_COMMAND_LIST_TYPE_DIRECT);
            return IsInited();
        }

        LOG_WARN("EnsureD3D12ForDx11: current command queue GetDevice failed: {:X}", (UINT) deviceResult);
    }

    if (State::Instance().currentD3D12Device != nullptr)
    {
        auto queue = CreateD3D12CommandQueue(State::Instance().currentD3D12Device);
        if (queue != nullptr)
        {
            SetD3D12Objects(State::Instance().currentD3D12Device, queue, D3D12_COMMAND_LIST_TYPE_DIRECT);
            return IsInited();
        }
    }

    ID3D12Device* dx12Device = nullptr;
    ID3D12CommandQueue* dx12CommandQueue = nullptr;
    if (CreateD3D12DeviceAndQueueFromD3D11(InDx11Device, InFeatureLevel, &dx12Device, &dx12CommandQueue))
        return IsInited();

    return CreateDx12Device(InFeatureLevel);
}

void WithDx12::GetHardwareAdapter(IDXGIFactory1* InFactory, IDXGIAdapter** InAdapter, D3D_FEATURE_LEVEL InFeatureLevel,
                                  bool InRequestHighPerformanceAdapter)
{
    LOG_FUNC();

    *InAdapter = nullptr;

    IDXGIAdapter1* adapter = nullptr;

    IDXGIFactory6* factory6 = nullptr;
    if (SUCCEEDED(InFactory->QueryInterface(IID_PPV_ARGS(&factory6))))
    {
        LOG_DEBUG("Using IDXGIFactory6 & EnumAdapterByGpuPreference");

        for (UINT adapterIndex = 0;
             DXGI_ERROR_NOT_FOUND != factory6->EnumAdapterByGpuPreference(adapterIndex,
                                                                          InRequestHighPerformanceAdapter == true
                                                                              ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE
                                                                              : DXGI_GPU_PREFERENCE_UNSPECIFIED,
                                                                          IID_PPV_ARGS(&adapter));
             ++adapterIndex)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);

            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            {
                adapter->Release();
                adapter = nullptr;
                continue;
            }

            *InAdapter = adapter;
            break;
        }

        factory6->Release();
    }
    else
    {
        LOG_DEBUG("Using InFactory & EnumAdapters1");
        for (UINT adapterIndex = 0; DXGI_ERROR_NOT_FOUND != InFactory->EnumAdapters1(adapterIndex, &adapter);
             ++adapterIndex)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);

            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            {
                adapter->Release();
                adapter = nullptr;
                continue;
            }

            // Check to see whether the adapter supports Direct3D 12, but don't create the actual device yet.
            auto result = TestD3D12DeviceOnAdapter(adapter, InFeatureLevel);

            if (result == S_FALSE)
            {
                LOG_DEBUG("D3D12CreateDevice test result: {:X}", (UINT) result);
                *InAdapter = adapter;
                break;
            }

            adapter->Release();
            adapter = nullptr;
        }
    }
}

ID3D12Device* WithDx12::RequestD3D12Device(D3D_FEATURE_LEVEL InFeatureLevel, IDXGIAdapter* adapter)
{
    if (GenshinConsumerPair())
        return PrepareGenshinPair(nullptr, InFeatureLevel, adapter) ? GetD3D12Device() : nullptr;
    if (CreateDx12Device(InFeatureLevel, adapter))
        return _d3d12Device;

    return nullptr;
}

ID3D12Device* WithDx12::RequestD3D12Device(D3D_FEATURE_LEVEL InFeatureLevel)
{
    return RequestD3D12Device(InFeatureLevel, nullptr);
}

ID3D12Device* WithDx12::GetD3D12Device()
{
    if (!GenshinConsumerPair()) return _d3d12Device;
    std::lock_guard lock(_publicationMutex);
    return _d3d12Device && _d3d12CommandQueue ? _d3d12Device : nullptr;
}

ID3D12CommandQueue* WithDx12::GetD3D12CommandQueue()
{
    if (!GenshinConsumerPair()) return _d3d12CommandQueue;
    std::lock_guard lock(_publicationMutex);
    return _d3d12Device && _d3d12CommandQueue ? _d3d12CommandQueue : nullptr;
}

D3D12_COMMAND_LIST_TYPE WithDx12::GetD3D12CommandListType()
{
    if (!GenshinConsumerPair()) return _commandListType;
    std::lock_guard lock(_publicationMutex);
    return _commandListType;
}

ID3D12Device* WithDx12::GetD3D12DeviceFromD3D11(ID3D11Device* dx11Device, D3D_FEATURE_LEVEL featureLevel)
{
    if (dx11Device == nullptr)
        return nullptr;

    ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
    ScopedSkipVulkanHooks skipVulkanHooks {};

    IDXGIDevice* dxgiDevice = nullptr;
    auto result = dx11Device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(result) || dxgiDevice == nullptr)
    {
        LOG_ERROR("QueryInterface IDXGIDevice failed: {:X}", (UINT) result);
        return nullptr;
    }

    IDXGIAdapter* adapter = nullptr;
    result = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();

    if (FAILED(result) || adapter == nullptr)
    {
        LOG_ERROR("GetAdapter failed: {:X}", (UINT) result);
        return nullptr;
    }

    ID3D12Device* dx12Device = nullptr;
    result = CreateD3D12DeviceOnAdapter(adapter, featureLevel, &dx12Device);

    if (FAILED(result) || dx12Device == nullptr)
    {
        LOG_ERROR("D3D12CreateDevice failed: {:X}", (UINT) result);
        adapter->Release();
        return nullptr;
    }

    DXGI_ADAPTER_DESC desc = {};
    if (SUCCEEDED(adapter->GetDesc(&desc)))
        LOG_INFO("D3D12 interop device created on D3D11 adapter: {}", wstring_to_string(desc.Description));

    adapter->Release();
    return dx12Device;
}

ID3D12CommandQueue* WithDx12::CreateD3D12CommandQueue(ID3D12Device* dx12Device)
{
    if (dx12Device == nullptr)
        return nullptr;

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = _commandListType;

    ID3D12CommandQueue* commandQueue = nullptr;
    auto result = dx12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue));
    if (FAILED(result) || commandQueue == nullptr)
    {
        LOG_ERROR("CreateD3D12CommandQueue failed: {:X}", (UINT) result);
        return nullptr;
    }

    return commandQueue;
}

bool WithDx12::CreateD3D12DeviceAndQueueFromD3D11(ID3D11Device* dx11Device, D3D_FEATURE_LEVEL featureLevel,
                                                  ID3D12Device** dx12Device, ID3D12CommandQueue** dx12CommandQueue)
{
    if (dx12Device == nullptr || dx12CommandQueue == nullptr)
        return false;

    *dx12Device = nullptr;
    *dx12CommandQueue = nullptr;

    ID3D12Device* localDevice = GetD3D12DeviceFromD3D11(dx11Device, featureLevel);
    if (localDevice == nullptr)
        return false;

    ID3D12CommandQueue* localQueue = CreateD3D12CommandQueue(localDevice);
    if (localQueue == nullptr)
    {
        localDevice->Release();
        return false;
    }

    _d3d12Device = localDevice;
    _d3d12CommandQueue = localQueue;
    _commandListType = D3D12_COMMAND_LIST_TYPE_DIRECT;

    *dx12Device = localDevice;
    *dx12CommandQueue = localQueue;

    UpdateStateObjects();

    return true;
}

bool WithDx12::CreateDx12Device(D3D_FEATURE_LEVEL InFeatureLevel, IDXGIAdapter* InAdapter)
{
    LOG_FUNC();

    ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
    ScopedSkipVulkanHooks skipVulkanHooks {};

    HRESULT result = S_OK;

    const bool forceCreate = static_cast<bool>(State::Instance().gameQuirks & GameQuirk::ForceCreateD3D12Device);

    if (_d3d12Device == nullptr && State::Instance().currentD3D12Device != nullptr && !forceCreate)
    {
        _d3d12Device = State::Instance().currentD3D12Device;
        LOG_DEBUG("Using captured currentD3D12Device as WithDx12 device");
    }

    if (_d3d12Device == nullptr || forceCreate)
    {
        IDXGIAdapter* hwAdapter = InAdapter;
        if (hwAdapter == nullptr)
        {
            IDXGIFactory2* factory = nullptr;

            if (DxgiProxy::Module() == nullptr)
                result = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
            else
                result = DxgiProxy::CreateDxgiFactory2_()(0, __uuidof(factory), &factory);

            if (FAILED(result) || factory == nullptr)
            {
                LOG_ERROR("Can't create factory: {0:x}", result);
                return false;
            }

            GetHardwareAdapter(factory, &hwAdapter, InFeatureLevel, true);

            if (hwAdapter == nullptr)
                LOG_WARN("Can't get hwAdapter, will try nullptr!");

            factory->Release();
        }

        ID3D12Device* newDevice = nullptr;
        result = CreateD3D12DeviceOnAdapter(hwAdapter, InFeatureLevel, &newDevice);

        if (FAILED(result) || newDevice == nullptr)
        {
            LOG_ERROR("Can't create device: {:X}", (UINT) result);

            if (InAdapter == nullptr && hwAdapter != nullptr)
            {
                hwAdapter->Release();
                hwAdapter = nullptr;
            }

            return false;
        }

        SAFE_RELEASE(_d3d12CommandQueue);
        SAFE_RELEASE(_d3d12Device);
        _d3d12Device = newDevice;

        if (hwAdapter != nullptr)
        {
            ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
            DXGI_ADAPTER_DESC desc = {};
            if (hwAdapter->GetDesc(&desc) == S_OK)
            {
                auto adapterDesc = wstring_to_string(desc.Description);
                LOG_INFO("D3D12Device created with adapter: {}", adapterDesc);
            }
        }

        if (InAdapter == nullptr && hwAdapter != nullptr)
        {
            hwAdapter->Release();
            hwAdapter = nullptr;
        }
    }

    if (_d3d12CommandQueue == nullptr)
    {
        _d3d12CommandQueue = CreateD3D12CommandQueue(_d3d12Device);

        if (_d3d12CommandQueue == nullptr)
            return false;
    }

    UpdateStateObjects();

    return true;
}
