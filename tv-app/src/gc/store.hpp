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
#include <map>
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
    bool ampr_present = false;
    bool is_mounted = false;
    bool can_move_to_usb = false;
    bool can_move_to_internal = false;
    bool can_stream_delete = false;
    bool ampr_update_needed = false;
    bool ampr_original_available = false;
    std::string ampr_version;
    std::string ampr_latest_version;
    std::string ampr_latest_sha256;
};

struct AmprVersion
{
    std::string version;
    std::string sha256;
};

// The payload's self-update (/api/gc/update).
struct UpdateInfo
{
    bool known = false; // the payload answered at least once
    bool busy = false;
    std::string phase = "idle"; // idle checking available uptodate downloading verifying installing installed error
    std::string error;
    std::string current_version;
    std::string latest_version;
    std::string app_status; // replaced created folder-copy none
    std::string file;
    std::int64_t current_build = 0;
    std::int64_t latest_build = 0;
    std::int64_t received = 0;
    std::int64_t total = 0;
    bool available = false;
    bool elf_saved = false;
};

struct IconPixels
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba; // width * height * 4
};

struct Target
{
    std::string id;
    std::string name;
    std::int64_t free_bytes = 0;
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
    std::vector<Target> targets; // USB / extended storage
    std::vector<AmprVersion> ampr_versions; // cached AMPR builds
    UpdateInfo update;
    bool auto_update = true;
    std::string theme = "Midnight"; // background theme (gc::ui::Skin name)
    int language = 0; // i18n::Language: 0 automatic, 1 English, 2 French
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
    action,  // any POST /api/gc/<path> on a game
    update_check,
    update_install,
    restart_payload, // sends the newest payload copy to the ELF loader
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
    void set_auto_update(bool on);
    void set_language(int language);
    void set_theme(const std::string &name);
    // Game icons: want_icon() queues the fetch; take_icon() hands the decoded pixels over once.
    void want_icon(const std::string &title_id);
    bool take_icon(const std::string &title_id, IconPixels *out);
    // Queues POST /api/gc/<path>?titleId&sourcePath[&extra] with a label for the message.
    void act(const std::string &path, const std::string &label, const Game &game,
             const std::string &extra = {});

  private:
    struct Command
    {
        Verb verb;
        std::string title_id;
        std::string source_path;
        std::string name;
        std::string path;  // Verb::action
        std::string label; // Verb::action
        std::string extra; // Verb::action, e.g. "&usbId=usb0"
    };

    void run();
    bool poll_job();
    bool poll_games();
    void poll_targets();
    void poll_ampr();
    void fetch_icons();
    bool poll_update();
    void load_settings();
    void save_settings();
    void update_logic(bool alive);
    void run_command(const Command &command);
    void set_message(const std::string &text);

    http::Transport &transport_;
    starter::Starter *starter_;
    Mutex mutex_;
    Signal wake_;
    Snapshot state_;
    std::deque<Command> commands_;
    std::deque<std::string> icon_queue_;
    std::map<std::string, int> icon_state_; // 1 queued, 2 ready, 3 failed, 4 taken
    std::map<std::string, IconPixels> icon_ready_;
    bool quit_ = false;
    bool outdated_handled_ = false;   // the outdated-payload restart was done (or tried)
    bool update_checked_ = false;     // the start-up check was sent
    bool restart_pending_ = false;    // restart the payload once idle
    std::int64_t installed_build_ = 0; // never install the same build twice per session
    std::int64_t restarted_build_ = 0;
    Thread thread_;
};

} // namespace gc
