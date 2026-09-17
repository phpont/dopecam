#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "VirtualCameraPublisher.h"

class MediaPipeline {
public:
    explicit MediaPipeline(HWND previewWindow);
    ~MediaPipeline();

    bool Start(int width, int height, int fps, std::string& error);
    void Stop();
    bool PushAccessUnit(const std::vector<std::uint8_t>& data, std::uint32_t rtpTimestamp, std::string& error);
    void NotifyDiscontinuity();

    void SetRotation(int degrees);
    int Rotation() const;
    bool ToggleMirrorHorizontal();
    bool ToggleMirrorVertical();
    bool MirrorHorizontal() const;
    bool MirrorVertical() const;

private:
    bool CreateGraphics(std::string& error);
    bool CreateDecoder(std::string& error);
    bool SelectDecoderOutputType(std::string& error);
    bool DrainDecoder(std::string& error);
    bool RenderSample(IMFSample* sample, std::string& error);
    bool EnsureSwapChain(UINT width, UINT height, std::string& error);
    bool EnsureVideoProcessor(UINT sourceWidth, UINT sourceHeight, UINT outputWidth, UINT outputHeight,
                              std::string& error);
    void ResetVideoProcessor();

    bool EnsureVirtualCameraProcessor(UINT sourceWidth, UINT sourceHeight, std::string& error);
    bool PublishVirtualCameraFrame(ID3D11Texture2D* source, UINT subresource, UINT sourceWidth, UINT sourceHeight,
                                   std::string& error);
    void ResetVirtualCameraProcessor();

    HWND previewWindow_ = nullptr;
    int streamWidth_ = 0;
    int streamHeight_ = 0;
    int fps_ = 30;
    bool mfStarted_ = false;
    bool decoderProvidesSamples_ = false;
    bool haveBaseTimestamp_ = false;
    std::uint32_t baseTimestamp_ = 0;

    std::atomic<int> rotation_{0};
    std::atomic<bool> mirrorH_{false};
    std::atomic<bool> mirrorV_{false};

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> videoContext_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext1> videoContext1_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> dxgiManager_;
    UINT dxgiResetToken_ = 0;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTarget_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outputView_;
    UINT swapWidth_ = 0;
    UINT swapHeight_ = 0;

    Microsoft::WRL::ComPtr<IMFTransform> decoder_;

    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> processorEnumerator_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> videoProcessor_;
    UINT processorSourceWidth_ = 0;
    UINT processorSourceHeight_ = 0;
    UINT processorOutputWidth_ = 0;
    UINT processorOutputHeight_ = 0;
    UINT processorFeatureCaps_ = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> virtualCameraTexture_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> virtualCameraStaging_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> virtualCameraEnumerator_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> virtualCameraProcessor_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> virtualCameraOutputView_;
    UINT virtualCameraSourceWidth_ = 0;
    UINT virtualCameraSourceHeight_ = 0;
    UINT virtualCameraFeatureCaps_ = 0;
    VirtualCameraPublisher virtualCameraPublisher_;
};
