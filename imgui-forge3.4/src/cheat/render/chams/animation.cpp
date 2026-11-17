#include <stdafx.hpp>
#include <render/chams/animation.hpp>
#include <core/assets/kv3.hpp>
#include <core/assets/resource.hpp>

namespace chams {

	namespace {

		// foundation::rotation declares its four-argument constructor constexpr but
		// defines it out of line, so it cannot actually be called from here.
		[[nodiscard]] foundation::rotation make_quat( float x, float y, float z, float w )
		{
			foundation::rotation q{};
			q.x = x; q.y = y; q.z = z; q.w = w;
			return q;
		}

		[[nodiscard]] foundation::rotation decode_quaternion48( const std::uint8_t* b )
		{
			const auto i1 = b[ 0 ] + ( ( b[ 1 ] & 127 ) << 8 );
			const auto i2 = b[ 2 ] + ( ( b[ 3 ] & 127 ) << 8 );
			const auto i3 = b[ 4 ] + ( ( b[ 5 ] & 127 ) << 8 );

			const auto swap_a = ( b[ 1 ] & 128 ) != 0;
			const auto swap_b = ( b[ 3 ] & 128 ) != 0;
			const auto negate = ( b[ 5 ] & 128 ) != 0;

			constexpr auto scale = 0.70710678f / 16384.0f; // sin(pi/4) / 16384
			const auto x = scale * static_cast< float >( i1 - 16384 );
			const auto y = scale * static_cast< float >( i2 - 16384 );
			const auto z = scale * static_cast< float >( i3 - 16384 );

			// The dropped component is the largest one, recovered from unit length.
			auto w = std::sqrt( std::max( 0.0f, 1.0f - x * x - y * y - z * z ) );
			if ( negate )
			{
				w = -w;
			}

			const auto slot = ( swap_a ? 2 : 0 ) | ( swap_b ? 1 : 0 );
			switch ( slot )
			{
			case 0:  return make_quat( w, x, y, z );
			case 1:  return make_quat( x, w, y, z );
			case 2:  return make_quat( x, y, w, z );
			default: return make_quat( x, y, z, w );
			}
		}

		[[nodiscard]] float dequantize( std::uint16_t raw, float start, float length )
		{
			return start + ( static_cast< float >( raw ) / 65535.0f ) * length;
		}

		[[nodiscard]] foundation::rotation quat_multiply( const foundation::rotation& a, const foundation::rotation& b )
		{
			return make_quat( a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z );
		}

		[[nodiscard]] foundation::rotation quat_conjugate( const foundation::rotation& q )
		{
			return make_quat( -q.x, -q.y, -q.z, q.w );
		}

		[[nodiscard]] foundation::rotation quat_slerp( const foundation::rotation& a, foundation::rotation b, float t )
		{
			auto dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
			if ( dot < 0.0f )
			{
				// Shortest arc: quaternions double-cover, so flip one side.
				b = make_quat( -b.x, -b.y, -b.z, -b.w );
				dot = -dot;
			}

			if ( dot > 0.9995f )
			{
				// Nearly parallel -- lerp and renormalize, slerp is numerically
				// unstable here and visually identical.
				auto out = make_quat(
					a.x + ( b.x - a.x ) * t, a.y + ( b.y - a.y ) * t,
					a.z + ( b.z - a.z ) * t, a.w + ( b.w - a.w ) * t );
				const auto len = std::sqrt( out.x * out.x + out.y * out.y + out.z * out.z + out.w * out.w );
				if ( len > 1e-8f ) { out.x /= len; out.y /= len; out.z /= len; out.w /= len; }
				return out;
			}

			const auto theta = std::acos( std::clamp( dot, -1.0f, 1.0f ) );
			const auto sin_theta = std::sin( theta );
			const auto wa = std::sin( ( 1.0f - t ) * theta ) / sin_theta;
			const auto wb = std::sin( t * theta ) / sin_theta;
			return make_quat( a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb );
		}

