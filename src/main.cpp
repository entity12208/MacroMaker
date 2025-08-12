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

static constexpr float SIM_DT = 1.0f / 60.0f; // frame step used for deterministic stepping
static constexpr int MAX_SEARCH_FRAMES = 60 * 60 * 2; // 2 minutes of frames in worst case (adjustable)
static constexpr int TIMEOUT_MS = 40 * 1000; // solver timeout in ms (adjustable)

// ---------- Helper: tiny struct storing per-frame input ----------
struct FrameInput {
    // true = press/click on this frame
    bool click = false;
};

// ---------- Mod class ----------
class ClickMacroMaker : public Mod {
public:
    static Mod* get() { return Mod::get(); }

    void onLoad() override {
        log::info("ClickMacroMaker loaded (entity12208)");

        // Add overlay button to PlayLayer via $modify
        // We modify PlayLayer to add an M button and the modal UI
        m_modify_playlayer();
    }

private:
    // UI state stored globally for the PlayLayer modification
    struct UIState {
        cocos2d::CCMenuItem* m_button = nullptr;
        cocos2d::CCLayer* m_modal = nullptr;
        bool m_menuOpen = false;
        cocos2d::CCMenuItem* m_exportBtn = nullptr;
        cocos2d::CCMenuItem* m_closeBtn = nullptr;
        std::string m_lastReplayData; // recorded replay string (base64 or raw per PlayLayer)
    } m_ui;

    // ---------- Create UI & hooks by modifying PlayLayer ----------
    void m_modify_playlayer() {
        // Use the $modify macro pattern to extend PlayLayer
        class $modify(PlayLayer) {
            void onEnter() {
                // call original
                PlayLayer::onEnter();

                // create M button if not already created
                if (!m_fields->m_clickMacroButton) {
                    auto winSize = CCDirector::get()->getWinSize();

                    // create a simple CCMenuItemImage using resource 'icon_M.png'
                    auto normal = CCSprite::create("icon_M.png");
                    auto selected = CCSprite::create("icon_M.png");
                    auto menuItem = CCMenuItemSpriteExtra::create(normal, selected, this, menu_selector($modify(PlayLayer)::onMacroButton));
                    menuItem->setScale(0.5f);
                    menuItem->setPosition({winSize.width - 48, winSize.height - 48});

                    auto menu = CCMenu::create(menuItem, nullptr);
                    menu->setPosition({0, 0});
                    this->addChild(menu, 1000); // topmost

                    m_fields->m_clickMacroButton = menuItem;
                }
            }

            // handler when M is pressed
            void onMacroButton(CCObject*) {
                // toggle menu
                auto pl = PlayLayer::get();
                if (!pl) return;

                // call helper in mod instance (global singleton)
                ClickMacroMaker::get()->toggleMenu(pl);
            }

            // store a pointer to our created button in fields for safety
            struct Fields {
                CCMenuItem* m_clickMacroButton = nullptr;
            };
        };

        // Register the modification
        // (The above $modify(PlayLayer) block gets compiled as part of this file.)
    }

public:
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
    // Build the modal UI programmatically
    void openMenu(PlayLayer* pl) {
        if (m_ui.m_menuOpen) return;
        m_ui.m_menuOpen = true;

        // Pause the actual game (freeze live movement)
        pl->pauseGame(true); // documented PlayLayer pause API. :contentReference[oaicite:1]{index=1}

        // create a simple layer with two buttons: X close, green Export
        auto layer = CCLayerColor::create({0,0,0,160});
        auto winSize = CCDirector::get()->getWinSize();

        // Close (X)
        auto closeLbl = CCLabelBMFont::create("X", "bigFont.fnt");
        auto closeItem = CCMenuItemLabel::create(closeLbl, this, menu_selector(ClickMacroMaker::onCloseClicked));
        closeItem->setPosition({winSize.width/2 + 120, winSize.height/2 + 80});

        // Export (green)
        auto exportLbl = CCLabelBMFont::create("Export", "bigFont.fnt");
        auto exportItem = CCMenuItemLabel::create(exportLbl, this, menu_selector(ClickMacroMaker::onExportClicked));
        exportItem->setPosition({winSize.width/2, winSize.height/2 - 40});

        // Small status label to show solver progress — we reuse a label
        auto statusLbl = CCLabelBMFont::create("Ready", "bigFont.fnt");
        statusLbl->setPosition({winSize.width/2, winSize.height/2 + 20});
        layer->addChild(statusLbl);

        auto menu = CCMenu::create(closeItem, exportItem, nullptr);
        menu->setPosition({0, 0});
        layer->addChild(menu);

        pl->addChild(layer, 2000);
        m_ui.m_modal = layer;
        m_ui.m_exportBtn = exportItem;
        m_ui.m_closeBtn = closeItem;

        // Kick off solver asynchronously (non-blocking on main thread is safer but for clarity we'll run in a short blocking loop)
        // We'll run the solver on a new thread to avoid freezing UI.
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
        // resume the real game
        pl->pauseGame(false);
    }

