// Game Compressor TV app - The full-screen interface.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/input.hpp"
#include "gc/store.hpp"
#include "gc/ui.hpp"
#include "gfx/backdrop_spec.hpp"
#include "gfx/draw_list.hpp"
#include "ui/feedback.hpp"

#include <functional>
#include <string>

namespace gc
{

struct Frame
{
    hui::gfx::BackdropSpec backdrop;
    hui::gfx::DrawList scene;
    hui::gfx::DrawList overlay;

    void reset()
    {
        backdrop = {};
        scene.clear();
        overlay.clear();
    }
};

struct Context
{
    Context(const ui::Type &type_, const hui::ui::Fonts &fonts_, Store &store_)
        : type(type_), fonts(fonts_), store(store_)
    {
    }
    const ui::Type &type;
    const hui::ui::Fonts &fonts;
    Store &store;
    std::string app_version;
    std::function<bool()> open_browser; // the full web interface, in the system browser
};

class App
{
  public:
    explicit App(Context &context) : context_(context) {}
    void update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void draw(Frame &frame);

  private:
    enum class Mode : std::uint8_t
    {
        list,
        menu, // action menu for the selected game
    };

    struct Choice
    {
        const char *label;
        Verb verb;
    };

    int choices(const Game &game, Choice out[3]) const;

    Context &context_;
    Snapshot state_;
    Mode mode_ = Mode::list;
    int selected_ = 0;
    int menu_index_ = 0;
    float scroll_ = 0.0f;
    float clock_ = 0.0f;
    std::string notice_;
    float notice_left_ = 0.0f;
    std::string last_message_;
    bool browser_failed_ = false;
    std::string measured_path_;
    float measured_age_ = 0.0f;
};

} // namespace gc
