// Predictive Input Buffer v2.0.6 - single-file version
// Sections: 1 Config | 2 Ghost simulation | 3 Input buffer | 4 Hooks

#include <Geode/Geode.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/ui/Notification.hpp>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace geode::prelude;

// ======================================================================================
// 1. CONFIG
// ======================================================================================

namespace ib {
    struct Config {
        bool enabled = true;
        bool landingBuffer = true;
        bool hotkey = true;
        bool debug = false;
        bool assumeTap = true;        // predict as if you release shortly after takeoff (instead of holding forever)
        bool ignoreUnmodeled = false; // keep predicting through pads/orbs/etc. instead of giving up
        float framesAll = 0.5f;   // buffer window, frames @ 60fps (all modes except wave)
        float framesWave = 0.5f;  // buffer window, frames @ 60fps (wave)
        double budgetMs = 3.0;    // max time a single press prediction may take; 0 = unlimited
    };

    // Cached snapshot of the mod settings.
    Config const& config();
    void reloadConfig();

    // Advance Light -> Medium -> Heavy -> Max -> Off -> Light and persist it to settings.
    void cyclePreset();

    // Convert a window in frames (@60fps) to physics ticks for the current tick length.
    int framesToTicks(float frames, float tickLength);
}



namespace ib {
    namespace {
        struct Preset {
            char const* name;
            bool enabled;
            float all;
            float wave;
        };

        constexpr std::array<Preset, 5> PRESETS {{
            { "Off",    false, 0.f,  0.f  },
            { "Light",  true,  0.5f, 0.5f },
            { "Medium", true,  1.f,  1.f  },
            { "Heavy",  true,  2.f,  2.f  },
            { "Max",    true,  4.f,  2.f  },
        }};

        constexpr std::array<char const*, 5> CYCLE_ORDER {{ "Light", "Medium", "Heavy", "Max", "Off" }};

        Config s_config;
    }

    Config const& config() {
        return s_config;
    }

    void reloadConfig() {
        auto* mod = Mod::get();
        Config c;
        c.enabled = mod->getSettingValue<bool>("enabled");
        c.landingBuffer = mod->getSettingValue<bool>("landing-buffer");
        c.hotkey = mod->getSettingValue<bool>("hotkey");
        c.debug = mod->getSettingValue<bool>("debug");
        c.ignoreUnmodeled = mod->getSettingValue<bool>("ignore-unmodeled");
        c.assumeTap = mod->getSettingValue<bool>("assume-tap");
        c.budgetMs = mod->getSettingValue<double>("sim-budget-ms");

        auto preset = mod->getSettingValue<std::string>("preset");
        if (preset == "Custom") {
            c.framesAll = static_cast<float>(mod->getSettingValue<double>("custom-all-frames"));
            c.framesWave = static_cast<float>(mod->getSettingValue<double>("custom-wave-frames"));
        }
        else {
            for (auto const& p : PRESETS) {
                if (preset == p.name) {
                    c.enabled = c.enabled && p.enabled;
                    c.framesAll = p.all;
                    c.framesWave = p.wave;
                    break;
                }
            }
        }
        s_config = c;
    }

    void cyclePreset() {
        auto* mod = Mod::get();
        auto current = mod->getSettingValue<std::string>("preset");
        size_t next = 0; // anything unknown (e.g. Custom) restarts at Light
        for (size_t i = 0; i < CYCLE_ORDER.size(); ++i) {
            if (current == CYCLE_ORDER[i]) {
                next = (i + 1) % CYCLE_ORDER.size();
                break;
            }
        }
        std::string name = CYCLE_ORDER[next];
        mod->setSettingValue<std::string>("preset", name);
        reloadConfig();
        Notification::create("Input buffer: " + name, NotificationIcon::None, 0.8f)->show();
        log::info("[InputBuffer] preset -> {}", name);
    }

    int framesToTicks(float frames, float tickLength) {
        if (!(tickLength > 0.001f)) {
            tickLength = 0.25f;
        }
        return std::max(0, static_cast<int>(std::lround(frames / tickLength)));
    }
}

// ======================================================================================
// 2. GHOST SIMULATION
// ======================================================================================

// Ghost simulation: runs invisible copies of the player(s) forward in time to see
// whether a press at a given delay survives. Nothing here touches the real game state.
namespace ib::sim {
    // lifecycle (one ghost pair per PlayLayer)
    void create(PlayLayer* layer);
    void destroy();

    // queried by the hooks that must go silent during a simulation
    bool active();
    bool isGhost(PlayerObject* player);
    void markDied();

    // physics tick length in frames @ 60fps (0.25 normally)
    void setTickLength(float frames);
    float tickLength();
    // scale a tick count that was tuned for the default 0.25-frame tick
    int scaled(int ticksAtDefaultTick);

    // each button press gets a time budget; check timedOut() after sim calls
    void beginQuery(double budgetMs);
    bool timedOut();
    bool unsure();            // last run() stopped at a thing it can't model
    int unsureObject();       // object ID of that thing
    int unsureType();         // its GameObjectType

    bool isCube(PlayerObject* player);

    // During a sim, keep only objects the ghost should really collide with and apply
    // portal effects to the ghost directly. `out` is then handed to the real collision code.
    void collectRelevant(PlayerObject* player, gd::vector<GameObject*>* objects, int count, gd::vector<GameObject*>& out);

