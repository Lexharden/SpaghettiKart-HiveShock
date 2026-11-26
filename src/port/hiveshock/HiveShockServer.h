#pragma once

// Network half of the HiveShock bridge. Knows nothing about the game: it hands complete text lines to a callback and
// sends text lines back. One thread accepts and reads, another delivers the events, so a missing or slow app on the
// event port never delays the commands. Stop() always joins both.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class HiveShockServer {
  public:
    using LineHandler = std::function<void(const std::string& line)>;     // called on the network thread
    using RejectHandler = std::function<void(const std::string& reason)>; // a connection refused before any line

    HiveShockServer(LineHandler onLine, RejectHandler onReject);
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
    void RunEvents();
    bool DeliverEvents(const std::string& payload);

    LineHandler mOnLine;
    RejectHandler mOnReject;
    std::thread mThread;
    std::thread mEventThread;
    std::atomic<bool> mRun{ false };
    std::atomic<uint32_t> mClients{ 0 };
    std::mutex mEventMutex;
    std::condition_variable mEventReady;
    std::deque<std::string> mEvents;
    intptr_t mListen = -1;
    uint16_t mEventPort = 0;
    bool mNetReady = false;
};
