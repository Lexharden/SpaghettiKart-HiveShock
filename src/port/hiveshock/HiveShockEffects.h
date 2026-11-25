#pragma once

// Game half of the HiveShock bridge: the action table and everything that touches karts, items and actors.
// All of it runs on the game thread.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace HiveShock {

// Deferred: not possible right now (kart already spinning, item slot busy...). The command keeps its turn.
// Held: not possible for this one only (e.g. bomb karts in an arena). It stays queued but lets the others pass.
enum class Result { Applied, Deferred, Held };

// Commands in the same lane run in order and one at a time; a blocked lane does not hold up the others.
enum class Lane : uint8_t { Item, Steal, Boost, Hit, Trap, Timed, Rivals, World, Count };

struct Request {
    int player = 0;      // 0..3, a human player
    int count = 1;       // "value" of the command, clamped to the range of the action
    int ticks = 0;       // "duration" of the command, in logic ticks
    const char* user = "";
};

struct Action {
    const char* id;
    const char* category;
    const char* label; // shown to viewers on stream
    bool good;         // helps the player
    Lane lane;
    int defaultValue;
    int maxValue;
    int defaultSeconds;
    Result (*run)(const Action& action, const Request& request);
    int param; // meaning depends on `run`
};

const std::vector<Action>& Actions();
const Action* FindAction(const std::string& id);

// The race is under way and somebody can be affected (not menus, countdown, demo, editor, results).
bool RaceRunning();
// That player is a human who is still racing and in control of the kart.
bool CanAffect(int player);
int HumanCount();

// Advances timed effects and spawned actors. Call once per logic tick while RaceRunning().
void TickEffects();
// The race ended or was restarted: drop everything that was active.
void ResetEffects();
// The world is being cleaned: the actors and objects that were spawned no longer exist.
void ForgetActors();
// Time trials: the run no longer counts as a clean ghost.
void MarkRaceModified();
// Removes the traps that are still on the track.
void ClearTraps();

struct TimedView {
    const char* label;
    std::string user;
    bool good;
    int player;
    int ticksLeft;
    int ticksTotal;
};
std::vector<TimedView> GetTimed();

int LiveTraps();
int MaxTraps();

// True on the tick in which Lakitu starts rescuing that player.
bool ConsumeFall(int player);

void FilterPad(int player, uint16_t& buttons, int8_t& stickX, int8_t& stickY);

} // namespace HiveShock
