#pragma once

#include "assets.h"

// The generated image is `const` at namespace scope, which would give it internal linkage.
// This declaration is forced in before that definition so the loader can see the symbol.
namespace nl::assets {
extern const Blob cs2_rgba;
}
