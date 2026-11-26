#pragma once

// Determinism tooling for LAN play (docs/LAN.md). Everything here is off unless one of these environment variables
// is set when the game starts:
//   SPAGHETTI_NET_BOT=<seed>       synthetic input on controller 1 (menus and racing), same seed = same input
//   SPAGHETTI_NET_BOT_PLAYERS=<n>  the bot picks n players in the main menu (default 1)
//   SPAGHETTI_NET_RECORD=<file>    records the four pads, one frame per line
//   SPAGHETTI_NET_PLAY=<file>      replays a recording instead of the real pads
//   SPAGHETTI_NET_HASHLOG=<file>   writes the state hash of every frame
//   SPAGHETTI_NET_FRAMES=<n>       quits after n frames (for unattended runs)
//   SPAGHETTI_NET_PREROLL=<n>,<s>  plays n frames with bot seed s first, then resets as a LAN session start does
// Two runs with the same input must produce identical hash logs; that is what lockstep relies on.

#ifdef __cplusplus
extern "C" {
#endif

// Right after the pads are read, before the game derives presses from them. `pads` is gControllerPads (OSContPad[4]).
void LanReplay_FilterPads(void* pads);

// After the frame's logic and rendering have run.
void LanReplay_EndFrame(void);

// Puts every machine in the same state: the port's reset plus whatever it leaves untouched. Applied next frame.
void LanReplay_ResetToKnownState(unsigned short seed);

// Hash of the simulation state that must match on every machine. Shared with the LAN session.
unsigned long long LanReplay_StateHash(void);

#ifdef __cplusplus
}
#endif
