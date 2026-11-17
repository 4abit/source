#pragma once

#include <core/math/vector.hpp>

#include <cstdint>
#include <string>

namespace features::world {

	// Installs the scene, fog, shader-parameter, view and file hooks once; cheap
	// to call every frame.
	void install_hooks();
	// One status line: which hooks were placed and how often they ran.
	[[nodiscard]] std::string report();

	// Game particle system, usable from the game thread (override view).
	namespace particles {
		constexpr std::uint32_t invalid = 0xFFFFFFFFu;

		[[nodiscard]] bool ready();
		[[nodiscard]] std::uint32_t create( const char* path );
		void control_point( std::uint32_t effect, int point, const foundation::vec3& value );
		void transform( std::uint32_t effect, int point, const foundation::vec3& origin, const float quaternion[ 4 ] );
		void destroy( std::uint32_t effect );

		// Changes whenever the particle manager or the round restarted, which
		// drops every effect index we hold.
		[[nodiscard]] std::uint64_t generation();
	}

	// Calls the unhooked shader parameter setters for the fog override.
	void set_shader_float( void* map, std::uint32_t hash, const void* value );
	void set_shader_int( void* map, std::uint32_t hash, int value );

	// Loads a resource (a sky .vtex) and builds a material from KV3 text. Both return
	// 0 when the game cannot resident the resource, which is how a bad sky is refused
	// before it ever reaches the renderer.
	[[nodiscard]] std::uintptr_t precache_texture( const char* path );
	[[nodiscard]] std::uintptr_t create_material( const char* vmat, const char* name );

} // namespace features::world
