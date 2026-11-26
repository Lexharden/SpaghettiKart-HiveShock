#pragma once

// LAN play by input lockstep (docs/LAN.md). Every machine runs the whole game; only the four pads travel. The host
// collects each player's pad for a frame and hands the complete frame to everyone, so all machines simulate the same
// inputs and stay identical. Requires the same build on the same platform (floats are compiled with fast-math).

#ifdef __cplusplus
extern "C" {
#endif

// Right after the pads are read. In a LAN game it replaces the four pads with the frame's agreed ones, waiting for
// the other machines if needed. Outside a game it only services the network (lobby, discovery).
void LanSession_ExchangePads(void* pads);

// At the start of each iteration of the game loop. 0 when the next lockstep frame has not arrived yet: the caller must
// not run any logic and shows the previous frame again, so the window and the menu keep working while a player is
// waited for. 1 otherwise (also outside a LAN game).
int LanSession_FrameReady(void);

// After the frame's logic and rendering: starts a pending game and reports the state hash.
void LanSession_EndFrame(void);

// The simulation is shared: settings that change the logic must not be touched locally.
int LanSession_InGame(void);

// This machine is playing with the host's save data, which must not be written to its own disk.
int LanSession_BlocksSaving(void);

#ifdef __cplusplus
}

#include <cstdint>
#include <string>
#include <vector>

namespace Lan {

constexpr uint16_t kGamePort = 43100;
constexpr uint16_t kDiscoveryPort = 43101;
constexpr int kMaxPlayers = 4;

enum class State { Idle, Joining, Lobby, InGame };

struct Player {
    bool present = false;
    std::string name;
    int pingMs = -1;
};

struct Status {
    State state = State::Idle;
    bool isHost = false;
    int localSlot = -1;
    Player players[kMaxPlayers];
    int playerCount = 0;
    uint32_t frame = 0;
    std::string waitingFor; // non-empty while the game waits for someone
    std::string lastMessage; // why the last session ended, or what is happening
    std::string hostAddress;
};

struct FoundGame {
    std::string name;
    std::string address;
    int players = 0;
    bool inGame = false;
};

bool IsSupported();
std::string BuildId();

std::string Host(const std::string& playerName);
std::string Join(const std::string& address, const std::string& playerName);
// Host only: everyone resets to the title screen together and the menus are played with the shared pads.
std::string StartGame();
void Leave();

void SearchGames();
std::vector<FoundGame> FoundGames();

Status GetStatus();

} // namespace Lan
#endif
