#pragma once

#include "menu/blur.h"

// OpenGL 3.2+ implementation of nl::blur (dual Kawase filter, like the Direct3D 11 one).
namespace nl::blur_gl {

using GetProc = void* (*)(const char* name);

// Call with a current context, e.g. init(reinterpret_cast<blur_gl::GetProc>(glfwGetProcAddress)).
bool init(GetProc get_proc);
void shutdown();
blur::Backend backend();

} // namespace nl::blur_gl
