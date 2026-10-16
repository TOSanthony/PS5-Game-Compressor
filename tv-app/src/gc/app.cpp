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

bool ends_with_ci(const std::string &text, const char *suffix)
{
    const std::string tail(suffix);
    if (text.size() < tail.size())
        return false;
    for (std::size_t i = 0; i < tail.size(); ++i)
    {
        char c = text[text.size() - tail.size() + i];
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        if (c != tail[i])
            return false;
    }
    return true;
}

// The container's real type, from its file name: Dump (game folder), exFAT,
// FFPFSC, FFPKG, PKG...
std::string kind_text(const Game &game)
{
    const std::string &path = game.source_path;
    if (game.source_kind == "folder")
        return "Dump";
    if (ends_with_ci(path, ".ffpfsc"))
        return "FFPFSC";
    if (ends_with_ci(path, ".exfat"))
        return "exFAT";
    if (ends_with_ci(path, ".ffpkg"))
        return "FFPKG";
    if (ends_with_ci(path, ".pkg"))
        return "PKG";
    if (ends_with_ci(path, ".ffpfs"))
        return "FFPFS";
    if (game.source_kind == "compressed")
        return "FFPFSC";
    if (game.source_kind == "image")
        return "Image";
    return "Unavailable";
}

} // namespace

std::vector<App::Item> App::main_menu(const Game &game) const
{
    std::vector<Item> items;
    const auto add = [&](const char *label, const char *path, bool danger = false,
                         Item::Next next = Item::Next::run, std::string extra = {})
    { items.push_back({label, path, std::move(extra), danger, next}); };
    if (game.source_kind == "folder")
    {
        add("Compress...", "/api/gc/compress", false, Item::Next::form_compress);
        add("Make image...", "/api/gc/make-image", false, Item::Next::form_make_image);
    }
    else if (game.source_kind == "compressed")
    {
        add("Validate and repair", "/api/gc/validate-repair");
        add("Validate only", "/api/gc/validate-only");
        add("Uncompress...", "/api/gc/uncompress", false, Item::Next::form_uncompress);
    }
    else if (game.source_kind == "image")
    {
        add("Compress...", "/api/gc/compress", false, Item::Next::form_compress);
        add("Set read-only", "/api/gc/set-read-only");
        if (game.is_mounted)
            add("Extract to folder", "/api/gc/extract-image");
    }
    else
        return items;
    add("Read speed test", "/api/gc/read-speed-test");
    if (game.ampr_present)
    {
        if (game.ampr_update_needed && !game.ampr_latest_version.empty() &&
            game.ampr_latest_sha256.size() == 64)
        {
            items.push_back({"Update AMPR to " + game.ampr_latest_version, "/api/gc/update-ampr",
                             "&version=" + game.ampr_latest_version +
                                 "&sha256=" + game.ampr_latest_sha256 + "&intent=manual",
                             false, Item::Next::run});
        }
        add("Choose AMPR version...", "", false, Item::Next::ampr);
        if (game.ampr_original_available)
            add("Restore original AMPR", "/api/gc/restore-ampr-original");
        add("Build AMPR index", "/api/gc/build-ampr-index");
    }
    if (game.can_move_to_usb || game.can_move_to_internal)
    {
        add("Copy to...", "", false, Item::Next::copy);
        add("Move to...", "", false, Item::Next::move);
    }
    add("Delete game data", "", true, Item::Next::confirm_delete);
    return items;
}

std::vector<App::Item> App::ampr_menu(const Game &game) const
{
    std::vector<Item> items;
    for (const AmprVersion &entry : state_.ampr_versions)
    {
        std::string label = entry.version;
        if (entry.version == game.ampr_version)
            label += "  (installed)";
        else if (entry.version == game.ampr_latest_version)
            label += "  (latest)";
        items.push_back({label, "/api/gc/update-ampr",
                         "&version=" + entry.version + "&sha256=" + entry.sha256 +
                             "&intent=manual",
                         false, Item::Next::run});
    }
    if (items.empty())
        items.push_back({"No AMPR build cached yet (open the web interface once)", "", {}, false,
                         Item::Next::close});
    items.push_back({"Back", "", {}, false, Item::Next::close});
    return items;
}

