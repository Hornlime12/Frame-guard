// Frame Guard - keeps Geometry Dash frame rate up in heavy levels.
//   1. Unlimited FPS toggle
//   2. Batch fast path: skips clean sprites in CCSpriteBatchNode::draw (self-verifying)
//   3. Optional updateVisibility throttle (experimental)
//   4. Optional debug log (off by default, zero hot-path cost when off)
//   5. Decoupled display (v1.1.0): game update runs every loop iteration, but the
//      scene is only drawn/presented at the monitor refresh rate. Physics is NOT touched.

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
    static std::atomic<bool> unlimitedFps{false};
    static std::atomic<bool> fastBatch{true};
    static std::atomic<bool> debug{false};
    static std::atomic<double> visThrottleMs{0.0};
    static std::atomic<double> camDelta{20.0};
    static std::atomic<bool> decoupled{false};
    static std::atomic<int64_t> displayHz{0};   // 0 = auto-detect
    static std::atomic<int64_t> logicFps{0};    // 0 = unlimited
    static std::atomic<int64_t> drawShare{75};  // max % of wall time drawing may take, 0 = off
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
    static Acc updLoop, presLoop, drawEst;  // update-only loops, present loops, estimated draw cost (ms)
    static int held = 0;                    // times a present was postponed by the draw budget
    static int presented = 0;               // frames actually drawn+swapped (decoupled mode)
    static int visRan = 0, visSkipped = 0, visForcedToggle = 0, visForcedCam = 0;

    // Physics checksum: hash of player positions per step. Run the same macro with
    // throttle 0 and N; equal hashes at levelComplete => identical physics.
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

        log::info("FPS {:.0f} | frame avg {:.2f} max {:.2f} ms | fast-batch {} draws, {}/{} nodes skipped ({:.1f}%), loop {:.2f}% of wall | vis-throttle ran {} skipped {} forced(toggle/camera) {}/{} | present {:.0f}/s | update-loop avg {:.2f} max {:.2f} ms | present-loop avg {:.2f} max {:.2f} ms | draw est avg {:.2f} max {:.2f} ms | budget-held {}",
            1000.0 / std::max(frame.avg(), 0.001), frame.avg(), frame.mx,
            fbDraws, fbSkipped, fbNodes, fbNodes ? 100.0 * fbSkipped / fbNodes : 0.0,
            fbLoopMs / wall * 100.0,
            visRan, visSkipped, visForcedToggle, visForcedCam,
            presented * 1000.0 / wall,
            updLoop.avg(), updLoop.mx, presLoop.avg(), presLoop.mx, drawEst.avg(), drawEst.mx, held);

        frame.reset(); fbLoopMs = 0; fbDraws = fbNodes = fbSkipped = 0;
        visRan = visSkipped = visForcedToggle = visForcedCam = 0;
        presented = 0; held = 0;
        updLoop.reset(); presLoop.reset(); drawEst.reset();
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

// ---------------------------------------------------------------------------
// updateVisibility throttle (experimental) + debug physics checksum
// ---------------------------------------------------------------------------
namespace vis {
    static double accum = 0.0;
    static float camX = 0, camY = 0, camScale = 1, camRot = 0;
    static bool haveCam = false;
    static void reset() { accum = 0.0; haveCam = false; }
}

class $modify(FGPlayLayer, PlayLayer) {
    void resetLevel() {
        dbg::resetHash();
        vis::reset();
        PlayLayer::resetLevel();
    }

    void levelComplete() {
        if (cfg::debug.load()) log::info("[CHK] levelComplete step {} hash {:016x}", dbg::steps, dbg::hash);
        PlayLayer::levelComplete();
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        if (cfg::debug.load() && m_player1 && m_player2) {
            dbg::mix(m_player1->getPositionX()); dbg::mix(m_player1->getPositionY());
            dbg::mix(m_player2->getPositionX()); dbg::mix(m_player2->getPositionY());
            if (++dbg::steps % 240 == 0)
                log::info("[CHK] step {} hash {:016x}", dbg::steps, dbg::hash);
        }
    }

