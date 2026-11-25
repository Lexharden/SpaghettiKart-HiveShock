#pragma once

// HiveShock bridge (https://hiveshock.yafel.dev): viewers' gifts become effects in the race.
// Protocol, CVars and actions: docs/HIVESHOCK.md. New actions go in HiveShockEffects.cpp.

#ifdef __cplusplus
extern "C" {
#endif

// Once per logic tick (60/s), before the karts are updated. Not called while the game is paused.
void HiveShock_GameTick(void);

// The world is about to drop its actors and objects.
void HiveShock_WorldCleared(void);

// Lets the active effects alter what a player is pressing. `pad` is the raw pad, before edges are computed.
void HiveShock_FilterPad(int controllerIndex, unsigned short* buttons, signed char* stickX, signed char* stickY);

#ifdef __cplusplus
}

#include <cstdint>
#include <string>
#include <vector>

namespace HiveShock {

enum class State { Disabled, Listening, Error, Unsupported };

struct Status {
    State state = State::Disabled;
    std::string error;
    std::string lastAction;
    std::string lastUser;
    std::string lastRejected;
    uint64_t received = 0;
    uint64_t executed = 0;
    uint64_t rejected = 0;
    size_t pending = 0;
    uint32_t clients = 0;
    int port = 0;
    bool lan = false;
    int falls = 0;
};

// Something to show on stream: "<user> → <label>".
struct Notice {
    std::string user;
    std::string label;
    bool good = false; // helps the player
    double time = 0.0; // steady clock, seconds
};

// Starts, stops or restarts the listener according to the CVars. Game thread only.
void Apply();
void Shutdown();

Status GetStatus();
std::vector<Notice> GetNotices();
double Now();

// Queues an action as if a viewer had sent it.
void QueueTest(const std::string& action);
void ClearPending();

} // namespace HiveShock
#endif
