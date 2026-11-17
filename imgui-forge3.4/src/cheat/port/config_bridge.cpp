#include <port/config_bridge.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

#include <config/settings.hpp>
#include <config/misc.hpp>
#include <config/visuals.hpp>
#include <config/combat.hpp>
#include <app/workers.hpp>

#include "menu/config.h"
#include "menu/agent_models.h"
#include "menu/inventory_catalog.h"

#include <string_view>

namespace nl_cheat {
namespace {

	[[nodiscard]] zdraw::rgba to_rgba( const ImVec4 color )
	{
		const auto clamp = []( const float value )
		{
			return value < 0.0f ? 0.0f : ( value > 1.0f ? 1.0f : value );
		};
		return zdraw::rgba{
			static_cast<std::uint8_t>( clamp( color.x ) * 255.0f + 0.5f ),
			static_cast<std::uint8_t>( clamp( color.y ) * 255.0f + 0.5f ),
			static_cast<std::uint8_t>( clamp( color.z ) * 255.0f + 0.5f ),
			static_cast<std::uint8_t>( clamp( color.w ) * 255.0f + 0.5f ),
		};
	}

	// NL material selector -> vesta chams material.
	[[nodiscard]] int map_material( const int material )
	{
		using mat = config::visual_profile::chams;
		switch ( material )
		{
		case 1: return mat::shaded;   // Flat
		case 2: return mat::glossy;   // Glossy
		case 3: return mat::glow;         // Glow
		case 4: return mat::shaded;       // Metallic (shaded, full metalness)
		case 5: return mat::solid;        // Wireframe (solid + wireframe flag)
		case 6: return mat::water_flow;   // Water Flow
		case 7: return mat::glow_outline; // Glow Outline
		case 8: return mat::iridescent;   // Iridescent
		default: return mat::solid;
		}
	}

	void apply_layer( config::visual_profile::chams::material& out,
		const nl::ModelLayer& layer, const bool wireframe = false )
	{
		using mat = config::visual_profile::chams;
		out.enabled = layer.material != 0;
		out.type = map_material( layer.material );
		out.wireframe = wireframe || layer.material == 5;
		out.color = to_rgba( layer.color );

		// The menu only picks a colour, so each material carries its own tuning.
		out.metalness = layer.material == 4 ? 1.0f : 0.0f;
		out.roughness = layer.material == 4 ? 0.28f : 0.45f;
		out.strength = 1.0f;
		out.speed = 1.0f;
		switch ( out.type )
		{
		case mat::glow:
			out.exponent = 2.2f;
			break;
		case mat::glow_outline:
			out.exponent = 1.6f;
			out.falloff = 0.45f;
			out.fresnel_fill = 0.12f;
			break;
		case mat::glossy:
			out.exponent = 3.0f;
			out.falloff = 0.4f;
			out.fresnel_fill = 0.55f;
			out.tint = { 255, 255, 255, 255 };
			break;
		case mat::iridescent:
			out.strength = 0.75f;
			out.roughness = 0.3f;
			break;
		case mat::water_flow:
			out.strength = 1.0f;
			out.speed = 1.0f;
			break;
		default:
			break;
		}
	}

