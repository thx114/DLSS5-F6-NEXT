#include "pch.h"
#include "IFGFeature_Dx12.h"
#include <State.h>
#include <Config.h>

#include <magic_enum.hpp>

bool IFGFeature_Dx12::GetResourceCopy(FG_ResourceType type, D3D12_RESOURCE_STATES bufferState, ID3D12Resource* output)
{
    if (!InitCopyCmdList())
        return false;

    auto resource = GetResource(type);

    if (!resource || (resource->copy == nullptr && resource->validity == FG_ResourceValidity::ValidNow))
    {
        LOG_WARN("No resource copy of type {} to use", magic_enum::enum_name(type));
        return false;
    }

    auto fIndex = GetIndex();

    if (!_uiCommandListResetted[fIndex])
    {
        auto result = _copyCommandAllocator[fIndex]->Reset();
        if (result != S_OK)
            return false;

        result = _copyCommandList[fIndex]->Reset(_copyCommandAllocator[fIndex], nullptr);
        if (result != S_OK)
            return false;
    }

    _copyCommandList[fIndex]->CopyResource(output, resource->GetResource());

    return true;
}

ID3D12CommandQueue* IFGFeature_Dx12::GetCommandQueue() { return _gameCommandQueue; }

bool IFGFeature_Dx12::HasResource(FG_ResourceType type, int index)
{
    if (index < 0)
        index = GetIndex();

    return _frameResources[index].contains(type);
}

IFGFeature_Dx12::UIAllocatorStatus IFGFeature_Dx12::PollUIAllocator(UINT index) const
{
    // This opt-in poll requires the real submission/allocator owners even for
    // a never-used slot. The default blocking implementation remains unchanged.
    if (index >= BUFFER_COUNT || !_device || !_gameCommandQueue || !_uiFence || !_uiFenceEvent ||
        !_uiCommandAllocator[index] || !_uiCommandList[index]) return UIAllocatorStatus::Invalid;
    const auto fenceValue = _uiAllocatorFenceValues[index];
    const auto completed = _uiFence->GetCompletedValue();
    if (completed == UINT64_MAX) return UIAllocatorStatus::Invalid;
    return completed >= fenceValue ? UIAllocatorStatus::Ready : UIAllocatorStatus::Busy;
}

bool IFGFeature_Dx12::WaitForUIAllocator(UINT index)
{
    const auto fenceValue = _uiAllocatorFenceValues[index];
    if (fenceValue == 0)
        return true;

    // A used slot needs its completion owner; absence cannot authorize Reset.
    if (_uiFence == nullptr || _uiFenceEvent == nullptr)
        return false;

    const auto completedValue = _uiFence->GetCompletedValue();
    if (completedValue == UINT64_MAX) // Device removal is not completion.
        return false;
    if (completedValue >= fenceValue)
        return true;

    auto result = _uiFence->SetEventOnCompletion(fenceValue, _uiFenceEvent);
    if (FAILED(result))
    {
        LOG_ERROR("UI allocator fence SetEventOnCompletion failed. slot {}, fence {}, completed {}, result {:X}", index,
                  fenceValue, completedValue, (UINT) result);
        return false;
    }

    const auto waitResult = WaitForSingleObject(_uiFenceEvent, 5000);
    // A prior timed-out registration may still signal this shared event. Only
    // this slot's actual fence target proves that its allocator can be reset.
    const auto completedAfterWait = _uiFence->GetCompletedValue();
    if (waitResult != WAIT_OBJECT_0 || completedAfterWait == UINT64_MAX || completedAfterWait < fenceValue)
    {
        LOG_ERROR("UI allocator fence wait incomplete. slot {}, fence {}, completed {}, waitResult {:X}", index, fenceValue,
                  completedAfterWait, waitResult);
        return false;
    }

    return true;
}

