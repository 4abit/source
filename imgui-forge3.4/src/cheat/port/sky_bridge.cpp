// Skybox list and previews for Ambience > Override Sky.
//
// The sky materials come from pak01 (materials/skybox/*.vmat_c) and from the loose
// <csgo>/materials/skybox/custom folder. A material names its texture in its external
// reference block as plain text, so the first .vtex path in the file is the sky's cube
// map. That cube is decoded on a worker; the render thread uploads it and unwraps the
// horizon into a small RGBA strip with one full-screen pass, which then stays as the
// preview while the cube itself is dropped.

#include <stdafx.hpp>

#include <menu/map_preview.h>
#include <menu/sky_list.h>

#include <render/chams/renderer.hpp>
#include <render/chams/texture.hpp>

#include <d3dcompiler.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace nl::skies {
	namespace {

		constexpr const char* k_folder = "materials/skybox/";
		constexpr int k_preview_w = 384;
		constexpr int k_preview_h = 64;

		struct preview
		{
			ID3D11Texture2D* texture{};
			ID3D11ShaderResourceView* srv{};
			bool queued{};
			bool failed{};
		};

		struct record
		{
			Entry entry{};
			preview image{};
		};

		ID3D11Device* g_device{};
		ID3D11DeviceContext* g_context{};
		ID3D11VertexShader* g_vs{};
		ID3D11PixelShader* g_ps{};
		ID3D11SamplerState* g_sampler{};
		bool g_pipeline_failed{};

		std::vector<record> g_records{};
		std::vector<Entry> g_entries{};
		bool g_listed{};

		std::future<chams::texture_data> g_job{};
		std::size_t g_job_index{};

		constexpr const char* k_shader = R"(
TextureCube sky_cube : register(t0);
Texture2D   sky_flat : register(t1);
SamplerState linear_clamp : register(s0);
cbuffer params : register(b0) { float4 mode; };

struct vs_out { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

vs_out vs_main(uint id : SV_VertexID)
{
	vs_out o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.uv = uv;
	o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	return o;
}

float4 ps_main(vs_out i) : SV_Target
{
	// Horizontal: a full turn. Vertical: from 45 degrees up to 12 below the horizon,
	// the band the game shows when looking around.
	const float pi = 3.14159265;
	float azimuth = i.uv.x * 2.0 * pi;
	float elevation = lerp(radians(45.0), radians(-12.0), i.uv.y);
	float3 dir = float3(cos(elevation) * cos(azimuth), cos(elevation) * sin(azimuth), sin(elevation));

	float3 c;
	if (mode.x > 0.5)
		c = sky_cube.SampleLevel(linear_clamp, dir, 0).rgb;
	else
		c = sky_flat.SampleLevel(linear_clamp, float2(i.uv.x, lerp(0.25, 0.567, i.uv.y)), 0).rgb;

	if (mode.y > 0.5)
	{
		c = max(c, 0.0) * mode.z;
		c = 1.0 - exp(-c);
		c = pow(c, 1.0 / 2.2);
	}
	return float4(saturate(c), 1.0);
}
)";

		[[nodiscard]] std::filesystem::path csgo_folder( )
		{
			const auto pak = chams::vpk_archive::locate_cs2_pak( );
			return pak.empty( ) ? std::filesystem::path{} : std::filesystem::path{ pak }.parent_path( );
		}

		[[nodiscard]] std::filesystem::path custom_folder( )
		{
			const auto root = csgo_folder( );
			return root.empty( ) ? root : root / "materials" / "skybox" / "custom";
		}

		[[nodiscard]] std::vector<std::uint8_t> read_file( const std::filesystem::path& path )
		{
			std::ifstream file( path, std::ios::binary );
			if ( !file )
				return {};
			return { std::istreambuf_iterator<char>( file ), std::istreambuf_iterator<char>( ) };
		}

		// The game's file names, spelled the way the map list spells the maps.
		struct known_name
		{
			std::string_view stem;
			const char* name;
		};
		constexpr known_name k_known[]{
			{ "sky_de_annubis", "Anubis" },
			{ "sky_hr_aztec_02", "Aztec" },
			{ "sky_hr_aztec_02_v1", "Aztec Dusk" },
			{ "sky_ar_baggage_01", "Baggage" },
			{ "sky_csgo_cloudy01", "Cloudy" },
			{ "sky_de_dust2", "Dust 2" },
			{ "sky_de_mirage", "Mirage" },
			{ "sky_de_nuke", "Nuke" },
			{ "sky_de_overpass_01", "Overpass" },
			{ "sky_de_train03", "Train" },
			{ "sky_de_vertigo", "Vertigo" },
			{ "sky_overcast_01", "Overcast" },
			{ "sky_rain_night_01", "Rainy Night" },
			{ "sky_black", "Black" },
			{ "sky_cs15_daylight01_hdr", "Daylight" },
			{ "convoy_sky", "Convoy" },
			{ "cs_italy_s2_skybox_2", "Italy" },
			{ "cs_office_45_0", "Office" },
			{ "rush_cloudy_01a", "Rush" },
			{ "s2_de_inferno_sky01", "Inferno" },
		};

		// Lighting-only and fog companions, test scenes and the duplicate of Dust 2.
		constexpr std::string_view k_skipped[]{
			"_lighting", "_fog", "_v1", "skymodel", "light_test", "hr_dust_ground", "overpass_airplane", "dust2vmat",
		};

		[[nodiscard]] std::string pretty( std::string stem )
		{
			for ( const auto& known : k_known )
				if ( stem == known.stem )
					return known.name;

			for ( const std::string_view prefix : { "sky_", "de_", "cs_", "hr_", "ar_" } )
				if ( stem.starts_with( prefix ) )
					stem.erase( 0, prefix.size( ) );
			std::string out{};
			bool upper = true;
			for ( const char c : stem )
			{
				if ( c == '_' || c == '-' )
				{
					if ( !out.empty( ) && out.back( ) != ' ' )
						out += ' ';
					upper = true;
					continue;
				}
				out += upper ? static_cast<char>( std::toupper( static_cast<unsigned char>( c ) ) ) : c;
				upper = false;
			}
			while ( !out.empty( ) && out.back( ) == ' ' )
				out.pop_back( );
			return out.empty( ) ? "Sky" : out;
		}

		void build_list( )
		{
			std::vector<record> next{};
			next.push_back( record{ Entry{ "Default", "", false }, {} } );

			std::vector<record> found{};
			for ( const auto& path : chams::g_renderer.vpk( ).list_prefix( k_folder ) )
			{
				if ( !path.ends_with( ".vmat_c" ) )
					continue;
				if ( std::ranges::any_of( k_skipped, [ & ]( std::string_view skip ) { return path.find( skip ) != std::string::npos; } ) )
					continue;
				const auto slash = path.find_last_of( '/' );
				const auto stem = path.substr( slash + 1, path.size( ) - slash - 1 - 7 );
				found.push_back( record{ Entry{ pretty( stem ), path.substr( 0, path.size( ) - 2 ), false }, {} } );
			}

			std::error_code error{};
			const auto custom = custom_folder( );
			if ( !custom.empty( ) && std::filesystem::is_directory( custom, error ) )
			{
				const auto root = csgo_folder( );
				for ( const auto& item : std::filesystem::recursive_directory_iterator( custom, error ) )
				{
					if ( !item.is_regular_file( error ) || item.path( ).extension( ) != ".vmat_c" )
						continue;
					auto relative = std::filesystem::relative( item.path( ), root, error ).generic_string( );
					if ( error || relative.size( ) < 2 )
						continue;
					relative.resize( relative.size( ) - 2 );
					std::ranges::transform( relative, relative.begin( ), [ ]( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
					found.push_back( record{ Entry{ pretty( item.path( ).stem( ).string( ) ), relative, true }, {} } );
				}
			}

			std::ranges::sort( found, [ ]( const record& a, const record& b )
			{
				if ( a.entry.custom != b.entry.custom )
					return !a.entry.custom;
				return a.entry.name < b.entry.name;
			} );

			// Keep the previews of skies that survive the rescan.
			for ( auto& item : found )
			{
				const auto old = std::ranges::find_if( g_records, [ & ]( const record& r ) { return r.entry.material == item.entry.material; } );
				if ( old != g_records.end( ) )
				{
					item.image = old->image;
					old->image = {};
				}
				next.push_back( std::move( item ) );
			}
			for ( auto& old : g_records )
			{
				if ( old.image.srv )
					old.image.srv->Release( );
				if ( old.image.texture )
					old.image.texture->Release( );
			}

			g_records = std::move( next );
			g_entries.clear( );
			for ( const auto& item : g_records )
				g_entries.push_back( item.entry );
			g_listed = true;
		}

		// The first ".vtex" reference in the material: skies have exactly one.
		[[nodiscard]] std::vector<std::string> texture_refs( const std::vector<std::uint8_t>& bytes )
		{
			std::vector<std::string> refs{};
			const std::string_view text( reinterpret_cast<const char*>( bytes.data( ) ), bytes.size( ) );
			std::size_t at = 0;
			while ( ( at = text.find( ".vtex", at ) ) != std::string_view::npos )
			{
				const auto start = text.rfind( "materials/", at );
				if ( start != std::string_view::npos && at - start < 200 )
				{
					std::string ref( text.substr( start, at + 5 - start ) );
					if ( std::ranges::none_of( ref, [ ]( char c ) { return c == '\0' || c == '\n'; } ) )
						refs.push_back( std::move( ref ) );
				}
				at += 5;
			}
			return refs;
		}

		[[nodiscard]] chams::texture_data decode( Entry entry )
		{
			const auto root = csgo_folder( );
			auto& vpk = chams::g_renderer.vpk( );
			auto load = [ & ]( const std::string& path ) -> std::vector<std::uint8_t>
			{
				std::error_code error{};
				const auto loose = root / ( path + "_c" );
				if ( !root.empty( ) && std::filesystem::exists( loose, error ) )
					return read_file( loose );
				if ( const auto* item = vpk.find( path + "_c" ) )
					return vpk.read( *item );
				return {};
			};

			try
			{
				const auto material = load( entry.material );
				chams::texture_data flat{};
				for ( const auto& ref : texture_refs( material ) )
				{
					auto data = chams::parse_texture( load( ref ) );
					if ( !data.valid( ) )
						continue;
					if ( data.faces == 6 )
						return data;
					if ( !flat.valid( ) )
						flat = std::move( data );
				}
				return flat;
			}
			catch ( const std::exception& )
			{
				return {};
			}
		}

		[[nodiscard]] bool pipeline( )
		{
			if ( g_ps )
				return true;
			if ( g_pipeline_failed || !g_device )
				return false;
			g_pipeline_failed = true;

			ID3DBlob* vs_blob{};
			ID3DBlob* ps_blob{};
			ID3DBlob* errors{};
			const auto length = std::strlen( k_shader );
			if ( FAILED( D3DCompile( k_shader, length, nullptr, nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vs_blob, &errors ) ) )
			{
				if ( errors )
					errors->Release( );
				return false;
			}
			if ( FAILED( D3DCompile( k_shader, length, nullptr, nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &ps_blob, &errors ) ) )
			{
				if ( errors )
					errors->Release( );
				vs_blob->Release( );
				return false;
			}
			g_device->CreateVertexShader( vs_blob->GetBufferPointer( ), vs_blob->GetBufferSize( ), nullptr, &g_vs );
			g_device->CreatePixelShader( ps_blob->GetBufferPointer( ), ps_blob->GetBufferSize( ), nullptr, &g_ps );
			vs_blob->Release( );
			ps_blob->Release( );

			D3D11_SAMPLER_DESC sd{};
			sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			sd.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
			sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sd.MaxLOD = D3D11_FLOAT32_MAX;
			g_device->CreateSamplerState( &sd, &g_sampler );

			g_pipeline_failed = !g_vs || !g_ps || !g_sampler;
			return !g_pipeline_failed;
		}

		[[nodiscard]] bool hdr( std::uint32_t format )
		{
			return format == DXGI_FORMAT_BC6H_UF16 || format == DXGI_FORMAT_BC6H_SF16
				|| format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R32G32B32A32_FLOAT;
		}

		// Render thread: upload the decoded sky, unwrap it once, keep only the strip.
		void bake( preview& out, const chams::texture_data& data )
		{
			if ( !data.valid( ) || !pipeline( ) )
				return;

			const bool cube = data.faces == 6;
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = data.width;
			desc.Height = data.height;
			desc.MipLevels = data.mip_count;
			desc.ArraySize = cube ? 6 : 1;
			desc.Format = static_cast<DXGI_FORMAT>( data.dxgi_format );
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_IMMUTABLE;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;

			std::vector<D3D11_SUBRESOURCE_DATA> subresources( static_cast<std::size_t>( desc.ArraySize ) * data.mip_count );
			for ( std::uint32_t face = 0; face < desc.ArraySize; ++face )
			{
				for ( std::uint32_t level = 0; level < data.mip_count; ++level )
				{
					const auto w = std::max<std::uint32_t>( 1, data.width >> level );
					const auto blocks_x = std::max<std::uint32_t>( 1, ( w + 3 ) / 4 );
					auto& sub = subresources[ face * data.mip_count + level ];
					sub.pSysMem = data.mips[ level ].data( ) + face * data.face_bytes( level );
					sub.SysMemPitch = data.block_size != 0 ? blocks_x * data.block_size : w * data.bytes_per_pixel;
				}
			}

			ID3D11Texture2D* source{};
			ID3D11ShaderResourceView* source_srv{};
			if ( FAILED( g_device->CreateTexture2D( &desc, subresources.data( ), &source ) ) )
				return;
			D3D11_SHADER_RESOURCE_VIEW_DESC view{};
			view.Format = desc.Format;
			if ( cube )
			{
				view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
				view.TextureCube.MipLevels = desc.MipLevels;
			}
			else
			{
				view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				view.Texture2D.MipLevels = desc.MipLevels;
			}
			if ( FAILED( g_device->CreateShaderResourceView( source, &view, &source_srv ) ) )
			{
				source->Release( );
				return;
			}

			D3D11_TEXTURE2D_DESC target{};
			target.Width = k_preview_w;
			target.Height = k_preview_h;
			target.MipLevels = 1;
			target.ArraySize = 1;
			target.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			target.SampleDesc.Count = 1;
			target.Usage = D3D11_USAGE_DEFAULT;
			target.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			ID3D11RenderTargetView* rtv{};
			const float params[ 4 ]{ cube ? 1.0f : 0.0f, hdr( data.dxgi_format ) ? 1.0f : 0.0f, 1.6f, 0.0f };
			D3D11_BUFFER_DESC cb{};
			cb.ByteWidth = sizeof( params );
			cb.Usage = D3D11_USAGE_IMMUTABLE;
			cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			const D3D11_SUBRESOURCE_DATA cb_data{ params, 0, 0 };
			ID3D11Buffer* constants{};

			if ( SUCCEEDED( g_device->CreateTexture2D( &target, nullptr, &out.texture ) )
				&& SUCCEEDED( g_device->CreateRenderTargetView( out.texture, nullptr, &rtv ) )
				&& SUCCEEDED( g_device->CreateShaderResourceView( out.texture, nullptr, &out.srv ) )
				&& SUCCEEDED( g_device->CreateBuffer( &cb, &cb_data, &constants ) ) )
			{
				// ImGui is still recording: its render target and viewport go back after.
				ID3D11RenderTargetView* saved_rtv{};
				ID3D11DepthStencilView* saved_dsv{};
				g_context->OMGetRenderTargets( 1, &saved_rtv, &saved_dsv );
				UINT saved_count{ 1 };
				D3D11_VIEWPORT saved_viewport{};
				g_context->RSGetViewports( &saved_count, &saved_viewport );

				const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>( k_preview_w ), static_cast<float>( k_preview_h ), 0.0f, 1.0f };
				g_context->OMSetRenderTargets( 1, &rtv, nullptr );
				g_context->RSSetViewports( 1, &viewport );
				g_context->RSSetState( nullptr );
				g_context->OMSetBlendState( nullptr, nullptr, 0xFFFFFFFF );
				g_context->OMSetDepthStencilState( nullptr, 0 );
				g_context->IASetInputLayout( nullptr );
				g_context->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
				g_context->VSSetShader( g_vs, nullptr, 0 );
				g_context->PSSetShader( g_ps, nullptr, 0 );
				ID3D11ShaderResourceView* views[ 2 ]{ cube ? source_srv : nullptr, cube ? nullptr : source_srv };
				g_context->PSSetShaderResources( 0, 2, views );
				g_context->PSSetSamplers( 0, 1, &g_sampler );
				g_context->PSSetConstantBuffers( 0, 1, &constants );
				g_context->Draw( 3, 0 );

				ID3D11ShaderResourceView* none[ 2 ]{};
				g_context->PSSetShaderResources( 0, 2, none );
				g_context->OMSetRenderTargets( 1, &saved_rtv, saved_dsv );
				g_context->RSSetViewports( saved_count, &saved_viewport );
				if ( saved_rtv )
					saved_rtv->Release( );
				if ( saved_dsv )
					saved_dsv->Release( );
			}
			else
			{
				if ( out.srv )
					out.srv->Release( );
				if ( out.texture )
					out.texture->Release( );
				out.srv = nullptr;
				out.texture = nullptr;
			}

			if ( constants )
				constants->Release( );
			if ( rtv )
				rtv->Release( );
			source_srv->Release( );
			source->Release( );
		}

		void pump( )
		{
			if ( !chams::g_renderer.ensure_vpk( ) )
				return;
			if ( !g_listed )
				build_list( );

			if ( g_job.valid( ) )
			{
				if ( g_job.wait_for( std::chrono::seconds( 0 ) ) != std::future_status::ready )
					return;
				const auto data = g_job.get( );
				if ( g_job_index < g_records.size( ) )
				{
					auto& image = g_records[ g_job_index ].image;
					image.queued = false;
					bake( image, data );
					image.failed = image.srv == nullptr;
				}
			}

			for ( std::size_t index = 1; index < g_records.size( ); ++index )
			{
				auto& image = g_records[ index ].image;
				if ( !image.queued )
					continue;
				g_job_index = index;
				g_job = std::async( std::launch::async, decode, g_records[ index ].entry );
				break;
			}
		}

		void queue( std::size_t index )
		{
			if ( index == 0 || index >= g_records.size( ) )
				return;
			auto& image = g_records[ index ].image;
			if ( !image.srv && !image.failed )
				image.queued = true;
		}

		void wait_job( )
		{
			if ( g_job.valid( ) )
				g_job.wait( );
			g_job = {};
		}

	} // namespace

	void set_device( void* device, void* context )
	{
		g_device = static_cast<ID3D11Device*>( device );
		g_context = static_cast<ID3D11DeviceContext*>( context );
	}

	void release( )
	{
		wait_job( );
		for ( auto& item : g_records )
		{
			if ( item.image.srv )
				item.image.srv->Release( );
			if ( item.image.texture )
				item.image.texture->Release( );
			item.image = {};
		}
		for ( IUnknown* object : { static_cast<IUnknown*>( g_vs ), static_cast<IUnknown*>( g_ps ), static_cast<IUnknown*>( g_sampler ) } )
			if ( object )
				object->Release( );
		g_vs = nullptr;
		g_ps = nullptr;
		g_sampler = nullptr;
		g_device = nullptr;
		g_context = nullptr;
	}

	const std::vector<Entry>& list( )
	{
		pump( );
		if ( g_entries.empty( ) )
			g_entries.push_back( Entry{ "Default", "", false } );
		return g_entries;
	}

	bool ready( )
	{
		pump( );
		return g_listed;
	}

	void refresh( )
	{
		if ( !chams::g_renderer.vpk_ready( ) )
			return;
		// The worker holds an index into the old list.
		wait_job( );
		for ( auto& item : g_records )
		{
			item.image.queued = false;
			if ( item.entry.custom )
				item.image.failed = false;
		}
		build_list( );
	}

	void open_folder( )
	{
		const auto folder = custom_folder( );
		if ( folder.empty( ) )
			return;
		std::error_code error{};
		std::filesystem::create_directories( folder, error );
		::ShellExecuteW( nullptr, L"open", folder.wstring( ).c_str( ), nullptr, nullptr, SW_SHOWNORMAL );
	}

	void warm( )
	{
		pump( );
		for ( std::size_t index = 1; index < g_records.size( ); ++index )
			queue( index );
	}

	int pending( )
	{
		pump( );
		int count = 0;
		for ( const auto& item : g_records )
			count += item.image.queued ? 1 : 0;
		return count + ( g_job.valid( ) ? 1 : 0 );
	}

	bool draw( ImDrawList* dl, int index, const ImVec2& min, const ImVec2& max, ImU32 tint,
		float rounding, ImDrawFlags corners, float fade )
	{
		pump( );
		if ( !dl || index <= 0 || static_cast<std::size_t>( index ) >= g_records.size( ) )
			return false;
		const auto& image = g_records[ static_cast<std::size_t>( index ) ].image;
		if ( !image.srv )
		{
			queue( static_cast<std::size_t>( index ) );
			return false;
		}

		const auto box_w = max.x - min.x;
		const auto box_h = max.y - min.y;
		if ( box_w <= 0.0f || box_h <= 0.0f )
			return false;
		// Cover fit, cropping the strip's sides when the box is narrower.
		const auto image_aspect = static_cast<float>( k_preview_w ) / static_cast<float>( k_preview_h );
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
		dl->AddImageRounded( reinterpret_cast<ImTextureID>( image.srv ), min, max, uv_min, uv_max, tint, rounding, corners );
		if ( fade > 0.0f )
			nl::maps::fade_in_x( dl, first, min.x, min.x + box_w * fade );
		return true;
	}

	int find( const char* material )
	{
		if ( !material || !*material )
			return 0;
		const auto& entries = list( );
		for ( std::size_t index = 1; index < entries.size( ); ++index )
			if ( entries[ index ].material == material )
				return static_cast<int>( index );
		return 0;
	}

} // namespace nl::skies