	void apply_player_esp( config::visual_profile& visual, const nl::Config& c )
	{
		// Elements the editor stacks along a side. The box, skeleton and hitboxes place
		// themselves around the player, so they never take part in the stacking.
		const auto stacked_element = []( const int index )
		{
			return index != 0 && index != 5 && index != 27;
		};

		using flag = config::visual_profile::player::info_flags::flag;
		auto& player = visual.m_player;
		const auto& esp = c.visuals.players.esp[ 0 ]; // Enemies

		player.enabled = c.visuals.players.enabled;
		player.m_box.enabled = esp[ 0 ].on;
		player.m_box.style = config::visual_profile::player::box::style_type::cornered;
		player.m_box.corner_length = 10.0f;
		player.m_box.visible_color = to_rgba( esp[ 0 ].color );
		player.m_box.occluded_color = to_rgba( esp[ 0 ].color );

		player.m_name.enabled = esp[ 1 ].on;
		player.m_name.color = to_rgba( esp[ 1 ].color );
		player.m_name.outline = esp[ 1 ].outline;
		player.m_name.avatar = esp[ 1 ].avatar;
		player.m_name.background = esp[ 1 ].background;
		player.m_name.font = std::clamp( esp[ 1 ].font, 0, 2 );
		player.m_name.border_color = to_rgba( esp[ 1 ].outline_color );
		player.m_name.text_case = static_cast<int>( esp[ 1 ].text_style );

		// The editor's sides and order become the engine's layout coordinates: the box is
		// 0..1 on both axes, and every element on the same side is spread along it in the
		// order the user arranged, so the preview and the overlay agree.
		{
			int side_counts[ 4 ]{};
			int side_ranks[ nl::esp_element_count ]{};
			for ( int index = 0; index < nl::esp_element_count; ++index )
			{
				if ( !esp[ index ].on || !stacked_element( index ) )
					continue;
				const auto side = static_cast<int>( esp[ index ].side );
				side_ranks[ index ] = side_counts[ side ]++;
			}
			const auto place = [ & ]( const int index, config::visual_profile::player::layout_element& out )
			{
				const auto side = static_cast<int>( esp[ index ].side );
				const auto count = side_counts[ side ] > 1 ? side_counts[ side ] : 1;
				// Evenly spread (rank + 1) / (count + 1): a single element lands centred,
				// which is what the engine's own defaults do.
				const auto along = static_cast<float>( side_ranks[ index ] + 1 )
					/ static_cast<float>( count + 1 );
				constexpr float gap = 0.0177f;
				switch ( esp[ index ].side )
				{
				case nl::EspSide::left: out.x = -gap; out.y = along; break;
				case nl::EspSide::right: out.x = 1.0f + gap; out.y = along; break;
				case nl::EspSide::top: out.x = along; out.y = -gap; break;
				default: out.x = along; out.y = 1.0f + gap; break;
				}
				out.scale = std::clamp( esp[ index ].scale, 0.5f, 2.5f );
			};
			place( 1, player.m_layout.name );
			place( 3, player.m_layout.health );
			// The weapon label is one engine element: the text half drives its place and
			// size, and the icon's own order decides which of the two is drawn first.
			place( esp[ 21 ].on ? 21 : 22, player.m_layout.weapon );
			player.m_weapon.icon_first = !esp[ 22 ].on || !esp[ 21 ].on
				|| esp[ 22 ].order >= esp[ 21 ].order;
			player.m_weapon.text_outline = esp[ 21 ].outline;
			player.m_weapon.text_case = static_cast<int>( esp[ 21 ].text_style );
		player.m_weapon.ammo.show_count = esp[ 22 ].ammo_count;
		player.m_weapon.pin_color = to_rgba( esp[ 22 ].pin_color );
		}

		player.m_health_bar.enabled = esp[ 3 ].on;
		player.m_health_bar.full_color = to_rgba( esp[ 3 ].color );
		player.m_health_bar.low_color = to_rgba( esp[ 3 ].color );
		// A thin bar with the fill mode the editor picked: normal, fixed two-tone or a
		// value gradient.
		player.m_health_bar.thickness = std::clamp( 2.0f * esp[ 3 ].scale, 1.0f, 8.0f );
		player.m_health_bar.gradient = esp[ 3 ].bar_style == nl::EspBar::gradient;
		player.m_health_bar.outline = esp[ 3 ].outline;

		player.m_weapon.ammo.enabled = esp[ 4 ].on || esp[ 23 ].on;
		player.m_skeleton.enabled = esp[ 5 ].on;
		player.m_skeleton.visible_color = to_rgba( esp[ 5 ].color );
		player.m_skeleton.occluded_color = to_rgba( esp[ 5 ].color );

		const bool weapon_text = esp[ 21 ].on;
		const bool weapon_icon = esp[ 22 ].on;
		player.m_weapon.enabled = weapon_text || weapon_icon || player.m_weapon.ammo.enabled;
		player.m_weapon.display = weapon_icon && weapon_text
			? config::visual_profile::player::weapon::display_type::text_and_icon
			: ( weapon_icon
				? config::visual_profile::player::weapon::display_type::icon
				: config::visual_profile::player::weapon::display_type::text );
		player.m_weapon.text_color = to_rgba( esp[ 21 ].color );
		player.m_weapon.icon_color = to_rgba( esp[ 22 ].color );

		std::uint16_t info_flags = 0;
		const auto set_flag = [ &info_flags ]( const bool on, const flag f )
		{
			if ( on ) info_flags |= static_cast<std::uint16_t>( f );
		};
		set_flag( esp[ 2 ].on, flag::distance );
		set_flag( esp[ 7 ].on, flag::kit );
		set_flag( esp[ 8 ].on, flag::flashed );
		set_flag( esp[ 9 ].on, flag::scoped );
		set_flag( esp[ 16 ].on, flag::defusing );
		set_flag( esp[ 18 ].on, flag::money );
		player.m_info_flags.enabled = info_flags != 0;
		player.m_info_flags.flags = info_flags;

		player.m_hitboxes.enabled = esp[ 27 ].on;
		player.m_hitboxes.visible_color = to_rgba( esp[ 27 ].color );
		player.m_hitboxes.occluded_color = to_rgba( esp[ 27 ].color );

		// The whole editor, per target, for the overlay's layout pass.
		{
			using editor = config::visual_profile::player::editor;
			static_assert( editor::count == nl::esp_element_count,
				"the engine's element ids must follow the menu's element table" );
			auto& out = player.m_editor;
			out.active = true;
			for ( std::size_t target = 0; target < out.targets.size( ); ++target )
			{
				const auto& source = c.visuals.players.esp[ target ];
				auto& layout = out.targets[ target ];
				for ( int index = 0; index < nl::esp_element_count; ++index )
				{
					const auto& in = source[ static_cast<std::size_t>( index ) ];
					auto& e = layout.elements[ static_cast<std::size_t>( index ) ];
					e.on = in.on;
					e.side = static_cast<editor::side_type>( std::clamp( static_cast<int>( in.side ), 0, 3 ) );
					e.scale = std::clamp( in.scale, 0.5f, 2.5f );
					e.color = to_rgba( in.color );
					e.outline_color = to_rgba( in.outline_color );
					e.outline = in.outline;
					e.background = in.background;
					e.shadow = in.shadow;
					e.flip = in.flip;
					e.avatar = in.avatar;
					e.ammo_count = in.ammo_count;
					e.pin_color = to_rgba( in.pin_color );
					e.text_case = static_cast<std::uint8_t>( in.text_style );
					e.font = static_cast<std::uint8_t>( std::clamp( in.font, 0, 2 ) );
					e.bar_style = static_cast<std::uint8_t>( in.bar_style );
					layout.order[ static_cast<std::size_t>( index ) ] = static_cast<std::uint8_t>( index );
				}
				std::stable_sort( layout.order.begin( ), layout.order.end( ),
					[ &source ]( const std::uint8_t a, const std::uint8_t b )
					{
						return source[ a ].order < source[ b ].order;
					} );
			}
		}

		player.m_offscreen_arrows.enabled = c.visuals.players.offscreen_arrow.on;
		player.m_offscreen_arrows.color = to_rgba( c.visuals.players.offscreen_arrow.color );

		// Elements the ENEMY MODEL / ESP Items panels do not offer are switched off
		// explicitly. Their engine defaults are on, which is why the in-game ESP used to
		// draw a head circle, a view line and an armour bar that the menu never showed.
		player.m_head_circle.enabled = false;
		player.m_view_line.enabled = false;
		player.m_armor_bar.enabled = false;
		player.m_threat_module.enabled = false;

		visual.m_sound.enabled = c.visuals.players.sounds.on;
		visual.m_sound.color = to_rgba( c.visuals.players.sounds.color );
		visual.m_sound.duration = std::clamp( c.visuals.players.sounds.duration, 0.2f, 6.0f );
		visual.m_sound.radius = std::clamp( c.visuals.players.sounds.radius, 4.0f, 200.0f );
		visual.m_sound.local_sync = c.visuals.players.sounds.local_sync;

		// Chams from the Model layers.
		auto& chams = visual.m_chams;
		apply_layer( chams.visible, c.visuals.players.player );
		apply_layer( chams.invisible, c.visuals.players.behind_walls );
		apply_layer( chams.on_shot.appearance, c.visuals.players.on_shot );
		chams.on_shot.enabled = c.visuals.players.on_shot.material != 0;
		chams.kill_effect.enabled = c.visuals.players.soul_particles.on;
		chams.kill_effect.color = to_rgba( c.visuals.players.soul_particles.color );
		chams.glow_effect.enabled = c.visuals.players.glow.on;
		chams.glow_effect.color = to_rgba( c.visuals.players.glow.color );
		// Chams are their own feature: the Model layers draw the player models even
		// when the 2D ESP master toggle is off. Gating this on `player.enabled` used
		// to switch the whole mesh pipeline (world demand, extraction) off with the
		// ESP box, which made the chams look broken.
		chams.enabled = chams.visible.enabled || chams.invisible.enabled
			|| chams.on_shot.enabled || chams.kill_effect.enabled
			|| chams.glow_effect.enabled;
	}

