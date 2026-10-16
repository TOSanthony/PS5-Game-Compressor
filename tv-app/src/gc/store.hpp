// Game Compressor TV app - The payload's state, polled over loopback.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One worker thread talks to the Game Compressor payload on 127.0.0.1:5910:
// it polls /api/gc/job often and /api/gc/games now and then, starts the
// payload through the ELF loader when nothing answers, and runs the actions
// the screen queues. The frame loop only copies a snapshot.

#pragma once

#include "gc/http.hpp"
#include "gc/starter.hpp"
#include "gc/thread.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace gc
{

struct Game
{
    std::string title_id;
    std::string name;
    std::string source_kind; // "folder", "image", "compressed", ...
    std::string source_path;
    std::string storage;
    std::string validation;
    std::string primary_action;
    std::int64_t source_size = 0;
    std::int64_t compressed_size = 0;
    std::int64_t saved_bytes = 0;
    bool size_pending = false;   // folder size not measured yet
    bool size_estimated = false; // cached value, may be stale
    std::string size_status;
};

struct Job
{
    bool busy = false;
    std::string verb;
    std::string phase;
    std::string current;
    std::string error;
    std::int64_t total_bytes = 0;
    std::int64_t copied_bytes = 0;
    std::int64_t total_blocks = 0;
    std::int64_t done_blocks = 0;

    float fraction() const
    {
        if (total_blocks > 0)
            return static_cast<float>(done_blocks) / static_cast<float>(total_blocks);
        if (total_bytes > 0)
            return static_cast<float>(copied_bytes) / static_cast<float>(total_bytes);
        return 0.0f;
    }
};

enum class Link : std::uint8_t
{
    starting, // looking for the payload
    online,
    offline, // no payload, no loader (kstuff / ELF loader missing?)
};

struct Snapshot
{
    Link link = Link::starting;
    bool games_loaded = false;
    std::vector<Game> games;
    Job job;
    std::string message; // last action result or start problem
    std::uint64_t version = 0; // bumps whenever the snapshot changes
};

enum class Verb : std::uint8_t
{
    compress,
    validate,
    repair,
    cancel,
    refresh,
    measure, // asks the payload to measure a folder's size now
};

class Store
{
  public:
    Store(http::Transport &transport, starter::Starter *starter);
    ~Store();
    void start();
    void stop();

    Snapshot snapshot();
    // Queues an action; game may be null for cancel / refresh.
    void request(Verb verb, const Game *game);

  private:
    struct Command
    {
        Verb verb;
        std::string title_id;
        std::string source_path;
        std::string name;
    };

    void run();
    bool poll_job();
    bool poll_games();
    void run_command(const Command &command);
    void set_message(const std::string &text);

    http::Transport &transport_;
    starter::Starter *starter_;
    Mutex mutex_;
    Signal wake_;
    Snapshot state_;
    std::deque<Command> commands_;
    bool quit_ = false;
    Thread thread_;
};

} // namespace gc
