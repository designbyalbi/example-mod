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

using namespace geode::prelude;

namespace {
    constexpr int HOOK_PRIORITY = -1000000;
    constexpr float TICK = 0.25f;
    constexpr int STAGE_COUNT = 5;

    constexpr uint64_t typeBit(GameObjectType type) {
        return 1ull << static_cast<int>(type);
    }

    constexpr uint64_t COLLIDING_TYPES = typeBit(GameObjectType::Solid) | typeBit(GameObjectType::Hazard) | typeBit(GameObjectType::Slope) |
                                         typeBit(GameObjectType::CollisionObject) | typeBit(GameObjectType::AnimatedHazard);

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
    } buf;

    void resetPending() {
        buf.pending = -1;
        buf.releaseQueued = false;
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
        return player->m_isDart || player->m_isBird || player->m_isSwing;
    }

    int simulate(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, bool* diedBeforeInput) {
        *diedBeforeInput = false;
        if (!buf.ghost1) {
            return horizon;
        }
        buf.simulating = true;
        buf.ghostDied = false;

        bool second = dual && buf.ghost2 && p2;
        copyState(buf.ghost1, p1);
        if (second) {
            copyState(buf.ghost2, p2);
        }

        float dt = buf.tickLength;
        if (!(dt > 0.001f && dt < 2.0f)) {
            dt = TICK;
        }

        if (delay > 0) {
            pressGhost(buf.ghost1, !down);
            if (second) {
                pressGhost(buf.ghost2, !down);
            }
        }

        bool flying = isFlying(p1) || (dual && p2 && isFlying(p2));
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
            if (buf.ghostDied) {
                died = true;
                break;
            }
            if (second) {
                checkGhost(layer, buf.ghost2, dt);
                if (buf.ghostDied) {
                    died = true;
                    break;
                }
            }

            if (tick < delay) {
                continue;
            }
            ++survived;
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
        if (buf.pending == -2) {
            buf.pending = -1;
            buf.releaseQueued = false;
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
        int horizon = mini ? 28 : (anyWave ? 38 : 64);
        int maxDelay = anyWave ? buf.ticksWave : buf.ticksAll;

        bool diedEarly = false;
        int base = simulate(layer, p1, p2, dual, 0, down, horizon, &diedEarly);
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
        return true;
    }

    void cycleStage() {
        buf.stage = (buf.stage + 1) % STAGE_COUNT;
        buf.pending = -1;
        buf.releaseQueued = false;
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
            sendButton(layer, buf.pendingState);
            if (buf.releaseQueued) {
                buf.pending = -2;
                buf.releaseQueued = false;
            }
            else {
                buf.pending = -1;
            }
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
            sendButton(layer, !buf.pendingState);
            buf.releaseQueued = false;
            buf.pending = -1;
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
        buf.pending = -1;
        buf.releaseQueued = false;
        buf.appliedInput = false;
        buf.rapid = 0;
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
        buf.pending = -1;
        buf.releaseQueued = false;
        buf.realInput = false;
        buf.appliedInput = false;
        buf.isPlayer1 = true;
        buf.simulating = false;
        buf.injecting = false;
        buf.sinceInput = 999;
        buf.rapid = 0;
        return true;
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
        for (int i = 0; i < objectCount; ++i) {
            auto* object = objects->at(i);
            if (!object) {
                continue;
            }
            auto type = static_cast<unsigned>(object->m_objectType);
            if (type <= 47 && ((COLLIDING_TYPES >> type) & 1)) {
                relevant.push_back(object);
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
