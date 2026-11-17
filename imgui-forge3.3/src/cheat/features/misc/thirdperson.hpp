#pragma once

#include <cstdint>

namespace features::misc {

	// Third person camera written into the view setup right after the game
	// built it: the boom grows out of the eye when switched on and back in when
	// switched off, follows the live view angles and folds in against walls.
	// While dead the game's own chase camera is used.
	class thirdperson_t
	{
	public:
		// Worker thread: publishes the settings and drives the chase camera.
		void tick( );
		// Game thread.
		void on_view( std::uintptr_t view_setup );

	private:
		float m_boom{ 0.0f };
		float m_reach{ -1.0f };
		double m_last_frame{ 0.0 };
		bool m_observer_forced{ false };
		std::uint8_t m_saved_observer_mode{ 2 };
	};

	inline thirdperson_t& thirdperson( ) { static thirdperson_t value{}; return value; }

} // namespace features::misc
