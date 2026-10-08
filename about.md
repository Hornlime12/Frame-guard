# Frame Guard

Defends your frame rate in heavy levels.

- **Unlimited FPS** - removes the FPS cap and turns VSync off (toggle, off by default).
- **[Experimental] Decoupled display** - the game updates every loop (physics untouched) while the screen is drawn only at your monitor's refresh rate.
- **Batch fast path** - skips per-frame work for sprites that did not change. It checks its own memory offsets while running and disables itself for the session if anything looks wrong.
- **[Experimental] Visibility throttle** - runs `updateVisibility` less often. Off by default.
- **Debug log** - optional. FPS, frame time avg/max, fast-batch skip ratio, throttle counters and a physics checksum.

Unlimited FPS is off by default. Turning it on can raise CPU/GPU load and heat.

Windows, Geometry Dash 2.2081 only.
