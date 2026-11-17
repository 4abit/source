#include <stdafx.hpp>
#include <render/chams/renderer.hpp>
#include <features/visuals/avatars.hpp>
#include <features/visuals/visuals.hpp>
#include <features/combat/ragebot.hpp>

#include <optional>
#include <unordered_map>

namespace features::visuals {
	namespace {
		struct meter_geometry
		{
			float x{};
			float y{};
			float width{};
			float height{};
			float fill{};
			bool vertical{};
		};

		template <typename Style, typename Layout>
		[[nodiscard]] meter_geometry paint_meter_body( zdraw::draw_list& draw_list,
			const game::bounds_projector::data& bounds, float fraction,
			const Style& style, const Layout& layout, bool vertical )
		{
			const auto thickness = std::clamp( style.thickness, 1.0f, 12.0f ) * layout.scale;
			meter_geometry result{};
			result.vertical = vertical;
			result.width = vertical ? thickness : std::floor( bounds.width( ) * layout.scale );
			result.height = vertical ? std::floor( bounds.height( ) * layout.scale ) : thickness;

			const auto center_x = config::visual_profile::player::resolve_layout_axis(
				bounds.min.x, bounds.max.x, layout.x,
				config::visual_profile::player::layout_reference_width );
			const auto center_y = config::visual_profile::player::resolve_layout_axis(
				bounds.min.y, bounds.max.y, layout.y,
				config::visual_profile::player::layout_reference_height );
			result.x = std::floor( center_x - result.width * 0.5f );
			result.y = std::floor( center_y - result.height * 0.5f );
			result.fill = std::floor( ( vertical ? result.height : result.width )
				* std::clamp( fraction, 0.0f, 1.0f ) );

			if ( style.outline )
			{
				const auto outline = std::clamp( style.outline_thickness, 0.5f, 4.0f ) * layout.scale;
				draw_list.add_rect_filled( result.x - outline, result.y - outline,
					result.width + outline * 2.0f, result.height + outline * 2.0f,
					style.outline_color );
			}
			draw_list.add_rect_filled( result.x, result.y, result.width, result.height,
				style.background_color );

			if ( result.fill > 0.0f )
			{
				const auto fill_x = result.x;
				const auto fill_y = vertical ? result.y + result.height - result.fill : result.y;
				const auto fill_width = vertical ? result.width : result.fill;
				const auto fill_height = vertical ? result.fill : result.height;
				if ( style.gradient )
				{
					if ( vertical )
						draw_list.add_rect_filled_multi_color( fill_x, fill_y, fill_width, fill_height,
							style.full_color, style.full_color, style.low_color, style.low_color );
					else
						draw_list.add_rect_filled_multi_color( fill_x, fill_y, fill_width, fill_height,
							style.low_color, style.full_color, style.full_color, style.low_color );
				}
				else
				{
					draw_list.add_rect_filled( fill_x, fill_y, fill_width, fill_height,
						style.full_color );
				}
			}

			// Separators are painted after the fill so every module stays visually
			// independent at any value, including partially filled blocks.
			const auto segments = std::clamp( style.segments, 1, 10 );
			const auto gap = std::clamp( style.segment_gap, 0.0f, 4.0f ) * layout.scale;
			if ( segments > 1 && gap > 0.0f )
			{
				const auto separator = style.outline ? style.outline_color : style.background_color;
				const auto extent = vertical ? result.height : result.width;
				for ( int segment = 1; segment < segments; ++segment )
				{
					const auto offset = std::floor( extent * static_cast<float>( segment ) / segments );
					if ( vertical )
						draw_list.add_rect_filled( result.x, result.y + offset - gap * 0.5f,
							result.width, gap, separator );
					else
						draw_list.add_rect_filled( result.x + offset - gap * 0.5f, result.y,
							gap, result.height, separator );
				}
			}
			return result;
		}

		enum class status_icon : std::uint8_t
		{
			money, armor, kit, scoped, defusing, flashed, ping, distance, bomb
		};

		void paint_status_icon( zdraw::draw_list& draw_list, status_icon icon,
			float x, float y, float size, zdraw::rgba color )
		{
			const auto s = std::max( 7.0f, size );
			const auto cx = x + s * 0.5f;
			const auto cy = y + s * 0.5f;
			const auto line = std::max( 1.0f, s / 9.0f );
			const zdraw::rgba outline{ 0, 0, 0, color.a };
			const auto stroke = [&]( float x0, float y0, float x1, float y1 )
			{
				draw_list.add_line( x0, y0, x1, y1, outline, line + 2.0f );
				draw_list.add_line( x0, y0, x1, y1, color, line );
			};

			switch ( icon )
			{
			case status_icon::kit:
			case status_icon::defusing:
				// Compact pliers: two jaws, crossed arms and two handle loops.
				stroke( x + s * 0.24f, y + s * 0.18f, x + s * 0.76f, y + s * 0.82f );
				stroke( x + s * 0.76f, y + s * 0.18f, x + s * 0.24f, y + s * 0.82f );
				draw_list.add_circle( x + s * 0.22f, y + s * 0.82f, s * 0.13f, outline, 12, line + 2.0f );
				draw_list.add_circle( x + s * 0.78f, y + s * 0.82f, s * 0.13f, outline, 12, line + 2.0f );
				draw_list.add_circle( x + s * 0.22f, y + s * 0.82f, s * 0.13f, color, 12, line );
				draw_list.add_circle( x + s * 0.78f, y + s * 0.82f, s * 0.13f, color, 12, line );
				break;
			case status_icon::scoped:
				draw_list.add_circle( cx, cy, s * 0.30f, outline, 16, line + 2.0f );
				draw_list.add_circle( cx, cy, s * 0.30f, color, 16, line );
				stroke( cx - s * 0.45f, cy, cx + s * 0.45f, cy );
				stroke( cx, cy - s * 0.45f, cx, cy + s * 0.45f );
				break;
			case status_icon::flashed:
				draw_list.add_circle_filled( cx, cy, s * 0.20f, color, 16 );
				for ( int ray = 0; ray < 8; ++ray )
				{
					const auto angle = static_cast<float>( ray ) * std::numbers::pi_v<float> / 4.0f;
					stroke( cx + std::cos( angle ) * s * 0.30f, cy + std::sin( angle ) * s * 0.30f,
						cx + std::cos( angle ) * s * 0.46f, cy + std::sin( angle ) * s * 0.46f );
				}
				break;
			case status_icon::bomb:
				draw_list.add_circle( cx, y + s * 0.60f, s * 0.28f, outline, 16, line + 2.0f );
				draw_list.add_circle( cx, y + s * 0.60f, s * 0.28f, color, 16, line );
				stroke( cx, y + s * 0.30f, x + s * 0.72f, y + s * 0.08f );
				break;
			case status_icon::armor:
				stroke( cx, y + s * 0.08f, x + s * 0.82f, y + s * 0.25f );
				stroke( x + s * 0.82f, y + s * 0.25f, x + s * 0.72f, y + s * 0.72f );
				stroke( x + s * 0.72f, y + s * 0.72f, cx, y + s * 0.92f );
				stroke( cx, y + s * 0.92f, x + s * 0.28f, y + s * 0.72f );
				stroke( x + s * 0.28f, y + s * 0.72f, x + s * 0.18f, y + s * 0.25f );
				stroke( x + s * 0.18f, y + s * 0.25f, cx, y + s * 0.08f );
				break;
			default:
				// Remaining flags keep a compact, language-independent badge.
				draw_list.add_circle( cx, cy, s * 0.35f, outline, 16, line + 2.0f );
				draw_list.add_circle( cx, cy, s * 0.35f, color, 16, line );
				stroke( cx, cy, cx + s * 0.22f, cy - s * 0.22f );
				break;
			}
		}
	} // namespace

	// ---------------------------------------------------------------------------
	// ESP editor layout. The menu's preview stacks every element on the side the
	// user dropped it, in the user's order; this pass repeats those rules on the
	// real box so the overlay matches the editor element for element.
	// ---------------------------------------------------------------------------
	namespace {
		using editor_cfg = config::visual_profile::player::editor;
		using aim_marker = std::optional<features::combat::aim_preview>;

		[[nodiscard]] zdraw::rgba with_alpha( zdraw::rgba color, float factor )
		{
			color.a = static_cast<std::uint8_t>( std::clamp( color.a * factor, 0.0f, 255.0f ) );
			return color;
		}

		[[nodiscard]] zdraw::rgba mix( zdraw::rgba a, zdraw::rgba b, float t )
		{
			t = std::clamp( t, 0.0f, 1.0f );
			const auto lerp = [ t ]( std::uint8_t x, std::uint8_t y )
			{
				return static_cast<std::uint8_t>( x + ( static_cast<float>( y ) - x ) * t + 0.5f );
			};
			return { lerp( a.r, b.r ), lerp( a.g, b.g ), lerp( a.b, b.b ), lerp( a.a, b.a ) };
		}

		[[nodiscard]] bool stacked_element( int id )
		{
			return id != editor_cfg::box && id != editor_cfg::skeleton && id != editor_cfg::hitboxes;
		}
		[[nodiscard]] bool bar_element( int id )
		{
			return id == editor_cfg::health_bar || id == editor_cfg::ammo_bar || id == editor_cfg::readiness_bar;
		}
		[[nodiscard]] bool icon_element( int id )
		{
			return id == editor_cfg::weapon_icon || id == editor_cfg::bomb_icon
				|| id == editor_cfg::defuser_icon || id == editor_cfg::taser_icon;
		}
		[[nodiscard]] bool flag_element( int id )
		{
			return id >= editor_cfg::unarmored && id <= editor_cfg::delay;
		}

