#include <Geode/Geode.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace {
    constexpr int HOOK_PRIORITY = -1000000;
    constexpr float TICK = 0.25f;
    constexpr int STAGE_COUNT = 5;

    // The landing buffer presses this many ticks BEFORE touchdown and keeps the
    // button down until the touchdown tick has passed, so the jump comes out on
    // the first possible tick, exactly like holding the button through a landing.
    constexpr int LANDING_LEAD = 1;

    // Prints a line every time a buffered press/release is actually delivered,
    // with whether the cube was on the ground at that moment.
    constexpr bool DEBUG_DELIVERY = true;

    constexpr uint64_t typeBit(GameObjectType type) {
        return 1ull << static_cast<int>(type);
    }

    constexpr uint64_t COLLIDING_TYPES = typeBit(GameObjectType::Solid) | typeBit(GameObjectType::Hazard) | typeBit(GameObjectType::Slope) |
                                         typeBit(GameObjectType::CollisionObject) | typeBit(GameObjectType::AnimatedHazard);

    // Which portal families the ghost sim models. Set one to false and that family
    // becomes a "stop the sim, don't delay" boundary instead. Handy for bisecting
    // a crash or a weird prediction.
    constexpr bool MODEL_GRAVITY = true;
    constexpr bool MODEL_SIZE = true;
    constexpr bool MODEL_SPEED = true;
    constexpr bool MODEL_MODE = true;

    // Portals the ghost applies to itself directly (the game never activates them,
    // so no shared object state is touched).
    constexpr uint64_t MODELED_TYPES = typeBit(GameObjectType::NormalGravityPortal) | typeBit(GameObjectType::InverseGravityPortal) |
                                       typeBit(GameObjectType::RegularSizePortal) | typeBit(GameObjectType::MiniSizePortal) |
                                       typeBit(GameObjectType::CubePortal) | typeBit(GameObjectType::ShipPortal) |
                                       typeBit(GameObjectType::BallPortal) | typeBit(GameObjectType::UfoPortal) |
                                       typeBit(GameObjectType::WavePortal) | typeBit(GameObjectType::RobotPortal) |
                                       typeBit(GameObjectType::SpiderPortal) | typeBit(GameObjectType::SwingPortal);

    // Things the sim can't model yet. If the ghost touches one, the sim stops and
    // the buffer stands down instead of guessing. Pads and orbs are next stage.
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

    struct Stage {
        bool enabled;
        int ticksAll;
        int ticksWave;
        char const* message;
    };

    constexpr Stage STAGES[STAGE_COUNT] = {
        { true, 2, 2, "[InputBuffer] F6 Cycle -> STAGE 1: 2 Ticks All | 2 Ticks Wave (0.5f @ 60fps)" },
        { true, 4, 4, "[InputBuffer] F6 Cycle -> STAGE 2: 4 Ticks All | 4 Ticks Wave (1.0f @ 60fps)" },
        { true, 8, 8, "[InputBuffer] F6 Cycle -> STAGE 3: 8 Ticks All | 8 Ticks Wave (2.0f @ 60fps)" },
        { true, 16, 8, "[InputBuffer] F6 Cycle -> STAGE 4: 16 Ticks All | 8 Ticks Wave (4.0f / 2.0f @ 60fps)" },
        { false, 0, 0, "[InputBuffer] F6 Cycle -> STAGE 5: OFF (100% Raw Input)" },
    };

    struct Buffer {
        PlayerObject* ghost1 = nullptr;
        PlayerObject* ghost2 = nullptr;
        bool simulating = false;
        bool injecting = false;
        bool ghostDied = false;
        bool realInput = false;
        bool appliedInput = false;
        bool isPlayer1 = true;
        bool enabled = true;
        int stage = 0;
        int ticksAll = 2;
        int ticksWave = 2;
        int pending = -1;
        bool pendingState = true;
        bool releaseQueued = false;
        float tickLength = TICK;
        int sinceInput = 999;
        int rapid = 0;
        bool simUnsure = false;
        int holdLeft = 0;
    } buf;

    void resetPending() {
        buf.pending = -1;
        buf.releaseQueued = false;
        buf.holdLeft = 0;
    }

    // Full wipe of all input tracking. realInput must be cleared too, otherwise
    // deliverPending() sees realInput != appliedInput after a respawn and
    // injects a press the player never made.
    void resetState() {
        resetPending();
        buf.realInput = false;
        buf.appliedInput = false;
        buf.injecting = false;
        buf.sinceInput = 999;
        buf.rapid = 0;
    }

    void dropGhosts() {
        if (buf.ghost1) {
            buf.ghost1->release();
            buf.ghost1 = nullptr;
        }
        if (buf.ghost2) {
            buf.ghost2->release();
            buf.ghost2 = nullptr;
        }
        buf.simulating = false;
        buf.injecting = false;
        buf.rapid = 0;
        resetPending();
    }

    PlayerObject* makeGhost(PlayLayer* layer) {
        auto* ghost = PlayerObject::create(1, 1, layer, layer, true);
        ghost->retain();
        ghost->setPosition(CCPoint(0.f, 105.f));
        ghost->setVisible(false);
        layer->m_objectLayer->addChild(ghost);
        return ghost;
    }

    void sendButton(PlayLayer* layer, bool down) {
        buf.injecting = true;
        buf.appliedInput = down;
        layer->handleButton(down, 1, buf.isPlayer1);
        buf.injecting = false;
    }

    void pressGhost(PlayerObject* ghost, bool down) {
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

    void advanceGhost(PlayerObject* ghost, float dt) {
        ghost->update(dt);
        ghost->updateRotation(dt);
    }

    void checkGhost(PlayLayer* layer, PlayerObject* ghost, float dt) {
        ghost->m_collisionLogTop->removeAllObjects();
        ghost->m_collisionLogBottom->removeAllObjects();
        ghost->m_collisionLogLeft->removeAllObjects();
        ghost->m_collisionLogRight->removeAllObjects();
        layer->checkCollisions(ghost, dt, false);
    }

    bool isFlying(PlayerObject* player) {
        return player->m_isShip || player->m_isDart || player->m_isBird || player->m_isSwing;
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
            case Mode::Cube:
                return !(p->m_isShip || p->m_isBall || p->m_isBird || p->m_isDart || p->m_isRobot || p->m_isSpider || p->m_isSwing);
        }
        return false;
    }

    // Switch the ghost to a gamemode with no effects. Everything else is turned
    // off first so the flags never overlap.
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

    // Each portal only applies once per ghost per simulation.
    std::vector<std::pair<PlayerObject*, GameObject*>> simTouched;

    bool firstTouch(PlayerObject* player, GameObject* object) {
        for (auto const& t : simTouched) {
            if (t.first == player && t.second == object) {
                return false;
            }
        }
        simTouched.emplace_back(player, object);
        return true;
    }

    // Called when the ghost overlaps a special object during a sim. Applies the
    // effect to the ghost only, or marks the sim as unsure if it can't be modeled.
    void touchSpecial(PlayerObject* player, GameObject* object) {
        float speed = speedOf(object->m_objectID);
        if (speed > 0.f) {
            if (!MODEL_SPEED) {
                buf.simUnsure = true;
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
                    buf.simUnsure = true;
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
                    buf.simUnsure = true;
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
                    buf.simUnsure = true;
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
                // pads, orbs, dual/solo/mirror/teleport: can't model yet
                buf.simUnsure = true;
                return;
        }
    }

    float simDt() {
        float dt = buf.tickLength;
        if (!(dt > 0.001f && dt < 2.0f)) {
            dt = TICK;
        }
        return dt;
    }

    int simulate(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, bool* diedBeforeInput) {
        *diedBeforeInput = false;
        if (!buf.ghost1) {
            return horizon;
        }
        buf.simulating = true;
        buf.ghostDied = false;
        buf.simUnsure = false;
        simTouched.clear();

        bool second = dual && buf.ghost2 && p2;
        copyState(buf.ghost1, p1);
        if (second) {
            copyState(buf.ghost2, p2);
        }

        float dt = simDt();

        if (delay > 0) {
            pressGhost(buf.ghost1, !down);
            if (second) {
                pressGhost(buf.ghost2, !down);
            }
        }

        int total = delay + horizon;
        if (total <= 0) {
            buf.simulating = false;
            return 0;
        }

        int survived = 0;
        int airTicks = 0;
        int groundTicks = 0;
        int tick = 0;
        bool died = false;
        for (; tick < total; ++tick) {
            if (tick == delay) {
                pressGhost(buf.ghost1, down);
                if (second) {
                    pressGhost(buf.ghost2, down);
                }
            }

            advanceGhost(buf.ghost1, dt);
            if (second) {
                advanceGhost(buf.ghost2, dt);
            }
            if (buf.ghostDied) {
                died = true;
                break;
            }

            checkGhost(layer, buf.ghost1, dt);
            if (buf.simUnsure) {
                // Hit something we can't model. Checked before death on purpose:
                // a death on the same tick can't be trusted either. If the input
                // hasn't been sent yet this variant can't be validated, so it
                // counts as 0; otherwise everything up to here was valid.
                buf.simulating = false;
                return tick < delay ? 0 : horizon;
            }
            if (buf.ghostDied) {
                died = true;
                break;
            }
            if (second) {
                checkGhost(layer, buf.ghost2, dt);
                if (buf.simUnsure) {
                    buf.simulating = false;
                    return tick < delay ? 0 : horizon;
                }
                if (buf.ghostDied) {
                    died = true;
                    break;
                }
            }

            if (tick < delay) {
                continue;
            }
            ++survived;

            // A portal may have changed the gamemode mid-sim.
            bool flying = isFlying(buf.ghost1) || (second && isFlying(buf.ghost2));
            if (flying) {
                continue;
            }

            bool grounded = buf.ghost1->m_isOnGround && (!second || buf.ghost2->m_isOnGround);
            if (!grounded) {
                ++airTicks;
                groundTicks = 0;
                continue;
            }
            if (airTicks < 6) {
                continue;
            }
            if (groundTicks > 2) {
                buf.simulating = false;
                return horizon;
            }
            ++groundTicks;
        }

        if (died && tick < delay) {
            *diedBeforeInput = true;
        }
        buf.simulating = false;
        return survived;
    }

    // Ticks until the cube touches the ground with no button held. Returns -1 if
    // it dies first, meets something the sim can't model, or doesn't land within
    // maxTicks.
    int ticksToLanding(PlayLayer* layer, PlayerObject* p1, int maxTicks) {
        if (!buf.ghost1) {
            return -1;
        }
        buf.simulating = true;
        buf.ghostDied = false;
        buf.simUnsure = false;
        simTouched.clear();

        copyState(buf.ghost1, p1);
        pressGhost(buf.ghost1, false);

        float dt = simDt();
        int landed = -1;
        for (int tick = 1; tick <= maxTicks; ++tick) {
            advanceGhost(buf.ghost1, dt);
            if (buf.ghostDied) {
                break;
            }
            checkGhost(layer, buf.ghost1, dt);
            if (buf.simUnsure || buf.ghostDied) {
                break;
            }
            if (buf.ghost1->m_isOnGround) {
                landed = tick;
                break;
            }
        }
        buf.simulating = false;
        return landed;
    }

    bool intercept(GJBaseGameLayer* self, bool down, bool isPlayer1) {
        auto* layer = GameManager::get()->m_playLayer;
        if (!layer) {
            return false;
        }

        bool dual = layer->m_gameState.m_isDualMode && layer->m_player2;
        auto* level = layer->m_level;
        if (level && !isPlayer1 && level->m_twoPlayerMode) {
            return false;
        }

        buf.isPlayer1 = isPlayer1;
        buf.realInput = down;
        if (!buf.enabled) {
            buf.appliedInput = down;
            return false;
        }

        auto* p1 = self->m_player1;
        auto* p2 = dual ? layer->m_player2 : nullptr;
        if (!p1 || p1->m_isDead || p1->m_isShip || (p2 && p2->m_isShip)) {
            resetPending();
            buf.appliedInput = down;
            return false;
        }

        if (buf.pending >= 0) {
            buf.releaseQueued = buf.pendingState != down;
            buf.sinceInput = 0;
            return true;
        }
        if (buf.pending == -2 && !down && buf.holdLeft > 0) {
            // Real release arrived while the landing press is still guarded.
            // Hold it back; deliverPending sends it once the guard has expired.
            buf.releaseQueued = true;
            buf.sinceInput = 0;
            return true;
        }
        if (buf.pending == -2) {
            resetPending();
            bool opposite = !buf.pendingState;
            if (buf.appliedInput != opposite) {
                sendButton(layer, opposite);
            }
        }

        bool p1Wave = p1->m_isDart;
        bool anyWave = p1Wave || (p2 && p2->m_isDart);
        bool mini = (p1Wave && p1->m_vehicleSize < 1.f) || (p2 && p2->m_isDart && p2->m_vehicleSize < 1.f);

        if (buf.sinceInput < 8) {
            int before = buf.rapid++;
            buf.sinceInput = 0;
            if (before >= 2 && anyWave) {
                buf.appliedInput = down;
                return false;
            }
        }
        else {
            buf.sinceInput = 0;
            buf.rapid = 0;
        }

        if (!p1Wave) {
            bool robot = p1->m_isRobot;
            bool wave2 = false;
            if (p2 && !robot) {
                wave2 = p2->m_isDart;
                if (!wave2) {
                    robot = p2->m_isRobot;
                }
            }
            if (!wave2 && !down && !robot) {
                buf.appliedInput = false;
                return false;
            }
        }

        char const* name = down ? "PRESS" : "RELEASE";
        buf.releaseQueued = false;
        int horizon = mini ? 28 : (anyWave ? 38 : 72);
        int maxDelay = anyWave ? buf.ticksWave : buf.ticksAll;

        bool diedEarly = false;
        int base = simulate(layer, p1, p2, dual, 0, down, horizon, &diedEarly);

        // Cube landing buffer. A press in mid-air does nothing in-game, and a tap
        // is gone before touchdown, so the jump never happens. Work out when the
        // cube lands with no input and fire the press right then, as long as that
        // is inside the buffer window and no worse than pressing now. This has to
        // run before the "immediate press is safe" shortcut below, because the
        // sim holds the button down forever and would call a lost tap safe.
        if (down && maxDelay > 0 && !dual && !anyWave && !p1->m_isOnGround && inMode(p1, Mode::Cube)) {
            int land = ticksToLanding(layer, p1, maxDelay);
            if (land > 0) {
                // Press slightly before touchdown. A button that is already down
                // jumps the moment the cube lands, so this can never be later than
                // holding through the landing. holdLeft keeps it down until the
                // touchdown tick has passed, even if the real release comes early.
                int fire = std::max(0, land - LANDING_LEAD);
                bool diedLate = false;
                int ret = simulate(layer, p1, p2, dual, fire, down, horizon, &diedLate);
                if (!diedLate && ret >= std::min(base, horizon)) {
                    std::printf("[InputBuffer] *** LANDING BUFFER %s in %d ticks (lands in %d) (Frame0: %d -> Delayed: %d) [Limit: %d] ***\n",
                        name, fire, land, base, ret, maxDelay);
                    buf.pendingState = down;
                    buf.pending = fire;
                    buf.holdLeft = land - fire;
                    return true;
                }
            }
        }

        if (base >= horizon || maxDelay <= 0) {
            buf.appliedInput = down;
            return false;
        }

        int saved = 0;
        int landed = 0;
        if (!anyWave) {
            int target = std::min(horizon, base + 12);
            for (int k = 1; k <= maxDelay; ++k) {
                int ret = simulate(layer, p1, p2, dual, k, down, horizon, &diedEarly);
                if (diedEarly) {
                    break;
                }
                if (ret >= target) {
                    saved = k;
                    landed = ret;
                    break;
                }
            }
        }
        else {
            int target = base + 10;
            int gain = 6;
            for (int k = 1; k <= maxDelay; ++k) {
                int ret = simulate(layer, p1, p2, dual, k, down, horizon, &diedEarly);
                if (diedEarly) {
                    break;
                }
                bool good = ret < horizon ? ret >= target : (ret - base >= gain || ret >= target);
                if (good) {
                    saved = k;
                    landed = ret;
                    break;
                }
                target += 2;
                gain += 2;
            }
        }

        if (saved <= 0) {
            buf.appliedInput = down;
            return false;
        }

        std::printf("[InputBuffer] *** SAVED +%d TICKS (%.2f frames @ 60fps) %s! (Frame0: %d -> Delayed: %d) [Limit: %d] ***\n",
            saved, static_cast<double>(static_cast<float>(saved) * TICK), name, base, landed, maxDelay);
        buf.pendingState = down;
        buf.pending = saved;
        buf.holdLeft = 0;
        return true;
    }

    void cycleStage() {
        buf.stage = (buf.stage + 1) % STAGE_COUNT;
        resetPending();
        auto const& stage = STAGES[buf.stage];
        buf.enabled = stage.enabled;
        buf.ticksAll = stage.ticksAll;
        buf.ticksWave = stage.ticksWave;
        std::puts(stage.message);
    }

    void deliverPending(PlayLayer* layer, PlayerObject* player) {
        if (buf.sinceInput <= 998) {
            ++buf.sinceInput;
        }

        bool dual = layer->m_gameState.m_isDualMode && layer->m_player2;
        if (player->m_isShip || (dual && layer->m_player2->m_isShip)) {
            resetPending();
            if (buf.appliedInput != buf.realInput) {
                sendButton(layer, buf.realInput);
            }
            return;
        }

        if (buf.pending == 0) {
            if (DEBUG_DELIVERY) {
                std::printf("[InputBuffer]   delivered %s (onGround=%d, hold guard=%d)\n",
                    buf.pendingState ? "PRESS" : "RELEASE", player->m_isOnGround ? 1 : 0, buf.holdLeft);
            }
            sendButton(layer, buf.pendingState);
            // -2 = press delivered, still waiting out its hold guard and/or a queued release
            buf.pending = (buf.releaseQueued || buf.holdLeft > 0) ? -2 : -1;
        }
        else if (buf.pending > 0) {
            --buf.pending;
        }
        else if (buf.pending == -1) {
            if (buf.appliedInput != buf.realInput) {
                sendButton(layer, buf.realInput);
            }
        }
        else if (buf.pending == -2) {
            if (buf.holdLeft > 0) {
                --buf.holdLeft;
            }
            else {
                if (buf.releaseQueued) {
                    sendButton(layer, !buf.pendingState);
                }
                buf.releaseQueued = false;
                buf.pending = -1;
            }
        }
    }
}

