#include "MediaPipeline.h"

#include <wmcodecdsp.h>
#include <mferror.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {
std::string HrText(const char* where, HRESULT hr) {
    char buffer[128]{};
    std::snprintf(buffer, sizeof(buffer), "%s failed (0x%08lX)", where, static_cast<unsigned long>(hr));
    return buffer;
}

RECT FitRect(UINT outputWidth, UINT outputHeight, UINT sourceWidth, UINT sourceHeight, int rotation) {
    double displayWidth = static_cast<double>(sourceWidth);
    double displayHeight = static_cast<double>(sourceHeight);
    if (rotation == 90 || rotation == 270) {
        std::swap(displayWidth, displayHeight);
    }

    double sourceAspect = displayWidth / displayHeight;
    double outputAspect = static_cast<double>(outputWidth) / static_cast<double>(outputHeight);
    LONG width = static_cast<LONG>(outputWidth);
    LONG height = static_cast<LONG>(outputHeight);
    LONG left = 0;
    LONG top = 0;

    if (outputAspect > sourceAspect) {
        width = static_cast<LONG>(outputHeight * sourceAspect);
        left = static_cast<LONG>((outputWidth - width) / 2);
    } else {
        height = static_cast<LONG>(outputWidth / sourceAspect);
        top = static_cast<LONG>((outputHeight - height) / 2);
    }
    return RECT{left, top, left + width, top + height};
}
}

MediaPipeline::MediaPipeline(HWND previewWindow) : previewWindow_(previewWindow) {}

MediaPipeline::~MediaPipeline() {
    Stop();
}

bool MediaPipeline::Start(int width, int height, int fps, std::string& error) {
    Stop();
    streamWidth_ = width;
    streamHeight_ = height;
    fps_ = fps > 0 ? fps : 30;

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        error = HrText("MFStartup", hr);
        return false;
    }
    mfStarted_ = true;

    if (!CreateGraphics(error) || !CreateDecoder(error)
            || !virtualCameraPublisher_.Start(width, height, fps_, error)) {
        Stop();
        return false;
    }
    return true;
}

void MediaPipeline::Stop() {
    if (decoder_) {
        decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        decoder_.Reset();
    }
    virtualCameraPublisher_.Stop();
    ResetVirtualCameraProcessor();
    ResetVideoProcessor();
    outputView_.Reset();
    renderTarget_.Reset();
    backBuffer_.Reset();
    swapChain_.Reset();
    dxgiManager_.Reset();
    videoContext1_.Reset();
    videoContext_.Reset();
    videoDevice_.Reset();
    context_.Reset();
    device_.Reset();
    swapWidth_ = swapHeight_ = 0;
    haveBaseTimestamp_ = false;
    if (mfStarted_) {
        MFShutdown();
        mfStarted_ = false;
    }
}

bool MediaPipeline::CreateGraphics(std::string& error) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL featureLevel{};
    HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &device_,
            &featureLevel,
            &context_);
    if (FAILED(hr)) {
        error = HrText("D3D11CreateDevice", hr);
        return false;
    }

    hr = device_.As(&videoDevice_);
    if (FAILED(hr)) {
        error = HrText("ID3D11VideoDevice", hr);
        return false;
    }
    hr = context_.As(&videoContext_);
    if (FAILED(hr)) {
        error = HrText("ID3D11VideoContext", hr);
        return false;
    }
    context_.As(&videoContext1_);

    hr = MFCreateDXGIDeviceManager(&dxgiResetToken_, &dxgiManager_);
    if (FAILED(hr)) {
        error = HrText("MFCreateDXGIDeviceManager", hr);
        return false;
    }
    hr = dxgiManager_->ResetDevice(device_.Get(), dxgiResetToken_);
    if (FAILED(hr)) {
        error = HrText("IMFDXGIDeviceManager::ResetDevice", hr);
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(device_.As(&dxgiDevice))
            || FAILED(dxgiDevice->GetAdapter(&adapter))
            || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        error = "Could not obtain DXGI factory";
        return false;
    }

    RECT rect{};
    GetClientRect(previewWindow_, &rect);
    UINT width = static_cast<UINT>(std::max<LONG>(1, rect.right - rect.left));
    UINT height = static_cast<UINT>(std::max<LONG>(1, rect.bottom - rect.top));

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    hr = factory->CreateSwapChainForHwnd(device_.Get(), previewWindow_, &desc, nullptr, nullptr, &swapChain_);
    if (FAILED(hr)) {
        error = HrText("CreateSwapChainForHwnd", hr);
        return false;
    }
    factory->MakeWindowAssociation(previewWindow_, DXGI_MWA_NO_ALT_ENTER);
    return EnsureSwapChain(width, height, error);
}

