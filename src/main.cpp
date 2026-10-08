// Frame Guard - SubFPS: decouples the game update rate from the drawn frame rate.
//   1. SubFPS mode: in levels the full game update runs exactly SubFPS times per second on a
//      phase-locked clock with a constant dt; the scene is drawn only at Display Hz.
//      Physics stays GD's own 240 steps/s - SubFPS only changes how often state is advanced.
//   2. Lean update: ticks that present no frame skip the draw pipeline and cosmetic work
//   3. Batch fast path: skips clean sprites in CCSpriteBatchNode::draw (self-verifying)
//   4. Optional debug log (off by default, zero hot-path cost when off)

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/CCSpriteBatchNode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif

using namespace geode::prelude;
using clk = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Settings (cached; updated by listeners, never polled in hot paths)
// ---------------------------------------------------------------------------
namespace cfg {
    static std::atomic<bool> fastBatch{true};
    static std::atomic<bool> debug{false};
    static std::atomic<int64_t> displayHz{0};   // 0 = auto-detect
    static std::atomic<bool> subfps{false};     // SubFPS mode
    static std::atomic<int64_t> subHz{240};     // SubFPS: game updates per second
    static std::atomic<int64_t> menuFps{1000};  // FPS cap outside levels, 0 = same as in levels
    static std::atomic<bool> showSubfps{false}; // on-screen SubFPS counter
    static std::atomic<bool> lean{true};        // update-only ticks skip the draw pipeline and cosmetic work
}

// ---------------------------------------------------------------------------
// Debug statistics (only touched when cfg::debug is on)
// ---------------------------------------------------------------------------
namespace dbg {
    struct Acc {
        double sum = 0, mx = 0; int n = 0;
        void add(double v) { sum += v; n++; if (v > mx) mx = v; }
        double avg() const { return n ? sum / n : 0.0; }
        void reset() { sum = mx = 0; n = 0; }
    };

    static Acc frame;                       // ms between drawScene calls
    static clk::time_point last, lastReport = clk::now();
    static bool haveLast = false;

    static double fbLoopMs = 0;             // time spent in the batch child loop
    static int64_t fbDraws = 0, fbNodes = 0, fbSkipped = 0;
    static Acc updLoop, presLoop;           // update-only loops, present loops (ms)
    static Acc schedMs, postMs, visMs;      // time inside the level update / postUpdate / updateVisibility per tick
    static int leanTicks = 0, leanSkippedBar = 0;
    static Acc tickDevUs;                   // |actual tick spacing - ideal| in microseconds
    static int ticks = 0, tickCatchup = 0, tickResync = 0;
    static int presented = 0;               // frames actually drawn+swapped (SubFPS mode)

    // Physics checksum: hash of player positions per step. Run the same macro with
    // SubFPS off and on; equal hashes at levelComplete => identical physics.
    static uint64_t hash = 1469598103934665603ULL;
    static int steps = 0;
    static void mix(float f) {
        uint32_t b; std::memcpy(&b, &f, 4);
        hash = (hash ^ b) * 1099511628211ULL;
    }
    static void resetHash() { hash = 1469598103934665603ULL; steps = 0; }

