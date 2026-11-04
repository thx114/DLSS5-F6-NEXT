#pragma once
#include "SysUtils.h"
#include "IFGFeature.h"
#include "FGHookRetirement.h"

#include <upscalers/IFeature.h>

#include <shaders/resource_flip/RF_Dx12.h>
#include <shaders/hudless_compare/HC_Dx12.h>
#include <shaders/render_ui/RUI_Dx12.h>

#include <dxgi1_6.h>
#include <d3d12.h>
#include <mutex>
#include <utility>

enum class FGExecutionScope { Inputs, Present, Lifecycle };

// A provider may serialize its recorded input commands, real Present, and destruction.
// Default providers return an empty guard and retain their existing behavior.
class FGExecutionGuard
{
    std::unique_lock<std::recursive_mutex> _lock;
    unsigned* _workDepth = nullptr;
  public:
    FGExecutionGuard() = default;
    FGExecutionGuard(std::recursive_mutex& mutex, unsigned* workDepth)
        : _lock(mutex), _workDepth(workDepth) { if (_workDepth) ++*_workDepth; }
    FGExecutionGuard(FGExecutionGuard&& other) noexcept
        : _lock(std::move(other._lock)), _workDepth(std::exchange(other._workDepth, nullptr)) {}
    FGExecutionGuard(const FGExecutionGuard&) = delete;
    FGExecutionGuard& operator=(const FGExecutionGuard&) = delete;
    void FinishWork() { if (_workDepth) { --*_workDepth; _workDepth = nullptr; } }
    // A caller that has completed all provider access may release both parts
    // of the guard before unrelated pacing. FinishWork keeps its old contract.
    void Finish() { FinishWork(); if (_lock.owns_lock()) _lock.unlock(); }
    ~FGExecutionGuard() { FinishWork(); }
};

struct Dx12Resource
{
    FG_ResourceType type;
    ID3D12Resource* resource = nullptr;
    UINT top = 0;
    UINT left = 0;
    UINT64 width = 0;
    UINT height = 0;
    ID3D12GraphicsCommandList* cmdList = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    FG_ResourceValidity validity = FG_ResourceValidity::ValidNow;

    // TODO: make private?
    ID3D12Resource* copy = nullptr;
    int frameIndex = -1;
    bool waitingExecution = false;

    ID3D12Resource* GetResource() { return (copy == nullptr) ? resource : copy; }
};

struct LockedDx12Resource
{
    Dx12Resource* resource = nullptr;
    std::shared_lock<std::shared_mutex> lock;

    Dx12Resource* operator->() { return resource; }
    Dx12Resource& operator*() { return *resource; }

    explicit operator bool() const { return resource != nullptr; }
};

class IFGFeature_Dx12 : public virtual IFGFeature
{
  private:
    ID3D12GraphicsCommandList* _copyCommandList[BUFFER_COUNT] {};
    ID3D12CommandAllocator* _copyCommandAllocator[BUFFER_COUNT] {};

    bool InitCopyCmdList();
    void DestroyCopyCmdList();

  protected:
    enum class UIAllocatorStatus { Ready, Busy, Invalid };
    UIAllocatorStatus PollUIAllocator(UINT index) const;
    virtual bool WaitForUIAllocator(UINT index);
    virtual bool SubmitUICommandList(UINT index);

  public:
    // The queue the game created the swapchain with, which frame generation takes the frame on. Not necessarily
    // State::currentCommandQueue: that follows every swapchain created, including the one frame generation makes.
    ID3D12CommandQueue* GameCommandQueue() const { return _gameCommandQueue; }
    virtual FGExecutionGuard GuardExecution(FGExecutionScope) { return {}; }
    virtual bool CanSubmitInputs() { return true; }
    virtual bool CompletePendingTeardown() { return true; }
    virtual bool PrepareForSwapchainChange(bool terminal = false) { return true; }
    // Separate opt-in contract: old bool/empty guard defaults are never completion receipts.
    virtual FGHookRetirementReceipt RetireSwapchainForHook(const FGHookRetirementRequest&) { return {}; }

  protected:
    ID3D12Device* _device = nullptr;
    IDXGISwapChain* _swapChain = nullptr;
    ID3D12CommandQueue* _gameCommandQueue = nullptr;

    HWND _hwnd = NULL;