$on_mod(Loaded) {
    AllocConsole();
    FILE* out = nullptr;
    freopen_s(&out, "CONOUT$", "w", stdout);
    SetConsoleTitleA("Geode Predictive Input Buffer - Live Console");
    std::puts("============================================================");
    std::puts(" [InputBuffer] v4.7 LOADED (Zero-Lag + Up Arrow Fix)");
    std::puts(" [InputBuffer] Starting Mode: STAGE 1 (2 All / 2 Wave)");
    std::puts(" [InputBuffer] Press F6 in-level to cycle: 2/2 -> 4/4 -> 8/8 -> 16/8 -> OFF");
    std::puts("============================================================");
}

class $modify(BufPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayLayer::playEndAnimationToPos", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayLayer::flipGravity", HOOK_PRIORITY);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (buf.simulating && (player == buf.ghost1 || player == buf.ghost2)) {
            buf.ghostDied = true;
            return;
        }
        resetState();
        PlayLayer::destroyPlayer(player, object);
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }
        auto* layer = GameManager::get()->m_playLayer;
        if (!layer) {
            return true;
        }
        buf.ghost1 = makeGhost(layer);
        buf.ghost2 = makeGhost(layer);
        resetState();
        buf.isPlayer1 = true;
        buf.simulating = false;
        return true;
    }

    // Checkpoint / practice / startpos / normal respawns all go through here.
    // Run the original first so any button calls it makes are handled with the
    // old state, then wipe so nothing stale survives into the new attempt.
    void resetLevel() {
        PlayLayer::resetLevel();
        resetState();
    }

    void onQuit() {
        PlayLayer::onQuit();
        dropGhosts();
    }

    void playEndAnimationToPos(CCPoint position) {
        if (buf.simulating) {
            return;
        }
        PlayLayer::playEndAnimationToPos(position);
    }
};

