// Game Compressor TV app - The payload's state, polled over loopback.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gc/store.hpp"
#include "gc/json.hpp"

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
    Command command{verb, {}, {}, {}};
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
                        "&sourcePath=" + encode(command.source_path);
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

void Store::run()
{
    double last_games = -1000.0;
    double last_start = -1000.0;
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
        {
            misses = 0;
            bool busy;
            {
                Lock lock(mutex_);
                busy = state_.job.busy;
                if (state_.link != Link::online)
                    state_.link = Link::online;
            }
            if (!state_.games_loaded || now - last_games > (busy ? 15.0 : 5.0))
            {
                if (poll_games())
                    last_games = now;
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
