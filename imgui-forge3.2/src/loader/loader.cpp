#include "loader/loader.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "assets.h"
#include "imgui_internal.h"
#include "menu/draw.h"
#include "menu/fonts.h"
#include "menu/menu.h"
#include "menu/style.h"
#include "menu/widgets.h"

namespace nl {
namespace {

using namespace nl::style;
using draw::snap;

constexpr float k_w = Loader::width, k_h = Loader::height, k_title_h = Loader::title_h;

constexpr ImU32 k_title_bg   = rgb(10, 11, 17);
constexpr ImU32 k_text       = rgb(232, 233, 238);
constexpr ImU32 k_muted      = rgb(132, 135, 147);
constexpr ImU32 k_dim        = rgb(94, 97, 108);
constexpr ImU32 k_blue       = rgb(58, 116, 248);
constexpr ImU32 k_blue_dark  = rgb(42, 94, 222);
constexpr ImU32 k_green      = rgb(82, 196, 124);
constexpr ImU32 k_field_bg   = rgb(17, 18, 25);
constexpr ImU32 k_field_edge = rgb(33, 34, 44);
constexpr ImU32 k_field_hover = rgb(46, 48, 60);

struct Product {
    const char* name;
    const char* sub;  // under the name
    const char* info; // under the Launch button
    bool beta;
    bool expired;
};

constexpr Product k_products[] = {
    { "Counter-Strike 2", "\xd0\x9f\xd0\xbe\xd0\xb4\xd0\xbf\xd0\xb8\xd1\x81\xd0\xba\xd0\xb0: 116 \xd0\xb4\xd0\xbd\xd0\xb5\xd0\xb9",
      "v2.4.1  \xc2\xb7  \xd0\xbe\xd0\xb1\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xbb\xd0\xb5\xd0\xbd\xd0\xbe 2 \xd1\x87\xd0\xb0\xd1\x81\xd0\xb0 \xd0\xbd\xd0\xb0\xd0\xb7\xd0\xb0\xd0\xb4",
      false, false },
    { "Counter-Strike 2", "\xd0\x9f\xd0\xbe\xd0\xb4\xd0\xbf\xd0\xb8\xd1\x81\xd0\xba\xd0\xb0: 116 \xd0\xb4\xd0\xbd\xd0\xb5\xd0\xb9",
      "v2.5.0 beta  \xc2\xb7  \xd0\xb2\xd0\xbe\xd0\xb7\xd0\xbc\xd0\xbe\xd0\xb6\xd0\xbd\xd1\x8b \xd0\xbe\xd1\x88\xd0\xb8\xd0\xb1\xd0\xba\xd0\xb8",
      true, false },
    { "CS:GO Legacy", "\xd0\x9f\xd0\xbe\xd0\xb4\xd0\xbf\xd0\xb8\xd1\x81\xd0\xba\xd0\xb0 \xd0\xb8\xd1\x81\xd1\x82\xd0\xb5\xd0\xba\xd0\xbb\xd0\xb0",
      "\xd0\x9f\xd1\x80\xd0\xbe\xd0\xb4\xd0\xbb\xd0\xb8\xd1\x82\xd0\xb5 \xd0\xbf\xd0\xbe\xd0\xb4\xd0\xbf\xd0\xb8\xd1\x81\xd0\xba\xd1\x83 \xd0\xbd\xd0\xb0 \xd1\x81\xd0\xb0\xd0\xb9\xd1\x82\xd0\xb5",
      false, true },
};
constexpr int k_product_count = static_cast<int>(sizeof(k_products) / sizeof(k_products[0]));

struct Step {
    const char* label;
    float duration;
};
constexpr Step k_steps[] = {
    { "\xd0\x9f\xd0\xbe\xd0\xb4\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5 \xd0\xba \xd1\x81\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80\xd1\x83...", 0.6f },
    { "\xd0\x9f\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb5\xd1\x80\xd0\xba\xd0\xb0 \xd0\xbf\xd0\xbe\xd0\xb4\xd0\xbf\xd0\xb8\xd1\x81\xd0\xba\xd0\xb8...", 0.6f },
    { "\xd0\x97\xd0\xb0\xd0\xb3\xd1\x80\xd1\x83\xd0\xb7\xd0\xba\xd0\xb0 \xd1\x84\xd0\xb0\xd0\xb9\xd0\xbb\xd0\xbe\xd0\xb2...", 1.6f },
    { "\xd0\x9f\xd0\xbe\xd0\xb4\xd0\xb3\xd0\xbe\xd1\x82\xd0\xbe\xd0\xb2\xd0\xba\xd0\xb0...", 0.8f },
};
constexpr const char* k_done_text = "\xd0\x97\xd0\xb0\xd0\xbf\xd1\x83\xd1\x81\xd1\x82\xd0\xb8\xd1\x82\xd0\xb5 "
                                    "\xd0\xb8\xd0\xb3\xd1\x80\xd1\x83 \xd0\xb8 \xd0\xbd\xd0\xb0\xd0\xb6\xd0\xbc\xd0\xb8\xd1\x82\xd0\xb5 Insert";

constexpr float total_load_time()
{
    float t = 0.0f;
    for (const Step& s : k_steps)
        t += s.duration;
    return t;
}

float smoothstep(float t) { return t * t * (3.0f - 2.0f * t); }

// Simulated progress: each step speeds up and slows down on its own, like a real download.
float load_progress(float time, const Step** current)
{
    float done = 0.0f;
    *current = &k_steps[0];
    for (const Step& s : k_steps) {
        *current = &s;
        if (time < s.duration)
            return (done + smoothstep(ImSaturate(time / s.duration)) * s.duration) / total_load_time();
        done += s.duration;
        time -= s.duration;
    }
    return 1.0f;
}

// Loader coordinates (unscaled, relative to the top-left corner `o`) to whole screen pixels.
ImVec2 at(const ImVec2& o, float x, float y) { return ImVec2(o.x + snap(px(x)), o.y + snap(px(y))); }

ImGuiID sub_id(ImGuiID id, int n) { return ImHashData(&n, sizeof(n), id); }

float ease(float t) { return t * (2.0f - t); }

// Text vertically centered on `cy`, starting at `x`.
void text_v(ImDrawList* dl, ImFont* font, float size, float x, float cy, ImU32 col, const char* s)
{
    draw::text(dl, font, size, ImVec2(snap(x), snap(cy - draw::cap_height(font, size) * 0.5f)), col, s);
}

void text_c(ImDrawList* dl, ImFont* font, float size, const ImVec2& c, ImU32 col, const char* s)
{
    text_v(dl, font, size, c.x - draw::text_width(font, size, s) * 0.5f, c.y, col, s);
}

void text_r(ImDrawList* dl, ImFont* font, float size, float right, float cy, ImU32 col, const char* s)
{
    text_v(dl, font, size, right - draw::text_width(font, size, s), cy, col, s);
}

void spinner(ImDrawList* dl, const ImVec2& c, float r, float thickness, ImU32 col, float time)
{
    const float a0 = time * 5.5f;
    const float len = IM_PI * (0.9f + 0.45f * std::sin(time * 2.6f));
    dl->PathArcTo(c, r, a0, a0 + len, 24);
    dl->PathStroke(col, thickness);
}

// Icon (or spinner) and label centered in [min, max].
void button_label(ImDrawList* dl, const ImVec2& min, const ImVec2& max, const char* label, const char* glyph, ImU32 col,
                  bool busy, float time)
{
    const Fonts& f = fonts();
    const float size = 13.2f;
    const float icon_w = glyph || busy ? px(17.0f) : 0.0f;
    const float tw = draw::text_width(f.semibold, size, label);
    const float x = snap((min.x + max.x - tw - icon_w) * 0.5f), cy = (min.y + max.y) * 0.5f;
    if (busy)
        spinner(dl, ImVec2(x + px(5.0f), cy), px(5.0f), px(1.6f), col, time);
    else if (glyph)
        draw::icon(dl, f.icons, 9.5f, ImVec2(x + px(5.0f), cy), col, glyph);
    text_v(dl, f.semibold, size, x + icon_w, cy, col, label);
}

// Blue fill with a light top edge, like the buttons on the site.
void blue_fill(ImDrawList* dl, const ImVec2& min, const ImVec2& max, float r, float light)
{
    draw::rect_gradient_v(dl, min, max, draw::lerp_color(k_blue, IM_COL32_WHITE, 0.06f + 0.08f * light),
                          draw::lerp_color(k_blue_dark, IM_COL32_WHITE, 0.08f * light), r);
    draw::border(dl, min, max, rgb(255, 255, 255, 22), r, draw::hairline());
}

// Draws a label layer with `alpha`, sliding in from below while it appears (`entering`) and
// out upwards while it leaves.
template <class Draw>
void label_layer(ImDrawList* dl, float alpha, bool entering, Draw&& draw)
{
    if (alpha <= 0.01f)
        return;
    const int v0 = dl->VtxBuffer.Size;
    draw();
    const float e = ease(alpha);
    fade_vertices(dl, v0, e, (entering ? 1.0f : -1.0f) * (1.0f - e) * px(7.0f));
}

// Primary button. `busy` crossfades the label to a spinner with `busy_label` and ignores clicks.
bool button(ImDrawList* dl, const char* str_id, const ImVec2& min, const ImVec2& max, const char* label,
            const char* busy_label, bool busy, float time)
{
    const ImGuiID id = ImGui::GetID(str_id);
    const ui::ItemState s = ui::item_behavior(id, min, max, !busy);
    const float h = ui::animate(sub_id(id, 0), s.hovered ? 1.0f : 0.0f, 16.0f);
    const float p = ui::animate(sub_id(id, 1), s.held ? 1.0f : 0.0f, 30.0f);
    const float b = ui::animate(sub_id(id, 2), busy ? 1.0f : 0.0f, 12.0f);
    if (s.hovered)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    const float r = px(6.0f);
    if (h > 0.0f) {
        dl->PushClipRectFullScreen();
        draw::soft_shadow(dl, min, max, r, px(12.0f), draw::with_alpha(k_blue, 0.3f * h));
        dl->PopClipRect();
    }
    blue_fill(dl, min, max, r, h - p);
    dl->PushClipRect(min, max, true);
    label_layer(dl, 1.0f - b, !busy, [&] { button_label(dl, min, max, label, nullptr, IM_COL32_WHITE, false, time); });
    label_layer(dl, b, busy, [&] { button_label(dl, min, max, busy_label, nullptr, IM_COL32_WHITE, true, time); });
    dl->PopClipRect();
    return s.pressed;
}

// Labelled input box with an icon. Returns true when Enter was pressed in it.
bool field(ImDrawList* dl, const char* str_id, const char* label, char* buf, int size, const ImVec2& min, const ImVec2& max,
           const char* glyph, const char* placeholder, bool error, bool focus, float right_pad,
           ImGuiInputTextFlags flags = 0)
{
    const Fonts& fo = fonts();
    text_v(dl, fo.medium, 11.8f, min.x, min.y - px(11.0f), rgb(160, 163, 174), label);

    const ImGuiID id = ImGui::GetCurrentWindow()->GetID(str_id);
    const bool active = ImGui::GetActiveID() == id;
    const float f = ui::animate(sub_id(id, 0), active ? 1.0f : 0.0f, 18.0f);
    const float h = ui::animate(sub_id(id, 1), ImGui::IsMouseHoveringRect(min, max) ? 1.0f : 0.0f, 18.0f);
    const float e = ui::animate(sub_id(id, 2), error ? 1.0f : 0.0f, 18.0f);
    const float r = px(6.0f);

    const ImU32 ring = draw::lerp_color(k_blue, col::window_danger, e);
    if (f > 0.0f || e > 0.0f) {
        const float g = px(3.0f);
        dl->AddRectFilled(ImVec2(min.x - g, min.y - g), ImVec2(max.x + g, max.y + g),
                          draw::with_alpha(ring, 0.16f * ImMax(f, e)), r + g);
    }
    dl->AddRectFilled(min, max, k_field_bg, r);
    ImU32 border = draw::lerp_color(k_field_edge, k_field_hover, h);
    border = draw::lerp_color(border, k_blue, f);
    border = draw::lerp_color(border, col::window_danger, e);
    draw::border(dl, min, max, border, r, draw::hairline());

    const float cy = (min.y + max.y) * 0.5f;
    ImU32 icon_col = draw::lerp_color(k_dim, rgb(150, 178, 255), f);
    icon_col = draw::lerp_color(icon_col, col::window_danger, e);
    draw::icon(dl, fo.icons, 10.0f, ImVec2(min.x + px(15.0f), cy), icon_col, glyph);
    return ui::text_field(str_id, buf, size, ImVec2(min.x + px(31.0f), min.y), ImVec2(max.x - px(right_pad), max.y),
                          placeholder, focus, k_text, flags | ImGuiInputTextFlags_EnterReturnsTrue);
}

// Premultiplied box filter of a square RGBA image from `sn` to `n` pixels, written at (1, 1) of
// `dst` (row length `stride` pixels) with the edge pixels repeated around it.
void resample(const unsigned char* src, int sn, unsigned char* dst, int stride, int n)
{
    const float scale = static_cast<float>(sn) / static_cast<float>(n);
    std::vector<float> pre(static_cast<size_t>(sn * sn * 4));
    for (int i = 0; i < sn * sn; ++i) {
        const float a = src[i * 4 + 3] / 255.0f;
        for (int c = 0; c < 3; ++c)
            pre[static_cast<size_t>(i * 4 + c)] = src[i * 4 + c] / 255.0f * a;
        pre[static_cast<size_t>(i * 4 + 3)] = a;
    }
    const auto weights = [&](int o, auto&& visit) {
        const float x0 = static_cast<float>(o) * scale, x1 = x0 + scale;
        for (int s = static_cast<int>(std::floor(x0)); s < static_cast<int>(std::ceil(x1)) && s < sn; ++s) {
            const float w = ImMin(x1, static_cast<float>(s + 1)) - ImMax(x0, static_cast<float>(s));
            if (w > 0.0f)
                visit(s, w / scale);
        }
    };
    std::vector<float> rows(static_cast<size_t>(n * sn * 4), 0.0f); // n columns, sn rows
    for (int y = 0; y < sn; ++y)
        for (int x = 0; x < n; ++x)
            weights(x, [&](int s, float w) {
                for (int c = 0; c < 4; ++c)
                    rows[static_cast<size_t>((y * n + x) * 4 + c)] += pre[static_cast<size_t>((y * sn + s) * 4 + c)] * w;
            });
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            float v[4] = {};
            weights(y, [&](int s, float w) {
                for (int c = 0; c < 4; ++c)
                    v[c] += rows[static_cast<size_t>((s * n + x) * 4 + c)] * w;
            });
            unsigned char* out = dst + (static_cast<size_t>(y + 1) * stride + x + 1) * 4;
            const float a = ImSaturate(v[3]);
            for (int c = 0; c < 3; ++c)
                out[c] = static_cast<unsigned char>(ImSaturate(a > 0.0f ? v[c] / a : 0.0f) * 255.0f + 0.5f);
            out[3] = static_cast<unsigned char>(a * 255.0f + 0.5f);
        }
    for (int y = 0; y < n + 2; ++y)
        for (int x = 0; x < n + 2; ++x) {
            const int sx = ImClamp(x, 1, n), sy = ImClamp(y, 1, n);
            if (sx != x || sy != y)
                std::memcpy(dst + (static_cast<size_t>(y) * stride + x) * 4, dst + (static_cast<size_t>(sy) * stride + sx) * 4, 4);
        }
}

} // namespace