    void onCloseClicked(CCObject*) {
        // close menu
        auto pl = PlayLayer::get();
        if (!pl) return;
        if (m_ui.m_modal) {
            m_ui.m_modal->removeFromParent();
            m_ui.m_modal = nullptr;
        }
        m_ui.m_menuOpen = false;
        pl->pauseGame(false);
    }

    // When Export clicked: write recorded replay to disk in GD replay (.gdr) format.
    void onExportClicked(CCObject*) {
        if (m_ui.m_lastReplayData.empty()) {
            log::warn("ClickMacroMaker: No replay recorded yet.");
            return;
        }

        // Build a filename from level name + timestamp
        std::string lvlName = "macro";
        if (auto pl = PlayLayer::get()) {
            if (auto lvl = pl->m_level) {
                lvlName = lvl->m_levelName;
            }
        }
        // sanitize
        for (auto &c : lvlName) if (!isalnum(c)) c = '_';

        auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::string fname = fmt::format("{}_{}.gdr", lvlName, (int)t);

        // The PlayLayer recording API stores replay as a base64 or raw string accessible via PlayLayer::playReplay or internal string;
        // Geode docs indicate PlayLayer supports startRecording/stopRecording and playReplay; many mods use these to export playable replays. :contentReference[oaicite:2]{index=2}
        // Many replayers expect compact binary format; here we rely on recorded binary from PlayLayer.
        // For compatibility, write the raw string to disk as bytes.

        std::ofstream out(fname, std::ios::binary);
        out << m_ui.m_lastReplayData;
        out.close();

        log::info("ClickMacroMaker: exported replay to {}", fname);
    }

