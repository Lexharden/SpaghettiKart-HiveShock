# LAN play

Up to four players, each on their own PC, in the same race. Open **Network > LAN** in the menu bar.

## Playing

1. Everyone downloads **the same package for the same OS** (Windows with Windows, Linux with Linux, Mac ARM with Mac
   ARM). Mixed systems are refused: the game is compiled with fast floating point, so different compilers do not
   produce the same results and the races would drift apart.
2. One player clicks **Host a game**. The others click **Search the LAN** and join the game found, or type the host's
   IP (e.g. `192.168.1.20`) and click **Join**.
3. When everyone is in, the host clicks **Start**. All PCs go back to the title screen together.
4. Player 1 (the host) picks the number of players and the mode in the main menu; each player picks their own
   character with their own controller. Controller 1 of each PC drives that PC's player.

For now every PC shows the same split screen. A full-screen view of your own player is the next step.

Leaving (or a PC that stops responding for 10 seconds) ends the game for everyone and returns to the title screen.

## Network

- UDP **43100**: the game. UDP **43101**: searching the LAN (broadcast). Allow both in the host's firewall; Windows
  asks the first time.
- Joining by IP works even where broadcast is blocked.

## What is shared

- The host's gameplay settings (enhancements, cheats, game speed, traffic counts) and its save data are used on every
  PC while the game lasts. Nothing is written to the other PCs' saves; each gets its own settings back at the end.
- Freecam, the editor and game speed changes are blocked during a game.
- HiveShock effects pause during a LAN game.
- Every 30 frames the PCs compare a hash of the race. If they differ (different mods, a different package), the game
  stops with an "out of sync" message instead of carrying on with different races.

## How it works

Input lockstep, like emulator netplay: every PC runs the whole game and only the controllers travel. The host gathers
each player's pad for a frame and sends the complete frame to everyone; the game uses a pad two frames after it was
read, and every datagram repeats the last eight frames so a lost one costs nothing. A game starts with the same reset
on every PC (`LanReplay_ResetToKnownState`). Code: `src/port/net/`.

## Testing without players

Environment variables, read at start-up:

| Variable | Effect |
|---|---|
| `SPAGHETTI_NET_BOT=<seed>` | Synthetic input on controller 1 (menus and racing) |
| `SPAGHETTI_NET_BOT_PLAYERS=<n>` | The bot picks n players in the main menu |
| `SPAGHETTI_NET_RECORD=<file>` / `SPAGHETTI_NET_PLAY=<file>` | Record / replay the four pads |
| `SPAGHETTI_NET_HASHLOG=<file>` | State hash of every frame |
| `SPAGHETTI_NET_FRAMES=<n>` | Quit after n frames |
| `SPAGHETTI_NET_PREROLL=<n>,<seed>` | Play n frames with another seed, then reset as a LAN start does |
| `SPAGHETTI_LAN_HOST=<name>` / `SPAGHETTI_LAN_JOIN=<address>` | Host or join on start-up |
| `SPAGHETTI_LAN_AUTOSTART=<n>` | The host starts once n players are in |
| `SPAGHETTI_LAN_LOG=<file>` | State hash of every LAN frame |

Two instances on one PC:

```sh
SPAGHETTI_NET_BOT=7 SPAGHETTI_NET_BOT_PLAYERS=2 SPAGHETTI_LAN_HOST=Host SPAGHETTI_LAN_AUTOSTART=2 \
  SPAGHETTI_LAN_LOG=host.txt SPAGHETTI_NET_FRAMES=2500 ./Spaghettify &
SPAGHETTI_NET_BOT=21 SPAGHETTI_LAN_JOIN=127.0.0.1 SPAGHETTI_LAN_LOG=client.txt SPAGHETTI_NET_FRAMES=2500 ./Spaghettify
```

`host.txt` and `client.txt` must match line by line.