void Loader::create_picture(Picture& p, const unsigned char* rgba, int side)
{
    p.src = rgba;
    p.src_side = side;
    p.tex = IM_NEW(ImTextureData)();
    p.tex->Create(ImTextureFormat_RGBA32, side + 2, side + 2);
    p.tex->UseColors = true;
    ImGui::RegisterUserTexture(p.tex);
}

void Loader::destroy_picture(Picture& p)
{
    if (!p.tex)
        return;
    ImGui::UnregisterUserTexture(p.tex);
    IM_DELETE(p.tex);
    p.tex = nullptr;
}

void Loader::draw_picture(ImDrawList* dl, Picture& p, const ImVec2& min, float d, float rounding, ImU32 tint, float inset)
{
    if (!p.tex)
        return;
    const int side = ImClamp(static_cast<int>(d / (1.0f - 2.0f * inset) + 0.5f), 1, p.src_side);
    if (side != p.side) {
        resample(p.src, p.src_side, static_cast<unsigned char*>(p.tex->GetPixels()), p.tex->Width, side);
        ImTextureDataQueueUpload(p.tex, 0, 0, side + 2, side + 2);
        p.side = side;
    }
    const float texel = 1.0f / static_cast<float>(p.tex->Width);
    const float crop = inset * static_cast<float>(side);
    const float uv0 = (1.0f + crop) * texel, uv1 = (1.0f + static_cast<float>(side) - crop) * texel;
    dl->AddImageRounded(p.tex->GetTexRef(), min, ImVec2(min.x + d, min.y + d), ImVec2(uv0, uv0), ImVec2(uv1, uv1), tint,
                        rounding);
}

