#pragma once
#include <cstdint>

namespace render {
inline constexpr int overlay_gpu_priority = -2;
struct feature_plan { bool sample_players{}; bool gpu_effects{}; };
[[nodiscard]] constexpr feature_plan plan_features(bool attached, bool player_esp,
    bool chams, bool hardware_device, bool use_gpu) noexcept
{
    const bool gpu = hardware_device && use_gpu;
    return {attached && (player_esp || (gpu && chams)), gpu};
}
[[nodiscard]] constexpr std::uint32_t pose_sample_limit(bool limited,
    std::uint32_t fps) noexcept
{
    // 0 means "no schedule": the sampler then re-reads every skeleton once per
    // presented frame. CS2 presents at 300+ Hz, and each sample costs one
    // synchronous 4 KB ReadProcessMemory per enemy, so an uncapped loop must be
    // capped. 144 Hz is already smoother than the 2D pass it feeds.
    constexpr std::uint32_t uncapped_pose_rate = 144u;
    // A capped render loop samples at its own rate instead: never faster than
    // the frames that consume the poses, but still expressed as a rate so the
    // schedule keeps enforcing the period across frame-time spikes.
    return limited && fps > 0 && fps <= 240 ? fps : uncapped_pose_rate;
}
} // namespace render
