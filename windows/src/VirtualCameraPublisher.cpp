#include "VirtualCameraPublisher.h"

#include <algorithm>
#include <cstdio>

namespace {
std::string Win32Text(const char* where, DWORD code) {
    char buffer[160]{};
    std::snprintf(buffer, sizeof(buffer), "%s failed (Win32 %lu)", where,
                  static_cast<unsigned long>(code));
    return buffer;
}

std::wstring ModuleDirectory() {
    wchar_t path[MAX_PATH]{};
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};

    std::wstring value(path, path + length);
    std::size_t slash = value.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    value.resize(slash);
    return value;
}
}

VirtualCameraPublisher::~VirtualCameraPublisher() {
    Stop();
}

bool VirtualCameraPublisher::Load(std::string& error) {
    if (module_) return true;

    std::wstring directory = ModuleDirectory();
    if (directory.empty()) {
        error = "Could not resolve the DopeCam executable directory";
        return false;
    }

    std::wstring dllPath = directory + L"\\DopeCamVirtualCamera.dll";
    module_ = LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module_) {
        error = Win32Text("LoadLibrary(DopeCamVirtualCamera.dll)", GetLastError());
        return false;
    }

    createCamera_ = reinterpret_cast<CreateCameraFn>(GetProcAddress(module_, "scCreateCamera"));
    deleteCamera_ = reinterpret_cast<DeleteCameraFn>(GetProcAddress(module_, "scDeleteCamera"));
    sendFrame_ = reinterpret_cast<SendFrameFn>(GetProcAddress(module_, "scSendFrame"));
    isConnected_ = reinterpret_cast<IsConnectedFn>(GetProcAddress(module_, "scIsConnected"));

    if (!createCamera_ || !deleteCamera_ || !sendFrame_ || !isConnected_) {
        error = "DopeCamVirtualCamera.dll does not expose the expected sender API";
        Stop();
        return false;
    }
    return true;
}

bool VirtualCameraPublisher::Start(int width, int height, int fps, std::string& error) {
    Stop();
    if (width <= 0 || height <= 0 || (width % 4) != 0 || (height % 4) != 0) {
        error = "Virtual camera dimensions must be positive multiples of four";
        return false;
    }

    if (!Load(error)) return false;

    camera_ = createCamera_(width, height, static_cast<float>(fps > 0 ? fps : 30));
    if (!camera_) {
        error = "Could not create the DirectShow virtual-camera sender buffer";
        Stop();
        return false;
    }

    width_ = width;
    height_ = height;
    bgr_.resize(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 3u);
    return true;
}

void VirtualCameraPublisher::Stop() {
    if (camera_ && deleteCamera_) {
        deleteCamera_(camera_);
    }
    camera_ = nullptr;
    createCamera_ = nullptr;
    deleteCamera_ = nullptr;
    sendFrame_ = nullptr;
    isConnected_ = nullptr;
    width_ = 0;
    height_ = 0;
    bgr_.clear();
    bgr_.shrink_to_fit();

    if (module_) {
        FreeLibrary(module_);
        module_ = nullptr;
    }
}

bool VirtualCameraPublisher::IsConnected() const {
    return camera_ && isConnected_ && isConnected_(camera_);
}

bool VirtualCameraPublisher::PublishBgra(const std::uint8_t* bgra, unsigned int rowPitch,
                                         std::string& error) {
    if (!camera_ || !sendFrame_ || !bgra) {
        error = "Virtual camera publisher is not initialized";
        return false;
    }

    const unsigned int minimumPitch = static_cast<unsigned int>(width_) * 4u;
    if (rowPitch < minimumPitch) {
        error = "Unexpected BGRA row pitch from D3D11";
        return false;
    }

    const std::size_t bgrStride = static_cast<std::size_t>(width_) * 3u;
    for (int y = 0; y < height_; ++y) {
        const std::uint8_t* source = bgra + static_cast<std::size_t>(y) * rowPitch;
        std::uint8_t* destination = bgr_.data() + static_cast<std::size_t>(y) * bgrStride;

        for (int x = 0; x < width_; ++x) {
            destination[static_cast<std::size_t>(x) * 3u + 0u] = source[static_cast<std::size_t>(x) * 4u + 0u];
            destination[static_cast<std::size_t>(x) * 3u + 1u] = source[static_cast<std::size_t>(x) * 4u + 1u];
            destination[static_cast<std::size_t>(x) * 3u + 2u] = source[static_cast<std::size_t>(x) * 4u + 2u];
        }
    }

    sendFrame_(camera_, bgr_.data());
    return true;
}