    // Survived ticks if the button is set to `down` after `delay` ticks (button state before
    // that is !down). Returns `horizon` if the future looks safe or can't be judged.
    int run(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, bool* diedBeforeInput);

    // Ticks until the cube touches ground with no input; -1 if it dies, hits something
    // unmodeled, or doesn't land within maxTicks.
    int ticksToLanding(PlayLayer* layer, PlayerObject* p1, int maxTicks);
}



namespace ib::sim {
    namespace {
        constexpr float DEFAULT_TICK = 0.25f;
        constexpr float GHOST_START_Y = 105.f;

        constexpr uint64_t typeBit(GameObjectType type) {
            return 1ull << static_cast<int>(type);
        }

        constexpr uint64_t COLLIDING_TYPES = typeBit(GameObjectType::Solid) | typeBit(GameObjectType::Hazard) | typeBit(GameObjectType::Slope) |
                                             typeBit(GameObjectType::CollisionObject) | typeBit(GameObjectType::AnimatedHazard);

        // Flip one to false to turn that portal family into a "stop, don't guess" boundary.
        constexpr bool MODEL_GRAVITY = true;
        constexpr bool MODEL_SIZE = true;
        constexpr bool MODEL_SPEED = true;
        constexpr bool MODEL_MODE = true;

        constexpr uint64_t MODELED_TYPES = typeBit(GameObjectType::NormalGravityPortal) | typeBit(GameObjectType::InverseGravityPortal) |
                                           typeBit(GameObjectType::RegularSizePortal) | typeBit(GameObjectType::MiniSizePortal) |
                                           typeBit(GameObjectType::CubePortal) | typeBit(GameObjectType::ShipPortal) |
                                           typeBit(GameObjectType::BallPortal) | typeBit(GameObjectType::UfoPortal) |
                                           typeBit(GameObjectType::WavePortal) | typeBit(GameObjectType::RobotPortal) |
                                           typeBit(GameObjectType::SpiderPortal) | typeBit(GameObjectType::SwingPortal);

        constexpr uint64_t UNMODELED_TYPES = typeBit(GameObjectType::YellowJumpPad) | typeBit(GameObjectType::PinkJumpPad) |
                                             typeBit(GameObjectType::RedJumpPad) | typeBit(GameObjectType::GravityPad) |
                                             typeBit(GameObjectType::SpiderPad) | typeBit(GameObjectType::YellowJumpRing) |
                                             typeBit(GameObjectType::PinkJumpRing) | typeBit(GameObjectType::RedJumpRing) |
                                             typeBit(GameObjectType::GravityRing) | typeBit(GameObjectType::GreenRing) |
                                             typeBit(GameObjectType::DropRing) | typeBit(GameObjectType::CustomRing) |
                                             typeBit(GameObjectType::DashRing) | typeBit(GameObjectType::GravityDashRing) |
                                             typeBit(GameObjectType::SpiderOrb) | typeBit(GameObjectType::DualPortal) |
                                             typeBit(GameObjectType::SoloPortal) | typeBit(GameObjectType::InverseMirrorPortal) |
                                             typeBit(GameObjectType::NormalMirrorPortal) | typeBit(GameObjectType::TeleportPortal) |
                                             typeBit(GameObjectType::GravityTogglePortal);

        constexpr uint64_t SPECIAL_TYPES = MODELED_TYPES | UNMODELED_TYPES;

        Ref<PlayerObject> s_ghost1;
        Ref<PlayerObject> s_ghost2;

        bool s_active = false;
        bool s_died = false;
        bool s_unsure = false;
        int s_unsureObject = 0;
        int s_unsureType = 0;
        float s_tick = DEFAULT_TICK;

        std::chrono::steady_clock::time_point s_deadline = std::chrono::steady_clock::time_point::max();
        bool s_timedOut = false;

        std::vector<std::pair<PlayerObject*, GameObject*>> s_touched;

        // sets s_active for exactly the lifetime of one simulation, whatever path it exits by
        struct Scope {
            Scope() { s_active = true; }
            ~Scope() { s_active = false; }
        };

        bool expired() {
            if (s_timedOut) {
                return true;
            }
            if (std::chrono::steady_clock::now() >= s_deadline) {
                s_timedOut = true;
            }
            return s_timedOut;
        }

        PlayerObject* makeGhost(PlayLayer* layer) {
            auto* ghost = PlayerObject::create(1, 1, layer, layer, true);
            if (!ghost) {
                return nullptr;
            }
            ghost->setPosition(CCPoint(0.f, GHOST_START_Y));
            ghost->setVisible(false);
            layer->m_objectLayer->addChild(ghost);
            return ghost;
        }

        void dropGhost(Ref<PlayerObject>& ghost) {
            if (ghost) {
                ghost->removeFromParent();
                ghost = nullptr;
            }
        }

        void setButton(PlayerObject* ghost, bool down) {
            if (down) {
                ghost->pushButton(PlayerButton::Jump);
            }
            else {
                ghost->releaseButton(PlayerButton::Jump);
            }
        }

