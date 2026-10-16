// Game Compressor TV app - Console entry point.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Opens the display and controller, then runs the full-screen interface every
// frame against the Game Compressor payload on this console's loopback address
// (port 5910). Adapted from the Orbit Store TV app and ps5-homebrew-ui's
// src/main.cpp.

#include "core/input.hpp"
#include "core/save_file.hpp"
#include "core/version.hpp"
#include "gc/app.hpp"
#include "gc/http.hpp"
#include "gc/starter.hpp"
#include "gc/store.hpp"
#include "gc/ui.hpp"
#include "gfx/renderer.hpp"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/system.hpp"
#include "ui/fonts.hpp"

#include <GL/glcorearb.h>

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>

extern "C" int sceSystemServiceLaunchWebBrowser(const char *uri, void *parameters);

namespace
{

constexpr const char *kAssets = "/app0/assets";
constexpr const char *kBackend = "127.0.0.1";
constexpr std::uint16_t kPort = 5910; // the payload's web server
constexpr const char *kBrowserUrl = "http://127.0.0.1:5910/";

bool load_font(hui::gfx::Renderer &renderer, const char *name, hui::gfx::Font *font,
               hui::ui::FontRef *ref)
{
    std::string data;
    const std::string path = std::string(kAssets) + "/fonts/" + name;
    if (!hui::save::read_file(path, &data) || !font->load(data))
    {
        hui::sys::log("[GC] font %s failed: %s", name, font->error().c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

} // namespace

int main()
{
    using namespace hui;
    sys::log("[GC] entry");

    ps5::Display display;
    const bool modes = ps5::Display::supports_display_modes();
    if (!(modes && display.open(3840, 2160)) && !display.open(1920, 1080))
    {
        sys::log("[GC] fatal: display open failed");
        sys::park();
    }
    sys::log("[GC] display %dx%d", display.width(), display.height());

    gfx::Renderer renderer;
    gfx::Font light;
    gfx::Font regular;
    gfx::Font medium;
    gfx::Font semibold;
    gc::ui::Type type;
    if (!renderer.init() ||
        !load_font(renderer, "inter-display-light.huifont", &light, &type.light) ||
        !load_font(renderer, "inter-regular.huifont", &regular, &type.regular) ||
        !load_font(renderer, "inter-medium.huifont", &medium, &type.medium) ||
        !load_font(renderer, "inter-semibold.huifont", &semibold, &type.semibold))
    {
        sys::log("[GC] fatal: renderer init failed");
        sys::park();
    }
    ui::Fonts fonts;
    fonts.regular = type.regular;
    fonts.semibold = type.semibold;
    fonts.display = type.light;
    fonts.mono = type.medium;
    fonts.pixel = type.regular;
    fonts.hand = type.regular;

    ps5::Pad pad;
    pad.open();
    pad.set_light_bar(0x2a, 0x9d, 0xf4);
    InputTracker tracker;

    gc::http::SocketTransport transport(kBackend, kPort);
    gc::starter::LoaderStarter starter({}, gc::starter::LoaderStarter::kLoaderPort,
                                       [](const std::string &line)
                                       { sys::log("[GC] %s", line.c_str()); });
    gc::Store store(transport, &starter);
    store.start();

    gc::Context context(type, fonts, store);
    context.app_version = read_content_version("/app0/sce_sys/param.json");
    context.open_browser = []
    { return sceSystemServiceLaunchWebBrowser(kBrowserUrl, nullptr) == 0; };
    context.make_texture = [&renderer](int w, int h, const std::uint8_t *rgba)
    { return renderer.batch().create_texture(w, h, rgba); };
    gc::App app(context);
    gc::Frame frame;
    sys::log("[GC] ready version=%s", context.app_version.c_str());

    std::int64_t last = sys::monotonic_us();
    std::uint64_t frames = 0;
    PadSample samples[64];
    ui::Feedback feedback;
    for (;;)
    {
        const std::int64_t now = sys::monotonic_us();
        float dt = frames == 0 ? 1.0f / 60.0f : static_cast<float>(now - last) / 1e6f;
        last = now;
        if (dt > 0.05f)
            dt = 0.05f;
        const std::size_t count = pad.read(samples);
        const InputFrame input = tracker.update(std::span<const PadSample>(samples, count),
                                                static_cast<std::uint64_t>(now));
        feedback.clear();
        app.update(input, dt, feedback);
        if (feedback.rumble_strength > 0.0f)
            pad.rumble(feedback.rumble_strength, feedback.rumble_seconds);
        pad.tick(dt);

        frame.reset();
        app.draw(frame);
        renderer.begin();
        renderer.backdrop(frame.backdrop);
        renderer.draw(frame.scene);
        renderer.draw(frame.overlay);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        renderer.present(0, display.width(), display.height());
        if (!display.swap())
        {
            sys::log("[GC] fatal: swap failed frame=%llu error=%s",
                     static_cast<unsigned long long>(frames),
                     ps5::egl_error_name(display.last_error()));
            sys::park();
        }
        if (++frames == 1)
            sys::log("[GC] first frame, splash hidden=%d", sys::hide_splash_screen() ? 1 : 0);
    }
}
