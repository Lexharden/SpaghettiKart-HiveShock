#include "LanSession.h"
#include "LanReplay.h"
#include "LanSocket.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>
#include <libultraship.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

extern "C" {
#include "macros.h"
#include "defines.h"
#include "main.h"
#include "save.h"
#include "save_data.h"
#include "buffers/random.h"
extern s16 sIsController1Unplugged;
}
#include "port/Game.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kMagic = 0x314C4B53; // "SKL1"
constexpr int kInputDelay = 2;          // frames between reading a pad and using it
constexpr int kRedundantFrames = 8;     // every packet repeats this many frames, so a lost datagram costs nothing
constexpr int kRing = 256;              // frames kept in memory (must be > the furthest a peer can run ahead)
constexpr int kHashEvery = 30;
constexpr auto kKeepAlive = std::chrono::milliseconds(250);
constexpr auto kLobbyTimeout = std::chrono::seconds(5);
constexpr auto kGameTimeout = std::chrono::seconds(10);
constexpr auto kJoinTimeout = std::chrono::seconds(5);
constexpr auto kResendWhileWaiting = std::chrono::milliseconds(4);
constexpr auto kFrameWaitBudget = std::chrono::milliseconds(8); // per displayed frame, see LanSession_FrameReady
// Receive buffers. The start packet (settings and the 512-byte save) is the largest, about 1.3 KB; it used to be read
// into 1500 bytes, so a few more synced settings would have made every start fail silently.
constexpr size_t kMaxDatagram = 8192;
// Above this a datagram is split into IP fragments; it still arrives, but losing any fragment loses all of it.
constexpr size_t kUnfragmentedDatagram = 1400;

enum PacketType : uint8_t {
    kHello = 1,
    kWelcome,
    kReject,
    kLobby,
    kStart,
    kInput,
    kFrames,
    kHash,
    kEnd,
    kKeepAliveMsg,
    kKeepAliveAck,
    kBye,
    kDiscover = 20,
    kAnnounce,
};

// Settings that change the simulation. The host's values are applied on every machine for the session.
struct SyncedCVar {
    const char* name;
    bool isFloat;
};
constexpr SyncedCVar kSyncedCVars[] = {
    { "gNoCulling", false },          { "gDisableLod", false },        { "gHarderCPU", false },
    { "gShellsShootStraight", false }, { "gNoWallColision", false },    { "gMultiplayerNoFeatureCuts", false },
    { "gSkipIntro", false },          { "gMinHeight", true },          { "gEnableCustomCC", false },
    { "gCustomCC", true },            { "gUniqueCharacterSelections", false }, { "gNumTrucks", false },
    { "gNumTrains", false },          { "gNumTankerTrucks", false },   { "gNumCars", false },
    { "gNumCarriages", false },       { "gNumBuses", false },          { "gHasTender", false },
    { "gGoFish", false },             { "gEnableMoonJump", false },    { "gDisableRubberbanding", false },
    { "gDisableItemboxes", false },   { "gEnableDebugMode", false },   { "gFarFrustrum", true },
    { "gFixVisuals", false },         { "gDrawSky", false },           { "gDrawTransparentTrack", false },
    { "gDrawTrackGeometry", false },  { "gDrawStaticMeshActors", false }, { "gDrawSkyActors", false },
    { "gDrawPlayers", false },        { "gDrawParticles", false },     { "gDrawObjects", false },
    { "gDrawItemBoxes", false },      { "gDrawHUD", false },           { "gDrawCPPActors", false },
    { "gDrawCActors", false },        { "gRenderCollisionMesh", false }, { "gEnhancements.Mods.AlternateAssets", false },
};
// Local-only tools that would desync the game: off for everyone during a session.
constexpr const char* kForcedOff[] = { "gFreecam", "gIsEditorEnabled" };

struct PadBytes {
    uint16_t button = 0;
    int8_t sx = 0, sy = 0, rx = 0, ry = 0;
};

// ---- Wire format (little-endian) ----

class Writer {
  public:
    explicit Writer(PacketType type) {
        Put32(kMagic);
        Put8(type);
    }
    void Put8(uint8_t v) {
        mData.push_back(v);
    }
    void Put16(uint16_t v) {
        Put8(v & 0xFF);
        Put8(v >> 8);
    }
    void Put32(uint32_t v) {
        Put16(v & 0xFFFF);
        Put16(v >> 16);
    }
    void Put64(uint64_t v) {
        Put32(static_cast<uint32_t>(v));
        Put32(static_cast<uint32_t>(v >> 32));
    }
    void PutF32(float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        Put32(bits);
    }
    void Str(const std::string& s) {
        const size_t n = std::min<size_t>(s.size(), 255);
        Put8(static_cast<uint8_t>(n));
        mData.insert(mData.end(), s.begin(), s.begin() + n);
    }
    void Bytes(const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        mData.insert(mData.end(), b, b + n);
    }
    void Pad(const PadBytes& p) {
        Put16(p.button);
        Put8(static_cast<uint8_t>(p.sx));
        Put8(static_cast<uint8_t>(p.sy));
        Put8(static_cast<uint8_t>(p.rx));
        Put8(static_cast<uint8_t>(p.ry));
    }
    const std::vector<uint8_t>& Data() const {
        return mData;
    }

  private:
    std::vector<uint8_t> mData;
};

