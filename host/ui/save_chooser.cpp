// The PSP's save list, drawn over the running game (ui/save_chooser.hpp).
#include "ui/save_chooser.hpp"

#include "ui/i18n.hpp"
#include "ui/layer.hpp"
#include "ui/widgets.hpp"

#include "gpu/vulkan_renderer.hpp"

#include "imgui.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <utility>

namespace portablekit::ui {
namespace {

float font() { return Layer::get().font_size(); }
float px(float value) { return std::round(value * Layer::get().scale()); }

struct Icon {
    std::uint64_t id{};  // for ImGui; zero: none
};

// A save's ICON0.PNG as an interface image. The renderer keeps every image it
// makes until it is destroyed, so each version of a file is made once.
Icon icon_for(const std::filesystem::path &path) {
    static std::map<std::string, Icon> made;
    std::error_code ec;
    const auto written = std::filesystem::last_write_time(path, ec);
    if (ec) return {};
    const std::string key = path.string() + "|" + std::to_string(written.time_since_epoch().count());
    if (const auto found = made.find(key); found != made.end()) return found->second;
    Icon icon;
    std::ifstream in(path, std::ios::binary);
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    int width = 0, height = 0, channels = 0;
    if (!bytes.empty()) {
        if (stbi_uc *pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height,
                                                    &channels, 4)) {
            if (width > 0 && height > 0 && width <= 512 && height <= 512)
                icon.id = Layer::get().renderer().create_ui_image(static_cast<std::uint32_t>(width),
                                                                  static_cast<std::uint32_t>(height),
                                                                  reinterpret_cast<const std::uint32_t *>(pixels));
            stbi_image_free(pixels);
        }
    }
    made[key] = icon;  // a file that cannot be read is not tried again
    return icon;
}

struct Chooser {
    bool open{};
    SaveChooserRequest request;
    SaveChooserDone on_done;
    std::vector<Icon> icons;
    // Focus request.focus on the next frame.
    bool refocus{};
    // The save the question (save over it, delete it) is about.
    std::optional<std::size_t> asking;
    bool ask_focus{};
};

Chooser &chooser() {
    static Chooser value;
    return value;
}

void finish(std::optional<std::string> chosen) {
    Chooser &c = chooser();
    if (!c.open) return;
    c.open = false;
    Layer &layer = Layer::get();
    layer.set_interactive(false);
    layer.renderer().set_game_input(true);
    layer.renderer().hold_frame(false);
    SaveChooserDone done = std::move(c.on_done);
    c.on_done = nullptr;
    if (done) done(std::move(chosen));
}

const char *title_of(SaveChooserMode mode) {
    switch (mode) {
    case SaveChooserMode::Load: return "Load";
    case SaveChooserMode::Save: return "Save";
    case SaveChooserMode::Delete: return "Delete";
    }
    return "";
}

std::string shown_title(const SaveSlot &slot) {
    if (!slot.exists) return tr("New save data");
    return slot.title.empty() ? slot.name : slot.title;
}

// One save: its icon, title, the start of its detail, and when it was saved.
// True when activated.
bool slot_row(std::size_t index, const SaveSlot &slot, const Icon &icon, bool focus_now) {
    // The title, and room for the three lines a detail usually has.
    const float height = std::round(font() * 4.8f);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 max{min.x + width, min.y + height};
    if (focus_now) ImGui::SetKeyboardFocusHere();
    ImGui::PushID(static_cast<int>(index));
    const bool pressed = ImGui::InvisibleButton("##slot", {width, height}, ImGuiButtonFlags_EnableNav);
    ImGui::PopID();
    const bool focused = ImGui::IsItemFocused();
    const bool hovered = ImGui::IsItemHovered();
    if (focus_now) ImGui::SetScrollHereY(0.5f);

    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float rounding = px(6.0f);
    if (focused) {
        draw->AddRectFilled(min, max, colors::kRowFocus, rounding);
        draw->AddRectFilled(min, {min.x + px(4.0f), max.y}, colors::kAccent, rounding, ImDrawFlags_RoundCornersLeft);
    } else if (hovered) {
        draw->AddRectFilled(min, max, colors::kRowHover, rounding);
    }

    // The PSP's save icons are 144 by 80 pixels.
    const float pad = px(8.0f);
    const float icon_height = height - 2.0f * pad;
    const ImVec2 icon_min{min.x + px(14.0f), min.y + pad};
    const ImVec2 icon_max{icon_min.x + std::round(icon_height * 144.0f / 80.0f), icon_min.y + icon_height};
    if (icon.id != 0u) {
        draw->AddImage(static_cast<ImTextureID>(icon.id), icon_min, icon_max);
    } else {
        draw->AddRect(icon_min, icon_max, with_alpha(colors::kPanelEdge, 160), px(4.0f), 0, px(1.5f));
        if (!slot.exists) {
            const ImVec2 c{(icon_min.x + icon_max.x) * 0.5f, (icon_min.y + icon_max.y) * 0.5f};
            const float arm = icon_height * 0.25f;
            draw->AddLine({c.x - arm, c.y}, {c.x + arm, c.y}, colors::kAccent, px(3.0f));
            draw->AddLine({c.x, c.y - arm}, {c.x, c.y + arm}, colors::kAccent, px(3.0f));
        }
    }

    const float text_x = icon_max.x + px(16.0f);
    const float right = max.x - px(16.0f);
    const float line = min.y + pad + px(2.0f);
    const float date_width = slot.date.empty() ? 0.0f : ImGui::CalcTextSize(slot.date.c_str()).x;
    const std::string title = shown_title(slot);
    ImGui::PushClipRect({text_x, min.y}, {right - date_width - px(12.0f), max.y}, true);
    draw->AddText({text_x, line}, focused ? colors::kAccentBright : colors::kText, title.c_str());
    ImGui::PopClipRect();
    if (!slot.date.empty()) draw->AddText({right - date_width, line}, colors::kTextDim, slot.date.c_str());
    if (!slot.detail.empty()) {
        ImGui::PushClipRect({text_x, min.y}, {right, max.y - pad}, true);
        draw->AddText(nullptr, 0.0f, {text_x, line + font() * 1.2f}, colors::kTextDim, slot.detail.c_str(), nullptr,
                      right - text_x);
        ImGui::PopClipRect();
    }
    if (focused || hovered) Layer::get().set_description(slot.exists ? slot.name : "");
    return pressed;
}

} // namespace