bool IFGFeature_Dx12::SubmitUICommandList(UINT index)
{
    if (index >= BUFFER_COUNT || !_uiCommandListResetted[index])
        return true;

    if (_gameCommandQueue == nullptr || _uiFence == nullptr)
    {
        LOG_ERROR("Can't submit UI command list. slot {}, queue {:X}, fence {:X}", index, (size_t) _gameCommandQueue,
                  (size_t) _uiFence);
        return false;
    }

    LOG_DEBUG("Executing _uiCommandList[{}]: {:X}, fence {}", index, (size_t) _uiCommandList[index],
              _uiAllocatorFenceValues[index]);

    auto closeResult = _uiCommandList[index]->Close();
    if (FAILED(closeResult))
    {
        LOG_ERROR("_uiCommandList[{}]->Close() error: {:X}", index, (UINT) closeResult);
        return false;
    }

    _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_uiCommandList[index]);
    _uiCommandListResetted[index] = false;

    auto signalResult = _gameCommandQueue->Signal(_uiFence, _uiAllocatorFenceValues[index]);
    if (FAILED(signalResult))
    {
        LOG_ERROR("UI allocator fence signal failed. slot {}, fence {}, result {:X}", index,
                  _uiAllocatorFenceValues[index], (UINT) signalResult);
        return false;
    }

    return true;
}

ID3D12GraphicsCommandList* IFGFeature_Dx12::GetUICommandList(int index)
{
    if (index < 0 || index >= BUFFER_COUNT)
        index = GetIndex();

    LOG_DEBUG("index: {}", index);

    if (_uiCommandAllocator[0] == nullptr)
    {
        if (_device != nullptr)
            CreateObjects(_device);
        else if (State::Instance().currentD3D12Device != nullptr)
            CreateObjects(State::Instance().currentD3D12Device);
        else
            return nullptr;
    }

    for (size_t j = 0; j < 2; j++)
    {
        auto i = (index + j) % BUFFER_COUNT;

        if (i != index && _uiCommandListResetted[i])
        {
            if (!SubmitUICommandList((UINT) i))
                return nullptr;
        }
    }

    if (!_uiCommandListResetted[index])
    {
        if (!WaitForUIAllocator((UINT) index))
            return nullptr;

        auto result = _uiCommandAllocator[index]->Reset();

        if (result == S_OK)
        {
            result = _uiCommandList[index]->Reset(_uiCommandAllocator[index], nullptr);

            if (result == S_OK)
            {
                _uiCommandListResetted[index] = true;
                _uiAllocatorFenceValues[index] = ++_uiFenceValue;
            }
            else
            {
                LOG_ERROR("_uiCommandList[{}]->Reset() error: {:X}", index, (UINT) result);
                return nullptr;
            }
        }
        else
        {
            LOG_ERROR("_uiCommandAllocator[{}]->Reset() error: {:X}", index, (UINT) result);
            return nullptr;
        }
    }

    return _uiCommandList[index];
}

ID3D12GraphicsCommandList* IFGFeature_Dx12::GetSCCommandList(int index)
{
    if (index < 0)
        index = GetIndex();

    LOG_DEBUG("index: {}", index);

    if (_scCommandAllocator[0] == nullptr)
    {
        if (_device != nullptr)
            CreateObjects(_device);
        else if (State::Instance().currentD3D12Device != nullptr)
            CreateObjects(State::Instance().currentD3D12Device);
        else
            return nullptr;
    }

    for (size_t j = 0; j < 2; j++)
    {
        auto i = (index + j) % BUFFER_COUNT;

        if (i != index && _scCommandListResetted[i])
        {
            LOG_DEBUG("Executing _scCommandList[{}]: {:X}", i, (size_t) _scCommandList[i]);
            auto closeResult = _scCommandList[i]->Close();

            if (closeResult != S_OK)
                LOG_ERROR("_scCommandList[{}]->Close() error: {:X}", i, (UINT) closeResult);

            _scCommandListResetted[i] = false;
        }
    }

    if (!_scCommandListResetted[index])
    {
        auto result = _scCommandAllocator[index]->Reset();

        if (result == S_OK)
        {
            result = _scCommandList[index]->Reset(_scCommandAllocator[index], nullptr);

            if (result == S_OK)
                _scCommandListResetted[index] = true;
            else
                LOG_ERROR("_scCommandList[{}]->Reset() error: {:X}", index, (UINT) result);
        }
        else
        {
            LOG_ERROR("_scCommandAllocator[{}]->Reset() error: {:X}", index, (UINT) result);
        }
    }

    return _scCommandList[index];
}

LockedDx12Resource IFGFeature_Dx12::GetResource(FG_ResourceType type, int index)
{
    if (index < 0)
        index = GetIndex();

    std::shared_lock lock(_resourceMutex[index]);

    auto& resources = _frameResources[index];

    auto it = resources.find(type);
    if (it != resources.end())
        return { &it->second, std::move(lock) };

    return { nullptr, std::move(lock) };
}