void Loader::init()
{
    create_picture(avatar_, assets::avatar_rgba.data, assets::avatar_size);
    create_picture(cs2_, assets::cs2_rgba.data, assets::cs2_size);
}

void Loader::shutdown()
{
    destroy_picture(avatar_);
    destroy_picture(cs2_);
}

float Loader::appear(int i) const { return ease(ImSaturate((enter_ - 0.06f * static_cast<float>(i)) / 0.35f)); }

void Loader::set_stage(int stage, int product)
{
    product_ = ImClamp(product, 0, k_product_count - 1);
    std::snprintf(pass_, sizeof(pass_), "%s", stage > 0 ? "demo" : "");
    stage_ = stage > 0 ? 1 : 0;
    load_ = stage == 2 ? 1 : 0;
    load_time_ = load_shown_ = 0.0f;
    bar_ = load_ == 1 ? 0.0f : 1.0f;
    enter_ = 0.0f;
}

bool Loader::caption_hit(const ImVec2& p) const
{
    if (dismissing_)
        return false;
    return p.y >= origin_.y && p.y < origin_.y + px(k_title_h) && p.x >= origin_.x && p.x < origin_.x + px(k_w - 80.0f);
}

void Loader::begin_dismiss()
{
    if (dismissing_)
        return;
    dismissing_ = true;
    dismiss_t_ = 0.0f;
}