	void apply_world( config::visual_profile& visual, config::general_profile& general,
		const nl::Config& c )
	{
		const auto& world = c.visuals.world;

		visual.m_bomb.enabled = world.bomb_colorize.on || world.bomb_indicator;
		visual.m_bomb.show_active_bomb = world.bomb_colorize.on;
		visual.m_bomb.active_bomb_color = to_rgba( world.bomb_colorize.color );
		visual.m_bomb.show_planted_bomb = world.bomb_indicator;
		visual.m_bomb.bomb_color_t = to_rgba( world.bomb_colorize.color );
		visual.m_bomb.bomb_color_ct = to_rgba( world.defuse_indicator.color );
		visual.m_bomb.show_timer = world.bomb_indicator_timer;
		visual.m_bomb.show_info_panel = world.window_bomb;
		visual.m_bomb.show_safe_zone = world.defuse_indicator.on;

		visual.m_item.enabled = world.weapons_colorize.on || world.weapons.enabled;
		visual.m_item.m_icon.color = to_rgba( world.weapons_colorize.color );
		visual.m_item.m_icon.enabled = true;
		visual.m_item.m_ammo.enabled = world.weapons.enabled;
		visual.m_item.m_name.enabled = world.weapons.enabled;

		visual.m_projectile.enabled = world.grenades.enabled;
		visual.m_projectile.default_color = to_rgba( world.grenades.color );

		visual.m_no_smoke.enabled = world.smoke_radius.on;
		visual.m_no_smoke.wireframe_color = to_rgba( world.smoke_radius.color );
		visual.m_projectile.show_inferno_bounds = world.molotov_radius.on;

		visual.m_radar.enabled = world.radar;
		visual.m_crosshair.enabled = world.sniper_crosshair;
		visual.m_crosshair.penetration_enabled = world.penetration_crosshair.on;
		visual.m_crosshair.penetration_color_yes = to_rgba( world.penetration_crosshair.color );

		general.m_grenades.enabled = world.grenade_trajectory;
		general.m_grenades.color = to_rgba( world.trajectory_color );
		general.m_grenades.thickness = world.trajectory_thickness;
		general.m_grenades.show_bounces = world.trajectory_bounces;

		general.m_hitmarker.enabled = world.hit_marker_crosshair.on;
		general.m_hitmarker.color = to_rgba( world.hit_marker_crosshair.color );
		general.m_hitmarker.duration = std::clamp( world.hit_marker_duration / 100.0f, 0.05f, 5.0f );
		general.m_hitsound.show_damage = world.hit_marker_damage.on;
		general.m_hitsound.damage_color = to_rgba( world.hit_marker_damage.color );

		general.m_bullet_tracers.enabled = world.tracers_local.on
			|| world.tracers_enemies.on || world.tracers_team.on;
		general.m_bullet_tracers.color = to_rgba( world.tracers_local.color );
		general.m_bullet_tracers.duration = std::clamp( world.tracer_duration / 100.0f, 0.1f, 20.0f );

		// The Neverlose menu draws the watermark and the bind list itself while it
		// is resident in the game process; only the panels it does not own are
		// mapped onto the vesta HUD.
		general.m_watermark.enabled = false;
		general.m_keybind_list.enabled = false;
		general.m_spectator_list.enabled = world.window_spectators;

		visual.m_thirdperson.enabled = world.thirdperson;
		visual.m_thirdperson.distance = std::clamp( world.thirdperson_distance, 30.0f, 300.0f );
		visual.m_thirdperson.collision = world.thirdperson_collision;
		visual.m_thirdperson.while_dead = world.thirdperson_dead;

		auto& ambience = visual.m_ambience;
		// MAP SELECTION: the entry whose name matches the map the player is on wins,
		// otherwise the Global entry (index 0) applies.
		const nl::AmbienceSettings& lighting = [ & ]
		{
			// The game reports "maps/de_inferno.vpk" or a bare id; the list stores the id.
			const auto bare = []( std::string text )
			{
				const auto slash = text.find_last_of( "/\\" );
				if ( slash != std::string::npos )
					text.erase( 0, slash + 1 );
				if ( text.size( ) > 4 && text.compare( text.size( ) - 4, 4, ".vpk" ) == 0 )
					text.resize( text.size( ) - 4 );
				return text;
			};
			const auto current = app::workers::current_map( );
			const std::string name = current ? bare( *current ) : std::string{};
			if ( !name.empty( ) )
			{
				for ( int index = 1; index < c.visuals.world.map_count; ++index )
				{
					// A map card switched off falls back to Global.
					if ( c.visuals.world.maps[ static_cast<size_t>( index ) ].used
						&& name == bare( c.visuals.world.map_name( index ) ) )
						return c.visuals.world.ambience_for( index );
				}
			}
			return c.visuals.world.ambience_for( 0 );
		}( );

		// The view has no master switch any more: ambience runs while any effect is on.
		ambience.enabled = lighting.nightmode || lighting.fullbright || lighting.sky || lighting.sunlight
			|| lighting.fog || lighting.dof || lighting.bloom || lighting.gamma
			|| std::any_of( std::begin( lighting.removals ), std::end( lighting.removals ),
				[]( const bool on ) { return on; } );
		ambience.nightmode = lighting.nightmode;
		ambience.nightmode_color = to_rgba( lighting.nightmode_color );
		ambience.nightmode_exposure = lighting.exposure;
		ambience.nightmode_overlay = lighting.overlay_transparency;
		ambience.fullbright = lighting.fullbright;
		ambience.fullbright_color = to_rgba( lighting.fullbright_color );
		ambience.fog_override = lighting.fog;
		ambience.fog_color = to_rgba( lighting.fog_color );
		ambience.fog_start = lighting.fog_start;
		ambience.fog_end = lighting.fog_end;
		ambience.fog_density = lighting.fog_density;
		ambience.fog_exponent = lighting.fog_exponent;
		ambience.bloom = lighting.bloom ? lighting.bloom_amount : -1.0f;
		ambience.sky_tint_override = lighting.sky;
		ambience.sky_tint = to_rgba( lighting.cloud_color );
		ambience.sky_brightness = lighting.sky_brightness;
		// Read by the game thread: only touched when the pick actually changes.
		if ( std::strncmp( ambience.skybox, lighting.skybox, sizeof( ambience.skybox ) ) != 0 )
			std::snprintf( ambience.skybox, sizeof( ambience.skybox ), "%.*s",
				static_cast<int>( sizeof( lighting.skybox ) ), lighting.skybox );
		ambience.sunlight = lighting.sunlight;
		ambience.sunlight_color = to_rgba( lighting.sunlight_color );
		ambience.sunlight_mode = lighting.sunlight_mode;
		ambience.vignette = lighting.vignette;
		ambience.vignette_amount = lighting.vignette_amount;
		ambience.screen_tint = lighting.screen_tint;
		ambience.screen_tint_color = to_rgba( lighting.screen_tint_color );
		ambience.tonemapping = lighting.tonemapping;
		ambience.tonemap_exposure = lighting.tonemap_exposure;
		ambience.contrast = lighting.contrast;
		ambience.contrast_amount = lighting.contrast_amount;
		ambience.contrast_brightness = lighting.contrast_brightness;
		ambience.dof = lighting.dof;
		ambience.dof_near = lighting.dof_near;
		ambience.dof_far = lighting.dof_far;
		for ( int index = 0; index < 6; ++index )
			ambience.removals[ index ] = lighting.removals[ index ];
		ambience.night_props = lighting.night_props;
		ambience.fullbright_intensity = lighting.fullbright_intensity;
		ambience.sunlight_intensity = lighting.sunlight_intensity;
		ambience.gamma = lighting.gamma;
		ambience.gamma_amount = lighting.gamma_amount;

		// Weather effects
		auto& weather = visual.m_weather;
		weather.wind_enabled = world.weather_wind;
		weather.wind_strength = world.wind_strength;
		weather.wind_direction = world.wind_direction;
		weather.wind_turbulence = world.wind_turbulence;

		weather.rain_enabled = world.weather_rain;
		weather.rain_color = to_rgba( world.rain_color );
		weather.rain_speed = world.rain_speed;
		weather.rain_density = world.rain_density;
		weather.rain_radius = world.rain_radius;
		weather.splashes = world.weather_splashes;
		weather.snow_enabled = world.weather_snow;
		weather.snow_speed = std::clamp( world.snow_speed, 0.05f, 3.0f );

		weather.lightning_enabled = world.weather_lightning;
		weather.lightning_color = to_rgba( world.lightning_color );
		weather.lightning_interval = world.lightning_interval;
		weather.lightning_duration = world.lightning_duration;
		weather.lightning_spawn_on_kill = world.lightning_spawn_on_kill;
		weather.lightning_sound = world.lightning_sound;

		weather.wetness_enabled = world.weather_model_wetness;
		weather.wetness_density = world.wetness_density;
		weather.wetness_speed = world.wetness_speed;
		weather.snow_color = to_rgba( world.snow_color );
		weather.stars_enabled = world.weather_stars;
		weather.stars_color = to_rgba( world.stars_color );
		weather.lightning_flash = world.lightning_flash;
		weather.thunder_volume = world.thunder_volume;
	}