void IFGFeature_Dx12::NewFrame()
{
    if (_waitingNewFrameData)
    {
        LOG_DEBUG("Re-activating FG");
        UpdateTarget();
        Activate();
        _waitingNewFrameData = false;
    }

    auto fIndex = GetIndex();

    // Submit fence value before the slot can be reused
    if (_uiCommandListResetted[fIndex] && !SubmitUICommandList((UINT) fIndex))
        LOG_ERROR("Failed to submit pending UI command list for recycled slot {}", fIndex);

    std::unique_lock<std::shared_mutex> lock(_resourceMutex[fIndex]);

    LOG_DEBUG("_frameCount: {}, fIndex: {}", _frameCount, fIndex);

    _frameResources[fIndex].clear();
    _lastFGFramePresentId = _fgFramePresentId;
}

bool IFGFeature_Dx12::FlipResource(Dx12Resource* resource)
{
    auto type = resource->type;

    if (type != FG_ResourceType::Depth && type != FG_ResourceType::Velocity)
        return false;

    auto fIndex = GetIndex();
    // Keep the slot owner current even if buffer recreation fails.
    auto& flipOutput = _resourceCopy[fIndex][type];
    std::unique_ptr<RF_Dx12>* flip = nullptr;

    if (!CreateBufferResource(_device, resource->resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &flipOutput, true,
                              resource->type == FG_ResourceType::Depth))
    {
        LOG_ERROR("{}, CreateBufferResource for flip is failed!", magic_enum::enum_name(type));
        return false;
    }

    if (type == FG_ResourceType::Depth)
    {
        if (_depthFlip.get() == nullptr)
        {
            _depthFlip = std::make_unique<RF_Dx12>("DepthFlip", _device);
            return false;
        }

        flip = &_depthFlip;
    }
    else
    {
        if (_mvFlip.get() == nullptr)
        {
            _mvFlip = std::make_unique<RF_Dx12>("VelocityFlip", _device);
            return false;
        }

        flip = &_mvFlip;
    }

    if (flip->get()->IsInit())
    {
        auto cmdList = (resource->cmdList != nullptr) ? resource->cmdList : GetUICommandList(fIndex);
        if (cmdList == nullptr)
        {
            LOG_ERROR("Can't flip {}: GetUICommandList({}) failed", magic_enum::enum_name(type), fIndex);
            return false;
        }

        auto result = flip->get()->Dispatch((ID3D12GraphicsCommandList*) cmdList, resource->resource, flipOutput,
                                            resource->width, resource->height, true);

        if (result)
        {
            LOG_TRACE("Setting {} from flip, index: {}", magic_enum::enum_name(type), fIndex);
            resource->validity = FG_ResourceValidity::UntilPresent;
            resource->copy = flipOutput;
            resource->state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            return true;
        }
    }

    return false;
}

bool IFGFeature_Dx12::CreateBufferResourceWithSize(ID3D12Device* device, ID3D12Resource* source,
                                                   D3D12_RESOURCE_STATES state, ID3D12Resource** target, UINT width,
                                                   UINT height, bool UAV, bool depth)
{
    if (device == nullptr || source == nullptr)
        return false;

    auto inDesc = source->GetDesc();

    if (UAV)
        inDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (depth)
        inDesc.Format = DXGI_FORMAT_R32_FLOAT;

    if (*target != nullptr)
    {
        auto bufDesc = (*target)->GetDesc();

        if (bufDesc.Width != width || bufDesc.Height != height || bufDesc.Format != inDesc.Format ||
            bufDesc.Flags != inDesc.Flags)
        {
            (*target)->Release();
            (*target) = nullptr;
        }
        else
        {
            return true;
        }
    }

    D3D12_HEAP_PROPERTIES heapProperties;
    D3D12_HEAP_FLAGS heapFlags;
    HRESULT hr = source->GetHeapProperties(&heapProperties, &heapFlags);

    if (hr != S_OK)
    {
        LOG_ERROR("GetHeapProperties result: {:X}", (UINT64) hr);
        return false;
    }

    inDesc.Width = width;
    inDesc.Height = height;

    hr = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &inDesc, state, nullptr,
                                         IID_PPV_ARGS(target));

    if (hr != S_OK)
    {
        LOG_ERROR("CreateCommittedResource result: {:X}", (UINT64) hr);
        return false;
    }

    LOG_DEBUG("Created new one: {}x{}", inDesc.Width, inDesc.Height);

    return true;
}