Loader::Dismiss Loader::dismiss() const
{
    constexpr float k_duration = 0.42f;
    const float u = dismissing_ ? ease(ImSaturate(dismiss_t_ / k_duration)) : 0.0f;
    Dismiss d;
    d.amount = u;
    d.alpha = 1.0f - u;
    d.scale = 1.0f - 0.16f * u;
    d.slide = -u * px(16.0f);
    return d;
}

void Loader::sign_in()
{
    if (signing_ > 0.0f)
        return;
    user_error_ = !user_[0];
    pass_error_ = !pass_[0];
    if (user_error_ || pass_error_) {
        message_ = user_error_ && pass_error_
                       ? "\xd0\x92\xd0\xb2\xd0\xb5\xd0\xb4\xd0\xb8\xd1\x82\xd0\xb5 \xd0\xbb\xd0\xbe\xd0\xb3\xd0\xb8\xd0\xbd \xd0\xb8 \xd0\xbf\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c"
                   : user_error_ ? "\xd0\x92\xd0\xb2\xd0\xb5\xd0\xb4\xd0\xb8\xd1\x82\xd0\xb5 \xd0\xbb\xd0\xbe\xd0\xb3\xd0\xb8\xd0\xbd"
                                 : "\xd0\x92\xd0\xb2\xd0\xb5\xd0\xb4\xd0\xb8\xd1\x82\xd0\xb5 \xd0\xbf\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c";
        shake_ = 0.4f;
        return;
    }
    message_ = nullptr;
    signing_ = 0.9f;
}

void Loader::render()
{
    const ImGuiIO& io = ImGui::GetIO();
    const float dt = io.DeltaTime;
    time_ += dt;

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##loader", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar(3);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int drawn_from = dl->VtxBuffer.Size;
    const bool fading_out = dismissing_;
    if (fading_out) {
        dismiss_t_ += dt;
        constexpr float k_duration = 0.42f;
        if (dismiss_t_ >= k_duration)
            close_ = true;
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
    }

    const ImVec2 size(snap(px(k_w)), snap(px(k_h)));
    origin_ = ImVec2(std::floor(ImMax(0.0f, io.DisplaySize.x - size.x) * 0.5f),
                     std::floor(ImMax(0.0f, io.DisplaySize.y - size.y) * 0.5f));
    const ImVec2 o = origin_;

    // The current stage fades out before the next one comes in.
    enter_ += dt;
    out_ = 1.0f;
    if (next_stage_ >= 0) {
        constexpr float k_leave = 0.18f;
        leave_ += dt;
        out_ = 1.0f - ease(ImSaturate(leave_ / k_leave));
        if (leave_ >= k_leave) {
            stage_ = next_stage_;
            next_stage_ = -1;
            enter_ = 0.0f;
            out_ = 1.0f;
        }
    }

    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), col::main_bg);
    draw_title_bar(dl, o);

    const int v0 = dl->VtxBuffer.Size;
    const bool leaving = next_stage_ >= 0;
    if (leaving)
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
    ImGui::PushID(stage_);
    if (stage_ == 0)
        draw_login(dl, o);
    else
        draw_products(dl, o);
    ImGui::PopID();
    if (leaving)
        ImGui::PopItemFlag();
    if (out_ < 1.0f)
        fade_vertices(dl, v0, out_, -(1.0f - out_) * px(10.0f));

    if (signing_ > 0.0f) {
        signing_ -= dt;
        if (signing_ <= 0.0f) {
            signing_ = 0.0f;
            next_stage_ = 1;
            leave_ = 0.0f;
        }
    }

    if (dismissing_) {
        const Dismiss d = dismiss();
        const ImVec2 c(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        for (int i = drawn_from; i < dl->VtxBuffer.Size; ++i) {
            ImDrawVert& v = dl->VtxBuffer[i];
            v.pos.x = c.x + (v.pos.x - c.x) * d.scale;
            v.pos.y = c.y + (v.pos.y - c.y) * d.scale + d.slide;
            v.col = draw::with_alpha(v.col, d.alpha);
        }
    }
    if (fading_out)
        ImGui::PopItemFlag();
    ImGui::End();
}

