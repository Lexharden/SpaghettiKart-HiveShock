#include "LanReplay.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <libultraship.h>
#include <spdlog/spdlog.h>

extern "C" {
#include "macros.h"
#include "defines.h"
#include "main.h"
#include "buffers/random.h"
}
#include "port/Game.h"

namespace {

constexpr int kPads = 4;

struct Config {
    bool loaded = false;
    bool bot = false;
    uint64_t botState = 0;
    FILE* record = nullptr;
    FILE* play = nullptr;
    FILE* hashLog = nullptr;
    long long maxFrames = 0;
    // Pre-roll: play with another seed for a while, then reset, as a LAN session start would.
    long long prerollFrames = 0;
    uint64_t mainBotState = 0;
};

Config sConfig;
long long sFrame = 0;

const char* Env(const char* name) {
    const char* value = std::getenv(name);
    return (value != nullptr && value[0] != '\0') ? value : nullptr;
}

FILE* OpenOrWarn(const char* path, const char* mode) {
    FILE* f = std::fopen(path, mode);
    if (f == nullptr) {
        SPDLOG_ERROR("[LanReplay] Cannot open {}", path);
    }
    return f;
}

void LoadConfig() {
    sConfig.loaded = true;
    if (const char* seed = Env("SPAGHETTI_NET_BOT")) {
        sConfig.bot = true;
        sConfig.botState = std::strtoull(seed, nullptr, 10) * 0x9E3779B97F4A7C15ull + 1;
    }
    if (const char* path = Env("SPAGHETTI_NET_RECORD")) {
        sConfig.record = OpenOrWarn(path, "w");
    }
    if (const char* path = Env("SPAGHETTI_NET_PLAY")) {
        sConfig.play = OpenOrWarn(path, "r");
    }
    if (const char* path = Env("SPAGHETTI_NET_HASHLOG")) {
        sConfig.hashLog = OpenOrWarn(path, "w");
    }
    if (const char* preroll = Env("SPAGHETTI_NET_PREROLL")) {
        // "<frames>,<seed>": diverge first with another seed, then reset and continue with SPAGHETTI_NET_BOT.
        char* rest = nullptr;
        sConfig.prerollFrames = std::strtoll(preroll, &rest, 10);
        const uint64_t prerollSeed = (rest != nullptr && *rest == ',') ? std::strtoull(rest + 1, nullptr, 10) : 1;
        sConfig.mainBotState = sConfig.botState;
        sConfig.botState = prerollSeed * 0x9E3779B97F4A7C15ull + 1;
    }
    if (const char* frames = Env("SPAGHETTI_NET_FRAMES")) {
        sConfig.maxFrames = std::atoll(frames);
    }
    if (sConfig.bot || sConfig.record || sConfig.play || sConfig.hashLog) {
        SPDLOG_INFO("[LanReplay] bot={} record={} play={} hashlog={} frames={}", sConfig.bot,
                    sConfig.record != nullptr, sConfig.play != nullptr, sConfig.hashLog != nullptr, sConfig.maxFrames);
    }
}

uint32_t BotNext() {
    // xorshift64*: identical on every platform, unlike the <random> distributions.
    uint64_t x = sConfig.botState;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    sConfig.botState = x;
    return static_cast<uint32_t>((x * 0x2545F4914F6CDD1Dull) >> 32);
}

// Menus: tap A (and Start now and then, for the title screen). Racing: throttle held, wandering steering, items and hops.
int sSteer = 0;
int sSteerTarget = 0;

void BotFillPad(OSContPad& pad) {
    const long long f = sFrame;

    pad = {};
    if (gGamestate != RACING) {
        if (f % 30 < 2) {
            pad.button |= A_BUTTON;
        } else if (f % 240 >= 120 && f % 240 < 122) {
            pad.button |= START_BUTTON;
        }
        return;
    }

    pad.button |= A_BUTTON;
    if (f % 45 == 0) {
        sSteerTarget = static_cast<int>(BotNext() % 161) - 80;
    }
    sSteer += (sSteerTarget > sSteer) ? 4 : (sSteerTarget < sSteer ? -4 : 0);
    pad.stick_x = static_cast<int8_t>(sSteer);
    const uint32_t roll = BotNext() % 1000;
    if (roll < 15) {
        pad.button |= Z_TRIG;
    } else if (roll < 30) {
        pad.button |= R_TRIG;
    }
}

void ReadRecordedFrame(OSContPad* pads) {
    for (int i = 0; i < kPads; i++) {
        unsigned int button = 0;
        int sx = 0, sy = 0, rx = 0, ry = 0;
        if (std::fscanf(sConfig.play, "%x %d %d %d %d", &button, &sx, &sy, &rx, &ry) != 5) {
            SPDLOG_INFO("[LanReplay] Recording finished at frame {}", sFrame);
            std::fclose(sConfig.play);
            sConfig.play = nullptr;
            return;
        }
        pads[i] = {};
        pads[i].button = static_cast<uint16_t>(button);
        pads[i].stick_x = static_cast<int8_t>(sx);
        pads[i].stick_y = static_cast<int8_t>(sy);
        pads[i].right_stick_x = static_cast<int8_t>(rx);
        pads[i].right_stick_y = static_cast<int8_t>(ry);
    }
}

uint64_t Fnv(uint64_t h, const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; i++) {
        h = (h ^ p[i]) * 0x100000001B3ull;
    }
    return h;
}

} // namespace

