#include "RtpReceiver.h"
#include "MediaPipeline.h"
#include "NetworkClient.h"
#include "Protocol.h"

#include <windows.h>

#include <array>
#include <chrono>
#include <future>

namespace {
std::string SocketError(const char* prefix) {
    return std::string(prefix) + " (WSA " + std::to_string(WSAGetLastError()) + ")";
}

void AppendStartCode(std::vector<std::uint8_t>& output) {
    output.push_back(0);
    output.push_back(0);
    output.push_back(0);
    output.push_back(1);
}
}

RtpReceiver::~RtpReceiver() {
    Stop();
}

bool RtpReceiver::Bind(std::uint16_t port, std::string& error) {
    Stop();
    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET) {
        error = SocketError("Could not create RTP socket");
        return false;
    }

    int receiveBuffer = 2 * 1024 * 1024;
    setsockopt(socket_, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&receiveBuffer), sizeof(receiveBuffer));
    DWORD timeout = 500;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        error = SocketError("Could not bind RTP socket");
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return false;
    }
    return true;
}

bool RtpReceiver::Start(int width, int height, int fps,
                        MediaPipeline* pipeline,
                        NetworkClient* network,
                        std::function<void(const std::string&)> fatalCallback,
                        std::string& error) {
    if (socket_ == INVALID_SOCKET) {
        error = "RTP socket is not bound";
        return false;
    }
    if (running_.exchange(true)) {
        error = "RTP receiver is already running";
        return false;
    }

    std::promise<std::pair<bool, std::string>> promise;
    auto future = promise.get_future();
    {
        std::lock_guard lock(recoveryMutex_);
        idrRequested_ = false;
    }
    recoveryThread_ = std::thread(&RtpReceiver::RecoveryLoop, this, network);
    thread_ = std::thread(&RtpReceiver::Loop, this, width, height, fps,
                          pipeline, std::move(fatalCallback), std::move(promise));

    if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        error = "Timed out starting the Windows media pipeline";
        Stop();
        return false;
    }
    auto result = future.get();
    if (!result.first) {
        error = result.second;
        Stop();
        return false;
    }
    return true;
}