bool IFGFeature_Dx12::InitCopyCmdList()
{
    if (_copyCommandList[0] != nullptr && _copyCommandAllocator[0] != nullptr)
        return true;

    if (_device == nullptr)
        return false;

    if (_copyCommandList[0] == nullptr || _copyCommandAllocator[0] == nullptr)
        DestroyCopyCmdList();

    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* cmdList = nullptr;

    for (size_t i = 0; i < BUFFER_COUNT; i++)
    {
        auto result =
            _device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_copyCommandAllocator[i]));
        if (result != S_OK)
        {
            LOG_ERROR("_copyCommandAllocator: {:X}", (unsigned long) result);
            return false;
        }

        _copyCommandAllocator[i]->SetName(L"_copyCommandAllocator");
        if (CheckForRealObject(__FUNCTION__, _copyCommandAllocator[i], (IUnknown**) &allocator))
            _copyCommandAllocator[i] = allocator;

        result = _device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _copyCommandAllocator[i], NULL,
                                            IID_PPV_ARGS(&_copyCommandList[i]));
        if (result != S_OK)
        {
            LOG_ERROR("_copyCommandAllocator: {:X}", (unsigned long) result);
            return false;
        }
        _copyCommandList[i]->SetName(L"_copyCommandList");
        if (CheckForRealObject(__FUNCTION__, _copyCommandList[i], (IUnknown**) &cmdList))
            _copyCommandList[i] = cmdList;

        result = _copyCommandList[i]->Close();
        if (result != S_OK)
        {
            LOG_ERROR("_copyCommandList->Close: {:X}", (unsigned long) result);
            return false;
        }
    }

    return true;
}

void IFGFeature_Dx12::DestroyCopyCmdList()
{
    for (size_t i = 0; i < BUFFER_COUNT; i++)
    {
        SAFE_RELEASE(_copyCommandAllocator[i]);
        SAFE_RELEASE(_copyCommandList[i]);
    }
}

bool IFGFeature_Dx12::CreateBufferResource(ID3D12Device* device, ID3D12Resource* source,
                                           D3D12_RESOURCE_STATES initialState, ID3D12Resource** target, bool UAV,
                                           bool depth)
{
    if (device == nullptr || source == nullptr)
        return false;

    auto inDesc = source->GetDesc();

    if (UAV)
        inDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (depth)
        inDesc.Format = DXGI_FORMAT_R32_FLOAT;

    if (*target != nullptr)
    {
        //(*target)->Release();
        //(*target) = nullptr;

        auto bufDesc = (*target)->GetDesc();

        if (bufDesc.Width != inDesc.Width || bufDesc.Height != inDesc.Height || bufDesc.Format != inDesc.Format ||
            bufDesc.Flags != inDesc.Flags)
        {
            (*target)->Release();
            (*target) = nullptr;
        }
        else
        {
            return true;
        }
    }

    D3D12_HEAP_PROPERTIES heapProperties;
    D3D12_HEAP_FLAGS heapFlags;
    auto hr = source->GetHeapProperties(&heapProperties, &heapFlags);

    hr = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &inDesc, initialState, nullptr,
                                         IID_PPV_ARGS(target));

    if (hr != S_OK)
    {
        LOG_ERROR("CreateCommittedResource result: {:X}", (UINT64) hr);
        return false;
    }

    LOG_DEBUG("Created new one: {}x{}", inDesc.Width, inDesc.Height);

    return true;
}

void IFGFeature_Dx12::ResourceBarrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource,
                                      D3D12_RESOURCE_STATES beforeState, D3D12_RESOURCE_STATES afterState)
{
    if (beforeState == afterState)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = beforeState;
    barrier.Transition.StateAfter = afterState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);
}

bool IFGFeature_Dx12::CopyResource(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source, ID3D12Resource** target,
                                   D3D12_RESOURCE_STATES sourceState)
{
    auto result = true;

    ResourceBarrier(cmdList, source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);

    if (CreateBufferResource(_device, source, D3D12_RESOURCE_STATE_COPY_DEST, target))
        cmdList->CopyResource(*target, source);
    else
        result = false;

    ResourceBarrier(cmdList, source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);

    return result;
}
