// Game Compressor TV app - The payload's state, polled over loopback.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gc/store.hpp"
#include "gc/json.hpp"

extern "C"
{
#include "third_party/stb/stb_image.inc"
}

#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace gc
{

namespace
{

std::string encode(const std::string &text)
{
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : text)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~' || c == '/')
            out += static_cast<char>(c);
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

} // namespace

Store::Store(http::Transport &transport, starter::Starter *starter)
    : transport_(transport), starter_(starter)
{
}

Store::~Store()
{
    stop();
}

void Store::start()
{
    thread_.start([this] { run(); }, 1u << 20);
}

void Store::stop()
{
    {
        Lock lock(mutex_);
        quit_ = true;
    }
    wake_.notify();
    thread_.join();
}

Snapshot Store::snapshot()
{
    Lock lock(mutex_);
    return state_;
}

void Store::request(Verb verb, const Game *game)
{
    Command command{verb, {}, {}, {}, {}, {}, {}};
    if (game)
    {
        command.title_id = game->title_id;
        command.source_path = game->source_path;
        command.name = game->name;
    }
    {
        Lock lock(mutex_);
        commands_.push_back(std::move(command));
    }
    wake_.notify();
}

void Store::act(const std::string &path, const std::string &label, const Game &game,
                const std::string &extra)
{
    Command command{Verb::action, game.title_id, game.source_path, game.name, path, label, extra};
    {
        Lock lock(mutex_);
        commands_.push_back(std::move(command));
    }
    wake_.notify();
}

void Store::set_message(const std::string &text)
{
    Lock lock(mutex_);
    state_.message = text;
    ++state_.version;
}

bool Store::poll_job()
{
    http::Request request;
    request.path = "/api/gc/job";
    const http::Response reply = transport_.send(request);
    if (!reply.transported() || reply.status != 200)
        return false;
    json::Value doc;
    if (!json::parse(reply.body, &doc))
        return false;
    Job job;
    job.busy = doc.flag("busy");
    job.verb = doc.text("verb");
    job.phase = doc.text("phase");
    job.current = doc.text("current");
    job.error = doc.text("error");
    job.total_bytes = static_cast<std::int64_t>(doc.number("totalBytes"));
    job.copied_bytes = static_cast<std::int64_t>(doc.number("copiedBytes"));
    job.total_blocks = static_cast<std::int64_t>(doc.number("totalBlocks"));
    job.done_blocks = static_cast<std::int64_t>(doc.number("doneBlocks"));
    Lock lock(mutex_);
    state_.job = std::move(job);
    state_.link = Link::online;
    ++state_.version;
    return true;
}

bool Store::poll_games()
{
    http::Request request;
    request.path = "/api/gc/games";
    request.max_bytes = 16u << 20;
    const http::Response reply = transport_.send(request);
    if (!reply.transported() || reply.status != 200)
        return false;
    json::Value doc;
    if (!json::parse(reply.body, &doc))
        return false;
    std::vector<Game> games;
    for (const json::Value &item : doc["games"].items())
    {
        Game game;
        game.title_id = item.text("titleId");
        game.name = item.text("name");
        game.source_kind = item.text("sourceKind");
        game.source_path = item.text("sourcePath");
        game.storage = item.text("storage");
        game.validation = item.text("validation");
        game.primary_action = item.text("primaryAction");
        game.source_size = static_cast<std::int64_t>(item.number("sourceSize"));
        game.compressed_size = static_cast<std::int64_t>(item.number("compressedSize"));
        game.saved_bytes = static_cast<std::int64_t>(item.number("savedBytes"));
        game.size_pending = item.flag("sizePending");
        game.size_estimated = item.flag("sizeEstimated");
        game.size_status = item.text("sizeStatus");
        game.ampr_present = item.flag("amprPresent");
        game.is_mounted = item.flag("isMounted");
        game.can_stream_delete = item.flag("canStreamDelete");
        game.ampr_update_needed = item.flag("amprUpdateNeeded");
        game.ampr_original_available = item.flag("amprOriginalAvailable");
        game.ampr_version = item.text("amprVersion");
        game.ampr_latest_version = item.text("amprLatestVersion");
        game.ampr_latest_sha256 = item.text("amprLatestSha256");
        game.can_move_to_usb = item.flag("canMoveToUsb");
        game.can_move_to_internal = item.flag("canMoveToInternal");
        if (game.name.empty())
            game.name = game.title_id;
        if (!game.title_id.empty())
            games.push_back(std::move(game));
    }
    Lock lock(mutex_);
    state_.games = std::move(games);
    state_.games_loaded = true;
    ++state_.version;
    return true;
}

void Store::poll_targets()
{
    http::Request request;
    request.path = "/api/gc/usb";
    const http::Response reply = transport_.send(request);
    json::Value doc;
    if (!reply.transported() || reply.status != 200 || !json::parse(reply.body, &doc))
        return;
    std::vector<Target> targets;
    for (const json::Value &item : doc["usb"].items())
    {
        Target target;
        target.id = item.text("id");
        target.name = item.text("name");
        target.free_bytes = static_cast<std::int64_t>(item.number("freeBytes"));
        if (!target.id.empty())
            targets.push_back(std::move(target));
    }
    Lock lock(mutex_);
    state_.targets = std::move(targets);
    ++state_.version;
}

void Store::want_icon(const std::string &title_id)
{
    {
        Lock lock(mutex_);
        if (title_id.empty() || icon_state_.count(title_id) != 0 || icon_state_.size() >= 96)
            return;
        icon_state_[title_id] = 1;
        icon_queue_.push_back(title_id);
    }
    wake_.notify();
}

bool Store::take_icon(const std::string &title_id, IconPixels *out)
{
    Lock lock(mutex_);
    const auto it = icon_ready_.find(title_id);
    if (it == icon_ready_.end())
        return false;
    *out = std::move(it->second);
    icon_ready_.erase(it);
    icon_state_[title_id] = 4;
    return true;
}

// PNG icons (usually 512 x 512) are reduced to at most 256 px with a box filter.
void Store::fetch_icons()
{
    for (;;)
    {
        std::string id;
        {
            Lock lock(mutex_);
            if (icon_queue_.empty() || quit_)
                return;
            id = icon_queue_.front();
            icon_queue_.pop_front();
        }
        http::Request request;
        request.path = "/api/gc/icon?titleId=" + encode(id);
        request.max_bytes = 3u << 20;
        const http::Response reply = transport_.send(request);
        IconPixels pixels;
        if (reply.transported() && reply.status == 200 && !reply.body.empty())
        {
            int w = 0;
            int h = 0;
            int channels = 0;
            unsigned char *data = stbi_load_from_memory(
                reinterpret_cast<const unsigned char *>(reply.body.data()),
                static_cast<int>(reply.body.size()), &w, &h, &channels, 4);
            if (data && w > 0 && h > 0 && w <= 2048 && h <= 2048)
            {
                int step = 1;
                while (w / step > 256 || h / step > 256)
                    ++step;
                const int ow = w / step;
                const int oh = h / step;
                pixels.width = ow;
                pixels.height = oh;
                pixels.rgba.resize(static_cast<std::size_t>(ow) * static_cast<std::size_t>(oh) * 4);
                for (int y = 0; y < oh; ++y)
                    for (int x = 0; x < ow; ++x)
                    {
                        unsigned sum[4] = {0, 0, 0, 0};
                        for (int dy = 0; dy < step; ++dy)
                            for (int dx = 0; dx < step; ++dx)
                            {
                                const unsigned char *src =
                                    data + (static_cast<std::size_t>(y * step + dy) *
                                                static_cast<std::size_t>(w) +
                                            static_cast<std::size_t>(x * step + dx)) *
                                               4;
                                for (int k = 0; k < 4; ++k)
                                    sum[k] += src[k];
                            }
                        std::uint8_t *dst = pixels.rgba.data() +
                                            (static_cast<std::size_t>(y) *
                                                 static_cast<std::size_t>(ow) +
                                             static_cast<std::size_t>(x)) *
                                                4;
                        for (int k = 0; k < 4; ++k)
                            dst[k] = static_cast<std::uint8_t>(sum[k] / static_cast<unsigned>(step * step));
                    }
            }
            if (data)
                stbi_image_free(data);
        }
        Lock lock(mutex_);
        if (pixels.width > 0)
        {
            icon_ready_[id] = std::move(pixels);
            icon_state_[id] = 2;
        }
        else
            icon_state_[id] = 3;
        ++state_.version;
    }
}

void Store::poll_ampr()
{
    http::Request request;
    request.path = "/api/gc/ampr/versions";
    const http::Response reply = transport_.send(request);
    json::Value doc;
    if (!reply.transported() || reply.status != 200 || !json::parse(reply.body, &doc))
        return;
    std::vector<AmprVersion> versions;
    for (const json::Value &item : doc["versions"].items())
        if (!item.text("version").empty() && item.text("sha256").size() == 64)
            versions.push_back({item.text("version"), item.text("sha256")});
    Lock lock(mutex_);
    state_.ampr_versions = std::move(versions);
    ++state_.version;
}

bool Store::poll_update()
{
    http::Request request;
    request.path = "/api/gc/update";
    const http::Response reply = transport_.send(request);
    json::Value doc;
    if (!reply.transported() || reply.status != 200 || !json::parse(reply.body, &doc))
        return false;
    UpdateInfo info;
    info.known = true;
    info.busy = doc.flag("busy");
    info.phase = doc.text("phase");
    info.error = doc.text("error");
    info.current_version = doc.text("currentVersion");
    info.latest_version = doc.text("latestVersion");
    info.app_status = doc.text("appStatus");
    info.file = doc.text("file");
    info.current_build = static_cast<std::int64_t>(doc.number("currentBuild"));
    info.latest_build = static_cast<std::int64_t>(doc.number("latestBuild"));
    info.received = static_cast<std::int64_t>(doc.number("received"));
    info.total = static_cast<std::int64_t>(doc.number("total"));
    info.available = doc.flag("available");
    info.elf_saved = doc.flag("elfSaved");
    Lock lock(mutex_);
    state_.update = std::move(info);
    ++state_.version;
    return true;
}

namespace
{
constexpr const char *kSettingsDir = "/data/game-compressor";
constexpr const char *kSettingsFile = "/data/game-compressor/tv-app.conf";
} // namespace

void Store::load_settings()
{
    const int fd = open(kSettingsFile, O_RDONLY);
    if (fd < 0)
        return;
    char text[128] = {};
    const ssize_t n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0)
        return;
    Lock lock(mutex_);
    state_.auto_update = std::string(text).find("auto_update=0") == std::string::npos;
}

