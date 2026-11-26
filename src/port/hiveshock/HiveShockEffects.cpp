#include "HiveShockEffects.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <random>

#include <libultraship.h>
#include "engine/World.h"
#include "engine/objects/BombKart.h"
#include "port/Game.h"

extern "C" {
#include "macros.h"
#include "defines.h"
#include "actor_types.h"
#include "main.h"
#include "menus.h"
#include "objects.h"
#include "effects.h"
#include "waypoints.h"
#include "update_objects.h"
#include "code_800029B0.h"
#include "code_80005FD0.h"
#include "code_80057C60.h"
#include "menu_items.h"
#include "replays.h"
#include "racing/actors.h"
#include "racing/actors_extended.h"
#include "racing/collision.h"
#include "racing/math_util.h"
extern s8 gPlayerCount;
extern f32 gKartBoundingBoxSizeTable[];
}

namespace HiveShock {
namespace {

constexpr int kTicksPerSecond = 60;
constexpr int kMaxSeconds = 120;
constexpr int kMaxTicksStacked = kMaxSeconds * kTicksPerSecond;
constexpr int kMaxHumans = 4;

// Distances are in track units. A kart covers roughly 300 per second at full speed, so this leaves about a second to react.
constexpr f32 kTrapDistance = 280.0f;
constexpr f32 kTrapRowSpacing = 35.0f;
constexpr f32 kLaneWidth = 28.0f;
constexpr f32 kShellChaseDistance = 130.0f;
constexpr f32 kShellAmbushDistance = 420.0f;
constexpr f32 kBombKartDistance = 380.0f;
constexpr f32 kShellSpeed = 7.0f;

constexpr f32 kSlowMaxSpeed = 2.5f; // about 30 km/h
constexpr f32 kMiniScale = 0.7f;    // what the thunderbolt uses
constexpr f32 kGiantScale = 1.35f;
constexpr f32 kLaunchSpeed = 3.2f; // what the moon jump cheat uses
constexpr int kLaunchTicks = 30;
constexpr int kBombKartLifeTicks = 30 * kTicksPerSecond;

// func_8002B5C0 throws triggers away while the kart is in any of these states. A viewer paid for the effect, so wait
// for the kart to recover instead.
constexpr u32 kBusyEffects = 0x40 | 0x80 | 0x400 | 0x800 | 0x4000 | 0x10000 | 0x20000 | 0x80000 | 0x800000 |
                             UNKNOWN_EFFECT_0x1000000 | HIT_BY_ITEM_EFFECT | HIT_EFFECT | BOOST_RAMP_ASPHALT_EFFECT |
                             BOOST_RAMP_WOOD_EFFECT;
constexpr u32 kShieldEffects = STAR_EFFECT | BOO_EFFECT;
constexpr u32 kPendingTriggers = HIT_TRIGGERS | RACING_SPINOUT_TRIGGERS | ANY_BOOST_TRIGGERS | STAR_TRIGGER | BOO_TRIGGER;

enum TimedId { kInvert, kDrunk, kStall, kFullThrottle, kHops, kTurbo, kSlow, kMini, kGiant, kLaunch, kTimedCount };

struct Timed {
    int ticksLeft = 0;
    int ticksTotal = 0;
    const Action* action = nullptr; // null: not shown to viewers
    std::string user;
};

struct BombKart {
    OBombKart* kart = nullptr;
    int age = 0;
};

Timed sTimed[kMaxHumans][kTimedCount];
int sHitCooldown[kMaxHumans];
int sUseItemTicks[kMaxHumans];
bool sRescued[kMaxHumans];
bool sFell[kMaxHumans];
bool sScaled[kMaxHumans];
std::vector<BombKart> sBombKarts;
std::vector<Actor*> sTraps;
std::vector<Actor*> sFreeActors; // spent traps, reused so that a long race does not pile up dead actors
u32 sTick = 0;

f32 Random(f32 min, f32 max) {
    // Not the game's generator: using it would change what the race itself rolls.
    static std::mt19937 engine{ std::random_device{}() };
    return std::uniform_real_distribution<f32>(min, max)(engine);
}

Player* GetPlayer(int index) {
    return &gPlayers[index];
}

bool IsBusy(const Player* player) {
    return (player->effects & kBusyEffects) != 0 || (player->kartProps & DRIVING_SPINOUT) != 0 ||
           (player->triggers & kPendingTriggers) != 0;
}

bool IsShielded(const Player* player) {
    return (player->effects & kShieldEffects) != 0;
}

bool TimedActive(int player, TimedId id) {
    return sTimed[player][id].ticksLeft > 0;
}

Timed& StartTimed(int player, TimedId id, int ticks, const Action* action, const char* user) {
    Timed& timed = sTimed[player][id];
    timed.ticksLeft = std::min(timed.ticksLeft + ticks, kMaxTicksStacked);
    timed.ticksTotal = timed.ticksLeft;
    timed.action = action;
    timed.user = user;
    return timed;
}

// ---------------------------------------------------------------------------------------------------------------
// Where to put things

struct Spot {
    Vec3f pos;
    f32 dirX;
    f32 dirZ;
};

bool GroundAt(f32 x, f32 nearY, f32 z, f32& outY) {
    const f32 y = spawn_actor_on_surface(x, nearY + 25.0f, z);
    if (y < nearY - 60.0f || y > nearY + 25.0f) {
        return false; // over the edge, or another level of the track
    }
    outY = y;
    return true;
}

// A point on the road `distance` units ahead of the kart (behind if negative), `side` units off the racing line.
// Follows the track's path in races; arenas have no path, so there it is a straight line.
bool FindSpot(const Player* player, f32 distance, f32 side, Spot& spot) {
    const int index = static_cast<int>(player - gPlayers);
    f32 x;
    f32 y;
    f32 z;

    const u16 pathIndex = gPathIndexByPlayerId[index];
    const TrackPathPoint* path = pathIndex < 4 ? gTrackPaths[pathIndex] : nullptr;
    const int count = path != nullptr ? gPathCountByPathIndex[pathIndex] : 0;

    if (gModeSelection != BATTLE && count > 3) {
        const int step = distance < 0.0f ? -1 : 1;
        const f32 wanted = std::fabs(distance);
        int at = gNearestPathPointByPlayerId[index] % count;
        f32 travelled = 0.0f;
        for (int i = 0; i < count / 2 && travelled < wanted; i++) {
            const int next = (at + step + count) % count;
            const f32 dx = static_cast<f32>(path[next].x - path[at].x);
            const f32 dz = static_cast<f32>(path[next].z - path[at].z);
            travelled += std::sqrt(dx * dx + dz * dz);
            at = next;
        }
        const TrackPathPoint& next = path[(at + 1) % count];
        x = path[at].x;
        y = path[at].y;
        z = path[at].z;
        spot.dirX = static_cast<f32>(next.x) - x;
        spot.dirZ = static_cast<f32>(next.z) - z;
    } else {
        Vec3f forward = { 0.0f, 0.0f, distance };
        func_802B64C4(forward, static_cast<s16>(player->rotation[1] + player->unk_0C0));
        x = player->pos[0] + forward[0];
        y = player->pos[1];
        z = player->pos[2] + forward[2];
        spot.dirX = forward[0];
        spot.dirZ = forward[2];
    }

    const f32 length = std::sqrt(spot.dirX * spot.dirX + spot.dirZ * spot.dirZ);
    if (length < 0.01f) {
        spot.dirX = 0.0f;
        spot.dirZ = 1.0f;
    } else {
        spot.dirX /= length;
        spot.dirZ /= length;
    }

    // Narrow road: try closer and closer to the racing line.
    for (const f32 scale : { 1.0f, 0.5f, 0.0f }) {
        const f32 px = x + spot.dirZ * side * scale;
        const f32 pz = z - spot.dirX * side * scale;
        f32 ground;
        if (GroundAt(px, y, pz, ground)) {
            spot.pos[0] = px;
            spot.pos[1] = ground;
            spot.pos[2] = pz;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// Spawning

bool IsTrapType(s16 type) {
    return type == ACTOR_BANANA || type == ACTOR_FAKE_ITEM_BOX || type == ACTOR_GREEN_SHELL || type == ACTOR_RED_SHELL ||
           type == ACTOR_BLUE_SPINY_SHELL;
}

// Vanilla actors are never freed during a race, only marked with type 0, so the pointers stay valid until the world is
// cleaned, which calls ForgetActors().
Actor* SpawnActor(Vec3f pos, s16 type) {
    Vec3s rot = { 0, 0, 0 };
    Vec3f velocity = { 0.0f, 0.0f, 0.0f };
    Actor* actor = nullptr;
    LiveTraps(); // refreshes the free list
    if (!sFreeActors.empty()) {
        actor = sFreeActors.back();
        sFreeActors.pop_back();
        actor_init(actor, pos, rot, velocity, type);
        gNumActors++;
    } else {
        const s16 index = add_actor_to_empty_slot(pos, rot, velocity, type);
        if (index < 0) {
            return nullptr;
        }
        actor = CM_GetActor(static_cast<size_t>(index));
    }
    // 0x1000 makes the actor ignore its owner. The owner is the victim, so that nobody gloats and everybody can be hit.
    actor->flags &= ~0x1000;
    sTraps.push_back(actor);
    return actor;
}

bool TrapRoom(int count) {
    return LiveTraps() + count <= MaxTraps();
}

Result RunBananas(const Action&, const Request& request) {
    if (!TrapRoom(request.count)) {
        return Result::Deferred;
    }
    const Player* player = GetPlayer(request.player);
    int placed = 0;
    for (int i = 0; i < request.count; i++) {
        Spot spot;
        const f32 side = request.count == 1 ? Random(-12.0f, 12.0f) : Random(-kLaneWidth, kLaneWidth);
        if (!FindSpot(player, kTrapDistance + kTrapRowSpacing * static_cast<f32>(i), side, spot)) {
            continue;
        }
        spot.pos[1] += 20.0f + static_cast<f32>(i % 3) * 6.0f;
        if (Actor* actor = SpawnActor(spot.pos, ACTOR_BANANA)) {
            auto* banana = reinterpret_cast<BananaActor*>(actor);
            banana->playerId = static_cast<s16>(request.player);
            banana->state = DROPPED_BANANA; // falls to the road
            placed++;
        }
    }
    return placed > 0 ? Result::Applied : Result::Deferred;
}

Result RunFakeBoxes(const Action&, const Request& request) {
    if (!TrapRoom(request.count)) {
        return Result::Deferred;
    }
    const Player* player = GetPlayer(request.player);
    int placed = 0;
    for (int i = 0; i < request.count; i++) {
        Spot spot;
        const f32 side = (static_cast<f32>(i) - static_cast<f32>(request.count - 1) * 0.5f) * 18.0f;
        if (!FindSpot(player, kTrapDistance, side, spot)) {
            continue;
        }
        if (Actor* actor = SpawnActor(spot.pos, ACTOR_FAKE_ITEM_BOX)) {
            auto* box = reinterpret_cast<FakeItemBox*>(actor);
            box->playerId = static_cast<f32>(request.player);
            box->state = FAKE_ITEM_BOX_ON_GROUND;
            box->targetY = spot.pos[1] + 8.66f;
            box->someTimer = 0;
            placed++;
        }
    }
    return placed > 0 ? Result::Applied : Result::Deferred;
}

Result RunGreenShells(const Action&, const Request& request) {
    if (!TrapRoom(request.count)) {
        return Result::Deferred;
    }
    const Player* player = GetPlayer(request.player);
    int placed = 0;
    for (int i = 0; i < request.count; i++) {
        Spot spot;
        if (!FindSpot(player, kShellAmbushDistance + 15.0f * static_cast<f32>(i), Random(-kLaneWidth, kLaneWidth), spot)) {
            continue;
        }
        spot.pos[1] += 5.0f;
        if (Actor* actor = SpawnActor(spot.pos, ACTOR_GREEN_SHELL)) {
            auto* shell = reinterpret_cast<ShellActor*>(actor);
            shell->playerId = static_cast<s16>(request.player);
            shell->state = MOVING_SHELL;
            shell->parentIndex = 0;
            // Against the traffic, slightly spread.
            shell->velocity[0] = -spot.dirX * kShellSpeed + Random(-0.8f, 0.8f);
            shell->velocity[2] = -spot.dirZ * kShellSpeed + Random(-0.8f, 0.8f);
            add_green_shell_in_unexpired_actor_list(static_cast<s32>(CM_FindActorIndex(actor)));
            placed++;
        }
    }
    return placed > 0 ? Result::Applied : Result::Deferred;
}

Result RunRedShell(const Action&, const Request& request) {
    const Player* player = GetPlayer(request.player);
    Spot spot;
    if (!TrapRoom(1) || (player->effects & BOO_EFFECT) || !FindSpot(player, -kShellChaseDistance, 0.0f, spot)) {
        return Result::Deferred;
    }
    spot.pos[1] += 5.0f;
    Actor* actor = SpawnActor(spot.pos, ACTOR_RED_SHELL);
    if (actor == nullptr) {
        return Result::Deferred;
    }
    auto* shell = reinterpret_cast<ShellActor*>(actor);
    shell->playerId = static_cast<s16>(request.player);
    shell->state = RED_SHELL_LOCK_ON;
    shell->shellId = 1000.0f;
    shell->targetPlayer = static_cast<s16>(request.player);
    add_red_shell_in_unexpired_actor_list(static_cast<s32>(CM_FindActorIndex(actor)));
    return Result::Applied;
}

// Follows the track to whoever is first, exactly like the item.
Result RunBlueShell(const Action&, const Request& request) {
    const Player* player = GetPlayer(request.player);
    const int count = gPathCountByPathIndex[gPathIndexByPlayerId[request.player] & 3];
    if (gModeSelection == BATTLE || count < 4) {
        return Result::Held;
    }
    Spot spot;
    if (!TrapRoom(1) || !FindSpot(player, -kShellChaseDistance, 0.0f, spot)) {
        return Result::Deferred;
    }
    spot.pos[1] += 5.0f;
    Actor* actor = SpawnActor(spot.pos, ACTOR_RED_SHELL);
    if (actor == nullptr) {
        return Result::Deferred;
    }
    auto* shell = reinterpret_cast<ShellActor*>(actor);
    shell->type = ACTOR_BLUE_SPINY_SHELL; // that is how the game itself makes one
    shell->playerId = static_cast<s16>(request.player);
    shell->state = BLUE_SHELL_LOCK_ON;
    shell->shellId = 1000.0f;
    shell->targetPlayer = gPlayerPositionLUT[0];
    shell->pathIndex = static_cast<u16>(gNearestPathPointByPlayerId[request.player] % count);
    add_blue_shell_in_unexpired_actor_list(static_cast<s32>(CM_FindActorIndex(actor)));
    return Result::Applied;
}

bool KartStillLoaded(const OBombKart* kart) {
    for (const auto& object : GetWorld()->Objects) {
        if (object.get() == kart) {
            return true;
        }
    }
    return false;
}

void RemoveKart(OBombKart* kart) {
    if (kart->_objectIndex >= 0) {
        delete_object(&kart->_objectIndex); // the destructor does not give the slot back
    }
    kart->Behaviour = OBombKart::States::DISABLED; // Tick() and Draw() do not touch the slot any more
    kart->Destroy();
}

Result RunBombKarts(const Action&, const Request& request) {
    // The explosion reads the racing path, which arenas do not have.
    if (gModeSelection == BATTLE || gTrackPaths[0] == nullptr || gPathCountByPathIndex[0] < 4) {
        return Result::Held;
    }
    if (!TrapRoom(request.count)) {
        return Result::Deferred;
    }
    const Player* player = GetPlayer(request.player);
    int placed = 0;
    for (int i = 0; i < request.count; i++) {
        Spot spot;
        const f32 side = (static_cast<f32>(i) - static_cast<f32>(request.count - 1) * 0.5f) * 25.0f;
        if (!FindSpot(player, kBombKartDistance + 30.0f * static_cast<f32>(i), side, spot)) {
            continue;
        }
        SpawnParams params;
        params.Name = "mk:bomb_kart";
        params.Behaviour = static_cast<int16_t>(OBombKart::States::CHASE);
        params.Location = FVector(spot.pos[0], spot.pos[1], spot.pos[2]);
        params.SpeedB = 5.0f;
        auto object = std::make_unique<OBombKart>(params);
        OBombKart* kart = object.get();
        GetWorld()->AddObject(std::move(object));
        // The constructor drops the kart on the highest surface; on multi-level tracks that is the wrong one.
        kart->Translate(FVector(spot.pos[0], spot.pos[1] + 3.5f, spot.pos[2]));
        kart->CenterY = spot.pos[1];
        kart->WaypointIndex = static_cast<u16>(gNearestPathPointByPlayerId[request.player] % gPathCountByPathIndex[0]);
        sBombKarts.push_back({ kart, 0 });
        placed++;
    }
    return placed > 0 ? Result::Applied : Result::Deferred;
}

// ---------------------------------------------------------------------------------------------------------------
// Items

Object* ItemWindow(int player) {
    return &gObjectList[gItemWindowObjectByPlayerId[player]];
}

// Plays the item roulette, rigged to stop on `action.param` (ITEM_NONE = a fair roll).
Result RunGiveItem(const Action& action, const Request& request) {
    const s32 window = gItemWindowObjectByPlayerId[request.player];
    if (!func_80072354(window, 4) || ItemWindow(request.player)->type != ITEM_NONE) {
        return Result::Deferred; // the player still has an item or the roulette is running
    }
    func_8007ABFC(request.player, action.param);
    return Result::Applied;
}

Result RunStealItem(const Action&, const Request& request) {
    Object* window = ItemWindow(request.player);
    if (window->type == ITEM_NONE) {
        return Result::Held; // wait until there is something to steal
    }
    if (window->type == ITEM_SUPER_MUSHROOM) {
        consume_item(request.player); // starts its countdown if it was not running...
        window->animationTimer = 0;   // ...and ends it
    }
    // The window reacts to what it was showing: a Boo would steal something back and a triple mushroom would leave two.
    window->unk_0A2 = ITEM_MUSHROOM;
    consume_item(request.player);
    return Result::Applied;
}

Result RunForceUse(const Action&, const Request& request) {
    if (ItemWindow(request.player)->type == ITEM_NONE || sUseItemTicks[request.player] > 0) {
        return Result::Held;
    }
    sUseItemTicks[request.player] = 24; // hold Z for 12 ticks, then release it (items are dropped on release)
    return Result::Applied;
}

// ---------------------------------------------------------------------------------------------------------------
// Karts

Result RunBoost(const Action& action, const Request& request) {
    Player* player = GetPlayer(request.player);
    const bool isBoost = action.param == SHROOM_TRIGGER;
    if (IsBusy(player) || (player->effects & BOO_EFFECT) || (player->effects & STAR_EFFECT) ||
        (isBoost && (player->effects & BOOST_EFFECT))) {
        return Result::Deferred;
    }
    player->triggers |= action.param;
    return Result::Applied;
}

Result RunHit(const Action& action, const Request& request) {
    Player* player = GetPlayer(request.player);
    if (sHitCooldown[request.player] > 0 || IsBusy(player) || IsShielded(player) ||
        (action.param == LIGHTNING_STRIKE_TRIGGER && (player->effects & LIGHTNING_EFFECT))) {
        return Result::Deferred;
    }
    if (action.param == THWOMP_SQUISH_TRIGGER) {
        // Not through the trigger: only a real thwomp clears the state the trigger leaves behind.
        trigger_squish(player, static_cast<s8>(request.player));
    } else {
        player->triggers |= action.param;
        if (action.param == LIGHTNING_STRIKE_TRIGGER) {
            func_8009E5BC(); // screen flash
        }
    }
    sHitCooldown[request.player] = CVarGetInteger("gHiveShock.HitGapMs", 2500) * kTicksPerSecond / 1000;
    return Result::Applied;
}

Result RunLakitu(const Action&, const Request& request) {
    Player* player = GetPlayer(request.player);
    if (sHitCooldown[request.player] > 0 || IsBusy(player) || IsShielded(player)) {
        return Result::Deferred;
    }
    player->lakituProps |= LAKITU_RETRIEVAL; // Lakitu comes by himself
    sHitCooldown[request.player] = CVarGetInteger("gHiveShock.HitGapMs", 2500) * kTicksPerSecond / 1000;
    return Result::Applied;
}

Result RunLaunch(const Action&, const Request& request) {
    Player* player = GetPlayer(request.player);
    if (sHitCooldown[request.player] > 0 || IsBusy(player)) {
        return Result::Deferred;
    }
    StartTimed(request.player, kLaunch, kLaunchTicks, nullptr, "");
    sHitCooldown[request.player] = CVarGetInteger("gHiveShock.HitGapMs", 2500) * kTicksPerSecond / 1000;
    return Result::Applied;
}

// Effects that would cancel each other out take turns.
int Opposite(int id) {
    switch (id) {
        case kTurbo:
            return kSlow;
        case kSlow:
            return kTurbo;
        case kMini:
            return kGiant;
        case kGiant:
            return kMini;
        case kStall:
            return kFullThrottle;
        case kFullThrottle:
            return kStall;
        default:
            return -1;
    }
}

Result RunTimed(const Action& action, const Request& request) {
    const int opposite = Opposite(action.param);
    if (opposite >= 0 && TimedActive(request.player, static_cast<TimedId>(opposite))) {
        return Result::Deferred;
    }
    StartTimed(request.player, static_cast<TimedId>(action.param), request.ticks, &action, request.user);
    return Result::Applied;
}

// ---------------------------------------------------------------------------------------------------------------
// Rivals (CPU karts)

Result RunRivals(const Action& action, const Request&) {
    int affected = 0;
    for (int i = 0; i < NUM_PLAYERS; i++) {
        Player* rival = GetPlayer(i);
        if (!(rival->type & PLAYER_EXISTS) || !(rival->type & PLAYER_CPU) || (rival->type & PLAYER_HUMAN) ||
            (rival->type & PLAYER_CINEMATIC_MODE) || IsBusy(rival) || IsShielded(rival) ||
            (rival->lakituProps & (HELD_BY_LAKITU | LAKITU_SCENE))) {
            continue;
        }
        if (action.param == LIGHTNING_STRIKE_TRIGGER && (rival->effects & LIGHTNING_EFFECT)) {
            continue;
        }
        rival->triggers |= action.param;
        affected++;
    }
    if (affected > 0 && action.param == LIGHTNING_STRIKE_TRIGGER) {
        func_8009E5BC();
    }
    return affected > 0 ? Result::Applied : Result::Held; // e.g. time trials have no rivals
}

Result RunClearTraps(const Action&, const Request&) {
    ClearTraps();
    return Result::Applied;
}

// Adding an effect is one function (or a shared one with another `param`) and one row here.
// The ids are the "action" of HiveShock's config/profiles/mario-kart-64/effects.json.
const std::vector<Action> kActions = {
    // id, category, label, good, lane, value, max value, seconds, run, param
    { "item_random", "Items", "Objeto sorpresa", true, Lane::Item, 1, 1, 0, RunGiveItem, ITEM_NONE },
    { "item_mushroom", "Items", "Champiñón", true, Lane::Item, 1, 1, 0, RunGiveItem, ITEM_MUSHROOM },
    { "item_triple_mushroom", "Items", "Triple champiñón", true, Lane::Item, 1, 1, 0, RunGiveItem, ITEM_TRIPLE_MUSHROOM },
    { "item_super_mushroom", "Items", "Champiñón dorado", true, Lane::Item, 1, 1, 0, RunGiveItem, ITEM_SUPER_MUSHROOM },
    { "item_star", "Items", "Estrella", true, Lane::Item, 1, 1, 0, RunGiveItem, ITEM_STAR },
    { "item_boo", "Items", "Boo", true, Lane::Item, 1, 1, 0, RunGiveItem, ITEM_BOO },
    { "item_thunderbolt", "Items", "Rayo", true, Lane::Item, 1, 1, 0, RunGiveItem, ITEM_THUNDERBOLT },
    { "item_banana", "Items", "Plátano", true, Lane::Item, 1, 1, 0, RunGiveItem, ITEM_BANANA },
    { "item_banana_bunch", "Items", "Racimo de plátanos", true, Lane::Item, 1, 1, 0, RunGiveItem, ITEM_BANANA_BUNCH },
    { "item_fake_box", "Items", "Caja falsa", true, Lane::Item, 1, 1, 0, RunGiveItem, ITEM_FAKE_ITEM_BOX },
    { "item_green_shell", "Items", "Caparazón verde", true, Lane::Item, 1, 1, 0, RunGiveItem, ITEM_GREEN_SHELL },
    { "item_triple_green_shell", "Items", "Triple caparazón verde", true, Lane::Item, 1, 1, 0, RunGiveItem, ITEM_TRIPLE_GREEN_SHELL },
    { "item_red_shell", "Items", "Caparazón rojo", true, Lane::Item, 1, 1, 0, RunGiveItem, ITEM_RED_SHELL },
    { "item_triple_red_shell", "Items", "Triple caparazón rojo", true, Lane::Item, 1, 1, 0, RunGiveItem, ITEM_TRIPLE_RED_SHELL },
    { "item_blue_shell", "Items", "Caparazón azul", true, Lane::Item, 1, 1, 0, RunGiveItem, ITEM_BLUE_SPINY_SHELL },
    { "item_steal", "Items", "Robo de objeto", false, Lane::Steal, 1, 1, 0, RunStealItem, 0 },
    { "item_use", "Items", "Usa el objeto ¡ya!", false, Lane::Steal, 1, 1, 0, RunForceUse, 0 },

    { "boost", "Power-ups", "Acelerón", true, Lane::Boost, 1, 1, 0, RunBoost, SHROOM_TRIGGER },
    { "star", "Power-ups", "Estrella instantánea", true, Lane::Boost, 1, 1, 0, RunBoost, STAR_TRIGGER },
    { "ghost", "Power-ups", "Modo fantasma", true, Lane::Boost, 1, 1, 0, RunBoost, BOO_TRIGGER },
    { "turbo", "Power-ups", "Turbo", true, Lane::Timed, 1, 1, 8, RunTimed, kTurbo },
    { "giant", "Power-ups", "Kart gigante", true, Lane::Timed, 1, 1, 20, RunTimed, kGiant },

    { "bananas", "Traps", "Lluvia de plátanos", false, Lane::Trap, 5, 15, 0, RunBananas, 0 },
    { "fake_boxes", "Traps", "Muro de cajas falsas", false, Lane::Trap, 3, 5, 0, RunFakeBoxes, 0 },
    { "green_shells", "Traps", "Ráfaga de caparazones", false, Lane::Trap, 4, 10, 0, RunGreenShells, 0 },
    { "red_shell", "Traps", "Caparazón rojo teledirigido", false, Lane::Trap, 1, 1, 0, RunRedShell, 0 },
    { "blue_shell", "Traps", "Caparazón azul al primero", false, Lane::Trap, 1, 1, 0, RunBlueShell, 0 },
    { "bomb_karts", "Traps", "Kart bomba", false, Lane::Trap, 1, 5, 0, RunBombKarts, 0 },
    { "clear_traps", "Traps", "Pista limpia", true, Lane::World, 1, 1, 0, RunClearTraps, 0 },

    { "slip", "Attacks", "Resbalón", false, Lane::Hit, 1, 1, 0, RunHit, HIT_BANANA_TRIGGER },
    { "spinout", "Attacks", "Trompo", false, Lane::Hit, 1, 1, 0, RunHit, SPINOUT_TRIGGER },
    { "tumble", "Attacks", "Voltereta", false, Lane::Hit, 1, 1, 0, RunHit, HIGH_TUMBLE_TRIGGER },
    { "explode", "Attacks", "Explosión", false, Lane::Hit, 1, 1, 0, RunHit, VERTICAL_TUMBLE_TRIGGER },
    { "lightning", "Attacks", "Rayo", false, Lane::Hit, 1, 1, 0, RunHit, LIGHTNING_STRIKE_TRIGGER },
    { "squish", "Attacks", "Aplastón", false, Lane::Hit, 1, 1, 0, RunHit, THWOMP_SQUISH_TRIGGER },
    { "launch", "Attacks", "Por los aires", false, Lane::Hit, 1, 1, 0, RunLaunch, 0 },
    { "lakitu", "Attacks", "Lakitu te recoge", false, Lane::Hit, 1, 1, 0, RunLakitu, 0 },

    { "invert", "Chaos", "Volante invertido", false, Lane::Timed, 1, 1, 12, RunTimed, kInvert },
    { "drunk", "Chaos", "Volante loco", false, Lane::Timed, 1, 1, 12, RunTimed, kDrunk },
    { "stall", "Chaos", "Motor apagado", false, Lane::Timed, 1, 1, 3, RunTimed, kStall },
    { "full_throttle", "Chaos", "Sin frenos", false, Lane::Timed, 1, 1, 12, RunTimed, kFullThrottle },
    { "hops", "Chaos", "Saltitos", false, Lane::Timed, 1, 1, 10, RunTimed, kHops },
    { "slow", "Chaos", "Tortuga", false, Lane::Timed, 1, 1, 8, RunTimed, kSlow },
    { "mini", "Chaos", "Kart miniatura", false, Lane::Timed, 1, 1, 20, RunTimed, kMini },

    { "rivals_boost", "Rivals", "Acelerón a los rivales", false, Lane::Rivals, 1, 1, 0, RunRivals, SHROOM_TRIGGER },
    { "rivals_star", "Rivals", "Estrella a los rivales", false, Lane::Rivals, 1, 1, 0, RunRivals, STAR_TRIGGER },
    { "rivals_spinout", "Rivals", "Trompo a los rivales", true, Lane::Rivals, 1, 1, 0, RunRivals, SPINOUT_TRIGGER },
    { "rivals_lightning", "Rivals", "Rayo a los rivales", true, Lane::Rivals, 1, 1, 0, RunRivals, LIGHTNING_STRIKE_TRIGGER },
};

void ApplyTimed(int index) {
    Player* player = GetPlayer(index);
    const bool controllable = CanAffect(index);

    if (TimedActive(index, kTurbo) && controllable && !IsBusy(player) && !IsShielded(player) &&
        !(player->effects & BOOST_EFFECT)) {
        player->triggers |= SHROOM_TRIGGER;
    }

    if (TimedActive(index, kSlow) && controllable) {
        const f32 vx = player->velocity[0];
        const f32 vz = player->velocity[2];
        const f32 speed = std::sqrt(vx * vx + vz * vz);
        if (speed > kSlowMaxSpeed) {
            player->velocity[0] = vx * kSlowMaxSpeed / speed;
            player->velocity[2] = vz * kSlowMaxSpeed / speed;
        }
    }

    if (TimedActive(index, kLaunch) && controllable && player->velocity[1] < kLaunchSpeed) {
        player->velocity[1] += 0.5f;
    }

    // While struck by lightning the game drives the size itself.
    const bool lightning = (player->effects & LIGHTNING_EFFECT) != 0;
    if (!lightning && (TimedActive(index, kMini) || TimedActive(index, kGiant))) {
        const bool mini = TimedActive(index, kMini);
        player->size += ((mini ? kMiniScale : kGiantScale) - player->size) * 0.1f;
        if (mini) {
            player->boundingBoxSize = gKartBoundingBoxSizeTable[player->characterId] * 0.9f;
        }
        sScaled[index] = true;
    } else if (sScaled[index]) {
        sScaled[index] = false;
        if (!lightning) {
            player->size = 1.0f;
            player->boundingBoxSize = gKartBoundingBoxSizeTable[player->characterId];
        }
    }
}

} // namespace

const std::vector<Action>& Actions() {
    return kActions;
}

const Action* FindAction(const std::string& id) {
    for (const auto& action : kActions) {
        if (id == action.id) {
            return &action;
        }
    }
    return nullptr;
}

bool RaceRunning() {
    return gGamestate == RACING && gRaceState == RACE_IN_PROGRESS && !gDemoMode && !gIsInQuitToMenuTransition &&
           D_8015F890 != 1 && GetWorld()->GetTrack() != nullptr;
}

int HumanCount() {
    return std::clamp(static_cast<int>(gPlayerCountSelection1), 1, kMaxHumans);
}

bool CanAffect(int index) {
    if (index < 0 || index >= HumanCount()) {
        return false;
    }
    const Player* player = GetPlayer(index);
    constexpr u16 blocked = PLAYER_CINEMATIC_MODE | PLAYER_START_SEQUENCE | PLAYER_INVISIBLE_OR_BOMB | PLAYER_UNKNOWN_0x40;
    return (player->type & PLAYER_EXISTS) && (player->type & PLAYER_HUMAN) && !(player->type & blocked) &&
           !(player->lakituProps & (LAKITU_RETRIEVAL | HELD_BY_LAKITU | LAKITU_SCENE)) &&
           !playerHUD[index].raceCompleteBool;
}

int LiveTraps() {
    std::erase_if(sTraps, [](Actor* actor) {
        if (actor->flags == 0 && actor->type == 0) {
            sFreeActors.push_back(actor);
            return true;
        }
        return !IsTrapType(actor->type);
    });
    return static_cast<int>(sTraps.size() + sBombKarts.size());
}

int MaxTraps() {
    return std::clamp(CVarGetInteger("gHiveShock.MaxTraps", 40), 5, 120);
}

void TickEffects() {
    sTick++;

    for (int i = 0; i < HumanCount(); i++) {
        ApplyTimed(i);
        // Being carried by Lakitu does not use up the time.
        if (CanAffect(i)) {
            for (auto& timed : sTimed[i]) {
                if (timed.ticksLeft > 0) {
                    timed.ticksLeft--;
                }
            }
            if (sHitCooldown[i] > 0 && !IsBusy(GetPlayer(i))) {
                sHitCooldown[i]--; // counts from the moment the kart recovers
            }
        }
        if (sUseItemTicks[i] > 0) {
            sUseItemTicks[i]--;
        }

        const bool rescued = (GetPlayer(i)->lakituProps & HELD_BY_LAKITU) != 0;
        sFell[i] = rescued && !sRescued[i];
        sRescued[i] = rescued;
    }

    for (auto it = sBombKarts.begin(); it != sBombKarts.end();) {
        if (!KartStillLoaded(it->kart)) {
            it = sBombKarts.erase(it);
            continue;
        }
        it->age++;
        if (it->kart->Behaviour == OBombKart::States::DISABLED || it->age > kBombKartLifeTicks) {
            RemoveKart(it->kart);
            it = sBombKarts.erase(it);
            continue;
        }
        // The body is drawn at CenterY, which chase mode never updates on slopes.
        if (it->kart->Behaviour == OBombKart::States::CHASE) {
            it->kart->CenterY = it->kart->Pos[1] - 3.5f;
        }
        ++it;
    }
}

void ResetEffects() {
    for (int i = 0; i < kMaxHumans; i++) {
        for (auto& timed : sTimed[i]) {
            timed = Timed{};
        }
        // Also on the way to the results: the kart is still on screen.
        if (sScaled[i] && gGamestate == RACING) {
            Player* player = GetPlayer(i);
            if (!(player->effects & LIGHTNING_EFFECT)) {
                player->size = 1.0f;
                player->boundingBoxSize = gKartBoundingBoxSizeTable[player->characterId];
            }
        }
        sScaled[i] = false;
        sHitCooldown[i] = 0;
        sUseItemTicks[i] = 0;
        sRescued[i] = false;
        sFell[i] = false;
    }
    for (const auto& entry : sBombKarts) {
        if (KartStillLoaded(entry.kart)) {
            RemoveKart(entry.kart);
        }
    }
    sBombKarts.clear();
}

void ForgetActors() {
    sBombKarts.clear();
    sTraps.clear();
    sFreeActors.clear();
}

void MarkRaceModified() {
    if (gModeSelection == TIME_TRIALS) {
        gPostTimeTrialReplayCannotSave = 1; // a ghost of this run would not be fair
    }
}

void ClearTraps() {
    LiveTraps(); // forgets the ones that are already gone
    for (Actor* actor : sTraps) {
        // A banana still falling to the road is skipped by the game's destroy: treat it as landed.
        if (actor->type == ACTOR_BANANA && reinterpret_cast<BananaActor*>(actor)->state == DROPPED_BANANA) {
            reinterpret_cast<BananaActor*>(actor)->state = BANANA_ON_GROUND;
        }
        destroy_destructable_actor(actor); // they stay in the list until their animation ends
    }
    for (const auto& entry : sBombKarts) {
        if (KartStillLoaded(entry.kart)) {
            RemoveKart(entry.kart);
        }
    }
    sBombKarts.clear();
}

std::vector<TimedView> GetTimed() {
    std::vector<TimedView> out;
    for (int i = 0; i < kMaxHumans; i++) {
        for (const auto& timed : sTimed[i]) {
            if (timed.ticksLeft > 0 && timed.action != nullptr) {
                out.push_back({ timed.action->label, timed.user, timed.action->good, i, timed.ticksLeft, timed.ticksTotal });
            }
        }
    }
    return out;
}

bool ConsumeFall(int player) {
    const bool fell = sFell[player];
    sFell[player] = false;
    return fell;
}

void FilterPad(int index, uint16_t& buttons, int8_t& stickX, int8_t& stickY) {
    (void) stickY;
    if (index < 0 || index >= kMaxHumans || gIsGamePaused || !RaceRunning() || !CanAffect(index)) {
        return;
    }

    int steer = stickX;
    if (TimedActive(index, kInvert)) {
        steer = -steer;
    }
    if (TimedActive(index, kDrunk)) {
        const f32 t = static_cast<f32>(sTick) / kTicksPerSecond;
        steer += static_cast<int>(38.0f * std::sin(t * 2.6f) + 20.0f * std::sin(t * 5.9f));
    }
    if (steer != stickX) {
        stickX = static_cast<int8_t>(std::clamp(steer, -85, 85));
    }
    if (TimedActive(index, kStall)) {
        buttons &= ~A_BUTTON;
    }
    if (TimedActive(index, kFullThrottle)) {
        buttons |= A_BUTTON;
        buttons &= ~B_BUTTON;
    }
    if (TimedActive(index, kHops)) {
        // A short tap every 0.7 s; the rest of the time R still belongs to the player.
        const u32 phase = sTick % 42;
        if (phase < 6) {
            buttons |= R_TRIG;
        } else if (phase >= 36) {
            buttons &= ~R_TRIG; // so the next tap is a new press
        }
    }
    if (sUseItemTicks[index] > 0) {
        if (sUseItemTicks[index] > 20) {
            buttons &= ~Z_TRIG;
        } else if (sUseItemTicks[index] > 8) {
            buttons |= Z_TRIG;
        } else {
            buttons &= ~Z_TRIG;
        }
    }
}

} // namespace HiveShock