App::Form App::build_form(const Game &game, Item::Next kind) const
{
    Form form;
    form.kind = kind;
    form.title = game.name;
    if (kind == Item::Next::form_uncompress)
    {
        form.path = "/api/gc/uncompress";
        form.label = "Uncompress";
        form.rows.push_back({"Output", {"App folder", "Image"}, {"app", "image"}, 0});
        FormRow where{"Destination", {"In place"}, {"inplace"}, 0};
        if (game.can_move_to_internal)
        {
            where.names.push_back("Internal storage");
            where.values.push_back("internal");
        }
        if (game.can_move_to_usb)
            for (const Target &t : state_.targets)
            {
                where.names.push_back(t.name.empty() ? t.id : t.name);
                where.values.push_back("usb:" + t.id);
            }
        form.rows.push_back(where);
        form.rows.push_back(
            {"Compressed file", {"Delete after", "Keep"}, {"after", "keep"}, 0});
        return form;
    }
    const bool make = kind == Item::Next::form_make_image;
    form.path = make ? "/api/gc/make-image" : "/api/gc/compress";
    form.label = make ? "Make image" : "Compress";
    form.rows.push_back(make ? FormRow{"Format", {"exFAT", "FFPFSC"}, {"exfat", "pfs"}, 0}
                             : FormRow{"Format", {"FFPFSC", "exFAT"}, {"pfs", "exfat"}, 0});
    FormRow source{"Original data", {"Delete after", "Keep"}, {"after", "keep"}, 0};
    if (game.can_stream_delete && !make)
    {
        source.names.push_back("Delete while working (risky)");
        source.values.push_back("stream");
    }
    form.rows.push_back(source);
    FormRow where{"Result goes to", {"Same place"}, {"keep"}, 0};
    if (game.can_move_to_internal)
    {
        where.names.push_back("Internal storage");
        where.values.push_back("internal");
    }
    if (game.can_move_to_usb)
        for (const Target &t : state_.targets)
        {
            where.names.push_back(t.name.empty() ? t.id : t.name);
            where.values.push_back("usb:" + t.id);
        }
    form.rows.push_back(where);
    return form;
}

void App::submit_form(const Game &game)
{
    std::string extra;
    const auto value = [&](std::size_t row)
    {
        const FormRow &r = form_.rows[row];
        return r.values[static_cast<std::size_t>(r.index)];
    };
    if (form_.kind == Item::Next::form_uncompress)
    {
        const std::string where = value(1);
        extra += "&mode=" + value(0) + "&deletePolicy=" + value(2);
        if (where.rfind("usb:", 0) == 0)
            extra += "&destination=usb&usbId=" + where.substr(4);
        else
            extra += "&destination=" + where;
    }
    else
    {
        std::string policy = value(1);
        const std::string where = value(2);
        const bool elsewhere = where != "keep";
        if (policy == "stream" && elsewhere)
            policy = "after";
        extra += "&format=" + value(0) + "&deletePolicy=" + policy;
        if (policy == "stream")
            extra += "&confirmStream=1";
        if (where.rfind("usb:", 0) == 0)
            extra += "&destination=usb&usbId=" + where.substr(4);
        else if (elsewhere)
            extra += "&destination=" + where;
    }
    if (state_.job.busy)
    {
        notice_ = "Another job is running. Wait or cancel it with Square.";
        notice_left_ = 5.0f;
        return;
    }
    context_.store.act(form_.path, form_.label, game, extra);
}

std::vector<App::Item> App::target_menu(const Game &game, bool copy) const
{
    std::vector<Item> items;
    if (game.can_move_to_internal)
        items.push_back({"Internal storage",
                         copy ? "/api/gc/copy-to-internal" : "/api/gc/move-to-internal",
                         {}, false, Item::Next::run});
    if (game.can_move_to_usb)
    {
        for (const Target &target : state_.targets)
            items.push_back({(target.name.empty() ? target.id : target.name) + "  (" +
                                 size_text(target.free_bytes) + " free)",
                             copy ? "/api/gc/copy-to-usb" : "/api/gc/move-to-usb",
                             "&usbId=" + target.id, false, Item::Next::run});
    }
    items.push_back({"Back", "", {}, false, Item::Next::close});
    return items;
}

