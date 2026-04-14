# Engine performance

Recoil is an RTS engine built for large-scale games — designed to handle thousands of units at once.

## Scale target

- **Target:** ~10k concurrent units, *including buildings*.
- Mobile units tend to be ~40% of that late-game total.
- Largest seen in a real game: ~17.7k units. That's a data point, not a design target.

## Sim, draw, and update frames

The main loop is `Update → Draw`, repeating. Each iteration produces one draw frame and drains any queued sim frame packets first (0..N sim frames per iteration). `CGame::Update` (synced) dispatches `SimFrame()` calls as `NETMSG_NEWFRAME` packets arrive; `CGame::Draw` then does an **unsynced update phase** (`CGame::UpdateUnsynced`: timings, interpolation, camera, GUI, sound, world-drawer prep) followed by **rendering** (`DrawGenesis` → `DrawScreenPost`). The sim burst is capped at ~500 ms (`minDrawFPS`) so draw always gets to run. It's all one thread — sim and rendering are **not concurrent**; parallelism only happens *inside* a phase.

Conversely, if no sim frames are in the queue the main loop runs `Draw`/`UpdateUnsynced` as fast as possible — many draw iterations can pass between successive sim frames, with visuals interpolating smoothly in between via `globalRendering->timeOffset`.

```
main-loop iteration  (repeats as fast as possible)
├── CGame::Update            (synced)
│   └── SimFrame × 0..N      ← processes queued sim frames capped at
|                              ~500ms per iteration
└── CGame::Draw              (unsynced)
    ├── UpdateUnsynced       ← unsynced update phase
    └── DrawGenesis → DrawScreenPost  ← render world + screen
```

| Phase | Rate | Synced? | Responsibility |
|---|---|---|---|
| **Sim frame** — `CGame::SimFrame` | fixed 30 Hz (`GAME_SPEED`) | yes | advance deterministic state: units, pathing, projectiles, line-of-sight, scripts, Lua `GameFrame` |
| **Draw frame** — `CGame::Draw` | variable | no | update phase (see below) + render world/screen |
| **Update phase** — `CGame::UpdateUnsynced` *(inside draw frame)* | per draw frame | no | timings, interpolation, camera, GUI, sound, world-drawer prep |

**Benchmark names.** `fightertest` reports these phases as three peer buckets **Sim / Update / Render** — Sim ≈ `CGame::SimFrame`, Update ≈ `CGame::UpdateUnsynced`, Render ≈ `DrawGenesis` → `DrawScreenPost`.

### Scheduling and CPU budget

- Sim has a target rate set by the server; draw is as fast as the hardware allows. The sim target is `30 Hz × speedFactor`; at a speed factor of 1x, in-game time tracks real-world time 1:1, and at 2x speed the server fires twice as many sim frames per real-world second so the world evolves twice as fast. 
- **Zero, one, or many** sim frames per draw frame — if the client falls behind, pending sim frames burst in the next iteration to catch up.
- Visuals interpolate between sim frames, so draw rate can exceed sim rate without stutter.
- **Sim has priority, but with headroom.** Sim targets only **~60% of wall-clock time** — the rest is reserved so rendering can run afterwards. If the **median of all players' recent sim CPU%** exceeds 60%, the server drops the delivered speed below the requested speed; re-evaluated every 2 s.
  - At 1× speed, that works out to ~20 ms of sim per ~33 ms tick. Higher speedFactor means more, shorter ticks per wall-second — the 60% wall-clock rule is unchanged.
  - One-off slow frames don't throttle; sustained overrun does.
  - Draw FPS may also drop as a side effect: the main thread is busy on sim, so fewer draw frames land per wall-clock second.

## Multi-threading

The engine runs one **main thread** plus a pool of **worker threads**, all pinned to distinct cores. The main thread drives the sim/draw loop; workers pick up parallel work dispatched from the main thread (via `for_mt` and friends in `rts/System/Threading/ThreadPool.h`). The main thread also participates in draining the task queue while it waits.

Worker count is chosen by inspecting CPU topology: cache groups are sorted by L3 size (largest first), and the pool fills from the best cache group, pulling in further groups until it has **at least** a target of ~6 threads on x86 (~4 on ARM). The target is a floor, not a ceiling — once a cache group is pulled, all its cores are pulled as well. This means the pool can be larger than the target. On a machine with fewer usable perf cores than the target, it shrinks to what's available. Pinning is done via `sched_setaffinity` on Linux and `SetThreadAffinityMask` on Windows. Efficiency cores are excluded on hybrid CPUs.

Most parallel work in the engine is **homogeneous** — the same operation applied over many items (unit updates, projectile steps, etc.) via `for_mt`. Keeping parallel work homogeneous is a deliberate discipline: it makes determinism easier to reason about and keeps sim output independent of how work happens to land across threads.

**QTPFS is the one heterogeneous exception.** The quad-tree pathfinder maintains its own per-worker search state (`SearchThreadData`, `SparseData`) independent of engine sim state, which lets it safely run path searches on the worker pool *in the background* via `for_mt_background`. Background tasks yield to higher-priority work by rescheduling themselves when other jobs arrive, so QTPFS soaks up idle worker capacity without preempting foreground parallelism.