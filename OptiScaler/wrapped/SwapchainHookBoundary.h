#pragma once
#include <dxgi1_6.h>

// External DXGI layers may retain one original method for several F6 wrapper
// vtables. Keep those entry points identical and dispatch through an immutable
// common prefix, never through the concrete layout of the first wrapper seen.
// Do not add virtual implementation slots: external layers clone only the DXGI
// vtable. This table survives such cloning and adds no registry or hot-path lock.
class SwapchainHookBoundary : public IDXGISwapChain4
{
    struct Operations
    {
        HRESULT (*setFullscreen)(SwapchainHookBoundary*, BOOL, IDXGIOutput*);
        HRESULT (*getFullscreen)(SwapchainHookBoundary*, BOOL*, IDXGIOutput**);
        HRESULT (*resizeBuffers)(SwapchainHookBoundary*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
        HRESULT (*resizeTarget)(SwapchainHookBoundary*, const DXGI_MODE_DESC*);
        HRESULT (*checkColor)(SwapchainHookBoundary*, DXGI_COLOR_SPACE_TYPE, UINT*);
        HRESULT (*setColor)(SwapchainHookBoundary*, DXGI_COLOR_SPACE_TYPE);
        HRESULT (*setHdr)(SwapchainHookBoundary*, DXGI_HDR_METADATA_TYPE, UINT, void*);
    };
    const Operations* const operations_;

  protected:
    template<class T> static const Operations& OperationsFor()
    {
        static const Operations operations {
            [](SwapchainHookBoundary* p, BOOL a, IDXGIOutput* b) { return static_cast<T*>(p)->HookSetFullscreenState(a,b); },
            [](SwapchainHookBoundary* p, BOOL* a, IDXGIOutput** b) { return static_cast<T*>(p)->HookGetFullscreenState(a,b); },
            [](SwapchainHookBoundary* p, UINT a, UINT b, UINT c, DXGI_FORMAT d, UINT e) { return static_cast<T*>(p)->HookResizeBuffers(a,b,c,d,e); },
            [](SwapchainHookBoundary* p, const DXGI_MODE_DESC* a) { return static_cast<T*>(p)->HookResizeTarget(a); },
            [](SwapchainHookBoundary* p, DXGI_COLOR_SPACE_TYPE a, UINT* b) { return static_cast<T*>(p)->HookCheckColorSpaceSupport(a,b); },
            [](SwapchainHookBoundary* p, DXGI_COLOR_SPACE_TYPE a) { return static_cast<T*>(p)->HookSetColorSpace1(a); },
            [](SwapchainHookBoundary* p, DXGI_HDR_METADATA_TYPE a, UINT b, void* c) { return static_cast<T*>(p)->HookSetHDRMetaData(a,b,c); }
        };
        return operations;
    }
    explicit SwapchainHookBoundary(const Operations& operations) noexcept : operations_(&operations) {}
    ~SwapchainHookBoundary() = default;

  public:
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL a, IDXGIOutput* b) final { return operations_->setFullscreen(this,a,b); }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* a, IDXGIOutput** b) final { return operations_->getFullscreen(this,a,b); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT a, UINT b, UINT c, DXGI_FORMAT d, UINT e) final { return operations_->resizeBuffers(this,a,b,c,d,e); }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* a) final { return operations_->resizeTarget(this,a); }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE a, UINT* b) final { return operations_->checkColor(this,a,b); }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE a) final { return operations_->setColor(this,a); }
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE a, UINT b, void* c) final { return operations_->setHdr(this,a,b,c); }
};