std::vector<App::SettingsRow> App::settings_rows() const
{
    using Kind = SettingsRow::Kind;
    const UpdateInfo &u = state_.update;
    std::vector<SettingsRow> rows;
    SettingsRow update{Kind::update, "Check for updates", ""};
    char text[96];
    if (!u.known)
        update.value = "Payload not reachable";
    else if (u.phase == "unsupported")
    {
        update.label = "Payload outdated";
        update.value = "restarting it from this app's copy...";
    }
    else if (u.phase == "checking")
    {
        update.label = "Checking for updates...";
    }
    else if (u.phase == "available")
    {
        update.label = "Install update";
        std::snprintf(text, sizeof(text), "build %lld available (installed: %lld)",
                      static_cast<long long>(u.latest_build),
                      static_cast<long long>(u.current_build));
        update.value = text;
    }
    else if (u.phase == "downloading" || u.phase == "verifying" || u.phase == "installing")
    {
        update.label = u.phase == "downloading" ? "Downloading " + u.file
                       : u.phase == "verifying" ? "Verifying..."
                                                : "Installing...";
        if (u.total > 0)
        {
            std::snprintf(text, sizeof(text), "%d%%", static_cast<int>(100 * u.received / u.total));
            update.value = text;
        }
    }
    else if (u.phase == "installed")
    {
        update.label = "Update installed";
        update.value = u.app_status == "folder-copy" ? "app folder copy: update by hand"
                                                     : "reopen the app";
    }
    else if (u.phase == "uptodate")
    {
        std::snprintf(text, sizeof(text), "Up to date (build %lld)",
                      static_cast<long long>(u.current_build));
        update.value = text;
    }
    else if (u.phase == "error")
        update.value = u.error;
    else
    {
        std::snprintf(text, sizeof(text), "build %lld", static_cast<long long>(u.current_build));
        update.value = text;
    }
    rows.push_back(update);
    rows.push_back({Kind::auto_update, "Automatic updates", state_.auto_update ? "On" : "Off"});
    rows.push_back({Kind::restart, "Restart the Game Compressor payload", ""});
    rows.push_back({Kind::refresh, "Refresh the game list", ""});
    rows.push_back({Kind::browser, "Open the web interface", "system browser"});
    return rows;
}

