#include <port/hook_hud.hpp>

#include <filesystem>

#include "imgui.h"
#include "imgui_impl_dx11.h"

#include <app/context.hpp>
#include <config/settings.hpp>
#include <features/aimbot/aimbot.hpp>
#include <features/combat/ragebot.hpp>
#include <features/misc/ambience.hpp>
#include <features/world/world_hooks.hpp>
#include <features/misc/misc.hpp>
#include <features/visuals/visuals.hpp>
#include <render/chams/preview.hpp>
#include <render/chams/renderer.hpp>
#include <render/overlay/hud.hpp>
#include <render/overlay/render_policy.hpp>
#include <resources/fonts/notosans_medium.hpp>
#include <resources/fonts/weapons.hpp>
#include <port/probe.hpp>
#include <scripting/runtime.hpp>
#include <system/runtime_events.hpp>

namespace hook_hud {
namespace {

	std::atomic<bool> g_chams_ready{ false };
	std::atomic<std::uint64_t> g_frames_rendered{ 0 };

	void load_fonts( ID3D11Device* device, ID3D11DeviceContext* context,
		std::uint32_t reference_width, std::uint32_t reference_height,
		float dpi_scale )
	{
		auto* atlas = ImGui::GetIO().Fonts;
		if ( !atlas )
			return;

		auto& overlay = app::context().overlay;
		auto& fonts = overlay.fonts_for_setup();

		fonts.notosans_medium_12 = zdraw::add_font_from_memory(
			resources::fonts::notosans_medium, static_cast<int>( sizeof( resources::fonts::notosans_medium ) ),
			12.0f, 512, 512, zdraw::font_raster_profile::smooth,
			atlas->GetGlyphRangesCyrillic() );
		fonts.esp_text_11 = zdraw::add_font_from_memory(
			resources::fonts::notosans_medium, static_cast<int>( sizeof( resources::fonts::notosans_medium ) ),
			11.0f, 512, 512, zdraw::font_raster_profile::esp_text,
			atlas->GetGlyphRangesCyrillic() );

		static constexpr ImWchar no_fallback_preload[]{ 0 };
		if ( std::filesystem::exists( "C:/Windows/Fonts/seguisym.ttf" ) )
			static_cast<void>( zdraw::merge_font_from_file( fonts.esp_text_11,
				"C:/Windows/Fonts/seguisym.ttf", 11.0f,
				zdraw::font_raster_profile::esp_text, no_fallback_preload ) );
		if ( std::filesystem::exists( "C:/Windows/Fonts/seguiemj.ttf" ) )
			static_cast<void>( zdraw::merge_font_from_file( fonts.esp_text_11,
				"C:/Windows/Fonts/seguiemj.ttf", 11.0f,
				zdraw::font_raster_profile::esp_text, no_fallback_preload ) );

		fonts.weapons_15 = zdraw::add_font_from_memory(
			resources::fonts::weapons, static_cast<int>( sizeof( resources::fonts::weapons ) ),
			16.0f, 512, 512, zdraw::font_raster_profile::smooth );
		fonts.weapons_esp_15 = zdraw::add_font_from_memory(
			resources::fonts::weapons, static_cast<int>( sizeof( resources::fonts::weapons ) ),
			16.0f, 512, 512, zdraw::font_raster_profile::esp_icon );
		if ( fonts.weapons_esp_15 && fonts.weapons_15 )
			fonts.weapons_esp_15->plain_im_font = fonts.weapons_15->im_font;

		const auto menu_dpi = std::clamp( dpi_scale, 1.0f, 1.5f );
		fonts.menu_regular_12 = zdraw::add_font_from_file( "C:/Windows/Fonts/segoeui.ttf", 16.0f * menu_dpi );
		fonts.menu_semibold_13 = zdraw::add_font_from_file( "C:/Windows/Fonts/seguisb.ttf", 16.0f * menu_dpi );
		fonts.menu_brand_30 = zdraw::add_font_from_file( "C:/Windows/Fonts/segoeuib.ttf", 38.0f * menu_dpi );

		const auto font_ready = []( const zdraw::font* font )
		{
			return font && font->im_font;
		};
		if ( !font_ready( fonts.notosans_medium_12 )
			|| !font_ready( fonts.esp_text_11 )
			|| !font_ready( fonts.weapons_15 )
			|| !font_ready( fonts.weapons_esp_15 )
			|| !font_ready( fonts.menu_regular_12 )
			|| !font_ready( fonts.menu_semibold_13 )
			|| !font_ready( fonts.menu_brand_30 ) )
			return;

		overlay.set_ui_reference( reference_width, reference_height );
		overlay.set_ui_dpi_scale( dpi_scale );
		overlay.set_ui_fullscreen_canvas( true );

		if ( !atlas->Build() )
			return;
		static_cast<void>( ImGui_ImplDX11_CreateDeviceObjects() );

		if ( !chams::g_renderer.initialize( device, context ) )
			app::context().diagnostics.warning( "chams renderer initialization failed." );
		else
			g_chams_ready.store( true, std::memory_order_release );
		static_cast<void>( chams::g_preview.initialize( device, context ) );
	}

} // namespace

void initialize( ID3D11Device* device, ID3D11DeviceContext* context,
	std::uint32_t reference_width, std::uint32_t reference_height,
	float dpi_scale )
{
	load_fonts( device, context, reference_width, reference_height, dpi_scale );
}

void render_frame( ID3D11RenderTargetView* rtv, std::uint32_t width,
	std::uint32_t height, bool menu_open )
{
	g_frames_rendered.fetch_add( 1, std::memory_order_relaxed );

	const bool gpu_enabled = g_chams_ready.load( std::memory_order_acquire )
		&& config::general_settings.use_gpu;

	const auto feature_plan = render::plan_features( true,
		config::visual_settings.m_player.active(),
		config::visual_settings.m_chams.enabled, true, gpu_enabled );
	game::render_poses().set_presentation_state( feature_plan.sample_players,
		render::pose_sample_limit( config::general_settings.limit_fps,
			static_cast<std::uint32_t>( std::max( 0, config::general_settings.fps_limit ) ) ) );

	zdraw::draw_list draw_list{ ImGui::GetBackgroundDrawList() };
	chams::g_renderer.begin_2d_bloom_frame( gpu_enabled );

	const auto spectator_suppressed =
		config::visual_settings.m_player.spectator_sync
		&& game::world().local_spectated();
	if ( game::local_player().valid() && !spectator_suppressed )
	{
		const auto pose_frame = game::render_poses().acquire_for_presentation();
		const auto& presentation_pose = pose_frame;
		game::presentation_camera_sample camera{};
		if ( game::camera().sample_presentation( camera ) )
			game::camera().begin_presentation_frame( camera, width, height );
		else if ( pose_frame )
			game::camera().begin_presentation_frame( pose_frame->camera, width, height );

		// 3D chams. They keep their own depth buffer and blend their resolve back
		// over the players' screen volumes, so the game's frame underneath stays
		// intact. The caller restores the render target around them.
		if ( gpu_enabled )
		{
			chams::g_renderer.render_world_effects( rtv, width, height );
			chams::g_renderer.render_frame( rtv, width, height, presentation_pose );
			chams::g_renderer.render_weapon_chams( rtv, width, height );
		}

		features::visuals::player().render( draw_list, presentation_pose );
		features::visuals::sound().on_render( draw_list );
		features::visuals::items().on_render( draw_list );
		features::visuals::projectiles().on_render( draw_list );
		features::visuals::bomb().on_render( draw_list );
		features::visuals::radar().on_render( draw_list );
		features::visuals::crosshair().on_render( draw_list );
		features::visuals::grenade_prediction().on_render( draw_list );
		features::misc::nade_helper().on_render( draw_list );
		features::visuals::bullet_impacts().on_render( draw_list );
		features::aimbot::aim().on_render( draw_list );
		features::combat::ragebot().on_render( draw_list );
	}

	features::world::install_hooks( );
	{
		static std::atomic<std::int64_t> next_world_report;
		const auto now = static_cast<std::int64_t>( ::GetTickCount64( ) );
		if ( now >= next_world_report.load( std::memory_order_relaxed ) )
		{
			next_world_report.store( now + 2000, std::memory_order_relaxed );
			platform::windows::runtime_event( "world", features::world::report( ) );
			platform::windows::runtime_event( "rage", features::combat::ragebot( ).report( ) );
		}
	}

	scripting::runtime().render( draw_list, width, height );

	// Map atmosphere: convar writes are idempotent, so a few times a second is
	// plenty and keeps the frame free of redundant work.
	{
		static std::atomic<std::int64_t> next_apply;
		const auto now = static_cast<std::int64_t>( ::GetTickCount64( ) );
		if ( now >= next_apply.load( std::memory_order_relaxed ) )
		{
			next_apply.store( now + 250, std::memory_order_relaxed );
			features::misc::ambience( ).apply( );
		}
	}

	// One line per second while chams are configured: the renderer's counters are
	// the only way to separate "config off" from "pak not found" from "mesh not
	// extracted", and the console sink is compiled out of this build.
	{
		static std::atomic<std::int64_t> next_report;
		const auto now = static_cast<std::int64_t>( ::GetTickCount64( ) );
		if ( config::visual_settings.m_chams.enabled
			&& now >= next_report.load( std::memory_order_relaxed ) )
		{
			next_report.store( now + 1000, std::memory_order_relaxed );
			// The internal attach loop only runs once (it parks afterwards), so the
			// schema probe is driven from here: it needs presented frames before it
			// samples, and it removes its marker once it has written the dump.
			nl_cheat::run_probe_if_armed();
			const auto& counters = chams::g_renderer.diag( );
			platform::windows::runtime_event( "chams", std::format(
				"seen={} enemy={} model={} mesh={} drawn={} vpk={} err={} last={}",
				counters.players_seen, counters.players_enemy,
				counters.players_model_resolved, counters.players_mesh_valid,
				counters.players_drawn, chams::g_renderer.vpk_ready( ),
				counters.init_error.empty( ) ? "-" : counters.init_error,
				counters.last_model_path ) );
		}
	}

	// The vesta HUD panels (watermark, spectator list, event log, bind list) are
	// deliberately not drawn: the Neverlose menu renders its own, and the vesta
	// ones are branded "VESTA". Their settings still drive the feature logic.
	static_cast<void>( menu_open );
}

std::uint64_t frames_rendered( )
{
	return g_frames_rendered.load( std::memory_order_relaxed );
}

} // namespace hook_hud