		[[nodiscard]] bool carries( const game::player_snapshot& p, std::string_view item )
		{
			if ( p.weapon.name == item ) return true;
			return std::find( p.loadout.begin( ), p.loadout.end( ), item ) != p.loadout.end( );
		}

		// Fraction of the current attack cooldown (shot, reload, deploy) that has run.
		[[nodiscard]] float weapon_readiness( const game::player_snapshot& p, float now )
		{
			struct track { std::int32_t tick{}; float total{}; };
			static std::unordered_map<std::uintptr_t, track> tracks{};
			if ( tracks.size( ) > 128 ) tracks.clear( );
			constexpr float interval = 1.0f / 64.0f;
			const auto remaining = static_cast<float>( p.weapon.next_attack_tick ) * interval - now;
			auto& entry = tracks[ p.pawn ];
			if ( p.weapon.next_attack_tick != entry.tick )
			{
				entry.tick = p.weapon.next_attack_tick;
				entry.total = std::max( remaining, 0.0f );
			}
			if ( remaining <= 0.0f || entry.total <= 0.0f ) return 1.0f;
			return std::clamp( 1.0f - remaining / entry.total, 0.0f, 1.0f );
		}

		struct element_content
		{
			bool present{};
			std::string text{}; // text, flag label or icon glyph
			float fraction{};   // bars
		};

		[[nodiscard]] element_content content_of( int id, const editor_cfg::element& e,
			const game::player_snapshot& p, const aim_marker& aim, float now )
		{
			const auto text = [ & ]( bool on, std::string value ) -> element_content
			{
				if ( !on || value.empty( ) ) return {};
				return { true, config::visual_profile::player::apply_text_case( value, e.text_case ), 0.0f };
			};
			const auto targeted = aim && aim->pawn == p.pawn;
			switch ( id )
			{
			case editor_cfg::name: return text( true, p.display_name );
			case editor_cfg::distance:
				return text( true, std::format( "{:.0f} M",
					game::camera( ).origin( ).distance( p.origin ) * 0.01905f ) );
			case editor_cfg::health_bar:
				return { true, {}, std::clamp( p.health, 0, 100 ) / 100.0f };
			case editor_cfg::ammo_bar:
				if ( p.weapon.max_ammo <= 0 || p.weapon.ammo < 0 ) return {};
				return { true, {}, static_cast<float>( std::clamp( p.weapon.ammo, 0, p.weapon.max_ammo ) )
					/ static_cast<float>( p.weapon.max_ammo ) };
			case editor_cfg::readiness_bar:
				if ( p.weapon.name.empty( ) || p.weapon.next_attack_tick <= 0 ) return {};
				return { true, {}, weapon_readiness( p, now ) };
			case editor_cfg::unarmored: return text( p.armor <= 0, "NOARM" );
			case editor_cfg::defuser: return text( p.has_defuser, "KIT" );
			case editor_cfg::blind: return text( p.is_flashed, "BLIND" );
			case editor_cfg::scoped: return text( p.is_scoped, "ZOOM" );
			case editor_cfg::reload: return text( p.weapon.reloading, "RELOAD" );
			case editor_cfg::immunity: return text( p.invulnerable, "IMMUNE" );
			case editor_cfg::slowed: return text( p.velocity_modifier < 0.98f, "SLOW" );
			case editor_cfg::vulnerable:
				// Cannot shoot back right now: no gun out, an empty clip or a reload.
				return text( p.weapon.name.empty( ) || p.weapon.max_ammo <= 0
					|| p.weapon.ammo <= 0 || p.weapon.reloading, "VULN" );
			case editor_cfg::bomb: return text( carries( p, "c4" ), "C4" );
			case editor_cfg::hostage: return text( p.carrying_hostage, "HOST" );
			case editor_cfg::defuse: return text( p.is_defusing, "DEFUSE" );
			case editor_cfg::pin_pulled: return text( p.weapon.pin_pulled, "PIN" );
			case editor_cfg::money: return text( true, std::format( "${}", p.money ) );
			case editor_cfg::order_priority: return text( targeted, "1ST" );
			case editor_cfg::delay: return text( targeted && !aim->ready, "DELAY" );
			case editor_cfg::weapon_text: return text( true, p.weapon.name );
			case editor_cfg::weapon_icon:
			{
				const auto glyph = player_t::weapon_glyph( p.weapon.name );
				if ( p.weapon.name.empty( ) || glyph.empty( ) ) return {};
				return { true, std::string{ glyph }, 0.0f };
			}
			case editor_cfg::bomb_icon:
				return carries( p, "c4" ) ? element_content{ true, "o", 0.0f } : element_content{};
			case editor_cfg::defuser_icon:
				return p.has_defuser ? element_content{ true, "r", 0.0f } : element_content{};
			case editor_cfg::taser_icon:
				return carries( p, "taser" ) ? element_content{ true, "h", 0.0f } : element_content{};
			default: return {};
			}
		}

		// The faces the editor offers. Smooth (non-ESP) fonts are used on purpose:
		// the ESP atlases carry a baked outline, and here Outline and Shadow are the
		// user's own switches.
		[[nodiscard]] zdraw::font editor_font( int id, const editor_cfg::element& e )
		{
			const auto& loaded = app::context().overlay.fonts( );
			const zdraw::font* base{};
			float size{};
			if ( icon_element( id ) )
			{
				base = loaded.weapons_15;
				size = 14.0f;
			}
			else if ( flag_element( id ) )
			{
				base = e.font == 0 ? loaded.notosans_medium_12 : loaded.menu_semibold_13;
				size = 10.0f;
			}
			else
			{
				base = e.font == 0 ? loaded.menu_regular_12
					: e.font == 2 ? loaded.menu_semibold_13 : loaded.notosans_medium_12;
				size = 11.5f;
			}
			if ( !base || !base->im_font ) base = loaded.notosans_medium_12;
			zdraw::font out = base ? *base : zdraw::font{};
			out.font_size = size * e.scale;
			return out;
		}

		void styled_text( zdraw::draw_list& dl, float x, float y, std::string_view text,
			const zdraw::font& font, const editor_cfg::element& e, zdraw::rgba color )
		{
			x = std::floorf( x );
			y = std::floorf( y );
			const auto fade = static_cast<float>( color.a ) / 255.0f;
			if ( e.shadow )
				dl.add_text( x + 1.0f, y + 1.0f, text, &font, with_alpha( { 0, 0, 0, 210 }, fade ) );
			if ( e.outline )
			{
				const auto outline = with_alpha( e.outline_color, fade );
				dl.add_text( x - 1.0f, y, text, &font, outline );
				dl.add_text( x + 1.0f, y, text, &font, outline );
				dl.add_text( x, y - 1.0f, text, &font, outline );
				dl.add_text( x, y + 1.0f, text, &font, outline );
			}
			dl.add_text( x, y, text, &font, color );
		}

		// Projects a hitbox capsule; false when it is behind the camera or degenerate.
		[[nodiscard]] bool project_capsule( const foundation::vec3& eye, const foundation::vec3& a,
			const foundation::vec3& b, float radius, foundation::vec2& sa, foundation::vec2& sb, float& screen_radius )
		{
			sa = game::camera( ).project( a );
			sb = game::camera( ).project( b );
			if ( !game::camera( ).projection_valid( sa ) || !game::camera( ).projection_valid( sb ) )
				return false;
			const auto center = ( a + b ) * 0.5f;
			const auto view = ( center - eye ).normalized( );
			auto side = ( b - a ).cross( view );
			if ( side.length( ) < 0.001f ) side = foundation::vec3{ 0.0f, 0.0f, 1.0f }.cross( view );
			if ( side.length( ) < 0.001f ) side = { 1.0f, 0.0f, 0.0f };
			side = side.normalized( ) * radius;
			const auto p0 = game::camera( ).project( center + side );
			const auto p1 = game::camera( ).project( center - side );
			if ( !game::camera( ).projection_valid( p0 ) || !game::camera( ).projection_valid( p1 ) )
				return false;
			screen_radius = std::hypot( p0.x - p1.x, p0.y - p1.y ) * 0.5f;
			return screen_radius > 0.0f && screen_radius < 500.0f;
		}

		void paint_capsule( zdraw::draw_list& dl, const foundation::vec2& sa, const foundation::vec2& sb,
			float radius, zdraw::rgba fill, zdraw::rgba line )
		{
			const auto dx = sb.x - sa.x;
			const auto dy = sb.y - sa.y;
			if ( std::hypot( dx, dy ) < 1.0f )
			{
				dl.add_circle_filled( sa.x, sa.y, radius, fill, 24 );
				dl.add_circle( sa.x, sa.y, radius, line, 24, 1.0f );
				return;
			}
			// One convex outline: a line plus end circles would stack their alpha.
			constexpr int cap_segments = 10;
			constexpr auto pi = std::numbers::pi_v<float>;
			const auto phi = std::atan2( dy, dx );
			std::array<float, ( cap_segments + 1 ) * 4> points{};
			std::size_t n{};
			for ( int s = 0; s <= cap_segments; ++s )
			{
				const auto angle = phi + pi * 0.5f + pi * static_cast<float>( s ) / cap_segments;
				points[ n++ ] = sa.x + std::cos( angle ) * radius;
				points[ n++ ] = sa.y + std::sin( angle ) * radius;
			}
			for ( int s = 0; s <= cap_segments; ++s )
			{
				const auto angle = phi - pi * 0.5f + pi * static_cast<float>( s ) / cap_segments;
				points[ n++ ] = sb.x + std::cos( angle ) * radius;
				points[ n++ ] = sb.y + std::sin( angle ) * radius;
			}
			const std::span<const float> poly( points.data( ), n );
			dl.add_convex_poly_filled( poly, fill );
			dl.add_polyline( poly, line, true, 1.0f );
		}