        void copyState(PlayerObject* ghost, PlayerObject* real) {
            ghost->setVisible(false);
            ghost->copyAttributes(real);
            ghost->setScale(real->getScale());
            ghost->setContentSize(real->getContentSize());
            ghost->m_width = real->m_width;
            ghost->m_height = real->m_height;
            ghost->m_gravityMod = real->m_gravityMod;
            ghost->m_isOnGround = real->m_isOnGround;
            ghost->m_isUpsideDown = real->m_isUpsideDown;
            ghost->m_vehicleSize = real->m_vehicleSize;
            ghost->m_playerSpeed = real->m_playerSpeed;
        }

        void advance(PlayerObject* ghost, float dt) {
            ghost->update(dt);
            ghost->updateRotation(dt);
        }

        void checkCollisions(PlayLayer* layer, PlayerObject* ghost, float dt) {
            ghost->m_collisionLogTop->removeAllObjects();
            ghost->m_collisionLogBottom->removeAllObjects();
            ghost->m_collisionLogLeft->removeAllObjects();
            ghost->m_collisionLogRight->removeAllObjects();
            layer->checkCollisions(ghost, dt, false);
        }

        bool isFlying(PlayerObject* p) {
            return p->m_isShip || p->m_isDart || p->m_isBird || p->m_isSwing;
        }

        enum class Mode { Cube, Ship, Ball, Ufo, Wave, Robot, Spider, Swing };

        bool inMode(PlayerObject* p, Mode mode) {
            switch (mode) {
                case Mode::Ship: return p->m_isShip;
                case Mode::Ball: return p->m_isBall;
                case Mode::Ufo: return p->m_isBird;
                case Mode::Wave: return p->m_isDart;
                case Mode::Robot: return p->m_isRobot;
                case Mode::Spider: return p->m_isSpider;
                case Mode::Swing: return p->m_isSwing;
                case Mode::Cube: return ib::sim::isCube(p);
            }
            return false;
        }

        void enterMode(PlayerObject* p, Mode mode) {
            if (inMode(p, mode)) {
                return;
            }
            if (p->m_isShip) p->toggleFlyMode(false, true);
            if (p->m_isBall) p->toggleRollMode(false, true);
            if (p->m_isBird) p->toggleBirdMode(false, true);
            if (p->m_isDart) p->toggleDartMode(false, true);
            if (p->m_isRobot) p->toggleRobotMode(false, true);
            if (p->m_isSpider) p->toggleSpiderMode(false, true);
            if (p->m_isSwing) p->toggleSwingMode(false, true);

            switch (mode) {
                case Mode::Ship: p->toggleFlyMode(true, true); break;
                case Mode::Ball: p->toggleRollMode(true, true); break;
                case Mode::Ufo: p->toggleBirdMode(true, true); break;
                case Mode::Wave: p->toggleDartMode(true, true); break;
                case Mode::Robot: p->toggleRobotMode(true, true); break;
                case Mode::Spider: p->toggleSpiderMode(true, true); break;
                case Mode::Swing: p->toggleSwingMode(true, true); break;
                case Mode::Cube: break;
            }
        }

        float speedOf(int objectID) {
            switch (objectID) {
                case 200: return 0.7f;
                case 201: return 0.9f;
                case 202: return 1.1f;
                case 203: return 1.3f;
                case 1334: return 1.6f;
                default: return 0.f;
            }
        }

        // each portal applies once per ghost per simulation
        bool firstTouch(PlayerObject* player, GameObject* object) {
            for (auto const& t : s_touched) {
                if (t.first == player && t.second == object) {
                    return false;
                }
            }
            s_touched.emplace_back(player, object);
            return true;
        }

        void touchSpecial(PlayerObject* player, GameObject* object) {
            s_unsureObject = object->m_objectID;
            s_unsureType = static_cast<int>(object->m_objectType);
            float speed = speedOf(object->m_objectID);
            if (speed > 0.f) {
                if (!MODEL_SPEED) {
                    s_unsure = true;
                }
                else if (firstTouch(player, object)) {
                    player->m_playerSpeed = speed;
                }
                return;
            }

            auto type = object->m_objectType;
            switch (type) {
                case GameObjectType::NormalGravityPortal:
                case GameObjectType::InverseGravityPortal: {
                    if (!MODEL_GRAVITY) {
                        s_unsure = true;
                        return;
                    }
                    if (!firstTouch(player, object)) {
                        return;
                    }
                    bool upsideDown = type == GameObjectType::InverseGravityPortal;
                    if (player->m_isUpsideDown != upsideDown) {
                        player->flipGravity(upsideDown, true);
                    }
                    return;
                }
                case GameObjectType::RegularSizePortal:
                case GameObjectType::MiniSizePortal: {
                    if (!MODEL_SIZE) {
                        s_unsure = true;
                        return;
                    }
                    if (!firstTouch(player, object)) {
                        return;
                    }
                    bool mini = type == GameObjectType::MiniSizePortal;
                    if ((player->m_vehicleSize < 1.f) != mini) {
                        player->togglePlayerScale(mini, true);
                    }
                    return;
                }
                case GameObjectType::CubePortal:
                case GameObjectType::ShipPortal:
                case GameObjectType::BallPortal:
                case GameObjectType::UfoPortal:
                case GameObjectType::WavePortal:
                case GameObjectType::RobotPortal:
                case GameObjectType::SpiderPortal:
                case GameObjectType::SwingPortal: {
                    if (!MODEL_MODE) {
                        s_unsure = true;
                        return;
                    }
                    if (!firstTouch(player, object)) {
                        return;
                    }
                    Mode mode = Mode::Cube;
                    switch (type) {
                        case GameObjectType::ShipPortal: mode = Mode::Ship; break;
                        case GameObjectType::BallPortal: mode = Mode::Ball; break;
                        case GameObjectType::UfoPortal: mode = Mode::Ufo; break;
                        case GameObjectType::WavePortal: mode = Mode::Wave; break;
                        case GameObjectType::RobotPortal: mode = Mode::Robot; break;
                        case GameObjectType::SpiderPortal: mode = Mode::Spider; break;
                        case GameObjectType::SwingPortal: mode = Mode::Swing; break;
                        default: break;
                    }
                    enterMode(player, mode);
                    return;
                }
                default:
                    // pads, orbs, dual/solo/mirror/teleport: not modeled.
                    // Default: stop predicting (safe). Optional: ignore the object and keep going.
                    if (!ib::config().ignoreUnmodeled) {
                        s_unsure = true;
                    }
                    return;
            }
        }

