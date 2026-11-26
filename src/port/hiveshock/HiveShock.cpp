#include "HiveShock.h"
#include "HiveShockEffects.h"
#include "HiveShockServer.h"
#include "port/net/LanSession.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>

#include <libultraship.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace HiveShock {
namespace {

using Json = nlohmann::json;

// Nothing is ever dropped to make room (a viewer paid for it): when the queue is full the NEWEST command is rejected.
constexpr size_t kMaxQueue = 1024;
constexpr size_t kMaxUserChars = 22;
constexpr size_t kMaxNotices = 6;
constexpr int kTicksPerSecond = 60;
constexpr int kMaxSeconds = 120;
constexpr int kQueueReportTicks = 30;

struct Command {
    const Action* action = nullptr;
    std::string user;
    int value = 0;   // 0 = default of the action
    int seconds = 0; // 0 = default of the action
    int player = -1; // -1 = decided by the "target" setting
};

std::mutex sMutex; // guards the queue, the status and the notices
std::deque<Command> sQueue;
uint32_t sGeneration = 0; // bumped when the queue is cleared
std::deque<Notice> sNotices;
Status sStatus;
std::string sToken;

std::unique_ptr<HiveShockServer> sServer;
std::chrono::steady_clock::time_point sLastRun{};
int sFalls = 0;
bool sWasRunning = false;
int sReportTimer = 0;
std::string sLastReport;

void Reject(const std::string& reason) {
    SPDLOG_WARN("[HiveShock] rejected: {}", reason);
    std::lock_guard<std::mutex> lock(sMutex);
    sStatus.rejected++;
    sStatus.lastRejected = reason;
}

// Cuts on a character boundary, so a name never ends in half an emoji, and drops control characters.
std::string CleanUser(const std::string& name) {
    std::string out;
    size_t chars = 0;
    for (size_t i = 0; i < name.size() && chars < kMaxUserChars; chars++) {
        const auto lead = static_cast<unsigned char>(name[i]);
        const size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        if (lead >= 0x20 && lead != 0x7F) {
            out += name.substr(i, length);
        }
        i += length;
    }
    return out;
}

int ReadInt(const Json& json, const char* key, int min, int max) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_number()) {
        return 0;
    }
    const double value = it->get<double>();
    return std::isfinite(value) ? static_cast<int>(std::clamp(std::round(value), double(min), double(max))) : 0;
}

void Push(Command command) {
    {
        std::lock_guard<std::mutex> lock(sMutex);
        if (sQueue.size() < kMaxQueue) {
            sQueue.push_back(std::move(command));
            sStatus.received++;
            return;
        }
    }
    Reject("queue full");
}

// Network thread. No CVar access here.
void OnLine(const std::string& line) {
    const Json json = Json::parse(line, nullptr, false);
    if (!json.is_object() || !json.contains("action") || !json["action"].is_string()) {
        Reject("invalid JSON");
        return;
    }

    std::string token;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        token = sToken;
    }
    if (!token.empty() && !(json.contains("token") && json["token"].is_string() && json["token"] == token)) {
        Reject("bad token");
        return;
    }

    const std::string id = json["action"].get<std::string>();
    if (id == "clear_queue" || id == "clear_pending") {
        ClearPending();
        return;
    }

    Command command;
    command.action = FindAction(id);
    if (command.action == nullptr) {
        Reject("unknown action '" + CleanUser(id) + "'");
        return;
    }
    if (json.contains("user") && json["user"].is_string()) {
        command.user = CleanUser(json["user"].get<std::string>());
    }
    command.value = ReadInt(json, "value", 1, 1000);
    command.seconds = ReadInt(json, "duration", 1, kMaxSeconds);
    command.player = ReadInt(json, "player", 1, 4) - 1; // 1..4 on the wire, -1 if absent
    Push(std::move(command));
}

bool CategoryEnabled(const Action& action) {
    const std::string cvar = std::string("gHiveShock.Cat.") + action.category;
    return CVarGetInteger(cvar.c_str(), 1) != 0;
}

