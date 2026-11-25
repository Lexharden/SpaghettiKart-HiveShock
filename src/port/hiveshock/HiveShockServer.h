#pragma once

// Network half of the HiveShock bridge. Knows nothing about the game: it hands complete text lines to a callback and
// sends text lines back. Everything runs on one thread, so there is nothing to detach and Stop() always joins.

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class HiveShockServer {
  public:
    using LineHandler = std::function<void(const std::string& line)>; // called on the network thread

    explicit HiveShockServer(LineHandler onLine);
    ~HiveShockServer();
    HiveShockServer(const HiveShockServer&) = delete;
    HiveShockServer& operator=(const HiveShockServer&) = delete;

    static bool IsSupported();

    // Empty string on success, otherwise the reason.
    std::string Start(uint16_t port, bool lan, uint16_t eventPort);
    void Stop();

    // Best effort: the line is dropped when the bridge is not listening. Callable from any thread.
    void SendEvent(std::string line);

    uint32_t ClientCount() const {
        return mClients;
    }

  private:
    void Run();
    void FlushEvents();

    LineHandler mOnLine;
    std::thread mThread;
    std::atomic<bool> mRun{ false };
    std::atomic<uint32_t> mClients{ 0 };
    std::mutex mEventMutex;
    std::deque<std::string> mEvents;
    intptr_t mListen = -1;
    uint16_t mEventPort = 0;
    bool mNetReady = false;
};
