#pragma once

#include <cstdint>
#include <imgui.h>

namespace features::visuals {

	// Steam avatars for the ESP nickname.
	//
	// The picture is fetched from the public community profile XML (no API key needed),
	// decoded on a worker thread and uploaded as an ImGui texture on the render thread.
	// Results are cached per SteamID, misses included, so a profile that has no public
	// avatar is only asked about once.
	class avatar_cache
	{
	public:
		void start( );
		void stop( );

		// Texture for a player, or 0 while it is still being fetched. Only call from the
		// render thread: creating a texture registers it with ImGui.
		[[nodiscard]] ImTextureID texture( std::uint64_t steamid );

		// Square size the picture should be drawn at, in pixels at the reference scale.
		[[nodiscard]] static float draw_size( float text_height );

	private:
		struct impl;
		impl* m_impl{};
	};

	inline avatar_cache& avatars( ) { static avatar_cache value{}; return value; }

} // namespace features::visuals
