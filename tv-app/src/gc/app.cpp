// Game Compressor TV app - The full-screen interface.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Layout on a virtual 1920 x 1080 canvas: a header, the game list on the left,
// the selected game's details on the right, a progress bar while a job runs and
// the controller hints at the bottom.

#include "gc/app.hpp"

#include <algorithm>
#include <cstdio>

namespace gc
{

namespace
{

constexpr float kRowH = 84.0f;
constexpr float kListX = ui::kGutter;
constexpr float kListY = 190.0f;
constexpr float kListW = 980.0f;
constexpr float kListBottom = 880.0f;

std::string size_text(std::int64_t bytes, bool pending = false, bool estimated = false)
{
    if (pending && bytes <= 0)
        return "Measuring...";
    if (estimated && bytes > 0)
        return "~" + size_text(bytes);
    char text[32];
    const double value = static_cast<double>(bytes);
    if (bytes >= (1ll << 30))
        std::snprintf(text, sizeof(text), "%.1f GB", value / static_cast<double>(1ll << 30));
    else if (bytes >= (1ll << 20))
        std::snprintf(text, sizeof(text), "%.0f MB", value / static_cast<double>(1ll << 20));
    else
        std::snprintf(text, sizeof(text), "%lld B", static_cast<long long>(bytes));
    return text;
}

const char *kind_text(const std::string &kind)
{
    if (kind == "compressed")
        return "Compressed";
    if (kind == "folder")
        return "Folder";
    if (kind == "image")
        return "Image";
    return "Unavailable";
}

} // namespace

int App::choices(const Game &game, Choice out[3]) const
{
    int n = 0;
    if (game.source_kind == "compressed")
    {
        out[n++] = {"Validate", Verb::validate};
        out[n++] = {"Validate and repair", Verb::repair};
    }
    else if (game.source_kind == "folder" || game.source_kind == "image")
    {
        out[n++] = {"Compress", Verb::compress};
    }
    return n;
}

void App::update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback)
{
    using hui::Action;
    using hui::Direction;
    clock_ += dt;
    state_ = context_.store.snapshot();
    if (!state_.message.empty() && state_.message != last_message_)
    {
        last_message_ = state_.message;
        notice_ = state_.message;
        notice_left_ = 6.0f;
    }
    if (notice_left_ > 0.0f)
        notice_left_ -= dt;

    const int count = static_cast<int>(state_.games.size());
    selected_ = count == 0 ? 0 : std::clamp(selected_, 0, count - 1);

    // A job can be cancelled from anywhere.
    if (state_.job.busy && input.is_pressed(Action::west))
        context_.store.request(Verb::cancel, nullptr);

    if (mode_ == Mode::menu)
    {
        Choice options[3];
        const int n = count ? choices(state_.games[static_cast<std::size_t>(selected_)], options) : 0;
        if (n == 0 || input.is_pressed(Action::back))
        {
            mode_ = Mode::list;
            return;
        }
        if (input.nav == Direction::up && menu_index_ > 0)
            --menu_index_;
        if (input.nav == Direction::down && menu_index_ < n - 1)
            ++menu_index_;
        if (input.is_pressed(Action::confirm))
        {
            if (state_.job.busy)
            {
                notice_ = "Another job is running. Wait or cancel it with Square.";
                notice_left_ = 5.0f;
            }
            else
            {
                context_.store.request(options[std::min(menu_index_, n - 1)].verb,
                                       &state_.games[static_cast<std::size_t>(selected_)]);
            }
            mode_ = Mode::list;
        }
        return;
    }

    if (input.nav == Direction::up && selected_ > 0)
        --selected_;
    if (input.nav == Direction::down && selected_ < count - 1)
        ++selected_;
    if (input.is_pressed(Action::page_prev))
        selected_ = std::max(0, selected_ - 8);
    if (input.is_pressed(Action::page_next))
        selected_ = std::max(0, std::min(count - 1, selected_ + 8));
    if (input.is_pressed(Action::confirm) && count > 0)
    {
        Choice options[3];
        if (choices(state_.games[static_cast<std::size_t>(selected_)], options) > 0)
        {
            mode_ = Mode::menu;
            menu_index_ = 0;
        }
        else
        {
            notice_ = "This entry can't be processed from here.";
            notice_left_ = 4.0f;
        }
    }
    if (input.is_pressed(Action::north) && context_.open_browser)
        browser_failed_ = !context_.open_browser();
    if (input.is_pressed(Action::menu) || input.is_pressed(Action::touch))
        context_.store.request(Verb::refresh, nullptr);

    // Folder sizes are not measured by the library scan: ask the payload to
    // measure the selected folder (again after 20 s if it is still unknown).
    measured_age_ += dt;
    if (count > 0)
    {
        const Game &game = state_.games[static_cast<std::size_t>(selected_)];
        const bool unknown = game.source_kind == "folder" && game.size_pending;
        if (unknown && (game.source_path != measured_path_ || measured_age_ > 20.0f))
        {
            measured_path_ = game.source_path;
            measured_age_ = 0.0f;
            context_.store.request(Verb::measure, &game);
        }
    }

    // Keep the selection in view.
    const float view = kListBottom - kListY;
    const float top = static_cast<float>(selected_) * kRowH;
    float target = scroll_;
    if (top < target)
        target = top;
    if (top + kRowH > target + view)
        target = top + kRowH - view;
    scroll_ += (target - scroll_) * std::min(1.0f, dt * 14.0f);
    (void)feedback;
}