void Loader::draw_title_bar(ImDrawList* dl, const ImVec2& o)
{
    const Fonts& f = fonts();
    dl->AddRectFilled(o, ImVec2(o.x + snap(px(k_w)), o.y + snap(px(k_title_h))), k_title_bg);
    draw::hline(dl, o.x, o.x + px(k_w), o.y + snap(px(k_title_h)), col::header_separator);

    const ImVec2 lmin = at(o, 14.0f, 9.0f), lmax = at(o, 36.0f, 31.0f);
    dl->AddRectFilled(lmin, lmax, col::logo_bg, px(6.0f));
    const float nl_size = 12.5f;
    const float nl_w = draw::text_width(f.black, nl_size, "NL");
    draw::text_glow(dl, f.black, nl_size,
                    ImVec2(snap((lmin.x + lmax.x - nl_w) * 0.5f),
                           snap((lmin.y + lmax.y) * 0.5f - draw::cap_height(f.black, nl_size) * 0.5f)),
                    col::logo_text, col::logo_glow, px(2.0f), "NL");
    text_v(dl, f.semibold, font_size::text, o.x + px(45.0f), o.y + px(20.0f), col::title, "Neverlose");

    // Signed-in user next to the window buttons; slides in after signing in.
    const float user_a = stage_ == 1 ? ease(ImSaturate(enter_ / 0.4f)) * out_ : 0.0f;
    if (user_a > 0.0f) {
        const float d = snap(px(22.0f));
        const float dx = snap((1.0f - user_a) * px(10.0f));
        const ImVec2 amin(at(o, k_w - 108.0f, 9.0f).x + dx, at(o, 0.0f, 9.0f).y);
        draw_picture(dl, avatar_, amin, d, d * 0.5f, draw::with_alpha(IM_COL32_WHITE, user_a), 1.0f / 30.0f);
        text_r(dl, f.medium, 12.4f, amin.x - px(8.0f), o.y + px(20.0f), draw::with_alpha(rgb(200, 202, 210), user_a), user_);
        draw::vline(dl, at(o, k_w - 78.0f, 0.0f).x, at(o, 0.0f, 13.0f).y, at(o, 0.0f, 27.0f).y,
                    draw::with_alpha(col::header_separator, user_a));
    }

    for (int i = 0; i < 2; ++i) {
        const float x0 = k_w - (i == 0 ? 72.0f : 38.0f);
        const ImVec2 min = at(o, x0, 8.0f), max = at(o, x0 + 30.0f, 32.0f);
        const ImGuiID id = ImGui::GetID(i == 0 ? "##minimize" : "##close");
        const ui::ItemState s = ui::item_behavior(id, min, max);
        const float h = ui::animate(sub_id(id, 0), s.hovered ? 1.0f : 0.0f, 18.0f);
        if (h > 0.0f)
            dl->AddRectFilled(min, max, i == 1 ? draw::with_alpha(col::window_danger, 0.9f * h)
                                               : rgb(255, 255, 255, static_cast<int>(14.0f * h)),
                              px(6.0f));
        draw::icon(dl, f.icons, 10.0f, ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f),
                   draw::lerp_color(k_muted, IM_COL32_WHITE, h), i == 0 ? icon::minus : icon::xmark);
        if (s.pressed) {
            if (i == 0)
                minimize_ = true;
            else
                begin_dismiss();
        }
    }
}