class Reader {
  public:
    Reader(const uint8_t* data, size_t size) : mData(data), mSize(size) {
    }
    bool Ok() const {
        return mOk;
    }
    uint8_t Get8() {
        if (mPos + 1 > mSize) {
            mOk = false;
            return 0;
        }
        return mData[mPos++];
    }
    uint16_t Get16() {
        const uint16_t lo = Get8();
        return static_cast<uint16_t>(lo | (Get8() << 8));
    }
    uint32_t Get32() {
        const uint32_t lo = Get16();
        return lo | (static_cast<uint32_t>(Get16()) << 16);
    }
    uint64_t Get64() {
        const uint64_t lo = Get32();
        return lo | (static_cast<uint64_t>(Get32()) << 32);
    }
    float GetF32() {
        const uint32_t bits = Get32();
        float v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    std::string Str() {
        const size_t n = Get8();
        if (mPos + n > mSize) {
            mOk = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(mData + mPos), n);
        mPos += n;
        return s;
    }
    bool Bytes(void* out, size_t n) {
        if (mPos + n > mSize) {
            mOk = false;
            return false;
        }
        std::memcpy(out, mData + mPos, n);
        mPos += n;
        return true;
    }
    PadBytes Pad() {
        PadBytes p;
        p.button = Get16();
        p.sx = static_cast<int8_t>(Get8());
        p.sy = static_cast<int8_t>(Get8());
        p.rx = static_cast<int8_t>(Get8());
        p.ry = static_cast<int8_t>(Get8());
        return p;
    }

  private:
    const uint8_t* mData;
    size_t mSize;
    size_t mPos = 0;
    bool mOk = true;
};

// ---- Session state (game thread only; the menu reads it through GetStatus under the mutex) ----

struct Peer {
    bool present = false;
    LanAddress address;
    std::string name;
    Clock::time_point lastHeard{};
    int pingMs = -1;
    bool started = false; // has sent input since the game started
};

struct FrameSlot {
    uint32_t frame = UINT32_MAX;
    uint8_t present = 0; // bit per slot
    PadBytes pads[Lan::kMaxPlayers];
};

struct PendingHash {
    uint32_t frame = UINT32_MAX;
    uint64_t hash = 0;
};

struct SavedCVar {
    std::string name;
    bool existed = false;
    bool isFloat = false;
    int32_t i = 0;
    float f = 0.0f;
};

struct Session {
    std::mutex mutex; // guards what the menu reads and the requests it makes
    Lan::State state = Lan::State::Idle;
    bool isHost = false;
    int localSlot = -1;
    std::string localName;
    std::string lastMessage;

    LanSocket socket;
    LanSocket discovery; // host: answers searches. Client: sends them.
    LanAddress hostAddress;
    Peer peers[Lan::kMaxPlayers];
    Clock::time_point lastKeepAlive{};
    Clock::time_point joinStarted{};

    // Game
    bool startPending = false; // apply the start at the end of this frame
    bool started = false;
    uint16_t seed = 0;
    std::vector<std::pair<std::string, float>> hostCVars; // name, value (ints travel as floats)
    std::vector<bool> hostCVarIsFloat;
    std::vector<bool> hostCVarIsSet; // unset on the host means "the game's default" (never 0: gDraw* default to on)
    int32_t hostTickLogic = 2;
    std::vector<uint8_t> hostSave;
    std::vector<SavedCVar> savedCVars;
    int32_t savedTickLogic = 2;
    uint8_t savedControllerBits = 0;
    s16 savedController1Unplugged = 0;
    bool usingHostSave = false;
    uint32_t frame = 0; // next lockstep frame to consume
    std::array<FrameSlot, kRing> frames{};
    std::array<uint64_t, kRing> hashes{};
    std::array<uint32_t, kRing> hashFrames{};
    // Host: clients' hashes that arrived before the host had simulated that frame.
    std::array<std::array<PendingHash, kRing>, Lan::kMaxPlayers> clientHashes{};
    std::string waitingFor;
    Clock::time_point waitStart{};    // since when frame `frame` is awaited; empty when not waiting
    Clock::time_point lastInputSend{}; // client
    std::vector<uint8_t> startPacket; // host: resent until each client confirms