    static void onFrame() {
        auto now = clk::now();
        if (haveLast) frame.add(std::chrono::duration<double, std::milli>(now - last).count());
        last = now; haveLast = true;

        if (now - lastReport < std::chrono::seconds(2)) return;
        double wall = std::chrono::duration<double, std::milli>(now - lastReport).count();
        lastReport = now;

        log::info("FPS {:.0f} | frame avg {:.2f} max {:.2f} ms | fast-batch {} draws, {}/{} nodes skipped ({:.1f}%), loop {:.2f}% of wall | present {:.0f}/s | update-loop avg {:.2f} max {:.2f} ms | present-loop avg {:.2f} max {:.2f} ms | subfps {:.0f}/s dev avg {:.1f} max {:.1f} us catch-up {} resync {} | lean updates {} | level-update avg {:.3f} max {:.3f} ms (postUpdate {:.3f}, visibility {:.3f}) bar-skipped {}",
            1000.0 / std::max(frame.avg(), 0.001), frame.avg(), frame.mx,
            fbDraws, fbSkipped, fbNodes, fbNodes ? 100.0 * fbSkipped / fbNodes : 0.0,
            fbLoopMs / wall * 100.0,
            presented * 1000.0 / wall,
            updLoop.avg(), updLoop.mx, presLoop.avg(), presLoop.mx,
            ticks * 1000.0 / wall, tickDevUs.avg(), tickDevUs.mx, tickCatchup, tickResync,
            leanTicks, schedMs.avg(), schedMs.mx, postMs.avg(), visMs.avg(), leanSkippedBar);

        frame.reset(); fbLoopMs = 0; fbDraws = fbNodes = fbSkipped = 0;
        presented = 0;
        ticks = tickCatchup = tickResync = 0; tickDevUs.reset();
        schedMs.reset(); postMs.reset(); visMs.reset(); leanTicks = leanSkippedBar = 0;
        updLoop.reset(); presLoop.reset();
    }
}

// ---------------------------------------------------------------------------
// Batch fast path
//   CCSpriteBatchNode::draw calls updateTransform() on every child every frame.
//   In most frames nearly every child is clean and childless, so that work is
//   wasted. We read the dirty flag / child array directly and skip those.
//   Offsets are fixed to GD 2.2081 (Windows), so they are verified against the
//   real isDirty()/getChildrenCount() every 64 draws; any mismatch disables the
//   fast path for the rest of the session and falls back to the original draw.
// ---------------------------------------------------------------------------
namespace fb {
    static bool failed = false;
    static uint32_t frameCounter = 0;
    static int64_t verified = 0;

    static constexpr uintptr_t OFF_DIRTY = 0x178;    // CCSprite::m_bDirty
    static constexpr uintptr_t OFF_CHILDREN = 0xd0;  // CCNode::m_pChildren

    static inline bool rawDirty(CCSprite* s) {
        return *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(s) + OFF_DIRTY) != 0;
    }
    static inline bool rawHasChildren(CCSprite* s) {
        auto arr = *reinterpret_cast<CCArray**>(reinterpret_cast<uintptr_t>(s) + OFF_CHILDREN);
        return arr != nullptr && arr->data != nullptr && arr->data->num > 0;
    }
}

class $modify(FGBatchNode, CCSpriteBatchNode) {
    void draw() {
        if (!cfg::fastBatch.load() || fb::failed) { CCSpriteBatchNode::draw(); return; }

        auto atlas = this->getTextureAtlas();
        if (!atlas || atlas->getTotalQuads() == 0) return;

        // CC_NODE_DRAW_SETUP
        auto prog = this->getShaderProgram();
        prog->use();
        prog->setUniformsForBuiltins();

        bool dbgOn = cfg::debug.load();
        auto children = this->getChildren();
        if (children && children->data && children->data->num > 0) {
            clk::time_point t0;
            if (dbgOn) t0 = clk::now();

            bool verify = (++fb::frameCounter % 64) == 0;
            int64_t total = 0, skipped = 0;
            auto p = children->data->arr;
            auto end = p + children->data->num;
            for (; p < end && *p; ++p) {
                auto spr = static_cast<CCSprite*>(*p);
                total++;
                if (verify) {
                    fb::verified++;
                    if (fb::rawDirty(spr) != spr->isDirty() ||
                        fb::rawHasChildren(spr) != (spr->getChildrenCount() > 0)) {
                        fb::failed = true;
                        log::error("[fast-batch] field offset verification failed (dirty raw={} api={}, children raw={} api={}) - fast path disabled for this session",
                            fb::rawDirty(spr), spr->isDirty(), fb::rawHasChildren(spr), spr->getChildrenCount());
                    }
                }
                if (fb::rawDirty(spr) || fb::rawHasChildren(spr)) spr->updateTransform();
                else skipped++;
            }

            if (dbgOn) {
                dbg::fbLoopMs += std::chrono::duration<double, std::milli>(clk::now() - t0).count();
                dbg::fbNodes += total; dbg::fbSkipped += skipped;
            }
        }
        if (dbgOn) dbg::fbDraws++;

        auto bf = this->getBlendFunc();
        ccGLBlendFunc(bf.src, bf.dst);
        atlas->drawQuads();
    }
};

