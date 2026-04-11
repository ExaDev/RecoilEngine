# Engine performance

Recoil is an RTS engine built for large-scale games — designed to handle thousands of units at once.

## Scale target

- **Target:** ~10k concurrent units, *including buildings*.
- Mobile units tend to be ~40% of that late-game total.
- Largest seen in a real game: ~17.7k units. That's a data point, not a design target.

## Sim, draw, and update frames

A main-loop iteration contains any pending **sim frames** followed by exactly one **draw frame**. `CGame::Update` runs first (synced) and dispatches `SimFrame()`s as network packets arrive; then `CGame::Draw` produces the iteration's draw frame — an **update phase** (`CGame::UpdateUnsynced` — timings, interpolation, camera, GUI, sound, world-drawer prep), then rendering the world and screen. It's all one thread — sim and rendering are **not concurrent**; parallelism only happens *inside* a phase.

```
main-loop iteration
├── CGame::Update       (synced plumbing)
│   └── SimFrame × 0..N  ← 0..N "sim frames" (30 Hz, deterministic)
└── CGame::Draw         ← one "draw frame" (unsynced)
    ├── UpdateUnsynced  (update phase)
    └── render world + screen
```

| Frame | Rate | Synced? | Responsibility |
|---|---|---|---|
| **Sim frame** — `CGame::SimFrame` | fixed 30 Hz (`GAME_SPEED`) | yes | advance deterministic state: units, pathing, projectiles, line-of-sight, scripts, Lua `GameFrame` |
| **Draw frame** — `CGame::Draw` | variable | no | update phase + render world/screen |

### Scheduling and CPU budget

- Sim is fixed-rate; draw is as fast as sim + rendering allow. No hard CPU split in normal builds.
- **Zero, one, or many** sim frames per draw frame — if the client falls behind, pending sim frames burst in the next iteration to catch up.
- Visuals interpolate between sim frames, so draw rate can exceed sim rate without stutter.
- **Sim has priority.** If sim work can't fit in ~33.3 ms per tick, `simspeed` drops below 1.0 and the game runs in slow motion — sim frames are never skipped, just stretched across more wall time.
  - Draw FPS may also drop as a side effect: the main thread is busy on sim, so fewer draw frames land per wall-clock second.