		[[nodiscard]] float segment_distance( const foundation::vec3& p, const foundation::vec3& a, const foundation::vec3& b )
		{
			const auto ab = b - a;
			const auto length = ab.dot( ab );
			const auto t = length > 0.0f ? std::clamp( ( p - a ).dot( ab ) / length, 0.0f, 1.0f ) : 0.0f;
			return p.distance( a + ab * t );
		}

		// Hitboxes element: the capsules of the player, the one the aimbot picked
		// highlighted, the exact aim point and the estimated chance to hit it.
		void paint_aim_hitboxes( zdraw::draw_list& dl, const game::skeleton_reader::data& bones,
			const game::player_snapshot& p, const editor_cfg::element& e, const aim_marker& aim )
		{
			const auto eye = game::camera( ).origin( );
			const auto targeted = aim && aim->pawn == p.pawn;
			const game::hitbox_catalog::entry* picked{};
			if ( targeted )
			{
				auto best = 1e9f;
				for ( const auto& hb : p.hitboxes )
				{
					if ( hb.radius <= 0.0f || hb.bone < 0 || hb.bone >= static_cast<int>( bones.bones.size( ) ) ) continue;
					const auto& bone = bones.bones[ static_cast<std::size_t>( hb.bone ) ];
					const auto a = bone.position + foundation::rotate( bone.rotation, hb.mins );
					const auto b = bone.position + foundation::rotate( bone.rotation, hb.maxs );
					const auto d = segment_distance( aim->point, a, b ) - hb.radius;
					if ( d < best ) { best = d; picked = &hb; }
				}
			}
			for ( const auto& hb : p.hitboxes )
			{
				if ( hb.radius <= 0.0f || hb.bone < 0 || hb.bone >= static_cast<int>( bones.bones.size( ) ) ) continue;
				const auto& bone = bones.bones[ static_cast<std::size_t>( hb.bone ) ];
				const auto a = bone.position + foundation::rotate( bone.rotation, hb.mins );
				const auto b = bone.position + foundation::rotate( bone.rotation, hb.maxs );
				foundation::vec2 sa{}, sb{};
				float radius{};
				if ( !project_capsule( eye, a, b, hb.radius, sa, sb, radius ) ) continue;
				const auto chosen = &hb == picked;
				paint_capsule( dl, sa, sb, radius,
					with_alpha( e.color, chosen ? 0.42f : 0.16f ),
					with_alpha( e.color, chosen ? 1.0f : 0.55f ) );
			}
			if ( !targeted ) return;

			const auto point = game::camera( ).project( aim->point );
			if ( !game::camera( ).projection_valid( point ) ) return;
			dl.add_circle_filled( point.x, point.y, 3.5f, { 0, 0, 0, 200 }, 12 );
			dl.add_circle_filled( point.x, point.y, 2.5f, aim->ready ? zdraw::rgba{ 120, 235, 120, 255 } : e.color, 12 );
			if ( aim->hit_chance < 0.0f ) return;
			editor_cfg::element label_style = e;
			label_style.shadow = true;
			label_style.outline = true;
			label_style.scale = 1.0f;
			label_style.font = 2;
			auto font = editor_font( editor_cfg::delay, label_style );
			const auto label = std::format( "{:.0f}%", std::clamp( aim->hit_chance, 0.0f, 100.0f ) );
			const auto [ w, h ] = zdraw::measure_text( label, &font );
			static_cast<void>( w );
			styled_text( dl, point.x + 6.0f, point.y - h * 0.5f, label, font, label_style, { 255, 255, 255, 255 } );
		}

		void paint_editor_bar( zdraw::draw_list& dl, float x, float y, float w, float h,
			float fraction, const editor_cfg::element& e )
		{
			const auto frame = e.outline ? with_alpha( e.outline_color, 0.9f ) : zdraw::rgba{ 0, 0, 0, 170 };
			dl.add_rect_filled( x - 1.0f, y - 1.0f, w + 2.0f, h + 2.0f, frame );
			const auto vertical = h > w;
			fraction = std::clamp( fraction, 0.0f, 1.0f );
			const auto fill = std::floorf( ( vertical ? h : w ) * fraction );
			if ( fill <= 0.0f ) return;
			const auto fx = x;
			const auto fy = vertical ? y + h - fill : y;
			const auto fw = vertical ? w : fill;
			const auto fh = vertical ? fill : h;
			const zdraw::rgba red{ 232, 68, 68, 255 };
			const auto base = e.bar_style == 2 ? mix( red, e.color, fraction ) : e.color;
			if ( e.bar_style == 0 )
			{
				dl.add_rect_filled( fx, fy, fw, fh, base );
				return;
			}
			const auto low = e.bar_style == 2 ? with_alpha( red, e.color.a / 255.0f ) : with_alpha( base, 0.45f );
			if ( vertical )
				dl.add_rect_filled_multi_color( fx, fy, fw, fh, base, base, low, low );
			else
				dl.add_rect_filled_multi_color( fx, fy, fw, fh, low, base, base, low );
		}

		void paint_editor_player( zdraw::draw_list& dl, const game::bounds_projector::data& bounds,
			const game::skeleton_reader::data& bones, const game::player_snapshot& p,
			const editor_cfg::layout& layout, const aim_marker& aim, float now )
		{
			const auto bmin_x = std::floorf( bounds.min.x );
			const auto bmin_y = std::floorf( bounds.min.y );
			const auto bmax_x = std::floorf( bounds.max.x );
			const auto bmax_y = std::floorf( bounds.max.y );

			// Box-level elements.
			if ( const auto& e = layout.elements[ editor_cfg::hitboxes ]; e.on )
				paint_aim_hitboxes( dl, bones, p, e, aim );
			if ( const auto& e = layout.elements[ editor_cfg::skeleton ]; e.on )
			{
				for ( const auto& [ from, to ] : player_t::skeleton_connections )
				{
					const auto a = game::camera( ).project( bones.get_position( from ) );
					const auto b = game::camera( ).project( bones.get_position( to ) );
					if ( !game::camera( ).projection_valid( a ) || !game::camera( ).projection_valid( b ) ) continue;
					if ( e.shadow )
						dl.add_line( a.x, a.y, b.x, b.y, with_alpha( { 0, 0, 0, 180 }, e.color.a / 255.0f ), 2.6f );
					dl.add_line( a.x, a.y, b.x, b.y, e.color, 1.2f );
				}
			}
			if ( const auto& e = layout.elements[ editor_cfg::box ]; e.on )
			{
				const auto w = bmax_x - bmin_x;
				const auto h = bmax_y - bmin_y;
				if ( e.shadow )
					dl.add_rect( bmin_x - 1.0f, bmin_y - 1.0f, w + 2.0f, h + 2.0f,
						with_alpha( { 0, 0, 0, 200 }, e.color.a / 255.0f ), 3.0f );
				dl.add_rect( bmin_x, bmin_y, w, h, e.color, 1.0f );
			}

			// Stacked elements, placed exactly like the editor preview: bars hug the
			// box first, side columns run down from the top of the box, top and
			// bottom rows grow away from it.
			constexpr float side_gap = 4.0f;
			float offset[ 4 ]{ side_gap, side_gap, side_gap, side_gap };
			float column_y[ 4 ]{ bmin_y, bmin_y, bmin_y, bmin_y };
			for ( int pass = 0; pass < 2; ++pass )
			{
				for ( const auto index : layout.order )
				{
					const int id = index;
					if ( id >= editor_cfg::count || !stacked_element( id ) ) continue;
					const auto& e = layout.elements[ static_cast<std::size_t>( id ) ];
					if ( !e.on ) continue;
					const auto bar = bar_element( id );
					const auto side = static_cast<int>( e.side );
					const auto vertical = e.side == editor_cfg::left || e.side == editor_cfg::right;
					if ( ( pass == 0 ) != bar && vertical ) continue;
					if ( pass == 1 && !vertical ) continue;

					const auto content = content_of( id, e, p, aim, now );
					if ( !content.present ) continue;

					// Extent of the element on its side.
					auto font = editor_font( id, e );
					float ext_w{}, ext_h{}, avatar_w{};
					ImTextureID avatar_texture{};
					if ( bar )
					{
						const auto thickness = std::max( 1.0f, std::floorf( 2.0f * e.scale + 0.5f ) );
						ext_w = vertical ? thickness : bmax_x - bmin_x;
						ext_h = vertical ? bmax_y - bmin_y : thickness;
					}
					else
					{
						const auto [ tw, th ] = zdraw::measure_text( content.text, &font );
						if ( id == editor_cfg::name && e.avatar && p.steamid != 0 )
						{
							if ( const auto texture = features::visuals::avatars( ).texture( p.steamid ) )
							{
								avatar_texture = texture;
								avatar_w = std::floorf( th ) + 3.0f;
							}
						}
						auto count_w = 0.0f;
						if ( id == editor_cfg::weapon_icon && e.ammo_count && p.weapon.max_ammo > 0 )
						{
							auto count_font = editor_font( editor_cfg::weapon_text, e );
							count_w = zdraw::measure_text( std::to_string( std::max( p.weapon.ammo, 0 ) ), &count_font ).first + 3.0f;
						}
						ext_w = std::floorf( tw + 2.0f + avatar_w + count_w );
						ext_h = std::floorf( th + 1.0f );
					}

					float cx{}, cy{};
					if ( vertical && !bar )
					{
						cx = e.side == editor_cfg::left ? bmin_x - offset[ side ] - ext_w * 0.5f
							: bmax_x + offset[ side ] + ext_w * 0.5f;
						cy = column_y[ side ] + ext_h * 0.5f;
						column_y[ side ] += ext_h + 1.0f;
					}
					else
					{
						switch ( e.side )
						{
						case editor_cfg::left: cx = bmin_x - offset[ side ] - ext_w * 0.5f; cy = ( bmin_y + bmax_y ) * 0.5f; break;
						case editor_cfg::right: cx = bmax_x + offset[ side ] + ext_w * 0.5f; cy = ( bmin_y + bmax_y ) * 0.5f; break;
						case editor_cfg::top: cx = ( bmin_x + bmax_x ) * 0.5f; cy = bmin_y - offset[ side ] - ext_h * 0.5f; break;
						default: cx = ( bmin_x + bmax_x ) * 0.5f; cy = bmax_y + offset[ side ] + ext_h * 0.5f; break;
						}
						offset[ side ] += ( vertical ? ext_w : ext_h ) + 2.0f;
					}
					const auto mn_x = std::floorf( cx - ext_w * 0.5f );
					const auto mn_y = std::floorf( cy - ext_h * 0.5f );

					if ( bar )
					{
						paint_editor_bar( dl, mn_x, mn_y, ext_w, ext_h, content.fraction, e );
						continue;
					}
					if ( e.background )
						dl.add_rect_filled( mn_x - 2.0f, mn_y, ext_w + 4.0f, ext_h, { 0, 0, 0, 140 } );

					auto color = e.color;
					if ( id == editor_cfg::weapon_icon && p.weapon.pin_pulled )
						color = { e.pin_color.r, e.pin_color.g, e.pin_color.b, e.color.a };
					const auto [ tw, th ] = zdraw::measure_text( content.text, &font );
					const auto text_x = mn_x + 1.0f + avatar_w;
					const auto text_y = mn_y + ( ext_h - th ) * 0.5f;
					auto* im = dl.m_im_draw_list;
					const auto first_vertex = im ? im->VtxBuffer.Size : 0;
					styled_text( dl, text_x, text_y, content.text, font, e, color );
					if ( icon_element( id ) && e.flip && im )
					{
						// Mirror what the icon just emitted around its own centre.
						const auto mirror = text_x + tw * 0.5f;
						for ( int v = first_vertex; v < im->VtxBuffer.Size; ++v )
							im->VtxBuffer[ v ].pos.x = mirror - ( im->VtxBuffer[ v ].pos.x - mirror );
					}
					if ( id == editor_cfg::weapon_icon && e.ammo_count && p.weapon.max_ammo > 0 )
					{
						auto count_font = editor_font( editor_cfg::weapon_text, e );
						const auto count = std::to_string( std::max( p.weapon.ammo, 0 ) );
						const auto count_h = zdraw::measure_text( count, &count_font ).second;
						styled_text( dl, text_x + tw + 3.0f, mn_y + ( ext_h - count_h ) * 0.5f,
							count, count_font, e, p.weapon.ammo <= 0 ? zdraw::rgba{ 225, 55, 65, 245 } : color );
					}
					if ( avatar_texture && im )
					{
						const auto size = std::floorf( th );
						const ImVec2 a_min( mn_x + 1.0f, std::floorf( cy - size * 0.5f ) );
						im->AddImageRounded( avatar_texture, a_min, ImVec2( a_min.x + size, a_min.y + size ), ImVec2( 0.0f, 0.0f ), ImVec2( 1.0f, 1.0f ),
							IM_COL32( 255, 255, 255, static_cast<int>( e.color.a * dl.m_alpha ) ), size * 0.5f );
					}
				}
			}
		}
	} // namespace