    UINT64 _fgFramePresentId = 0;
    UINT64 _lastFGFramePresentId = 0;

    ID3D12GraphicsCommandList* _scCommandList[BUFFER_COUNT] {};
    ID3D12CommandAllocator* _scCommandAllocator[BUFFER_COUNT] {};
    bool _scCommandListResetted[BUFFER_COUNT] { false, false, false, false };
    UINT64 _scAllocatorFenceValues[BUFFER_COUNT] {};
    ID3D12Fence* _scFence = nullptr;
    HANDLE _scFenceEvent = nullptr;

    ID3D12GraphicsCommandList* _uiCommandList[BUFFER_COUNT] {};
    ID3D12CommandAllocator* _uiCommandAllocator[BUFFER_COUNT] {};
    bool _uiCommandListResetted[BUFFER_COUNT] { false, false, false, false };
    UINT64 _uiAllocatorFenceValues[BUFFER_COUNT] {};
    UINT64 _uiFenceValue = 0;
    ID3D12Fence* _uiFence = nullptr;
    HANDLE _uiFenceEvent = nullptr;

    std::unordered_map<FG_ResourceType, Dx12Resource> _frameResources[BUFFER_COUNT] {};
    std::unordered_map<FG_ResourceType, ID3D12Resource*> _resourceCopy[BUFFER_COUNT] {};
    std::shared_mutex _resourceMutex[BUFFER_COUNT];

    std::unique_ptr<RF_Dx12> _mvFlip;
    std::unique_ptr<RF_Dx12> _depthFlip;
    std::unique_ptr<HC_Dx12> _hudlessCompare;
    std::unique_ptr<RUI_Dx12> _renderUI;

    bool CreateBufferResource(ID3D12Device* InDevice, ID3D12Resource* InSource, D3D12_RESOURCE_STATES InState,
                              ID3D12Resource** OutResource, bool UAV = false, bool depth = false);
    bool CreateBufferResourceWithSize(ID3D12Device* device, ID3D12Resource* source, D3D12_RESOURCE_STATES state,
                                      ID3D12Resource** target, UINT width, UINT height, bool UAV, bool depth);
    void ResourceBarrier(ID3D12GraphicsCommandList* InCommandList, ID3D12Resource* InResource,
                         D3D12_RESOURCE_STATES InBeforeState, D3D12_RESOURCE_STATES InAfterState);
    bool CopyResource(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source, ID3D12Resource** target,
                      D3D12_RESOURCE_STATES sourceState);

    void NewFrame() override final;
    bool FlipResource(Dx12Resource* resource);

  protected:
    virtual void ReleaseObjects() = 0;
    virtual void CreateObjects(ID3D12Device* InDevice) = 0;

  public:
    virtual void* FrameGenerationContext() = 0;
    virtual void* SwapchainContext() = 0;
    virtual HWND Hwnd() = 0;

    virtual bool CreateSwapchain(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, DXGI_SWAP_CHAIN_DESC* desc,
                                 IDXGISwapChain** swapChain, bool readyToRelease) = 0;
    virtual bool CreateSwapchain1(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, HWND hwnd,
                                  DXGI_SWAP_CHAIN_DESC1* desc, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                  IDXGISwapChain1** swapChain, bool readyToRelease) = 0;

    virtual void CreateContext(ID3D12Device* device, FG_Constants& fgConstants) = 0;
    virtual void EvaluateState(ID3D12Device* device, FG_Constants& fgConstants) = 0;

    virtual bool SetResource(Dx12Resource* inputResource) = 0;
    virtual void SetCommandQueue(FG_ResourceType type, ID3D12CommandQueue* queue) = 0;

    ID3D12GraphicsCommandList* GetUICommandList(int index = -1);
    virtual ID3D12GraphicsCommandList* GetSCCommandList(int index = -1);

    LockedDx12Resource GetResource(FG_ResourceType type, int index = -1);
    bool GetResourceCopy(FG_ResourceType type, D3D12_RESOURCE_STATES bufferState, ID3D12Resource* output);
    ID3D12CommandQueue* GetCommandQueue();

    bool HasResource(FG_ResourceType type, int index = -1) override final;

    IFGFeature_Dx12() = default;
    virtual ~IFGFeature_Dx12() { DestroyCopyCmdList(); }
};
