#pragma once

#include "imgui.h"

#include "assets.h"

struct ImTextureData;

namespace nl::assets {
extern const Blob cs2_rgba; // 128x128 RGBA8, defined in the loader target
inline constexpr int cs2_size = 128;
}

namespace nl {

// Loader window: sign in, pick a product, press Launch.
class Loader {
public:
    // Size at scale 1. The loader fills a borderless window of this size times style::ui_scale.
    static constexpr float width = 380.0f;
    static constexpr float height = 320.0f;
    static constexpr float title_h = 40.0f;

    // Call after ImGui::CreateContext() and load_fonts().
    void init();
    // Call after the renderer backend has shut down and before ImGui::DestroyContext().
    void shutdown();
    // Call between ImGui::NewFrame() and ImGui::Render().
    void render();

    // 0 sign in, 1 products, 2 loading the selected product. For previews and tests.
    void set_stage(int stage, int product = 0);
    // The empty part of the title bar, where the window can be dragged. `p` is in client pixels.
    bool caption_hit(const ImVec2& p) const;
    bool close_requested() const { return close_; }
    // Close-out after a successful inject (and the title-bar close button).
    // `amount` eases 0..1 while the window shrinks, rises and fades.
    struct Dismiss {
        float amount = 0.0f;
        float alpha = 1.0f;
        float scale = 1.0f;
        float slide = 0.0f;
    };
    Dismiss dismiss() const;
    bool take_minimize()
    {
        const bool m = minimize_;
        minimize_ = false;
        return m;
    }

    // Launch runs the inject job instead of the simulated download.
    void bind_launch(bool real);
    bool take_launch();
    bool busy() const { return load_ == 1; }
    // Ignored unless a launch is in progress. `status` must stay alive (a literal).
    void set_job(float progress, const char* status, bool done, bool failed);

private:
    // A square image resampled to the size it is drawn at, so it stays sharp at any scale.
    struct Picture {
        ImTextureData* tex = nullptr;
        const unsigned char* src = nullptr;
        int src_side = 0;
        int side = 0; // size of the last resample, in pixels
    };
    static void create_picture(Picture& p, const unsigned char* rgba, int side);
    static void destroy_picture(Picture& p);
    // Draws `p` over [min, min + d] with the given corner rounding; `inset` crops that fraction
    // of the image on each side.
    static void draw_picture(ImDrawList* dl, Picture& p, const ImVec2& min, float d, float rounding,
                             ImU32 tint = IM_COL32_WHITE, float inset = 0.0f);

    void draw_title_bar(ImDrawList* dl, const ImVec2& o);
    void draw_login(ImDrawList* dl, const ImVec2& o);
    void draw_products(ImDrawList* dl, const ImVec2& o);
    void sign_in();

    void draw_launch(ImDrawList* dl, const ImVec2& min, const ImVec2& max);
    void draw_status(ImDrawList* dl, const ImVec2& o);
    // Entrance of the i-th element of the current stage, 0..1 (elements follow each other).
    float appear(int i) const;

    int stage_ = 0;           // 0 sign in, 1 products
    int next_stage_ = -1;     // stage shown once the current one has faded out
    float enter_ = 0.0f;      // seconds since the current stage appeared
    float leave_ = 0.0f;      // seconds since the current stage started fading out
    float out_ = 1.0f;        // 1 while shown, falls to 0 while fading out
    int product_ = 0;
    int load_ = 0;            // 0 idle, 1 loading, 2 done
    float load_time_ = 0.0f;
    float load_shown_ = 0.0f; // percentage, eased towards the real progress
    float bar_ = 1.0f;        // fill of the launch button: full when idle, the progress while loading
    float flash_ = 0.0f;      // highlight when loading finishes
    const char* status_ = nullptr, *status_prev_ = nullptr; // line under the button, crossfaded
    float status_t_ = 1.0f;
    float time_ = 0.0f;
    ImVec2 origin_{};

    char user_[64] = "x3lay";
    char pass_[64] = {};
    bool show_pass_ = false;
    bool focus_pass_ = false;
    bool user_error_ = false, pass_error_ = false;
    const char* message_ = nullptr;
    const char* shown_message_ = nullptr; // kept while the message fades out
    float shake_ = 0.0f;
    float signing_ = 0.0f;    // countdown of the fake sign-in request

    bool real_ = false;
    bool launch_request_ = false;
    bool task_finished_ = false;
    bool task_failed_ = false;
    float done_hold_ = 0.0f;
    bool dismissing_ = false;
    float dismiss_t_ = 0.0f;
    void begin_dismiss();
    float task_progress_ = 0.0f;
    const char* task_status_ = nullptr;
    const char* error_ = nullptr;

    bool close_ = false;
    bool minimize_ = false;
    Picture avatar_, cs2_;
};

} // namespace nl