void Loader::draw_login(ImDrawList* dl, const ImVec2& o)
{
    const Fonts& f = fonts();
    shake_ = ImMax(0.0f, shake_ - ImGui::GetIO().DeltaTime);
    const float sx = shake_ > 0.0f ? std::sin(shake_ * 70.0f) * px(5.0f) * (shake_ / 0.4f) : 0.0f;
    const auto pos = [&](float x, float y) { return ImVec2(at(o, x, y).x + sx, at(o, x, y).y); };
    const auto layer = [&](int i, auto&& draw) {
        const int v = dl->VtxBuffer.Size;
        draw();
        const float a = appear(i);
        if (a < 1.0f)
            fade_vertices(dl, v, a, (1.0f - a) * px(10.0f));
    };

    layer(0, [&] {
        draw::text(dl, f.semibold, 17.0f, at(o, 24.0f, 60.0f), col::title,
                   "\xd0\x92\xd1\x85\xd0\xbe\xd0\xb4 \xd0\xb2 \xd0\xb0\xd0\xba\xd0\xba\xd0\xb0\xd1\x83\xd0\xbd\xd1\x82");
        draw::text(dl, f.medium, 12.2f, at(o, 24.0f, 84.0f), k_muted,
                   "\xd0\x98\xd1\x81\xd0\xbf\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xb7\xd1\x83\xd0\xb9\xd1\x82\xd0\xb5 "
                   "\xd0\xb4\xd0\xb0\xd0\xbd\xd0\xbd\xd1\x8b\xd0\xb5 \xd0\xbe\xd1\x82 neverlose.cc");
    });

    const bool busy = signing_ > 0.0f;
    if (busy)
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
    layer(1, [&] {
        if (field(dl, "##user", "\xd0\x9b\xd0\xbe\xd0\xb3\xd0\xb8\xd0\xbd", user_, sizeof(user_), pos(24.0f, 126.0f),
                  pos(356.0f, 162.0f), icon::user,
                  "\xd0\x92\xd0\xb2\xd0\xb5\xd0\xb4\xd0\xb8\xd1\x82\xd0\xb5 \xd0\xbb\xd0\xbe\xd0\xb3\xd0\xb8\xd0\xbd", user_error_,
                  false, 12.0f))
            focus_pass_ = true;
        if (user_[0])
            user_error_ = false;
    });

    layer(2, [&] {
        const ImVec2 pmin = pos(24.0f, 192.0f), pmax = pos(356.0f, 228.0f);
        // Show / hide password, registered before the field so it wins the overlap.
        const ImVec2 c(pmax.x - px(18.0f), (pmin.y + pmax.y) * 0.5f);
        const ImGuiID id = ImGui::GetID("##eye");
        const ui::ItemState s = ui::item_behavior(id, ImVec2(c.x - px(12.0f), c.y - px(12.0f)),
                                                  ImVec2(c.x + px(12.0f), c.y + px(12.0f)));
        if (s.hovered)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (s.pressed)
            show_pass_ = !show_pass_;
        const float h = ui::animate(sub_id(id, 0), s.hovered ? 1.0f : 0.0f, 18.0f);
        const float shown = ui::animate(sub_id(id, 1), show_pass_ ? 1.0f : 0.0f, 16.0f);
        const bool submit = field(dl, "##pass", "\xd0\x9f\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c", pass_, sizeof(pass_),
                                  pmin, pmax, icon::lock,
                                  "\xd0\x92\xd0\xb2\xd0\xb5\xd0\xb4\xd0\xb8\xd1\x82\xd0\xb5 \xd0\xbf\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c",
                                  pass_error_, focus_pass_, 36.0f, show_pass_ ? 0 : ImGuiInputTextFlags_Password);
        focus_pass_ = false;
        if (pass_[0])
            pass_error_ = false;
        const ImU32 ic = draw::lerp_color(k_dim, col::window_label, h);
        if (shown < 1.0f)
            draw::icon(dl, f.icons, 10.0f, c, draw::with_alpha(ic, 1.0f - shown), icon::eye);
        if (shown > 0.0f)
            draw::icon(dl, f.icons, 10.0f, c, draw::with_alpha(ic, shown), icon::eye_slash);
        if (submit)
            sign_in();
    });

    layer(3, [&] {
        if (button(dl, "##signin", at(o, 24.0f, 246.0f), at(o, 356.0f, 282.0f), "\xd0\x92\xd0\xbe\xd0\xb9\xd1\x82\xd0\xb8",
                   "\xd0\x92\xd1\x85\xd0\xbe\xd0\xb4...", busy, time_))
            sign_in();
    });
    if (busy)
        ImGui::PopItemFlag();

    if (message_)
        shown_message_ = message_;
    const float a = ui::animate(ImGui::GetID("##message"), message_ ? 1.0f : 0.0f, 14.0f);
    if (shown_message_)
        label_layer(dl, a, message_ != nullptr, [&] {
            text_c(dl, f.medium, 12.0f, ImVec2(o.x + px(k_w * 0.5f), o.y + px(301.0f)), col::window_danger, shown_message_);
        });
}

void Loader::draw_products(ImDrawList* dl, const ImVec2& o)
{
    const Fonts& f = fonts();
    const auto layer = [&](int i, auto&& draw) {
        const int v = dl->VtxBuffer.Size;
        draw();
        const float a = appear(i);
        if (a < 1.0f)
            fade_vertices(dl, v, a, (1.0f - a) * px(10.0f));
    };

    constexpr float y0 = 68.0f, row_h = 52.0f;
    const ImVec2 pmin = at(o, 16.0f, y0), pmax = at(o, k_w - 16.0f, y0 + row_h * k_product_count);
    layer(0, [&] {
        draw::text(dl, f.medium, font_size::header, at(o, 18.0f, 56.0f), col::section_header,
                   "\xd0\x9f\xd0\xa0\xd0\x9e\xd0\x94\xd0\xa3\xd0\x9a\xd0\xa2\xd0\xab");
        dl->AddRectFilled(pmin, pmax, col::panel_bg, px(8.0f));
    });

    // A row per product; the selected row has a filled radio mark.
    for (int i = 0; i < k_product_count; ++i) {
        layer(1 + i, [&] {
            const Product& p = k_products[i];
            const float y = y0 + row_h * static_cast<float>(i);
            const ImVec2 min(pmin.x, at(o, 0.0f, y).y), max(pmax.x, at(o, 0.0f, y + row_h).y);
            ImGui::PushID(i);
            const ImGuiID id = ImGui::GetID("##product");
            ImGui::PopID();
            const bool selected = i == product_;
            const ui::ItemState s = ui::item_behavior(id, min, max, load_ != 1);
            const float hv = ui::animate(sub_id(id, 0), s.hovered && !selected ? 1.0f : 0.0f, 18.0f);
            const float sel = ui::animate(sub_id(id, 1), selected ? 1.0f : 0.0f, 16.0f);
            const float press = ui::animate(sub_id(id, 2), s.held ? 1.0f : 0.0f, 30.0f);
            if (s.hovered && !selected)
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (s.pressed && !selected) {
                product_ = i;
                load_ = 0;
            }
            const ImDrawFlags corners = i == 0                     ? ImDrawFlags_RoundCornersTop
                                        : i == k_product_count - 1 ? ImDrawFlags_RoundCornersBottom
                                                                   : ImDrawFlags_RoundCornersNone;
            if (hv > 0.0f || sel > 0.0f)
                dl->AddRectFilled(min, max,
                                  draw::lerp_color(draw::with_alpha(col::panel_row_hover, hv), draw::with_alpha(k_blue, 0.09f), sel),
                                  px(8.0f), corners);
            if (i > 0)
                draw::hline(dl, min.x + px(14.0f), max.x - px(14.0f), min.y, col::row_separator);

            // The content nudges right while hovered and dips while pressed.
            const float cy = (min.y + max.y) * 0.5f + snap(press * px(1.0f));
            const float nx = snap(hv * px(2.0f));
            const float d = snap(px(32.0f));
            const ImVec2 imin(o.x + snap(px(30.0f)) + nx, snap(cy - d * 0.5f));
            draw_picture(dl, cs2_, imin, d, px(7.0f), p.expired ? rgb(120, 120, 128, 150) : IM_COL32_WHITE);

            const float tx = o.x + px(76.0f) + nx;
            text_v(dl, f.semibold, 13.4f, tx, cy - px(8.0f), p.expired ? rgb(150, 152, 162) : k_text, p.name);
            if (p.beta) {
                const float bx = snap(tx + draw::text_width(f.semibold, 13.4f, p.name) + px(7.0f));
                const float bw = snap(draw::text_width(f.semibold, 8.6f, "BETA") + px(10.0f));
                const ImVec2 bmin(bx, snap(cy - px(15.0f))), bmax(bx + bw, snap(cy - px(1.0f)));
                dl->AddRectFilled(bmin, bmax, rgb(150, 100, 255, 40), px(4.0f));
                text_c(dl, f.semibold, 8.6f, ImVec2((bmin.x + bmax.x) * 0.5f, (bmin.y + bmax.y) * 0.5f),
                       rgb(190, 160, 255), "BETA");
            }
            text_v(dl, f.medium, 11.6f, tx, cy + px(9.0f),
                   p.expired ? draw::with_alpha(col::window_danger, 0.85f) : k_muted, p.sub);

            // Radio mark: the ring brightens on hover, the dot pops in when selected.
            const ImVec2 rc(max.x - px(24.0f), cy);
            const float rr = px(7.5f);
            dl->AddCircleFilled(rc, rr, draw::lerp_color(rgb(28, 29, 38), k_blue, sel), 32);
            if (sel < 1.0f)
                dl->AddCircle(rc, rr - px(0.5f),
                              draw::with_alpha(draw::lerp_color(rgb(62, 64, 78), rgb(104, 108, 126), hv), 1.0f - sel), 32,
                              ImMax(1.0f, px(1.2f)));
            if (sel > 0.0f) {
                const float pop = sel < 1.0f ? 1.0f + 0.35f * std::sin(sel * IM_PI) : 1.0f;
                dl->AddCircleFilled(rc, px(3.0f) * sel * pop, IM_COL32_WHITE, 24);
            }
        });
    }
    layer(0, [&] { draw::border(dl, pmin, pmax, col::panel_border, px(8.0f), draw::hairline()); });

    layer(4, [&] { draw_launch(dl, at(o, 16.0f, 240.0f), at(o, k_w - 16.0f, 276.0f)); });
    layer(5, [&] { draw_status(dl, o); });
}