bool open_game_save_chooser(SaveChooserRequest request, SaveChooserDone on_done) {
    Layer &layer = Layer::get();
    if (!layer.attached() || request.slots.empty()) return false;
    if (chooser().open) finish(std::nullopt);
    Chooser &c = chooser();
    c = Chooser{};
    for (const SaveSlot &slot : request.slots) c.icons.push_back(slot.icon.empty() ? Icon{} : icon_for(slot.icon));
    request.focus = std::min(request.focus, request.slots.size() - 1u);
    c.request = std::move(request);
    c.on_done = std::move(on_done);
    c.refocus = true;
    c.open = true;
    // The frame on screen stays behind the list, as the game's own screen
    // stays behind the PSP's dialog.
    layer.renderer().hold_frame(true);
    layer.renderer().set_game_input(false);
    layer.set_interactive(true);
    return true;
}

bool save_chooser_open() { return chooser().open; }

void save_chooser_frame() {
    Chooser &c = chooser();
    if (!c.open) return;
    Layer &layer = Layer::get();
    // The menu does not open over the list.
    (void)layer.take_menu_toggle();
    const ImGuiKey cancel = layer.confirm_south() ? ImGuiKey_GamepadFaceRight : ImGuiKey_GamepadFaceDown;
    const bool back = layer.take_back() || ImGui::IsKeyPressed(cancel, false);
    const SaveChooserMode mode = c.request.mode;

    begin_panel("##savedata", title_of(mode), c.request.game_title, true);
    begin_content();
    std::optional<std::size_t> chosen;
    bool yes = false;
    bool no = false;
    if (c.asking) {
        const SaveSlot &slot = c.request.slots[*c.asking];
        heading(tr(mode == SaveChooserMode::Save ? "Save over this save?" : "Delete this save?"));
        paragraph(shown_title(slot) + (slot.date.empty() ? "" : "  ·  " + slot.date), colors::kTextDim);
        ImGui::Dummy({0.0f, px(8.0f)});
        if (std::exchange(c.ask_focus, false)) focus_next_row();
        no = button_row("No");
        yes = button_row("Yes");
    } else {
        for (std::size_t i = 0; i < c.request.slots.size(); ++i)
            if (slot_row(i, c.request.slots[i], c.icons[i], c.refocus && i == c.request.focus)) chosen = i;
        c.refocus = false;
    }
    begin_footer();
    hints({{Control::Confirm, "Choose"}, {Control::Back, c.asking ? "No" : "Cancel"}});
    end_panel();

    if (c.asking) {
        if (yes) {
            finish(c.request.slots[*c.asking].name);
        } else if (no || back) {
            c.request.focus = *c.asking;
            c.asking.reset();
            c.refocus = true;
        }
        return;
    }
    if (back) {
        finish(std::nullopt);
        return;
    }
    if (chosen) {
        if (c.request.slots[*chosen].exists && mode != SaveChooserMode::Load) {
            c.asking = *chosen;
            c.ask_focus = true;
        } else {
            finish(c.request.slots[*chosen].name);
        }
    }
}

} // namespace portablekit::ui