	void apply_rage( config::combat_profile& combat, const nl::Config& c )
	{
		auto& out = combat.rage;
		const auto& in = c.rage;
		out.enabled = in.enabled;
		out.silent_aim = in.silent_aim;
		out.auto_fire = in.auto_fire;
		out.limiters = in.limiters;
		out.aim_fov = in.aim_fov;
		out.aim_speed = in.aim_speed;
		out.reaction_time = in.reaction_time;
		out.kill_delay = in.kill_delay;
		out.max_walls = in.max_walls;
		out.backtrack = in.backtrack;
		out.selftrack = in.selftrack;
		out.spoof_fps = in.spoof_fps;
		out.refine_shot = in.refine_shot;
		out.field_of_view = in.field_of_view;
		out.history = in.history;
		out.delay_shot[ 0 ] = in.delay_shot[ 0 ];
		out.delay_shot[ 1 ] = in.delay_shot[ 1 ];
		out.delay_shot[ 2 ] = in.delay_shot[ 2 ];
		out.delay_shot_time = in.delay_shot_time;
		out.remove_spread = in.remove_spread;
		out.duck_peek_assist = in.duck_peek_assist;
		out.quick_peek_assist = in.quick_peek_assist;
		out.quick_peek_color[ 0 ] = in.quick_peek_color.x;
		out.quick_peek_color[ 1 ] = in.quick_peek_color.y;
		out.quick_peek_color[ 2 ] = in.quick_peek_color.z;
		out.quick_peek_color[ 3 ] = in.quick_peek_color.w;
		out.quick_peek_retreat = in.quick_peek_retreat;
		out.quick_peek_auto_stop = in.quick_peek_auto_stop;
		out.double_precision = in.double_precision;
		out.double_tap = in.double_tap;
		out.anti_aim = in.anti_aim;
		out.anti_aim_on_use = in.anti_aim_on_use;
		out.anti_aim_freeze_time = in.anti_aim_freeze_time;
		out.yaw_base = in.yaw_base;
		out.pitch = in.pitch;
		out.pitch_offset = in.pitch_offset;
		out.yaw = in.yaw;
		out.yaw_offset = in.yaw_offset;
		out.yaw_modifier = in.yaw_modifier;
		out.yaw_modifier_degree = in.yaw_modifier_degree;
		out.mouse_override = in.mouse_override;
		out.mouse_override_speed = in.mouse_override_speed;
		out.mouse_override_return = in.mouse_override_return;

		for ( std::size_t index = 0; index < out.weapons.size( ); ++index )
		{
			const auto& src = index == 0 || in.weapons[ index ].custom ? in.weapons[ index ] : in.weapons[ 0 ];
			auto& dst = out.weapons[ index ];
			for ( int part = 0; part < 6; ++part )
				dst.hitboxes[ part ] = src.hitboxes[ part ];
			dst.extrapolation = src.extrapolation;
			dst.hit_chance = src.hit_chance;
			dst.hit_chance_double_tap = src.hit_chance_double_tap;
			dst.hit_chance_strict = src.hit_chance_strict;
			dst.min_damage = src.min_damage;
			dst.quick_stop = src.quick_stop;
			for ( int option = 0; option < 4; ++option )
				dst.quick_stop_options[ option ] = src.quick_stop_options[ option ];
			dst.quick_scope = src.quick_scope;
		}
	}