namespace tk { static bool lean = false; static bool barPending = false; }   // true only while an update-only tick is running

// ---------------------------------------------------------------------------
// On-screen counter (Fixed tick): "<ticks>/s TPS | <presented>/s FPS"
// ---------------------------------------------------------------------------
namespace ov {
    static Ref<CCLabelBMFont> label;
    static uint64_t ticks = 0, presents = 0;     // ticks counts every tick period, catch-ups included
    static clk::time_point last = clk::now();

    static void create(CCNode* parent) {
        label = CCLabelBMFont::create("", "bigFont.fnt");
        label->setAnchorPoint({1.f, 1.f});
        label->setScale(0.28f);
        label->setOpacity(110);
        auto ws = CCDirector::sharedDirector()->getWinSize();
        label->setPosition({ws.width - 6.f, ws.height - 20.f});   // below the percentage label
        parent->addChild(label, 10000);
    }
    static void drop() { label = nullptr; }

    static void update(clk::time_point now) {
        if (!label) return;
        double dt = std::chrono::duration<double>(now - last).count();
        if (dt < 0.25) return;
        label->setString(fmt::format("{:.0f} SubFPS", ticks / dt).c_str());
        ticks = presents = 0; last = now;
    }
}

// ---------------------------------------------------------------------------
// Debug physics checksum and per-update timing
// ---------------------------------------------------------------------------
class $modify(FGPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        if (cfg::showSubfps.load()) ov::create(this);
    }

    void onQuit() {
        ov::drop();
        PlayLayer::onQuit();
    }

    void resetLevel() {
        dbg::resetHash();
        PlayLayer::resetLevel();
    }

    void levelComplete() {
        if (cfg::debug.load()) log::info("[CHK] levelComplete step {} hash {:016x}", dbg::steps, dbg::hash);
        PlayLayer::levelComplete();
    }

    void updateProgressbar() {
        if (tk::lean) { tk::barPending = true; if (cfg::debug.load()) dbg::leanSkippedBar++; return; }  // visual only; refreshed on the next presented frame
        PlayLayer::updateProgressbar();
    }

    void postUpdate(float dt) {
        clk::time_point pt0;
        bool pdbg = cfg::debug.load();
        if (pdbg) pt0 = clk::now();
        PlayLayer::postUpdate(dt);
        if (pdbg) dbg::postMs.add(std::chrono::duration<double, std::milli>(clk::now() - pt0).count());
        if (cfg::debug.load() && m_player1 && m_player2) {
            dbg::mix(m_player1->getPositionX()); dbg::mix(m_player1->getPositionY());
            dbg::mix(m_player2->getPositionX()); dbg::mix(m_player2->getPositionY());
            if (++dbg::steps % 240 == 0)
                log::info("[CHK] step {} hash {:016x}", dbg::steps, dbg::hash);
        }
    }

    void updateVisibility(float dt) {
        if (!cfg::debug.load()) { PlayLayer::updateVisibility(dt); return; }
        auto vt0 = clk::now();
        PlayLayer::updateVisibility(dt);
        dbg::visMs.add(std::chrono::duration<double, std::milli>(clk::now() - vt0).count());
    }
};