		[[nodiscard]] foundation::rotation matrix_to_quat( const bone_matrix& m )
		{
			// The 3x3 may carry a uniform scale; normalize the columns first.
			float r[ 3 ][ 3 ]{};
			for ( int col = 0; col < 3; ++col )
			{
				const auto len = std::sqrt( m.m[ 0 ][ col ] * m.m[ 0 ][ col ]
					+ m.m[ 1 ][ col ] * m.m[ 1 ][ col ] + m.m[ 2 ][ col ] * m.m[ 2 ][ col ] );
				const auto inv = len > 1e-8f ? 1.0f / len : 0.0f;
				r[ 0 ][ col ] = m.m[ 0 ][ col ] * inv;
				r[ 1 ][ col ] = m.m[ 1 ][ col ] * inv;
				r[ 2 ][ col ] = m.m[ 2 ][ col ] * inv;
			}

			const auto trace = r[ 0 ][ 0 ] + r[ 1 ][ 1 ] + r[ 2 ][ 2 ];
			if ( trace > 0.0f )
			{
				const auto s = std::sqrt( trace + 1.0f ) * 2.0f;
				return make_quat( ( r[ 2 ][ 1 ] - r[ 1 ][ 2 ] ) / s, ( r[ 0 ][ 2 ] - r[ 2 ][ 0 ] ) / s,
					( r[ 1 ][ 0 ] - r[ 0 ][ 1 ] ) / s, 0.25f * s );
			}
			if ( r[ 0 ][ 0 ] > r[ 1 ][ 1 ] && r[ 0 ][ 0 ] > r[ 2 ][ 2 ] )
			{
				const auto s = std::sqrt( 1.0f + r[ 0 ][ 0 ] - r[ 1 ][ 1 ] - r[ 2 ][ 2 ] ) * 2.0f;
				return make_quat( 0.25f * s, ( r[ 0 ][ 1 ] + r[ 1 ][ 0 ] ) / s,
					( r[ 0 ][ 2 ] + r[ 2 ][ 0 ] ) / s, ( r[ 2 ][ 1 ] - r[ 1 ][ 2 ] ) / s );
			}
			if ( r[ 1 ][ 1 ] > r[ 2 ][ 2 ] )
			{
				const auto s = std::sqrt( 1.0f + r[ 1 ][ 1 ] - r[ 0 ][ 0 ] - r[ 2 ][ 2 ] ) * 2.0f;
				return make_quat( ( r[ 0 ][ 1 ] + r[ 1 ][ 0 ] ) / s, 0.25f * s,
					( r[ 1 ][ 2 ] + r[ 2 ][ 1 ] ) / s, ( r[ 0 ][ 2 ] - r[ 2 ][ 0 ] ) / s );
			}
			const auto s = std::sqrt( 1.0f + r[ 2 ][ 2 ] - r[ 0 ][ 0 ] - r[ 1 ][ 1 ] ) * 2.0f;
			return make_quat( ( r[ 0 ][ 2 ] + r[ 2 ][ 0 ] ) / s, ( r[ 1 ][ 2 ] + r[ 2 ][ 1 ] ) / s,
				0.25f * s, ( r[ 1 ][ 0 ] - r[ 0 ][ 1 ] ) / s );
		}

		[[nodiscard]] bone_matrix rotation_matrix( const foundation::rotation& q )
		{
			bone_matrix out{};
			const auto xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
			const auto xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
			const auto wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

			out.m[ 0 ][ 0 ] = 1.0f - 2.0f * ( yy + zz ); out.m[ 0 ][ 1 ] = 2.0f * ( xy - wz );       out.m[ 0 ][ 2 ] = 2.0f * ( xz + wy );
			out.m[ 1 ][ 0 ] = 2.0f * ( xy + wz );        out.m[ 1 ][ 1 ] = 1.0f - 2.0f * ( xx + zz ); out.m[ 1 ][ 2 ] = 2.0f * ( yz - wx );
			out.m[ 2 ][ 0 ] = 2.0f * ( xz - wy );        out.m[ 2 ][ 1 ] = 2.0f * ( yz + wx );       out.m[ 2 ][ 2 ] = 1.0f - 2.0f * ( xx + yy );
			return out;
		}

