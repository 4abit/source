#pragma once

#include <d3d11.h>

#include <cstdint>

namespace hook_hud {

// Boots the in-game presentation: loads the HUD font set into the shared
// overlay slot, builds the ImGui atlas and device objects, and initializes
// the chams renderer. Called once from the game's present hook.
void initialize( ID3D11Device* device, ID3D11DeviceContext* context,
	std::uint32_t reference_width, std::uint32_t reference_height,
	float dpi_scale );

// Draws one in-game frame: pose sampling, chams, player/world ESP and the
// HUD panels. The caller owns the ImGui frame (NewFrame/Render).
void render_frame( ID3D11RenderTargetView* rtv, std::uint32_t width,
	std::uint32_t height, bool menu_open );

// Frames presented since the hook came up. Runtime diagnostics wait for this so
// they never sample state that the game has not created yet.
[[nodiscard]] std::uint64_t frames_rendered( );

} // namespace hook_hud
