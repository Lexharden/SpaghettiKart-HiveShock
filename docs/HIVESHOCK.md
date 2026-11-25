# HiveShock integration

SpaghettiKart can receive live stream effects (gifts, chat, follows, bits, channel points) from the
[HiveShock](https://hiveshock.yafel.dev) app. Enable it in **Network > HiveShock**; it then starts with the game.

## Protocol

The app opens a short TCP connection to port `43000` and sends one JSON object per line:

```json
{"action":"bananas","user":"viewer","value":5}
```

| Field | Meaning |
|---|---|
| `action` | Required. One of the ids below. |
| `user` | Viewer name, shown on the overlay. |
| `value` | How many (traps). Clamped to the action's maximum. |
| `duration` | Seconds, for timed effects (1-120). Receiving the same effect again adds time. |
| `player` | 1-4, overrides the *Affected player* setting. |
| `token` | Required only if `gHiveShock.Token` is set. |

`clear_queue` forgets everything that is waiting and works anywhere.

The game reports to `127.0.0.1:43002` (best effort): `player_death` when Lakitu has to rescue a player, and
`spawn_queue` with the waiting effects and live traps.

## How effects are scheduled

- Effects only run while the race is on and the player is in control. In menus, the countdown, the pause screen, demos and
  results they wait.
- Nothing is dropped. The queue holds 1024; when full, the newest command is rejected. It is in memory only: closing the
  game loses what was waiting.
- Each action belongs to a *lane* (items, attacks, traps...). Within a lane the order is kept; a blocked lane (item slot
  busy, kart already spinning, star active, too many traps) does not hold up the others.
- The game throws away hits that arrive while a kart is already tumbling. HiveShock waits for the kart to recover, plus a
  grace time, so every gift is seen.

## Actions

The single source is `kActions` in `src/port/hiveshock/HiveShockEffects.cpp`; the menu is generated from it.

- **Items** (roulette rigged to that item): `item_random`, `item_mushroom`, `item_triple_mushroom`, `item_super_mushroom`,
  `item_star`, `item_boo`, `item_thunderbolt`, `item_banana`, `item_banana_bunch`, `item_fake_box`, `item_green_shell`,
  `item_triple_green_shell`, `item_red_shell`, `item_triple_red_shell`, `item_blue_shell`; `item_steal`, `item_use`
- **Power-ups**: `boost`, `star`, `ghost`, `turbo` (timed), `giant` (timed)
- **Traps**: `bananas`, `fake_boxes`, `green_shells`, `red_shell`, `blue_shell`, `bomb_karts`, `clear_traps`
- **Attacks**: `slip`, `spinout`, `tumble`, `explode`, `lightning`, `squish`, `launch`, `lakitu`
- **Chaos** (timed): `invert`, `drunk`, `stall`, `full_throttle`, `hops`, `slow`, `mini`
- **Rivals**: `rivals_boost`, `rivals_star`, `rivals_spinout`, `rivals_lightning`

`blue_shell` and `bomb_karts` need a racing path, so in battle mode they wait for the next race. A time trial that
receives an effect cannot save its ghost.

## CVars (`gHiveShock.*`)

`Enabled` (0), `Port` (43000), `EventPort` (43002), `AllowLAN` (0), `Token` (""), `Target` (0-3 = P1-P4, 4 = random),
`MinGapMs` (250), `HitGapMs` (2500), `MaxTraps` (40), `Overlay` (1), `OverlaySide` (0 left, 1 right), `OverlayScale` (1.0),
`Cat.<Category>` (1).

## Code

| File | Role |
|---|---|
| `src/port/hiveshock/HiveShockServer.*` | Sockets. One thread, no game headers. |
| `src/port/hiveshock/HiveShock.*` | Parsing, queue, scheduling, events, C hooks. |
| `src/port/hiveshock/HiveShockEffects.*` | Action table and effects. Game thread only. |
| `src/port/hiveshock/HiveShockOverlay.*` | On-stream feed. |
| `src/port/ui/PortMenuNetwork.cpp` | Network menu. |

Hooks in the game: `process_game_tick` and `update_controller` (`main.c`), `CleanWorld`/`CleanActors` (`World.cpp`),
`CustomEngineInit`/`CustomEngineDestroy` (`Game.cpp`).