		[[nodiscard]] bool decode_data_block( vpk_archive& vpk, const std::string& path,
			resource& res, kv3::document& doc )
		{
			auto full = path;
			if ( !full.ends_with( "_c" ) )
			{
				full += "_c"; // the reference is to the source asset, the VPK holds the compiled one
			}

			const auto* entry = vpk.find( full );
			if ( !entry || !res.parse( vpk.read( *entry ) ) )
			{
				return false;
			}

			const auto* block = res.find( "DATA" );
			if ( !block )
			{
				return false;
			}

			doc = kv3::decode( res.bytes( *block ), block->size );
			return true;
		}

	} // namespace

	nm_skeleton load_nm_skeleton( vpk_archive& vpk, const std::string& archive_path )
	{
		nm_skeleton out{};

		try
		{
			resource res{};
			kv3::document doc{};
			if ( !decode_data_block( vpk, archive_path, res, doc ) )
			{
				return out;
			}

			const auto* names = doc.root.find( "m_boneIDs" );
			const auto* parents = doc.root.find( "m_parentIndices" );
			const auto* rest = doc.root.find( "m_parentSpaceReferencePose" );
			if ( !names || !parents || !rest || names->size( ) == 0 )
			{
				return out;
			}

			out.bone_names.reserve( names->size( ) );
			out.parent_indices.reserve( names->size( ) );
			out.rest_positions.reserve( names->size( ) );

			for ( std::size_t i = 0; i < names->size( ); ++i )
			{
				out.bone_names.push_back( names->at( i )->as_string( ) );
				out.parent_indices.push_back( i < parents->size( )
					? static_cast< int >( parents->at( i )->as_int( ) ) : -1 );

				foundation::vec3 p{};
				if ( i < rest->size( ) )
				{
					const auto* entry = rest->at( i );
					if ( entry && entry->size( ) >= 3 )
					{
						p = foundation::vec3{
							static_cast< float >( entry->at( 0 )->as_double( ) ),
							static_cast< float >( entry->at( 1 )->as_double( ) ),
							static_cast< float >( entry->at( 2 )->as_double( ) ) };
					}
				}
				out.rest_positions.push_back( p );
			}

			return out;
		}
		catch ( const std::exception& )
		{
			return {};
		}
	}

