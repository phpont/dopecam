#pragma once

#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class MediaPipeline;
class NetworkClient;

class RtpReceiver {
public:
    RtpReceiver() = default;
    ~RtpReceiver();

    bool Bind(std::uint16_t port, std::string& error);
    bool Start(int width, int height, int fps,
               MediaPipeline* pipeline,
               NetworkClient* network,
               std::function<void(const std::string&)> fatalCallback,
               std::string& error);
    void Stop();

private:
    void Loop(int width, int height, int fps,
              MediaPipeline* pipeline,
              std::function<void(const std::string&)> fatalCallback,
              std::promise<std::pair<bool, std::string>> ready);
    void RecoveryLoop(NetworkClient* network);
    void ScheduleIdr();

    SOCKET socket_ = INVALID_SOCKET;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::thread recoveryThread_;
    std::mutex recoveryMutex_;
    std::condition_variable recoveryCv_;
    bool idrRequested_ = false;
};