class $modify(BufEditorLayer, LevelEditorLayer) {
    bool init(GJGameLevel* level, bool noUI) {
        bool ok = LevelEditorLayer::init(level, noUI);
        dropGhosts();
        return ok;
    }
};

class $modify(BufHardStreak, HardStreak) {
    void addPoint(CCPoint point) {
        if (buf.simulating) {
            return;
        }
        HardStreak::addPoint(point);
    }
};

class $modify(BufEffectGameObject, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        if (buf.simulating) {
            return;
        }
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
};

class $modify(BufGameObject, GameObject) {
    void playShineEffect() {
        if (buf.simulating) {
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
        if (!buf.simulating) {
            auto* layer = GameManager::get()->m_playLayer;
            if (layer && this == layer->m_player1) {
                if (GetAsyncKeyState(VK_F6) & 1) {
                    cycleStage();
                }
                if (buf.enabled) {
                    deliverPending(layer, this);
                }
            }
        }

        PlayerObject::update(dt);

        auto* layer = GameManager::get()->m_playLayer;
        if (layer && !buf.simulating) {
            float warp = layer->m_gameState.m_timeWarp;
            if (warp > 0.f) {
                dt /= warp;
            }
            buf.tickLength = dt;
        }
    }

    void incrementJumps() {
        if (buf.simulating) {
            return;
        }
        PlayerObject::incrementJumps();
    }

    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        if (buf.simulating) {
            return;
        }
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void ringJump(RingObject* ring, bool skipCheck) {
        if (buf.simulating) {
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
        if (buf.simulating) {
            return false;
        }
        return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
    }

    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int objectCount, float dt) {
        if (!buf.simulating) {
            GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);
            return;
        }
        static gd::vector<GameObject*> relevant;
        relevant.clear();
        if (static_cast<int>(relevant.capacity()) < objectCount) {
            relevant.reserve(objectCount);
        }
        CCRect playerRect;
        bool haveRect = false;
        for (int i = 0; i < objectCount; ++i) {
            auto* object = objects->at(i);
            if (!object) {
                continue;
            }
            auto type = static_cast<unsigned>(object->m_objectType);
            if (type < 64 && ((COLLIDING_TYPES >> type) & 1)) {
                relevant.push_back(object);
                continue;
            }

            // Portals / pads / orbs never reach the real collision code during a
            // sim. Instead, check overlap ourselves and apply the effect to the
            // ghost only (or flag the sim as unsure).
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
        GJBaseGameLayer::collisionCheckObjects(player, &relevant, static_cast<int>(relevant.size()), dt);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (!buf.simulating && button == 1 && !buf.injecting && intercept(this, down, isPlayer1)) {
            return;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (buf.simulating) {
            return;
        }
        GJBaseGameLayer::playerTouchedRing(player, ring);
    }
};
