// Non-Windows stub of the enemy model preview.
//
// The real implementation lives in src/cheat/port/esp_model_bridge.cpp, which is
// only part of the Windows build (the cheat library itself is Windows-only). This
// translation unit keeps the menu's call sites compiling on the GLFW/OpenGL
// preview build, where the tab keeps using the shipped picture.

#include "menu/esp_model.h"

#if !defined(_WIN32)

namespace nl::esp_model {

void set_device(void*, void*) {}
void release() {}

bool available() { return false; }

bool frame_ready() { return false; }

frame render(float, float, float, bool) { return frame{}; }

int skeleton_segments() { return 0; }

bool skeleton_segment(int, float*, float*, float*, float*) { return false; }

bool part_bounds(part, float*, float*, float*, float*) { return false; }

} // namespace nl::esp_model

#endif