        float simDt() {
            return (s_tick > 0.001f && s_tick < 2.0f) ? s_tick : DEFAULT_TICK;
        }
    }

    // ---- lifecycle -------------------------------------------------------------------

    void create(PlayLayer* layer) {
        destroy();
        s_ghost1 = makeGhost(layer);
        // ghost2 is created lazily the first time dual mode needs it
    }

    void destroy() {
        dropGhost(s_ghost1);
        dropGhost(s_ghost2);
        s_active = false;
        s_touched.clear();
    }

    // ---- state -----------------------------------------------------------------------

    bool active() { return s_active; }

    bool isGhost(PlayerObject* player) {
        return player && (player == s_ghost1.data() || player == s_ghost2.data());
    }

    void markDied() { s_died = true; }

    void setTickLength(float frames) { s_tick = frames; }
    float tickLength() { return simDt(); }

    int scaled(int ticksAtDefaultTick) {
        return std::max(1, static_cast<int>(std::lround(ticksAtDefaultTick * DEFAULT_TICK / simDt())));
    }

    void beginQuery(double budgetMs) {
        s_timedOut = false;
        if (budgetMs > 0.0) {
            s_deadline = std::chrono::steady_clock::now() +
                         std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::milli>(budgetMs));
        }
        else {
            s_deadline = std::chrono::steady_clock::time_point::max();
        }
    }

    bool timedOut() { return s_timedOut; }
    bool unsure() { return s_unsure; }
    int unsureObject() { return s_unsureObject; }
    int unsureType() { return s_unsureType; }

    bool isCube(PlayerObject* p) {
        return !(p->m_isShip || p->m_isBall || p->m_isBird || p->m_isDart || p->m_isRobot || p->m_isSpider || p->m_isSwing);
    }

    // ---- collision filtering ---------------------------------------------------------

    void collectRelevant(PlayerObject* player, gd::vector<GameObject*>* objects, int count, gd::vector<GameObject*>& out) {
        out.clear();
        if (static_cast<int>(out.capacity()) < count) {
            out.reserve(count);
        }
        CCRect playerRect;
        bool haveRect = false;
        for (int i = 0; i < count; ++i) {
            auto* object = objects->at(i);
            if (!object) {
                continue;
            }
            auto type = static_cast<unsigned>(object->m_objectType);
            if (type < 64 && ((COLLIDING_TYPES >> type) & 1)) {
                out.push_back(object);
                continue;
            }
            // Portals / pads / orbs never reach the real collision code during a sim.
            // Check overlap here and apply the effect to the ghost only.
            bool special = (type < 64 && ((SPECIAL_TYPES >> type) & 1)) || speedOf(object->m_objectID) > 0.f;
            if (!special) {
                continue;
            }
            if (!haveRect) {
                playerRect = player->getObjectRect();
                haveRect = true;
            }
            if (playerRect.intersectsRect(object->getObjectRect())) {
                touchSpecial(player, object);
            }
        }
    }

    // ---- simulation ------------------------------------------------------------------

    int run(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, bool* diedBeforeInput) {
        *diedBeforeInput = false;

        bool second = dual && p2;
        if (second && !s_ghost2) {
            s_ghost2 = makeGhost(layer);
        }
        PlayerObject* g1 = s_ghost1.data();
        PlayerObject* g2 = second ? s_ghost2.data() : nullptr;
        if (!g1 || (second && !g2)) {
            return horizon;
        }

        int total = delay + horizon;
        if (total <= 0) {
            return 0;
        }

        Scope scope;
        s_died = false;
        s_unsure = false;
        s_touched.clear();

        copyState(g1, p1);
        if (g2) {
            copyState(g2, p2);
        }

        float dt = simDt();
        if (delay > 0) {
            setButton(g1, !down);
            if (g2) setButton(g2, !down);
        }

        int minAirTicks = scaled(6);
        int maxGroundTicks = scaled(2);
        bool tapModel = ib::config().assumeTap;
        bool released = false;
        int tapTicks = scaled(2);

        int survived = 0;
        int airTicks = 0;
        int groundTicks = 0;
        int tick = 0;
        bool died = false;
        for (; tick < total; ++tick) {
            if (expired()) {
                return horizon; // caller checks timedOut() and falls back to raw input
            }
            if (tick == delay) {
                setButton(g1, down);
                if (g2) setButton(g2, down);
            }

            advance(g1, dt);
            if (g2) advance(g2, dt);
            if (s_died) {
                died = true;
                break;
            }

            checkCollisions(layer, g1, dt);
            if (s_unsure) {
                // Checked before death on purpose: a death on the same tick can't be
                // trusted. If the input hasn't been sent yet this variant can't be
                // validated (0); otherwise everything up to here was valid.
                return tick < delay ? 0 : horizon;
            }
            if (s_died) {
                died = true;
                break;
            }
            if (g2) {
                checkCollisions(layer, g2, dt);
                if (s_unsure) {
                    return tick < delay ? 0 : horizon;
                }
                if (s_died) {
                    died = true;
                    break;
                }
            }

            if (tick < delay) {
                continue;
            }
            ++survived;

            // a portal may have changed the gamemode mid-sim
            if (isFlying(g1) || (g2 && isFlying(g2))) {
                continue;
            }

            bool grounded = g1->m_isOnGround && (!g2 || g2->m_isOnGround);
            if (!grounded) {
                ++airTicks;
                groundTicks = 0;
                // Tap model: a cube that has taken off is assumed to be released, so it lands and
                // the jump can be judged. Holding forever makes it bunny-hop into whatever comes
                // next, which would make almost every delayed variant look like a death.
                if (tapModel && down && !released && airTicks >= tapTicks && isCube(g1) && (!g2 || isCube(g2))) {
                    setButton(g1, false);
                    if (g2) setButton(g2, false);
                    released = true;
                }
                continue;
            }
            if (airTicks < minAirTicks) {
                continue;
            }
            if (groundTicks > maxGroundTicks) {
                return horizon; // came back down and stayed down: safe
            }
            ++groundTicks;
        }

        if (died && tick < delay) {
            *diedBeforeInput = true;
        }
        return survived;
    }

    int ticksToLanding(PlayLayer* layer, PlayerObject* p1, int maxTicks) {
        PlayerObject* g1 = s_ghost1.data();
        if (!g1) {
            return -1;
        }

        Scope scope;
        s_died = false;
        s_unsure = false;
        s_touched.clear();

        copyState(g1, p1);
        setButton(g1, false);

        float dt = simDt();
        for (int tick = 1; tick <= maxTicks; ++tick) {
            if (expired()) {
                return -1;
            }
            advance(g1, dt);
            if (s_died) {
                return -1;
            }
            checkCollisions(layer, g1, dt);
            if (s_unsure || s_died) {
                return -1;
            }
            if (g1->m_isOnGround) {
                return tick;
            }
        }
        return -1;
    }
}

