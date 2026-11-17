#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include <core/math/vector.hpp>

namespace features::misc {

// Weather built from the game's own particle system: rain, snow and stars
// follow the camera, wind leans them and sways the foliage shaders, lightning
// is a chain of electric beams in the world with a flash of the sky and the
// scene lights and a rolling thunder behind it.
class weather_t
{
public:
	// Game thread (override view).
	void on_frame();
	// Render threads.
	void on_shader_param( void*& value, std::uint32_t hash ) const;

	[[nodiscard]] float flash_level() const { return m_flash.load( std::memory_order_relaxed ); }

private:
	struct layer
	{
		std::uint32_t effect{ 0xFFFFFFFFu };
	};

	struct thunder
	{
		float at{};
		float distance{};
	};

	void update_layer( layer& slot, bool wanted, const char* path, const foundation::vec3& origin,
		const foundation::vec3& color, bool lean );
	void release_all();
	void strike( const foundation::vec3& around, bool exact );
	void play_thunder( float distance ) const;

	layer m_rain{};
	layer m_snow{};
	layer m_stars{};
	std::uint64_t m_generation{};
	float m_next_strike{};
	float m_flash_start{ -100.0f };
	std::atomic<float> m_flash{ 0.0f };
	std::uint64_t m_last_kill_sequence{};
	std::vector<thunder> m_thunder{};
	std::uint32_t m_seed{ 0x9E3779B9u };
};

inline weather_t& weather()
{
	static weather_t value{};
	return value;
}

} // namespace features::misc