// -1 when nobody can be affected right now.
int PickPlayer(const Command& command) {
    const int target = command.player >= 0 ? command.player : CVarGetInteger("gHiveShock.Target", 0);
    if (target >= 0 && target < 4) {
        // A configured player who is not in this race falls back to player one.
        const int player = target < HumanCount() ? target : 0;
        return CanAffect(player) ? player : -1;
    }

    std::array<int, 4> able{};
    size_t count = 0;
    for (int i = 0; i < HumanCount(); i++) {
        if (CanAffect(i)) {
            able[count++] = i;
        }
    }
    return count > 0 ? able[static_cast<size_t>(std::chrono::steady_clock::now().time_since_epoch().count() / 1000) % count] : -1;
}

void Announce(const Command& command) {
    std::lock_guard<std::mutex> lock(sMutex);
    const std::string user = command.user.empty() ? "anon" : command.user;
    sStatus.executed++;
    sStatus.lastAction = command.action->id;
    sStatus.lastUser = user;
    sNotices.push_back({ user, command.action->label, command.action->good, Now() });
    if (sNotices.size() > kMaxNotices) {
        sNotices.pop_front();
    }
}

// Game thread. Tries the queue in order; within a lane nobody overtakes.
void Drain() {
    const int gapMs = CVarGetInteger("gHiveShock.MinGapMs", 250);
    const auto now = std::chrono::steady_clock::now();
    if (now - sLastRun < std::chrono::milliseconds(gapMs)) {
        return;
    }

    // Take the queue out so effects never run with the lock held; what the network adds meanwhile goes behind.
    std::deque<Command> work;
    uint32_t generation;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        work.swap(sQueue);
        generation = sGeneration;
    }

    std::array<bool, static_cast<size_t>(Lane::Count)> blocked{};
    bool ran = false;
    for (auto it = work.begin(); it != work.end() && !ran;) {
        const Action& action = *it->action;
        if (!CategoryEnabled(action)) {
            Reject(std::string("category disabled: ") + action.id);
            it = work.erase(it);
            continue;
        }

        bool& laneBlocked = blocked[static_cast<size_t>(action.lane)];
        const int player = laneBlocked ? -1 : PickPlayer(*it);
        if (player < 0) {
            laneBlocked = true;
            ++it;
            continue;
        }

        Request request;
        request.player = player;
        request.count = std::clamp(it->value > 0 ? it->value : action.defaultValue, 1, action.maxValue);
        if (action.lane == Lane::Trap) {
            request.count = std::min(request.count, MaxTraps());
        }
        request.ticks = (it->seconds > 0 ? it->seconds : action.defaultSeconds) * kTicksPerSecond;
        request.user = it->user.c_str();
        const Result result = action.run(action, request);
        if (result != Result::Applied) {
            laneBlocked = result == Result::Deferred;
            ++it;
            continue;
        }

        SPDLOG_INFO("[HiveShock] {} <- {} (P{})", action.id, it->user.empty() ? "anon" : it->user, player + 1);
        MarkRaceModified();
        Announce(*it);
        it = work.erase(it);
        ran = true;
    }
    if (ran) {
        sLastRun = now; // one effect per gap: a burst of gifts arrives as a show, not as a single frame
    }

    std::lock_guard<std::mutex> lock(sMutex);
    if (generation != sGeneration) {
        return; // cleared meanwhile
    }
    sQueue.insert(sQueue.begin(), std::make_move_iterator(work.begin()), std::make_move_iterator(work.end()));
}

void SendEvent(const Json& event) {
    if (sServer) {
        sServer->SendEvent(event.dump());
    }
}