	void player_t::render( zdraw::draw_list& draw_list,
		const std::shared_ptr<const game::player_pose_frame>& frame )
	{
		const auto& cfg = config::visual_settings.m_player;
		if ( !cfg.active( ) || !frame || !frame->world )
		{
			return;
		}
		if ( cfg.spectator_sync
			&& game::world( ).local_spectated( ) ) return;

		const auto current_time = game::local_player().game_time( );
		if ( current_time <= 0.0f )
		{
			return;
		}

		if ( cfg.m_editor.active )
		{
			const auto aim = features::combat::latest_aim_preview( );
			const auto draw_one = [ & ]( const game::player_snapshot& player,
				const game::skeleton_reader::data& bones, const editor_cfg::layout& layout )
			{
				if ( !bones.is_valid( ) ) return;
				const auto bounds = game::projection_bounds( ).get( bones );
				if ( !bounds.is_valid( ) ) return;
				const auto opacity = player.legit_opacity * ( player.invulnerable ? 0.45f : 1.0f );
				zdraw::draw_list player_draw{ draw_list.m_im_draw_list, opacity };
				paint_editor_player( player_draw, bounds, bones, player, layout, aim, current_time );
			};

			// Enemies use the presentation poses, the same bones the chams follow.
			const auto& enemies = cfg.m_editor.targets[ 0 ];
			if ( enemies.any( ) )
			{
				for ( const auto& pose : frame->players )
				{
					if ( pose.source_index >= frame->world->size( ) ) continue;
					const auto& player = ( *frame->world )[ pose.source_index ];
					if ( !game::local_player( ).is_enemy( player.team ) || !player.legit_visible ) continue;
					draw_one( player, pose.bones, enemies );
				}
			}
			// Teammates are not part of the pose frame; their sampled bones are used.
			const auto& teammates = cfg.m_editor.targets[ 1 ];
			if ( teammates.any( ) )
			{
				for ( const auto& player : *frame->world )
				{
					if ( game::local_player( ).is_enemy( player.team ) || player.health <= 0 ) continue;
					draw_one( player, player.bones, teammates );
				}
			}

			if ( cfg.m_offscreen_arrows.enabled && !app::context().menu.is_open( ) )
				this->paint_offscreen_markers( draw_list, *frame->world, cfg.m_offscreen_arrows );
			return;
		}

		active_bomb_info bomb_info{};
		if ( cfg.m_info_flags.enabled &&
			cfg.m_info_flags.has( config::visual_profile::player::info_flags::flag::bomb_damage ) )
		{
			const auto sampled = features::visuals::bomb( ).player_damage_snapshot( );
			bomb_info.valid = sampled.valid;
			bomb_info.position = sampled.position;
			bomb_info.blow_time = sampled.blow_time;
			bomb_info.baked_site = sampled.baked_site;
		}

		for ( const auto& pose : frame->players )
		{
			if ( pose.source_index >= frame->world->size( ) ) continue;
			const auto& player = ( *frame->world )[ pose.source_index ];
			if ( !game::local_player().is_enemy( player.team ) )
			{
				continue;
			}
			if ( !player.legit_visible ) continue;

			if ( !pose.bones.is_valid( ) )
			{
				continue;
			}

			const auto opacity = player.legit_opacity
				* ( player.invulnerable ? 0.45f : 1.0f );
			zdraw::draw_list player_draw{ draw_list.m_im_draw_list, opacity };
			this->paint_threat_highlights(
				player_draw, pose.bones, player, current_time );
		}

		for ( const auto& pose : frame->players )
		{
			if ( pose.source_index >= frame->world->size( ) ) continue;
			const auto& player = ( *frame->world )[ pose.source_index ];
			if ( !game::local_player().is_enemy( player.team ) )
			{
				continue;
			}
			if ( !player.legit_visible ) continue;

			const auto& bones = pose.bones;
			if ( !bones.is_valid( ) )
			{
				continue;
			}

			const auto bounds = game::projection_bounds().get( bones );
			if ( !bounds.is_valid( ) )
			{
				continue;
			}
			const auto opacity = player.legit_opacity
				* ( player.invulnerable ? 0.45f : 1.0f );
			zdraw::draw_list player_draw{ draw_list.m_im_draw_list, opacity };
			zdraw::draw_list legit_bright_draw{
				draw_list.m_im_draw_list, player.legit_opacity };

			if ( cfg.m_box.enabled )
			{
				this->paint_frame( player_draw, bounds, cfg.m_box, player.is_visible );
			}

			if ( cfg.m_skeleton.enabled )
			{
				this->paint_skeleton( player_draw, bones, cfg.m_skeleton, player.is_visible );
			}

			if ( cfg.m_head_circle.enabled )
			{
				this->paint_head_marker( player_draw, bones, player, cfg.m_head_circle );
			}

			if ( cfg.m_view_line.enabled )
			{
				this->paint_view_direction( player_draw, bones, player, cfg.m_view_line );
			}

			if ( cfg.m_health_bar.enabled )
			{
				this->paint_health_meter( player_draw, bounds, player, cfg.m_health_bar );
			}

			if ( cfg.m_armor_bar.enabled && player.armor > 0 )
			{
				this->paint_armor_meter( player_draw, bounds, player, cfg.m_armor_bar );
			}

			if ( cfg.m_name.enabled && !player.display_name.empty( ) )
			{
				this->paint_identity( player_draw, bounds, player, cfg.m_name );
			}

			if ( cfg.m_weapon.enabled && !player.weapon.name.empty( ) )
			{
				this->paint_weapon_label( player_draw, bounds, player, cfg.m_weapon );
			}

			if ( cfg.m_info_flags.enabled || player.invulnerable )
			{
				this->paint_status_labels( player_draw, legit_bright_draw,
					bounds, player, cfg, bomb_info, current_time );
			}
		}

		// The arrows are screen-space markers; while the Neverlose window owns the
		// frame they would float over the menu, so they are held back with it.
		if ( cfg.m_offscreen_arrows.enabled && !app::context().menu.is_open( ) )
		{
			this->paint_offscreen_markers( draw_list, *frame->world, cfg.m_offscreen_arrows );
		}
	}

