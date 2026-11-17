#include <stdafx.hpp>
#include <render/chams/material.hpp>
#include <core/assets/kv3.hpp>
#include <core/assets/resource.hpp>

#include <algorithm>
#include <cctype>
#include <string_view>
#include <unordered_set>

namespace chams {

	namespace {

		[[nodiscard]] const kv3::object* find_param( const kv3::object* array, const char* name )
		{
			if ( !array )
			{
				return nullptr;
			}

			for ( std::size_t i = 0; i < array->size( ); ++i )
			{
				const auto* item = array->at( i );
				const auto* name_kv = item ? item->find( "m_name" ) : nullptr;
				if ( name_kv && name_kv->as_string( ) == name )
				{
					return item;
				}
			}

			return nullptr;
		}

		[[nodiscard]] std::string texture_path( const kv3::object* params, const char* slot )
		{
			const auto* param = find_param( params, slot );
			const auto* value = param ? param->find( "m_pValue" ) : nullptr;
			return value ? value->as_string( ) : std::string{};
		}

		void read_vector( const kv3::object* params, const char* name, float* out, int count )
		{
			const auto* param = find_param( params, name );
			const auto* value = param ? param->find( "m_value" ) : nullptr;
			if ( !value )
			{
				return;
			}

			float raw[ 4 ]{};
			if ( count == 4 && value->as_float4( raw ) )
			{
				for ( int i = 0; i < 4; ++i ) out[ i ] = raw[ i ];
			}
			else if ( count == 2 )
			{
				// Stored as a float4 even when only xy is meaningful.
				if ( value->as_float4( raw ) )
				{
					out[ 0 ] = raw[ 0 ];
					out[ 1 ] = raw[ 1 ];
				}
			}
		}

		void read_float( const kv3::object* params, const char* name, float& out )
		{
			const auto* param = find_param( params, name );
			if ( const auto* value = param ? param->find( "m_flValue" ) : nullptr )
			{
				out = static_cast< float >( value->as_double( ) );
			}
		}

		void read_int( const kv3::object* params, const char* name, int& out )
		{
			const auto* param = find_param( params, name );
			if ( const auto* value = param ? param->find( "m_nValue" ) : nullptr )
			{
				out = static_cast< int >( value->as_int( ) );
			}
		}

		// Paint kits flagged use_legacy_model in items_game, by name. Read once: the
		// file is large and the flag never changes within a session.
		[[nodiscard]] bool paint_kit_uses_legacy_model( vpk_archive& vpk, const std::string& kit )
		{
			static std::unordered_set<std::string> legacy{};
			static bool loaded = false;
			if ( !loaded )
			{
				loaded = true;
				if ( const auto* entry = vpk.find( "scripts/items/items_game.txt" ) )
				{
					const auto bytes = vpk.read( *entry );
					const std::string_view text{ reinterpret_cast< const char* >( bytes.data( ) ), bytes.size( ) };

					// Quoted tokens in order; the flag follows the "name" of its kit.
					std::string current{};
					std::string_view previous{};
					for ( std::size_t at = text.find( '"' ); at != std::string_view::npos; )
					{
						const auto end = text.find( '"', at + 1 );
						if ( end == std::string_view::npos )
							break;
						const auto token = text.substr( at + 1, end - at - 1 );
						if ( previous == "name" )
							current.assign( token );
						else if ( previous == "use_legacy_model" && token == "1" && !current.empty( ) )
							legacy.insert( current );
						previous = token;
						at = text.find( '"', end + 1 );
					}
				}
			}

			std::string key = kit;
			std::transform( key.begin( ), key.end( ), key.begin( ),
				[ ]( unsigned char c ) { return static_cast< char >( std::tolower( c ) ); } );
			return legacy.contains( key );
		}

	} // namespace