// ======================================================================================
// 3. INPUT BUFFER
// ======================================================================================

namespace ib {
    // Owns all real-input bookkeeping. Explicit state machine instead of magic ints:
    //   Idle    - nothing pending; each tick re-syncs the game's button with the real one
    //   Waiting - a press/release was swallowed and will be delivered after `countdown_` ticks
    //   Holding - the buffered press was delivered; keep it down for `holdLeft_` ticks and/or
    //             deliver a release that arrived early
    class InputBuffer {
    public:
        // Called from GJBaseGameLayer::handleButton. Returns true if the input was swallowed.
        bool onButton(PlayLayer* layer, GJBaseGameLayer* self, bool down, bool isPlayer1);
        // Called once per physics tick before PlayerObject::update (player 1 only).
        void onTick(PlayLayer* layer, PlayerObject* p1);
        void reset();
        bool injecting() const { return injecting_; }

    private:
        enum class Phase { Idle, Waiting, Holding };

        bool pass(bool down, char const* why = nullptr);
        void clearPending();
        void schedule(bool state, int countdown, int hold);
        void send(PlayLayer* layer, bool down);

        Phase phase_ = Phase::Idle;
        int countdown_ = 0;
        int holdLeft_ = 0;
        bool pendingState_ = true;
        bool releaseQueued_ = false;
        bool repress_ = false;     // press -> release -> press all arrived inside one delay
        bool repressDue_ = false;  // the second press is delivered one tick after the release

        bool realInput_ = false;
        bool appliedInput_ = false;
        bool injecting_ = false;
        bool isPlayer1_ = true;

        int sinceInput_ = 999;
        int rapid_ = 0;
    };

    InputBuffer& buffer();
}



namespace ib {
    namespace {
        // The cube landing buffer presses this many ticks BEFORE touchdown and keeps the
        // button down until touchdown has passed, so the jump comes out on the first
        // possible tick, exactly like holding the button through a landing.
        constexpr int LANDING_LEAD = 1;

        // Prediction horizons, in frames @ 60fps (28 / 38 / 72 ticks at the default tick).
        constexpr float HORIZON_MINI_WAVE = 7.f;
        constexpr float HORIZON_WAVE = 9.5f;
        // A cube jump stays airborne for ~23 frames, so ground modes must look further than that
        // or an early press that lands on a hazard after the horizon is wrongly judged safe.
        constexpr float HORIZON_GROUND = 32.f;  // cube, ball, robot, spider
        // UFO / swing: the sim assumes NO further clicks, but a real player clicks again within a few
        // frames, so a long look-ahead makes every variant look like a death. Judge only the
        // immediate result of this click.
        constexpr float HORIZON_FLAP = 10.f;

