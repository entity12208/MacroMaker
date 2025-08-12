// src/main.cpp
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/utils/web.hpp>
#include <cocos2d.h>
#include <queue>
#include <vector>
#include <optional>
#include <chrono>
#include <fstream>

using namespace geode::prelude;

// Simple alias for timeouts
using Clock = std::chrono::steady_clock;

static constexpr float SIM_DT = 1.0f / 60.0f; // frame step for deterministic stepping
static constexpr int MAX_SEARCH_FRAMES = 60 * 60 * 2; // 2 minutes max
static constexpr int TIMEOUT_MS = 40 * 1000; // 40 seconds timeout

// ---------- Helper: tiny struct storing per-frame input ----------
struct FrameInput {
    bool click = false;
};

// Forward declaration for access inside $modify callbacks
class ClickMacroMaker;

// ---------- Mod class ----------
class ClickMacroMaker {
public:
    // Singleton pattern for mod instance
    static ClickMacroMaker* get() {
        static ClickMacroMaker instance;
        return &instance;
    }

    void onLoad() {
        log::info("ClickMacroMaker loaded (entity12208)");

        // Register modification on PlayLayer
        // This registers the $modify declared below (must be global scope)
    }

    // Toggle menu: open/close
    void toggleMenu(PlayLayer* pl) {
        if (!pl) return;
        if (!m_ui.m_menuOpen) {
            openMenu(pl);
        } else {
            closeMenu(pl);
        }
    }

private:
    struct UIState {
        cocos2d::CCMenuItem* m_button = nullptr;
        cocos2d::CCLayer* m_modal = nullptr;
        bool m_menuOpen = false;
        cocos2d::CCMenuItem* m_exportBtn = nullptr;
        cocos2d::CCMenuItem* m_closeBtn = nullptr;
        std::string m_lastReplayData; // recorded replay string
    } m_ui;

    void openMenu(PlayLayer* pl) {
        if (m_ui.m_menuOpen) return;
        m_ui.m_menuOpen = true;

        pl->pauseGame(true);

        auto layer = CCLayerColor::create({0,0,0,160});
        auto winSize = CCDirector::get()->getWinSize();

        auto closeLbl = CCLabelBMFont::create("X", "bigFont.fnt");
        auto closeItem = CCMenuItemLabel::create(closeLbl, this, menu_selector(ClickMacroMaker::onCloseClicked));
        closeItem->setPosition({winSize.width/2 + 120, winSize.height/2 + 80});

        auto exportLbl = CCLabelBMFont::create("Export", "bigFont.fnt");
        auto exportItem = CCMenuItemLabel::create(exportLbl, this, menu_selector(ClickMacroMaker::onExportClicked));
        exportItem->setPosition({winSize.width/2, winSize.height/2 - 40});

        auto statusLbl = CCLabelBMFont::create("Ready", "bigFont.fnt");
        statusLbl->setPosition({winSize.width/2, winSize.height/2 + 20});
        layer->addChild(statusLbl);

        auto menu = CCMenu::create(closeItem, exportItem, nullptr);
        menu->setPosition({0,0});
        layer->addChild(menu);

        pl->addChild(layer, 2000);

        m_ui.m_modal = layer;
        m_ui.m_exportBtn = exportItem;
        m_ui.m_closeBtn = closeItem;

        // Run solver asynchronously
        std::thread([this, pl, statusLbl]() {
            this->runSolverAndRecord(pl, statusLbl);
        }).detach();
    }

    void closeMenu(CCObject*) {
        auto pl = PlayLayer::get();
        if (!pl) return;

        if (m_ui.m_modal) {
            m_ui.m_modal->removeFromParent();
            m_ui.m_modal = nullptr;
        }

        m_ui.m_menuOpen = false;
        pl->pauseGame(false);
    }

    void onCloseClicked(CCObject*) {
        closeMenu(nullptr);
    }

