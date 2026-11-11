#include "pch.h"
#include <integration/HoYoShade.h>
#include <integration/HoYoShadeNaturalAssociation.h>
#include <atomic>

#include "Dx12FrameSource.h"

#include <dlssnr/DlssNrFeature_Dx12.h>
#include <resource_tracking/GenericDepth_Dx12.h>

namespace native
{

namespace
{
// The colour format to read the swap chain's buffer through (a typeless buffer needs a typed view).
DXGI_FORMAT ViewFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return format;
    }
}

ColorSpace ToColorSpace(DXGI_COLOR_SPACE_TYPE type)
{
    switch (type)
    {
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        return ColorSpace::ScRgb;
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        return ColorSpace::Pq;
    default:
        return ColorSpace::Srgb;
    }
}
} // namespace

void Dx12FrameSource::SetPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, ID3D12Device* device)
{
    if (_swapChain != swapChain || _queue != queue || _device != device) {
        static std::atomic<uint64_t> next{0};
        _generation = ++next; // physical binding changes, never a notification
    }
    _swapChain = swapChain;
    _queue = queue;
    _device = device;
}

void Dx12FrameSource::Release()
{
    if (_backBuffer != nullptr)
    {
        _backBuffer->Release();
        _backBuffer = nullptr;
    }
}

AcquireStatus Dx12FrameSource::Acquire(FrameInput& input)
{
    Release();

    if (HoYoShade::ColorConsumers(_swapChain) != HoYoShade::ConsumerAccess::Safe)
        return AcquireStatus::Unavailable;
    if (_swapChain == nullptr || _queue == nullptr)
        return AcquireStatus::Unavailable;

    // The game has an upscaler of its own: this is not the case the producer is for, and the pictures before and after a
    // change would be compared across it.
    const bool gameUpscaler = GenericDepthDx12::GameCallsUpscaler();
    const bool colorStage = HoYoShade::Selected(HoYoShade::Stage::BeforeNr);
    if (gameUpscaler && !colorStage)
        return AcquireStatus::WaitingForUpscaler;

    IDXGISwapChain3* chain3 = nullptr;
    UINT index = 0;

    if (SUCCEEDED(_swapChain->QueryInterface(IID_PPV_ARGS(&chain3))))
    {
        index = chain3->GetCurrentBackBufferIndex();
        chain3->Release();
    }

    if (FAILED(_swapChain->GetBuffer(index, IID_PPV_ARGS(&_backBuffer))))
        return AcquireStatus::Unavailable;

    const D3D12_RESOURCE_DESC desc = _backBuffer->GetDesc();

    input = FrameInput {};
    input.api = Api::D3D12;
    input.frame = ++_frame;
    input.naturalDevice = reinterpret_cast<uint64_t>(_device);
    input.naturalBackbuffer = reinterpret_cast<uint64_t>(_backBuffer);
    // BeforeSR belongs only to the owned virtual consumer, never to an
    // external game NGX list. Capture association for its real source buffer;
    // the eventual SR input may be the owner's separate typed color copy.
    const bool virtualBeforeSr = !gameUpscaler && Config::Instance()->DlssNrNativeMotion.value_or_default() &&
        Config::Instance()->DlssNrNativeUpscaler.value_or_default() && HoYoShade::Selected(HoYoShade::Stage::BeforeSr);
    if (colorStage || virtualBeforeSr)
        input.naturalSource = HoYoShade::NaturalAssociation::Source::Capture(_swapChain, _device,
            _backBuffer, index, reinterpret_cast<uint64_t>(static_cast<IFrameSource*>(this)), 1, input.frame);
    input.colorOnly = colorStage && (gameUpscaler || !Config::Instance()->DlssNrNativeMotion.value_or_default() ||
        !Config::Instance()->DlssNrEnabled.value_or_default());
    input.picture = _backBuffer;
    input.pictureFormat = ViewFormat(desc.Format);
    input.pictureState = D3D12_RESOURCE_STATE_PRESENT;
    input.colorSpace = ToColorSpace(DlssNr::NativeInputColourSpace(_swapChain, desc.Format));
    input.colorTransferKnown = State::Instance().outputColorSpace.valid;
    if (virtualBeforeSr)
    {
        // Existing FinishedPictureColorSpace metadata, written on this exact
        // chain by the real color-space call. NativeInputColourSpace's default
        // and another chain's global valid bit cannot authorize BeforeSR SDR.
        constexpr GUID colorSpaceKey = {0x34a31e7b, 0x84c5, 0x44ef, {0xa7, 0x4d, 0x6b, 0xd3, 0x60, 0x8c, 0xe5, 0x22}};
        DXGI_COLOR_SPACE_TYPE space = DXGI_COLOR_SPACE_CUSTOM;
        UINT bytes = sizeof(space);
        input.colorTransferKnown = SUCCEEDED(_swapChain->GetPrivateData(colorSpaceKey, &bytes, &space)) &&
            bytes == sizeof(space) && space == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        if (input.colorTransferKnown) input.colorSpace = ColorSpace::Srgb;
    }
    input.width = (uint32_t) desc.Width;
    input.height = desc.Height;

    // The scene's depth as the finder copied it this frame (none yet: the producer then runs the flow only), resolved
    // on the producer's queue when the game's buffer is multisampled.
    if (input.colorOnly) return AcquireStatus::Ready;
    const auto depth = GenericDepthDx12::ResolvedSnapshot(_queue);

    if (depth.valid)
    {
        input.depthCount = (std::min)(depth.copyCount, (int) FrameInput::kMaxDepthCopies);

        for (int i = 0; i < input.depthCount; ++i)
            input.depth[i] = depth.copies[i];

        input.depthView = depth.viewFormat;
        input.depthWidth = depth.width;
        input.depthHeight = depth.height;
        input.depthReversed = depth.reversed;
    }

    return AcquireStatus::Ready;
}

void Dx12FrameSource::Return(const FrameInput& input, const FrameOutput& output)
{
    (void) input;
    if (output.uncertain) {
        // Retention is not safety. Deny this physical chain/color to ALL
        // subsequent NR, FG, overlay and Present consumers, even when disabled.
        HoYoShade::QuarantineColor(_swapChain, _queue, _device, _backBuffer, _generation);
        Release(); // isolation owns its strong COM hold, no silent raw leak
        return;
    }

    // Zero-copy: the producer worked on the back buffer in place, on the queue the game presents from, so the order of the queue
    // already puts its work before the present.
    Release();
}

} // namespace native
