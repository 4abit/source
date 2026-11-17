#pragma once

#include <render/draw.hpp>
#include <core/math/vector.hpp>

#include <cstdint>
#include <optional>

namespace features::combat {

	// What the active aimbot is about to shoot at, for the ESP "Hitboxes"
	// element: the target, the aim point and the estimated chance to hit it.
	struct aim_preview
	{
		std::uintptr_t pawn{};
		foundation::vec3 point{};
		float hit_chance{ -1.0f }; // percent; negative when it was not estimated
		float damage{};
		bool ready{};              // the bot would fire at it right now
	};

	void publish_aim_preview( const aim_preview& value );
	void clear_aim_preview();
	// The last published preview while it is fresh (a stalled aim thread must
	// not leave a marker hanging on a player).
	[[nodiscard]] std::optional<aim_preview> latest_aim_preview();

	class ragebot_t
	{
	public:
		void tick();
		void reset();
		void on_render( zdraw::draw_list& draw_list ) const;
		[[nodiscard]] std::string report( ) const;
	};

	inline ragebot_t& ragebot() { static ragebot_t value{}; return value; }

	// The create_move hook is shared by every feature that edits the command;
	// it no longer waits for the ragebot to be switched on.
	void ensure_command_hook();
	[[nodiscard]] bool command_hook_active();

	// Bunny hop done inside the command: while armed, jump is released for
	// every airborne command and pressed on the first grounded one. `hold`
	// presses it even when the jump key itself is not held (separate bind).
	void set_bhop_wish( bool armed, bool hold );

} // namespace features::combat
