#pragma once

#include <core/assets/resource.hpp>
#include <core/assets/vpk.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace chams {

	struct texture_data
	{
		std::uint32_t width{};
		std::uint32_t height{};
		std::uint32_t mip_count{};
		std::uint32_t dxgi_format{}; // DXGI_FORMAT, already translated
		std::uint32_t block_size{};  // bytes per 4x4 block, 0 for uncompressed formats
		std::uint32_t bytes_per_pixel{}; // uncompressed formats only
		std::uint32_t faces{ 1 };        // 6 for a cube map

		// Mip 0 first, matching the order D3D11's subresource array expects --
		// i.e. already reversed relative to how the file stores them. A cube map
		// keeps its six faces back to back inside each mip, +X -X +Y -Y +Z -Z.
		std::vector<std::vector<std::uint8_t>> mips{};

		[[nodiscard]] bool valid( ) const { return !mips.empty( ) && width > 0 && height > 0; }
		[[nodiscard]] std::size_t face_bytes( std::uint32_t level ) const
		{
			return faces ? mips[ level ].size( ) / faces : 0;
		}
	};

	[[nodiscard]] texture_data load_texture( vpk_archive& vpk, const std::string& archive_path );
	// Same, for a compiled .vtex_c already in memory (a loose file of the game folder).
	[[nodiscard]] texture_data parse_texture( const std::vector<std::uint8_t>& file );

	// Panorama pictures are compiled with the source PNG stored as-is. Returns the PNG
	// bytes, or nothing for any other kind of texture.
	[[nodiscard]] std::vector<std::uint8_t> embedded_png( const std::vector<std::uint8_t>& file );

} // namespace chams