    std::vector<Lan::FoundGame> found;
};

Session& S() {
    static Session s;
    return s;
}

// Unattended runs (docs/LAN.md): SPAGHETTI_LAN_HOST=<name>, SPAGHETTI_LAN_JOIN=<address>,
// SPAGHETTI_LAN_AUTOSTART=<players> (host starts once that many are in), SPAGHETTI_LAN_LOG=<file> (hash per frame).
struct AutoConfig {
    bool checked = false;
    int autostart = 0;
    FILE* log = nullptr;
    bool dump = false;
    long long fakeDesyncFrom = -1;
};
AutoConfig sAuto;

const char* EnvValue(const char* name) {
    const char* v = std::getenv(name);
    return (v != nullptr && v[0] != '\0') ? v : nullptr;
}

bool CVarIsSet(const char* name) {
    return Ship::Context::GetRawInstance()->GetConsoleVariables()->Get(name) != nullptr;
}

std::string CleanName(std::string name) {
    name.erase(std::remove_if(name.begin(), name.end(), [](char c) { return static_cast<unsigned char>(c) < 32; }),
               name.end());
    if (name.empty()) {
        name = "Player";
    }
    return name.substr(0, 16);
}

void Send(const LanAddress& to, const Writer& w) {
    S().socket.SendTo(to, w.Data().data(), w.Data().size());
}

int PlayerCount() {
    int n = 0;
    for (const auto& p : S().peers) {
        n += p.present ? 1 : 0;
    }
    return n;
}

uint8_t PresentMask() {
    uint8_t mask = 0;
    for (int i = 0; i < Lan::kMaxPlayers; i++) {
        if (S().peers[i].present) {
            mask |= 1 << i;
        }
    }
    return mask;
}

FrameSlot& Slot(uint32_t frame) {
    FrameSlot& slot = S().frames[frame % kRing];
    if (slot.frame != frame) {
        slot = FrameSlot{};
        slot.frame = frame;
    }
    return slot;
}

PadBytes FromPad(const OSContPad& p) {
    return PadBytes{ p.button, p.stick_x, p.stick_y, p.right_stick_x, p.right_stick_y };
}

void ToPad(const PadBytes& b, OSContPad& p) {
    p = {};
    p.button = b.button;
    p.stick_x = b.sx;
    p.stick_y = b.sy;
    p.right_stick_x = b.rx;
    p.right_stick_y = b.ry;
}

// While a game lasts this machine runs with the host's settings. If anything saves the CVars meanwhile, or the game
// stops without ending the session, the host's values would stay in this machine's config. So the local values also go
// to a file, which the end of the session (or the next start, after a crash) uses to put them back.
std::string RestoreFilePath() {
    return Ship::Context::GetPathRelativeToAppDirectory("lan_restore.json");
}

void PutBack(const std::vector<SavedCVar>& cvars) {
    for (const auto& saved : cvars) {
        if (!saved.existed) {
            CVarClear(saved.name.c_str());
        } else if (saved.isFloat) {
            CVarSetFloat(saved.name.c_str(), saved.f);
        } else {
            CVarSetInteger(saved.name.c_str(), saved.i);
        }
    }
    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
}

void WriteRestoreFile(const std::vector<SavedCVar>& cvars) {
    nlohmann::json list = nlohmann::json::array();
    for (const auto& saved : cvars) {
        list.push_back({ { "name", saved.name },
                         { "existed", saved.existed },
                         { "isFloat", saved.isFloat },
                         { "i", saved.i },
                         { "f", saved.f } });
    }
    std::ofstream out(RestoreFilePath(), std::ios::trunc);
    out << list.dump(2);
    if (!out) {
        SPDLOG_WARN("[LAN] Cannot write {}: a crash during this game could leave the host's settings here",
                    RestoreFilePath());
    }
}

void RemoveRestoreFile() {
    std::error_code ignored;
    std::filesystem::remove(RestoreFilePath(), ignored);
}

// At start-up: a LAN game was still running when the game stopped last time.
void RecoverLocalSettings() {
    std::ifstream in(RestoreFilePath());
    if (!in) {
        return;
    }
    const nlohmann::json list = nlohmann::json::parse(in, nullptr, false);
    in.close();
    std::vector<SavedCVar> cvars;
    if (list.is_array()) {
        for (const auto& item : list) {
            if (!item.is_object() || !item.contains("name") || !item["name"].is_string()) {
                continue;
            }
            SavedCVar saved;
            saved.name = item["name"].get<std::string>();
            saved.existed = item.value("existed", false);
            saved.isFloat = item.value("isFloat", false);
            saved.i = item.value("i", 0);
            saved.f = item.value("f", 0.0f);
            cvars.push_back(std::move(saved));
        }
    }
    PutBack(cvars);
    RemoveRestoreFile();
    SPDLOG_INFO("[LAN] The last LAN game did not end normally: {} local settings put back", cvars.size());
}

void ApplyHostSettings() {
    Session& s = S();
    s.savedCVars.clear();
    // The host already has its own settings; clients take them for the session.
    for (size_t i = 0; i < s.hostCVars.size() && !s.isHost; i++) {
        const auto& [name, value] = s.hostCVars[i];
        SavedCVar saved{ name, CVarIsSet(name.c_str()), s.hostCVarIsFloat[i] };
        saved.i = CVarGetInteger(name.c_str(), 0);
        saved.f = CVarGetFloat(name.c_str(), 0.0f);
        s.savedCVars.push_back(saved);
    }
    for (const char* name : kForcedOff) {
        SavedCVar saved{ name, CVarIsSet(name), false };
        saved.i = CVarGetInteger(name, 0);
        s.savedCVars.push_back(saved);
    }
    // Written before any value changes, so that the file always holds this machine's own settings.
    WriteRestoreFile(s.savedCVars);
    for (size_t i = 0; i < s.hostCVars.size() && !s.isHost; i++) {
        const auto& [name, value] = s.hostCVars[i];
        if (!s.hostCVarIsSet[i]) {
            CVarClear(name.c_str());
        } else if (s.hostCVarIsFloat[i]) {
            CVarSetFloat(name.c_str(), value);
        } else {
            CVarSetInteger(name.c_str(), static_cast<int32_t>(value));
        }
    }
    for (const char* name : kForcedOff) {
        CVarSetInteger(name, 0);
    }
    s.savedTickLogic = gTickLogic;
    gTickLogic = s.hostTickLogic;
    // The menus look at which controllers are plugged in; every machine has all four for the session.
    s.savedControllerBits = gControllerBits;
    s.savedController1Unplugged = sIsController1Unplugged;
    gControllerBits = 0x0F;
    sIsController1Unplugged = false;

    if (!s.isHost && s.hostSave.size() == sizeof(SaveData)) {
        std::memcpy(&gSaveData, s.hostSave.data(), sizeof(SaveData));
        s.usingHostSave = true;
    }
}

void RestoreLocalSettings() {
    Session& s = S();
    PutBack(s.savedCVars); // and saved: whatever was written during the game is overwritten
    RemoveRestoreFile();
    s.savedCVars.clear();
    gTickLogic = s.savedTickLogic;
    gControllerBits = s.savedControllerBits;
    sIsController1Unplugged = s.savedController1Unplugged;
    if (s.usingHostSave) {
        s.usingHostSave = false;
        load_save_data(); // back to this machine's own save
    }
}

// Ends whatever is running. In a game everyone goes back to the title screen with their own settings.
void Finish(const std::string& why, bool notifyOthers) {
    Session& s = S();
    if (s.state == Lan::State::Idle) {
        return;
    }
    if (notifyOthers) {
        Writer w(s.isHost ? kEnd : kBye);
        w.Str(why);
        if (s.isHost) {
            for (int i = 1; i < Lan::kMaxPlayers; i++) {
                if (s.peers[i].present) {
                    Send(s.peers[i].address, w);
                }
            }
        } else if (s.hostAddress.IsValid()) {
            Send(s.hostAddress, w);
        }
    }
    const bool wasInGame = s.started;
    if (wasInGame) {
        RestoreLocalSettings();
        CM_RequestReset();
    }
    SPDLOG_INFO("[LAN] Session ended: {}", why);
    s.socket.Close();
    s.discovery.Close();
    s.state = Lan::State::Idle;
    s.isHost = false;
    s.localSlot = -1;
    s.started = false;
    s.startPending = false;
    s.waitingFor.clear();
    s.startPacket.clear();
    for (auto& p : s.peers) {
        p = Peer{};
    }
    s.lastMessage = why;
}

// ---- Host ----

// Host: the host's own hash for frame `f` is known. A different one from a client ends the game for everyone.
void CompareHash(int slot, uint32_t f, uint64_t clientHash) {
    Session& s = S();
    if (s.hashes[f % kRing] == clientHash) {
        return;
    }
    SPDLOG_ERROR("[LAN] Desync with {} at frame {}: host {:016x}, client {:016x}", s.peers[slot].name, f,
                 s.hashes[f % kRing], clientHash);
    Finish("Out of sync with " + s.peers[slot].name + " at frame " + std::to_string(f) +
               ". Check that everyone uses the same package and the same mods.",
           true);
}

void HostSendLobby() {
    Session& s = S();
    Writer w(kLobby);
    for (const auto& p : s.peers) {
        w.Put8(p.present ? 1 : 0);
        w.Str(p.name);
    }
    w.Put8(s.started ? 1 : 0);
    for (int i = 1; i < Lan::kMaxPlayers; i++) {
        if (s.peers[i].present) {
            Send(s.peers[i].address, w);
        }
    }
}

void HostHandle(PacketType type, Reader& r, const LanAddress& from) {
    Session& s = S();
    int slot = -1;
    for (int i = 1; i < Lan::kMaxPlayers; i++) {
        if (s.peers[i].present && s.peers[i].address == from) {
            slot = i;
        }
    }
    if (slot > 0) {
        s.peers[slot].lastHeard = Clock::now();
    }

    switch (type) {
        case kHello: {
            const std::string build = r.Str();
            const std::string name = CleanName(r.Str());
            if (!r.Ok()) {
                return;
            }
            Writer reject(kReject);
            if (build != Lan::BuildId()) {
                reject.Str("Different version: the host runs " + Lan::BuildId() + " and you run " + build +
                           ". Everyone needs the same package on the same OS.");
                Send(from, reject);
                return;
            }
            if (slot < 0) {
                if (s.started) {
                    reject.Str("The game has already started.");
                    Send(from, reject);
                    return;
                }
                for (int i = 1; i < Lan::kMaxPlayers && slot < 0; i++) {
                    if (!s.peers[i].present) {
                        slot = i;
                    }
                }
                if (slot < 0) {
                    reject.Str("The game is full (4 players).");
                    Send(from, reject);
                    return;
                }
                s.peers[slot] = Peer{ true, from, name, Clock::now(), -1, false };
                SPDLOG_INFO("[LAN] {} joined from {} as player {}", name, from.ToString(), slot + 1);
            }
            Writer welcome(kWelcome);
            welcome.Put8(static_cast<uint8_t>(slot));
            Send(from, welcome);
            HostSendLobby();
            break;
        }
        case kInput: {
            if (slot < 0 || !s.started) {
                return;
            }
            r.Get8(); // the slot the client thinks it has; the address is what counts
            const uint32_t first = r.Get32();
            const int count = r.Get8();
            s.peers[slot].started = true;
            for (int i = 0; i < count && r.Ok(); i++) {
                const PadBytes pad = r.Pad();
                const uint32_t f = first + i;
                // Only frames not yet handed out and within the ring.
                if (r.Ok() && f >= s.frame && f < s.frame + kRing - kRedundantFrames) {
                    FrameSlot& fs = Slot(f);
                    fs.pads[slot] = pad;
                    fs.present |= 1 << slot;
                }
            }
            break;
        }
        case kHash: {
            if (slot < 0) {
                return;
            }
            const uint32_t f = r.Get32();
            const uint64_t hash = r.Get64();
            if (!r.Ok() || !s.started) {
                return;
            }
            if (s.hashFrames[f % kRing] == f) {
                CompareHash(slot, f, hash);
            } else {
                // The client can simulate a frame before the host does (it only needs the host's packet): keep the
                // hash until the host has its own.
                s.clientHashes[slot][f % kRing] = { f, hash };
            }
            break;
        }
        case kKeepAliveMsg: {
            Writer ack(kKeepAliveAck);
            ack.Put32(r.Get32());
            Send(from, ack);
            break;
        }
        case kKeepAliveAck: {
            if (slot > 0) {
                const uint32_t sent = r.Get32();
                const auto now = static_cast<uint32_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
                s.peers[slot].pingMs = static_cast<int>(now - sent);
            }
            break;
        }
        case kBye: {
            if (slot > 0) {
                const std::string name = s.peers[slot].name;
                if (s.started) {
                    Finish(name + " left the game.", true);
                } else {
                    s.peers[slot] = Peer{};
                    HostSendLobby();
                }
            }
            break;
        }
        default:
            break;
    }
}

void HostService() {
    Session& s = S();
    uint8_t buffer[kMaxDatagram];
    LanAddress from;
    int got;
    while (s.state != Lan::State::Idle && (got = s.socket.Receive(buffer, sizeof(buffer), from)) > 0) {
        Reader r(buffer, static_cast<size_t>(got));
        if (r.Get32() != kMagic) {
            continue;
        }
        HostHandle(static_cast<PacketType>(r.Get8()), r, from);
    }
    // Searches from other machines on the LAN.
    while (s.state != Lan::State::Idle && (got = s.discovery.Receive(buffer, sizeof(buffer), from)) > 0) {
        Reader r(buffer, static_cast<size_t>(got));
        if (r.Get32() != kMagic || r.Get8() != kDiscover) {
            continue;
        }
        Writer w(kAnnounce);
        w.Str(s.localName);
        w.Put8(static_cast<uint8_t>(PlayerCount()));
        w.Put8(s.started ? 1 : 0);
        w.Put16(s.socket.LocalPort());
        s.discovery.SendTo(from, w.Data().data(), w.Data().size());
    }
    if (s.state == Lan::State::Idle) {
        return;
    }

    const auto now = Clock::now();
    if (now - s.lastKeepAlive >= kKeepAlive) {
        s.lastKeepAlive = now;
        Writer ping(kKeepAliveMsg);
        ping.Put32(static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()));
        for (int i = 1; i < Lan::kMaxPlayers; i++) {
            if (!s.peers[i].present) {
                continue;
            }
            Send(s.peers[i].address, ping);
            if (!s.started && now - s.peers[i].lastHeard > kLobbyTimeout) {
                SPDLOG_INFO("[LAN] {} timed out in the lobby", s.peers[i].name);
                s.peers[i] = Peer{};
            }
        }
        if (!s.started) {
            HostSendLobby();
        }
    }
    // A client that has not answered the start yet gets it again.
    if (s.started && !s.startPacket.empty()) {
        for (int i = 1; i < Lan::kMaxPlayers; i++) {
            if (s.peers[i].present && !s.peers[i].started) {
                s.socket.SendTo(s.peers[i].address, s.startPacket.data(), s.startPacket.size());
            }
        }
    }
}