void Loader::draw_launch(ImDrawList* dl, const ImVec2& min, const ImVec2& max)
{
    const float dt = ImGui::GetIO().DeltaTime;
    const Product& p = k_products[product_];

    if (load_ == 1) {
        if (real_ && task_failed_) {
            error_ = task_status_;
            load_ = 0;
            load_shown_ = 0.0f;
            task_failed_ = false;
            task_status_ = nullptr;
        } else if (real_) {
            load_shown_ = ImLerp(load_shown_, task_progress_, 1.0f - std::exp(-8.0f * dt));
            if (task_finished_ && load_shown_ > 0.997f) {
                load_shown_ = 1.0f;
                load_ = 2;
                flash_ = 1.0f;
                task_finished_ = false;
                done_hold_ = 0.0f;
            }
        } else {
            load_time_ += dt;
            const Step* step = nullptr;
            const float progress = load_progress(load_time_, &step);
            load_shown_ = ImLerp(load_shown_, progress, 1.0f - std::exp(-8.0f * dt));
            if (progress >= 1.0f && load_shown_ > 0.997f) {
                load_shown_ = 1.0f;
                load_ = 2;
                flash_ = 1.0f;
            }
        }
    }
    // Full while idle; on Launch it drains to the progress and then fills up with it.
    const float target = load_ == 1 ? load_shown_ : 1.0f;
    bar_ = ImLerp(bar_, target, 1.0f - std::exp(-12.0f * dt));
    if (std::fabs(bar_ - target) < 0.0005f)
        bar_ = target;
    flash_ = ImMax(0.0f, flash_ - dt * 1.6f);
    // The bar has reached the end and "Готово" is on screen. Leave it long enough
    // to read, then close. A failed launch stays open.
    if (real_ && load_ == 2) {
        done_hold_ += dt;
        if (done_hold_ > 0.45f)
            begin_dismiss();
    }

    const ImGuiID id = ImGui::GetID("##launch");
    const bool clickable = load_ != 1 && !p.expired;
    const ui::ItemState s = ui::item_behavior(id, min, max, clickable);
    const float h = ui::animate(sub_id(id, 0), s.hovered && clickable ? 1.0f : 0.0f, 16.0f);
    const float pr = ui::animate(sub_id(id, 1), s.held ? 1.0f : 0.0f, 30.0f);
    const float e = ui::animate(sub_id(id, 2), p.expired ? 0.0f : 1.0f, 12.0f);
    if (s.hovered && clickable)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (s.pressed) {
        if (load_ == 2) {
            load_ = 0;
            task_finished_ = false;
            task_status_ = nullptr;
            error_ = nullptr;
        } else if (real_) {
            load_ = 1;
            load_time_ = load_shown_ = 0.0f;
            bar_ = 0.0f;
            task_progress_ = 0.02f;
            task_finished_ = false;
            task_failed_ = false;
            task_status_ = "\xd0\x97\xd0\xb0\xd0\xbf\xd1\x83\xd1\x81\xd0\xba...";
            error_ = nullptr;
            launch_request_ = true;
        } else {
            load_ = 1;
            load_time_ = load_shown_ = 0.0f;
        }
    }

    const float r = px(6.0f);
    const float glow = ImMax(h * 0.3f, flash_ * 0.5f) * e;
    if (glow > 0.0f) {
        dl->PushClipRectFullScreen();
        draw::soft_shadow(dl, min, max, r, px(12.0f + 6.0f * flash_), draw::with_alpha(k_blue, glow));
        dl->PopClipRect();
    }
    dl->AddRectFilled(min, max, rgb(22, 24, 34), r);
    draw::border(dl, min, max, k_field_edge, r, draw::hairline());

    const float w = (max.x - min.x) * bar_;
    if (w > 1.0f && e > 0.0f) {
        const ImVec2 fmax(snap(min.x + w), max.y);
        const int v0 = dl->VtxBuffer.Size;
        blue_fill(dl, min, fmax, ImMin(r, (fmax.x - min.x) * 0.5f), h - pr + flash_ * 0.8f);
        if (load_ == 1) {
            // A soft light sweeping along the filled part, kept off the rounded corners.
            const float band = px(60.0f);
            const float x = min.x - band + std::fmod(time_ * 0.75f, 1.0f) * (max.x - min.x + 2.0f * band);
            const float x1 = ImMax(min.x + r, fmax.x - r);
            // Fades out before it reaches the end of the fill, so the clip edge never shows.
            const float k = ImSaturate((x1 - x) / (band * 1.5f)) * ImSaturate((x - min.x) / band);
            dl->PushClipRect(ImVec2(min.x + r, min.y), ImVec2(x1, max.y), true);
            const ImU32 c0 = rgb(255, 255, 255, 0), c1 = draw::with_alpha(rgb(255, 255, 255, 46), k);
            dl->AddRectFilledMultiColor(ImVec2(x - band, min.y), ImVec2(x, max.y), c0, c1, c1, c0);
            dl->AddRectFilledMultiColor(ImVec2(x, min.y), ImVec2(x + band, max.y), c1, c0, c0, c1);
            dl->PopClipRect();
        }
        if (e < 1.0f)
            fade_vertices(dl, v0, e);
    }

    // Labels of the four states crossfade into each other.
    char pct[16];
    std::snprintf(pct, sizeof(pct), "%d%%", static_cast<int>(load_shown_ * 100.0f + 0.5f));
    const int state = p.expired ? 3 : load_;
    const char* labels[] = { "\xd0\x97\xd0\xb0\xd0\xbf\xd1\x83\xd1\x81\xd1\x82\xd0\xb8\xd1\x82\xd1\x8c", pct,
                             "\xd0\x93\xd0\xbe\xd1\x82\xd0\xbe\xd0\xb2\xd0\xbe",
                             "\xd0\x9d\xd0\xb5\xd0\xb4\xd0\xbe\xd1\x81\xd1\x82\xd1\x83\xd0\xbf\xd0\xbd\xd0\xbe" };
    const char* glyphs[] = { icon::play, nullptr, icon::check, icon::lock };
    dl->PushClipRect(min, max, true);
    for (int k = 0; k < 4; ++k) {
        const float a = ui::animate(sub_id(id, 10 + k), state == k ? 1.0f : 0.0f, 12.0f);
        label_layer(dl, a, state == k, [&] {
            button_label(dl, min, max, labels[k], glyphs[k], k == 3 ? k_dim : IM_COL32_WHITE, false, time_);
        });
    }
    dl->PopClipRect();
}

