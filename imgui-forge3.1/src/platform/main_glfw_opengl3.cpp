// Linux/macOS viewer: GLFW + OpenGL 3.
//
// Besides the interactive window it can render a deterministic screenshot, which is how the
// menu is compared pixel-by-pixel against the reference:
//   neverlose_menu --size 663x502 --menu-pos 14,6 --screenshot out.ppm
//   neverlose_menu --click 344,90,5 --screenshot out.ppm   (click at x,y on frame 5; --rclick for the right button)
//   neverlose_menu --scale 1.5 --screenshot out.ppm         (UI scale; the window grows with it)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <GL/gl.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "menu/backdrop.h"
#include "menu/blur.h"
#include "menu/fonts.h"
#include "menu/menu.h"
#include "menu/style.h"
#include "platform/blur_gl.h"

namespace {

struct InputEvent {
    int frame;
    float x, y;
    int button; // -1 = move only, 0/1 = left/right click (press on `frame`, release on `frame + 1`)
};

struct Options {
    int width = 872;
    int height = 624;
    bool size_set = false;
    float scale = 1.0f;
    bool menu_pos_set = false;
    float menu_x = 0.0f, menu_y = 0.0f;
    const char* screenshot = nullptr;
    int frames = 30;
    int tab = -1;
    int subtab = 0;
    int picker = -1;
    bool blur = true;
    std::vector<InputEvent> events;
};

bool parse_pair(const char* s, float& a, float& b, char sep)
{
    char* end = nullptr;
    a = std::strtof(s, &end);
    if (!end || *end != sep)
        return false;
    b = std::strtof(end + 1, &end);
    return end && *end == '\0';
}

bool parse_event(const char* s, InputEvent& e)
{
    return std::sscanf(s, "%f,%f,%d", &e.x, &e.y, &e.frame) == 3;
}

Options parse_args(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
        float x = 0.0f, y = 0.0f;
        InputEvent e{};
        if (!std::strcmp(a, "--size") && v && parse_pair(v, x, y, 'x')) {
            o.width = static_cast<int>(x);
            o.height = static_cast<int>(y);
            o.size_set = true;
            ++i;
        } else if (!std::strcmp(a, "--scale") && v) {
            o.scale = std::strtof(v, nullptr);
            ++i;
        } else if (!std::strcmp(a, "--menu-pos") && v && parse_pair(v, x, y, ',')) {
            o.menu_pos_set = true;
            o.menu_x = x;
            o.menu_y = y;
            ++i;
        } else if (!std::strcmp(a, "--screenshot") && v) {
            o.screenshot = v;
            ++i;
        } else if (!std::strcmp(a, "--frames") && v) {
            o.frames = std::atoi(v);
            ++i;
        } else if (!std::strcmp(a, "--tab") && v) {
            o.tab = std::atoi(v);
            ++i;
        } else if (!std::strcmp(a, "--subtab") && v) {
            o.subtab = std::atoi(v);
            ++i;
        } else if (!std::strcmp(a, "--picker") && v) {
            o.picker = std::atoi(v);
            ++i;
        } else if (!std::strcmp(a, "--no-blur")) {
            o.blur = false;
        } else if (!std::strcmp(a, "--mouse") && v && parse_pair(v, x, y, ',')) {
            o.events.push_back({ 0, x, y, -1 });
            ++i;
        } else if (!std::strcmp(a, "--move") && v && parse_event(v, e)) {
            e.button = -1;
            o.events.push_back(e);
            ++i;
        } else if (!std::strcmp(a, "--click") && v && parse_event(v, e)) {
            e.button = 0;
            o.events.push_back(e);
            ++i;
        } else if (!std::strcmp(a, "--rclick") && v && parse_event(v, e)) {
            e.button = 1;
            o.events.push_back(e);
            ++i;
        } else {
            std::fprintf(stderr, "unknown or incomplete argument: %s\n", a);
        }
    }
    o.scale = std::clamp(o.scale, nl::Menu::min_scale, nl::Menu::max_scale);
    if (!o.size_set) {
        o.width = static_cast<int>(std::lround(o.width * o.scale));
        o.height = static_cast<int>(std::lround(o.height * o.scale));
    }
    return o;
}