	void apply_combat( config::combat_profile& combat, const nl::Config& c )
	{
		using activation = config::combat_profile::activation;
		auto& global = combat.global;

		const bool rage = c.rage.enabled;
		// The triggerbot is its own feature: it used to require the Legit aimbot toggle,
		// so enabling only the triggerbot left it switched off every frame.
		const bool legit_aim = !rage && c.legit.enabled;
		const bool legit_trigger = !rage && c.legit.triggerbot;
		const bool legit = legit_aim || legit_trigger;

		// Rage runs on its own controller. Leaving the legit aimbot armed made both
		// of them move the mouse.
		apply_rage( combat, c );
		global.aimbot_enabled = legit_aim;
		global.triggerbot_enabled = legit_trigger;
		global.aimbot_activation_mode = activation::always;
		global.aimbot_key = 0;
		global.aimbot_draw_fov = false;

		if ( legit )
		{
			const auto fov = static_cast<int>( std::clamp( c.legit.field_of_view, 1.0f, 180.0f ) );
			global.aimbot_fov = fov;
			global.aimbot_smoothing = static_cast<int>( std::clamp(
				std::lround( c.legit.smoothing / 10.0f ), 0L, 10L ) );
			global.aimbot_humanize = static_cast<int>( std::clamp(
				std::lround( c.legit.humanize ), 0L, 100L ) );
			global.aimbot_visible_only = c.legit.visible_only;
			global.aimbot_autowall = c.legit.autowall;
			global.aimbot_multipoint = c.legit.multipoint;
			global.aimbot_min_damage = c.legit.min_damage;
			// "Neck" has no aim part of its own in the engine; it aims at the head like
			// the first entry rather than silently falling through to "everything".
			global.aimbot_hitbox_parts =
				c.legit.target == 0 || c.legit.target == 1
					? config::combat_profile::aim_part::head
				: ( c.legit.target == 2 ? config::combat_profile::aim_part::body
					: config::combat_profile::aim_part::all );
			global.aimbot_lethal_only = c.legit.lethal_only;
			global.aimbot_fov_config.selection =
				config::combat_profile::fov_settings::fixed;

			global.aimbot_checks.smoke = c.legit.smoke_check;
			global.aimbot_checks.flashed = c.legit.flash_check;
			global.aimbot_checks.airborne = c.legit.airborne;
			// Left unset, this inherited whatever the rage branch wrote last and the
			// legitbot acted on a wall policy the tab never showed.
			global.aimbot_checks.walls = std::clamp( c.legit.wall_policy, 0, 2 );

			global.aimbot_rcs.enabled = c.legit.recoil_control;
			global.aimbot_rcs.pitch = c.legit.recoil_x;
			global.aimbot_rcs.yaw = c.legit.recoil_y;

			global.aimbot_prediction.enabled = c.legit.backtrack;
			global.aimbot_prediction.max_horizon_ms = c.legit.backtrack_time;

			global.triggerbot_activation_mode = activation::always;
			global.triggerbot_enabled = legit_trigger;
			global.triggerbot_delay = static_cast<int>( c.legit.trigger_delay );
			global.triggerbot_hitchance = c.legit.trigger_hit_chance;
			global.triggerbot_reaction_time = static_cast<int>( c.legit.reaction_time );
			global.triggerbot_autostop = c.legit.trigger_autostop;
			global.triggerbot_predictive = c.legit.trigger_magnet;
			global.triggerbot_lethal_only = c.legit.trigger_lethal_only;
			global.triggerbot_min_damage_override_enabled = c.legit.trigger_min_damage_enabled;
			global.triggerbot_min_damage_override = c.legit.trigger_min_damage;
			global.triggerbot_seed_type = std::clamp( c.legit.trigger_seed_type, 0, 2 );
			// The trigger aims through the same hitboxes the user picked for the Legit
			// aimbot: leaving the engine default at "head only" made the feature look
			// dead whenever the crosshair was anywhere else.
			{
				int parts = 0;
				if ( c.legit.hitboxes[ 0 ] ) parts |= config::combat_profile::aim_part::head;
				if ( c.legit.hitboxes[ 1 ] || c.legit.hitboxes[ 2 ] )
					parts |= config::combat_profile::aim_part::body;
				if ( c.legit.hitboxes[ 3 ] ) parts |= config::combat_profile::aim_part::arms;
				if ( c.legit.hitboxes[ 4 ] || c.legit.hitboxes[ 5 ] )
					parts |= config::combat_profile::aim_part::legs;
				global.aimbot_hitbox_parts = parts ? parts
					: config::combat_profile::aim_part::head;
				global.triggerbot_hitbox_parts = parts ? parts
					: config::combat_profile::aim_part::head | config::combat_profile::aim_part::body;
			}
			global.triggerbot_checks.smoke = c.legit.trigger_smoke_check;
			global.triggerbot_checks.flashed = c.legit.trigger_flash_check;
			global.triggerbot_checks.walls = std::clamp( c.legit.wall_policy, 0, 2 );
			global.triggerbot_autowall = c.legit.trigger_autowall;
		}
		else
		{
			global.triggerbot_enabled = false;
			global.aimbot_enabled = false;
		}
		global.penetration_crosshair = c.visuals.world.penetration_crosshair.on;

		// Weapon groups belong to the rage controller now. The legit bot reads global.
		for ( auto& override : combat.overrides )
			override.use_global = true;
	}

