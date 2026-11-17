#pragma once

#include <string_view>

namespace render::localization
{

enum class id : int
{
    en = 0,
    ru = 1,
    count
};

void set(id value);
[[nodiscard]] id current();
[[nodiscard]] const char *code(id value);

// Stub: the Neverlose menu owns its own localization; HUD text stays English.
[[nodiscard]] const char *tr(const char *english);
[[nodiscard]] std::string_view tr(std::string_view english);

} // namespace render::localization