// Re-sent every frame: the GLFW backend may report the real cursor position otherwise.
void feed_scripted_input(const Options& o, int frame, ImVec2& mouse)
{
    ImGuiIO& io = ImGui::GetIO();
    for (const InputEvent& e : o.events)
        if ((e.button < 0 && e.frame == frame) || (e.button >= 0 && e.frame - 1 == frame))
            mouse = ImVec2(e.x, e.y);
    io.AddMousePosEvent(mouse.x, mouse.y);
    for (const InputEvent& e : o.events) {
        if (e.button >= 0 && e.frame == frame)
            io.AddMouseButtonEvent(e.button, true);
        if (e.button >= 0 && e.frame + 1 == frame)
            io.AddMouseButtonEvent(e.button, false);
    }
}

bool write_ppm(const char* path, int w, int h)
{
    std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    FILE* f = std::fopen(path, "wb");
    if (!f)
        return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; --y)
        std::fwrite(pixels.data() + static_cast<size_t>(y) * w * 3, 1, static_cast<size_t>(w) * 3, f);
    std::fclose(f);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    const Options opt = parse_args(argc, argv);
    const bool scripted = opt.screenshot != nullptr;

    glfwSetErrorCallback([](int code, const char* msg) { std::fprintf(stderr, "GLFW error %d: %s\n", code, msg); });
    if (!glfwInit())
        return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, scripted ? GLFW_FALSE : GLFW_TRUE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(opt.width, opt.height, "Neverlose \xe2\x80\x94 ImGui", nullptr, nullptr);
    if (!window)
        return 1;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(scripted ? 0 : 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    nl::apply_imgui_style();
    nl::load_fonts();

    ImGui_ImplGlfw_InitForOpenGL(window, !scripted);
    ImGui_ImplOpenGL3_Init("#version 150");
    if (opt.blur && nl::blur_gl::init(reinterpret_cast<nl::blur_gl::GetProc>(glfwGetProcAddress)))
        nl::blur::set_backend(nl::blur_gl::backend());

    int fb_w = 0, fb_h = 0;
    glfwGetFramebufferSize(window, &fb_w, &fb_h);
    const ImVec2 menu_pos =
        opt.menu_pos_set
            ? ImVec2(opt.menu_x, opt.menu_y)
            : ImVec2(std::floor((static_cast<float>(fb_w) - nl::style::layout::width * opt.scale) * 0.5f),
                     std::floor((static_cast<float>(fb_h) - nl::style::layout::height * opt.scale) * 0.5f));
    nl::Menu menu;
    menu.init(menu_pos, opt.scale);
    if (opt.tab >= 0)
        menu.set_tab(opt.tab, opt.subtab);
    if (opt.picker >= 0)
        menu.open_picker(opt.picker);

    bool insert_was_down = false;
    ImVec2 scripted_mouse(-FLT_MAX, -FLT_MAX);
    for (int frame = 0; !glfwWindowShouldClose(window); ++frame) {
        glfwPollEvents();
        if (!scripted) {
            const bool insert_down = glfwGetKey(window, GLFW_KEY_INSERT) == GLFW_PRESS;
            if (insert_down && !insert_was_down)
                menu.toggle_visible();
            insert_was_down = insert_down;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        if (scripted) {
            io.DeltaTime = 1.0f / 60.0f;
            feed_scripted_input(opt, frame, scripted_mouse);
        }
        ImGui::NewFrame();
        nl::draw_backdrop(ImGui::GetBackgroundDrawList(), io.DisplaySize, menu.visible());
        menu.render();
        ImGui::Render();

        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (scripted && frame + 1 >= opt.frames) {
            glFinish();
            const bool ok = write_ppm(opt.screenshot, fb_w, fb_h);
            std::printf("%s %s (%dx%d)\n", ok ? "wrote" : "failed to write", opt.screenshot, fb_w, fb_h);
            break;
        }
        glfwSwapBuffers(window);
    }

    nl::blur::set_backend({});
    nl::blur_gl::shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    menu.shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