void ReportQueue() {
    if (++sReportTimer < kQueueReportTicks) {
        return;
    }
    sReportTimer = 0;
    size_t pending;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        pending = sQueue.size();
    }
    const Json event = { { "event", "spawn_queue" },
                         { "pending", pending },
                         { "active", LiveTraps() },
                         { "load", LiveTraps() },
                         { "max", MaxTraps() } };
    std::string line = event.dump();
    if (line != sLastReport) {
        sLastReport = line;
        SendEvent(event);
    }
}

} // namespace

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Apply() {
    sServer.reset(); // stops and joins

    const bool enabled = CVarGetInteger("gHiveShock.Enabled", 0) != 0;
    const int port = std::clamp(CVarGetInteger("gHiveShock.Port", 43000), 1024, 65535);
    const int eventPort = std::clamp(CVarGetInteger("gHiveShock.EventPort", 43002), 1024, 65535);
    const bool lan = CVarGetInteger("gHiveShock.AllowLAN", 0) != 0;

    {
        std::lock_guard<std::mutex> lock(sMutex);
        sToken = CVarGetString("gHiveShock.Token", "");
        sStatus.state = HiveShockServer::IsSupported() ? State::Disabled : State::Unsupported;
        sStatus.error.clear();
        sStatus.port = port;
        sStatus.lan = lan;
    }
    sLastReport.clear();

    if (!HiveShockServer::IsSupported()) {
        return;
    }
    if (!enabled) {
        ClearPending(); // turning it off is deliberate
        ResetEffects();
        return;
    }

    auto server = std::make_unique<HiveShockServer>(OnLine);
    const std::string error = server->Start(static_cast<uint16_t>(port), lan, static_cast<uint16_t>(eventPort));
    std::lock_guard<std::mutex> lock(sMutex);
    if (!error.empty()) {
        SPDLOG_ERROR("[HiveShock] {}", error);
        sStatus.state = State::Error;
        sStatus.error = error;
        return;
    }
    sServer = std::move(server);
    sStatus.state = State::Listening;
    SPDLOG_INFO("[HiveShock] listening on {}:{}", lan ? "0.0.0.0" : "127.0.0.1", port);
}

void Shutdown() {
    sServer.reset();
}

Status GetStatus() {
    std::lock_guard<std::mutex> lock(sMutex);
    Status status = sStatus;
    status.pending = sQueue.size();
    status.clients = sServer ? sServer->ClientCount() : 0;
    status.falls = sFalls;
    return status;
}

std::vector<Notice> GetNotices() {
    std::lock_guard<std::mutex> lock(sMutex);
    return { sNotices.begin(), sNotices.end() };
}

void QueueTest(const std::string& action) {
    Command command;
    command.action = FindAction(action);
    command.user = "Test";
    if (command.action != nullptr) {
        Push(std::move(command));
    }
}

void ClearPending() {
    std::lock_guard<std::mutex> lock(sMutex);
    sQueue.clear();
    sGeneration++;
}

} // namespace HiveShock

extern "C" void HiveShock_GameTick(void) {
    using namespace HiveShock;

    // A LAN game is simulated on every machine from the shared pads only: effects would desync it.
    if (LanSession_InGame()) {
        return;
    }

    if (!RaceRunning()) {
        // Countdown, results, or a new race: the queue waits, what was active ends.
        if (sWasRunning) {
            sWasRunning = false;
            ResetEffects();
        }
        return;
    }
    if (!sWasRunning) {
        sWasRunning = true;
        ResetEffects();
        ClearTraps(); // a retry keeps the actors of the previous attempt
    }
    // Without the bridge there can still be test effects to run, but it costs nothing when the queue is empty.
    TickEffects();
    Drain();

    if (sServer) {
        for (int i = 0; i < HumanCount(); i++) {
            if (ConsumeFall(i)) {
                sFalls++;
                SendEvent({ { "event", "player_death" }, { "deaths", sFalls } });
            }
        }
        ReportQueue();
    }
}

extern "C" void HiveShock_WorldCleared(void) {
    HiveShock::ForgetActors();
}

extern "C" void HiveShock_FilterPad(int controllerIndex, unsigned short* buttons, signed char* stickX,
                                    signed char* stickY) {
    if (LanSession_InGame()) {
        return;
    }
    uint16_t b = *buttons;
    int8_t x = *stickX;
    int8_t y = *stickY;
    HiveShock::FilterPad(controllerIndex, b, x, y);
    *buttons = b;
    *stickX = x;
    *stickY = y;
}