bool MediaPipeline::CreateDecoder(std::string& error) {
    HRESULT hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&decoder_));
    if (FAILED(hr)) {
        error = HrText("H.264 decoder creation", hr);
        return false;
    }

    ComPtr<IMFAttributes> attributes;
    if (SUCCEEDED(decoder_->GetAttributes(&attributes)) && attributes) {
        attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
    }

    hr = decoder_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                  reinterpret_cast<ULONG_PTR>(dxgiManager_.Get()));
    if (FAILED(hr)) {
        error = HrText("H.264 decoder D3D manager", hr);
        return false;
    }

    ComPtr<IMFMediaType> inputType;
    hr = MFCreateMediaType(&inputType);
    if (FAILED(hr)) {
        error = HrText("MFCreateMediaType", hr);
        return false;
    }
    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, streamWidth_, streamHeight_);
    MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, fps_, 1);
    MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = decoder_->SetInputType(0, inputType.Get(), 0);
    if (FAILED(hr)) {
        error = HrText("H.264 decoder input type", hr);
        return false;
    }
    if (!SelectDecoderOutputType(error)) {
        return false;
    }

    MFT_OUTPUT_STREAM_INFO info{};
    hr = decoder_->GetOutputStreamInfo(0, &info);
    if (FAILED(hr)) {
        error = HrText("H.264 output stream info", hr);
        return false;
    }
    decoderProvidesSamples_ = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    if (!decoderProvidesSamples_) {
        error = "Windows H.264 decoder did not expose D3D output samples";
        return false;
    }

    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool MediaPipeline::SelectDecoderOutputType(std::string& error) {
    for (DWORD index = 0;; ++index) {
        ComPtr<IMFMediaType> type;
        HRESULT hr = decoder_->GetOutputAvailableType(0, index, &type);
        if (hr == MF_E_NO_MORE_TYPES) {
            break;
        }
        if (FAILED(hr)) {
            error = HrText("GetOutputAvailableType", hr);
            return false;
        }
        GUID subtype{};
        if (SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFVideoFormat_NV12) {
            hr = decoder_->SetOutputType(0, type.Get(), 0);
            if (FAILED(hr)) {
                error = HrText("H.264 decoder NV12 output type", hr);
                return false;
            }
            return true;
        }
    }
    error = "Windows H.264 decoder exposed no NV12 output type";
    return false;
}

bool MediaPipeline::PushAccessUnit(const std::vector<std::uint8_t>& data, std::uint32_t rtpTimestamp,
                                   std::string& error) {
    if (!decoder_ || data.empty()) {
        return true;
    }

    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateSample(&sample);
    if (FAILED(hr)) {
        error = HrText("MFCreateSample", hr);
        return false;
    }
    hr = MFCreateMemoryBuffer(static_cast<DWORD>(data.size()), &buffer);
    if (FAILED(hr)) {
        error = HrText("MFCreateMemoryBuffer", hr);
        return false;
    }

    BYTE* destination = nullptr;
    DWORD maximum = 0;
    hr = buffer->Lock(&destination, &maximum, nullptr);
    if (FAILED(hr) || maximum < data.size()) {
        error = HrText("Compressed buffer lock", FAILED(hr) ? hr : E_FAIL);
        return false;
    }
    std::memcpy(destination, data.data(), data.size());
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(data.size()));
    sample->AddBuffer(buffer.Get());

    if (!haveBaseTimestamp_) {
        baseTimestamp_ = rtpTimestamp;
        haveBaseTimestamp_ = true;
    }
    std::uint32_t delta = rtpTimestamp - baseTimestamp_;
    LONGLONG sampleTime = static_cast<LONGLONG>(delta) * 10'000'000LL / 90'000LL;
    sample->SetSampleTime(sampleTime);
    sample->SetSampleDuration(10'000'000LL / std::max(1, fps_));

    hr = decoder_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        if (!DrainDecoder(error)) {
            return false;
        }
        hr = decoder_->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) {
        error = HrText("H.264 ProcessInput", hr);
        return false;
    }
    return DrainDecoder(error);
}