void Store::save_settings()
{
    bool on;
    {
        Lock lock(mutex_);
        on = state_.auto_update;
    }
    mkdir(kSettingsDir, 0755);
    const int fd = open(kSettingsFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    const char *line = on ? "auto_update=1\n" : "auto_update=0\n";
    if (write(fd, line, 14) < 0)
    {
    }
    close(fd);
}

void Store::set_auto_update(bool on)
{
    {
        Lock lock(mutex_);
        state_.auto_update = on;
        ++state_.version;
    }
    save_settings();
}

void Store::run_command(const Command &command)
{
    http::Request request;
    request.method = "POST";
    std::string label;
    switch (command.verb)
    {
    case Verb::refresh:
        poll_games();
        return;
    case Verb::measure:
        request.path = "/api/gc/size-priority?path=" + encode(command.source_path);
        transport_.send(request);
        poll_games();
        return;
    case Verb::update_check:
        request.path = "/api/gc/update/check";
        transport_.send(request);
        poll_update();
        return;
    case Verb::update_install:
        request.path = "/api/gc/update/install";
        transport_.send(request);
        poll_update();
        return;
    case Verb::restart_payload:
        restart_pending_ = true;
        return;
    case Verb::action:
        request.path = command.path;
        label = command.label;
        break;
    case Verb::cancel:
        request.path = "/api/gc/job/cancel";
        label = "Cancel";
        break;
    case Verb::compress:
        request.path = "/api/gc/compress";
        label = "Compress";
        break;
    case Verb::validate:
        request.path = "/api/gc/validate-only";
        label = "Validate";
        break;
    case Verb::repair:
        request.path = "/api/gc/validate-repair";
        label = "Validate and repair";
        break;
    }
    if (command.verb != Verb::cancel)
        request.path += "?titleId=" + encode(command.title_id) +
                        "&sourcePath=" + encode(command.source_path) + command.extra;
    const http::Response reply = transport_.send(request);
    json::Value doc;
    std::string detail;
    if (reply.transported() && json::parse(reply.body, &doc))
        detail = doc.text("error");
    if (reply.transported() && reply.status >= 200 && reply.status < 300)
        set_message(label + " started" + (command.name.empty() ? "" : ": " + command.name));
    else
        set_message(label + " refused" + (detail.empty() ? "" : ": " + detail));
    poll_job();
}

// Start-up check, automatic install, and the payload restart that follows.
void Store::update_logic(bool alive)
{
    if (!alive)
        return;
    UpdateInfo info;
    bool automatic;
    bool busy;
    {
        Lock lock(mutex_);
        info = state_.update;
        automatic = state_.auto_update;
        busy = state_.job.busy;
    }
    if (!update_checked_)
    {
        update_checked_ = true;
        http::Request request;
        request.method = "POST";
        request.path = "/api/gc/update/check";
        transport_.send(request);
        poll_update();
        return;
    }
    if (automatic && info.known && info.phase == "available" && info.available && !busy &&
        installed_build_ != info.latest_build)
    {
        installed_build_ = info.latest_build;
        http::Request request;
        request.method = "POST";
        request.path = "/api/gc/update/install";
        transport_.send(request);
        poll_update();
        set_message("Updating Game Compressor to build " + std::to_string(info.latest_build) + "...");
        return;
    }
    if (info.phase == "installed" && info.elf_saved && restarted_build_ != info.latest_build)
    {
        restart_pending_ = true;
        restarted_build_ = info.latest_build;
    }
    if (restart_pending_ && !busy && starter_)
    {
        restart_pending_ = false;
        const starter::Result result = starter_->start();
        std::string text;
        if (info.phase == "installed")
        {
            text = "Updated to build " + std::to_string(info.latest_build) +
                   (info.app_status == "folder-copy"
                        ? ". The app folder copy was not changed: update it by hand."
                        : ". Close and reopen the app to use the new interface.");
        }
        else
        {
            text = result.detail;
        }
        set_message(text);
    }
}

void Store::run()
{
    load_settings();
    double last_games = -1000.0;
    double last_start = -1000.0;
    double last_ampr = -1000.0;
    int misses = 0;
    for (;;)
    {
        std::deque<Command> todo;
        {
            Lock lock(mutex_);
            if (quit_)
                return;
            todo.swap(commands_);
        }
        for (const Command &command : todo)
            run_command(command);

        const bool alive = poll_job();
        const double now = now_seconds();
        if (alive)
            fetch_icons();
        if (alive)
        {
            bool active;
            {
                Lock lock(mutex_);
                const std::string &phase = state_.update.phase;
                active = state_.update.busy || phase == "checking" || phase == "downloading" ||
                         phase == "verifying" || phase == "installing";
            }
            if (active)
                poll_update();
            update_logic(true);
        }
        if (alive)
        {
            misses = 0;
            bool busy;
            bool sizing = false;
            {
                Lock lock(mutex_);
                busy = state_.job.busy;
                for (const Game &game : state_.games)
                    sizing = sizing || (game.source_kind == "folder" && game.size_pending);
                if (state_.link != Link::online)
                    state_.link = Link::online;
            }
            if (!state_.games_loaded || now - last_games > (sizing ? 2.0 : busy ? 15.0 : 5.0))
            {
                if (poll_games())
                {
                    last_games = now;
                    poll_targets();
                    bool any_ampr = false;
                    {
                        Lock lock(mutex_);
                        for (const Game &game : state_.games)
                            any_ampr = any_ampr || game.ampr_present;
                    }
                    if (any_ampr && now - last_ampr > 60.0)
                    {
                        last_ampr = now;
                        poll_ampr();
                    }
                }
            }
        }
        else if (++misses >= 2 && now - last_start > 30.0)
        {
            // Nothing answers: hand the payload to the ELF loader.
            last_start = now;
            if (starter_)
            {
                const starter::Result result = starter_->start();
                Lock lock(mutex_);
                state_.link = result.outcome == starter::Outcome::sent ? Link::starting
                                                                       : Link::offline;
                state_.message = result.detail;
                ++state_.version;
            }
        }
        Lock lock(mutex_);
        if (quit_)
            return;
        wake_.wait(mutex_, alive ? (state_.job.busy ? 0.5 : 1.0) : 2.0);
    }
}

} // namespace gc
