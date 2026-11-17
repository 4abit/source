#include <stdafx.hpp>

#include <features/misc/skybox.hpp>
#include <features/world/world_hooks.hpp>

#include <app/context.hpp>
#include <config/settings.hpp>
#include <core/assets/vpk.hpp>
#include <render/chams/renderer.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace features::misc {
namespace {

	constexpr int k_stage_net_update_postdataupdate_end = 7;

	[[nodiscard]] bool plausible( std::uintptr_t address )
	{
		return address >= 0x10000 && address < 0x00007FFFFFFFFFFFull;
	}

	[[nodiscard]] std::string wanted( )
	{
		const auto& cfg = config::visual_settings.m_ambience;
		if ( !cfg.enabled || !cfg.sky_tint_override )
			return {};
		const auto length = ::strnlen( cfg.skybox, sizeof( cfg.skybox ) );
		return std::string( cfg.skybox, length );
	}

	[[nodiscard]] std::string lower_copy( std::string text )
	{
		std::ranges::transform( text, text.begin( ), []( unsigned char c )
		{
			return static_cast<char>( std::tolower( c ) );
		} );
		return text;
	}

	// Companion materials (lighting, fog, the aztec _v1 variant) are not skies.
	// Putting one on the sky descriptor makes the renderer abort with an error material.
	[[nodiscard]] bool unsafe_sky( std::string_view path )
	{
		const auto lower = lower_copy( std::string{ path } );
		return lower.find( "_v1" ) != std::string::npos
			|| lower.find( "_lighting" ) != std::string::npos
			|| lower.find( "_fog" ) != std::string::npos;
	}

	[[nodiscard]] std::string first_texture( const std::vector<std::uint8_t>& bytes )
	{
		const std::string_view text( reinterpret_cast<const char*>( bytes.data( ) ), bytes.size( ) );
		std::size_t at = 0;
		while ( ( at = text.find( ".vtex", at ) ) != std::string_view::npos )
		{
			const auto start = text.rfind( "materials/", at );
			if ( start != std::string_view::npos && at - start < 200 )
			{
				std::string ref( text.substr( start, at + 5 - start ) );
				if ( std::ranges::none_of( ref, []( char c ) { return c == '\0' || c == '\n' || c == '"'; } )
					&& !unsafe_sky( ref ) )
					return ref;
			}
			at += 5;
		}
		return {};
	}

	[[nodiscard]] std::vector<std::uint8_t> material_bytes( const std::string& material )
	{
		auto compiled = material;
		if ( compiled.ends_with( ".vmat" ) )
			compiled += "_c";
		compiled = lower_copy( std::move( compiled ) );

		if ( chams::g_renderer.vpk( ).is_open( ) )
		{
			if ( const auto* entry = chams::g_renderer.vpk( ).find( compiled ) )
				return chams::g_renderer.vpk( ).read( *entry );
		}

		const auto pak = chams::vpk_archive::locate_cs2_pak( );
		if ( pak.empty( ) )
			return {};
		const auto file = std::filesystem::path{ pak }.parent_path( ) / std::filesystem::path{ compiled };
		std::ifstream stream( file, std::ios::binary );
		if ( !stream )
			return {};
		return { std::istreambuf_iterator<char>( stream ), std::istreambuf_iterator<char>( ) };
	}

	[[nodiscard]] std::string texture_of( const std::string& material )
	{
		if ( material.ends_with( ".vtex" ) )
			return unsafe_sky( material ) ? std::string{} : material;
		return first_texture( material_bytes( material ) );
	}

} // namespace

void skybox_t::prepare( const std::string& name )
{
	if ( name == this->m_requested && ( this->m_material.load( std::memory_order_acquire ) || this->m_gave_up ) )
		return;

	const auto now = static_cast<std::uint64_t>( ::GetTickCount64( ) );
	if ( name == this->m_requested && now < this->m_next_try_ms )
		return;
	this->m_requested = name;
	this->m_next_try_ms = now + 1000;
	this->m_gave_up = false;
	this->m_material.store( 0, std::memory_order_release );

	if ( name.empty( ) || unsafe_sky( name ) )
	{
		if ( !name.empty( ) )
			app::context().diagnostics.info( "[skybox] '{}' is not a sky material, left the map sky", name );
		this->m_gave_up = true;
		this->m_loaded.clear( );
		return;
	}

	const auto texture = texture_of( name );
	if ( texture.empty( ) )
	{
		// The pak may still be opening. Retry until it is, then give up.
		if ( chams::g_renderer.vpk( ).is_open( ) )
		{
			app::context().diagnostics.info( "[skybox] '{}' has no sky texture, left the map sky", name );
			this->m_gave_up = true;
		}
		return;
	}

	if ( !features::world::precache_texture( texture.c_str( ) ) )
		return;

	const auto source = std::format(
		R"VMAT(<!-- kv3 encoding:text:version{{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d}} format:generic:version{{7412167c-06e9-4698-aff2-e63eb59037e7}} -->
{{
	Shader = "sky.vfx"
	g_flBrightnessExposureBias = 0.0
	g_flRenderOnlyExposureBias = 0.0
	SkyTexture = resource:"{}"
	g_tSkyTexture = resource:"{}"
}}
)VMAT", texture, texture );

	std::uint32_t hash = 2166136261u;
	for ( const unsigned char c : texture )
		hash = ( hash ^ c ) * 16777619u;
	const auto material_name = std::format( "materials/skybox/nl_{:08x}.vmat", hash );
	const auto material = features::world::create_material( source.c_str( ), material_name.c_str( ) );
	if ( !material )
	{
		app::context().diagnostics.info( "[skybox] could not build a material for '{}'", texture );
		this->m_gave_up = true;
		return;
	}

	this->m_loaded = name;
	this->m_gave_up = true;
	this->m_material.store( material, std::memory_order_release );
	app::context().diagnostics.info( "[skybox] '{}' -> '{}' ({:#x})", name, texture, material );
}

void skybox_t::on_frame_stage( const int stage )
{
	if ( stage != k_stage_net_update_postdataupdate_end )
		return;
	this->prepare( wanted( ) );
}

skybox_t::draw_token skybox_t::on_draw_pre( std::uintptr_t meshes, int count )
{
	draw_token token{};
	const auto material = this->m_material.load( std::memory_order_acquire );
	if ( !material || !plausible( meshes ) || count <= 0 || count > 4096 )
		return token;

	const auto descriptor = *reinterpret_cast<const std::uintptr_t*>(
		meshes + static_cast<std::uintptr_t>( count ) * 0x70 - 0x58 );
	if ( !plausible( descriptor ) )
		return token;
	const auto binding = *reinterpret_cast<const std::uintptr_t*>( descriptor + 0xD0 );
	if ( !plausible( binding ) )
		return token;
	const auto original = *reinterpret_cast<const std::uintptr_t*>( binding );
	if ( !plausible( original ) || original == material )
		return token;

	*reinterpret_cast<std::uintptr_t*>( binding ) = material;
	token.binding = binding;
	token.original = original;
	return token;
}

void skybox_t::on_draw_post( const draw_token& token ) const
{
	if ( token.binding && token.original )
		*reinterpret_cast<std::uintptr_t*>( token.binding ) = token.original;
}

} // namespace features::misc
