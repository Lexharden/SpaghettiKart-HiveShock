#include "HiveShockOverlay.h"

#include "HiveShock.h"
#include "HiveShockEffects.h"

#include <algorithm>
#include <cstdio>

#include <imgui.h>

namespace HiveShock {
namespace {

constexpr float kNoticeSeconds = 6.0f;
constexpr float kFadeSeconds = 0.8f;
constexpr float kSlideSeconds = 0.2f;

const ImVec4 kGood(0.36f, 0.86f, 0.45f, 1.0f);
const ImVec4 kBad(1.00f, 0.38f, 0.33f, 1.0f);
const ImVec4 kUser(0.92f, 0.63f, 0.04f, 1.0f); // HiveShock orange
const ImVec4 kText(1.0f, 1.0f, 1.0f, 1.0f);

ImU32 Fade(ImVec4 color, float alpha) {
    color.w *= alpha;
    return ImGui::GetColorU32(color);
}

struct Painter {
    ImDrawList* list;
    ImFont* font;
    float size;
    float width;
    float left;
    float screenRight;
    bool rightSide;
    float y;

    // progress < 0: no bar.
    void Card(const std::string& user, const char* label, bool good, float alpha, float offset, float progress) {
        const float pad = size * 0.45f;
        const float bar = size * 0.22f;
        const float height = size * 2.1f + pad * 2.0f + (progress >= 0.0f ? bar : 0.0f);
        const float x = rightSide ? screenRight - width + offset : left - offset;
        const ImVec2 min(x, y);
        const ImVec2 max(x + width, y + height);
        const ImVec4 accent = good ? kGood : kBad;

        list->AddRectFilled(min, max, Fade(ImVec4(0.04f, 0.05f, 0.09f, 0.78f), alpha), size * 0.35f);
        list->AddRectFilled(min, ImVec2(min.x + size * 0.28f, max.y), Fade(accent, alpha), size * 0.35f,
                            ImDrawFlags_RoundCornersLeft);

        list->PushClipRect(min, max, true);
        const float textX = min.x + size * 0.28f + pad;
        list->AddText(font, size * 0.85f, ImVec2(textX, min.y + pad), Fade(kUser, alpha), user.c_str());
        list->AddText(font, size, ImVec2(textX, min.y + pad + size * 0.98f), Fade(kText, alpha), label);
        if (progress >= 0.0f) {
            const float barY = max.y - bar - pad * 0.35f;
            list->AddRectFilled(ImVec2(textX, barY), ImVec2(max.x - pad, barY + bar), Fade(ImVec4(1, 1, 1, 0.18f), alpha),
                                bar * 0.5f);
            list->AddRectFilled(ImVec2(textX, barY), ImVec2(textX + (max.x - pad - textX) * progress, barY + bar),
                                Fade(accent, alpha), bar * 0.5f);
        }
        list->PopClipRect();
        y += height + size * 0.4f;
    }
};

} // namespace

void OverlayWindow::Draw() {
    if (!CVarGetInteger("gHiveShock.Enabled", 0) || !CVarGetInteger("gHiveShock.Overlay", 1)) {
        return;
    }

    const auto notices = GetNotices();
    const auto timed = RaceRunning() ? GetTimed() : std::vector<TimedView>{};
    if (notices.empty() && timed.empty()) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float scale =
        std::clamp(viewport->Size.y / 720.0f, 0.5f, 4.0f) * CVarGetFloat("gHiveShock.OverlayScale", 1.0f);

    Painter painter{};
    painter.list = ImGui::GetForegroundDrawList();
    painter.font = ImGui::GetFont();
    painter.size = 18.0f * scale;
    painter.width = 270.0f * scale;
    painter.rightSide = CVarGetInteger("gHiveShock.OverlaySide", 0) == 1;
    painter.left = viewport->Pos.x + 14.0f * scale;
    painter.screenRight = viewport->Pos.x + viewport->Size.x - 14.0f * scale;
    painter.y = viewport->Pos.y + viewport->Size.y * 0.30f; // below the lap counter and the timer

    const bool severalPlayers = HumanCount() > 1;
    for (const auto& effect : timed) {
        std::string user = effect.user.empty() ? "anon" : effect.user;
        if (severalPlayers) {
            user = "P" + std::to_string(effect.player + 1) + "  " + user;
        }
        char label[96];
        std::snprintf(label, sizeof(label), "%s  %ds", effect.label, (effect.ticksLeft + 59) / 60);
        painter.Card(user, label, effect.good, 1.0f, 0.0f,
                     static_cast<float>(effect.ticksLeft) / static_cast<float>(std::max(effect.ticksTotal, 1)));
    }

    const double now = Now();
    for (auto it = notices.rbegin(); it != notices.rend(); ++it) { // newest first
        const float age = static_cast<float>(now - it->time);
        if (age < 0.0f || age > kNoticeSeconds) {
            continue;
        }
        const float alpha = std::clamp((kNoticeSeconds - age) / kFadeSeconds, 0.0f, 1.0f);
        const float slide = 1.0f - std::clamp(age / kSlideSeconds, 0.0f, 1.0f);
        painter.Card(it->user, it->label.c_str(), it->good, alpha, slide * painter.width, -1.0f);
    }
}

} // namespace HiveShock
