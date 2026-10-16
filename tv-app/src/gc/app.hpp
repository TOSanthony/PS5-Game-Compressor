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
#include <map>
#include <string>
#include <vector>

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
    // Uploads RGBA pixels as a texture (the renderer's batch); 0 on failure.
    std::function<std::uint32_t(int, int, const std::uint8_t *)> make_texture;
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
        menu,     // action menu for the selected game
        form,     // options before compress / make image / uncompress
        settings, // updates and general settings
    };

    struct Item
    {
        enum class Next : std::uint8_t
        {
            run,
            copy,
            move,
            confirm_delete,
            close,
            form_compress,
            form_make_image,
            form_uncompress,
            ampr,
        };
        std::string label;
        std::string path;  // POST /api/gc/<path>
        std::string extra; // extra query, e.g. "&usbId=usb0"
        bool danger = false;
        Next next = Next::run;
    };

    struct FormRow
    {
        std::string label;
        std::vector<std::string> names;
        std::vector<std::string> values;
        int index = 0;
    };
    struct Form
    {
        std::string title;
        std::string path;
        std::string label;
        Item::Next kind = Item::Next::form_compress;
        std::vector<FormRow> rows;
    };
    struct SettingsRow
    {
        enum class Kind : std::uint8_t
        {
            update,
            auto_update,
            language,
            theme,
            restart,
            refresh,
            browser,
        };
        Kind kind;
        std::string label;
        std::string value;
    };

    Form build_form(const Game &game, Item::Next kind) const;
    void submit_form(const Game &game);
    std::vector<SettingsRow> settings_rows() const;
    void update_settings(const hui::InputFrame &input);
    void draw_settings(Frame &frame);
    std::vector<Item> ampr_menu(const Game &game) const;
    std::vector<Item> main_menu(const Game &game) const;
    std::vector<Item> target_menu(const Game &game, bool copy) const;

    Context &context_;
    Snapshot state_;
    Mode mode_ = Mode::list;
    int selected_ = 0;
    int menu_index_ = 0;
    std::vector<Item> menu_;
    std::string menu_title_;
    bool submenu_ = false;
    float scroll_ = 0.0f;
    float clock_ = 0.0f;
    std::string notice_;
    float notice_left_ = 0.0f;
    std::string last_message_;
    bool browser_failed_ = false;
    struct IconTexture
    {
        std::uint32_t texture = 0;
        int width = 0;
        int height = 0;
    };
    std::map<std::string, IconTexture> icons_;
    Form form_;
    int form_index_ = 0;
    int settings_index_ = 0;
    std::string measured_path_;
    float measured_age_ = 0.0f;
};

} // namespace gc