	void apply_skins( config::skin_profile& skins, const nl::Config& c )
	{
		skins = {};
		skins.enabled = c.inventory.apply;

		for ( std::size_t side = 0; side < 2; ++side )
		{
			auto& team = skins.teams[ side ];
			for ( std::size_t slot = 0; slot < nl::inv::slot_count; ++slot )
			{
				const auto& equipped = c.inventory.loadout[ side ][ slot ];
				const auto& rolled = c.inventory.finish[ side ][ slot ];
				if ( equipped.weapon < 0 || equipped.weapon >= nl::inv::weapon_count )
					continue;
				const auto& weapon = nl::inv::weapons[ equipped.weapon ];

				if ( weapon.kind == nl::inv::Kind::agent )
				{
					if ( equipped.item < 0 || equipped.item >= nl::inv::item_count )
						continue;
					const auto* model = nl::agents::model_for_name( nl::inv::items[ equipped.item ].name );
					if ( !model )
						continue;
					std::string_view path{ model };
					if ( path.ends_with( "_c" ) )
						path.remove_suffix( 2 );
					const auto length = std::min( path.size( ), sizeof( team.agent_model ) - 1 );
					std::memcpy( team.agent_model, path.data( ), length );
					team.agent_model[ length ] = '\0';
					continue;
				}

				if ( equipped.weapon >= nl::inv::weapon_def_index_count
					|| equipped.item < 0 || equipped.item >= nl::inv::item_paint_kit_count )
					continue;
				const auto def_index = nl::inv::weapon_def_index[ equipped.weapon ];
				const auto paint_kit = nl::inv::item_paint_kit[ equipped.item ];
				if ( !def_index )
					continue;

				config::skin_kind kind = config::skin_kind::weapon;
				if ( weapon.kind == nl::inv::Kind::knife )
					kind = config::skin_kind::knife;
				else if ( weapon.kind == nl::inv::Kind::gloves )
					kind = config::skin_kind::gloves;
				else if ( weapon.kind != nl::inv::Kind::pistol && weapon.kind != nl::inv::Kind::mid_tier
					&& weapon.kind != nl::inv::Kind::rifle && weapon.kind != nl::inv::Kind::zeus )
					continue;

				// A vanilla knife still swaps the model; everything else needs a finish.
				if ( !paint_kit && kind != config::skin_kind::knife )
					continue;
				if ( team.count >= config::skin_loadout::capacity )
					break;

				auto& entry = team.entries[ team.count++ ];
				entry.kind = kind;
				entry.def_index = def_index;
				entry.finish.paint_kit = paint_kit;
				entry.finish.wear = std::clamp( rolled.wear, 0.0f, 1.0f );
				entry.finish.seed = std::clamp( rolled.seed, 0, 1000 );
				entry.finish.stat_trak = rolled.stat_trak ? std::max( rolled.kills, 0 ) : -1;
				entry.finish.quality = kind == config::skin_kind::knife || kind == config::skin_kind::gloves ? 3
					: rolled.stat_trak ? 9 : -1;
				{
					const int material = std::clamp( rolled.model_material, 0,
						static_cast<int>( std::size( nl::inv::k_model_material_type ) ) - 1 );
					entry.finish.chams_type = nl::inv::k_model_material_type[ material ];
					const auto color = to_rgba( rolled.model_color );
					entry.finish.chams_color = { color.r, color.g, color.b, color.a };
					entry.finish.chams_exponent = rolled.model_exponent;
				}
				if ( kind != config::skin_kind::gloves )
				{
					static_assert( sizeof( entry.finish.name ) == sizeof( rolled.name ) );
					std::memcpy( entry.finish.name, rolled.name, sizeof( rolled.name ) );
					entry.finish.name[ sizeof( entry.finish.name ) - 1 ] = '\0';
				}
			}
		}
	}