    void onExportClicked(CCObject*) {
        if (m_ui.m_lastReplayData.empty()) {
            log::warn("ClickMacroMaker: No replay recorded yet.");
            return;
        }

        std::string lvlName = "macro";
        if (auto pl = PlayLayer::get()) {
            if (auto lvl = pl->m_level) {
                lvlName = lvl->m_levelName;
            }
        }
        for (auto& c : lvlName) if (!isalnum(c)) c = '_';

        auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::string fname = fmt::format("{}_{}.gdr", lvlName, (int)t);

        std::ofstream out(fname, std::ios::binary);
        out.write(m_ui.m_lastReplayData.data(), m_ui.m_lastReplayData.size());
        out.close();

        log::info("ClickMacroMaker: exported replay to {}", fname);
    }

    void runSolverAndRecord(PlayLayer* pl, CCLabelBMFont* statusLabel) {
        if (!pl) return;

        auto updateStatus = [&](const std::string& s) {
            if (statusLabel) statusLabel->setString(s.c_str());
            log::info("ClickMacroMaker: {}", s);
        };

        updateStatus("Preparing snapshot...");

        pl->takeStateSnapshot();

        pl->startRecording();

        updateStatus("Searching...");

        auto startTime = Clock::now();
        std::vector<FrameInput> bestSequence;
        bool found = false;

        std::vector<FrameInput> current;
        current.reserve(10000);

        std::function<bool(int)> dfs = [&](int frame) -> bool {
            if (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - startTime).count() > TIMEOUT_MS) return false;
            if (frame >= MAX_SEARCH_FRAMES) return false;

            if (pl->getCurrentPercentInt() >= 100) {
                bestSequence = current;
                return true;
            }

            if (pl->m_player1 && pl->m_player1->playerIsFalling(0.0f) && pl->m_player1->playerDestroyed) return false;

            // no click
            pl->update(SIM_DT);
            current.push_back({false});
            if (dfs(frame + 1)) return true;
            pl->restoreStateSnapshot();
            current.pop_back();

            // click
            if (pl->m_player1) pl->m_player1->pushButton(PlayerButton::Jump);
            pl->update(SIM_DT);
            current.push_back({true});
            if (dfs(frame + 1)) return true;
            pl->restoreStateSnapshot();
            current.pop_back();

            return false;
        };

        bool ok = dfs(0);
        if (ok) {
            updateStatus("Found run! Finalizing...");
            found = true;
        } else {
            updateStatus("No run found (timeout or no solution).");
        }

        pl->stopRecording();

        // Use documented getter for replay string, if available
        std::string replayString;
        try {
            replayString = pl->getRecordedReplay(); // Use official getter, instead of accessing internal field
        } catch (...) {
            replayString = "";
        }

        m_ui.m_lastReplayData = replayString;

        if (!replayString.empty()) {
            updateStatus("Recording ready to export.");
        } else if (found) {
            updateStatus("Run found but replay retrieval failed; try built-in recording APIs.");
        }

        pl->restoreStateSnapshot();
    }
};

// ---------------------
// PlayLayer modification: global scope!
// ---------------------
class $modify(PlayLayer) {
    void onEnter() {
        PlayLayer::onEnter();

        if (!m_fields->m_clickMacroButton) {
            auto winSize = CCDirector::get()->getWinSize();

            auto normal = CCSprite::create("icon_M.png");
            auto selected = CCSprite::create("icon_M.png");
            auto menuItem = CCMenuItemSpriteExtra::create(
                normal,
                selected,
                this,
                menu_selector($modify(PlayLayer)::onMacroButton)
            );
            menuItem->setScale(0.5f);
            menuItem->setPosition({winSize.width - 48, winSize.height - 48});

            auto menu = CCMenu::create(menuItem, nullptr);
            menu->setPosition({0, 0});
            this->addChild(menu, 1000);

            m_fields->m_clickMacroButton = menuItem;
        }
    }

    void onMacroButton(CCObject*) {
        auto pl = PlayLayer::get();
        if (!pl) return;
        ClickMacroMaker::get()->toggleMenu(pl);
    }

    struct Fields {
        CCMenuItem* m_clickMacroButton = nullptr;
    };
};

// Register mod entry point, without inheriting from Mod (Mod is final)
CREATE_GEODE_DLL_ENTRY_POINT(ClickMacroMaker)
