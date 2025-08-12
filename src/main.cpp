// src/main.cpp
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/utils/web.hpp>
#include <cocos2d.h>

#include <thread>
#include <vector>
#include <string>
#include <atomic>
#include <fstream>
#include <chrono>
#include <random>

using namespace geode::prelude;

// Small aliases
using Clock = std::chrono::steady_clock;
static constexpr float SIM_DT = 1.0f / 60.0f;
static constexpr int MAX_SEARCH_FRAMES = 60 * 30; // 30s limit for single attempt (tuneable)
static constexpr int MAX_TRIALS = 500;           // number of different candidate sequences to try
static constexpr int TIMEOUT_MS = 35 * 1000;     // overall solver timeout for the modal session

// ---------- frame input ----------
struct FrameInput {
    bool click = false;
};

// ---------- UI global state (per PlayLayer instance via fields below) ----------
struct ModalState {
    cocos2d::CCLayer* modalLayer = nullptr;
    cocos2d::CCMenuItem* exportBtn = nullptr;
    cocos2d::CCMenuItem* closeBtn = nullptr;
    cocos2d::CCLabelBMFont* statusLabel = nullptr;
    std::string lastReplayData; // our simple macro representation (text listing click frames)
    std::atomic<bool> running{false}; // solver running flag
} g_modalState;

// Utility: write small macro format (frame indices separated by commas)
static void exportMacroToFile(std::string const& baseName, std::string const& content) {
    std::string fname = baseName + ".macro.txt";
    std::ofstream out(fname, std::ios::binary);
    out << content;
    out.close();
    log::info("ClickMacroMaker: wrote macro to {}", fname);
}

// Solver: tries random candidate sequences by restarting the level each attempt.
// When a successful run (getCurrentPercentInt >= 100) is found, we record the clicked frames.
static void solverRun(PlayLayer* pl, cocos2d::CCLabelBMFont* statusLabel) {
    if (!pl) return;

    auto now = Clock::now();
    auto updateStatus = [&](std::string s) {
        log::info("[ClickMacroMaker] {}", s);
        if (statusLabel) statusLabel->setString(s.c_str());
    };

    updateStatus("Solver starting...");

    // check readiness
    if (!pl->isGameplayActive()) {
        // ensure gameplay is (re)started so update() has an effect
        // We do not call startRecording/take snapshots to avoid TodoReturn issues.
        pl->startGame();
    }

    // We'll try many randomized patterns: for each trial we pick a random set of frames to click
    // up to a short horizon. This is a pragmatic approach that avoids expensive full BFS and avoids
    // snapshot APIs that caused compile issues on your CI.
    std::mt19937 rng((unsigned)std::chrono::system_clock::now().time_since_epoch().count());

    const auto deadline = Clock::now() + std::chrono::milliseconds(TIMEOUT_MS);

    bool found = false;
    std::vector<int> foundClicks;

    for (int trial = 0; trial < MAX_TRIALS && Clock::now() < deadline && !found; ++trial) {
        // compute randomized sequence length and click density
        int seqLen = std::uniform_int_distribution<int>(60, std::min(MAX_SEARCH_FRAMES, 600))(rng);
        float clickProb = std::uniform_real_distribution<float>(0.01f, 0.15f)(rng); // sparse clicks generally
        std::vector<FrameInput> seq(seqLen);
        for (int i = 0; i < seqLen; ++i) {
            seq[i].click = (std::uniform_real_distribution<float>(0.0f, 1.0f)(rng) < clickProb);
        }

        // Reset the level so we run from a consistent state (avoids snapshot APIs).
        pl->resetLevelFromStart();

        // give the engine a short moment to reinitialize
        for (int k = 0; k < 2; ++k) {
            pl->update(SIM_DT);
        }

        // simulate
        bool died = false;
        for (int f = 0; f < seqLen; ++f) {
            // apply click if requested
            if (seq[f].click) {
                if (pl->m_player1) {
                    // PlayerObject::pushButton exists per docs
                    pl->m_player1->pushButton(PlayerButton::Jump);
                }
            }

            // step engine one frame
            pl->update(SIM_DT);

            // success check
            if (pl->getCurrentPercentInt() >= 100) {
                // record clicks
                found = true;
                foundClicks.clear();
                for (int i = 0; i < seqLen; ++i) {
                    if (seq[i].click) foundClicks.push_back(i);
                }
                break;
            }

            // simple fail heuristic: if gameplay not active (player dead or ended), break
            if (!pl->isGameplayActive()) {
                died = true;
                break;
            }
        }

        if (trial % 10 == 0) {
            updateStatus(fmt::format("Trying... trial {}/{} (seq {})", trial, MAX_TRIALS, seqLen));
        }
    }

    if (found) {
        updateStatus("Found a run! Preparing export...");
        // Convert foundClicks to simple CSV format
        std::string outStr;
        for (size_t i = 0; i < foundClicks.size(); ++i) {
            if (i) outStr += ",";
            outStr += fmt::format("{}", foundClicks[i]);
        }
        g_modalState.lastReplayData = outStr;
        updateStatus("Ready to export (click Export).");
    } else {
        updateStatus("No run found (timeout / trials exhausted).");
    }

    // mark done
    g_modalState.running = false;
}

// ---------- PlayLayer modification ----------
class $modify(PlayLayer) {
    struct Fields {
        CCMenuItem* m_clickMacroButton = nullptr;
        bool m_macroModalOpen = false;
    };

