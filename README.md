# Frame Guard

A [Geode](https://geode-sdk.org) mod for Geometry Dash that decouples update rate from frame rate.

**Platform:** Windows, GD 2.2081, Geode 5.10.1

## Features

| Setting | Default | What it does |
|---|---|---|
| SubFPS mode | off | In levels: update exactly SubFPS times/s with constant dt, draw only at Display Hz. VSync off. Physics untouched (240 steps/s). |
| SubFPS | 240 | Game updates per second (use a multiple of 240). Max 100000; real loop speed tops out around 20000-40000/s. |
| Show SubFPS counter | off | Top-right overlay with the executed update rate. |
| Display Hz | 0 (auto) | Draw rate in SubFPS mode. |
| Draw budget (%) | 75 | Max share of time drawing may take; 0 = off. |
| Menu FPS cap | 1000 | Cap outside levels while SubFPS mode is on. |
| Lean update | on | Updates that present no frame skip the draw pipeline and the progress bar refresh. |
| Lean particles | off | Experimental. Particles are simulated only on presented frames. |
| Batch fast path | on | Skips clean sprites in `CCSpriteBatchNode::draw`. Verifies field offsets every 64 draws and disables itself for the session on mismatch. |
| Visibility throttle (ms) | 0 (off) | Experimental. Runs `updateVisibility` at most once per interval. |
| Debug log | off | FPS, frame time, update cost breakdown, fast-batch skip ratio, throttle counters, physics checksum. |

## Known limitations

- The batch fast path reads `CCSprite` / `CCNode` fields by raw offset, so it is tied to GD 2.2081 on Windows. Other versions need new offsets.
- The visibility throttle has not been proven to leave physics unchanged in every level. Compare the `[CHK]` hash at `levelComplete` with throttle 0 and N before trusting it.
- SubFPS values far above 240 raise CPU load without making physics finer.

## Building

Requires the [Geode SDK](https://docs.geode-sdk.org/getting-started/) and the `GEODE_SDK` environment variable.

```
cmake -B build -A x64
cmake --build build --config RelWithDebInfo
```

## License

MIT