	nm_clip load_nm_clip( vpk_archive& vpk, const std::string& archive_path )
	{
		nm_clip out{};

		try
		{
			resource res{};
			kv3::document doc{};
			if ( !decode_data_block( vpk, archive_path, res, doc ) )
			{
				return out;
			}

			const auto& root = doc.root;
			const auto* frames = root.find( "m_nNumFrames" );
			const auto* duration = root.find( "m_flDuration" );
			const auto* pose_data = root.find( "m_compressedPoseData" );
			const auto* offsets = root.find( "m_compressedPoseOffsets" );
			const auto* settings = root.find( "m_trackCompressionSettings" );

			if ( !frames || !pose_data || !offsets || !settings )
			{
				return out;
			}

			out.frame_count = static_cast< std::uint32_t >( frames->as_int( ) );
			out.duration = duration ? static_cast< float >( duration->as_double( ) ) : 0.0f;
			if ( const auto* additive = root.find( "m_bIsAdditive" ) )
			{
				out.additive = additive->as_bool( );
			}

			// Additive clips are deltas meant to be layered on a base pose; using
			// one on its own produces a mangled figure.
			if ( out.additive || out.frame_count == 0 )
			{
				return {};
			}

			// kv3::object exposes no blob accessor, so read the alternative
			// straight out of the variant.
			const auto* blob = std::get_if<std::vector<std::uint8_t>>( &pose_data->m_value );
			if ( !blob || blob->size( ) < 2 )
			{
				return {};
			}

			out.words.resize( blob->size( ) / 2 );
			std::memcpy( out.words.data( ), blob->data( ), out.words.size( ) * 2 );

			out.frame_offsets.reserve( offsets->size( ) );
			for ( std::size_t i = 0; i < offsets->size( ); ++i )
			{
				out.frame_offsets.push_back( static_cast< std::uint32_t >( offsets->at( i )->as_int( ) ) );
			}

			const auto range = [ ]( const kv3::object* obj, const char* name, float& start, float& length )
				{
					const auto* r = obj ? obj->find( name ) : nullptr;
					if ( !r ) return;
					if ( const auto* s = r->find( "m_flRangeStart" ) ) start = static_cast< float >( s->as_double( ) );
					if ( const auto* l = r->find( "m_flRangeLength" ) ) length = static_cast< float >( l->as_double( ) );
				};

			out.tracks.reserve( settings->size( ) );
			for ( std::size_t i = 0; i < settings->size( ); ++i )
			{
				const auto* item = settings->at( i );
				nm_clip::track t{};

				range( item, "m_translationRangeX", t.translation_start[ 0 ], t.translation_length[ 0 ] );
				range( item, "m_translationRangeY", t.translation_start[ 1 ], t.translation_length[ 1 ] );
				range( item, "m_translationRangeZ", t.translation_start[ 2 ], t.translation_length[ 2 ] );
				range( item, "m_scaleRange", t.scale_start, t.scale_length );

				if ( const auto* v = item->find( "m_nTrackReadOffset" ) ) t.read_offset = static_cast< std::uint32_t >( v->as_int( ) );
				if ( const auto* v = item->find( "m_bIsRotationStatic" ) ) t.rotation_static = v->as_bool( );
				if ( const auto* v = item->find( "m_bIsTranslationStatic" ) ) t.translation_static = v->as_bool( );
				if ( const auto* v = item->find( "m_bIsScaleStatic" ) ) t.scale_static = v->as_bool( );

				if ( const auto* c = item->find( "m_constantRotation" ); c && c->size( ) >= 4 )
				{
					t.constant_rotation = make_quat( static_cast< float >( c->at( 0 )->as_double( ) ), static_cast< float >( c->at( 1 )->as_double( ) ), static_cast< float >( c->at( 2 )->as_double( ) ), static_cast< float >( c->at( 3 )->as_double( ) ) );
				}

				out.tracks.push_back( t );
			}

			return out;
		}
		catch ( const std::exception& )
		{
			return {};
		}
	}

	std::vector<int> map_tracks_to_model( const nm_skeleton& skeleton, const skinned_mesh& mesh )
	{
		std::unordered_map<std::string, int> by_name{};
		for ( std::size_t i = 0; i < mesh.bones.size( ); ++i )
		{
			by_name.emplace( mesh.bones[ i ].name, static_cast< int >( i ) );
		}

		std::vector<int> out( skeleton.bone_names.size( ), -1 );
		for ( std::size_t i = 0; i < skeleton.bone_names.size( ); ++i )
		{
			if ( const auto it = by_name.find( skeleton.bone_names[ i ] ); it != by_name.end( ) )
			{
				out[ i ] = it->second;
			}
		}
		return out;
	}