void App::draw(Frame &frame)
{
    using namespace ui;
    const Type &type = context_.type;
    DrawList &list = frame.scene;

    frame.backdrop.mode = hui::gfx::BackdropMode::gradient;
    frame.backdrop.colors[0] = Color::rgb(0x10131a);
    frame.backdrop.colors[1] = Color::rgb(0x030304);
    frame.backdrop.colors[2] = Color::rgb(0x1b2a4a);
    frame.backdrop.params[0] = 0.8f;
    frame.backdrop.params[1] = 0.0f;
    frame.backdrop.params[2] = 0.35f;

    ui::text(list, type.light, "Game Compressor", kGutter, 118.0f, 64.0f, color::text);
    char sub[96];
    std::snprintf(sub, sizeof(sub), "v%s", context_.app_version.c_str());
    ui::text(list, type.regular, sub, kGutter + 560.0f, 118.0f, 24.0f, color::text3);

    const char *status = state_.link == Link::online     ? "Connected"
                         : state_.link == Link::starting ? "Starting the payload..."
                                                         : "Payload not running";
    ui::text(list, type.medium, status, 1920.0f - kGutter, 112.0f, 26.0f,
         state_.link == Link::online ? color::text2 : color::text3, Align::right);

    const int count = static_cast<int>(state_.games.size());
    if (state_.link != Link::online || !state_.games_loaded || count == 0)
    {
        const char *line1 = "Loading games...";
        const char *line2 = "";
        if (state_.link == Link::offline)
        {
            line1 = "The Game Compressor payload is not running.";
            line2 = "Make sure the jailbreak and its ELF loader (port 9021) are active, then retry.";
        }
        else if (state_.link == Link::starting)
        {
            line1 = "Starting Game Compressor...";
        }
        else if (state_.games_loaded)
        {
            line1 = "No game found.";
            line2 = "Press Options to refresh.";
        }
        ui::text(list, type.light, line1, 960.0f, 520.0f, 44.0f, color::text, Align::center);
        ui::text(list, type.regular, line2, 960.0f, 580.0f, 26.0f, color::text3, Align::center);
    }
    else
    {
        list.push_clip({0.0f, kListY - 8.0f, 1920.0f, kListBottom - kListY + 16.0f});
        for (int i = 0; i < count; ++i)
        {
            const float y = kListY + static_cast<float>(i) * kRowH - scroll_;
            if (y + kRowH < kListY - 10.0f || y > kListBottom + 10.0f)
                continue;
            const Game &game = state_.games[static_cast<std::size_t>(i)];
            const Rect row{kListX, y, kListW, kRowH - 10.0f};
            const bool on = i == selected_;
            if (on)
                focus_ring(list, row, 16.0f, 1.0f);
            surface(list, row, 16.0f, on ? color::field_open : color::field);
            text_fit(list, on ? type.semibold : type.medium, game.name, row.x + 28.0f,
                     row.y + 36.0f, 28.0f, 560.0f, color::text);
            ui::text(list, type.regular, game.title_id, row.x + 28.0f, row.y + 64.0f, 20.0f,
                 color::text3);
            chip(list, type, row.x + 640.0f, row.cy(), kind_text(game.source_kind),
                 color::field_open, color::text2, 18.0f);
            ui::text(list, type.regular, size_text(game.source_size, game.size_pending, game.size_estimated), row.x + row.w - 24.0f,
                 row.cy() + 8.0f, 24.0f, color::text2, Align::right);
        }
        list.pop_clip();

        const Game &game = state_.games[static_cast<std::size_t>(selected_)];
        const Rect panel{1160.0f, kListY, 1920.0f - kGutter - 1160.0f, kListBottom - kListY};
        surface(list, panel, kPanelRadius, color::field);
        float y = panel.y + 66.0f;
        y = ui::paragraph(list, type.light, game.name, panel.x + 36.0f, y, 38.0f, panel.w - 72.0f, 46.0f,
                      color::text, 2);
        y += 14.0f;
        const auto line = [&](const char *label, const std::string &value)
        {
            ui::text(list, type.regular, label, panel.x + 36.0f, y, 20.0f, color::text3);
            text_fit(list, type.medium, value.empty() ? "-" : value, panel.x + 36.0f + 190.0f, y,
                     22.0f, panel.w - 36.0f - 190.0f - 24.0f, color::text);
            y += 46.0f;
        };
        line("Title ID", game.title_id);
        line("Type", kind_text(game.source_kind));
        line("Storage", game.storage);
        line("Size", size_text(game.source_size, game.size_pending, game.size_estimated));
        if (game.compressed_size > 0)
            line("Compressed", size_text(game.compressed_size));
        if (game.saved_bytes > 0)
            line("Saved", size_text(game.saved_bytes));
        line("Validation", game.validation);
        text_fit(list, type.regular, game.source_path, panel.x + 36.0f, panel.y + panel.h - 30.0f,
                 17.0f, panel.w - 72.0f, color::text3);
    }

    // Progress while a job runs.
    if (state_.job.busy)
    {
        const Rect bar{kGutter, 910.0f, 1920.0f - 2.0f * kGutter, 14.0f};
        std::string label = state_.job.verb.empty() ? std::string("Working") : state_.job.verb;
        if (!state_.job.current.empty())
            label += ": " + state_.job.current;
        if (!state_.job.phase.empty())
            label += "  -  " + state_.job.phase;
        text_fit(list, type.medium, label, kGutter, 900.0f, 22.0f, 1500.0f, color::text);
        char pct[16];
        std::snprintf(pct, sizeof(pct), "%d%%", static_cast<int>(state_.job.fraction() * 100.0f));
        ui::text(list, type.semibold, pct, 1920.0f - kGutter, 900.0f, 22.0f, color::text, Align::right);
        progress_bar(list, bar, state_.job.fraction(), color::action);
    }
    else if (!state_.job.error.empty() && notice_left_ <= 0.0f)
    {
        text_fit(list, type.regular, state_.job.error, kGutter, 920.0f, 22.0f, 1500.0f, color::text3);
    }

    if (notice_left_ > 0.0f)
        text_fit(list, type.medium, notice_, 960.0f, 968.0f, 24.0f, 1700.0f, color::text2,
                 Align::center);
    if (browser_failed_)
        ui::text(list, type.regular, "Couldn't open the system browser.", 960.0f, 1000.0f, 20.0f,
             color::text3, Align::center);

    // Action menu.
    if (mode_ == Mode::menu && count > 0)
    {
        DrawList &over = frame.overlay;
        over.rounded_rect({0.0f, 0.0f, 1920.0f, 1080.0f}, 0.0f, Color::rgb(0x000000, 0.6f));
        const Rect box{660.0f, 340.0f, 600.0f, 400.0f};
        modal(over, box);
        const Game &game = state_.games[static_cast<std::size_t>(selected_)];
        text_fit(over, type.semibold, game.name, box.cx(), box.y + 70.0f, 30.0f, box.w - 60.0f,
                 color::text, Align::center);
        Choice options[3];
        const int n = choices(game, options);
        for (int i = 0; i < n; ++i)
        {
            const Rect r{box.x + 80.0f, box.y + 130.0f + static_cast<float>(i) * 80.0f,
                         box.w - 160.0f, kButtonHeight};
            button(over, type, r, options[i].label,
                   i == menu_index_ ? ButtonKind::primary : ButtonKind::secondary,
                   i == menu_index_ ? 1.0f : 0.0f, true, 22.0f);
        }
    }

    using hui::ui::Button;
    std::vector<hui::ui::Hint> hint_list;
    hint_list.push_back({Button::cross, mode_ == Mode::menu ? "Start" : "Actions"});
    if (mode_ == Mode::menu)
        hint_list.push_back({Button::circle, "Back"});
    else
    {
        hint_list.push_back({Button::triangle, "Web interface"});
        hint_list.push_back({Button::options, "Refresh"});
    }
    if (state_.job.busy)
        hint_list.push_back({Button::square, "Cancel job"});
    hints(frame.overlay, type, hint_list);
}

} // namespace gc
