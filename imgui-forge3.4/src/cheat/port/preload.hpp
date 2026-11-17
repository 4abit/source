#pragma once

namespace nl {
struct Config;
}

// Everything the menu would otherwise load the first time it is looked at: the game archive,
// the map and sky previews and the inventory's 3D models. Run while the loading window is up
// so the menu opens with all of it in place.
namespace nl_cheat::preload {

struct status {
    float progress = 0.0f;
    const char* text = "";
    bool done = false;
};

// Render thread, once per frame until `done`. Every stage has a time cap: whatever is not
// ready by then keeps loading lazily the way it did before.
status pump(const nl::Config& config);

} // namespace nl_cheat::preload