bool MediaPipeline::DrainDecoder(std::string& error) {
    while (decoder_) {
        MFT_OUTPUT_DATA_BUFFER output{};
        output.dwStreamID = 0;
        DWORD status = 0;
        HRESULT hr = decoder_->ProcessOutput(0, 1, &output, &status);

        if (output.pEvents) {
            output.pEvents->Release();
            output.pEvents = nullptr;
        }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            return true;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (!SelectDecoderOutputType(error)) {
                return false;
            }
            continue;
        }
        if (FAILED(hr)) {
            error = HrText("H.264 ProcessOutput", hr);
            return false;
        }
        if (output.pSample) {
            ComPtr<IMFSample> sample;
            sample.Attach(output.pSample);
            if (!RenderSample(sample.Get(), error)) {
                return false;
            }
        }
    }
    return true;
}

bool MediaPipeline::RenderSample(IMFSample* sample, std::string& error) {
    ComPtr<IMFMediaBuffer> mediaBuffer;
    HRESULT hr = sample->GetBufferByIndex(0, &mediaBuffer);
    if (FAILED(hr)) {
        error = HrText("Decoded sample buffer", hr);
        return false;
    }

    ComPtr<IMFDXGIBuffer> dxgiBuffer;
    hr = mediaBuffer.As(&dxgiBuffer);
    if (FAILED(hr)) {
        error = "H.264 decoder returned a system-memory frame instead of a D3D11 surface";
        return false;
    }

    ComPtr<ID3D11Texture2D> inputTexture;
    hr = dxgiBuffer->GetResource(IID_PPV_ARGS(&inputTexture));
    if (FAILED(hr)) {
        error = HrText("Decoded D3D11 texture", hr);
        return false;
    }
    UINT subresource = 0;
    dxgiBuffer->GetSubresourceIndex(&subresource);

    D3D11_TEXTURE2D_DESC inputDesc{};
    inputTexture->GetDesc(&inputDesc);
    if (inputDesc.Format != DXGI_FORMAT_NV12) {
        error = "H.264 decoder returned a non-NV12 D3D11 texture";
        return false;
    }

    RECT client{};
    GetClientRect(previewWindow_, &client);
    UINT outputWidth = static_cast<UINT>(std::max<LONG>(1, client.right - client.left));
    UINT outputHeight = static_cast<UINT>(std::max<LONG>(1, client.bottom - client.top));
    if (!EnsureSwapChain(outputWidth, outputHeight, error)
            || !EnsureVideoProcessor(inputDesc.Width, inputDesc.Height, outputWidth, outputHeight, error)) {
        return false;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc{};
    inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputViewDesc.Texture2D.MipSlice = 0;
    inputViewDesc.Texture2D.ArraySlice = subresource;
    ComPtr<ID3D11VideoProcessorInputView> inputView;
    hr = videoDevice_->CreateVideoProcessorInputView(
            inputTexture.Get(), processorEnumerator_.Get(), &inputViewDesc, &inputView);
    if (FAILED(hr)) {
        error = HrText("Video processor input view", hr);
        return false;
    }

    const float black[4] = {0.f, 0.f, 0.f, 1.f};
    context_->ClearRenderTargetView(renderTarget_.Get(), black);

    RECT sourceRect{0, 0, static_cast<LONG>(inputDesc.Width), static_cast<LONG>(inputDesc.Height)};
    int rotation = rotation_.load(std::memory_order_relaxed);
    RECT destRect = FitRect(outputWidth, outputHeight, inputDesc.Width, inputDesc.Height, rotation);

    videoContext_->VideoProcessorSetStreamFrameFormat(
            videoProcessor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    videoContext_->VideoProcessorSetStreamAutoProcessingMode(videoProcessor_.Get(), 0, FALSE);
    videoContext_->VideoProcessorSetStreamSourceRect(videoProcessor_.Get(), 0, TRUE, &sourceRect);
    videoContext_->VideoProcessorSetStreamDestRect(videoProcessor_.Get(), 0, TRUE, &destRect);

    D3D11_VIDEO_PROCESSOR_ROTATION d3dRotation = D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY;
    if (rotation == 90) d3dRotation = D3D11_VIDEO_PROCESSOR_ROTATION_90;
    else if (rotation == 180) d3dRotation = D3D11_VIDEO_PROCESSOR_ROTATION_180;
    else if (rotation == 270) d3dRotation = D3D11_VIDEO_PROCESSOR_ROTATION_270;

    bool wantsRotation = rotation != 0;
    if (wantsRotation && (processorFeatureCaps_ & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ROTATION) == 0) {
        error = "GPU video processor does not support rotation";
        return false;
    }
    videoContext_->VideoProcessorSetStreamRotation(
            videoProcessor_.Get(), 0, wantsRotation ? TRUE : FALSE, d3dRotation);

    bool mirrorH = mirrorH_.load(std::memory_order_relaxed);
    bool mirrorV = mirrorV_.load(std::memory_order_relaxed);
    bool wantsMirror = mirrorH || mirrorV;
    if (wantsMirror) {
        if (!videoContext1_ || (processorFeatureCaps_ & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_MIRROR) == 0) {
            error = "GPU video processor does not support mirroring";
            return false;
        }
        videoContext1_->VideoProcessorSetStreamMirror(
                videoProcessor_.Get(), 0, TRUE, mirrorH ? TRUE : FALSE, mirrorV ? TRUE : FALSE);
    } else if (videoContext1_) {
        videoContext1_->VideoProcessorSetStreamMirror(videoProcessor_.Get(), 0, FALSE, FALSE, FALSE);
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.Get();
    hr = videoContext_->VideoProcessorBlt(videoProcessor_.Get(), outputView_.Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        error = HrText("VideoProcessorBlt", hr);
        return false;
    }

    hr = swapChain_->Present(0, 0);
    if (FAILED(hr)) {
        error = HrText("DXGI Present", hr);
        return false;
    }

    if (virtualCameraPublisher_.IsConnected()
            && !PublishVirtualCameraFrame(inputTexture.Get(), subresource, inputDesc.Width, inputDesc.Height, error)) {
        return false;
    }
    return true;
}

bool MediaPipeline::EnsureVirtualCameraProcessor(UINT sourceWidth, UINT sourceHeight, std::string& error) {
    if (virtualCameraProcessor_ && virtualCameraTexture_ && virtualCameraStaging_
            && virtualCameraSourceWidth_ == sourceWidth
            && virtualCameraSourceHeight_ == sourceHeight) {
        return true;
    }

    ResetVirtualCameraProcessor();

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = static_cast<UINT>(streamWidth_);
    textureDesc.Height = static_cast<UINT>(streamHeight_);
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device_->CreateTexture2D(&textureDesc, nullptr, &virtualCameraTexture_);
    if (FAILED(hr)) {
        error = HrText("virtual camera BGRA texture", hr);
        return false;
    }

    D3D11_TEXTURE2D_DESC stagingDesc = textureDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = device_->CreateTexture2D(&stagingDesc, nullptr, &virtualCameraStaging_);
    if (FAILED(hr)) {
        error = HrText("virtual camera staging texture", hr);
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputFrameRate.Numerator = fps_;
    desc.InputFrameRate.Denominator = 1;
    desc.InputWidth = sourceWidth;
    desc.InputHeight = sourceHeight;
    desc.OutputFrameRate.Numerator = fps_;
    desc.OutputFrameRate.Denominator = 1;
    desc.OutputWidth = static_cast<UINT>(streamWidth_);
    desc.OutputHeight = static_cast<UINT>(streamHeight_);
    desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    hr = videoDevice_->CreateVideoProcessorEnumerator(&desc, &virtualCameraEnumerator_);
    if (FAILED(hr)) {
        error = HrText("virtual camera video processor enumerator", hr);
        return false;
    }

    UINT inputFlags = 0;
    hr = virtualCameraEnumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &inputFlags);
    if (FAILED(hr) || (inputFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0) {
        error = FAILED(hr) ? HrText("virtual camera NV12 input support", hr)
                           : "GPU virtual-camera processor does not accept NV12";
        return false;
    }

    UINT outputFlags = 0;
    hr = virtualCameraEnumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &outputFlags);
    if (FAILED(hr) || (outputFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
        error = FAILED(hr) ? HrText("virtual camera BGRA output support", hr)
                           : "GPU virtual-camera processor cannot output BGRA";
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CAPS caps{};
    hr = virtualCameraEnumerator_->GetVideoProcessorCaps(&caps);
    if (FAILED(hr) || caps.RateConversionCapsCount == 0) {
        error = FAILED(hr) ? HrText("virtual camera video processor caps", hr)
                           : "GPU exposes no virtual-camera video processor capability";
        return false;
    }
    virtualCameraFeatureCaps_ = caps.FeatureCaps;

    hr = videoDevice_->CreateVideoProcessor(virtualCameraEnumerator_.Get(), 0, &virtualCameraProcessor_);
    if (FAILED(hr)) {
        error = HrText("virtual camera video processor", hr);
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
    outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outputDesc.Texture2D.MipSlice = 0;
    hr = videoDevice_->CreateVideoProcessorOutputView(
            virtualCameraTexture_.Get(), virtualCameraEnumerator_.Get(), &outputDesc, &virtualCameraOutputView_);
    if (FAILED(hr)) {
        error = HrText("virtual camera output view", hr);
        return false;
    }

    virtualCameraSourceWidth_ = sourceWidth;
    virtualCameraSourceHeight_ = sourceHeight;
    return true;
}

bool MediaPipeline::PublishVirtualCameraFrame(ID3D11Texture2D* source, UINT subresource,
                                               UINT sourceWidth, UINT sourceHeight, std::string& error) {
    if (!EnsureVirtualCameraProcessor(sourceWidth, sourceHeight, error)) {
        return false;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
    inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputDesc.Texture2D.MipSlice = 0;
    inputDesc.Texture2D.ArraySlice = subresource;

    ComPtr<ID3D11VideoProcessorInputView> inputView;
    HRESULT hr = videoDevice_->CreateVideoProcessorInputView(
            source, virtualCameraEnumerator_.Get(), &inputDesc, &inputView);
    if (FAILED(hr)) {
        error = HrText("virtual camera input view", hr);
        return false;
    }

    const int rotation = rotation_.load(std::memory_order_relaxed);
    const bool mirrorH = mirrorH_.load(std::memory_order_relaxed);
    const bool mirrorV = mirrorV_.load(std::memory_order_relaxed);

    if (rotation != 0 && (virtualCameraFeatureCaps_ & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ROTATION) == 0) {
        error = "GPU virtual-camera processor does not support rotation";
        return false;
    }
    if ((mirrorH || mirrorV)
            && (!videoContext1_ || (virtualCameraFeatureCaps_ & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_MIRROR) == 0)) {
        error = "GPU virtual-camera processor does not support mirroring";
        return false;
    }

    RECT sourceRect{0, 0, static_cast<LONG>(sourceWidth), static_cast<LONG>(sourceHeight)};
    RECT destinationRect = FitRect(
            static_cast<UINT>(streamWidth_), static_cast<UINT>(streamHeight_),
            sourceWidth, sourceHeight, rotation);

    D3D11_VIDEO_COLOR background{};
    background.RGBA.R = 0.0f;
    background.RGBA.G = 0.0f;
    background.RGBA.B = 0.0f;
    background.RGBA.A = 1.0f;
    videoContext_->VideoProcessorSetOutputBackgroundColor(virtualCameraProcessor_.Get(), FALSE, &background);
    videoContext_->VideoProcessorSetStreamFrameFormat(
            virtualCameraProcessor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    videoContext_->VideoProcessorSetStreamAutoProcessingMode(virtualCameraProcessor_.Get(), 0, FALSE);
    videoContext_->VideoProcessorSetStreamSourceRect(virtualCameraProcessor_.Get(), 0, TRUE, &sourceRect);
    videoContext_->VideoProcessorSetStreamDestRect(virtualCameraProcessor_.Get(), 0, TRUE, &destinationRect);

    D3D11_VIDEO_PROCESSOR_ROTATION d3dRotation = D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY;
    if (rotation == 90) d3dRotation = D3D11_VIDEO_PROCESSOR_ROTATION_90;
    else if (rotation == 180) d3dRotation = D3D11_VIDEO_PROCESSOR_ROTATION_180;
    else if (rotation == 270) d3dRotation = D3D11_VIDEO_PROCESSOR_ROTATION_270;
    videoContext_->VideoProcessorSetStreamRotation(
            virtualCameraProcessor_.Get(), 0, rotation != 0 ? TRUE : FALSE, d3dRotation);

    if (videoContext1_) {
        videoContext1_->VideoProcessorSetStreamMirror(
                virtualCameraProcessor_.Get(), 0, (mirrorH || mirrorV) ? TRUE : FALSE,
                mirrorH ? TRUE : FALSE, mirrorV ? TRUE : FALSE);
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.Get();
    hr = videoContext_->VideoProcessorBlt(
            virtualCameraProcessor_.Get(), virtualCameraOutputView_.Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        error = HrText("virtual camera VideoProcessorBlt", hr);
        return false;
    }

    context_->CopyResource(virtualCameraStaging_.Get(), virtualCameraTexture_.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context_->Map(virtualCameraStaging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        error = HrText("virtual camera staging map", hr);
        return false;
    }

    const bool published = virtualCameraPublisher_.PublishBgra(
            static_cast<const std::uint8_t*>(mapped.pData), mapped.RowPitch, error);
    context_->Unmap(virtualCameraStaging_.Get(), 0);
    return published;
}

void MediaPipeline::ResetVirtualCameraProcessor() {
    virtualCameraOutputView_.Reset();
    virtualCameraProcessor_.Reset();
    virtualCameraEnumerator_.Reset();
    virtualCameraStaging_.Reset();
    virtualCameraTexture_.Reset();
    virtualCameraSourceWidth_ = 0;
    virtualCameraSourceHeight_ = 0;
    virtualCameraFeatureCaps_ = 0;
}

bool MediaPipeline::EnsureSwapChain(UINT width, UINT height, std::string& error) {
    if (!swapChain_) {
        error = "Swap chain is not initialized";
        return false;
    }
    if (swapWidth_ == width && swapHeight_ == height && backBuffer_ && renderTarget_) {
        return true;
    }

    outputView_.Reset();
    renderTarget_.Reset();
    backBuffer_.Reset();
    ResetVideoProcessor();

    HRESULT hr = swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        error = HrText("ResizeBuffers", hr);
        return false;
    }
    hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer_));
    if (FAILED(hr)) {
        error = HrText("Swap-chain backbuffer", hr);
        return false;
    }
    hr = device_->CreateRenderTargetView(backBuffer_.Get(), nullptr, &renderTarget_);
    if (FAILED(hr)) {
        error = HrText("Render target view", hr);
        return false;
    }
    swapWidth_ = width;
    swapHeight_ = height;
    return true;
}

bool MediaPipeline::EnsureVideoProcessor(UINT sourceWidth, UINT sourceHeight,
                                         UINT outputWidth, UINT outputHeight, std::string& error) {
    if (videoProcessor_
            && processorSourceWidth_ == sourceWidth
            && processorSourceHeight_ == sourceHeight
            && processorOutputWidth_ == outputWidth
            && processorOutputHeight_ == outputHeight
            && outputView_) {
        return true;
    }

    ResetVideoProcessor();

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputFrameRate.Numerator = fps_;
    desc.InputFrameRate.Denominator = 1;
    desc.InputWidth = sourceWidth;
    desc.InputHeight = sourceHeight;
    desc.OutputFrameRate.Numerator = fps_;
    desc.OutputFrameRate.Denominator = 1;
    desc.OutputWidth = outputWidth;
    desc.OutputHeight = outputHeight;
    desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    HRESULT hr = videoDevice_->CreateVideoProcessorEnumerator(&desc, &processorEnumerator_);
    if (FAILED(hr)) {
        error = HrText("CreateVideoProcessorEnumerator", hr);
        return false;
    }

    UINT inputFormatFlags = 0;
    hr = processorEnumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &inputFormatFlags);
    if (FAILED(hr) || (inputFormatFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0) {
        error = FAILED(hr) ? HrText("CheckVideoProcessorFormat NV12", hr)
                           : "GPU video processor does not accept NV12 input";
        return false;
    }

    UINT outputFormatFlags = 0;
    hr = processorEnumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &outputFormatFlags);
    if (FAILED(hr) || (outputFormatFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
        error = FAILED(hr) ? HrText("CheckVideoProcessorFormat BGRA", hr)
                           : "GPU video processor cannot render to the preview backbuffer";
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CAPS caps{};
    hr = processorEnumerator_->GetVideoProcessorCaps(&caps);
    if (FAILED(hr) || caps.RateConversionCapsCount == 0) {
        error = FAILED(hr) ? HrText("GetVideoProcessorCaps", hr) : "GPU exposes no video processor rate conversion capability";
        return false;
    }
    processorFeatureCaps_ = caps.FeatureCaps;

    hr = videoDevice_->CreateVideoProcessor(processorEnumerator_.Get(), 0, &videoProcessor_);
    if (FAILED(hr)) {
        error = HrText("CreateVideoProcessor", hr);
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
    outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outputDesc.Texture2D.MipSlice = 0;
    hr = videoDevice_->CreateVideoProcessorOutputView(
            backBuffer_.Get(), processorEnumerator_.Get(), &outputDesc, &outputView_);
    if (FAILED(hr)) {
        error = HrText("Video processor output view", hr);
        return false;
    }

    processorSourceWidth_ = sourceWidth;
    processorSourceHeight_ = sourceHeight;
    processorOutputWidth_ = outputWidth;
    processorOutputHeight_ = outputHeight;
    return true;
}

void MediaPipeline::ResetVideoProcessor() {
    outputView_.Reset();
    videoProcessor_.Reset();
    processorEnumerator_.Reset();
    processorSourceWidth_ = processorSourceHeight_ = 0;
    processorOutputWidth_ = processorOutputHeight_ = 0;
    processorFeatureCaps_ = 0;
}

void MediaPipeline::NotifyDiscontinuity() {
    if (!decoder_) return;
    decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    haveBaseTimestamp_ = false;
}

void MediaPipeline::SetRotation(int degrees) {
    int normalized = ((degrees % 360) + 360) % 360;
    if (normalized != 0 && normalized != 90 && normalized != 180 && normalized != 270) {
        normalized = 0;
    }
    rotation_.store(normalized, std::memory_order_relaxed);
}

int MediaPipeline::Rotation() const {
    return rotation_.load(std::memory_order_relaxed);
}

bool MediaPipeline::ToggleMirrorHorizontal() {
    const bool next = !mirrorH_.load(std::memory_order_relaxed);
    mirrorH_.store(next, std::memory_order_relaxed);
    return next;
}

bool MediaPipeline::ToggleMirrorVertical() {
    const bool next = !mirrorV_.load(std::memory_order_relaxed);
    mirrorV_.store(next, std::memory_order_relaxed);
    return next;
}

bool MediaPipeline::MirrorHorizontal() const {
    return mirrorH_.load(std::memory_order_relaxed);
}

bool MediaPipeline::MirrorVertical() const {
    return mirrorV_.load(std::memory_order_relaxed);
}