	material_data load_material( vpk_archive& vpk, const std::string& archive_path )
	{
		material_data result{};

		try
		{
			auto path = archive_path;
			if ( path.ends_with( ".vmat" ) )
			{
				path += "_c";
			}

			const auto* entry = vpk.find( path );
			if ( !entry )
			{
				return result;
			}

			resource res{};
			if ( !res.parse( vpk.read( *entry ) ) )
			{
				return result;
			}

			const auto* data_block = res.find( "DATA" );
			if ( !data_block )
			{
				return result;
			}

			const auto doc = kv3::decode( res.bytes( *data_block ), data_block->size );
			const auto& root = doc.root;

			if ( const auto* shader = root.find( "m_shaderName" ) )
			{
				result.shader = shader->as_string( );
			}

			const auto* textures = root.find( "m_textureParams" );
			result.color = texture_path( textures, "g_tColor" );
			result.normal = texture_path( textures, "g_tNormal" );
			result.metalness = texture_path( textures, "g_tMetalness" );
			result.ambient_occlusion = texture_path( textures, "g_tAmbientOcclusion" );
			result.gloss = texture_path( textures, "g_tAnisoGloss" );
			result.pattern = texture_path( textures, "g_tPattern" );
			result.masks = texture_path( textures, "g_tMasks" );
			result.position = texture_path( textures, "g_tPosition" );
			result.layer_mask = texture_path( textures, "g_tLayerMask" );
			result.surface = texture_path( textures, "g_tSurface" );
			result.tint_id = texture_path( textures, "g_tTintId" );
			result.wear = texture_path( textures, "g_tWear" );
			result.grunge = texture_path( textures, "g_tGrunge" );

			const auto* vectors = root.find( "m_vectorParams" );
			read_vector( vectors, "g_vTexCoordScale", result.uv_scale, 2 );
			read_vector( vectors, "g_vTexCoordOffset", result.uv_offset, 2 );
			read_vector( vectors, "g_vColorTint", result.color_tint, 4 );
			read_vector( vectors, "g_vPatternTexCoordOffset", result.pattern_uv_offset, 2 );
			read_vector( vectors, "g_fPatternTexCoordOffset", result.pattern_uv_offset, 2 );
			static constexpr const char* k_glove_tint[ 8 ]{ "g_vColorTint1", "g_vColorTint2",
				"g_vColorTint3", "g_vColorTint4", "g_vColorTint5", "g_vColorTint6",
				"g_vColorTint7", "g_vColorTint8" };
			for ( int i = 0; i < 8; ++i )
			{
				float value[ 4 ]{};
				read_vector( vectors, k_glove_tint[ i ], value, 4 );
				for ( int c = 0; c < 3; ++c )
					result.glove_tint[ i ][ c ] = value[ c ];
			}
			float palette_index[ 4 ]{};
			read_vector( vectors, "g_vPatternPaletteIndices", palette_index, 4 );
			for ( int i = 0; i < 4; ++i )
				result.glove_pattern_palette[ i ] = palette_index[ i ];
			static constexpr const char* k_id_color[ 8 ]{ "g_vId1Color", "g_vId2Color",
				"g_vId3Color", "g_vId4Color", "g_vId5Color", "g_vId6Color",
				"g_vId7Color", "g_vId8Color" };
			for ( int i = 0; i < 8; ++i )
			{
				float value[ 4 ]{ 1.0f, 1.0f, 1.0f, 1.0f };
				read_vector( vectors, k_id_color[ i ], value, 4 );
				// read_vector leaves the buffer untouched when the param is absent,
				// so seed it white first.
				if ( !find_param( vectors, k_id_color[ i ] ) )
				{
					value[ 0 ] = value[ 1 ] = value[ 2 ] = 1.0f;
				}
				for ( int c = 0; c < 3; ++c )
					result.id_color[ i ][ c ] = value[ c ];
			}
			float scale_xy[ 4 ]{ 1.0f, 1.0f, 0.0f, 0.0f };
			read_vector( vectors, "g_vPatternTexCoordScale", scale_xy, 4 );
			if ( find_param( vectors, "g_vPatternTexCoordScale" ) )
			{
				result.pattern_scale_xy[ 0 ] = scale_xy[ 0 ] > 0.0f ? scale_xy[ 0 ] : 1.0f;
				result.pattern_scale_xy[ 1 ] = scale_xy[ 1 ] > 0.0f ? scale_xy[ 1 ] : 1.0f;
			}
			float cloth[ 4 ]{ 1.0f, 1.0f, 1.0f, 1.0f };
			read_vector( vectors, "g_vSurfaceColorTint1", cloth, 4 );
			if ( find_param( vectors, "g_vSurfaceColorTint1" ) )
			{
				for ( int c = 0; c < 3; ++c )
					result.surface_tint[ c ] = cloth[ c ];
			}

			const auto* floats = root.find( "m_floatParams" );
			read_float( floats, "g_flPatternTexCoordScale", result.pattern_scale );
			read_float( floats, "g_fPatternTexCoordScale", result.pattern_scale );
			if ( result.pattern_scale == 0.0f )
			{
				result.pattern_scale = 1.0f;
			}
			read_float( floats, "g_flPatternTexCoordRotation", result.pattern_rotation );
			read_float( floats, "g_fPatternTexCoordRotation", result.pattern_rotation );
			read_float( floats, "g_flUvScale1", result.uv_scale1 );
			read_float( floats, "g_flWeaponLength1", result.weapon_length1 );
			read_float( floats, "g_flColorBrightness", result.color_brightness );
			read_float( floats, "g_flPaintRoughness", result.paint_roughness );
			read_float( floats, "g_flWearTexCoordScale", result.wear_scale );
			read_float( floats, "g_flWearTexCoordRotation", result.wear_rotation );
			read_float( floats, "g_flGrungeTexCoordScale", result.grunge_scale );
			read_float( floats, "g_flGrungeTexCoordRotation", result.grunge_rotation );

			const auto* ints = root.find( "m_intParams" );
			read_int( ints, "F_PAINT_STYLE", result.paint_style );
			int ignore = 0;
			read_int( ints, "g_bIgnoreWeaponSizeScale", ignore );
			result.ignore_size = ignore != 0;
			read_int( ints, "g_nPatternTextureHorizontalSampling", result.pattern_sampling[ 0 ] );
			read_int( ints, "g_nPatternTextureVerticalSampling", result.pattern_sampling[ 1 ] );
			read_int( ints, "F_PATTERN", result.glove_pattern );
			read_int( ints, "g_nPatternMode", result.glove_pattern_mode );
			read_int( ints, "g_bFlipFixup", result.glove_flip );
			if ( find_param( ints, "g_nPatternReplaceIndex" ) )
				read_int( ints, "g_nPatternReplaceIndex", result.glove_pattern_replace );
			static constexpr const char* k_id_pattern[ 8 ]{ "g_bId1Pattern", "g_bId2Pattern",
				"g_bId3Pattern", "g_bId4Pattern", "g_bId5Pattern", "g_bId6Pattern",
				"g_bId7Pattern", "g_bId8Pattern" };
			for ( int i = 0; i < 8; ++i )
			{
				int bit = 0;
				if ( find_param( ints, k_id_pattern[ i ] ) )
					read_int( ints, k_id_pattern[ i ], bit );
				if ( bit )
					result.id_pattern_bits |= 1 << i;
			}

			// The palette. Style paints index it with the pattern's channels; solid and
			// anodized paints have no pattern and are the palette alone.
			static constexpr const char* k_palette[ 4 ]{ "g_vColor0", "g_vColor1",
				"g_vColor2", "g_vColor3" };
			// Anodized paints name only g_vColor0; an entry left out repeats the one
			// before it, so the chain blend in the shader cannot pull towards black.
			for ( int i = 0; i < 4; ++i )
			{
				const auto* param = find_param( vectors, k_palette[ i ] );
				float value[ 4 ]{};
				const auto* kv = param ? param->find( "m_value" ) : nullptr;
				const bool present = kv && kv->as_float4( value );
				if ( i == 0 )
				{
					result.has_palette = present;
				}
				for ( int c = 0; c < 3; ++c )
				{
					result.palette[ i ][ c ] = present ? value[ c ] : ( i > 0 ? result.palette[ i - 1 ][ c ] : 0.0f );
				}
			}

			return result;
		}
		catch ( const std::exception& )
		{
			return {};
		}
	}

