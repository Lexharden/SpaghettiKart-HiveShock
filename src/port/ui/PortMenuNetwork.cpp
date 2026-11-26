#include "PortMenu.h"
#include "UIWidgets.h"
#include "port/hiveshock/HiveShock.h"
#include "port/hiveshock/HiveShockEffects.h"
#include "port/net/LanSession.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameUI {

using namespace UIWidgets;

static const std::unordered_map<int32_t, const char*> hiveShockTargets = {
    { 0, "Player 1" }, { 1, "Player 2" }, { 2, "Player 3" }, { 3, "Player 4" }, { 4, "Random player" },
};

static const std::unordered_map<int32_t, const char*> hiveShockSides = {
    { 0, "Left" },
    { 1, "Right" },
};

// Generated from the action table, so a new effect shows up here by itself.
static void DrawHiveShockEffects(WidgetInfo&) {
    std::vector<const char*> categories;
    for (const auto& action : HiveShock::Actions()) {
        const bool known = std::any_of(categories.begin(), categories.end(),
                                       [&](const char* c) { return std::string(c) == action.category; });
        if (!known) {
            categories.push_back(action.category);
        }
    }

    for (const char* category : categories) {
        ImGui::SeparatorText(category);
        ImGui::PushID(category);
        const std::string cvar = std::string("gHiveShock.Cat.") + category;
        bool allowed = CVarGetInteger(cvar.c_str(), 1) != 0;
        if (ImGui::Checkbox("Allowed", &allowed)) {
            CVarSetInteger(cvar.c_str(), allowed ? 1 : 0);
            Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        }
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        bool first = true;
        for (const auto& action : HiveShock::Actions()) {
            if (std::string(action.category) != category) {
                continue;
            }
            const float width = ImGui::CalcTextSize(action.id).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            if (!first && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width < right) {
                ImGui::SameLine();
            }
            first = false;
            if (ImGui::Button(action.id)) {
                HiveShock::QueueTest(action.id);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", action.label);
            }
        }
        ImGui::PopID();
    }
}


static void SaveCVarsSoon() {
    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
}

// Input field bound to a string CVar.
static void CVarInputText(const char* label, const char* cvar, const char* fallback, float width) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%s", CVarGetString(cvar, fallback));
    ImGui::SetNextItemWidth(width);
    if (ImGui::InputText(label, buffer, sizeof(buffer))) {
        CVarSetString(cvar, buffer);
        SaveCVarsSoon();
    }
}

static void DrawLanPlayers(const Lan::Status& status) {
    for (int i = 0; i < Lan::kMaxPlayers; i++) {
        const auto& p = status.players[i];
        if (!p.present) {
            ImGui::TextDisabled("Player %d: free", i + 1);
            continue;
        }
        std::string line = "Player " + std::to_string(i + 1) + ": " + p.name;
        if (i == 0) {
            line += " (host)";
        }
        if (i == status.localSlot) {
            line += " - you";
        } else if (p.pingMs >= 0) {
            line += "  " + std::to_string(p.pingMs) + " ms";
        }
        ImGui::TextUnformatted(line.c_str());
    }
}