void HostPublishFrames() {
    Session& s = S();
    const uint8_t mask = PresentMask();
    // The newest complete frames, oldest first; repeated in every packet.
    uint32_t last = s.frame;
    while (last + 1 < s.frame + kInputDelay + kRedundantFrames && (Slot(last + 1).present & mask) == mask) {
        last++;
    }
    if ((Slot(last).present & mask) != mask) {
        return;
    }
    const uint32_t first = (last + 1 >= static_cast<uint32_t>(kRedundantFrames)) ? last + 1 - kRedundantFrames : 0;
    Writer w(kFrames);
    w.Put32(first);
    w.Put8(static_cast<uint8_t>(last - first + 1));
    for (uint32_t f = first; f <= last; f++) {
        const FrameSlot& fs = S().frames[f % kRing];
        for (int p = 0; p < Lan::kMaxPlayers; p++) {
            w.Pad(fs.frame == f ? fs.pads[p] : PadBytes{});
        }
    }
    for (int i = 1; i < Lan::kMaxPlayers; i++) {
        if (s.peers[i].present) {
            Send(s.peers[i].address, w);
        }
    }
}

// ---- Client ----

// Sends this machine's pads up to `s.frame + lastOffset`: kInputDelay once the newest pad is stored, one less before
// (an empty slot must never travel as if it were a released pad).
void ClientSendInput(int lastOffset) {
    Session& s = S();
    const uint32_t last = s.frame + static_cast<uint32_t>(lastOffset);
    const uint32_t first = last + 1 >= static_cast<uint32_t>(kRedundantFrames) ? last + 1 - kRedundantFrames : 0;
    Writer w(kInput);
    w.Put8(static_cast<uint8_t>(s.localSlot));
    w.Put32(first);
    w.Put8(static_cast<uint8_t>(last - first + 1));
    for (uint32_t f = first; f <= last; f++) {
        const FrameSlot& fs = s.frames[f % kRing];
        w.Pad(fs.frame == f ? fs.pads[s.localSlot] : PadBytes{});
    }
    Send(s.hostAddress, w);
}

