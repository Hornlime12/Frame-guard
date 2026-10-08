# Frame Guard

A [Geode](https://geode-sdk.org) mod for Geometry Dash that defends your frame rate in heavy levels.

**Platform:** Windows, GD 2.2081, Geode 5.10.1

## Features

| Setting | Default | What it does |
|---|---|---|
| Unlimited FPS | off | Removes the FPS cap and turns VSync off. Off restores GD's own interval (restart to restore VSync). |
| Decoupled display (experimental) | off | Update runs every loop, scene is drawn/presented only at Display Hz. VSync is turned off. Physics is not hooked. |
| Display Hz | 0 (auto) | Present rate for Decoupled display. |
| Logic FPS cap | 0 (unlimited) | Update rate for Decoupled display. Unlimited uses a full CPU core. |
| Batch fast path | on | Skips clean sprites in `CCSpriteBatchNode::draw`. Verifies field offsets every 64 draws and disables itself for the session on mismatch. |
| Visibility throttle (ms) | 0 (off) | Experimental. Runs `updateVisibility` at most once per interval. |
| Debug log | off | FPS, frame time avg/max, fast-batch skip ratio, throttle counters, physics checksum. |

## Known limitations

- The batch fast path reads `CCSprite` / `CCNode` fields by raw offset, so it is tied to GD 2.2081 on Windows. Other versions need new offsets.
- The visibility throttle has not been proven to leave physics unchanged in every level. Compare the `[CHK]` hash at `levelComplete` with throttle 0 and N before trusting it.
- Unlimited FPS increases CPU/GPU load and heat.

## Building

Requires the [Geode SDK](https://docs.geode-sdk.org/getting-started/) and the `GEODE_SDK` environment variable.

```
cmake -B build -A x64
cmake --build build --config RelWithDebInfo
```

## License

MIT
