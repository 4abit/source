#pragma once

#include <cstdint>

// Forward declaration: the Neverlose menu config (plain data).
namespace nl { struct Config; }

namespace nl_cheat {

// Copies the NL menu config into the vesta runtime profiles and publishes the
// snapshot the feature workers read. Called from the menu thread; the profiles
// are only touched when the config actually changed.
void sync_config(const nl::Config& config);

} // namespace nl_cheat