void ClientHandleStart(Reader& r) {
    Session& s = S();
    const uint16_t seed = r.Get16();
    const int32_t tickLogic = static_cast<int32_t>(r.Get32());
    const int count = r.Get16();
    std::vector<std::pair<std::string, float>> cvars;
    std::vector<bool> isFloat;
    std::vector<bool> isSet;
    for (int i = 0; i < count && r.Ok(); i++) {
        std::string name = r.Str();
        const uint8_t flags = r.Get8();
        const float value = r.GetF32();
        cvars.emplace_back(std::move(name), value);
        isFloat.push_back((flags & 1) != 0);
        isSet.push_back((flags & 2) != 0);
    }
    std::vector<uint8_t> save(r.Get16());
    if (!save.empty()) {
        r.Bytes(save.data(), save.size());
    }
    if (!r.Ok() || s.started || s.startPending) {
        return; // malformed, or a repeat of a start already taken
    }
    s.seed = seed;
    s.hostTickLogic = tickLogic;
    s.hostCVars = std::move(cvars);
    s.hostCVarIsFloat = std::move(isFloat);
    s.hostCVarIsSet = std::move(isSet);
    s.hostSave = std::move(save);
    s.startPending = true;
}

void ClientHandle(PacketType type, Reader& r, const LanAddress& from) {
    Session& s = S();
    if (from != s.hostAddress) {
        return;
    }
    s.peers[0].lastHeard = Clock::now();
    switch (type) {
        case kWelcome:
            if (s.state == Lan::State::Joining) {
                s.localSlot = r.Get8();
                s.state = Lan::State::Lobby;
                s.lastMessage.clear();
                SPDLOG_INFO("[LAN] Joined {} as player {}", from.ToString(), s.localSlot + 1);
            }
            break;
        case kReject:
            Finish(r.Str(), false);
            break;
        case kLobby:
            for (auto& p : s.peers) {
                const bool present = r.Get8() != 0;
                std::string name = r.Str();
                if (r.Ok()) {
                    p.present = present;
                    p.name = std::move(name);
                }
            }
            break;
        case kStart:
            ClientHandleStart(r);
            break;
        case kFrames: {
            if (!s.started) {
                return;
            }
            const uint32_t first = r.Get32();
            const int count = r.Get8();
            for (int i = 0; i < count; i++) {
                PadBytes pads[Lan::kMaxPlayers];
                for (auto& p : pads) {
                    p = r.Pad();
                }
                const uint32_t f = first + i;
                if (!r.Ok()) {
                    break;
                }
                if (f >= s.frame && f < s.frame + kRing - kRedundantFrames) {
                    FrameSlot& fs = Slot(f);
                    // Keep this machine's own pad (already stored) and take the agreed frame from the host.
                    std::copy(std::begin(pads), std::end(pads), fs.pads);
                    fs.present = 0xFF;
                }
            }
            break;
        }
        case kEnd:
            Finish(r.Str(), false);
            break;
        case kKeepAliveMsg: {
            Writer ack(kKeepAliveAck);
            ack.Put32(r.Get32());
            Send(from, ack);
            break;
        }
        case kKeepAliveAck: {
            const uint32_t sent = r.Get32();
            const auto now = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
            s.peers[0].pingMs = static_cast<int>(now - sent);
            break;
        }
        default:
            break;
    }
}