// ---------------------------------------------------------------------------
// Present pacing
//   The scene visit + buffer swap only happen when a present is due (Display Hz).
//   Update-only ticks hide the running scene (setVisible(false)) so CCNode::visit is a
//   no-op, and CCEGLView::swapBuffers is suppressed.
// ---------------------------------------------------------------------------
namespace dec {
    static clk::time_point nextPresent = clk::now();
    static clk::duration interval = std::chrono::milliseconds(16);
    static bool skipSwap = false;

    static double detectHz() {
#ifdef GEODE_IS_WINDOWS
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
            return static_cast<double>(dm.dmDisplayFrequency);
#endif
        return 60.0;
    }

    static void refresh() {
        int64_t hz = cfg::displayHz.load();
        double h = hz > 0 ? static_cast<double>(hz) : detectHz();
        interval = std::chrono::duration_cast<clk::duration>(std::chrono::duration<double>(1.0 / h));
        nextPresent = clk::now();
        log::info("[subfps] present interval {:.3f} ms ({:.0f} Hz{})",
            1000.0 / h, h, hz > 0 ? "" : ", auto-detected");
    }

    // SubFPS mode: look without consuming, then commit once the present is really done
    static bool peek(clk::time_point now) { return now >= nextPresent; }
    static void commit(clk::time_point now) {
        nextPresent += interval;
        if (nextPresent <= now) nextPresent = now + interval;
    }
}

// ---------------------------------------------------------------------------
// Fixed tick (v1.2.0)
//   In a level, the full update (CCScheduler::update -> PlayLayer -> GD physics) runs on a
//   phase-locked clock, once per 1/tickHz, with a *constant* dt. Between ticks nothing is
//   updated; the last state is simply drawn when a present is due (monitor refresh).
//   GD's physics code and step counting are not touched: it sees what a perfectly paced
//   240 FPS game would feed it. If the game falls behind, n ticks' worth of dt is passed in
//   one update (like vanilla catch-up); beyond kMaxLagSec of lag the clock is resynced instead.
// ---------------------------------------------------------------------------
namespace tk {
    enum class Mode { Normal, Tick, Swallow };
    static Mode mode = Mode::Normal;
    static float tickDt = 1.0f / 240.0f;
    static clk::time_point nextTick = clk::now();
    static clk::time_point lastExec = clk::now();
    static bool haveExec = false;
    // Catch-up limit is a duration, not a tick count: a count of 16 is 67 ms at 240 Hz but only
    // 17 ms at 960 Hz, so one slow draw frame in a heavy level dropped time (= game slowdown).
    static constexpr double kMaxLagSec = 0.100;
    static constexpr int kMinCatchup = 16;
    static constexpr double kBias = 1e-6;  // keeps dt a hair above the step so GD never rounds it to 0 steps

    static clk::duration period() {
        double hz = static_cast<double>(std::max<int64_t>(1, cfg::subHz.load()));
        return std::chrono::duration_cast<clk::duration>(std::chrono::duration<double>(1.0 / hz));
    }

    static void reset() { nextTick = clk::now(); haveExec = false; }

    // Called when a tick is due. Computes the dt for this update and advances the clock.
    static void arm(clk::time_point now, bool dbgOn) {
        auto per = period();
        double perS = std::chrono::duration<double>(per).count();
        int64_t behind = (now - nextTick) / per;           // whole periods we are late by
        int64_t maxBehind = std::max<int64_t>(kMinCatchup, static_cast<int64_t>(kMaxLagSec / perS));
        int n = 1;
        if (behind > maxBehind) {                         // far behind: drop the debt
            nextTick = now + per;
            if (dbgOn) dbg::tickResync++;
        } else {
            n += static_cast<int>(behind);
            nextTick += per * n;
            if (n > 1 && dbgOn) dbg::tickCatchup++;
        }
        tickDt = static_cast<float>(perS * n * (1.0 + kBias));
        ov::ticks++;   // executed updates, not merged catch-up periods
        if (dbgOn) {
            if (haveExec) {
                double iv = std::chrono::duration<double>(now - lastExec).count();
                dbg::tickDevUs.add(std::fabs(iv - perS * n) * 1e6);
            }
            dbg::ticks++;
        }
        lastExec = now; haveExec = true;
    }
}

