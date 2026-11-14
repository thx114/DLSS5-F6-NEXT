// Test-only COM facade. Every device method forwards to the real WARP device.
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
class PreviewDeviceAlias final : public ID3D12Device {
    std::atomic<ULONG> refs {1};
    Microsoft::WRL::ComPtr<ID3D12Device> native;
public:
    explicit PreviewDeviceAlias(ID3D12Device* p):native(p){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(iid==__uuidof(IUnknown)||iid==__uuidof(ID3D12Object)||iid==__uuidof(ID3D12Device)){*out=static_cast<ID3D12Device*>(this);AddRef();return S_OK;}
        constexpr GUID known={0x7f2c9a11,0x3b4e,0x4d6a,{0x81,0x2f,0x5e,0x9c,0xd3,0x7a,0x1b,0x42}};
        return iid==known?native->QueryInterface(__uuidof(IUnknown),out):E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override{const ULONG n=--refs;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *pDataSize, void *pData) override {return native->GetPrivateData(guid, pDataSize, pData);}
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void *pData) override {return native->SetPrivateData(guid, DataSize, pData);}
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *pData) override {return native->SetPrivateDataInterface(guid, pData);}
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override {return native->SetName(Name);}
    UINT STDMETHODCALLTYPE GetNodeCount(void) override {return native->GetNodeCount();}
    HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC *pDesc, REFIID riid, void **ppCommandQueue) override {return native->CreateCommandQueue(pDesc, riid, ppCommandQueue);}
    HRESULT STDMETHODCALLTYPE CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type, REFIID riid, void **ppCommandAllocator) override {return native->CreateCommandAllocator(type, riid, ppCommandAllocator);}
    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState) override {return native->CreateGraphicsPipelineState(pDesc, riid, ppPipelineState);}
    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState) override {return native->CreateComputePipelineState(pDesc, riid, ppPipelineState);}
    HRESULT STDMETHODCALLTYPE CreateCommandList(UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator *pCommandAllocator, ID3D12PipelineState *pInitialState, REFIID riid, void **ppCommandList) override {return native->CreateCommandList(nodeMask, type, pCommandAllocator, pInitialState, riid, ppCommandList);}
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE Feature, void *pFeatureSupportData, UINT FeatureSupportDataSize) override {return native->CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize);}
    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC *pDescriptorHeapDesc, REFIID riid, void **ppvHeap) override {return native->CreateDescriptorHeap(pDescriptorHeapDesc, riid, ppvHeap);}
    UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType) override {return native->GetDescriptorHandleIncrementSize(DescriptorHeapType);}
    HRESULT STDMETHODCALLTYPE CreateRootSignature(UINT nodeMask, const void *pBlobWithRootSignature, SIZE_T blobLengthInBytes, REFIID riid, void **ppvRootSignature) override {return native->CreateRootSignature(nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, ppvRootSignature);}
    void STDMETHODCALLTYPE CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {return native->CreateConstantBufferView(pDesc, DestDescriptor);}
    void STDMETHODCALLTYPE CreateShaderResourceView(ID3D12Resource *pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {return native->CreateShaderResourceView(pResource, pDesc, DestDescriptor);}
    void STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D12Resource *pResource, ID3D12Resource *pCounterResource, const D3D12_UNORDERED_ACCESS_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {return native->CreateUnorderedAccessView(pResource, pCounterResource, pDesc, DestDescriptor);}
    void STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Resource *pResource, const D3D12_RENDER_TARGET_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {return native->CreateRenderTargetView(pResource, pDesc, DestDescriptor);}
    void STDMETHODCALLTYPE CreateDepthStencilView(ID3D12Resource *pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {return native->CreateDepthStencilView(pResource, pDesc, DestDescriptor);}
    void STDMETHODCALLTYPE CreateSampler(const D3D12_SAMPLER_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {return native->CreateSampler(pDesc, DestDescriptor);}
    void STDMETHODCALLTYPE CopyDescriptors(UINT NumDestDescriptorRanges, const D3D12_CPU_DESCRIPTOR_HANDLE *pDestDescriptorRangeStarts, const UINT *pDestDescriptorRangeSizes, UINT NumSrcDescriptorRanges, const D3D12_CPU_DESCRIPTOR_HANDLE *pSrcDescriptorRangeStarts, const UINT *pSrcDescriptorRangeSizes, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) override {return native->CopyDescriptors(NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes, NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes, DescriptorHeapsType);}
    void STDMETHODCALLTYPE CopyDescriptorsSimple(UINT NumDescriptors, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart, D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) override {return native->CopyDescriptorsSimple(NumDescriptors, DestDescriptorRangeStart, SrcDescriptorRangeStart, DescriptorHeapsType);}
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC *pResourceDescs) override {return native->GetResourceAllocationInfo(visibleMask, numResourceDescs, pResourceDescs);}
    D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE GetCustomHeapProperties(UINT nodeMask, D3D12_HEAP_TYPE heapType) override {return native->GetCustomHeapProperties(nodeMask, heapType);}
    HRESULT STDMETHODCALLTYPE CreateCommittedResource(const D3D12_HEAP_PROPERTIES *pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialResourceState, const D3D12_CLEAR_VALUE *pOptimizedClearValue, REFIID riidResource, void **ppvResource) override {return native->CreateCommittedResource(pHeapProperties, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue, riidResource, ppvResource);}
    HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC *pDesc, REFIID riid, void **ppvHeap) override {return native->CreateHeap(pDesc, riid, ppvHeap);}
    HRESULT STDMETHODCALLTYPE CreatePlacedResource(ID3D12Heap *pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *pOptimizedClearValue, REFIID riid, void **ppvResource) override {return native->CreatePlacedResource(pHeap, HeapOffset, pDesc, InitialState, pOptimizedClearValue, riid, ppvResource);}
    HRESULT STDMETHODCALLTYPE CreateReservedResource(const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *pOptimizedClearValue, REFIID riid, void **ppvResource) override {return native->CreateReservedResource(pDesc, InitialState, pOptimizedClearValue, riid, ppvResource);}
    HRESULT STDMETHODCALLTYPE CreateSharedHandle(ID3D12DeviceChild *pObject, const SECURITY_ATTRIBUTES *pAttributes, DWORD Access, LPCWSTR Name, HANDLE *pHandle) override {return native->CreateSharedHandle(pObject, pAttributes, Access, Name, pHandle);}
    HRESULT STDMETHODCALLTYPE OpenSharedHandle(HANDLE NTHandle, REFIID riid, void **ppvObj) override {return native->OpenSharedHandle(NTHandle, riid, ppvObj);}
    HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(LPCWSTR Name, DWORD Access, HANDLE *pNTHandle) override {return native->OpenSharedHandleByName(Name, Access, pNTHandle);}
    HRESULT STDMETHODCALLTYPE MakeResident(UINT NumObjects, ID3D12Pageable *const *ppObjects) override {return native->MakeResident(NumObjects, ppObjects);}
    HRESULT STDMETHODCALLTYPE Evict(UINT NumObjects, ID3D12Pageable *const *ppObjects) override {return native->Evict(NumObjects, ppObjects);}
    HRESULT STDMETHODCALLTYPE CreateFence(UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void **ppFence) override {return native->CreateFence(InitialValue, Flags, riid, ppFence);}
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason(void) override {return native->GetDeviceRemovedReason();}
    void STDMETHODCALLTYPE GetCopyableFootprints(const D3D12_RESOURCE_DESC *pResourceDesc, UINT FirstSubresource, UINT NumSubresources, UINT64 BaseOffset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *pLayouts, UINT *pNumRows, UINT64 *pRowSizeInBytes, UINT64 *pTotalBytes) override {return native->GetCopyableFootprints(pResourceDesc, FirstSubresource, NumSubresources, BaseOffset, pLayouts, pNumRows, pRowSizeInBytes, pTotalBytes);}
    HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC *pDesc, REFIID riid, void **ppvHeap) override {return native->CreateQueryHeap(pDesc, riid, ppvHeap);}
    HRESULT STDMETHODCALLTYPE SetStablePowerState(BOOL Enable) override {return native->SetStablePowerState(Enable);}
    HRESULT STDMETHODCALLTYPE CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC *pDesc, ID3D12RootSignature *pRootSignature, REFIID riid, void **ppvCommandSignature) override {return native->CreateCommandSignature(pDesc, pRootSignature, riid, ppvCommandSignature);}
    void STDMETHODCALLTYPE GetResourceTiling(ID3D12Resource *pTiledResource, UINT *pNumTilesForEntireResource, D3D12_PACKED_MIP_INFO *pPackedMipDesc, D3D12_TILE_SHAPE *pStandardTileShapeForNonPackedMips, UINT *pNumSubresourceTilings, UINT FirstSubresourceTilingToGet, D3D12_SUBRESOURCE_TILING *pSubresourceTilingsForNonPackedMips) override {return native->GetResourceTiling(pTiledResource, pNumTilesForEntireResource, pPackedMipDesc, pStandardTileShapeForNonPackedMips, pNumSubresourceTilings, FirstSubresourceTilingToGet, pSubresourceTilingsForNonPackedMips);}
    LUID STDMETHODCALLTYPE GetAdapterLuid(void) override {return native->GetAdapterLuid();}
};