void ClientService() {
    Session& s = S();
    uint8_t buffer[kMaxDatagram];
    LanAddress from;
    int got;
    while (s.state != Lan::State::Idle && (got = s.socket.Receive(buffer, sizeof(buffer), from)) > 0) {
        Reader r(buffer, static_cast<size_t>(got));
        if (r.Get32() != kMagic) {
            continue;
        }
        ClientHandle(static_cast<PacketType>(r.Get8()), r, from);
    }
    if (s.state == Lan::State::Idle) {
        return;
    }

    const auto now = Clock::now();
    if (now - s.lastKeepAlive >= kKeepAlive) {
        s.lastKeepAlive = now;
        if (s.state == Lan::State::Joining) {
            Writer hello(kHello);
            hello.Str(Lan::BuildId());
            hello.Str(s.localName);
            Send(s.hostAddress, hello);
            if (now - s.joinStarted > kJoinTimeout) {
                Finish("No answer from " + s.hostAddress.ToString() +
                           ". Check the address and that the firewall allows UDP " + std::to_string(Lan::kGamePort) + ".",
                       false);
            }
            return;
        }
        Writer ping(kKeepAliveMsg);
        ping.Put32(static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()));
        Send(s.hostAddress, ping);
        if (!s.started && now - s.peers[0].lastHeard > kLobbyTimeout) {
            Finish("Lost the connection to the host.", false);
        }
    }
}

void Service() {
    if (S().isHost) {
        HostService();
    } else {
        ClientService();
    }
}

std::string NameOf(int slot) {
    const std::string& n = S().peers[slot].name;
    return n.empty() ? "player " + std::to_string(slot + 1) : n;
}

// Services the network once and reports whether frame `s.frame` is complete. While it is not, keeps track of who is
// being waited for and ends the session after kGameTimeout (then the state is Idle). `ownPadStored`: this machine's pad
// for `s.frame + kInputDelay` is already in the ring and may be sent. `sendNow`: send it without waiting for the resend
// interval (a new pad was just stored).
bool FrameArrived(bool ownPadStored, bool sendNow) {
    Session& s = S();
    Service();
    if (s.state == Lan::State::Idle) {
        return false;
    }
    const auto now = Clock::now();
    if (s.isHost) {
        HostPublishFrames();
    } else if (sendNow || now - s.lastInputSend >= kResendWhileWaiting) {
        ClientSendInput(ownPadStored ? kInputDelay : kInputDelay - 1);
        s.lastInputSend = now;
    }

    const FrameSlot& fs = s.frames[s.frame % kRing];
    const uint8_t needed = s.isHost ? PresentMask() : 0xFF;
    if (fs.frame == s.frame && (fs.present & needed) == needed) {
        s.waitStart = {};
        if (!s.waitingFor.empty()) {
            std::lock_guard lock(s.mutex);
            s.waitingFor.clear();
        }
        return true;
    }

    if (s.waitStart == Clock::time_point{}) {
        s.waitStart = now;
    }
    if (s.waitingFor.empty() && now - s.waitStart > std::chrono::milliseconds(100)) {
        std::lock_guard lock(s.mutex);
        if (s.isHost) {
            for (int i = 1; i < Lan::kMaxPlayers; i++) {
                if (s.peers[i].present && !(fs.frame == s.frame && (fs.present & (1 << i)))) {
                    s.waitingFor = NameOf(i);
                    break;
                }
            }
        } else {
            s.waitingFor = NameOf(0);
        }
    }
    if (now - s.waitStart > kGameTimeout) {
        Finish(s.waitingFor.empty() ? "Lost the connection." : s.waitingFor + " is not responding.", true);
    }
    return false;
}

// Fills `pads` with the agreed frame. LanSession_FrameReady() has normally seen it arrive already; if not, waits.
// False if the session ended meanwhile.
bool LockstepFrame(OSContPad* pads) {
    Session& s = S();
    // The menus stay usable during a game: whatever was changed there that would desync it is put back.
    gTickLogic = s.hostTickLogic;
    for (const char* name : kForcedOff) {
        if (CVarGetInteger(name, 0) != 0) {
            CVarSetInteger(name, 0);
        }
    }
    // This machine's pad goes into the frame `kInputDelay` ahead.
    FrameSlot& own = Slot(s.frame + kInputDelay);
    own.pads[s.localSlot] = FromPad(pads[0]);
    own.present |= 1 << s.localSlot;

    bool sendNow = true;
    while (!FrameArrived(true, sendNow)) {
        if (s.state == Lan::State::Idle) {
            return false;
        }
        sendNow = false;
        s.socket.WaitReadable(1);
    }

    const FrameSlot& fs = s.frames[s.frame % kRing];
    for (int i = 0; i < Lan::kMaxPlayers; i++) {
        ToPad(fs.pads[i], pads[i]);
    }
    s.frame++;
    return true;
}

void BeginGame() {
    Session& s = S();
    ApplyHostSettings();
    LanReplay_ResetToKnownState(s.seed);
    s.frames.fill(FrameSlot{});
    s.hashFrames.fill(UINT32_MAX);
    for (auto& slotHashes : s.clientHashes) {
        slotHashes.fill(PendingHash{});
    }
    // The first frames have no input yet: everyone starts with released pads.
    for (uint32_t f = 0; f < static_cast<uint32_t>(kInputDelay); f++) {
        FrameSlot& fs = Slot(f);
        fs.present = 0xFF;
    }
    s.frame = 0;
    s.waitStart = {};
    s.lastInputSend = {};
    s.started = true;
    s.startPending = false;
    {
        std::lock_guard lock(s.mutex);
        s.state = Lan::State::InGame;
    }
    SPDLOG_INFO("[LAN] Game started: {} players, seed {:04x}, this machine is player {}", PlayerCount(), s.seed,
                s.localSlot + 1);
}

} // namespace