    // Called when the PlayLayer enters the scene
    void onEnter() {
        // call original
        PlayLayer::onEnter();

        // Only create our button once per PlayLayer
        if (!m_fields->m_clickMacroButton) {
            auto win = CCDirector::get()->getWinSize();

            // create a small sprite for our button; expected resource should be added to mod's resources
            auto normal = CCSprite::create("icon_M.png");
            auto selected = CCSprite::create("icon_M.png");
            if (!normal) {
                // fallback: a label if sprite missing (keeps compile/runtime stable)
                auto label = CCLabelBMFont::create("M", "bigFont.fnt");
                normal = CCSprite::create();
                normal->addChild(label);
                selected = CCSprite::create();
                selected->addChild(label);
            }

            auto item = CCMenuItemSpriteExtra::create(normal, selected, this, menu_selector(PlayLayer::onMacroButton));
            item->setScale(0.55f);
            item->setPosition({win.width - 46.0f, win.height - 46.0f});

            auto menu = CCMenu::create(item, nullptr);
            menu->setPosition({0, 0});
            this->addChild(menu, 1000);

            m_fields->m_clickMacroButton = item;
        }
    }

    // our macro button handler (target is PlayLayer* because we are inside $modify(PlayLayer))
    void onMacroButton(CCObject*) {
        // toggle modal
        if (m_fields->m_macroModalOpen) {
            // remove modal
            if (g_modalState.modalLayer) {
                g_modalState.modalLayer->removeFromParent();
                g_modalState.modalLayer = nullptr;
            }
            m_fields->m_macroModalOpen = false;
            // resume gameplay
            this->pauseGame(false);
            // if solver still running keep it running in background; user can re-open modal later
            return;
        }

        // open modal
        m_fields->m_macroModalOpen = true;
        this->pauseGame(true); // pause the real game while the modal is open

        auto win = CCDirector::get()->getWinSize();
        auto layer = CCLayerColor::create({0,0,0,160});
        layer->setPosition({0, 0});

        // Status label
        auto status = CCLabelBMFont::create("Preparing...", "bigFont.fnt");
        status->setPosition({win.width/2.0f, win.height/2.0f + 20.0f});
        layer->addChild(status);

        // Close button (handled by PlayLayer method below)
        auto closeLbl = CCLabelBMFont::create("X", "bigFont.fnt");
        auto closeItem = CCMenuItemLabel::create(closeLbl, this, menu_selector(PlayLayer::onCloseClicked));
        closeItem->setPosition({win.width/2.0f + 120.0f, win.height/2.0f + 80.0f});

        // Export button
        auto exportLbl = CCLabelBMFont::create("Export", "bigFont.fnt");
        auto exportItem = CCMenuItemLabel::create(exportLbl, this, menu_selector(PlayLayer::onExportClicked));
        exportItem->setPosition({win.width/2.0f, win.height/2.0f - 40.0f});

        auto menu = CCMenu::create(closeItem, exportItem, nullptr);
        menu->setPosition({0,0});
        layer->addChild(menu);

        // keep pointers for handlers
        g_modalState.modalLayer = layer;
        g_modalState.exportBtn = exportItem;
        g_modalState.closeBtn = closeItem;
        g_modalState.statusLabel = status;

        this->addChild(layer, 2000);

        // start solver on detached thread (mark running)
        if (!g_modalState.running) {
            g_modalState.running = true;
            std::thread([pl = this, status]() {
                solverRun(pl, status);
            }).detach();
        }
    }

    void onCloseClicked(CCObject*) {
        // remove modal and resume game
        if (g_modalState.modalLayer) {
            g_modalState.modalLayer->removeFromParent();
            g_modalState.modalLayer = nullptr;
        }
        this->pauseGame(false);
        m_fields->m_macroModalOpen = false;
    }

    void onExportClicked(CCObject*) {
        if (g_modalState.lastReplayData.empty()) {
            FLAlertLayer::create(
                "ClickMacroMaker",
                "No macro recorded yet. Wait for the solver or try again.",
                "OK"
            )->show();
            return;
        }

        // determine level name (safe fallback)
        std::string lvlName = "macro";
        if (this->m_level && this->m_level->m_levelName) {
            lvlName = this->m_level->m_levelName;
        }
        for (auto &c : lvlName) if (!isalnum((unsigned char)c)) c = '_';

        // filename with timestamp
        auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        auto fname = fmt::format("{}_{}.macro.txt", lvlName, (int)t);

        // write our simple CSV click frame list to disk
        std::ofstream out(fname, std::ios::binary);
        out << g_modalState.lastReplayData;
        out.close();

        log::info("ClickMacroMaker: exported macro to {}", fname);

        FLAlertLayer::create(
            "ClickMacroMaker",
            fmt::format("Exported to {}", fname).c_str(),
            "OK"
        )->show();
    }
};

/// No explicit Mod subclass used. The $modify hooks above are enough to run the UI.
/// If you prefer an explicit entrypoint for logging / mod setup you can still add one:
class ClickMacroMakerEntry {
public:
    static void onLoad() {
        log::info("ClickMacroMaker loaded (entity12208) — using PlayLayer hooks.");
    }
};
CREATE_GEODE_DLL_ENTRY_POINT(ClickMacroMakerEntry)
