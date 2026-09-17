#include "NetworkClient.h"
#include "Protocol.h"

#include <windows.h>
#include <iphlpapi.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>
#include <stdexcept>

namespace {
void SetSocketTimeout(SOCKET socket, DWORD milliseconds) {
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&milliseconds), sizeof(milliseconds));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&milliseconds), sizeof(milliseconds));
}

std::string SocketError(const char* prefix) {
    return std::string(prefix) + " (WSA " + std::to_string(WSAGetLastError()) + ")";
}
}

NetworkClient::~NetworkClient() {
    Close();
}

bool NetworkClient::Discover(DiscoveredPhone& phone, std::string& error) {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        error = SocketError("Could not create discovery socket");
        return false;
    }

    BOOL broadcast = TRUE;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast), sizeof(broadcast)) == SOCKET_ERROR) {
        error = SocketError("Could not enable UDP broadcast");
        closesocket(sock);
        return false;
    }
    SetSocketTimeout(sock, 300);

    std::vector<std::uint32_t> targetAddresses;
    auto addTarget = [&](std::uint32_t networkOrderAddress) {
        if (std::find(targetAddresses.begin(), targetAddresses.end(), networkOrderAddress) == targetAddresses.end()) {
            targetAddresses.push_back(networkOrderAddress);
        }
    };
    addTarget(INADDR_BROADCAST);

    ULONG bufferLength = 15 * 1024;
    std::vector<unsigned char> adapterBuffer(bufferLength);
    auto* adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(adapterBuffer.data());
    ULONG result = GetAdaptersAddresses(
            AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, adapters, &bufferLength);
    if (result == ERROR_BUFFER_OVERFLOW) {
        adapterBuffer.resize(bufferLength);
        adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(adapterBuffer.data());
        result = GetAdaptersAddresses(
                AF_INET,
                GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                nullptr, adapters, &bufferLength);
    }

    if (result == NO_ERROR) {
        for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
                continue;
            }
            for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
                if (unicast->Address.lpSockaddr == nullptr
                        || unicast->Address.lpSockaddr->sa_family != AF_INET
                        || unicast->OnLinkPrefixLength > 30) {
                    continue;
                }

                auto* local = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
                std::uint32_t hostAddress = ntohl(local->sin_addr.s_addr);
                if ((hostAddress >> 24) == 127) {
                    continue;
                }

                unsigned prefixLength = unicast->OnLinkPrefixLength;
                std::uint32_t mask = prefixLength == 0 ? 0u : (0xFFFFFFFFu << (32u - prefixLength));
                std::uint32_t directedBroadcast = hostAddress | ~mask;
                addTarget(htonl(directedBroadcast));
            }
        }
    }

    const char* message = dopecam::protocol::kDiscoverMessage;
    const int messageLength = static_cast<int>(std::strlen(message));

    auto tryReceive = [&]() -> bool {
        WSASetLastError(0);
        std::array<char, 512> buffer{};
        sockaddr_in from{};
        int fromLen = sizeof(from);
        int received = recvfrom(sock, buffer.data(), static_cast<int>(buffer.size() - 1), 0,
                                reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (received <= 0) {
            return false;
        }

        buffer[static_cast<std::size_t>(received)] = '\0';
        auto parts = SplitTabs(std::string(buffer.data(), static_cast<std::size_t>(received)));
        if (parts.size() < 4 || parts[0] != "DOPECAM_HERE" || parts[3] != "1") {
            return false;
        }

        char ip[INET_ADDRSTRLEN]{};
        if (!inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip))) {
            return false;
        }

        try {
            int port = std::stoi(parts[2]);
            if (port < 1 || port > 65535) {
                return false;
            }
            phone.ip = ip;
            phone.model = parts[1];
            phone.controlPort = static_cast<std::uint16_t>(port);
            return true;
        } catch (...) {
            return false;
        }
    };

    for (int attempt = 0; attempt < 3; ++attempt) {
        int sentCount = 0;
        for (std::uint32_t address : targetAddresses) {
            sockaddr_in target{};
            target.sin_family = AF_INET;
            target.sin_port = htons(dopecam::protocol::kDiscoveryPort);
            target.sin_addr.s_addr = address;
            if (sendto(sock, message, messageLength, 0,
                       reinterpret_cast<sockaddr*>(&target), sizeof(target)) != SOCKET_ERROR) {
                ++sentCount;
            }
        }

        if (sentCount == 0) {
            error = SocketError("Discovery broadcast failed on all local interfaces");
            closesocket(sock);
            return false;
        }

        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
        while (std::chrono::steady_clock::now() < deadline) {
            if (tryReceive()) {
                closesocket(sock);
                return true;
            }
            int socketError = WSAGetLastError();
            if (socketError != WSAETIMEDOUT && socketError != WSAEWOULDBLOCK && socketError != 0) {
                break;
            }
        }
        Sleep(80);
    }

    closesocket(sock);
    error = "No armed DopeCam phone replied. Enter the IPv4 shown on the phone and click Connect IP.";
    return false;
}

bool NetworkClient::Connect(const std::string& ip, std::uint16_t port, std::string& error) {
    std::lock_guard lock(mutex_);
    CloseLocked();

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        error = SocketError("Could not create control socket");
        return false;
    }
    SetSocketTimeout(sock, 2500);
    BOOL noDelay = TRUE;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) {
        closesocket(sock);
        error = "Invalid IPv4 address";
        return false;
    }

    if (connect(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        error = SocketError("Could not connect to DopeCam control channel");
        closesocket(sock);
        return false;
    }

    socket_ = sock;
    receiveBuffer_.clear();
    std::string hello;
    if (!ReadLineLocked(hello, error) || hello.rfind("HELLO\tDOPECAM\t1", 0) != 0) {
        CloseLocked();
        if (error.empty()) {
            error = "Phone returned an incompatible DopeCam protocol greeting";
        }
        return false;
    }
    return true;
}