        // Tuned at the default 0.25-frame tick; scaled with sim::scaled().
        constexpr int RAPID_GAP_TICKS = 8;     // inputs closer than this count as spam
        constexpr int CUBE_EXTRA_TICKS = 12;   // a delay must outlive the raw press by this much
        constexpr int WAVE_EXTRA_TICKS = 10;
        constexpr int WAVE_GAIN_TICKS = 6;
        constexpr int WAVE_STEP = 2;           // threshold grows per delay tick (scale-invariant)
    }

    InputBuffer& buffer() {
        static InputBuffer instance;
        return instance;
    }

    bool InputBuffer::pass(bool down, char const* why) {
        appliedInput_ = down;
        if (why && config().debug) {
            log::info("[InputBuffer]   not buffered: {}", why);
        }
        return false;
    }

    void InputBuffer::clearPending() {
        phase_ = Phase::Idle;
        countdown_ = 0;
        holdLeft_ = 0;
        releaseQueued_ = false;
        repress_ = false;
        repressDue_ = false;
    }

    void InputBuffer::schedule(bool state, int countdown, int hold) {
        phase_ = Phase::Waiting;
        pendingState_ = state;
        countdown_ = countdown;
        holdLeft_ = hold;
        releaseQueued_ = false;
        repress_ = false;
        repressDue_ = false;
    }

    // Full wipe. realInput_ must be cleared too, otherwise the Idle re-sync would inject a
    // press the player never made after a respawn.
    void InputBuffer::reset() {
        clearPending();
        realInput_ = false;
        appliedInput_ = false;
        injecting_ = false;
        sinceInput_ = 999;
        rapid_ = 0;
    }

    void InputBuffer::send(PlayLayer* layer, bool down) {
        injecting_ = true;
        appliedInput_ = down;
        layer->handleButton(down, 1, isPlayer1_);
        injecting_ = false;
    }

