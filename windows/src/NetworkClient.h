#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct DiscoveredPhone {
    std::string ip;
    std::string model;
    std::uint16_t controlPort = 0;
};

struct CameraDesc {
    std::string id;
    int lensFacing = 0;
    float zoomMin = 1.0f;
    float zoomMax = 1.0f;
    bool logical = false;
};

struct CameraChoice {
    std::string cameraId;
    std::wstring label;
    float initialZoom = 1.0f;
    float zoomMin = 1.0f;
    float zoomMax = 1.0f;
};

struct StartInfo {
    int width = 0;
    int height = 0;
    int fps = 0;
    int bitrate = 0;
};

class NetworkClient {
public:
    NetworkClient() = default;
    ~NetworkClient();

    static bool Discover(DiscoveredPhone& phone, std::string& error);

    bool Connect(const std::string& ip, std::uint16_t port, std::string& error);
    void Close();
    bool IsConnected() const;

    bool GetCameras(std::vector<CameraDesc>& cameras, std::string& error);
    bool StartStream(const std::string& cameraId, const std::string& preset,
                     std::uint16_t udpPort, float zoom, StartInfo& info, std::string& error);
    bool StopStream(std::string& error);
    bool SetZoom(float zoom, std::string& error);
    bool RequestIdr();

private:
    bool Command(const std::string& line, std::string& response, std::string& error);
    bool SendLineLocked(const std::string& line, std::string& error);
    bool ReadLineLocked(std::string& line, std::string& error);
    void CloseLocked();

    mutable std::mutex mutex_;
    SOCKET socket_ = INVALID_SOCKET;
    std::string receiveBuffer_;
};

std::wstring Utf8ToWide(const std::string& input);
std::string WideToUtf8(const std::wstring& input);
std::vector<std::string> SplitTabs(const std::string& line);