	finish_data load_finish( vpk_archive& vpk, const std::string& material,
		const std::string& color, const std::string& normal, const std::string& roughness,
		const std::string& ambient_occlusion, const std::string& mask, const bool palette )
	{
		finish_data result{};

		try
		{
			if ( !material.empty( ) )
			{
				const auto data = load_material( vpk, material );

				// The newer glove paints (Violet Beadwork and the rest of that set) name
				// eight region colours plus a picture that replaces the regions flagged
				// in g_bIdNPattern. A near-white region with no picture is the cloth tint.
				if ( data.shader.find( "customglove_preview" ) != std::string::npos
					&& !data.tint_id.empty( ) )
				{
					result.glove = true;
					result.pattern_mode = 3;
					result.mask = data.tint_id;
					result.normal = data.normal;
					result.has_normal = !data.normal.empty( );
					result.flip_v = data.glove_flip != 0;
					result.paint_roughness = 0.88f;
					result.pattern_scale = data.pattern_scale_xy[ 0 ] > 0.0f ? data.pattern_scale_xy[ 0 ] : 1.0f;
					result.pattern_scale_v = data.pattern_scale_xy[ 1 ] > 0.0f ? data.pattern_scale_xy[ 1 ] : 1.0f;
					result.pattern_bits = data.id_pattern_bits;
					for ( int i = 0; i < 8; ++i )
					{
						for ( int c = 0; c < 3; ++c )
							result.glove_tint[ i ][ c ] = data.id_color[ i ][ c ];
					}
					for ( int c = 0; c < 3; ++c )
						result.palette[ 0 ][ c ] = data.surface_tint[ c ];
					if ( data.glove_pattern != 0 && !data.pattern.empty( ) )
						result.color = data.pattern;
					return result;
				}

				// Gloves are not a picture laid over the mesh. The classic glove shader
				// stores eight colours and a mask whose alpha selects one of them; a
				// pattern, where the material has one, stands in for a single colour.
				if ( data.shader.find( "customglove" ) != std::string::npos
					&& data.shader.find( "preview" ) == std::string::npos
					&& !data.layer_mask.empty( ) )
				{
					result.glove = true;
					result.mask = data.layer_mask;
					result.normal = data.normal;
					result.ambient_occlusion = data.surface;
					result.has_normal = !data.normal.empty( );
					result.has_ao = !data.surface.empty( );
					result.pattern_uv_offset[ 0 ] = data.pattern_uv_offset[ 0 ];
					result.pattern_uv_offset[ 1 ] = data.pattern_uv_offset[ 1 ];
					result.pattern_scale = data.pattern_scale > 0.0f ? data.pattern_scale : 1.0f;
					result.pattern_rotation = data.pattern_rotation;
					result.pattern_mode = data.glove_pattern_mode;
					result.flip_v = data.glove_flip != 0;
					// Cloth, not the shiny default a weapon paint wants.
					result.paint_roughness = 0.88f;
					for ( int i = 0; i < 8; ++i )
					{
						for ( int c = 0; c < 3; ++c )
							result.glove_tint[ i ][ c ] = data.glove_tint[ i ][ c ];
					}
					if ( data.glove_pattern != 0 && !data.pattern.empty( ) )
					{
						result.color = data.pattern;
						// The index is 1-based in the material, matching the tint names.
						result.pattern_replace = data.glove_pattern_replace > 0
							? data.glove_pattern_replace - 1 : -1;
						for ( int i = 0; i < 4; ++i )
						{
							const int index = static_cast< int >( data.glove_pattern_palette[ i ] ) - 1;
							result.pattern_palette[ i ] = index >= 0 ? index : 0;
						}
					}
					return result;
				}

				// Only the weapon paint shader is one a single albedo can stand in for.
				if ( data.shader.find( "customweapon" ) == std::string::npos )
				{
					return result;
				}

				// Paint materials put the finish in g_tPattern; g_tColor is a shared grey
				// placeholder, and so are g_tMasks and g_tPosition -- the real ones live in
				// the weapon's composite inputs, which the preview looks up per draw call.
				// Solid and anodized paints have no pattern and are the palette alone.
				const int st = data.paint_style >= 0 ? data.paint_style : ( data.pattern.empty( ) ? 0 : 6 );
				result.color = data.pattern;
				result.solid = data.pattern.empty( ) && ( st == 0 || st == 3 ) && data.has_palette;
				result.pattern_uv_offset[ 0 ] = data.pattern_uv_offset[ 0 ];
				result.pattern_uv_offset[ 1 ] = data.pattern_uv_offset[ 1 ];
				result.pattern_scale = data.pattern_scale;
				result.pattern_rotation = data.pattern_rotation;
				result.pattern_sampling[ 0 ] = data.pattern_sampling[ 0 ];
				result.pattern_sampling[ 1 ] = data.pattern_sampling[ 1 ];
				result.paint_style = st;
				result.ignore_size = data.ignore_size;
				result.wear = data.wear;
				result.grunge = data.grunge;
				result.wear_scale = data.wear_scale > 0.0f ? data.wear_scale : 1.0f;
				result.wear_rotation = data.wear_rotation;
				result.grunge_scale = data.grunge_scale > 0.0f ? data.grunge_scale : 1.0f;
				result.grunge_rotation = data.grunge_rotation;

				// The finish material is named after its paint kit.
				const auto slash = material.find_last_of( '/' );
				auto kit = material.substr( slash == std::string::npos ? 0 : slash + 1 );
				if ( kit.ends_with( ".vmat" ) )
					kit.resize( kit.size( ) - 5 );
				result.legacy_model = paint_kit_uses_legacy_model( vpk, kit );
				result.color_brightness = data.color_brightness > 0.0f ? data.color_brightness : 1.0f;
				if ( data.paint_roughness >= 0.0f )
				{
					result.paint_roughness = data.paint_roughness;
				}

				// Which styles recolour a weight pattern through the palette: hydrographic
				// (1), spray (2) and the anodized families (4, 5). Custom (6), patina (7)
				// and gunsmith (8) ship the finished artwork in the pattern itself, so the
				// palette beside them is a template leftover that must not be applied.
				const bool style_palette = ( st == 1 || st == 2 || st == 4 || st == 5 );
				result.palette_mode = ( style_palette || result.solid ) && data.has_palette;
				for ( int i = 0; i < 4; ++i )
				{
					for ( int c = 0; c < 3; ++c )
					{
						result.palette[ i ][ c ] = data.palette[ i ][ c ];
					}
				}

				// The finish's own normal/roughness slots are placeholders in every paint
				// material shipped so far, so only the loose-map path below sets them.
			}
			else
			{
				result.color = color;
				result.normal = normal;
				result.roughness = roughness;
				result.ambient_occlusion = ambient_occlusion;
				result.mask = mask;
				result.loose = true;
				result.paint_style = 6;
				result.has_normal = !normal.empty( );
				result.has_roughness = !roughness.empty( );
				result.has_ao = !ambient_occlusion.empty( );
				// Loose maps ship without a paint material; they wear through the
				// shared maps most weapon paints point at.
				result.wear = "materials/models/weapons/customization/shared/paint_wear_psd_a2abffd8.vtex";
				result.grunge = "materials/models/weapons/customization/shared/gun_grunge_psd_f3419fed.vtex";
			}

			return result;
		}
		catch ( const std::exception& )
		{
			return {};
		}
	}

} // namespace chams
