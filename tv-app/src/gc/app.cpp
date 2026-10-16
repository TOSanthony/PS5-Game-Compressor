// Game Compressor TV app - The full-screen interface.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Layout on a virtual 1920 x 1080 canvas: a header, the game list on the left,
// the selected game's details on the right, a progress bar while a job runs and
// the controller hints at the bottom.

#include "gc/app.hpp"
#include "gc/i18n.hpp"

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
        return tr("Measuring...");
    if (estimated && bytes > 0)
        return "~" + size_text(bytes);
    char text[32];
    const double value = static_cast<double>(bytes);
    if (bytes >= (1ll << 30))
        std::snprintf(text, sizeof(text), i18n::french() ? "%.1f Go" : "%.1f GB", value / static_cast<double>(1ll << 30));
    else if (bytes >= (1ll << 20))
        std::snprintf(text, sizeof(text), i18n::french() ? "%.0f Mo" : "%.0f MB", value / static_cast<double>(1ll << 20));
    else
        std::snprintf(text, sizeof(text), i18n::french() ? "%lld o" : "%lld B", static_cast<long long>(bytes));
    return text;
}

// "12.3 / 51.0 MB (24%)" while a download runs; "12.3 MB" if the size is unknown.
std::string download_text(const UpdateInfo &u)
{
    char text[96];
    const double mb = 1024.0 * 1024.0;
    if (u.total > 0)
        std::snprintf(text, sizeof(text), "%.1f / %.1f MB (%d%%)", static_cast<double>(u.received) / mb,
                      static_cast<double>(u.total) / mb, static_cast<int>(100 * u.received / u.total));
    else
        std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(u.received) / mb);
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
        return tr("Image");
    return tr("Unavailable");
}

} // namespace

std::vector<App::Item> App::main_menu(const Game &game) const
{
    std::vector<Item> items;
    const auto add = [&](const std::string &label, const char *path, bool danger = false,
                         Item::Next next = Item::Next::run, std::string extra = {})
    { items.push_back({label, path, std::move(extra), danger, next}); };
    if (game.source_kind == "folder")
    {
        add(tr("Compress..."), "/api/gc/compress", false, Item::Next::form_compress);
        add(tr("Make image..."), "/api/gc/make-image", false, Item::Next::form_make_image);
    }
    else if (game.source_kind == "compressed")
    {
        add(tr("Validate and repair"), "/api/gc/validate-repair");
        add(tr("Validate only"), "/api/gc/validate-only");
        add(tr("Uncompress..."), "/api/gc/uncompress", false, Item::Next::form_uncompress);
    }
    else if (game.source_kind == "image")
    {
        add(tr("Compress..."), "/api/gc/compress", false, Item::Next::form_compress);
        add(tr("Set read-only"), "/api/gc/set-read-only");
        if (game.is_mounted)
            add(tr("Extract to folder"), "/api/gc/extract-image");
    }
    else
        return items;
    add(tr("Read speed test"), "/api/gc/read-speed-test");
    if (game.ampr_present)
    {
        if (game.ampr_update_needed && !game.ampr_latest_version.empty() &&
            game.ampr_latest_sha256.size() == 64)
        {
            items.push_back({tr("Update AMPR to {version}", {{"version", game.ampr_latest_version}}),
                             "/api/gc/update-ampr",
                             "&version=" + game.ampr_latest_version +
                                 "&sha256=" + game.ampr_latest_sha256 + "&intent=manual",
                             false, Item::Next::run});
        }
        add(tr("Choose AMPR version..."), "", false, Item::Next::ampr);
        if (game.ampr_original_available)
            add(tr("Restore original AMPR"), "/api/gc/restore-ampr-original");
        add(tr("Build AMPR index"), "/api/gc/build-ampr-index");
    }
    if (game.can_move_to_usb || game.can_move_to_internal)
    {
        add(tr("Copy to..."), "", false, Item::Next::copy);
        add(tr("Move to..."), "", false, Item::Next::move);
    }
    add(tr("Delete game data"), "", true, Item::Next::confirm_delete);
    return items;
}