    bool InputBuffer::onButton(PlayLayer* layer, GJBaseGameLayer* self, bool down, bool isPlayer1) {
        auto const& cfg = config();

        bool dual = layer->m_gameState.m_isDualMode && layer->m_player2;
        if (layer->m_level && !isPlayer1 && layer->m_level->m_twoPlayerMode) {
            return false;
        }

        isPlayer1_ = isPlayer1;
        realInput_ = down;
        if (!cfg.enabled) {
            return pass(down);
        }

        auto* p1 = self->m_player1;
        auto* p2 = dual ? layer->m_player2 : nullptr;
        if (!p1 || p1->m_isDead || p1->m_isShip || (p2 && p2->m_isShip)) {
            clearPending();
            return pass(down);
        }

        // ---- input arriving while something is already pending ----
        if (phase_ == Phase::Waiting) {
            if (down != pendingState_) {
                releaseQueued_ = true;   // release arrived during the delay: replay it after the press
                repress_ = false;
            }
            else if (releaseQueued_) {
                repress_ = true;         // second press arrived: keep it instead of dropping it
            }
            sinceInput_ = 0;
            return true;
        }
        if (phase_ == Phase::Holding && !down && holdLeft_ > 0) {
            // real release arrived while the landing press is still guarded; hold it back
            releaseQueued_ = true;
            sinceInput_ = 0;
            return true;
        }
        if (phase_ == Phase::Holding) {
            bool opposite = !pendingState_;
            bool flushRepress = repressDue_;
            clearPending();
            if (flushRepress) {
                send(layer, pendingState_);
            }
            else if (appliedInput_ != opposite) {
                send(layer, opposite);
            }
        }

        bool p1Wave = p1->m_isDart;
        bool p2Wave = p2 && p2->m_isDart;
        bool anyWave = p1Wave || p2Wave;
        bool mini = (p1Wave && p1->m_vehicleSize < 1.f) || (p2Wave && p2->m_vehicleSize < 1.f);

        // ---- spam bypass: rapid wave tapping is never buffered ----
        if (sinceInput_ < sim::scaled(RAPID_GAP_TICKS)) {
            int before = rapid_++;
            sinceInput_ = 0;
            if (before >= 2 && anyWave) {
                return pass(down);
            }
        }
        else {
            sinceInput_ = 0;
            rapid_ = 0;
        }

        // ---- releases only matter for wave and robot ----
        if (!p1Wave) {
            bool robot = p1->m_isRobot || (p2 && !p2Wave && p2->m_isRobot);
            if (!p2Wave && !down && !robot) {
                return pass(down);
            }
        }

        float tick = sim::tickLength();
        int maxDelay = framesToTicks(anyWave ? cfg.framesWave : cfg.framesAll, tick);
        if (maxDelay <= 0) {
            return pass(down, "window is 0 ticks (preset Off?)");
        }
        bool groundMode = sim::isCube(p1) || p1->m_isBall || p1->m_isRobot || p1->m_isSpider;
        bool flapMode = p1->m_isBird || p1->m_isSwing || (p2 && (p2->m_isBird || p2->m_isSwing));
        int horizon = framesToTicks(
            mini ? HORIZON_MINI_WAVE : (anyWave ? HORIZON_WAVE : (groundMode ? HORIZON_GROUND : HORIZON_FLAP)), tick);
        char const* name = down ? "PRESS" : "RELEASE";
        if (cfg.debug) {
            log::info("[InputBuffer] {} cube={} ufo={} wave={} dual={} onGround={} window={}t horizon={}t x={:.0f} speed={:.2f} tick={:.3f}",
                name, sim::isCube(p1), p1->m_isBird, anyWave, dual, p1->m_isOnGround, maxDelay, horizon, p1->getPositionX(), p1->m_playerSpeed, tick);
        }

        sim::beginQuery(cfg.budgetMs);
        bool diedEarly = false;
        int base = sim::run(layer, p1, p2, dual, 0, down, horizon, &diedEarly);
        if (sim::timedOut()) {
            return pass(down, "prediction exceeded the time budget (raise it in settings)");
        }

        // ---- cube landing buffer ----
        // A mid-air press does nothing in-game and a tap is gone before touchdown. Work out
        // when the cube lands with no input and fire the press right then, if that is inside
        // the window and no worse than pressing now. Must run before the "immediate press is
        // safe" shortcut, because the sim holds the button forever and would call a lost tap safe.
        if (cfg.landingBuffer && down && !dual && !anyWave && !p1->m_isOnGround && sim::isCube(p1)) {
            int land = sim::ticksToLanding(layer, p1, maxDelay);
            if (sim::timedOut()) {
                return pass(down, "prediction exceeded the time budget (raise it in settings)");
            }
            if (land > 0) {
                int fire = std::max(0, land - LANDING_LEAD);
                bool diedLate = false;
                int ret = sim::run(layer, p1, p2, dual, fire, down, horizon, &diedLate);
                if (sim::timedOut()) {
                    return pass(down, "prediction exceeded the time budget (raise it in settings)");
                }
                if (!diedLate && ret >= std::min(base, horizon)) {
                    if (cfg.debug) {
                        log::info("[InputBuffer] LANDING BUFFER {} in {} ticks (lands in {}) base={} -> {} limit={}",
                            name, fire, land, base, ret, maxDelay);
                    }
                    schedule(down, fire, land - fire);
                    return true;
                }
            }
        }

        if (base >= horizon) {
            if (cfg.debug && sim::unsure()) {
                log::info("[InputBuffer]   prediction stopped at unmodeled object id {} (type {}) and was treated as safe", sim::unsureObject(), sim::unsureType());
            }
            return pass(down, "pressing now looks safe (or prediction hit an unmodeled object)");
        }

        // ---- search for the smallest delay that clearly helps ----
        int saved = 0;
        int landed = 0;
        int bestRet = -1;
        int bestK = 0;
        int need = 0;
        std::string trace; // k:survivedTicks for every delay tried ('?' = hit unmodeled object, 'x' = died before the press)
        if (!anyWave) {
            int target = std::min(horizon, base + sim::scaled(CUBE_EXTRA_TICKS));
            need = target;
            for (int k = 1; k <= maxDelay; ++k) {
                int ret = sim::run(layer, p1, p2, dual, k, down, horizon, &diedEarly);
                if (sim::timedOut()) {
                    return pass(down, "prediction exceeded the time budget (raise it in settings)");
                }
                trace += std::to_string(k) + ":" + std::to_string(ret) + (sim::unsure() ? "?" : "") + (diedEarly ? "x" : "") + " ";
                if (diedEarly) {
                    break;
                }
                if (ret > bestRet) {
                    bestRet = ret;
                    bestK = k;
                }
                if (ret >= target) {
                    saved = k;
                    landed = ret;
                    break;
                }
            }
        }
        else {
            int target = base + sim::scaled(WAVE_EXTRA_TICKS);
            int gain = sim::scaled(WAVE_GAIN_TICKS);
            for (int k = 1; k <= maxDelay; ++k) {
                int ret = sim::run(layer, p1, p2, dual, k, down, horizon, &diedEarly);
                if (sim::timedOut()) {
                    return pass(down, "prediction exceeded the time budget (raise it in settings)");
                }
                if (diedEarly) {
                    break;
                }
                bool good = ret < horizon ? ret >= target : (ret - base >= gain || ret >= target);
                if (good) {
                    saved = k;
                    landed = ret;
                    break;
                }
                target += WAVE_STEP;
                gain += WAVE_STEP;
            }
        }

        if (saved <= 0) {
            if (cfg.debug) {
                log::info("[InputBuffer]   pressing now dies after {} of {} ticks; best delay in window = {} ticks -> survives {} (needed {})", base, horizon, bestK, bestRet, need);
                log::info("[InputBuffer]   delay curve (k:survived): {}", trace);
            }
            return pass(down, "no delay inside the window helps");
        }

        if (cfg.debug) {
            log::info("[InputBuffer] SAVED +{} ticks ({:.2f} frames) {} base={} -> {} limit={}",
                saved, saved * tick, name, base, landed, maxDelay);
        }
        schedule(down, saved, 0);
        return true;
    }

