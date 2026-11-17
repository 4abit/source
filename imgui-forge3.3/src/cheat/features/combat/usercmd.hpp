#pragma once

#include <core/math/vector.hpp>

#include <cstdint>

namespace features::combat::usercmd {

	// Input button bits as the client keeps them in CUserCmd / CInButtonState.
	inline constexpr std::uint64_t in_attack = 1ull << 0;
	inline constexpr std::uint64_t in_jump = 1ull << 1;
	inline constexpr std::uint64_t in_forward = 1ull << 3;
	inline constexpr std::uint64_t in_back = 1ull << 4;
	inline constexpr std::uint64_t in_use = 1ull << 5;
	inline constexpr std::uint64_t in_moveleft = 1ull << 9;
	inline constexpr std::uint64_t in_moveright = 1ull << 10;
	inline constexpr std::uint64_t in_attack2 = 1ull << 11;

	// Game-thread only: every call goes through client.dll functions or the
	// command that create_move just built.
	[[nodiscard]] bool resolve();
	[[nodiscard]] bool ready();

	[[nodiscard]] std::uintptr_t current( std::uintptr_t controller );

	[[nodiscard]] bool view_angles( foundation::vec3& out );
	[[nodiscard]] bool set_view_angles( const foundation::vec3& angles );

	// Buttons held in this command (CUserCmd::buttons, falling back to the
	// protobuf button state). Returns false when neither could be read.
	[[nodiscard]] bool buttons( std::uintptr_t cmd, std::uint64_t& out );

	// Presses or releases `mask` in this command, marks it as changed when it
	// differs from `was_down` (the state sent with the previous command) and
	// re-signs the move CRC.
	bool set_button( std::uintptr_t cmd, std::uint64_t mask, bool down, bool was_down );

	// Replaces the move of this command (view-relative, -1..1) with one analog
	// step and matching movement keys, then re-signs the move CRC. Ground only.
	bool set_move( std::uintptr_t cmd, float forward, float left );

	struct write_options
	{
		bool fix_movement{ true };
		bool attack{};
		// Rewrites the movement keys to match the rotated move (ground only:
		// in the air the keys only steer the strafe and must stay as pressed).
		bool grounded{};
	};

	// Replaces base view angles and every input history entry. Movement is
	// rebuilt for the new yaw: the client's subtick key steps are replaced by a
	// single analog step, the way the server expects a rotated move. The move
	// CRC is re-signed afterwards.
	bool write_angles( std::uintptr_t cmd, float pitch, float yaw, const write_options& options );

	// Lock-free mirror of the live view angles for the worker threads.
	[[nodiscard]] bool cached_view_angles( foundation::vec3& out );
	void publish_view_angles();

} // namespace features::combat::usercmd