	void sample_pose( const nm_clip& clip, const nm_skeleton& skeleton, const skinned_mesh& mesh,
		const std::vector<int>& track_to_bone, float time,
		std::vector<bone_matrix>& out, std::vector<bone_matrix>& world,
		const foundation::vec3* present_backs )
	{
		const auto bone_count = mesh.bones.size( );
		out.assign( bone_count, bone_matrix::identity( ) );
		world.assign( bone_count, bone_matrix::identity( ) );

		const auto track_count = clip.tracks.size( );
		if ( !clip.valid( ) || bone_count == 0 || track_count == 0
			|| skeleton.parent_indices.size( ) < track_count )
		{
			return;
		}

		// Loop, blending the two neighbouring frames so playback is smooth rather
		// than stepping at the clip's authored rate.
		const auto duration = clip.duration > 0.001f ? clip.duration : 1.0f;
		auto phase = std::fmod( time, duration ) / duration;
		if ( phase < 0.0f ) phase += 1.0f;

		const auto exact = phase * static_cast< float >( clip.frame_count );
		const auto frame_a = static_cast< std::uint32_t >( exact ) % clip.frame_count;
		const auto frame_b = ( frame_a + 1 ) % clip.frame_count;
		const auto blend = exact - std::floor( exact );

		if ( frame_a >= clip.frame_offsets.size( ) || frame_b >= clip.frame_offsets.size( ) )
		{
			return;
		}

		const auto read = [ & ]( std::uint32_t frame, std::size_t index, foundation::rotation& rotation, foundation::vec3& translation )
			{
				const auto& track = clip.tracks[ index ];
				auto cursor = clip.frame_offsets[ frame ] + track.read_offset;

				rotation = track.constant_rotation;
				if ( !track.rotation_static )
				{
					if ( cursor + 3 <= clip.words.size( ) )
					{
						std::uint8_t bytes[ 6 ]{};
						std::memcpy( bytes, clip.words.data( ) + cursor, 6 );
						rotation = decode_quaternion48( bytes );
					}
					cursor += 3;
				}

				// Static translations come from the skeleton's rest pose; only the
				// bones that actually move store per-frame values.
				translation = skeleton.rest_positions[ index ];
				if ( !track.translation_static && cursor + 3 <= clip.words.size( ) )
				{
					translation = foundation::vec3{
						dequantize( clip.words[ cursor + 0 ], track.translation_start[ 0 ], track.translation_length[ 0 ] ),
						dequantize( clip.words[ cursor + 1 ], track.translation_start[ 1 ], track.translation_length[ 1 ] ),
						dequantize( clip.words[ cursor + 2 ], track.translation_start[ 2 ], track.translation_length[ 2 ] ) };
				}
			};

		// Compose the clip's own hierarchy. Its parents are stored
		// parent-before-child, so a single forward pass suffices.
		std::vector<bone_matrix> clip_world( track_count, bone_matrix::identity( ) );
		for ( std::size_t i = 0; i < track_count; ++i )
		{
			foundation::rotation rot_a{}, rot_b{};
			foundation::vec3 pos_a{}, pos_b{};
			read( frame_a, i, rot_a, pos_a );
			read( frame_b, i, rot_b, pos_b );

			auto local = rotation_matrix( quat_slerp( rot_a, rot_b, blend ) );
			local.m[ 0 ][ 3 ] = pos_a.x + ( pos_b.x - pos_a.x ) * blend;
			local.m[ 1 ][ 3 ] = pos_a.y + ( pos_b.y - pos_a.y ) * blend;
			local.m[ 2 ][ 3 ] = pos_a.z + ( pos_b.z - pos_a.z ) * blend;

			const auto parent = skeleton.parent_indices[ i ];
			clip_world[ i ] = parent < 0 || static_cast< std::size_t >( parent ) >= i
				? local
				: clip_world[ static_cast< std::size_t >( parent ) ] * local;
		}

		// The composed transforms are already in the model's space, so the
		// skinning matrix is the plain world * inverse bind with no retarget.
		for ( std::size_t i = 0; i < track_count && i < track_to_bone.size( ); ++i )
		{
			const auto bone = track_to_bone[ i ];
			if ( bone < 0 || static_cast< std::size_t >( bone ) >= bone_count )
			{
				continue;
			}

			world[ static_cast< std::size_t >( bone ) ] = clip_world[ i ];
			out[ static_cast< std::size_t >( bone ) ] = clip_world[ i ] * mesh.bones[ static_cast< std::size_t >( bone ) ].inverse_bind;
		}

		std::vector<bool> driven( bone_count, false );
		for ( std::size_t i = 0; i < track_count && i < track_to_bone.size( ); ++i )
		{
			const auto bone = track_to_bone[ i ];
			if ( bone >= 0 && static_cast< std::size_t >( bone ) < bone_count )
			{
				driven[ static_cast< std::size_t >( bone ) ] = true;
			}
		}

		// The clip turns the palms up toward a studio camera, including while the
		// fingers are spread. From the player's eyes that open pose has to show the
		// back of the glove. Roll each hand about the forearm until its dorsal axis
		// faces the camera, and carry the fingers with that roll. The wrist bend
		// itself is left alone.
		if ( present_backs && present_backs->length_sqr( ) > 1e-6f )
		{
			const auto view = present_backs->normalized( );
			const auto column = [ ]( const bone_matrix& m, int c )
				{
					return foundation::vec3{ m.m[ 0 ][ c ], m.m[ 1 ][ c ], m.m[ 2 ][ c ] };
				};

			for ( const char* hand_name : { "hand_L", "hand_R" } )
			{
				int hand = -1;
				for ( std::size_t b = 0; b < bone_count; ++b )
				{
					if ( mesh.bones[ b ].name == hand_name && driven[ b ] )
					{
						hand = static_cast< int >( b );
						break;
					}
				}
				if ( hand < 0 )
					continue;

				const auto fore = mesh.bones[ static_cast< std::size_t >( hand ) ].parent;
				if ( fore < 0 || !driven[ static_cast< std::size_t >( fore ) ] )
					continue;

				const auto axis = column( world[ static_cast< std::size_t >( fore ) ], 0 ).normalized( );
				if ( axis.length_sqr( ) < 1e-6f )
					continue;

				// Fingers curl toward local +Y on the left hand, so the back is -Y.
				// The right hand bone is mirrored (fingers run along -X, the thumb
				// sits on -Y), so there the back is +Y.
				const auto side = column( world[ static_cast< std::size_t >( hand ) ], 1 );
				const auto dorsal = mesh.bones[ static_cast< std::size_t >( hand ) ].name == "hand_L" ? -side : side;
				const auto dorsal_perp = dorsal - axis * dorsal.dot( axis );
				const auto view_perp = view - axis * view.dot( axis );
				if ( dorsal_perp.length_sqr( ) < 1e-6f || view_perp.length_sqr( ) < 1e-6f )
					continue;

				const auto theta = std::atan2(
					axis.dot( dorsal_perp.cross( view_perp ) ), dorsal_perp.dot( view_perp ) );
				if ( std::fabs( theta ) < 0.02f )
					continue;

				const auto half = theta * 0.5f;
				const auto R = rotation_matrix( make_quat(
					axis.x * std::sin( half ), axis.y * std::sin( half ), axis.z * std::sin( half ),
					std::cos( half ) ) );

				const foundation::vec3 pivot{
					world[ static_cast< std::size_t >( hand ) ].m[ 0 ][ 3 ],
					world[ static_cast< std::size_t >( hand ) ].m[ 1 ][ 3 ],
					world[ static_cast< std::size_t >( hand ) ].m[ 2 ][ 3 ] };
				auto pivot_m = bone_matrix::identity( );
				pivot_m.m[ 0 ][ 3 ] = pivot.x;
				pivot_m.m[ 1 ][ 3 ] = pivot.y;
				pivot_m.m[ 2 ][ 3 ] = pivot.z;
				const auto spun_pivot = R * pivot_m;

				const auto under_hand = [ & ]( std::size_t bone, auto&& self ) -> bool {
					if ( static_cast< int >( bone ) == hand )
						return true;
					const auto p = mesh.bones[ bone ].parent;
					if ( p < 0 )
						return false;
					return self( static_cast< std::size_t >( p ), self );
				};
				for ( std::size_t b = 0; b < bone_count; ++b )
				{
					if ( !under_hand( b, under_hand ) )
						continue;
					auto spun = R * world[ b ];
					spun.m[ 0 ][ 3 ] += pivot.x - spun_pivot.m[ 0 ][ 3 ];
					spun.m[ 1 ][ 3 ] += pivot.y - spun_pivot.m[ 1 ][ 3 ];
					spun.m[ 2 ][ 3 ] += pivot.z - spun_pivot.m[ 2 ][ 3 ];
					world[ b ] = spun;
					out[ b ] = spun * mesh.bones[ b ].inverse_bind;
				}
			}
		}

		for ( std::size_t i = 0; i < bone_count; ++i )
		{
			if ( driven[ i ] )
			{
				continue;
			}

			const auto bind = mesh.bones[ i ].inverse_bind.inverse_rigid( );
			const auto parent = mesh.bones[ i ].parent;

			// Forearm twist bones are not in the viewmodel clip. Left at their bind
			// roll they hold the wrist cuff at the rest twist while the hand has
			// turned, so the left wrist reads as rolled out of the forearm.
			const auto& name = mesh.bones[ i ].name;
			if ( parent >= 0 && name.find( "TWIST" ) != std::string::npos )
			{
				const auto limb = [ & ]() -> int {
					const char* key = name.find( "arm_lower" ) != std::string::npos ? "hand_"
						: name.find( "arm_upper" ) != std::string::npos ? "arm_lower_" : nullptr;
					if ( !key )
						return -1;
					for ( std::size_t c = 0; c < bone_count; ++c )
					{
						if ( mesh.bones[ c ].parent != parent || !driven[ c ] )
							continue;
						if ( mesh.bones[ c ].name.find( key ) != std::string::npos
							&& mesh.bones[ c ].name.find( "TWIST" ) == std::string::npos )
							return static_cast< int >( c );
					}
					return -1;
				}( );
				if ( limb >= 0 )
				{
					const auto local = mesh.bones[ static_cast< std::size_t >( parent ) ].inverse_bind * bind;
					const auto weight = name.find( "TWIST1" ) != std::string::npos ? 0.72f : 0.34f;
					foundation::rotation q{};
					if ( present_backs )
					{
						// Follow only the roll around the limb. A full slerp takes the
						// short arc, and once the hand has turned palm-down that arc
						// kinks the wrist cuff the other way.
						const auto& pw = world[ static_cast< std::size_t >( parent ) ];
						const auto& lw = world[ static_cast< std::size_t >( limb ) ];
						foundation::vec3 axis{ pw.m[ 0 ][ 0 ], pw.m[ 1 ][ 0 ], pw.m[ 2 ][ 0 ] };
						const auto axis_len = axis.length( );
						if ( axis_len > 1e-6f )
							axis /= axis_len;
						const foundation::vec3 parent_side{ pw.m[ 0 ][ 1 ], pw.m[ 1 ][ 1 ], pw.m[ 2 ][ 1 ] };
						const foundation::vec3 limb_side{ lw.m[ 0 ][ 1 ], lw.m[ 1 ][ 1 ], lw.m[ 2 ][ 1 ] };
						const auto parent_perp = parent_side - axis * parent_side.dot( axis );
						const auto limb_perp = limb_side - axis * limb_side.dot( axis );
						const auto twist = axis_len > 1e-6f
							&& parent_perp.length_sqr( ) > 1e-6f && limb_perp.length_sqr( ) > 1e-6f
							? std::atan2( axis.dot( parent_perp.cross( limb_perp ) ), parent_perp.dot( limb_perp ) )
							: 0.0f;
						const auto half = twist * weight * 0.5f;
						const auto roll = make_quat(
							axis.x * std::sin( half ), axis.y * std::sin( half ), axis.z * std::sin( half ),
							std::cos( half ) );
						q = quat_multiply( roll, matrix_to_quat( pw ) );
					}
					else
					{
						q = quat_slerp(
							matrix_to_quat( world[ static_cast< std::size_t >( parent ) ] ),
							matrix_to_quat( world[ static_cast< std::size_t >( limb ) ] ), weight );
					}
					auto posed = rotation_matrix( q );
					const auto& pw = world[ static_cast< std::size_t >( parent ) ];
					for ( int row = 0; row < 3; ++row )
					{
						posed.m[ row ][ 3 ] = pw.m[ row ][ 0 ] * local.m[ 0 ][ 3 ]
							+ pw.m[ row ][ 1 ] * local.m[ 1 ][ 3 ]
							+ pw.m[ row ][ 2 ] * local.m[ 2 ][ 3 ]
							+ pw.m[ row ][ 3 ];
					}
					world[ i ] = posed;
					out[ i ] = posed * mesh.bones[ i ].inverse_bind;
					continue;
				}
			}

			if ( parent < 0 || static_cast< std::size_t >( parent ) >= bone_count )
			{
				world[ i ] = bind;
			}
			else
			{

				const auto local = mesh.bones[ static_cast< std::size_t >( parent ) ].inverse_bind * bind;
				world[ i ] = world[ static_cast< std::size_t >( parent ) ] * local;
			}

			out[ i ] = world[ i ] * mesh.bones[ i ].inverse_bind;
		}
	}

} // namespace chams