static void DrawLan(WidgetInfo&) {
    static std::string sError;
    const Lan::Status status = Lan::GetStatus();

    if (!Lan::IsSupported()) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "LAN play is not supported on this platform.");
        return;
    }

    auto showError = [](const std::string& err) {
        if (!err.empty()) {
            sError = err;
        }
    };

    switch (status.state) {
        case Lan::State::Idle: {
            CVarInputText("Your name", "gLan.Name", "Player", 200.0f);
            ImGui::Spacing();
            if (ImGui::Button("Host a game")) {
                sError.clear();
                showError(Lan::Host(CVarGetString("gLan.Name", "Player")));
            }
            ImGui::SameLine();
            ImGui::TextDisabled("UDP %d", Lan::kGamePort);

            ImGui::SeparatorText("Join");
            CVarInputText("##address", "gLan.Address", "192.168.1.", 200.0f);
            ImGui::SameLine();
            if (ImGui::Button("Join")) {
                sError.clear();
                showError(Lan::Join(CVarGetString("gLan.Address", ""), CVarGetString("gLan.Name", "Player")));
            }
            if (ImGui::Button("Search the LAN")) {
                Lan::SearchGames();
            }
            for (const auto& game : Lan::FoundGames()) {
                ImGui::PushID(game.address.c_str());
                const std::string label = game.name + " - " + std::to_string(game.players) + "/4 - " + game.address +
                                          (game.inGame ? " (playing)" : "");
                ImGui::BeginDisabled(game.inGame);
                if (ImGui::Button("Join##found")) {
                    sError.clear();
                    CVarSetString("gLan.Address", game.address.c_str());
                    SaveCVarsSoon();
                    showError(Lan::Join(game.address, CVarGetString("gLan.Name", "Player")));
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextUnformatted(label.c_str());
                ImGui::PopID();
            }
            break;
        }
        case Lan::State::Joining:
            ImGui::TextUnformatted(status.lastMessage.c_str());
            if (ImGui::Button("Cancel")) {
                Lan::Leave();
            }
            break;
        case Lan::State::Lobby:
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), status.isHost ? "Hosting. Waiting for players..."
                                                                             : "In the lobby. The host starts the game.");
            DrawLanPlayers(status);
            ImGui::Spacing();
            if (status.isHost) {
                ImGui::BeginDisabled(status.playerCount < 2);
                if (ImGui::Button("Start")) {
                    sError.clear();
                    showError(Lan::StartGame());
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
            }
            if (ImGui::Button("Leave")) {
                Lan::Leave();
            }
            break;
        case Lan::State::InGame:
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "Playing over LAN (frame %u)", status.frame);
            DrawLanPlayers(status);
            ImGui::TextWrapped("Player 1 picks the number of players and the mode in the main menu; everyone picks "
                               "their own character.");
            if (ImGui::Button("Leave the game")) {
                Lan::Leave();
            }
            break;
    }

    if (!sError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", sError.c_str());
    } else if (status.state == Lan::State::Idle && !status.lastMessage.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", status.lastMessage.c_str());
    }

    ImGui::SeparatorText("How it works");
    ImGui::TextWrapped(
        "Every PC runs the game and only the controllers travel over the network, so everyone must use the same "
        "package on the same OS (Windows with Windows, Linux with Linux, Mac with Mac). The game uses the host's "
        "settings and save data while it lasts; nothing is saved on the other PCs. HiveShock effects pause during a "
        "LAN game. Your controller 1 drives your player.");
    ImGui::TextDisabled("Build: %s", Lan::BuildId().c_str());
}