std::vector<App::Item> App::ampr_menu(const Game &game) const
{
    std::vector<Item> items;
    for (const AmprVersion &entry : state_.ampr_versions)
    {
        std::string label = entry.version;
        if (entry.version == game.ampr_version)
            label = tr("{version}  (installed)", {{"version", entry.version}});
        else if (entry.version == game.ampr_latest_version)
            label = tr("{version}  (latest)", {{"version", entry.version}});
        items.push_back({label, "/api/gc/update-ampr",
                         "&version=" + entry.version + "&sha256=" + entry.sha256 +
                             "&intent=manual",
                         false, Item::Next::run});
    }
    if (items.empty())
        items.push_back({tr("No AMPR build cached yet (open the web interface once)"), "", {}, false,
                         Item::Next::close});
    items.push_back({tr("Back"), "", {}, false, Item::Next::close});
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
        form.label = tr("Uncompress");
        form.rows.push_back({tr("Output"), {tr("App folder"), tr("Image")}, {"app", "image"}, 0});
        FormRow where{tr("Destination"), {tr("In place")}, {"inplace"}, 0};
        if (game.can_move_to_internal)
        {
            where.names.push_back(tr("Internal storage"));
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
            {tr("Compressed file"), {tr("Delete after"), tr("Keep")}, {"after", "keep"}, 0});
        return form;
    }
    const bool make = kind == Item::Next::form_make_image;
    form.path = make ? "/api/gc/make-image" : "/api/gc/compress";
    form.label = make ? tr("Make image") : tr("Compress");
    form.rows.push_back(make ? FormRow{tr("Format"), {"exFAT", "FFPFSC"}, {"exfat", "pfs"}, 0}
                             : FormRow{tr("Format"), {"FFPFSC", "exFAT"}, {"pfs", "exfat"}, 0});
    FormRow source{tr("Original data"), {tr("Delete after"), tr("Keep")}, {"after", "keep"}, 0};
    if (game.can_stream_delete && !make)
    {
        source.names.push_back(tr("Delete while working (risky)"));
        source.values.push_back("stream");
    }
    form.rows.push_back(source);
    FormRow where{tr("Result goes to"), {tr("Same place")}, {"keep"}, 0};
    if (game.can_move_to_internal)
    {
        where.names.push_back(tr("Internal storage"));
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
        notice_ = tr("Another job is running. Wait or cancel it with Square.");
        notice_left_ = 5.0f;
        return;
    }
    context_.store.act(form_.path, form_.label, game, extra);
}

std::vector<App::Item> App::target_menu(const Game &game, bool copy) const
{
    std::vector<Item> items;
    if (game.can_move_to_internal)
        items.push_back({tr("Internal storage"),
                         copy ? "/api/gc/copy-to-internal" : "/api/gc/move-to-internal",
                         {}, false, Item::Next::run});
    if (game.can_move_to_usb)
    {
        for (const Target &target : state_.targets)
            items.push_back({tr("{name}  ({free} free)",
                                 {{"name", target.name.empty() ? target.id : target.name},
                                  {"free", size_text(target.free_bytes)}}),
                             copy ? "/api/gc/copy-to-usb" : "/api/gc/move-to-usb",
                             "&usbId=" + target.id, false, Item::Next::run});
    }
    items.push_back({tr("Back"), "", {}, false, Item::Next::close});
    return items;
}