void NetworkClient::Close() {
    std::lock_guard lock(mutex_);
    CloseLocked();
}

void NetworkClient::CloseLocked() {
    if (socket_ != INVALID_SOCKET) {
        shutdown(socket_, SD_BOTH);
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
    receiveBuffer_.clear();
}

bool NetworkClient::IsConnected() const {
    std::lock_guard lock(mutex_);
    return socket_ != INVALID_SOCKET;
}

bool NetworkClient::GetCameras(std::vector<CameraDesc>& cameras, std::string& error) {
    std::lock_guard lock(mutex_);
    cameras.clear();
    if (!SendLineLocked("CAPS", error)) {
        CloseLocked();
        return false;
    }

    std::string line;
    if (!ReadLineLocked(line, error) || line.rfind("CAPS_BEGIN\t1", 0) != 0) {
        if (error.empty()) error = "Invalid CAPS response";
        CloseLocked();
        return false;
    }

    while (ReadLineLocked(line, error)) {
        if (line == "CAPS_END") {
            return !cameras.empty();
        }
        auto parts = SplitTabs(line);
        if (parts.size() == 6 && parts[0] == "CAMERA") {
            try {
                CameraDesc camera;
                camera.id = parts[1];
                camera.lensFacing = std::stoi(parts[2]);
                camera.zoomMin = std::stof(parts[3]);
                camera.zoomMax = std::stof(parts[4]);
                camera.logical = parts[5] == "1";
                cameras.push_back(camera);
            } catch (...) {
                error = "Phone returned malformed camera capabilities";
                CloseLocked();
                return false;
            }
        }
    }

    CloseLocked();
    return false;
}

bool NetworkClient::StartStream(const std::string& cameraId, const std::string& preset,
                                std::uint16_t udpPort, float zoom, StartInfo& info, std::string& error) {
    char zoomBuffer[32]{};
    std::snprintf(zoomBuffer, sizeof(zoomBuffer), "%.4f", static_cast<double>(zoom));
    std::string response;
    std::string command = "START\t" + cameraId + "\t" + preset + "\t"
            + std::to_string(udpPort) + "\t" + zoomBuffer;
    if (!Command(command, response, error)) {
        return false;
    }
    auto parts = SplitTabs(response);
    if (parts.size() != 6 || parts[0] != "OK" || parts[1] != "START") {
        error = "Unexpected START response: " + response;
        return false;
    }
    try {
        info.width = std::stoi(parts[2]);
        info.height = std::stoi(parts[3]);
        info.fps = std::stoi(parts[4]);
        info.bitrate = std::stoi(parts[5]);
    } catch (...) {
        error = "Malformed START response";
        return false;
    }
    return true;
}

bool NetworkClient::StopStream(std::string& error) {
    std::string response;
    return Command("STOP", response, error) && response.rfind("OK\tSTOP", 0) == 0;
}

bool NetworkClient::SetZoom(float zoom, std::string& error) {
    char value[32]{};
    std::snprintf(value, sizeof(value), "%.4f", static_cast<double>(zoom));
    std::string response;
    return Command(std::string("SET_ZOOM\t") + value, response, error)
            && response.rfind("OK\tSET_ZOOM", 0) == 0;
}

bool NetworkClient::RequestIdr() {
    std::string response;
    std::string error;
    return Command("REQUEST_IDR", response, error)
            && response.rfind("OK\tREQUEST_IDR", 0) == 0;
}

bool NetworkClient::Command(const std::string& line, std::string& response, std::string& error) {
    std::lock_guard lock(mutex_);
    if (!SendLineLocked(line, error) || !ReadLineLocked(response, error)) {
        CloseLocked();
        return false;
    }
    if (response.rfind("ERR\t", 0) == 0) {
        error = response.substr(4);
        return false;
    }
    return true;
}

bool NetworkClient::SendLineLocked(const std::string& line, std::string& error) {
    if (socket_ == INVALID_SOCKET) {
        error = "Control channel is not connected";
        return false;
    }
    std::string data = line + "\n";
    std::size_t offset = 0;
    while (offset < data.size()) {
        int sent = send(socket_, data.data() + offset, static_cast<int>(data.size() - offset), 0);
        if (sent <= 0) {
            error = SocketError("Control send failed");
            return false;
        }
        offset += static_cast<std::size_t>(sent);
    }
    return true;
}

bool NetworkClient::ReadLineLocked(std::string& line, std::string& error) {
    while (true) {
        std::size_t newline = receiveBuffer_.find('\n');
        if (newline != std::string::npos) {
            line = receiveBuffer_.substr(0, newline);
            receiveBuffer_.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }

        std::array<char, 1024> chunk{};
        int received = recv(socket_, chunk.data(), static_cast<int>(chunk.size()), 0);
        if (received <= 0) {
            error = SocketError("Control receive failed");
            return false;
        }
        receiveBuffer_.append(chunk.data(), static_cast<std::size_t>(received));
        if (receiveBuffer_.size() > 64 * 1024) {
            error = "Control response exceeded safety limit";
            return false;
        }
    }
}

std::wstring Utf8ToWide(const std::string& input) {
    if (input.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
    std::wstring output(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), count);
    return output;
}

std::string WideToUtf8(const std::wstring& input) {
    if (input.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    std::string output(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), count, nullptr, nullptr);
    return output;
}

std::vector<std::string> SplitTabs(const std::string& line) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        std::size_t pos = line.find('\t', start);
        if (pos == std::string::npos) {
            parts.push_back(line.substr(start));
            break;
        }
        parts.push_back(line.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}