class $modify(FGScheduler, CCScheduler) {
    static void onModify(auto& self) {
        (void) self.setHookPriority("cocos2d::CCScheduler::update", Priority::VeryEarly);
    }

    void update(float dt) {
        if (tk::mode != tk::Mode::Normal && this == CCDirector::sharedDirector()->getScheduler()) {
            if (tk::mode == tk::Mode::Swallow) return;   // present-only frame: no game update
            dt = tk::tickDt;                              // tick frame: constant dt
            if (cfg::debug.load()) {
                auto t0 = clk::now();
                CCScheduler::update(dt);
                dbg::schedMs.add(std::chrono::duration<double, std::milli>(clk::now() - t0).count());
                return;
            }
        }
        CCScheduler::update(dt);
    }
};

class $modify(FGView, CCEGLView) {
    void swapBuffers() {
        if (dec::skipSwap) return;
        CCEGLView::swapBuffers();
    }
};

// ---------------------------------------------------------------------------
// SubFPS loop rate
//   SubFPS mode ON: animation interval -> spin step (period/20) and VSync off.
//   OFF: animation interval restored to whatever GD had before we touched it.
//   (VSync is not restored at runtime; restart the game to get GD's own VSync setting back.)
// ---------------------------------------------------------------------------
namespace fps {
    static bool dirty = true;
    static bool applied = false;
    static double origInterval = 1.0 / 60.0;
    static constexpr double kUnlimited = 1.0 / 99999.0;   // hard ceiling: never run faster than 99999 loops/s
    static bool inLevel = false;
    static constexpr double kTickLoop = 1.0 / 20000.0;   // SubFPS: spin granularity (50 us)

    static bool overriding() { return cfg::subfps.load(); }

    static double desired() {
        double d = kUnlimited;
        if (cfg::subfps.load() && inLevel) {
            double perS = 1.0 / static_cast<double>(std::max<int64_t>(1, cfg::subHz.load()));
            d = std::max(d, std::min(kTickLoop, perS / 20.0));   // spin step = period/20, between 10 us and 50 us
        }
        if (!inLevel) {  // menus/lobby do not need extreme rates
            int64_t mc = cfg::menuFps.load();
            if (mc > 0) d = std::max(d, 1.0 / static_cast<double>(mc));
        }
        return d;
    }

    static void apply(CCDirector* dir) {
        auto app = CCApplication::sharedApplication();
        if (overriding()) {
            double d = desired();
            dir->setAnimationInterval(d);
            if (app) { app->setAnimationInterval(d); app->toggleVerticalSync(false); }
            applied = true;
        } else if (applied) {
            dir->setAnimationInterval(origInterval);
            if (app) app->setAnimationInterval(origInterval);
            applied = false;
        }
    }

    // GD can reset the interval (e.g. when its own settings change); re-assert it.
    static void enforce(CCDirector* dir) {
        double d = desired();
        if (std::fabs(dir->getAnimationInterval() - d) > d * 1e-3) {
            dir->setAnimationInterval(d);
            if (auto app = CCApplication::sharedApplication()) app->setAnimationInterval(d);
        }
    }
}

