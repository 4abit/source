// Map previews read out of the installed game.
//
// The icons are panorama textures inside pak01
// (panorama/images/map_icons/screenshots/<size>/<map>_png.vtex_c). Each one is a compiled
// resource that keeps the source PNG right after its DATA block, so the PNG is cut out and
// decoded with stb on a worker; the render thread only turns the pixels into a registered
// ImGui texture. The archive is the one the chams already keep open.

#include <stdafx.hpp>

#include <menu/map_preview.h>

#include "imgui_internal.h"

#include <render/chams/renderer.hpp>
#include <render/chams/texture.hpp>

#include <algorithm>
#include <deque>
#include <future>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// A private copy of the decoder: the menu's images.cpp compiles stb as static, so its
// symbols are not visible here and this translation unit needs its own.
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace nl::maps {
	namespace {

		struct pixels
		{
			std::vector<unsigned char> rgba{};
			int width{};
			int height{};
		};

		struct preview
		{
			ImTextureData* texture{};
			int width{};
			int height{};
			bool failed{};
			bool queued{};
		};

		std::mutex g_mutex{};
		std::unordered_map<std::string, preview> g_cache{};
		std::deque<std::string> g_queue{};
		std::future<pixels> g_job{};
		std::string g_job_map{};

		// 640x360 is already sharper than any card the menu draws; 720p is the fallback
		// for a map that only ships the larger set.
		constexpr const char* k_folders[]{
			"panorama/images/map_icons/screenshots/360p",
			"panorama/images/map_icons/screenshots/720p",
		};

		pixels decode( const std::string& map_id )
		{
			pixels out{};
			auto& vpk = chams::g_renderer.vpk( );
			for ( const char* folder : k_folders )
			{
				const auto* item = vpk.find( std::string{ folder } + '/' + map_id + "_png.vtex_c" );
				if ( !item )
					continue;
				const auto png = chams::embedded_png( vpk.read( *item ) );
				if ( png.empty( ) )
					continue;
				int channels = 0;
				unsigned char* rgba = stbi_load_from_memory( png.data( ), static_cast<int>( png.size( ) ),
					&out.width, &out.height, &channels, 4 );
				if ( !rgba )
					continue;
				out.rgba.assign( rgba, rgba + static_cast<std::size_t>( out.width ) * out.height * 4 );
				stbi_image_free( rgba );
				// The icons are opaque; a stray transparent pixel must not punch a hole
				// into the card behind it.
				for ( std::size_t index = 3; index < out.rgba.size( ); index += 4 )
					out.rgba[ index ] = 255;
				break;
			}
			return out;
		}

		ImTextureData* upload( const pixels& source )
		{
			ImTextureData* texture = IM_NEW( ImTextureData )( );
			texture->Create( ImTextureFormat_RGBA32, source.width, source.height );
			texture->UseColors = true;
			std::memcpy( texture->GetPixelsAt( 0, 0 ), source.rgba.data( ), source.rgba.size( ) );
			ImGui::RegisterUserTexture( texture );
			return texture;
		}

		// Render thread: one icon decodes at a time, the finished one is uploaded here.
		void pump( )
		{
			if ( !chams::g_renderer.ensure_vpk( ) )
				return;

			if ( g_job.valid( ) )
			{
				if ( g_job.wait_for( std::chrono::seconds( 0 ) ) != std::future_status::ready )
					return;
				const auto result = g_job.get( );
				auto& entry = g_cache[ g_job_map ];
				entry.queued = false;
				if ( result.width > 0 && result.height > 0 )
				{
					entry.texture = upload( result );
					entry.width = result.width;
					entry.height = result.height;
				}
				entry.failed = entry.texture == nullptr;
			}

			if ( g_queue.empty( ) )
				return;
			g_job_map = g_queue.front( );
			g_queue.pop_front( );
			g_job = std::async( std::launch::async, decode, g_job_map );
		}

		const preview* lookup( const char* map_id )
		{
			if ( !map_id || !*map_id )
				return nullptr;
			const std::scoped_lock lock{ g_mutex };
			pump( );
			auto [ found, inserted ] = g_cache.try_emplace( map_id );
			if ( inserted )
			{
				found->second.queued = true;
				g_queue.emplace_back( map_id );
			}
			const auto& entry = found->second;
			return entry.texture ? &entry : nullptr;
		}

	} // namespace

	bool available( )
	{
		return chams::g_renderer.vpk_ready( );
	}

	void request( const char* map_id )
	{
		static_cast<void>( lookup( map_id ) );
	}

	int pending( )
	{
		const std::scoped_lock lock{ g_mutex };
		pump( );
		return static_cast<int>( std::ranges::count_if( g_cache, [ ]( const auto& item ) { return item.second.queued; } ) );
	}

	ImTextureID texture( const char* map_id )
	{
		const preview* found = lookup( map_id );
		return found ? found->texture->GetTexID( ) : static_cast<ImTextureID>( 0 );
	}

	bool texture_size( const char* map_id, int* width, int* height )
	{
		const preview* found = lookup( map_id );
		if ( !found )
			return false;
		if ( width )
			*width = found->width;
		if ( height )
			*height = found->height;
		return true;
	}

	bool draw( ImDrawList* dl, const char* map_id, const ImVec2& min, const ImVec2& max, ImU32 tint,
		float rounding, ImDrawFlags corners, float fade )
	{
		const preview* found = lookup( map_id );
		if ( !found || !dl )
			return false;

		const auto box_w = max.x - min.x;
		const auto box_h = max.y - min.y;
		if ( box_w <= 0.0f || box_h <= 0.0f || found->width <= 0 || found->height <= 0 )
			return false;

		// Cover fit through the UVs: the picture fills the box and the overflow is
		// cropped evenly on both sides.
		const auto image_aspect = static_cast<float>( found->width ) / static_cast<float>( found->height );
		const auto box_aspect = box_w / box_h;
		ImVec2 uv_min( 0.0f, 0.0f ), uv_max( 1.0f, 1.0f );
		if ( box_aspect > image_aspect )
		{
			const auto visible = image_aspect / box_aspect;
			uv_min.y = ( 1.0f - visible ) * 0.5f;
			uv_max.y = 1.0f - uv_min.y;
		}
		else
		{
			const auto visible = box_aspect / image_aspect;
			uv_min.x = ( 1.0f - visible ) * 0.5f;
			uv_max.x = 1.0f - uv_min.x;
		}
		const int first = dl->VtxBuffer.Size;
		dl->AddImageRounded( found->texture->GetTexRef( ), min, max, uv_min, uv_max, tint, rounding, corners );
		if ( fade > 0.0f )
			fade_in_x( dl, first, min.x, min.x + box_w * fade );
		return true;
	}

} // namespace nl::maps