	void apply_misc( config::general_profile& general, const nl::Config& c )
	{
		const auto& misc = c.misc;
		general.m_bunny_hop.enabled = misc.bunny_hop;
		general.m_edge_jump.enabled = misc.edge_jump;
		general.m_auto_stop.enabled = misc.standalone_quick_stop;

		general.m_grenade_assist.auto_release = misc.automatic_grenade_release;
		general.m_grenade_assist.min_damage = std::clamp( misc.grenade_release_damage, 1.0f, 100.0f );
		general.m_grenade_assist.super_toss = misc.super_toss;
		general.m_quick_switch.enabled = misc.quick_switch;
		general.m_afk_guard.enabled = misc.prevent_afk_kick;
		general.m_knife_bot.enabled = misc.knife_bot;
		general.m_hitsound.enabled = misc.hit_sound;
		general.m_hitsound.style = std::clamp( misc.hit_sound_kind, 0, 4 );
		general.m_hitsound.volume = std::clamp( misc.hit_sound_volume / 100.0f, 0.0f, 1.0f );

		general.m_nade_helper.enabled = true;
		general.m_nade_helper.auto_release = misc.automatic_grenade_release;

		general.auto_accept = misc.auto_accept_matchmaking;

		general.m_event_log.enabled = misc.log_events[ 0 ] || misc.log_events[ 1 ]
			|| misc.log_events[ 2 ] || misc.log_events[ 3 ] || misc.log_events[ 4 ];
		general.m_event_log.show_shots = misc.log_events[ 0 ];
		general.m_event_log.show_hits = misc.log_events[ 1 ];
		general.m_event_log.show_kills = misc.log_events[ 2 ];
		general.m_event_log.show_misses = misc.log_events[ 3 ];
		general.m_event_log.show_blocked = misc.log_events[ 4 ];
		general.m_event_log.show_info = true;

		general.lua_enabled = true;
		general.limit_fps = false;
		general.use_gpu = true;
	}

} // namespace

void sync_config( const nl::Config& config )
{
	// Throttle: the memcmp walks ~15 KB and runs every presented frame (which can be
	// 300+ Hz). Run at most once per 16 ms (~60 Hz) unless the config fingerprint
	// (the preset pointer) changed, which signals an explicit load.
	//
	// The throttle stamps have to advance even when nothing changed: the old code
	// returned on the "identical" path *before* refreshing `last_sync`, so
	// `throttle_elapsed` stayed true and the full memcmp ran on every single
	// presented frame — the documented 60 Hz cap never took effect.
	static nl::Config last{};
	static const void* last_preset = nullptr;
	static std::int64_t last_sync = 0;
	const auto now = static_cast<std::int64_t>( ::GetTickCount64() );
	const void* current_preset = &config; // Proxy: in practice each preset is a distinct address
	const bool preset_changed = current_preset != last_preset;
	const bool throttle_elapsed = ( now - last_sync ) >= 16;
	if ( !preset_changed && !throttle_elapsed )
		return;
	last_sync = now;
	last_preset = current_preset;
	if ( std::memcmp( &config, &last, sizeof( config ) ) == 0 )
		return;
	last = config;

	config::visual_profile& visual = config::visual_settings;
	config::general_profile& general = config::general_settings;
	config::combat_profile& combat = config::combat_settings;
	config::skin_profile& skins = config::skin_settings;

	apply_player_esp( visual, config );
	apply_world( visual, general, config );
	apply_combat( combat, config );
	apply_misc( general, config );
	apply_skins( skins, config );

	config::publish_runtime_snapshot();
}

} // namespace nl_cheat