void App::update_settings(const hui::InputFrame &input)
{
    using hui::Action;
    using hui::Direction;
    using Kind = SettingsRow::Kind;
    const std::vector<SettingsRow> rows = settings_rows();
    const int n = static_cast<int>(rows.size());
    settings_index_ = std::clamp(settings_index_, 0, n - 1);
    if (input.is_pressed(Action::back) || input.is_pressed(Action::menu))
    {
        mode_ = Mode::list;
        return;
    }
    if (input.nav == Direction::up && settings_index_ > 0)
        --settings_index_;
    if (input.nav == Direction::down && settings_index_ < n - 1)
        ++settings_index_;
    if (!input.is_pressed(Action::confirm))
        return;
    const UpdateInfo &u = state_.update;
    switch (rows[static_cast<std::size_t>(settings_index_)].kind)
    {
    case Kind::update:
        if (u.phase == "available" && u.available)
            context_.store.request(Verb::update_install, nullptr);
        else if (u.phase != "checking" && u.phase != "downloading" && u.phase != "verifying" &&
                 u.phase != "installing")
            context_.store.request(Verb::update_check, nullptr);
        break;
    case Kind::auto_update:
        context_.store.set_auto_update(!state_.auto_update);
        break;
    case Kind::restart:
        if (state_.job.busy)
        {
            notice_ = "A job is running: wait for it to finish first.";
            notice_left_ = 5.0f;
        }
        else
        {
            context_.store.request(Verb::restart_payload, nullptr);
            notice_ = "Restarting the payload...";
            notice_left_ = 4.0f;
        }
        break;
    case Kind::refresh:
        context_.store.request(Verb::refresh, nullptr);
        break;
    case Kind::browser:
        if (context_.open_browser)
            browser_failed_ = !context_.open_browser();
        break;
    }
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

    if (mode_ == Mode::settings)
    {
        update_settings(input);
        return;
    }
    if (mode_ == Mode::form)
    {
        if (count == 0 || form_.rows.empty())
        {
            mode_ = Mode::list;
            return;
        }
        const Game &game = state_.games[static_cast<std::size_t>(selected_)];
        const int rows = static_cast<int>(form_.rows.size());
        if (input.is_pressed(Action::back))
        {
            mode_ = Mode::menu;
            return;
        }
        if (input.nav == Direction::up && form_index_ > 0)
            --form_index_;
        if (input.nav == Direction::down && form_index_ < rows)
            ++form_index_;
        if (form_index_ < rows)
        {
            FormRow &row = form_.rows[static_cast<std::size_t>(form_index_)];
            const int n = static_cast<int>(row.values.size());
            if (input.nav == Direction::left)
                row.index = (row.index + n - 1) % n;
            if (input.nav == Direction::right ||
                (input.is_pressed(Action::confirm) && n > 1))
                row.index = (row.index + 1) % n;
        }
        else if (input.is_pressed(Action::confirm))
        {
            submit_form(game);
            mode_ = Mode::list;
        }
        return;
    }
    if (mode_ == Mode::menu)
    {
        const int n = static_cast<int>(menu_.size());
        if (count == 0 || n == 0)
        {
            mode_ = Mode::list;
            return;
        }
        const Game &game = state_.games[static_cast<std::size_t>(selected_)];
        if (input.is_pressed(Action::back))
        {
            if (submenu_)
            {
                menu_ = main_menu(game);
                menu_title_ = game.name;
                submenu_ = false;
                menu_index_ = 0;
            }
            else
                mode_ = Mode::list;
            return;
        }
        if (input.nav == Direction::up && menu_index_ > 0)
            --menu_index_;
        if (input.nav == Direction::down && menu_index_ < n - 1)
            ++menu_index_;
        if (input.is_pressed(Action::confirm))
        {
            const Item item = menu_[static_cast<std::size_t>(std::min(menu_index_, n - 1))];
            switch (item.next)
            {
            case Item::Next::copy:
            case Item::Next::move:
                menu_ = target_menu(game, item.next == Item::Next::copy);
                menu_title_ = item.next == Item::Next::copy ? "Copy to" : "Move to";
                submenu_ = true;
                menu_index_ = 0;
                break;
            case Item::Next::confirm_delete:
                menu_ = {{"Keep the game", "", {}, false, Item::Next::close},
                         {"Yes, delete game data", "/api/gc/delete-game-data", {}, true,
                          Item::Next::run}};
                menu_title_ = "Delete " + game.name + "?";
                submenu_ = true;
                menu_index_ = 0;
                break;
            case Item::Next::form_compress:
            case Item::Next::form_make_image:
            case Item::Next::form_uncompress:
                form_ = build_form(game, item.next);
                form_index_ = 0;
                mode_ = Mode::form;
                break;
            case Item::Next::ampr:
                menu_ = ampr_menu(game);
                menu_title_ = "AMPR version";
                submenu_ = true;
                menu_index_ = 0;
                break;
            case Item::Next::close:
                menu_ = main_menu(game);
                menu_title_ = game.name;
                submenu_ = false;
                menu_index_ = 0;
                break;
            case Item::Next::run:
                if (state_.job.busy)
                {
                    notice_ = "Another job is running. Wait or cancel it with Square.";
                    notice_left_ = 5.0f;
                }
                else
                {
                    context_.store.act(item.path, item.label, game, item.extra);
                }
                mode_ = Mode::list;
                break;
            }
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
        const Game &game = state_.games[static_cast<std::size_t>(selected_)];
        menu_ = main_menu(game);
        if (!menu_.empty())
        {
            menu_title_ = game.name;
            submenu_ = false;
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
    if (input.is_pressed(Action::menu))
    {
        mode_ = Mode::settings;
        settings_index_ = 0;
    }
    if (input.is_pressed(Action::touch))
        context_.store.request(Verb::refresh, nullptr);

    // Icon of the selected game: fetched once, uploaded to a texture here.
    if (count > 0)
    {
        const std::string &title = state_.games[static_cast<std::size_t>(selected_)].title_id;
        if (icons_.count(title) == 0)
        {
            context_.store.want_icon(title);
            IconPixels pixels;
            if (context_.make_texture && context_.store.take_icon(title, &pixels))
                icons_[title] = {context_.make_texture(pixels.width, pixels.height,
                                                       pixels.rgba.data()),
                                 pixels.width, pixels.height};
        }
    }

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

    if (state_.update.phase == "available" && state_.update.available)
        ui::text(list, type.regular, "Update available - press Options", 1920.0f - kGutter, 148.0f,
                 20.0f, color::text2, Align::right);
    else if (state_.update.phase == "installed")
        ui::text(list, type.regular, "Update installed - reopen the app", 1920.0f - kGutter, 148.0f,
                 20.0f, color::text2, Align::right);

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
            chip(list, type, row.x + 640.0f, row.cy(), kind_text(game),
                 color::field_open, color::text2, 18.0f);
            ui::text(list, type.regular, size_text(game.source_size, game.size_pending, game.size_estimated), row.x + row.w - 24.0f,
                 row.cy() + 8.0f, 24.0f, color::text2, Align::right);
        }
        list.pop_clip();

        const Game &game = state_.games[static_cast<std::size_t>(selected_)];
        const Rect panel{1160.0f, kListY, 1920.0f - kGutter - 1160.0f, kListBottom - kListY};
        surface(list, panel, kPanelRadius, color::field);
        const Rect icon_box{panel.x + 36.0f, panel.y + 34.0f, 176.0f, 176.0f};
        const auto icon = icons_.find(game.title_id);
        if (icon != icons_.end() && icon->second.texture != 0)
            image_cover(list, icon->second.texture, icon->second.width, icon->second.height,
                        icon_box, Color::rgb(0xffffff), 22.0f);
        else
            cover_placeholder(list, type, icon_box, game.name, 22.0f);
        float y = ui::paragraph(list, type.light, game.name, icon_box.x + icon_box.w + 28.0f,
                                panel.y + 84.0f, 34.0f, panel.w - 72.0f - icon_box.w - 28.0f, 42.0f,
                                color::text, 3);
        y = std::max(y, icon_box.y + icon_box.h) + 52.0f;
        const auto line = [&](const char *label, const std::string &value)
        {
            ui::text(list, type.regular, label, panel.x + 36.0f, y, 20.0f, color::text3);
            text_fit(list, type.medium, value.empty() ? "-" : value, panel.x + 36.0f + 190.0f, y,
                     22.0f, panel.w - 36.0f - 190.0f - 24.0f, color::text);
            y += 46.0f;
        };
        line("Title ID", game.title_id);
        line("Type", kind_text(game));
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
        const int n = static_cast<int>(menu_.size());
        const float height = 150.0f + static_cast<float>(n) * 70.0f;
        const Rect box{610.0f, std::max(40.0f, (1080.0f - height) * 0.5f), 700.0f, height};
        modal(over, box);
        text_fit(over, type.semibold, menu_title_, box.cx(), box.y + 70.0f, 30.0f, box.w - 60.0f,
                 color::text, Align::center);
        for (int i = 0; i < n; ++i)
        {
            const Item &item = menu_[static_cast<std::size_t>(i)];
            const Rect r{box.x + 60.0f, box.y + 110.0f + static_cast<float>(i) * 70.0f,
                         box.w - 120.0f, kButtonHeight};
            const ButtonKind kind = i == menu_index_
                                        ? (item.danger ? ButtonKind::danger : ButtonKind::primary)
                                        : ButtonKind::secondary;
            button(over, type, r, item.label, kind, i == menu_index_ ? 1.0f : 0.0f, true, 22.0f);
        }
    }

    // Options form (compress, make image, uncompress).
    if (mode_ == Mode::form && count > 0 && !form_.rows.empty())
    {
        DrawList &over = frame.overlay;
        over.rounded_rect({0.0f, 0.0f, 1920.0f, 1080.0f}, 0.0f, Color::rgb(0x000000, 0.6f));
        const int rows = static_cast<int>(form_.rows.size());
        const float height = 190.0f + static_cast<float>(rows + 1) * 78.0f;
        const Rect box{460.0f, std::max(30.0f, (1080.0f - height) * 0.5f), 1000.0f, height};
        modal(over, box);
        ui::text(over, type.semibold, form_.label, box.cx(), box.y + 62.0f, 34.0f, color::text,
                 Align::center);
        text_fit(over, type.regular, form_.title, box.cx(), box.y + 100.0f, 22.0f, box.w - 80.0f,
                 color::text3, Align::center);
        for (int i = 0; i < rows; ++i)
        {
            const FormRow &row = form_.rows[static_cast<std::size_t>(i)];
            const Rect r{box.x + 50.0f, box.y + 130.0f + static_cast<float>(i) * 78.0f,
                         box.w - 100.0f, 64.0f};
            const bool on = i == form_index_;
            if (on)
                focus_ring(over, r, 16.0f, 1.0f);
            surface(over, r, 16.0f, on ? color::field_open : color::field);
            ui::text(over, type.regular, row.label, r.x + 26.0f, r.cy() + 8.0f, 24.0f,
                     color::text2);
            const std::string &name = row.names[static_cast<std::size_t>(row.index)];
            const std::string shown =
                (on && row.names.size() > 1) ? "<  " + name + "  >" : name;
            text_fit(over, type.semibold, shown, r.x + r.w - 26.0f, r.cy() + 8.0f, 24.0f,
                     r.w * 0.62f, color::text, Align::right);
        }
        const Rect start{box.x + 300.0f, box.y + 130.0f + static_cast<float>(rows) * 78.0f + 8.0f,
                         box.w - 600.0f, kButtonHeight};
        button(over, type, start, "Start", ButtonKind::primary,
               form_index_ == rows ? 1.0f : 0.0f, true, 24.0f);
    }

    if (mode_ == Mode::settings)
        draw_settings(frame);

    using hui::ui::Button;
    std::vector<hui::ui::Hint> hint_list;
    if (mode_ == Mode::form)
    {
        hint_list.push_back({Button::cross, "Change / Start"});
        hint_list.push_back({Button::circle, "Back"});
    }
    else if (mode_ == Mode::settings)
    {
        hint_list.push_back({Button::cross, "Select"});
        hint_list.push_back({Button::circle, "Back"});
    }
    else if (mode_ == Mode::menu)
    {
        hint_list.push_back({Button::cross, "Select"});
        hint_list.push_back({Button::circle, "Back"});
    }
    else
    {
        hint_list.push_back({Button::cross, "Actions"});
        hint_list.push_back({Button::triangle, "Web interface"});
        hint_list.push_back({Button::options, "Settings"});
        hint_list.push_back({Button::touchpad, "Refresh"});
    }
    if (state_.job.busy)
        hint_list.push_back({Button::square, "Cancel job"});
    hints(frame.overlay, type, hint_list);
}

void App::draw_settings(Frame &frame)
{
    using namespace ui;
    const Type &type = context_.type;
    DrawList &over = frame.overlay;
    over.rounded_rect({0.0f, 0.0f, 1920.0f, 1080.0f}, 0.0f, Color::rgb(0x050507, 0.97f));
    ui::text(over, type.light, "Settings", kGutter, 118.0f, 64.0f, color::text);
    const std::vector<SettingsRow> rows = settings_rows();
    const float x = 360.0f;
    const float w = 1200.0f;
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        const Rect r{x, 210.0f + static_cast<float>(i) * 92.0f, w, 76.0f};
        const bool on = static_cast<int>(i) == settings_index_;
        if (on)
            focus_ring(over, r, 18.0f, 1.0f);
        surface(over, r, 18.0f, on ? color::field_open : color::field);
        text_fit(over, type.semibold, rows[i].label, r.x + 30.0f, r.cy() + 9.0f, 26.0f, 620.0f,
                 color::text);
        text_fit(over, type.regular, rows[i].value, r.x + r.w - 30.0f, r.cy() + 8.0f, 22.0f, 480.0f,
                 color::text2, Align::right);
    }
    const UpdateInfo &u = state_.update;
    if ((u.phase == "downloading") && u.total > 0)
    {
        progress_bar(over, {x, 210.0f + static_cast<float>(rows.size()) * 92.0f + 6.0f, w, 12.0f},
                     static_cast<float>(u.received) / static_cast<float>(u.total), color::action);
    }
    const float y = 210.0f + static_cast<float>(rows.size()) * 92.0f + 70.0f;
    char line[200];
    std::snprintf(line, sizeof(line), "TV app %s   -   payload %s (build %lld)",
                  context_.app_version.c_str(),
                  u.current_version.empty() ? "?" : u.current_version.c_str(),
                  static_cast<long long>(u.current_build));
    ui::text(over, type.regular, line, x, y, 20.0f, color::text3);
    if (!u.latest_version.empty())
    {
        std::snprintf(line, sizeof(line), "Latest release: %s (build %lld)", u.latest_version.c_str(),
                      static_cast<long long>(u.latest_build));
        ui::text(over, type.regular, line, x, y + 34.0f, 20.0f, color::text3);
    }
    ui::text(over, type.regular,
             "Updates come from the project's GitHub releases. The app image is replaced in place; "
             "reopen the app afterwards.",
             x, y + 76.0f, 18.0f, color::text3);
}

} // namespace gc