    void InputBuffer::onTick(PlayLayer* layer, PlayerObject* p1) {
        if (sinceInput_ < 999) {
            ++sinceInput_;
        }

        bool dual = layer->m_gameState.m_isDualMode && layer->m_player2;
        if (p1->m_isShip || (dual && layer->m_player2->m_isShip)) {
            clearPending();
            if (appliedInput_ != realInput_) {
                send(layer, realInput_);
            }
            return;
        }

        switch (phase_) {
            case Phase::Idle:
                if (appliedInput_ != realInput_) {
                    send(layer, realInput_);
                }
                break;

            case Phase::Waiting:
                if (countdown_ > 0) {
                    --countdown_;
                    break;
                }
                if (config().debug) {
                    log::info("[InputBuffer]   delivered {} (onGround={}, hold={})",
                        pendingState_ ? "PRESS" : "RELEASE", p1->m_isOnGround, holdLeft_);
                }
                send(layer, pendingState_);
                phase_ = (releaseQueued_ || holdLeft_ > 0) ? Phase::Holding : Phase::Idle;
                break;

            case Phase::Holding:
                if (holdLeft_ > 0) {
                    --holdLeft_;
                    break;
                }
                if (repressDue_) {
                    repressDue_ = false;
                    send(layer, pendingState_);
                    phase_ = Phase::Idle;
                    break;
                }
                if (releaseQueued_) {
                    send(layer, !pendingState_);
                    releaseQueued_ = false;
                    if (repress_) {
                        repress_ = false;
                        repressDue_ = true;   // stay in Holding; press again next tick
                        break;
                    }
                }
                phase_ = Phase::Idle;
                break;
        }
    }
}

// ======================================================================================
// 4. HOOKS
// ======================================================================================



namespace {
    // run our hooks last so we see the final state other mods produced
    constexpr int HOOK_PRIORITY = -1000000;
    constexpr int CONFIG_REFRESH_FRAMES = 30;

    bool gdHasFocus() {
        HWND fg = GetForegroundWindow();
        if (!fg) {
            return false;
        }
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        return pid == GetCurrentProcessId();
    }
}

$on_mod(Loaded) {
    ib::reloadConfig();
    log::info("[InputBuffer] v2.0.6 loaded");
}

class $modify(BufPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayLayer::playEndAnimationToPos", HOOK_PRIORITY);
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }
        ib::reloadConfig();
        ib::sim::create(this);
        ib::buffer().reset();
        return true;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (ib::sim::active() && ib::sim::isGhost(player)) {
            ib::sim::markDied();
            return;
        }
        ib::buffer().reset();
        PlayLayer::destroyPlayer(player, object);
    }

    // checkpoints / practice / startpos / normal respawns all come through here
    void resetLevel() {
        PlayLayer::resetLevel();
        ib::buffer().reset();
    }

    void onQuit() {
        PlayLayer::onQuit();
        ib::sim::destroy();
        ib::buffer().reset();
    }

    void playEndAnimationToPos(CCPoint position) {
        if (ib::sim::active()) {
            return;
        }
        PlayLayer::playEndAnimationToPos(position);
    }

    // once per frame: pick up setting changes and handle the preset hotkey
    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        static int frames = 0;
        if (++frames >= CONFIG_REFRESH_FRAMES) {
            frames = 0;
            ib::reloadConfig();
        }

        static bool wasDown = false;
        bool isDown = ib::config().hotkey && gdHasFocus() && (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        if (isDown && !wasDown) {
            ib::cyclePreset();
        }
        wasDown = isDown;
    }
};

class $modify(BufHardStreak, HardStreak) {
    void addPoint(CCPoint point) {
        if (ib::sim::active()) {
            return;
        }
        HardStreak::addPoint(point);
    }
};

class $modify(BufEffectGameObject, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        if (ib::sim::active()) {
            return;
        }
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
};

class $modify(BufGameObject, GameObject) {
    void playShineEffect() {
        if (ib::sim::active()) {
            return;
        }
        GameObject::playShineEffect();
    }
};

class $modify(BufPlayerObject, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::playSpiderDashEffect", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::incrementJumps", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::ringJump", HOOK_PRIORITY);
    }

    void update(float dt) {
        if (!ib::sim::active()) {
            auto* layer = PlayLayer::get();
            if (layer && this == layer->m_player1) {
                float warp = layer->m_gameState.m_timeWarp;
                ib::sim::setTickLength(warp > 0.f ? dt / warp : dt);
                if (ib::config().enabled) {
                    ib::buffer().onTick(layer, this);
                }
            }
        }
        PlayerObject::update(dt);
    }

    void incrementJumps() {
        if (ib::sim::active()) {
            return;
        }
        PlayerObject::incrementJumps();
    }

    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        if (ib::sim::active()) {
            return;
        }
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void ringJump(RingObject* ring, bool skipCheck) {
        if (ib::sim::active()) {
            return;
        }
        PlayerObject::ringJump(ring, skipCheck);
    }
};

class $modify(BufBaseGameLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::canBeActivatedByPlayer", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::collisionCheckObjects", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::playerTouchedRing", HOOK_PRIORITY);
    }

    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        if (ib::sim::active()) {
            return false;
        }
        return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
    }

    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int objectCount, float dt) {
        if (!ib::sim::active()) {
            GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);
            return;
        }
        static gd::vector<GameObject*> relevant;
        ib::sim::collectRelevant(player, objects, objectCount, relevant);
        GJBaseGameLayer::collisionCheckObjects(player, &relevant, static_cast<int>(relevant.size()), dt);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (button == 1 && !ib::sim::active() && !ib::buffer().injecting()) {
            auto* layer = PlayLayer::get();
            if (layer && ib::buffer().onButton(layer, this, down, isPlayer1)) {
                return;
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (ib::sim::active()) {
            return;
        }
        GJBaseGameLayer::playerTouchedRing(player, ring);
    }
};
