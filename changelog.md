# v1.3.1
- Removed the Draw budget, Lean particles, Visibility throttle and Throttle camera limit options. Only SubFPS mode, Lean update, the batch fast path and the debug log remain. Old settings are not migrated.
- Debug log no longer shows vis-throttle, draw estimate and budget-held values.

# v1.3.0
- Renamed Fixed tick to SubFPS mode (setting `SubFPS` = game updates per second). It does not change physics: GD stays at 240 steps/s. Maximum raised to 100000; the loop itself tops out around 20000-40000 updates/s.
- Removed Unlimited FPS, Decoupled display and Logic FPS cap. Only SubFPS mode, its optimizations, the batch fast path, the visibility throttle and the debug log remain. Old settings are not migrated.
- Fixed SubFPS slowing the game at high rates: the catch-up limit was a fixed 16 ticks (17 ms at 960), so a slow frame made the clock drop time. It is now 100 ms regardless of rate.
- New Lean update (default on): updates that present no frame skip the draw pipeline; the progress bar and percentage refresh on the next presented frame.
- New Lean particles (experimental, default off).
- New Show SubFPS counter overlay.
- Debug log shows lean update count and level update / postUpdate / visibility time per update.

# v1.2.0
- Added experimental Fixed tick: in levels the game update runs on a phase-locked clock (default 240/s) with a constant dt, and the screen is drawn only at Display Hz. Physics is untouched. New settings: Fixed tick, Tick rate.
- Debug log shows tick rate, tick timing deviation, catch-ups and resyncs.

# v1.1.2
- Hard ceiling of 99999 loops/s for Unlimited FPS and Decoupled display.
- New Menu FPS cap (default 1000) applies outside levels, so the lobby no longer runs at extreme rates.

# v1.1.1
- Decoupled display: new Draw budget setting. Slow draws in very heavy levels are spaced out so the game update no longer drops to the draw frame rate.
- Debug log shows update-loop / present-loop times, estimated draw cost and how often a present was postponed.

# v1.1.0
- Added experimental Decoupled display: the game updates every loop (physics untouched) while the screen is drawn only at the monitor refresh rate. New settings: Decoupled display, Display Hz, Logic FPS cap.
- Debug log now shows the present rate.

# v1.0.1
- Unlimited FPS is now off by default, so GD's own FPS limit applies unless you turn it on.
- Clearer setting names and descriptions.

# v1.0.0
- Initial release as Frame Guard.
- Unlimited FPS toggle, batch fast path, experimental visibility throttle, optional debug log.