    void updateVisibility(float dt) {
        double throttle = cfg::visThrottleMs.load();
        bool dbgOn = cfg::debug.load();
        if (throttle > 0.0) {
            vis::accum += dt;
            bool mustRun = false;

            // 1) group toggle pending (this+0x38d0, cleared at the end of updateVisibility)
            if (*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(this) + 0x38d0) != 0) {
                if (dbgOn) dbg::visForcedToggle++;
                mustRun = true;
            }

            // 2) camera moved a lot (teleport / static camera) or zoom/rotation changed
            float cx = 0, cy = 0, cs = 1, cr = 0;
            if (m_objectLayer) {
                cx = m_objectLayer->getPositionX(); cy = m_objectLayer->getPositionY();
                cs = m_objectLayer->getScale();     cr = m_objectLayer->getRotation();
            }
            double delta = cfg::camDelta.load();
            if (!vis::haveCam || std::fabs(cx - vis::camX) >= delta || std::fabs(cy - vis::camY) >= delta
                || cs != vis::camScale || cr != vis::camRot) {
                if (vis::haveCam && dbgOn) dbg::visForcedCam++;
                mustRun = true;
            }

            if (!mustRun && vis::accum * 1000.0 < throttle) {
                if (dbgOn) dbg::visSkipped++;
                return;
            }

            vis::camX = cx; vis::camY = cy; vis::camScale = cs; vis::camRot = cr; vis::haveCam = true;
            dt = static_cast<float>(vis::accum);
            vis::accum = 0.0;
        }
        if (dbgOn) dbg::visRan++;
        PlayLayer::updateVisibility(dt);
    }
};

// ---------------------------------------------------------------------------
// Decoupled display
//   Logic (CCScheduler::update -> PlayLayer -> GD's own 240 TPS physics) runs on every
//   loop iteration. The scene visit + buffer swap only happen when a present is due
//   (monitor refresh rate). Physics code is not hooked or modified; GD derives its
//   step count from the frame delta exactly as it does with plain unlimited FPS.
//   Skipped frames hide the running scene (setVisible(false)) so CCNode::visit is a
//   no-op, and CCEGLView::swapBuffers is suppressed.
// ---------------------------------------------------------------------------
namespace dec {
    static clk::time_point nextPresent = clk::now();
    static clk::duration interval = std::chrono::milliseconds(16);
    static bool skipSwap = false;
    static clk::time_point holdUntil = clk::now();   // draw budget: no present before this
    static double emaUpdMs = 0.0, emaDrawMs = 0.0;   // smoothed update-loop and draw cost

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
        nextPresent = holdUntil = clk::now();
        log::info("[decoupled] present interval {:.3f} ms ({:.0f} Hz{})",
            1000.0 / h, h, hz > 0 ? "" : ", auto-detected");
    }

    // true when the scene should be drawn and presented on this loop iteration
    static bool due() {
        auto now = clk::now();
        if (now < holdUntil) { if (cfg::debug.load()) dbg::held++; return false; }
        if (now < nextPresent) return false;
        nextPresent += interval;
        if (nextPresent <= now) nextPresent = now + interval;  // fell behind: resync
        return true;
    }

    // Called after every decoupled loop with its duration. A present loop is update + draw,
    // so draw cost ~= present-loop time - typical update-loop time. If drawing would take
    // more than the allowed share of wall time, postpone the next present so that update-only
    // loops (and therefore input handling / physics cadence) are not dragged down by slow draws.
    static void account(bool presented, double loopMs, clk::time_point end) {
        constexpr double a = 0.2;
        if (!presented) {
            emaUpdMs = emaUpdMs == 0.0 ? loopMs : emaUpdMs * (1 - a) + loopMs * a;
            return;
        }
        double draw = std::max(0.0, loopMs - emaUpdMs);
        emaDrawMs = emaDrawMs == 0.0 ? draw : emaDrawMs * (1 - a) + draw * a;
        if (cfg::debug.load()) dbg::drawEst.add(draw);

        double share = static_cast<double>(cfg::drawShare.load()) / 100.0;
        if (share <= 0.0 || share >= 1.0) return;
        double gapMs = std::min(250.0, emaDrawMs * (1.0 - share) / share);
        holdUntil = end + std::chrono::duration_cast<clk::duration>(std::chrono::duration<double, std::milli>(gapMs));
    }
}

class $modify(FGView, CCEGLView) {
    void swapBuffers() {
        if (dec::skipSwap) return;
        CCEGLView::swapBuffers();
    }
};

