#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

class VirtualCameraPublisher {
public:
    VirtualCameraPublisher() = default;
    ~VirtualCameraPublisher();

    bool Start(int width, int height, int fps, std::string& error);
    void Stop();
    bool IsConnected() const;
    bool PublishBgra(const std::uint8_t* bgra, unsigned int rowPitch, std::string& error);

    int Width() const { return width_; }
    int Height() const { return height_; }

private:
    using CameraHandle = void*;
    using CreateCameraFn = CameraHandle (__cdecl*)(int, int, float);
    using DeleteCameraFn = void (__cdecl*)(CameraHandle);
    using SendFrameFn = void (__cdecl*)(CameraHandle, const void*);
    using IsConnectedFn = bool (__cdecl*)(CameraHandle);

    bool Load(std::string& error);

    HMODULE module_ = nullptr;
    CameraHandle camera_ = nullptr;
    CreateCameraFn createCamera_ = nullptr;
    DeleteCameraFn deleteCamera_ = nullptr;
    SendFrameFn sendFrame_ = nullptr;
    IsConnectedFn isConnected_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    std::vector<std::uint8_t> bgr_;
};