// ---- C hooks ----

extern "C" void LanSession_ExchangePads(void* rawPads) {
    Session& s = S();
    if (!sAuto.checked) {
        sAuto.checked = true;
        RecoverLocalSettings();
        if (const char* v = EnvValue("SPAGHETTI_LAN_AUTOSTART")) {
            sAuto.autostart = std::atoi(v);
        }
        if (const char* v = EnvValue("SPAGHETTI_LAN_LOG")) {
            sAuto.log = std::fopen(v, "w");
        }
        sAuto.dump = EnvValue("SPAGHETTI_LAN_DUMP") != nullptr;
        if (const char* v = EnvValue("SPAGHETTI_LAN_FAKE_DESYNC")) {
            sAuto.fakeDesyncFrom = std::atoll(v);
        }
        std::string err;
        if (const char* v = EnvValue("SPAGHETTI_LAN_HOST")) {
            err = Lan::Host(v);
        } else if (const char* v = EnvValue("SPAGHETTI_LAN_JOIN")) {
            err = Lan::Join(v, EnvValue("SPAGHETTI_LAN_NAME") ? EnvValue("SPAGHETTI_LAN_NAME") : "Client");
        }
        if (!err.empty()) {
            SPDLOG_ERROR("[LAN] {}", err);
        }
    }
    if (s.state == Lan::State::Idle) {
        return;
    }
    auto* pads = static_cast<OSContPad*>(rawPads);
    if (!s.started) {
        Service();
        if (s.startPending) {
            // The game is reset at the end of this frame; until then nobody drives anything.
            for (int i = 0; i < Lan::kMaxPlayers; i++) {
                pads[i] = {};
            }
        }
        return;
    }
    if (!LockstepFrame(pads)) {
        for (int i = 0; i < Lan::kMaxPlayers; i++) {
            pads[i] = {};
        }
    }
}

extern "C" int LanSession_FrameReady(void) {
    Session& s = S();
    if (!s.started) {
        return 1;
    }
    // A frame a few milliseconds late still runs this time; later than that, the last image is shown again.
    const auto deadline = Clock::now() + kFrameWaitBudget;
    while (true) {
        if (FrameArrived(false, false) || s.state == Lan::State::Idle) {
            return 1; // ready, or the session ended: either way the game runs on
        }
        if (Clock::now() >= deadline) {
            return 0;
        }
        s.socket.WaitReadable(1);
    }
}

extern "C" void LanSession_EndFrame(void) {
    Session& s = S();
    if (sAuto.autostart > 0 && s.isHost && s.state == Lan::State::Lobby && !s.startPending &&
        PlayerCount() >= sAuto.autostart) {
        sAuto.autostart = 0;
        Lan::StartGame();
    }
    if (sAuto.log != nullptr && s.started && s.frame > 0) {
        std::fprintf(sAuto.log, "%u %d %d %04x %016llx", s.frame - 1, gGamestate, gRaceState, gRandomSeed16,
                     static_cast<unsigned long long>(LanReplay_StateHash()));
        if (sAuto.dump) { // the bytes of every player, to see which field differs
            for (int p = 0; p < NUM_PLAYERS; p++) {
                std::fputc(' ', sAuto.log);
                const auto* b = reinterpret_cast<const uint8_t*>(&gPlayers[p]);
                for (int i = 0; i < 0xE0; i++) {
                    std::fprintf(sAuto.log, "%02x", b[i]);
                }
            }
        }
        std::fputc('\n', sAuto.log);
        std::fflush(sAuto.log);
    }
    if (s.startPending) {
        BeginGame();
        return;
    }
    if (!s.started || s.frame == 0) {
        return;
    }
    // The frame just simulated is s.frame - 1.
    const uint32_t f = s.frame - 1;
    if (f % kHashEvery != 0) {
        return;
    }
    const uint64_t hash = LanReplay_StateHash();
    if (s.isHost) {
        s.hashes[f % kRing] = hash;
        s.hashFrames[f % kRing] = f;
        for (int slot = 1; slot < Lan::kMaxPlayers && s.started; slot++) {
            const PendingHash early = s.clientHashes[slot][f % kRing];
            if (early.frame == f) {
                s.clientHashes[slot][f % kRing] = {};
                CompareHash(slot, f, early.hash);
            }
        }
    } else {
        // Test of the desync check (docs/LAN.md): from that frame on this client reports a wrong hash.
        const long long fakeFrom = sAuto.fakeDesyncFrom;
        Writer w(kHash);
        w.Put32(f);
        w.Put64(fakeFrom >= 0 && f >= fakeFrom ? ~hash : hash);
        Send(s.hostAddress, w);
    }
}

extern "C" int LanSession_InGame(void) {
    return S().started || S().startPending ? 1 : 0;
}

extern "C" int LanSession_BlocksSaving(void) {
    return S().usingHostSave ? 1 : 0;
}

// ---- Menu API ----