	void player_t::paint_offscreen_markers( zdraw::draw_list& draw_list, const std::vector<game::player_snapshot>& players, const config::visual_profile::player::offscreen_arrows& cfg )
	{
		const auto display = zdraw::get_display_size( );
		const auto center_x = static_cast<float>( display.first ) * 0.5f;
		const auto center_y = static_cast<float>( display.second ) * 0.5f;

		const auto size = std::max( 6.0f, cfg.size );
		// Keep the ring on screen even at large radii so the arrow head can never be
		// pushed past the viewport edge.
		const auto max_radius = std::max( 20.0f, std::min( center_x, center_y ) - size * 2.0f );
		const auto radius = std::clamp( cfg.radius, 20.0f, max_radius );

		// Same view-projection matrix game::camera_state::project() uses, read once.
		const auto m = game::camera().matrix( );
		if ( m[ 3 ][ 3 ] == 0.0f )
		{
			return;
		}

		static const auto animation_start = std::chrono::steady_clock::now( );
		const auto seconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now( ) - animation_start ).count( );
		const auto phase = 0.5f - 0.5f * static_cast<float>( std::cos(
			seconds * static_cast<double>( std::max( 0.05f, cfg.bloom_speed ) )
				* 2.0 * std::numbers::pi_v<double> ) );
		const auto smooth_phase = phase * phase * ( 3.0f - 2.0f * phase );
		const auto bloom_min = std::clamp( cfg.bloom_min_alpha, 0.0f, 1.0f );
		const auto bloom_max = std::max( bloom_min,
			std::clamp( cfg.bloom_max_alpha, 0.0f, 1.0f ) );
		const auto bloom_intensity = std::lerp(
			bloom_min, bloom_max, smooth_phase );
		const auto bloom_alpha = static_cast<std::uint8_t>( std::clamp(
			static_cast<float>( cfg.bloom_color.a ) * bloom_intensity, 0.0f, 255.0f ) );
		const zdraw::rgba arrow_color{ cfg.color.r, cfg.color.g, cfg.color.b, 255 };