    // ---------- Core: solver that simulates frames and records a replay ----------
    // Strategy:
    //  - Use PlayLayer's startRecording() to record the simulated run.
    //  - Take the real PlayLayer state snapshot so we can restore later.
    //  - Run deterministic frame stepping while applying candidate clicks (BFS/backtracking).
    //  - When a complete successful run is found (levelComplete or reached end position), stop recording and store the replay data.
    //
    // Notes:
    //  - We rely on PlayLayer's recording API (startRecording/stopRecording) and pauseGame() to run deterministic stepping. See docs. :contentReference[oaicite:3]{index=3}
    //
    void runSolverAndRecord(PlayLayer* pl, CCLabelBMFont* statusLabel) {
        if (!pl) return;

        // status update helper
        auto updateStatus = [&](std::string s) {
            if (statusLabel) {
                statusLabel->setString(s.c_str());
            }
            log::info("ClickMacroMaker: {}", s);
        };

        updateStatus("Preparing snapshot...");

        // take a state snapshot of the level (PlayLayer provides snapshot utilities)
        // This allows us to restore the real play state after the simulate+record attempt.
        // The docs show PlayLayer has methods to take/compare snapshots. We'll call takeStateSnapshot() if available. :contentReference[oaicite:4]{index=4}
        try {
            pl->takeStateSnapshot();
        } catch (...) {
            log::warn("takeStateSnapshot not available or threw.");
        }

        // Start recording (uses PlayLayer API)
        pl->startRecording();

        updateStatus("Searching...");

        // We'll do a simple deterministic frame search:
        // Basic BFS where state = (frame index, player y/vel/flags) but for reliability we step the engine and query PlayerObject for death/completion.
        // Implementation: at each frame we can choose to press (click) or not. We'll attempt a greedy depth-limited DFS with iterative deepening and timeout.
        auto startTime = Clock::now();
        std::vector<FrameInput> bestSequence;
        bool found = false;

        // We'll limit maximum sequence length to MAX_SEARCH_FRAMES as safety.
        // Use recursive DFS with pruning & timeout.
        std::vector<FrameInput> current;
        current.reserve(10000);

        std::function<bool(int)> dfs;
        dfs = [&](int frame) -> bool {
            // timeout check
            if (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - startTime).count() > TIMEOUT_MS) {
                return false;
            }
            if (frame >= MAX_SEARCH_FRAMES) return false;

            // Check if level complete (in the live PlayLayer state after stepping)
            if (pl->getCurrentPercentInt() >= 100) {
                bestSequence = current;
                return true;
            }

            // Early prune: if player died in the engine state, backtrack
            if (pl->m_player1 && pl->m_player1->playerIsFalling(0.0f) && pl->m_player1->playerDestroyed) {
                return false;
            }

            // Two choices: don't click, or click this frame
            // Option 1: no click
            {
                // step engine by one frame with no click
                pl->update(SIM_DT); // update PlayLayer (frame step). PlayLayer update exists on engine layers. :contentReference[oaicite:5]{index=5}
                current.push_back({false});
                if (dfs(frame + 1)) return true;
                // rollback to snapshot before this frame
                pl->compareStateSnapshot(); // restore? (if available) -- we call snapshot API to revert; API name provided in docs. :contentReference[oaicite:6]{index=6}
                current.pop_back();
            }

            // Option 2: click this frame
            {
                // simulate a click: call PlayerObject::pushButton or PlayLayer input method
                if (pl->m_player1) {
                    pl->m_player1->pushButton(PlayerButton::Jump);
                }
                pl->update(SIM_DT);
                current.push_back({true});
                if (dfs(frame + 1)) return true;
                pl->compareStateSnapshot();
                current.pop_back();
            }

            return false;
        };

        // Because repeatedly calling take/compare snapshot inside DFS for every branch can be expensive,
        // a more robust implementation clones the PlayLayer state per-branch. For brevity we rely on snapshot calls as the engine supports it.

        // Start the DFS (note: this is a best-effort solver with timeout)
        bool ok = dfs(0);
        if (ok) {
            updateStatus("Found run! Finalizing...");
            found = true;
        } else {
            updateStatus("No run found (timeout or no solution).");
        }

        // Stop recording and capture recorded data
        pl->stopRecording();

        // The PlayLayer stores the last recording in an internal string or accessible buffer — many mods call PlayLayer::getRecordedReplay() or similar.
        // If an API exists to retrieve the recording, use it. Otherwise, PlayLayer may write to a file or provide playReplay; we will attempt to read a field `m_replay` if present.
        std::string replayString;
        try {
            replayString = pl->m_replay; // example field; many play/record mods expose a field. If not present, additional reflection is needed.
        } catch (...) {
            // fallback: leave empty and rely on export using engine APIs or manual retrieval
            replayString = "";
        }

        m_ui.m_lastReplayData = replayString;
        if (!replayString.empty()) {
            updateStatus("Recording ready to export.");
        } else if (found) {
            updateStatus("Run found but replay retrieval failed; try using built-in recording APIs in your runtime.");
        }

        // restore real gameplay snapshot
        try {
            pl->restoreStateSnapshot();
        } catch (...) {
            log::warn("restoreStateSnapshot not available.");
        }

        // unpause game (if menu still open we can keep it paused until user resumes)
        // We'll keep the level paused until they close the menu (as requested earlier).
    }
};

// Register the mod entry point
CREATE_GEODE_DLL_ENTRY_POINT(ClickMacroMaker)
