#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace features::misc {

	// Skybox changer. The picked sky is a sky.vfx material built from the cube
	// texture inside the chosen .vmat. It is swapped onto the sky descriptor for
	// the draw and put back afterwards, so changing the selection takes effect on
	// the next frame and a material that failed to load is never left on the sky.
	class skybox_t
	{
	public:
		void on_frame_stage( int stage );

		struct draw_token
		{
			std::uintptr_t binding{};
			std::uintptr_t original{};
		};

		[[nodiscard]] draw_token on_draw_pre( std::uintptr_t meshes, int count );
		void on_draw_post( const draw_token& token ) const;

	private:
		void prepare( const std::string& name );

		std::string m_requested{};
		std::string m_loaded{};
		std::atomic<std::uintptr_t> m_material{ 0 };
		std::uint64_t m_next_try_ms{};
		bool m_gave_up{};
	};

	inline skybox_t& skybox( )
	{
		static skybox_t value{};
		return value;
	}

} // namespace features::misc