$on_mod(Loaded) {
    auto mod = Mod::get();
    cfg::fastBatch    = mod->getSettingValue<bool>("fast-batch");
    cfg::debug        = mod->getSettingValue<bool>("debug-log");
    cfg::displayHz     = mod->getSettingValue<int64_t>("display-hz");
    cfg::menuFps       = mod->getSettingValue<int64_t>("menu-fps");
    cfg::subfps        = mod->getSettingValue<bool>("subfps-mode");
    cfg::subHz        = mod->getSettingValue<int64_t>("subfps");
    cfg::showSubfps       = mod->getSettingValue<bool>("show-subfps");
    cfg::lean          = mod->getSettingValue<bool>("lean-update");
    dec::refresh();

    listenForSettingChanges<bool>("fast-batch", [](bool v) { cfg::fastBatch = v; });
    listenForSettingChanges<bool>("debug-log", [](bool v) { cfg::debug = v; });
    listenForSettingChanges<int64_t>("display-hz", [](int64_t v) { cfg::displayHz = v; dec::refresh(); });
    listenForSettingChanges<bool>("subfps-mode", [](bool v) { cfg::subfps = v; fps::dirty = true; tk::reset(); });
    listenForSettingChanges<int64_t>("subfps", [](int64_t v) { cfg::subHz = v; fps::dirty = true; tk::reset(); });
    listenForSettingChanges<bool>("show-subfps", [](bool v) { cfg::showSubfps = v; });
    listenForSettingChanges<bool>("lean-update", [](bool v) { cfg::lean = v; });
    listenForSettingChanges<int64_t>("menu-fps", [](int64_t v) { cfg::menuFps = v; fps::dirty = true; });
}

class $modify(FGDirector, CCDirector) {
    void drawScene() {
        static int frames = 0;

        {
            bool lvl = PlayLayer::get() != nullptr;
            if (lvl != fps::inLevel) { fps::inLevel = lvl; fps::dirty = true; if (lvl) tk::reset(); }
        }

        // remember GD's own interval while we are not overriding it
        if (!fps::applied) fps::origInterval = this->getAnimationInterval();

        if (fps::dirty) { fps::dirty = false; fps::apply(this); }
        else if (fps::overriding() && ++frames % 120 == 0) fps::enforce(this);

        bool dbgOn = cfg::debug.load();

        // Fixed tick: update on a phase-locked clock with constant dt, draw at the display rate.
        // Most loop iterations return here without doing anything.
        CCScene* fscene = cfg::subfps.load() && fps::inLevel ? this->getRunningScene() : nullptr;
        if (fscene) {
            auto now = clk::now();
            bool tickDue = now >= tk::nextTick;
            bool presDue = dec::peek(now);
            if (!tickDue && !presDue) return;

            if (dbgOn) dbg::onFrame();
            else dbg::haveLast = false;

            if (tickDue) tk::arm(now, dbgOn);
            if (presDue) { dec::commit(now); ov::presents++; if (dbgOn) dbg::presented++; }
            if (presDue && cfg::showSubfps.load()) ov::update(now);

            tk::mode = tickDue ? tk::Mode::Tick : tk::Mode::Swallow;
            if (presDue && tk::barPending) {     // progress bar / percent were skipped on lean ticks
                tk::barPending = false;
                if (auto pl = PlayLayer::get()) pl->updateProgressbar();
            }
            bool hide = !presDue;                // tick without a present: update only
            auto t0 = clk::now();

            // Lean tick: nothing is drawn, so skip the whole draw pipeline (glClear, matrices,
            // scene visit, swap, stats) and run only the scheduler update. A pending scene change
            // is handled by the next present, at most one display frame later.
            if (hide && tickDue && cfg::lean.load() && !this->isPaused()) {
                tk::lean = true;
                this->getScheduler()->update(tk::tickDt);
                tk::lean = false;
                tk::mode = tk::Mode::Normal;
                auto t1 = clk::now();
                double loopMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
                if (dbgOn) { dbg::updLoop.add(loopMs); dbg::leanTicks++; }
                return;
            }
            if (hide) { fscene->retain(); fscene->setVisible(false); dec::skipSwap = true; }
            CCDirector::drawScene();
            if (hide) { dec::skipSwap = false; fscene->setVisible(true); fscene->release(); }
            tk::mode = tk::Mode::Normal;

            auto t1 = clk::now();
            double loopMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
            if (dbgOn && tickDue) (presDue ? dbg::presLoop : dbg::updLoop).add(loopMs);
            return;
        }

        if (dbgOn) dbg::onFrame();
        else dbg::haveLast = false;

        CCDirector::drawScene();
    }
};