namespace Lan {

bool IsSupported() {
    return LanSocket::IsSupported();
}

std::string BuildId() {
#if defined(_WIN32)
    const char* os = "windows";
#elif defined(__APPLE__)
    const char* os = "macos";
#elif defined(__linux__)
    const char* os = "linux";
#else
    const char* os = "other";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    const char* arch = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    const char* arch = "x64";
#else
    const char* arch = "other";
#endif
    return std::string(SPAGHETTI_VERSION) + " " + os + "-" + arch;
}

std::string Host(const std::string& playerName) {
    Session& s = S();
    if (s.state != State::Idle) {
        return "Already in a LAN game.";
    }
    if (std::string err = s.socket.Open(kGamePort, false); !err.empty()) {
        return err;
    }
    if (std::string err = s.discovery.Open(kDiscoveryPort, true, true); !err.empty()) {
        SPDLOG_WARN("[LAN] Discovery disabled: {}", err); // joining by IP still works
    }
    s.isHost = true;
    s.localSlot = 0;
    s.localName = CleanName(playerName);
    s.peers[0] = Peer{ true, {}, s.localName, Clock::now(), 0, true };
    s.state = State::Lobby;
    s.lastMessage.clear();
    SPDLOG_INFO("[LAN] Hosting on UDP {} as {} ({})", kGamePort, s.localName, BuildId());
    return {};
}

std::string Join(const std::string& address, const std::string& playerName) {
    Session& s = S();
    if (s.state != State::Idle) {
        return "Already in a LAN game.";
    }
    std::string host = address;
    uint16_t port = kGamePort;
    if (const size_t colon = host.rfind(':'); colon != std::string::npos) {
        port = static_cast<uint16_t>(std::atoi(host.c_str() + colon + 1));
        host.resize(colon);
    }
    LanAddress target;
    if (!LanSocket::Resolve(host, port, target)) {
        return "Cannot understand the address " + address + ".";
    }
    if (std::string err = s.socket.Open(0, false); !err.empty()) {
        return err;
    }
    s.isHost = false;
    s.hostAddress = target;
    s.localName = CleanName(playerName);
    s.peers[0] = Peer{};
    s.peers[0].lastHeard = Clock::now();
    s.joinStarted = Clock::now();
    s.lastKeepAlive = {};
    s.state = State::Joining;
    s.lastMessage = "Connecting to " + target.ToString() + "...";
    return {};
}

std::string StartGame() {
    Session& s = S();
    if (!s.isHost || s.state != State::Lobby) {
        return "Only the host can start, from the lobby.";
    }
    if (PlayerCount() < 2) {
        return "At least one more player is needed.";
    }
    s.seed = static_cast<uint16_t>(std::random_device{}());
    s.hostTickLogic = gTickLogic;
    s.hostCVars.clear();
    s.hostCVarIsFloat.clear();
    s.hostCVarIsSet.clear();
    for (const auto& cvar : kSyncedCVars) {
        s.hostCVarIsSet.push_back(CVarIsSet(cvar.name));
        const float value = cvar.isFloat ? CVarGetFloat(cvar.name, 0.0f)
                                         : static_cast<float>(CVarGetInteger(cvar.name, 0));
        s.hostCVars.emplace_back(cvar.name, value);
        s.hostCVarIsFloat.push_back(cvar.isFloat);
    }
    s.hostSave.assign(reinterpret_cast<const uint8_t*>(&gSaveData),
                      reinterpret_cast<const uint8_t*>(&gSaveData) + sizeof(SaveData));

    Writer w(kStart);
    w.Put16(s.seed);
    w.Put32(static_cast<uint32_t>(s.hostTickLogic));
    w.Put16(static_cast<uint16_t>(s.hostCVars.size()));
    for (size_t i = 0; i < s.hostCVars.size(); i++) {
        w.Str(s.hostCVars[i].first);
        w.Put8(static_cast<uint8_t>((s.hostCVarIsFloat[i] ? 1 : 0) | (s.hostCVarIsSet[i] ? 2 : 0)));
        w.PutF32(s.hostCVars[i].second);
    }
    w.Put16(static_cast<uint16_t>(s.hostSave.size()));
    w.Bytes(s.hostSave.data(), s.hostSave.size());
    s.startPacket = w.Data();
    if (s.startPacket.size() > kMaxDatagram) {
        return "The settings to share do not fit in one packet (" + std::to_string(s.startPacket.size()) + " bytes).";
    }
    if (s.startPacket.size() > kUnfragmentedDatagram) {
        SPDLOG_WARN("[LAN] Start packet is {} bytes: it travels in IP fragments", s.startPacket.size());
    }
    for (int i = 1; i < kMaxPlayers; i++) {
        s.peers[i].started = false;
        if (s.peers[i].present) {
            s.socket.SendTo(s.peers[i].address, s.startPacket.data(), s.startPacket.size());
        }
    }
    s.startPending = true;
    return {};
}

void Leave() {
    Finish("You left the LAN game.", true);
}

void SearchGames() {
    Session& s = S();
    if (s.state != State::Idle) {
        return;
    }
    if (!s.discovery.IsOpen() && !s.discovery.Open(0, true).empty()) {
        return;
    }
    s.found.clear();
    Writer w(kDiscover);
    s.discovery.SendTo(LanSocket::Broadcast(kDiscoveryPort), w.Data().data(), w.Data().size());
}

std::vector<FoundGame> FoundGames() {
    Session& s = S();
    if (s.state == State::Idle && s.discovery.IsOpen()) {
        uint8_t buffer[kMaxDatagram];
        LanAddress from;
        int got;
        while ((got = s.discovery.Receive(buffer, sizeof(buffer), from)) > 0) {
            Reader r(buffer, static_cast<size_t>(got));
            if (r.Get32() != kMagic || r.Get8() != kAnnounce) {
                continue;
            }
            FoundGame g;
            g.name = r.Str();
            g.players = r.Get8();
            g.inGame = r.Get8() != 0;
            const uint16_t port = r.Get16();
            if (!r.Ok()) {
                continue;
            }
            g.address = LanAddress{ from.ip, port }.ToString();
            auto same = std::find_if(s.found.begin(), s.found.end(),
                                     [&](const FoundGame& o) { return o.address == g.address; });
            if (same != s.found.end()) {
                *same = g;
            } else {
                s.found.push_back(g);
            }
        }
    }
    return s.found;
}

Status GetStatus() {
    Session& s = S();
    std::lock_guard lock(s.mutex);
    Status st;
    st.state = s.state;
    st.isHost = s.isHost;
    st.localSlot = s.localSlot;
    for (int i = 0; i < kMaxPlayers; i++) {
        st.players[i].present = s.peers[i].present;
        st.players[i].name = s.peers[i].name;
        st.players[i].pingMs = s.peers[i].pingMs;
    }
    st.playerCount = PlayerCount();
    st.frame = s.frame;
    st.waitingFor = s.waitingFor;
    st.lastMessage = s.lastMessage;
    st.hostAddress = s.hostAddress.IsValid() ? s.hostAddress.ToString() : std::string{};
    return st;
}

} // namespace Lan