std::vector<App::SettingsRow> App::settings_rows() const
{
    using Kind = SettingsRow::Kind;
    const UpdateInfo &u = state_.update;
    std::vector<SettingsRow> rows;
    SettingsRow update{Kind::update, tr("Check for updates"), ""};
    if (!u.known)
        update.value = tr("Payload not reachable");
    else if (u.phase == "unsupported")
    {
        update.label = tr("Payload outdated");
        update.value = tr("restarting it from this app's copy...");
    }
    else if (u.phase == "checking")
    {
        update.label = tr("Checking for updates...");
    }
    else if (u.phase == "available")
    {
        update.label = tr("Install update");
        update.value = tr("build {latest} available (installed: {current})",
                          {{"latest", std::to_string(u.latest_build)},
                           {"current", std::to_string(u.current_build)}});
    }
    else if (u.phase == "downloading" || u.phase == "verifying" || u.phase == "installing")
    {
        update.label = u.phase == "downloading" ? tr("Downloading {file}", {{"file", u.file}})
                       : u.phase == "verifying" ? tr("Verifying...")
                                                : tr("Installing...");
        if (u.phase == "downloading")
            update.value = download_text(u);
    }
    else if (u.phase == "installed")
    {
        update.label = tr("Update installed");
        update.value = u.app_status == "folder-copy"      ? tr("app folder copy: update by hand")
                       : u.app_status == "folder-pending" ? tr("close the app: it updates itself")
                                                          : tr("reopen the app");
    }
    else if (u.phase == "uptodate")
    {
        update.value = tr("Up to date (build {build})", {{"build", std::to_string(u.current_build)}});
    }
    else if (u.phase == "error")
        update.value = u.error;
    else
    {
        update.value = tr("build {build}", {{"build", std::to_string(u.current_build)}});
    }
    rows.push_back(update);
    rows.push_back({Kind::auto_update, tr("Automatic updates"), state_.auto_update ? tr("On") : tr("Off")});
    rows.push_back({Kind::language, tr("Language"),
                    state_.language == 2   ? "Français"
                    : state_.language == 1 ? "English"
                                           : tr("Automatic")});
    rows.push_back({Kind::theme, tr("Theme"), state_.theme});
    rows.push_back({Kind::restart, tr("Restart the Game Compressor payload"), ""});
    rows.push_back({Kind::refresh, tr("Refresh the game list"), ""});
    rows.push_back({Kind::browser, tr("Open the web interface"), tr("system browser")});
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
    if (rows[static_cast<std::size_t>(settings_index_)].kind == Kind::theme &&
        (input.nav == Direction::left || input.nav == Direction::right))
    {
        const int step = input.nav == Direction::left ? ui::skin_count() - 1 : 1;
        context_.store.set_theme(
            ui::skin_at((ui::skin_index(state_.theme) + step) % ui::skin_count()).name);
        return;
    }
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
    case Kind::language:
        context_.store.set_language((state_.language + 1) % 3);
        break;
    case Kind::theme:
        context_.store.set_theme(
            ui::skin_at((ui::skin_index(state_.theme) + 1) % ui::skin_count()).name);
        break;
    case Kind::restart:
        if (state_.job.busy)
        {
            notice_ = tr("A job is running: wait for it to finish first.");
            notice_left_ = 5.0f;
        }
        else
        {
            context_.store.request(Verb::restart_payload, nullptr);
            notice_ = tr("Restarting the payload...");
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
    i18n::set_language(static_cast<i18n::Language>(state_.language));
    ui::use_skin(ui::skin_index(state_.theme));
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
                menu_title_ = item.next == Item::Next::copy ? tr("Copy to") : tr("Move to");
                submenu_ = true;
                menu_index_ = 0;
                break;
            case Item::Next::confirm_delete:
                menu_ = {{tr("Keep the game"), "", {}, false, Item::Next::close},
                         {"Yes, delete game data", "/api/gc/delete-game-data", {}, true,
                          Item::Next::run}};
                menu_title_ = tr("Delete {name}?", {{"name", game.name}});
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
                menu_title_ = tr("AMPR version");
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
                    notice_ = tr("Another job is running. Wait or cancel it with Square.");
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
            notice_ = tr("This entry can't be processed from here.");
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

    frame.backdrop = ui::skin().backdrop;
    frame.backdrop.time = clock_;

    // Logo tile, title, version and a status pill.
    {
        const Rect logo{kGutter, 66.0f, 72.0f, 72.0f};
        list.glow(logo, 20.0f, 18.0f, ui::accent().with_alpha(0.4f));
        list.gradient_rect(logo, 20.0f, ui::accent(),
                           hui::gfx::mix(ui::accent(), Color::rgb(0x000000, ui::accent().a), 0.4f));
        icon_compress(list, logo.cx(), logo.cy(), 40.0f, ui::skin().on_accent);
    }
    const float title_x = kGutter + 100.0f;
    ui::text(list, type.light, "Game Compressor", title_x, 118.0f, 60.0f, color::text);
    char sub[96];
    std::snprintf(sub, sizeof(sub), "v%s", context_.app_version.c_str());
    ui::text(list, type.regular, sub, title_x + 540.0f, 118.0f, 24.0f, color::text3);

    const std::string status = state_.link == Link::online     ? tr("Connected")
                         : state_.link == Link::starting ? tr("Starting the payload...")
                                                         : tr("Payload not running");
    {
        const Color dot = state_.link == Link::online     ? Color::rgb(0x3ddc84)
                          : state_.link == Link::starting ? Color::rgb(0xffb020)
                                                          : Color::rgb(0xff5d5d);
        const float font_size = 24.0f;
        const float w = ui::text(list, type.medium, status, 0.0f, -1000.0f, font_size, color::text2);
        const Rect pill{1920.0f - kGutter - w - 64.0f, 76.0f, w + 64.0f, 50.0f};
        surface(list, pill, 25.0f, color::field_open);
        list.glow({pill.x + 22.0f, pill.cy() - 6.0f, 12.0f, 12.0f}, 6.0f, 8.0f, dot.with_alpha(0.55f));
        list.circle(pill.x + 28.0f, pill.cy(), 6.0f, dot);
        ui::text(list, type.medium, status, pill.x + 48.0f, pill.cy() + 8.0f, font_size, color::text2);
    }

    if (state_.update.phase == "downloading" || state_.update.phase == "verifying" ||
        state_.update.phase == "installing")
    {
        const UpdateInfo &u = state_.update;
        const std::string line = u.phase == "downloading"
                                     ? tr("Downloading {file}", {{"file", u.file}}) + "  " + download_text(u)
                                 : u.phase == "verifying" ? tr("Verifying...")
                                                          : tr("Installing...");
        ui::text(list, type.regular, line, 1920.0f - kGutter, 152.0f, 20.0f, color::text2, Align::right);
        progress_bar(list, {1920.0f - kGutter - 520.0f, 164.0f, 520.0f, 8.0f},
                     u.total > 0 ? static_cast<float>(u.received) / static_cast<float>(u.total) : 0.0f,
                     ui::accent());
    }
    else if (state_.update.phase == "available" && state_.update.available)
        ui::text(list, type.regular, tr("Update available - press Options"), 1920.0f - kGutter, 148.0f,
                 20.0f, color::text2, Align::right);
    else if (state_.update.phase == "installed")
        ui::text(list, type.regular, tr("Update installed - reopen the app"), 1920.0f - kGutter, 148.0f,
                 20.0f, color::text2, Align::right);

    const int count = static_cast<int>(state_.games.size());
    if (state_.link != Link::online || !state_.games_loaded || count == 0)
    {
        std::string line1 = tr("Loading games...");
        std::string line2;
        if (state_.link == Link::offline)
        {
            line1 = tr("The Game Compressor payload is not running.");
            line2 = tr("Make sure the jailbreak and its ELF loader (port 9021) are active, then retry.");
        }
        else if (state_.link == Link::starting)
        {
            line1 = tr("Starting Game Compressor...");
        }
        else if (state_.games_loaded)
        {
            line1 = tr("No game found.");
            line2 = tr("Press the touchpad to refresh.");
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
            if (on)
            {
                list.gradient_rect_h(row, 16.0f, ui::accent().with_alpha(0.10f), ui::accent().with_alpha(0.0f));
                list.rounded_rect({row.x + 10.0f, row.y + 16.0f, 5.0f, row.h - 32.0f}, 2.5f, ui::accent());
            }
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
        list.gradient_rect(Rect{panel.x, panel.y, panel.w, 250.0f}, kPanelRadius,
                           ui::accent().with_alpha(0.08f), ui::accent().with_alpha(0.0f));
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
        enum class Glyph { tag, cube, drive, ruler, compress, download, shield };
        const auto line = [&](Glyph glyph, const std::string &label, const std::string &value)
        {
            const float cx = panel.x + 36.0f + 20.0f;
            const float cy = y - 7.0f;
            list.circle(cx, cy, 21.0f, color::field_open);
            switch (glyph)
            {
            case Glyph::tag: icon_tag(list, cx, cy, 24.0f, color::text2); break;
            case Glyph::cube: icon_cube(list, cx, cy, 26.0f, color::text2); break;
            case Glyph::drive: icon_drive(list, cx, cy, 26.0f, color::text2); break;
            case Glyph::ruler: icon_ruler(list, cx, cy, 26.0f, color::text2); break;
            case Glyph::compress: icon_compress(list, cx, cy, 26.0f, color::text2); break;
            case Glyph::download: icon_download(list, cx, cy, 24.0f, color::text2); break;
            case Glyph::shield: icon_shield(list, cx, cy, 26.0f, color::text2); break;
            }
            const float label_x = panel.x + 36.0f + 58.0f;
            ui::text(list, type.regular, label, label_x, y, 20.0f, color::text3);
            text_fit(list, type.medium, value.empty() ? "-" : value, label_x + 178.0f, y, 22.0f,
                     panel.w - 36.0f - 58.0f - 178.0f - 24.0f, color::text);
            y += 46.0f;
        };
        line(Glyph::tag, tr("Title ID"), game.title_id);
        line(Glyph::cube, tr("Type"), kind_text(game));
        line(Glyph::drive, tr("Storage"), tr(game.storage));
        line(Glyph::ruler, tr("Size"),
             size_text(game.source_size, game.size_pending, game.size_estimated));
        if (game.compressed_size > 0)
            line(Glyph::compress, tr("Compressed"), size_text(game.compressed_size));
        if (game.saved_bytes > 0)
            line(Glyph::download, tr("Saved"), size_text(game.saved_bytes));
        line(Glyph::shield, tr("Validation"), tr(game.validation));
        text_fit(list, type.regular, game.source_path, panel.x + 36.0f, panel.y + panel.h - 30.0f,
                 17.0f, panel.w - 72.0f, color::text3);
    }

    // Progress while a job runs.
    if (state_.job.busy)
    {
        const Rect bar{kGutter, 910.0f, 1920.0f - 2.0f * kGutter, 14.0f};
        std::string label = state_.job.verb.empty() ? tr("Working") : state_.job.verb;
        if (!state_.job.current.empty())
            label += ": " + state_.job.current;
        if (!state_.job.phase.empty())
            label += "  -  " + state_.job.phase;
        text_fit(list, type.medium, label, kGutter, 900.0f, 22.0f, 1500.0f, color::text);
        char pct[16];
        std::snprintf(pct, sizeof(pct), "%d%%", static_cast<int>(state_.job.fraction() * 100.0f));
        ui::text(list, type.semibold, pct, 1920.0f - kGutter, 900.0f, 22.0f, color::text, Align::right);
        progress_bar(list, bar, state_.job.fraction(), ui::accent());
    }
    else if (!state_.job.error.empty() && notice_left_ <= 0.0f)
    {
        text_fit(list, type.regular, state_.job.error, kGutter, 920.0f, 22.0f, 1500.0f, color::text3);
    }

    if (notice_left_ > 0.0f)
        text_fit(list, type.medium, notice_, 960.0f, 968.0f, 24.0f, 1700.0f, color::text2,
                 Align::center);
    if (browser_failed_)
        ui::text(list, type.regular, tr("Couldn't open the system browser."), 960.0f, 1000.0f, 20.0f,
             color::text3, Align::center);

    // Action menu.
    if (mode_ == Mode::menu && count > 0)
    {
        DrawList &over = frame.overlay;
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
        button(over, type, start, tr("Start"), ButtonKind::primary,
               form_index_ == rows ? 1.0f : 0.0f, true, 24.0f);
    }

    if (mode_ == Mode::settings)
        draw_settings(frame);

    using hui::ui::Button;
    std::vector<hui::ui::Hint> hint_list;
    std::vector<std::string> hint_text;
    hint_text.reserve(8);
    const auto hint = [&](Button button, const char *english)
    {
        hint_text.push_back(tr(english));
        hint_list.push_back({button, hint_text.back().c_str()});
    };
    if (mode_ == Mode::form)
    {
        hint(Button::cross, "Change / Start");
        hint(Button::circle, "Back");
    }
    else if (mode_ == Mode::settings)
    {
        hint(Button::cross, "Select");
        hint(Button::circle, "Back");
    }
    else if (mode_ == Mode::menu)
    {
        hint(Button::cross, "Select");
        hint(Button::circle, "Back");
    }
    else
    {
        hint(Button::cross, "Actions");
        hint(Button::triangle, "Web interface");
        hint(Button::options, "Settings");
        hint(Button::touchpad, "Refresh");
    }
    if (state_.job.busy)
        hint(Button::square, "Cancel job");
    hints(frame.overlay, type, hint_list);
}

void App::draw_settings(Frame &frame)
{
    using namespace ui;
    const Type &type = context_.type;
    DrawList &over = frame.overlay;
    over.rounded_rect({0.0f, 0.0f, 1920.0f, 1080.0f}, 0.0f, color::sheet);
    ui::text(over, type.light, tr("Settings"), kGutter, 118.0f, 64.0f, color::text);
    const std::vector<SettingsRow> rows = settings_rows();
    const float x = 360.0f;
    const float w = 1200.0f;
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        const Rect r{x, 176.0f + static_cast<float>(i) * 72.0f, w, 62.0f};
        const bool on = static_cast<int>(i) == settings_index_;
        if (on)
            focus_ring(over, r, 18.0f, 1.0f);
        surface(over, r, 18.0f, on ? color::field_open : color::field);
        text_fit(over, type.semibold, rows[i].label, r.x + 30.0f, r.cy() + 9.0f, 26.0f, 620.0f,
                 color::text);
        if (on)
        {
            over.gradient_rect_h(r, 18.0f, ui::accent().with_alpha(0.10f), ui::accent().with_alpha(0.0f));
            over.rounded_rect({r.x + 10.0f, r.y + 18.0f, 5.0f, r.h - 36.0f}, 2.5f, ui::accent());
        }
        float value_w = text_fit(over, type.regular, rows[i].value, r.x + r.w - 30.0f, r.cy() + 8.0f,
                                 22.0f, 480.0f, color::text2, Align::right);
        if (rows[i].kind == SettingsRow::Kind::theme)
        {
            const float sx = r.x + r.w - 30.0f - value_w - 30.0f;
            const Color swatch = ui::skin_at(ui::skin_index(state_.theme)).swatch;
            over.glow({sx - 12.0f, r.cy() - 12.0f, 24.0f, 24.0f}, 12.0f, 10.0f, swatch.with_alpha(0.5f));
            over.circle(sx, r.cy(), 12.0f, swatch);
            if (on)
                ui::text(over, type.regular, "<  >", sx - 40.0f, r.cy() + 8.0f, 22.0f, color::text3,
                         Align::right);
        }
    }
    const UpdateInfo &u = state_.update;
    if (u.phase == "downloading")
    {
        progress_bar(over, {x, 176.0f + static_cast<float>(rows.size()) * 72.0f + 4.0f, w, 10.0f},
                     u.total > 0 ? static_cast<float>(u.received) / static_cast<float>(u.total) : 0.0f,
                     ui::accent());
    }
    const float y = 176.0f + static_cast<float>(rows.size()) * 72.0f + 56.0f;
    ui::text(over, type.regular,
             tr("TV app {app}   -   payload {payload} (build {build})",
                {{"app", context_.app_version},
                 {"payload", u.current_version.empty() ? std::string("?") : u.current_version},
                 {"build", std::to_string(u.current_build)}}),
             x, y, 20.0f, color::text3);
    if (!u.latest_version.empty())
    {
        ui::text(over, type.regular,
                 tr("Latest release: {version} (build {build})",
                    {{"version", u.latest_version}, {"build", std::to_string(u.latest_build)}}),
                 x, y + 34.0f, 20.0f, color::text3);
    }
    if (!context_.app_source.empty())
        ui::text_fit(over, type.regular, tr("Running from: {path}", {{"path", context_.app_source}}), x,
                     y + 112.0f, 18.0f, w, color::text2);
    if (!u.app_note.empty())
        ui::paragraph(over, type.regular, tr("App files: {note}", {{"note", u.app_note}}), x, y + 146.0f,
                      18.0f, w, 26.0f, color::text2, 3);
    ui::text(over, type.regular,
             tr("Updates come from the project's GitHub releases. The app image is replaced in place; reopen the app afterwards."),
             x, y + 76.0f, 18.0f, color::text3);
}

} // namespace gc