void Loader::draw_status(ImDrawList* dl, const ImVec2& o)
{
    const Fonts& f = fonts();
    const Product& p = k_products[product_];
    const char* text = p.info;
    if (load_ == 1) {
        if (real_ && task_status_)
            text = task_status_;
        else if (!real_) {
            const Step* step = nullptr;
            load_progress(load_time_, &step);
            text = step->label;
        }
    } else if (load_ == 2) {
        text = (real_ && task_status_) ? task_status_ : k_done_text;
    } else if (error_) {
        text = error_;
    }
    if (text != status_) {
        status_prev_ = status_;
        status_ = text;
        status_t_ = 0.0f;
    }
    status_t_ = ImMin(1.0f, status_t_ + ImGui::GetIO().DeltaTime * 5.0f);

    const float cy = o.y + px(298.0f);
    const auto line = [&](const char* s, float a, bool entering) {
        label_layer(dl, a, entering, [&] {
            if (load_ == 2 && entering) {
                const float w = px(16.0f) + draw::text_width(f.medium, 11.8f, s);
                const float x = snap(o.x + (px(k_w) - w) * 0.5f);
                draw::icon(dl, f.icons, 9.5f, ImVec2(x + px(5.0f), cy), k_green, icon::circle_check);
                text_v(dl, f.medium, 11.8f, x + px(16.0f), cy, k_green, s);
                return;
            }
            bool step = load_ == 1;
            if (!real_) {
                for (const Step& st : k_steps)
                    step = step || st.label == s;
            }
            const ImU32 col = (error_ && s == error_) ? col::window_danger : (step ? k_muted : k_dim);
            text_c(dl, f.medium, 11.8f, ImVec2(o.x + px(k_w * 0.5f), cy), col, s);
        });
    };
    if (status_prev_ && status_t_ < 1.0f)
        line(status_prev_, 1.0f - status_t_, false);
    line(status_, status_t_, true);
}

void Loader::bind_launch(bool real) { real_ = real; }

bool Loader::take_launch()
{
    const bool requested = launch_request_;
    launch_request_ = false;
    return requested;
}

void Loader::set_job(float progress, const char* status, bool done, bool failed)
{
    if (load_ != 1)
        return;
    task_progress_ = progress;
    if (status && status[0])
        task_status_ = status;
    task_failed_ = failed;
    task_finished_ = done && !failed;
}

} // namespace nl
