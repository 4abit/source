#pragma once

#include <d3d11.h>

#include "menu/blur.h"

// Direct3D 11 implementation of nl::blur: a copy of the back buffer is blurred with a dual Kawase
// filter (downsampled 4 times, then upsampled back to half resolution).
namespace nl::blur_dx11 {

bool init(ID3D11Device* device, ID3D11DeviceContext* context);
void shutdown();
// Call before ImGui::NewFrame() with the back buffer size. Recreates the textures when it changed
// and returns the backend to hand to nl::blur::set_backend().
blur::Backend prepare(int width, int height);

} // namespace nl::blur_dx11