void PortMenu::AddNetwork() {
    AddMenuEntry("Network", "gSettings.Menu.NetworkSidebarSection");
    AddSidebarEntry("Network", "HiveShock", 2);
    WidgetPath path = { "Network", "HiveShock", SECTION_COLUMN_1 };

    AddWidget(path, "HiveShock", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Status", WIDGET_TEXT).PreFunc([](WidgetInfo& info) {
        // Ports and the token take effect once nothing is being edited: dragging a port slider must not restart the
        // listener on every step.
        if (!ImGui::IsAnyItemActive()) {
            HiveShock::ApplyIfChanged();
        }
        const auto status = HiveShock::GetStatus();
        switch (status.state) {
            case HiveShock::State::Listening:
                info.name = "Status: listening on port " + std::to_string(status.port) +
                            (status.lan ? " (LAN)" : " (this PC only)");
                info.options->color = Colors::Green;
                break;
            case HiveShock::State::Error:
                info.name = "Status: error - " + status.error;
                info.options->color = Colors::Red;
                break;
            case HiveShock::State::Unsupported:
                info.name = "Status: not supported on this platform";
                info.options->color = Colors::Orange;
                break;
            default:
                info.name = "Status: disabled";
                info.options->color = Colors::Gray;
                break;
        }
    });
    AddWidget(path, "Enable HiveShock", WIDGET_CVAR_CHECKBOX)
        .CVar("gHiveShock.Enabled")
        .Options(CheckboxOptions().Tooltip("Receive live stream effects (gifts, chat, follows, bits) from the HiveShock "
                                           "app. Turning it off forgets everything that was queued."))
        .Callback([](WidgetInfo&) { HiveShock::Apply(); });
    AddWidget(path, "Allow LAN connections", WIDGET_CVAR_CHECKBOX)
        .CVar("gHiveShock.AllowLAN")
        .Options(CheckboxOptions().Tooltip("Accept effects from a phone or another PC. Anyone on the network can send "
                                           "them unless a token is set (CVar gHiveShock.Token)."))
        .Callback([](WidgetInfo&) { HiveShock::Apply(); });
    AddWidget(path, "Affected player", WIDGET_CVAR_COMBOBOX)
        .CVar("gHiveShock.Target")
        .Options(ComboboxOptions().ComboMap(hiveShockTargets).Tooltip("Who receives the effects in multiplayer."));

    AddWidget(path, "Pacing", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Gap between effects: %d ms", WIDGET_CVAR_SLIDER_INT)
        .CVar("gHiveShock.MinGapMs")
        .Options(IntSliderOptions().Min(0).Max(5000).Step(50).DefaultValue(250).Tooltip(
            "A burst of gifts plays out one after another instead of all at once."));
    AddWidget(path, "Grace after an attack: %d ms", WIDGET_CVAR_SLIDER_INT)
        .CVar("gHiveShock.HitGapMs")
        .Options(IntSliderOptions().Min(0).Max(15000).Step(250).DefaultValue(2500).Tooltip(
            "Time to drive between two attacks, counted from when the kart recovers. Attacks wait, they are not lost."));
    AddWidget(path, "Maximum traps on track: %d", WIDGET_CVAR_SLIDER_INT)
        .CVar("gHiveShock.MaxTraps")
        .Options(IntSliderOptions().Min(5).Max(120).DefaultValue(40).Tooltip(
            "Above this number, new traps wait until some are gone."));

    AddWidget(path, "Stream overlay", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Show who sent each effect", WIDGET_CVAR_CHECKBOX)
        .CVar("gHiveShock.Overlay")
        .Options(CheckboxOptions().DefaultValue(true).Tooltip("Shows the viewer's name, the effect and its remaining time."));
    AddWidget(path, "Overlay side", WIDGET_CVAR_COMBOBOX)
        .CVar("gHiveShock.OverlaySide")
        .Options(ComboboxOptions().ComboMap(hiveShockSides));
    AddWidget(path, "Overlay scale: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gHiveShock.OverlayScale")
        .Options(FloatSliderOptions().Min(0.5f).Max(2.0f).Step(0.1f).DefaultValue(1.0f));

    AddWidget(path, "Ports", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Effects port: %d", WIDGET_CVAR_SLIDER_INT)
        .CVar("gHiveShock.Port")
        .Options(IntSliderOptions().Min(1024).Max(65535).DefaultValue(43000).Tooltip(
            "Port the HiveShock app sends effects to (default 43000). Applied when you let go of the slider."));
    AddWidget(path, "Events port: %d", WIDGET_CVAR_SLIDER_INT)
        .CVar("gHiveShock.EventPort")
        .Options(IntSliderOptions().Min(1024).Max(65535).DefaultValue(43002).Tooltip(
            "Port of the HiveShock app that receives falls and the queue (default 43002). Applied when you let go of "
            "the slider."));

    path.column = SECTION_COLUMN_2;
    AddWidget(path, "Activity", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Stats", WIDGET_TEXT).PreFunc([](WidgetInfo& info) {
        const auto status = HiveShock::GetStatus();
        info.name = "Received: " + std::to_string(status.received) + "   Executed: " + std::to_string(status.executed) +
                    "   Rejected: " + std::to_string(status.rejected) + "\nWaiting: " + std::to_string(status.pending) +
                    "   Falls: " + std::to_string(status.falls);
    });
    AddWidget(path, "Last effect", WIDGET_TEXT).PreFunc([](WidgetInfo& info) {
        const auto status = HiveShock::GetStatus();
        info.name = status.lastAction.empty() ? "Last effect: -"
                                              : "Last effect: " + status.lastAction + " <- " + status.lastUser;
    });
    AddWidget(path, "Last rejected", WIDGET_TEXT).PreFunc([](WidgetInfo& info) {
        const auto status = HiveShock::GetStatus();
        info.isHidden = status.lastRejected.empty();
        info.name = "Last rejected: " + status.lastRejected;
        info.options->color = Colors::Orange;
    });
    AddWidget(path, "Forget waiting effects", WIDGET_BUTTON)
        .Options(ButtonOptions().Size(Sizes::Inline))
        .Callback([](WidgetInfo&) { HiveShock::ClearPending(); });
    AddWidget(path, "Effects (click to test during a race)", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "HiveShock effects", WIDGET_CUSTOM).CustomFunction(DrawHiveShockEffects);

    AddSidebarEntry("Network", "LAN", 1);
    WidgetPath lanPath = { "Network", "LAN", SECTION_COLUMN_1 };
    AddWidget(lanPath, "LAN play", WIDGET_SEPARATOR_TEXT);
    AddWidget(lanPath, "LAN", WIDGET_CUSTOM).CustomFunction(DrawLan);
}

} // namespace GameUI
