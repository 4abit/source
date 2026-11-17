#pragma once

#include <cstdint>
#include <unordered_map>

namespace features::misc {

	// World atmosphere. Lighting, sky, fog and post processing are changed in
	// the scene hooks right before the engine consumes them and put back after,
	// so nothing sticks once a toggle goes off. The removals are convars whose
	// original values are restored the moment they are switched back.
	class ambience_t
	{
	public:
		// Present thread: convar removals.
		void apply( );

		struct skybox_token
		{
			std::uintptr_t descriptor{};
			float color[ 3 ]{};
		};

		[[nodiscard]] skybox_token on_skybox_pre( std::uintptr_t meshes, int count ) const;
		void on_skybox_post( const skybox_token& token ) const;
		[[nodiscard]] bool on_light_pre( std::uintptr_t object, float saved[ 3 ] ) const;
		void on_light_post( std::uintptr_t object, const float saved[ 3 ] ) const;
		void on_scene_objects( std::uintptr_t batch, int count ) const;
		// Aggregate world geometry (walls, floors, static props). Its color lives in
		// the light-data queue, not on the mesh the per-object tint writes.
		void on_aggregate( std::uintptr_t object_array ) const;
		[[nodiscard]] bool on_fog( void* output, int* mode ) const;
		void on_shader_param( void*& value, std::uint32_t hash ) const;

	private:
		struct removal_state
		{
			bool active{};
			int saved{};
		};

		// Night mode on the map's own geometry. Walls and floors are aggregate scene
		// objects whose tint is baked into GPU data when the map loads, so they are
		// darkened through the exposure of every post-processing volume instead.
		struct exposure_state
		{
			float min{};
			float max{};
			float compensation{};
			bool control{};
		};

		struct tonemap_state
		{
			float min{};
			float max{};
		};

		void apply_exposure( );
		void apply_tonemap( bool night, float scale );

		removal_state m_removals[ 6 ]{};
		std::unordered_map<std::uintptr_t, exposure_state> m_exposure{};
		std::unordered_map<std::uintptr_t, tonemap_state> m_tonemap{};
		float m_exposure_scale{ 1.0f };
	};

	inline ambience_t& ambience( )
	{
		static ambience_t value{};
		return value;
	}

} // namespace features::misc