void RtpReceiver::Stop() {
    running_.store(false);
    recoveryCv_.notify_all();
    if (socket_ != INVALID_SOCKET) {
        shutdown(socket_, SD_BOTH);
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    recoveryCv_.notify_all();
    if (recoveryThread_.joinable()) {
        recoveryThread_.join();
    }
}

void RtpReceiver::ScheduleIdr() {
    {
        std::lock_guard lock(recoveryMutex_);
        idrRequested_ = true;
    }
    recoveryCv_.notify_one();
}

void RtpReceiver::RecoveryLoop(NetworkClient* network) {
    while (running_.load()) {
        std::unique_lock lock(recoveryMutex_);
        recoveryCv_.wait(lock, [&]() { return !running_.load() || idrRequested_; });
        if (!running_.load()) break;
        idrRequested_ = false;
        lock.unlock();
        network->RequestIdr();
    }
}

void RtpReceiver::Loop(int width, int height, int fps,
                       MediaPipeline* pipeline,
                       std::function<void(const std::string&)> fatalCallback,
                       std::promise<std::pair<bool, std::string>> ready) {
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool comInitialized = SUCCEEDED(com);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
        ready.set_value({false, "CoInitializeEx failed for RTP thread"});
        running_.store(false);
        return;
    }

    std::string pipelineError;
    if (!pipeline->Start(width, height, fps, pipelineError)) {
        ready.set_value({false, pipelineError});
        running_.store(false);
        if (comInitialized) CoUninitialize();
        return;
    }
    ready.set_value({true, {}});

    std::array<std::uint8_t, 2048> packet{};
    std::vector<std::uint8_t> accessUnit;
    accessUnit.reserve(2 * 1024 * 1024);

    bool haveSequence = false;
    std::uint16_t lastSequence = 0;
    bool haveTimestamp = false;
    std::uint32_t currentTimestamp = 0;
    bool corrupted = false;
    bool fuOpen = false;
    auto lastIdrRequest = std::chrono::steady_clock::time_point::min();
    auto lastPacket = std::chrono::steady_clock::now();

    auto requestRecovery = [&]() {
        pipeline->NotifyDiscontinuity();
        auto now = std::chrono::steady_clock::now();
        if (now - lastIdrRequest >= std::chrono::milliseconds(500)) {
            ScheduleIdr();
            lastIdrRequest = now;
        }
    };

    auto finalize = [&](bool forceCorrupt) -> bool {
        bool bad = corrupted || forceCorrupt || fuOpen;
        if (bad) {
            requestRecovery();
        } else if (!accessUnit.empty()) {
            std::string error;
            if (!pipeline->PushAccessUnit(accessUnit, currentTimestamp, error)) {
                if (fatalCallback) fatalCallback(error);
                return false;
            }
        }
        accessUnit.clear();
        corrupted = false;
        fuOpen = false;
        haveTimestamp = false;
        return true;
    };

    while (running_.load()) {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        int received = recvfrom(socket_, reinterpret_cast<char*>(packet.data()),
                                static_cast<int>(packet.size()), 0,
                                reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (received == SOCKET_ERROR) {
            int code = WSAGetLastError();
            if (!running_.load()) break;
            if (code == WSAETIMEDOUT || code == WSAEWOULDBLOCK) {
                auto silentFor = std::chrono::steady_clock::now() - lastPacket;
                if (silentFor >= std::chrono::seconds(1)) ScheduleIdr();
                if (silentFor >= std::chrono::seconds(5)) {
                    if (fatalCallback) fatalCallback("No RTP video received for 5 seconds");
                    break;
                }
                continue;
            }
            if (fatalCallback) fatalCallback(SocketError("RTP receive failed"));
            break;
        }
        if (received < 13) continue;
        lastPacket = std::chrono::steady_clock::now();

        std::uint8_t b0 = packet[0];
        std::uint8_t b1 = packet[1];
        if ((b0 >> 6) != 2 || (b0 & 0x0f) != 0 || (b0 & 0x10) != 0) continue;
        if ((b1 & 0x7f) != dopecam::protocol::kRtpPayloadType) continue;

        bool marker = (b1 & 0x80) != 0;
        std::uint16_t sequence = static_cast<std::uint16_t>((packet[2] << 8) | packet[3]);
        std::uint32_t timestamp = (static_cast<std::uint32_t>(packet[4]) << 24)
                | (static_cast<std::uint32_t>(packet[5]) << 16)
                | (static_cast<std::uint32_t>(packet[6]) << 8)
                | static_cast<std::uint32_t>(packet[7]);

        bool gap = haveSequence && sequence != static_cast<std::uint16_t>(lastSequence + 1);
        lastSequence = sequence;
        haveSequence = true;

        if (!haveTimestamp) {
            currentTimestamp = timestamp;
            haveTimestamp = true;
            corrupted = gap;
        } else if (timestamp != currentTimestamp) {
            if (!finalize(gap)) break;
            currentTimestamp = timestamp;
            haveTimestamp = true;
            corrupted = gap;
        } else if (gap) {
            corrupted = true;
            fuOpen = false;
        }

        const std::uint8_t* payload = packet.data() + 12;
        int payloadSize = received - 12;
        if (payloadSize <= 0) continue;
        int nalType = payload[0] & 0x1f;

        if (nalType >= 1 && nalType <= 23) {
            AppendStartCode(accessUnit);
            accessUnit.insert(accessUnit.end(), payload, payload + payloadSize);
            fuOpen = false;
        } else if (nalType == 28 && payloadSize >= 2) {
            std::uint8_t fuHeader = payload[1];
            bool start = (fuHeader & 0x80) != 0;
            bool end = (fuHeader & 0x40) != 0;
            std::uint8_t reconstructed = static_cast<std::uint8_t>((payload[0] & 0xe0) | (fuHeader & 0x1f));

            if (start) {
                if (fuOpen) corrupted = true;
                AppendStartCode(accessUnit);
                accessUnit.push_back(reconstructed);
                accessUnit.insert(accessUnit.end(), payload + 2, payload + payloadSize);
                fuOpen = !end;
            } else if (!fuOpen) {
                corrupted = true;
            } else {
                accessUnit.insert(accessUnit.end(), payload + 2, payload + payloadSize);
                if (end) fuOpen = false;
            }
        } else {
            corrupted = true;
        }

        if (accessUnit.size() > 16 * 1024 * 1024) {
            accessUnit.clear();
            corrupted = true;
            fuOpen = false;
        }

        if (marker) {
            if (!finalize(false)) break;
        }
    }

    pipeline->Stop();
    running_.store(false);
    recoveryCv_.notify_all();
    if (comInitialized) CoUninitialize();
}
