#include <render/menu/localization.hpp>

namespace render::localization
{

void set(id)
{
}

id current()
{
	return id::en;
}

const char* code(id value)
{
	return value == id::ru ? "RU" : "EN";
}

const char* tr(const char* english)
{
	return english;
}

std::string_view tr(std::string_view english)
{
	return english;
}

} // namespace render::localization