		for ( const auto& player : players )
		{
			if ( !game::local_player().is_enemy( player.team ) || player.health <= 0
				|| !player.legit_visible )
			{
				continue;
			}

			const auto& target = player.collision_center;
			const auto clip_x = m[ 0 ][ 0 ] * target.x + m[ 0 ][ 1 ] * target.y + m[ 0 ][ 2 ] * target.z + m[ 0 ][ 3 ];
			const auto clip_y = m[ 1 ][ 0 ] * target.x + m[ 1 ][ 1 ] * target.y + m[ 1 ][ 2 ] * target.z + m[ 1 ][ 3 ];
			const auto clip_w = m[ 3 ][ 0 ] * target.x + m[ 3 ][ 1 ] * target.y + m[ 3 ][ 2 ] * target.z + m[ 3 ][ 3 ];
			if ( clip_w == 0.0f )
			{
				continue;
			}

			auto ndc_x = clip_x / clip_w;
			auto ndc_y = clip_y / clip_w;
			const bool behind = clip_w < 0.0f;

			// In front of the near plane and inside the viewport -> the player is on
			// screen and the normal ESP already covers them.
			if ( !behind && std::abs( ndc_x ) <= 1.0f && std::abs( ndc_y ) <= 1.0f )
			{
				continue;
			}

			// A target behind the camera has its projection mirrored through the
			// origin; flip it back so the arrow points the correct way.
			if ( behind )
			{
				ndc_x = -ndc_x;
				ndc_y = -ndc_y;
			}

			// Screen space: +x is right, +y is down (ndc_y points up).
			auto dir_x = ndc_x;
			auto dir_y = -ndc_y;
			const auto length = std::sqrt( dir_x * dir_x + dir_y * dir_y );
			if ( !std::isfinite( length ) || length < 1e-4f )
			{
				continue;
			}
			dir_x /= length;
			dir_y /= length;

			const auto anchor_x = center_x + dir_x * radius;
			const auto anchor_y = center_y + dir_y * radius;
			const auto perp_x = -dir_y;
			const auto perp_y = dir_x;
			const auto point = [ & ]( float forward, float side ) -> ImVec2
			{
				return {
					anchor_x + dir_x * forward + perp_x * side,
					anchor_y + dir_y * forward + perp_y * side };
			};
			auto* canvas = draw_list.m_im_draw_list;
			if ( !canvas ) continue;
			const auto saved_flags = canvas->Flags;
			canvas->Flags |= ImDrawListFlags_AntiAliasedFill | ImDrawListFlags_AntiAliasedLines;

			// Start with a regular triangle. The fourth vertex is the exact centre
			// of its base moved halfway along the altitude toward the tip.
			const auto tip_forward = size * 0.78f;
			const auto base_forward = -size * 0.52f;
			const auto tip = point( tip_forward, 0.0f );
			const auto left_base = point( base_forward, -size * 0.54f );
			const auto right_base = point( base_forward, size * 0.54f );
			const auto notch = point( base_forward + ( tip_forward - base_forward ) / 4.0f, 0.0f );

			const std::array<ImVec2, 4> perimeter{ tip, right_base, notch, left_base };

			auto player_arrow = arrow_color;
			if ( player.invulnerable )
			{
				const auto muted = static_cast<std::uint8_t>(
					( static_cast<unsigned>( player_arrow.r ) + player_arrow.g
						+ player_arrow.b ) / 3u * 0.72f );
				player_arrow.r = player_arrow.g = player_arrow.b = muted;
			}

			player_arrow.a = static_cast<std::uint8_t>( std::clamp(
				255.0f * player.legit_opacity
					* ( player.invulnerable ? 0.45f : 1.0f ), 0.0f, 255.0f ) );
			const auto fill = zdraw::draw_list::to_im_color( player_arrow );

			canvas->AddConcavePolyFilled( perimeter.data( ),
				static_cast<int>( perimeter.size( ) ), fill );

			if ( cfg.bloom && bloom_alpha > 0 )
			{
				const auto immunity_scale = ( player.invulnerable ? 0.45f : 1.0f )
					* player.legit_opacity;
				const zdraw::rgba glow{ cfg.bloom_color.r, cfg.bloom_color.g,
					cfg.bloom_color.b, static_cast<std::uint8_t>( std::clamp(
						bloom_alpha * immunity_scale, 0.0f, 255.0f ) ) };
				const auto bloom_radius = std::clamp( cfg.bloom_radius, 1.0f, 16.0f );

				chams::g_renderer.add_2d_bloom_triangle( tip.x, tip.y,
					left_base.x, left_base.y, notch.x, notch.y, bloom_radius, glow );
				chams::g_renderer.add_2d_bloom_triangle( tip.x, tip.y,
					notch.x, notch.y, right_base.x, right_base.y, bloom_radius, glow );
			}
			canvas->Flags = saved_flags;
		}
	}

	void player_t::paint_threat_highlights( zdraw::draw_list& draw_list, const game::skeleton_reader::data& bones, const game::player_snapshot& player, float current_time )
	{
		const auto& cfg = config::visual_settings.m_player.m_threat_module;
		if ( !cfg.enabled )
		{
			return;
		}

		const auto eye_pos = game::camera().origin( );

		auto& anim = this->m_animations[ player.controller ];
		if ( player.health < anim.last_health )
		{
			anim.last_damage_time = current_time;
		}
		anim.last_health = player.health;

		const auto damage_flash = std::clamp( 1.0f - ( current_time - anim.last_damage_time ) * 3.0f, 0.0f, 1.0f );

		auto draw_capsule = [ & ]( std::span<const int> bone_indices, const zdraw::rgba& base_color, bool use_color_alpha = false )
			{
				for ( const auto bone_index : bone_indices )
				{
					const game::hitbox_catalog::entry* target_hb = nullptr;
					for ( const auto& hb : player.hitboxes )
					{
						if ( hb.bone == bone_index && hb.radius > 0.0f )
						{
							target_hb = &hb;
							break;
						}
					}

					if ( !target_hb )
					{
						continue;
					}

					const auto& bone_data = bones.bones[ static_cast< std::size_t >( target_hb->bone ) ];

					// mins/maxs — это две точки оси капсулы в пространстве кости (не bounding box)
					const auto cap_a = bone_data.position + foundation::rotate( bone_data.rotation, target_hb->mins );
					const auto cap_b = bone_data.position + foundation::rotate( bone_data.rotation, target_hb->maxs );
					const auto center_world = ( cap_a + cap_b ) * 0.5f;

					const auto sa = game::camera().project( cap_a );
					const auto sb = game::camera().project( cap_b );

					if ( !game::camera().projection_valid( sa ) || !game::camera().projection_valid( sb ) )
					{
						continue;
					}

					// Экранный радиус: проецируем точку на границе капсулы перпендикулярно взгляду
					const auto view_dir = ( center_world - eye_pos ).normalized( );
					auto perp = ( cap_b - cap_a ).cross( view_dir );
					const auto pl = perp.length( );

					if ( pl < 0.001f )
					{
						perp = foundation::vec3{ 0.0f, 0.0f, 1.0f }.cross( view_dir );
						const auto pl2 = perp.length( );
						perp = pl2 > 0.001f ? perp / pl2 : foundation::vec3{ 1.0f, 0.0f, 0.0f };
					}
					else
					{
						perp = perp / pl;
					}

					const auto perp_world = perp * target_hb->radius;
					const auto p_left = game::camera().project( center_world + perp_world );
					const auto p_right = game::camera().project( center_world - perp_world );

					if ( !game::camera().projection_valid( p_left ) || !game::camera().projection_valid( p_right ) )
					{
						continue;
					}

					const auto screen_radius = std::sqrt( ( p_left.x - p_right.x ) * ( p_left.x - p_right.x ) + ( p_left.y - p_right.y ) * ( p_left.y - p_right.y ) ) * 0.5f;
					if ( screen_radius <= 0.0f || screen_radius > 500.0f )
					{
						continue;
					}

					// Альфа берётся из альфы выбранного цвета (+ подсветка при получении урона)
					const auto flash_boost = use_color_alpha ? 0.0f : damage_flash * 60.0f;
					const auto final_alpha = static_cast< std::uint8_t >( std::min( static_cast< float >( base_color.a ) + flash_boost, 255.0f ) );

					const auto fill_color = zdraw::rgba( base_color.r, base_color.g, base_color.b, final_alpha );

					// Обводка: слайдер outline_alpha масштабируется альфой цвета
					const auto outline_alpha = static_cast< std::uint8_t >( cfg.outline_alpha * ( static_cast< float >( base_color.a ) / 255.0f ) );
					const auto outline_color = zdraw::rgba( base_color.r, base_color.g, base_color.b, outline_alpha );

					// Пилюля одним выпуклым полигоном: если рисовать линию + торцевые круги,
					// их перекрытия складывают альфу и капсула становится непрозрачной
					const auto dx = sb.x - sa.x;
					const auto dy = sb.y - sa.y;
					const auto axis_len = std::sqrt( dx * dx + dy * dy );

					if ( axis_len < 1.0f )
					{
						// Ось вырождена на экране — просто круг
						draw_list.add_circle_filled( sa.x, sa.y, screen_radius, fill_color, 24 );

						if ( cfg.outline_thickness > 0.0f )
						{
							draw_list.add_circle( sa.x, sa.y, screen_radius, outline_color, 24, cfg.outline_thickness );
						}
						continue;
					}

					constexpr auto cap_segments = 12;
					constexpr auto pi = std::numbers::pi_v<float>;
					const auto phi = std::atan2( dy, dx );

					std::array<float, ( cap_segments + 1 ) * 4> pts{};
					int n = 0;

					// Полукруг вокруг A (дальняя от B сторона), затем полукруг вокруг B
					for ( int s = 0; s <= cap_segments; ++s )
					{
						const auto a = phi + pi * 0.5f + pi * static_cast< float >( s ) / cap_segments;
						pts[ n++ ] = sa.x + std::cos( a ) * screen_radius;
						pts[ n++ ] = sa.y + std::sin( a ) * screen_radius;
					}

					for ( int s = 0; s <= cap_segments; ++s )
					{
						const auto a = phi - pi * 0.5f + pi * static_cast< float >( s ) / cap_segments;
						pts[ n++ ] = sb.x + std::cos( a ) * screen_radius;
						pts[ n++ ] = sb.y + std::sin( a ) * screen_radius;
					}

					const auto poly = std::span<const float>( pts.data( ), static_cast< std::size_t >( n ) );

					draw_list.add_convex_poly_filled( poly, fill_color );

					if ( cfg.outline_thickness > 0.0f )
					{
						draw_list.add_polyline( poly, outline_color, true, cfg.outline_thickness );
					}
				}
			};

		if ( cfg.head_hitbox )
		{
			draw_capsule( threat_head_bones, cfg.head_color, true );
		}

		if ( cfg.body_hitbox )
		{
			draw_capsule( threat_body_bones, cfg.body_color );
		}

		if ( cfg.limb_hitbox )
		{
			draw_capsule( threat_limb_bones, cfg.limb_color );
		}
	}

	void player_t::paint_frame( zdraw::draw_list& draw_list, const game::bounds_projector::data& bounds, const config::visual_profile::player::box& cfg, bool is_visible )
	{
		const auto& color = is_visible ? cfg.visible_color : cfg.occluded_color;

		const auto x = std::floorf( bounds.min.x );
		const auto y = std::floorf( bounds.min.y );
		const auto w = std::floorf( bounds.max.x - bounds.min.x );
		const auto h = std::floorf( bounds.max.y - bounds.min.y );

		if ( cfg.fill )
		{

			const auto& fill_color = is_visible ? cfg.fill_visible_color : cfg.fill_occluded_color;

			draw_list.add_rect_filled( x + 1, y + 1, w - 2, h - 2, fill_color );
		}

		if ( cfg.style == config::visual_profile::player::box::style_type::full )
		{
			if ( cfg.outline )
			{
				draw_list.add_rect( x - 1, y - 1, w + 2, h + 2, zdraw::rgba( 0, 0, 0, 180 ), 1.0f );
				draw_list.add_rect( x, y, w, h, zdraw::rgba( 0, 0, 0, 200 ), 2.0f );
			}

			draw_list.add_rect( x, y, w, h, color, 1.0f );
		}
		else
		{
			const auto corner = std::min( cfg.corner_length, std::min( w, h ) * 0.4f );

			if ( cfg.outline )
			{
				draw_list.add_rect_cornered( x - 1, y - 1, w + 2, h + 2, zdraw::rgba( 0, 0, 0, 180 ), corner + 1, 1.0f );
				draw_list.add_rect_cornered( x, y, w, h, zdraw::rgba( 0, 0, 0, 200 ), corner, 2.0f );
			}

			draw_list.add_rect_cornered( x, y, w, h, color, corner, 1.0f );
		}
	}

	void player_t::paint_skeleton( zdraw::draw_list& draw_list, const game::skeleton_reader::data& bones, const config::visual_profile::player::skeleton& cfg, bool is_visible )
	{
		const auto& color = is_visible ? cfg.visible_color : cfg.occluded_color;

		for ( const auto& [from, to] : skeleton_connections )
		{
			const auto from_screen = game::camera().project( bones.get_position( from ) );
			const auto to_screen = game::camera().project( bones.get_position( to ) );

			if ( !game::camera().projection_valid( from_screen ) || !game::camera().projection_valid( to_screen ) )
			{
				continue;
			}

			draw_list.add_line( from_screen.x, from_screen.y, to_screen.x, to_screen.y, color, cfg.thickness );
		}
	}

	void player_t::paint_head_marker( zdraw::draw_list& draw_list, const game::skeleton_reader::data& bones, const game::player_snapshot& player, const config::visual_profile::player::head_circle& cfg )
	{
		const game::hitbox_catalog::entry* head_hb = nullptr;
		for ( const auto& hb : player.hitboxes )
		{
			if ( game::hitbox_data().hitgroup_from_hitbox( hb.index ) == 1 )
			{
				head_hb = &hb;
				break;
			}
		}

		if ( !head_hb )
		{
			return;
		}

		const auto& bone_data = bones.bones[ head_hb->bone ];
		const auto center_local = ( head_hb->mins + head_hb->maxs ) * 0.5f;
		const auto center_world = bone_data.position + foundation::rotate( bone_data.rotation, center_local );

		const auto screen_pos = game::camera().project( center_world );
		if ( !game::camera().projection_valid( screen_pos ) )
		{
			return;
		}

		const auto eye_pos = game::camera().origin( );
		const auto view_dir = ( center_world - eye_pos ).normalized( );
		auto perp = foundation::vec3{ 0.0f, 0.0f, 1.0f }.cross( view_dir );

		if ( perp.length_sqr( ) < 0.001f )
		{
			perp = foundation::vec3{ 1.0f, 0.0f, 0.0f };
		}
		else
		{
			perp.normalize( );
		}

		const auto edge_world = center_world + perp * head_hb->radius;
		const auto edge_screen = game::camera().project( edge_world );

		if ( !game::camera().projection_valid( edge_screen ) )
		{
			return;
		}

		const auto screen_radius = std::sqrt( ( edge_screen.x - screen_pos.x ) * ( edge_screen.x - screen_pos.x ) + ( edge_screen.y - screen_pos.y ) * ( edge_screen.y - screen_pos.y ) );

		if ( screen_radius > 0.0f && screen_radius < 500.0f )
		{
			draw_list.add_circle( screen_pos.x, screen_pos.y, screen_radius, cfg.color, 32, cfg.thickness );
		}
	}

	void player_t::paint_view_direction( zdraw::draw_list& draw_list, const game::skeleton_reader::data& bones, const game::player_snapshot& player, const config::visual_profile::player::view_line& cfg )
	{
		const auto head_pos = bones.get_position( game::rules::joint_id::head );
		if ( head_pos.x == 0.0f && head_pos.y == 0.0f && head_pos.z == 0.0f )
		{
			return;
		}

		foundation::vec3 forward{}, right{}, up{};
		foundation::basis_from_angles( player.eye_angles, forward, right, up );

		const auto end_pos = head_pos + forward * cfg.length;

		const auto screen_start = game::camera().project( head_pos );
		const auto screen_end = game::camera().project( end_pos );

		if ( game::camera().projection_valid( screen_start ) && game::camera().projection_valid( screen_end ) )
		{
			draw_list.add_line( screen_start.x, screen_start.y, screen_end.x, screen_end.y, cfg.color, cfg.thickness );
		}
	}

	void player_t::paint_health_meter( zdraw::draw_list& draw_list, const game::bounds_projector::data& bounds, const game::player_snapshot& player, const config::visual_profile::player::health_bar& cfg )
	{
		auto& anim = this->m_animations[ player.controller ];
		const auto clamped_health = std::clamp( player.health, 0, 100 );
		const auto target_fraction = clamped_health / 100.0f;

		if ( !anim.initialized || ( target_fraction - anim.health.value( ) > 0.5f ) )
		{
			anim.health.snap( target_fraction );
			anim.initialized = true;
		}
		else
		{
			anim.health.set_target( target_fraction );
			anim.health.update( );
		}

		const auto vertical = cfg.position == config::visual_profile::player::health_bar::position_type::left ||
			cfg.position == config::visual_profile::player::health_bar::position_type::right;
		const auto& layout = config::visual_settings.m_player.m_layout.health;
		const auto meter = paint_meter_body( draw_list, bounds,
			anim.health.value( ), cfg, layout, vertical );

		if ( cfg.show_value && clamped_health < 100 )
		{
			const auto* base_font = app::context().overlay.fonts( ).esp_text_11;
			if ( !base_font ) return;
			auto scaled_font = *base_font;
			scaled_font.font_size *= layout.scale;
			const auto text = std::to_string( clamped_health );
			const auto [text_w, text_h] = zdraw::measure_text( text, &scaled_font );
			const auto text_x = std::floor( meter.x + meter.width * 0.5f - text_w * 0.5f );
			const auto text_y = vertical
				? std::floor( meter.y + meter.height - meter.fill - text_h - 2.0f )
				: std::floor( meter.y - text_h - 2.0f );

			draw_list.add_text( text_x, text_y, text, &scaled_font, cfg.text_color, zdraw::text_style::outlined );
		}
	}

	void player_t::paint_armor_meter( zdraw::draw_list& draw_list, const game::bounds_projector::data& bounds, const game::player_snapshot& player, const config::visual_profile::player::armor_bar& cfg )
	{
		auto& anim = this->m_animations[ player.controller ];
		const auto clamped_armor = std::clamp( player.armor, 0, 100 );
		const auto target_fraction = clamped_armor / 100.0f;

		if ( !anim.initialized || ( target_fraction - anim.armor.value( ) > 0.5f ) )
		{
			anim.armor.snap( target_fraction );
			anim.initialized = true;
		}
		else
		{
			anim.armor.set_target( target_fraction );
			anim.armor.update( );
		}

		const auto vertical = cfg.position == config::visual_profile::player::armor_bar::position_type::left ||
			cfg.position == config::visual_profile::player::armor_bar::position_type::right;
		const auto& layout = config::visual_settings.m_player.m_layout.armor;
		const auto meter = paint_meter_body( draw_list, bounds,
			anim.armor.value( ), cfg, layout, vertical );

		if ( cfg.show_value && clamped_armor > 0 )
		{
			const auto* base_font = app::context().overlay.fonts( ).esp_text_11;
			if ( !base_font ) return;
			auto scaled_font = *base_font;
			scaled_font.font_size *= layout.scale;
			const auto text = std::to_string( clamped_armor );
			const auto [text_w, text_h] = zdraw::measure_text( text, &scaled_font );
			const auto text_x = std::floor( meter.x + meter.width * 0.5f - text_w * 0.5f );
			const auto text_y = vertical
				? std::floor( meter.y + meter.height - meter.fill - text_h - 2.0f )
				: std::floor( meter.y - text_h - 2.0f );
			draw_list.add_text( text_x, text_y, text, &scaled_font, cfg.text_color,
				zdraw::text_style::outlined );
		}
	}

	void player_t::paint_identity( zdraw::draw_list& draw_list, const game::bounds_projector::data& bounds, const game::player_snapshot& player, const config::visual_profile::player::name& cfg )
	{
		// Which weight the editor picked for this element: the overlay carries three
		// usable faces, so the choice is real in game and not just in the preview.
		const auto& loaded = app::context().overlay.fonts( );
		const auto* base_font = cfg.font == 0 ? loaded.esp_text_11
			: cfg.font == 2 ? loaded.menu_semibold_13 : loaded.notosans_medium_12;
		if ( !base_font || !base_font->im_font ) return;
		const auto& layout = config::visual_settings.m_player.m_layout.name;
		auto scaled_font = *base_font;
		scaled_font.font_size *= layout.scale;
		// The ESP editor styles the sample; the game draws the real nickname through the
		// same three knobs so the preview and the overlay cannot drift apart.
		const std::string label = config::visual_profile::player::apply_text_case( player.display_name, cfg.text_case );
		const auto [ text_w, text_h ] = zdraw::measure_text( label, &scaled_font );
		const auto anchor_x = config::visual_profile::player::resolve_layout_axis(
			bounds.min.x, bounds.max.x, layout.x, config::visual_profile::player::layout_reference_width );
		const auto anchor_y = config::visual_profile::player::resolve_layout_axis(
			bounds.min.y, bounds.max.y, layout.y, config::visual_profile::player::layout_reference_height );

		// Steam avatar in front of the nickname: the picture is fetched in the background
		// and appears as soon as it is decoded, so the label keeps its place meanwhile.
		float avatar_extent = 0.0f;
		if ( cfg.avatar && player.steamid != 0 && draw_list.m_im_draw_list )
		{
			const auto texture = features::visuals::avatars( ).texture( player.steamid );
			if ( texture )
			{
				const auto size = std::floorf(
					features::visuals::avatar_cache::draw_size( text_h ) );
				const auto gap = std::floorf( size * 0.28f );
				const auto left = std::floorf( anchor_x - ( text_w + size + gap ) * 0.5f );
				const ImVec2 min( left, std::floorf( anchor_y - size * 0.5f ) );
				const ImVec2 max( min.x + size, min.y + size );
				draw_list.m_im_draw_list->AddImageRounded( texture, min, max,
					ImVec2( 0.0f, 0.0f ), ImVec2( 1.0f, 1.0f ),
					IM_COL32( 255, 255, 255, static_cast<int>( cfg.color.a ) ), size * 0.5f );
				avatar_extent = size + gap;
			}
		}

		const auto text_x = std::floorf( anchor_x - ( text_w + avatar_extent ) * 0.5f + avatar_extent );
		const auto text_y = std::floorf( anchor_y - text_h * 0.5f );

		// Background panel behind the label. Outline belongs to the glyphs only (below).
		if ( cfg.background )
		{
			const auto pad_x = std::floorf( text_h * 0.35f );
			const auto pad_y = std::floorf( text_h * 0.18f );
			draw_list.add_rect_filled( text_x - pad_x - avatar_extent, text_y - pad_y,
				text_w + pad_x * 2.0f + avatar_extent, text_h + pad_y * 2.0f,
				cfg.background_color );
		}
		// The engine's "outlined" style uses a fixed outline colour, so the colour picked
		// in the ESP editor is applied by hand: four offset passes behind the label.
		if ( cfg.outline )
		{
			const zdraw::rgba border = cfg.border_color;
			draw_list.add_text( text_x - 1.0f, text_y, label, &scaled_font, border, zdraw::text_style::normal );
			draw_list.add_text( text_x + 1.0f, text_y, label, &scaled_font, border, zdraw::text_style::normal );
			draw_list.add_text( text_x, text_y - 1.0f, label, &scaled_font, border, zdraw::text_style::normal );
			draw_list.add_text( text_x, text_y + 1.0f, label, &scaled_font, border, zdraw::text_style::normal );
		}
		draw_list.add_text( text_x, text_y, label, &scaled_font, cfg.color, zdraw::text_style::normal );
	}

	void player_t::paint_weapon_label( zdraw::draw_list& draw_list, const game::bounds_projector::data& bounds, const game::player_snapshot& player, const config::visual_profile::player::weapon& cfg )
	{
		const auto* text_font_base = app::context().overlay.fonts( ).esp_text_11;
		const auto* icon_font_base = app::context().overlay.fonts( ).weapons_esp_15;
		if ( !text_font_base || !icon_font_base ) return;
		const auto& layout = config::visual_settings.m_player.m_layout.weapon;
		auto text_font = *text_font_base;
		auto icon_font = *icon_font_base;
		text_font.font_size *= layout.scale;
		icon_font.font_size *= layout.scale;

		const auto show_icon = cfg.display == config::visual_profile::player::weapon::display_type::icon || cfg.display == config::visual_profile::player::weapon::display_type::text_and_icon;
		const auto show_text = cfg.display == config::visual_profile::player::weapon::display_type::text || cfg.display == config::visual_profile::player::weapon::display_type::text_and_icon;
		const auto icon = this->weapon_glyph( player.weapon.name );
		const auto [ icon_w, icon_h ] = show_icon ? zdraw::measure_text( icon, &icon_font ) : std::pair{ 0.0f, 0.0f };
		const auto ammo_valid = player.weapon.max_ammo > 0 && player.weapon.ammo >= 0;
		const auto exact_ammo = cfg.ammo.enabled && cfg.ammo.show_count && ammo_valid
			? std::to_string( std::clamp( player.weapon.ammo, 0, player.weapon.max_ammo ) )
			: std::string{};
		auto weapon_text = player.weapon.name;
		if ( show_text && !show_icon && !exact_ammo.empty( ) )
			weapon_text += "  " + exact_ammo;
		const auto [ text_w, text_h ] = show_text
			? zdraw::measure_text( weapon_text, &text_font ) : std::pair{ 0.0f, 0.0f };
		const auto [ ammo_w, ammo_h ] = show_icon && !exact_ammo.empty( )
			? zdraw::measure_text( exact_ammo, &text_font ) : std::pair{ 0.0f, 0.0f };
		const auto ammo_gap = ammo_w > 0.0f ? 3.0f * layout.scale : 0.0f;
		const auto gap = show_icon && show_text ? 2.0f * layout.scale : 0.0f;
		const auto icon_row_height = std::max( icon_h, ammo_h );
		const auto total_height = ( show_icon ? icon_row_height : 0.0f ) + text_h + gap;
		const auto anchor_x = config::visual_profile::player::resolve_layout_axis(
			bounds.min.x, bounds.max.x, layout.x, config::visual_profile::player::layout_reference_width );
		const auto anchor_y = config::visual_profile::player::resolve_layout_axis(
			bounds.min.y, bounds.max.y, layout.y, config::visual_profile::player::layout_reference_height );
		// The editor can put the icon row above or below the weapon name; both rows keep
		// their own anchor so the swap cannot shift the label off the player.
		const auto top = anchor_y - total_height * 0.5f;
		const auto icon_row_y = cfg.icon_first ? top : top + ( show_text ? text_h + gap : 0.0f );
		const auto text_row_y = cfg.icon_first ? top + ( show_icon ? icon_row_height + gap : 0.0f ) : top;

		if ( show_icon )
		{
			const auto row_width = icon_w + ammo_gap + ammo_w;
			const auto icon_x = std::floorf( anchor_x - row_width * 0.5f );
			const auto icon_y = std::floorf( icon_row_y + ( icon_row_height - icon_h ) * 0.5f );
			// Colour picked in the editor, with the icon colour's transparency.
			const zdraw::rgba pin_color{ cfg.pin_color.r, cfg.pin_color.g, cfg.pin_color.b, cfg.icon_color.a };
			if ( player.weapon.pin_pulled )
			{
				// A primed grenade is a state, not an ammo fraction: paint the whole
				// glyph red so it cannot be confused with the ordinary ammo mask.
				draw_list.add_text( icon_x, icon_y, icon, &icon_font,
					pin_color, zdraw::text_style::outlined );
			}
			else if ( cfg.ammo.enabled && ammo_valid )
			{
				const auto ammo = std::clamp( player.weapon.ammo, 0, player.weapon.max_ammo );
				const auto fraction = static_cast<float>( ammo ) / player.weapon.max_ammo;

				draw_list.add_text( icon_x, icon_y, icon, &icon_font,
					cfg.ammo.empty_color, zdraw::text_style::outlined );
				if ( fraction > 0.0f && draw_list.m_im_draw_list )
				{
					const auto split_x = icon_x + icon_w * ( 1.0f - fraction );
					draw_list.m_im_draw_list->PushClipRect(
						{ split_x, icon_y - 2.0f },
						{ icon_x + icon_w + 2.0f, icon_y + icon_h + 2.0f }, true );
					draw_list.add_text( icon_x, icon_y, icon, &icon_font,
						cfg.icon_color, zdraw::text_style::normal );
					draw_list.m_im_draw_list->PopClipRect( );
				}
			}
			else
			{
				draw_list.add_text( icon_x, icon_y, icon, &icon_font,
					cfg.icon_color, zdraw::text_style::outlined );
			}
			if ( !exact_ammo.empty( ) )
			{
				const auto ammo_x = std::floorf( icon_x + icon_w + ammo_gap );
				const auto ammo_y = std::floorf(
					icon_row_y + ( icon_row_height - ammo_h ) * 0.5f );
				draw_list.add_text( ammo_x, ammo_y, exact_ammo, &text_font,
					player.weapon.pin_pulled ? pin_color : cfg.text_color,
					zdraw::text_style::outlined );
			}
		}
		if ( show_text )
		{
			const std::string label = config::visual_profile::player::apply_text_case( weapon_text, cfg.text_case );
			const auto [ label_w, label_h ] = zdraw::measure_text( label, &text_font );
			static_cast<void>( label_h );
			draw_list.add_text( std::floorf( anchor_x - label_w * 0.5f ),
				std::floorf( text_row_y ), label, &text_font,
				cfg.text_color, cfg.text_outline ? zdraw::text_style::outlined : zdraw::text_style::normal );
		}
	}

	void player_t::paint_status_labels( zdraw::draw_list& draw_list,
		zdraw::draw_list& bright_draw_list,
		const game::bounds_projector::data& bounds,
		const game::player_snapshot& player,
		const config::visual_profile::player& cfg,
		const active_bomb_info& bomb, float current_time )
	{
		const auto* base_font = app::context().overlay.fonts( ).esp_text_11;
		const auto* icon_font = app::context().overlay.fonts( ).weapons_esp_15;
		if ( !base_font || !base_font->im_font ) return;
		const auto& layout = config::visual_settings.m_player.m_layout.flags;
		const auto x = std::floorf( config::visual_profile::player::resolve_layout_axis(
			bounds.min.x, bounds.max.x, layout.x, config::visual_profile::player::layout_reference_width ) );
		auto y = std::floorf( config::visual_profile::player::resolve_layout_axis(
			bounds.min.y, bounds.max.y, layout.y, config::visual_profile::player::layout_reference_height ) );

		using info_flags = config::visual_profile::player::info_flags;
		struct status_line
		{
			std::string text;
			const info_flags::style* style{};
			status_icon icon{};
		};
		std::array<status_line, 9> lines{};
		std::size_t line_count{};
		const auto append = [ & ]( bool visible, std::string text,
			const info_flags::style& style, status_icon icon )
		{
			if ( visible && line_count < lines.size( ) )
				lines[ line_count++ ] = { std::move( text ), &style, icon };
		};
		const auto& flags = cfg.m_info_flags;

		if ( flags.has( info_flags::flag::bomb_damage ) && bomb.valid && bomb.blow_time > current_time )
		{
			const auto damage = bomb.baked_site >= 0
				? game::blast_damage().predicted_damage(
					player.collision_center, bomb.baked_site, player.eye_angles, player.is_ducked )
				: -1;
			append( damage > 0, std::format( "-{} HP", damage ),
				flags.bomb_damage_style, status_icon::bomb );
		}

		using flag = info_flags::flag;
		if ( flags.has( flag::money ) )
			append( true, std::format( "${}", player.money ),
				flags.money_style, status_icon::money );
		append( flags.has( flag::armor ) && player.armor > 0,
			player.has_helmet ? "hk" : "k", flags.armor_style, status_icon::armor );
		append( flags.has( flag::scoped ) && player.is_scoped, "zoom",
			flags.scoped_style, status_icon::scoped );
		append( flags.has( flag::defusing ) && player.is_defusing, "defusing",
			flags.defusing_style, status_icon::defusing );
		if ( flags.has( flag::ping ) )
			append( true, std::format( "{}ms", player.ping ),
				flags.ping_style, status_icon::ping );
		if ( flags.has( flag::distance ) )
		{
			const auto distance = game::camera().origin( ).distance( player.origin ) * 0.01905f;
			append( true, std::format( "{:.0f}m", distance ),
				flags.distance_style, status_icon::distance );
		}
		// Icon flags are deliberately appended last so they always sit below every
		// textual status regardless of which subset is currently enabled.
		append( flags.has( flag::kit ) && player.has_defuser, "kit",
			flags.kit_style, status_icon::kit );
		append( flags.has( flag::flashed ) && player.is_flashed, "flashed",
			flags.flashed_style, status_icon::flashed );

		for ( std::size_t line_index{}; line_index < line_count; ++line_index )
		{
			const auto& line = lines[ line_index ];
			auto line_font = *base_font;
			line_font.font_size *= layout.scale * line.style->scale;
			if ( line.icon == status_icon::kit )
			{
				if ( icon_font && icon_font->im_font )
				{
					auto kit_font = *icon_font;
					kit_font.font_size = 15.0f * layout.scale * line.style->scale;
					// Verified from the embedded font cmap/preview: 'r' is the pliers glyph.
					draw_list.add_text( x, y, "r", &kit_font, line.style->color,
						zdraw::text_style::outlined );
					y += zdraw::measure_text( "r", &kit_font ).second;
				}
				else
				{
					paint_status_icon( draw_list, status_icon::kit, x, y,
						line_font.font_size, line.style->color );
					y += line_font.font_size + 1.0f;
				}
			}
			else if ( line.icon == status_icon::flashed && icon_font && icon_font->im_font )
			{
				auto flash_font = *icon_font;
				flash_font.font_size = 15.0f * layout.scale * line.style->scale;
				// 'i' is the flashbang glyph in the bundled CS weapon font.
				draw_list.add_text( x, y, "i", &flash_font, line.style->color,
					zdraw::text_style::outlined );
				y += zdraw::measure_text( "i", &flash_font ).second;
			}
			else
			{
				draw_list.add_text( x, y, line.text, &line_font, line.style->color,
					zdraw::text_style::outlined );
				y += zdraw::measure_text( line.text, &line_font ).second;
			}
		}
		if ( player.invulnerable )
		{
			auto immune_font = *base_font;
			immune_font.font_size *= layout.scale;
			bright_draw_list.add_text( x, y, "IMMUNE", &immune_font,
				{ 255, 220, 110, 255 }, zdraw::text_style::outlined );
		}
	}

	std::string_view player_t::weapon_glyph( const std::string_view weapon_name )
	{
		using icon_entry = std::pair<std::string_view, std::string_view>;
		static constexpr auto icons = std::to_array<icon_entry>( {
			{ "knife_ct", "]" }, { "knife_t", "[" }, { "knife", "]" },
			{ "deagle", "A" }, { "elite", "B" }, { "fiveseven", "C" }, { "glock", "D" },
			{ "revolver", "J" }, { "hkp2000", "E" }, { "p250", "F" }, { "usp_silencer", "G" },
			{ "tec9", "H" }, { "cz75a", "I" }, { "mac10", "K" }, { "ump45", "L" },
			{ "bizon", "M" }, { "mp7", "N" }, { "mp9", "R" }, { "p90", "O" },
			{ "mp5sd", "N" }, { "galilar", "Q" }, { "famas", "R" }, { "m4a1_silencer", "T" },
			{ "m4a1", "S" }, { "aug", "U" }, { "sg556", "V" }, { "ak47", "W" },
			{ "g3sg1", "X" }, { "scar20", "Y" }, { "awp", "Z" }, { "ssg08", "a" },
			{ "xm1014", "b" }, { "sawedoff", "c" }, { "mag7", "d" }, { "nova", "e" },
			{ "negev", "f" }, { "m249", "g" }, { "taser", "h" }, { "flashbang", "i" },
			{ "hegrenade", "j" }, { "smokegrenade", "k" }, { "molotov", "l" },
			{ "decoy", "m" }, { "incgrenade", "n" }, { "c4", "o" }
		} );
		const auto match = std::ranges::find( icons, weapon_name,
			[]( const auto& entry ) { return entry.first; } );
		if ( match == icons.end( ) ) return "?";
		return match->second;
	}

} // namespace features::visuals