extern "C" void LanReplay_FilterPads(void* rawPads) {
    if (!sConfig.loaded) {
        LoadConfig();
    }
    auto* pads = static_cast<OSContPad*>(rawPads);

    if (sConfig.play != nullptr) {
        ReadRecordedFrame(pads);
    } else if (sConfig.bot) {
        BotFillPad(pads[0]);
        for (int i = 1; i < kPads; i++) {
            pads[i] = {};
        }
    }

    if (sConfig.record != nullptr) {
        for (int i = 0; i < kPads; i++) {
            std::fprintf(sConfig.record, "%x %d %d %d %d%c", pads[i].button, pads[i].stick_x, pads[i].stick_y,
                         pads[i].right_stick_x, pads[i].right_stick_y, i == kPads - 1 ? '\n' : ' ');
        }
    }
}

extern "C" void LanReplay_ResetToKnownState(unsigned short seed) {
    // The port's reset takes the menus back to the start; what it leaves behind is reset here.
    CM_RequestReset();
    gRandomSeed16 = seed;
    // Leftovers of the last race: the next race overwrites them, but until then they would differ between machines.
    std::memset(gPlayers, 0, sizeof(Player) * NUM_PLAYERS);
    gRaceState = RACE_INIT;
    gCourseTimer = 0.0f;
}

extern "C" unsigned long long LanReplay_StateHash(void) {
    uint64_t h = 0xCBF29CE484222325ull;
    // The first 0xE0 bytes of each player hold position, speed, rotation, lap, rank and effects, and no pointers.
    for (int i = 0; i < NUM_PLAYERS; i++) {
        h = Fnv(h, &gPlayers[i], 0xE0);
    }
    h = Fnv(h, &gRandomSeed16, sizeof(gRandomSeed16));
    h = Fnv(h, &gGamestate, sizeof(gGamestate));
    h = Fnv(h, &gRaceState, sizeof(gRaceState));
    h = Fnv(h, &gCourseTimer, sizeof(gCourseTimer));
    return h;
}

extern "C" void LanReplay_EndFrame(void) {
    if (!sConfig.loaded) {
        LoadConfig();
    }
    if (sConfig.prerollFrames > 0) {
        if (++sFrame >= sConfig.prerollFrames) {
            SPDLOG_INFO("[LanReplay] Pre-roll done after {} frames, resetting", sFrame);
            sConfig.prerollFrames = 0;
            sConfig.botState = sConfig.mainBotState;
            sFrame = 0;
            sSteer = sSteerTarget = 0;
            LanReplay_ResetToKnownState(0x5EED);
        }
        return;
    }
    if (sConfig.hashLog != nullptr) {
        std::fprintf(sConfig.hashLog, "%lld %d %d %04x %016llx %.3f %.3f %.3f\n", sFrame, gGamestate, gRaceState,
                     gRandomSeed16, LanReplay_StateHash(), gPlayers[0].pos[0], gPlayers[0].pos[1], gPlayers[0].pos[2]);
    }
    sFrame++;

    if (sConfig.maxFrames > 0 && sFrame >= sConfig.maxFrames) {
        SPDLOG_INFO("[LanReplay] {} frames done, quitting", sFrame);
        for (FILE* f : { sConfig.record, sConfig.hashLog }) {
            if (f != nullptr) {
                std::fflush(f);
            }
        }
        // Test runs only: skip teardown so a slow shutdown cannot blur the result.
        std::_Exit(0);
    }
}
