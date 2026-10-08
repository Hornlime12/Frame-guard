# Frame Guard

Decouples how often the game updates from how often it draws.

- **SubFPS mode** - in levels the game update runs on a precise fixed clock (SubFPS updates per second, default 240) while the screen is drawn at your monitor's refresh rate. Physics stays at GD's own 240 steps/s and is not touched; SubFPS only changes how often state is advanced.
- **Lean update** - updates that present no frame skip the draw pipeline and cosmetic work.
- **Batch fast path** - skips per-frame work for sprites that did not change. It checks its own memory offsets while running and disables itself for the session if anything looks wrong.
- **[Experimental] Visibility throttle** - runs `updateVisibility` less often. Off by default.
- **Debug log** - optional. FPS, frame time avg/max, update cost breakdown, fast-batch skip ratio, throttle counters and a physics checksum.

Windows, Geometry Dash 2.2081 only.