// ---------------------------------------------------------------------------
// Unlimited FPS / decoupled loop rate
//   Unlimited ON, or decoupled ON: animation interval -> logic cap (or ~0) and VSync off.
//   Both OFF: animation interval restored to whatever GD had before we touched it.
//   (VSync is not restored at runtime; restart the game to get GD's own VSync setting back.)
// ---------------------------------------------------------------------------
namespace fps {
    static bool dirty = true;
    static bool applied = false;
    static double origInterval = 1.0 / 60.0;
    static constexpr double kUnlimited = 1.0 / 1000000.0;

    static bool overriding() { return cfg::decoupled.load() || cfg::unlimitedFps.load(); }

    static double desired() {
        if (cfg::decoupled.load()) {
            int64_t cap = cfg::logicFps.load();
            if (cap > 0) return 1.0 / static_cast<double>(cap);
        }
        return kUnlimited;
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
    cfg::unlimitedFps = mod->getSettingValue<bool>("unlimited-fps");
    cfg::fastBatch    = mod->getSettingValue<bool>("fast-batch");
    cfg::debug        = mod->getSettingValue<bool>("debug-log");
    cfg::visThrottleMs = mod->getSettingValue<double>("vis-throttle-ms");
    cfg::camDelta      = mod->getSettingValue<double>("vis-camera-delta");
    cfg::decoupled     = mod->getSettingValue<bool>("decoupled-mode");
    cfg::displayHz     = mod->getSettingValue<int64_t>("display-hz");
    cfg::logicFps      = mod->getSettingValue<int64_t>("logic-fps");
    cfg::drawShare     = mod->getSettingValue<int64_t>("draw-budget");
    dec::refresh();

    listenForSettingChanges<bool>("unlimited-fps", [](bool v) { cfg::unlimitedFps = v; fps::dirty = true; });
    listenForSettingChanges<bool>("fast-batch", [](bool v) { cfg::fastBatch = v; });
    listenForSettingChanges<bool>("debug-log", [](bool v) { cfg::debug = v; });
    listenForSettingChanges<double>("vis-throttle-ms", [](double v) { cfg::visThrottleMs = v; vis::reset(); });
    listenForSettingChanges<double>("vis-camera-delta", [](double v) { cfg::camDelta = v; });
    listenForSettingChanges<bool>("decoupled-mode", [](bool v) { cfg::decoupled = v; fps::dirty = true; dec::refresh(); });
    listenForSettingChanges<int64_t>("display-hz", [](int64_t v) { cfg::displayHz = v; dec::refresh(); });
    listenForSettingChanges<int64_t>("draw-budget", [](int64_t v) { cfg::drawShare = v; dec::holdUntil = clk::now(); });
    listenForSettingChanges<int64_t>("logic-fps", [](int64_t v) { cfg::logicFps = v; fps::dirty = true; });
}

class $modify(FGDirector, CCDirector) {
    void drawScene() {
        static int frames = 0;

        // remember GD's own interval while we are not overriding it
        if (!fps::applied) fps::origInterval = this->getAnimationInterval();

        if (fps::dirty) { fps::dirty = false; fps::apply(this); }
        else if (fps::overriding() && ++frames % 120 == 0) fps::enforce(this);

        bool dbgOn = cfg::debug.load();
        if (dbgOn) dbg::onFrame();
        else dbg::haveLast = false;

        if (!cfg::decoupled.load()) {
            CCDirector::drawScene();
            return;
        }

        // Decoupled: update every iteration (GD's own delta + physics path), draw only when due.
        CCScene* scene = this->getRunningScene();
        bool present = dec::due() || !scene;
        auto t0 = clk::now();

        if (!present) {
            scene->retain();                 // the update may replace the scene
            scene->setVisible(false);        // CCNode::visit becomes a no-op
            dec::skipSwap = true;
            CCDirector::drawScene();         // calculateDeltaTime + scheduler update + (empty) draw
            dec::skipSwap = false;
            scene->setVisible(true);
            scene->release();
        } else {
            if (dbgOn) dbg::presented++;
            CCDirector::drawScene();
        }

        auto t1 = clk::now();
        double loopMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (dbgOn) (present ? dbg::presLoop : dbg::updLoop).add(loopMs);
        dec::account(present, loopMs, t1);
    }
};
