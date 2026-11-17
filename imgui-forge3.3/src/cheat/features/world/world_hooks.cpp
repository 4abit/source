#include <stdafx.hpp>

#include <features/world/world_hooks.hpp>

#include <core/hooking/trampoline.hpp>
#include <features/misc/ambience.hpp>
#include <features/misc/skybox.hpp>
#include <features/misc/thirdperson.hpp>
#include <features/misc/weather.hpp>
#include <features/skins/skins.hpp>
#include <resources/particles/effects.hpp>
#include <resources/particles/weather.hpp>

#include <app/context.hpp>
#include <core/state/services.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace features::world {
namespace {

	struct path_buffer
	{
		std::uint32_t unknown1{};
		std::uint32_t unknown2{ 0xc00000c8 };
		union
		{
			std::uintptr_t pointer;
			std::uint8_t data[ 0xc8 ];
		};
		std::uintptr_t unknown3{};
		std::uintptr_t unknown4{};
	};

	using override_view_fn = void( __fastcall* )( void*, void* );
	using frame_stage_fn = void( __fastcall* )( void*, int );
	using skybox_fn = void( __fastcall* )( std::uintptr_t, std::uintptr_t, std::uintptr_t, int, int, std::uintptr_t, std::uintptr_t, std::uintptr_t );
	using light_fn = std::uintptr_t( __fastcall* )( std::uintptr_t, std::uintptr_t, std::uintptr_t );
	using scene_object_fn = std::uintptr_t( __fastcall* )( std::uintptr_t, std::uintptr_t, std::uintptr_t, int, int, std::uintptr_t, std::uintptr_t, std::uintptr_t );
	using scene_array_fn = void( __fastcall* )( std::uintptr_t, std::uintptr_t, std::uintptr_t );
	using fog_fn = std::uintptr_t( __fastcall* )( void*, int* );
	using shader_fn = std::uintptr_t( __fastcall* )( void*, std::uint32_t, void* );
	using shader_int_fn = std::uintptr_t( __fastcall* )( void*, std::uint32_t, int );
	using service_read_fn = std::uintptr_t( __fastcall* )( std::uintptr_t );
	using fs_close_fn = void( __fastcall* )( std::uintptr_t, int );
	using fs_alloc_fn = std::uintptr_t( __fastcall* )( std::uintptr_t, std::size_t, const char* );

	using init_path_fn = void( __fastcall* )( path_buffer*, const char* );
	using precache_fn = void( __fastcall* )( void*, path_buffer*, const char* );
	using create_effect_fn = int*( __fastcall* )( std::uintptr_t, std::uint32_t*, const char*, int, std::int64_t, std::int64_t, std::int64_t, int );
	using control_point_fn = bool( __fastcall* )( std::uintptr_t, std::uint32_t, int, const foundation::vec3*, int );
	using transform_fn = bool( __fastcall* )( std::uintptr_t, std::uint32_t, int, const void*, int );
	using destroy_fn = void( __fastcall* )( std::uintptr_t, std::uint32_t, bool, bool );

	struct state
	{
		hooking::trampoline override_view{};
		hooking::trampoline frame_stage{};
		hooking::trampoline skybox{};
		hooking::trampoline light{};
		hooking::trampoline scene_object{};
		hooking::trampoline scene_array{};
		hooking::trampoline fog{};
		hooking::trampoline shader{};
		hooking::trampoline service_read{};

		fs_close_fn fs_close{};
		shader_int_fn shader_int{};

		init_path_fn init_path{};
		precache_fn precache{};
		create_effect_fn create_effect{};
		control_point_fn control_point{};
		transform_fn transform{};
		destroy_fn destroy{};
		std::uintptr_t manager_global{};
		std::uintptr_t rules_global{};
		void* resource_system{};

		std::uintptr_t last_manager{};
		std::uintptr_t last_rules{};
		float last_round_start{ -1.0f };
		std::atomic<std::uint64_t> generation{ 1 };
		std::unordered_set<std::string> precached{};
	};

	state& self()
	{
		static state value{};
		return value;
	}

	[[nodiscard]] std::uintptr_t module( const char* name )
	{
		return app::context().process.module_base( name );
	}

	[[nodiscard]] std::uintptr_t scan( const char* name, std::string_view signature )
	{
		const auto base = module( name );
		return base ? app::context().process.scan_signature( base, signature ) : 0;
	}

	[[nodiscard]] std::uintptr_t call_target( std::uintptr_t site )
	{
		return site ? app::context().process.decode_call( site ) : 0;
	}

	[[nodiscard]] std::uintptr_t rip( std::uintptr_t site, int offset = 3, int size = 7 )
	{
		return site ? app::context().process.decode_rip( site, offset, size ) : 0;
	}

	[[nodiscard]] std::uintptr_t load( std::uintptr_t address )
	{
		return address ? app::context().process.load<std::uintptr_t>( address ) : 0;
	}

	void* interface_of( const char* module_name, const char* name )
	{
		const auto handle = ::GetModuleHandleA( module_name );
		if ( !handle )
			return nullptr;
		using create_fn = void*( * )( const char*, int* );
		const auto create = reinterpret_cast<create_fn>( ::GetProcAddress( handle, "CreateInterface" ) );
		return create ? create( name, nullptr ) : nullptr;
	}

	// ---- detours ---------------------------------------------------------------

	std::atomic<std::uint64_t> g_view_calls{};
	std::atomic<std::uint64_t> g_stage_calls{};
	std::atomic<std::uint64_t> g_skybox_calls{};
	std::atomic<std::uint64_t> g_shader_calls{};
	std::string g_installed{};

	void __fastcall override_view_hook( void* self_ptr, void* view_setup )
	{
		g_view_calls.fetch_add( 1, std::memory_order_relaxed );
		self().override_view.original<override_view_fn>()( self_ptr, view_setup );
		misc::weather().on_frame();
		if ( view_setup )
			misc::thirdperson().on_view( reinterpret_cast<std::uintptr_t>( view_setup ) );
	}

	void __fastcall frame_stage_hook( void* self_ptr, int stage )
	{
		g_stage_calls.fetch_add( 1, std::memory_order_relaxed );
		skins::changer().on_frame_stage( stage );
		self().frame_stage.original<frame_stage_fn>()( self_ptr, stage );
		misc::skybox().on_frame_stage( stage );
	}

	void __fastcall skybox_hook( std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t meshes, int count,
		int a5, std::uintptr_t a6, std::uintptr_t a7, std::uintptr_t a8 )
	{
		g_skybox_calls.fetch_add( 1, std::memory_order_relaxed );
		const auto token = misc::ambience().on_skybox_pre( meshes, count );
		const auto sky = misc::skybox().on_draw_pre( meshes, count );
		self().skybox.original<skybox_fn>()( a1, a2, meshes, count, a5, a6, a7, a8 );
		misc::skybox().on_draw_post( sky );
		misc::ambience().on_skybox_post( token );
	}

	std::uintptr_t __fastcall light_hook( std::uintptr_t a1, std::uintptr_t object, std::uintptr_t a3 )
	{
		float saved[ 3 ]{};
		const auto changed = misc::ambience().on_light_pre( object, saved );
		const auto result = self().light.original<light_fn>()( a1, object, a3 );
		if ( changed )
			misc::ambience().on_light_post( object, saved );
		return result;
	}

	std::uintptr_t __fastcall scene_object_hook( std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t batch, int count,
		int a5, std::uintptr_t a6, std::uintptr_t a7, std::uintptr_t a8 )
	{
		misc::ambience().on_scene_objects( batch, count );
		return self().scene_object.original<scene_object_fn>()( a1, a2, batch, count, a5, a6, a7, a8 );
	}

	void __fastcall scene_array_hook( std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t object_array )
	{
		self().scene_array.original<scene_array_fn>()( a1, a2, object_array );
		// The draw fills the light records; the wall tint has to land after that.
		misc::ambience().on_aggregate( object_array );
	}

	std::uintptr_t __fastcall fog_hook( void* output, int* mode )
	{
		// The override replaces the engine's fog pass outright, so it may only run
		// when both setters exist; otherwise the fog constants stay uninitialised.
		if ( self().shader.active() && self().shader_int && misc::ambience().on_fog( output, mode ) )
			return 0;
		return self().fog.original<fog_fn>()( output, mode );
	}

	std::uintptr_t __fastcall shader_hook( void* map, std::uint32_t hash, void* value )
	{
		g_shader_calls.fetch_add( 1, std::memory_order_relaxed );
		misc::ambience().on_shader_param( value, hash );
		misc::weather().on_shader_param( value, hash );
		return self().shader.original<shader_fn>()( map, hash, value );
	}

	[[nodiscard]] std::span<const unsigned char> embedded( std::string_view name )
	{
		namespace w = resources::particles::weather;
		namespace e = resources::particles::effects;
		if ( name.find( "snow" ) != std::string_view::npos ) return w::snow;
		if ( name.find( "rain" ) != std::string_view::npos ) return w::rain;
		if ( name.find( "stars" ) != std::string_view::npos ) return w::stars;
		if ( name.find( "tracer" ) != std::string_view::npos ) return e::tracer;
		if ( name.find( "sparks" ) != std::string_view::npos ) return e::sparks;
		return {};
	}

	// The game asks the file system for particles/embedded/*.vpcf_c; the
	// compiled resources shipped with the cheat are handed over in its buffer.
	std::uintptr_t __fastcall service_read_hook( std::uintptr_t request )
	{
		auto& s = self();
		const auto original = s.service_read.original<service_read_fn>();
		const auto flags = *reinterpret_cast<const std::uint32_t*>( request - 212 );
		const auto length = flags & 0x3fffffffu;
		if ( length == 0 || length >= 512 || !s.fs_close )
			return original( request );

		char name[ 512 ]{};
		if ( flags & 0x40000000u )
			std::memcpy( name, reinterpret_cast<const void*>( request - 208 ), length );
		else if ( const auto text = *reinterpret_cast<const char* const*>( request - 208 ) )
			std::memcpy( name, text, length );
		const std::string_view view{ name };
		if ( view.find( "particles/embedded/" ) == std::string_view::npos )
			return original( request );

		const auto blob = embedded( view );
		const auto filesystem = *reinterpret_cast<const std::uintptr_t*>( request + 24 );
		if ( blob.empty() || !filesystem )
			return original( request );
		const auto vtable = *reinterpret_cast<const std::uintptr_t*>( filesystem );
		const auto allocate = *reinterpret_cast<const fs_alloc_fn*>( vtable + 22 * sizeof( void* ) );
		const auto buffer = allocate( filesystem, blob.size(), name );
		if ( !buffer )
			return original( request );
		std::memcpy( reinterpret_cast<void*>( buffer ), blob.data(), blob.size() );
		*reinterpret_cast<std::uintptr_t*>( request + 56 ) = buffer;
		*reinterpret_cast<std::uintptr_t*>( request + 64 ) = blob.size();
		*reinterpret_cast<std::uintptr_t*>( request + 72 ) = blob.size();
		s.fs_close( request - 224, 0 );
		return 0;
	}

	void hook( hooking::trampoline& slot, std::uintptr_t target, void* detour, const char* name )
	{
		auto& log = app::context().diagnostics;
		if ( slot.active() )
		{
			g_installed += std::format( "{}=ok ", name );
			return;
		}
		if ( !target )
		{
			log.warning( "[world] {} was not found.", name );
			g_installed += std::format( "{}=missing ", name );
			return;
		}
		if ( !slot.install( reinterpret_cast<void*>( target ), detour ) )
		{
			log.warning( "[world] {} could not be hooked.", name );
			g_installed += std::format( "{}=failed ", name );
		}
		else
		{
			log.info( "[world] {} hooked.", name );
			g_installed += std::format( "{}=ok ", name );
		}
	}

	[[nodiscard]] bool all_installed()
	{
		const auto& s = self();
		return s.override_view.active() && s.frame_stage.active() && s.skybox.active() && s.light.active()
			&& s.scene_object.active() && s.scene_array.active() && s.fog.active() && s.shader.active()
			&& s.service_read.active();
	}

	void install_once()
	{
		auto& s = self();
		g_installed.clear();

		s.init_path = reinterpret_cast<init_path_fn>( scan( "client.dll", "48 89 5C 24 ? 57 48 83 EC ? 8B 41 ? 48 8D 79" ) );
		s.precache = reinterpret_cast<precache_fn>( scan( "resourcesystem.dll", "40 53 55 57 48 81 EC 80 00 00 00" ) );
		s.create_effect = reinterpret_cast<create_effect_fn>( scan( "client.dll", "4C 8B DC 53 48 81 EC 90 00 00 00 F2 0F 10 05" ) );
		s.control_point = reinterpret_cast<control_point_fn>( scan( "client.dll", "48 83 EC 58 F3 41 0F 10 51 04 F3 41 0F 10 09 F3 41 0F 10 59 08" ) );
		s.transform = reinterpret_cast<transform_fn>( scan( "client.dll", "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 48 83 EC 40 48 8B F9 49 8B E9" ) );
		s.destroy = reinterpret_cast<destroy_fn>( scan( "client.dll", "83 FA FF 0F 84 ? ? ? ? 41 54" ) );
		s.manager_global = rip( scan( "client.dll", "48 8B 35 ? ? ? ? 44 89 6C 24 ?" ) );
		s.rules_global = rip( scan( "client.dll", "48 8B 0D ? ? ? ? 4C 89 70 10" ) );
		for ( int version = 13; version <= 25 && !s.resource_system; ++version )
		{
			char name[ 32 ]{};
			std::snprintf( name, sizeof( name ), "ResourceSystem%03d", version );
			s.resource_system = interface_of( "resourcesystem.dll", name );
		}
		s.shader_int = reinterpret_cast<shader_int_fn>( scan( "client.dll", "48 89 6C 24 ? 48 89 74 24 ? 57 48 83 EC 20 66 0F 6E CA 41 8B F0" ) );
		s.fs_close = reinterpret_cast<fs_close_fn>( call_target(
			scan( "filesystem_stdio.dll", "E8 ? ? ? ? FF D3 4C 8B A4 24 98 00 00 00" ) ) );
		if ( !s.fs_close )
		{
			const auto site = scan( "filesystem_stdio.dll", "48 8D 8F 20 FF FF FF 8B D5 E8 ? ? ? ? FF D3" );
			s.fs_close = reinterpret_cast<fs_close_fn>( call_target( site ? site + 9 : 0 ) );
		}

		app::context().diagnostics.info( "[world] particles: path={} precache={} create={} cp={} tf={} destroy={} manager={:#x} rs={} fs_close={} shader_int={}",
			s.init_path != nullptr, s.precache != nullptr, s.create_effect != nullptr, s.control_point != nullptr,
			s.transform != nullptr, s.destroy != nullptr, s.manager_global, s.resource_system != nullptr,
			s.fs_close != nullptr, s.shader_int != nullptr );

		const auto view_site = scan( "client.dll", "A8 00 00 00 48 8D 05 ? ? ? ? 4C 89 74 24 20" );
		hook( s.override_view, load( rip( view_site ? view_site + 4 : 0 ) + 0x78 ),
			reinterpret_cast<void*>( &override_view_hook ), "override view" );
		hook( s.frame_stage, scan( "client.dll", "48 89 5C 24 18 48 89 6C 24 20 57 48 83 EC 40 48 8B F9" ),
			reinterpret_cast<void*>( &frame_stage_hook ), "frame stage notify" );
		hook( s.skybox, scan( "scenesystem.dll", "45 85 C9 0F 8E ? ? ? ? 4C 8B DC" ),
			reinterpret_cast<void*>( &skybox_hook ), "skybox" );
		hook( s.light, call_target( scan( "scenesystem.dll", "E8 ? ? ? ? 44 0F 28 5C 24 60" ) ),
			reinterpret_cast<void*>( &light_hook ), "scene lighting" );
		hook( s.scene_object, load( rip( scan( "scenesystem.dll", "48 8D 05 ? ? ? ? 48 89 07 48 8B 7C 24 48" ) ) + 8 ),
			reinterpret_cast<void*>( &scene_object_hook ), "scene objects" );
		hook( s.scene_array, scan( "scenesystem.dll",
			"48 8B C4 48 89 50 ? 48 89 48 ? 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 ? ? ? ? 48 81 EC ? ? ? ? 0F 29 70 ?" ),
			reinterpret_cast<void*>( &scene_array_hook ), "scene object array" );
		hook( s.fog, scan( "client.dll", "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 48 89 4C 24 ? 57 41 54 41 55 41 56 41 57 48 83 EC 20 48 63 02" ),
			reinterpret_cast<void*>( &fog_hook ), "fog" );
		auto shader_target = call_target( scan( "client.dll", "E8 ? ? ? ? 0F B6 43 25" ) );
		if ( !shader_target )
		{
			// The float setter is instantiated several times with identical prologues;
			// the gradient-fog call site pins the one the fog and post-process use.
			const auto site = scan( "client.dll", "BA 63 FF 01 4B 49 8B CF 0F 10 00 0F 29 44 24 30 E8" );
			shader_target = call_target( site ? site + 16 : 0 );
		}
		hook( s.shader, shader_target, reinterpret_cast<void*>( &shader_hook ), "shader params" );
		const auto read_site = scan( "filesystem_stdio.dll", "00 48 89 07 48 8D 05 ? ? ? ? 48 89 87 E0 00 00 00" );
		hook( s.service_read, load( rip( read_site ? read_site + 4 : 0 ) ),
			reinterpret_cast<void*>( &service_read_hook ), "file service" );
	}

	[[nodiscard]] std::uintptr_t manager()
	{
		return load( self().manager_global );
	}

} // namespace

void install_hooks()
{
	// The present hook runs while the attach loop is still (re)opening the process
	// handle; module lookups already succeed then but every read fails, so each
	// signature comes back empty. Wait until the image is actually readable and
	// retry whatever stayed missing.
	constexpr int k_max_attempts = 5;
	constexpr std::int64_t k_retry_ms = 3000;
	static std::atomic<int> attempts{ 0 };
	static std::atomic<std::int64_t> next_attempt{ 0 };
	if ( attempts.load( std::memory_order_acquire ) >= k_max_attempts )
		return;
	const auto now = static_cast<std::int64_t>( ::GetTickCount64() );
	if ( now < next_attempt.load( std::memory_order_relaxed ) )
		return;

	auto& process = app::context().process;
	if ( !process.native_handle() || !app::context().modules.client )
		return;
	const auto client = process.module_base( "client.dll" );
	if ( !client || !process.module_base( "scenesystem.dll" ) || !process.module_base( "filesystem_stdio.dll" ) )
		return;
	if ( process.load<std::uint16_t>( client ) != IMAGE_DOS_SIGNATURE )
		return;

	next_attempt.store( now + k_retry_ms, std::memory_order_relaxed );
	install_once();
	attempts.store( all_installed() ? k_max_attempts : attempts.load( std::memory_order_relaxed ) + 1,
		std::memory_order_release );
}

std::string report()
{
	return std::format( "view={} stage={} skybox={} shader={} | {}",
		g_view_calls.load( std::memory_order_relaxed ), g_stage_calls.load( std::memory_order_relaxed ),
		g_skybox_calls.load( std::memory_order_relaxed ), g_shader_calls.load( std::memory_order_relaxed ),
		g_installed );
}

void set_shader_float( void* map, std::uint32_t hash, const void* value )
{
	if ( self().shader.active() )
		self().shader.original<shader_fn>()( map, hash, const_cast<void*>( value ) );
}

void set_shader_int( void* map, std::uint32_t hash, int value )
{
	if ( self().shader_int )
		self().shader_int( map, hash, value );
}

std::uintptr_t precache_texture( const char* path )
{
	auto& s = self();
	if ( !path || !*path || !s.init_path || !s.precache || !s.resource_system )
		return 0;

	path_buffer buffer{};
	s.init_path( &buffer, path );
	buffer.unknown4 = 'xetv';
	s.precache( s.resource_system, &buffer, "" );

	s.init_path( &buffer, path );
	buffer.unknown4 = 'xetv';
	const auto table = *reinterpret_cast<const std::uintptr_t*>( s.resource_system );
	const auto find = table ? *reinterpret_cast<const std::uintptr_t*>( table + 79 * sizeof( void* ) ) : 0;
	if ( !find )
		return 0;
	using find_fn = std::uintptr_t( __fastcall* )( void*, path_buffer*, std::int64_t );
	const auto binding = reinterpret_cast<find_fn>( find )( s.resource_system, &buffer, 0 );
	if ( binding < 0x10000 )
		return 0;
	const auto texture = *reinterpret_cast<const std::uintptr_t*>( binding );
	return texture >= 0x10000 ? texture : 0;
}

std::uintptr_t create_material( const char* vmat, const char* name )
{
	if ( !vmat || !name )
		return 0;

	struct key_values3
	{
		std::uint64_t metadata{};
		std::uint64_t payload{};
	};
	struct kv3_id
	{
		const char* format{};
		std::uint64_t low{};
		std::uint64_t high{};
	};
	struct strong_handle
	{
		void* binding{};
	};

	using set_type_fn = key_values3*( __fastcall* )( key_values3*, std::uint32_t, std::uint32_t );
	using destroy_fn = void( __fastcall* )( key_values3*, std::uint32_t );
	using load_fn = bool( __fastcall* )( key_values3*, void*, const char*, const kv3_id*, void*, std::uint32_t );
	using create_fn = void*( __fastcall* )( void*, strong_handle*, const char*, key_values3*, int, bool );

	static set_type_fn set_type{};
	static destroy_fn destroy{};
	static load_fn load{};
	static create_fn create{};
	static bool resolved{};
	if ( !resolved )
	{
		resolved = true;
		set_type = reinterpret_cast<set_type_fn>( scan( "tier0.dll", "40 53 48 83 EC 30 80 FA 06 0F B6 C2 41 B9 16" ) );
		destroy = reinterpret_cast<destroy_fn>( scan( "tier0.dll", "40 57 41 57 48 83 EC 38 4C 8B 01 44 8B FA 49 8B C0 48 8B F9 48 C1 E8 02" ) );
		create = reinterpret_cast<create_fn>( scan( "materialsystem2.dll",
			"48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 41 56 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 8B F2" ) );
		if ( const auto tier0 = ::GetModuleHandleA( "tier0.dll" ) )
			load = reinterpret_cast<load_fn>( ::GetProcAddress( tier0,
				"?LoadKV3@@YA_NPEAVKeyValues3@@PEAVCUtlString@@PEBDAEBUKV3ID_t@@2I@Z" ) );
	}
	if ( !set_type || !destroy || !load || !create )
		return 0;

	key_values3 kv{};
	if ( set_type( &kv, 1u, 6u ) != &kv )
		return 0;

	const kv3_id id{ "generic", 0x41B818518343427Eull, 0xB5F447C23C0CDF8Cull };
	strong_handle handle{};
	const auto loaded = load( &kv, nullptr, vmat, &id, nullptr, 0u );
	if ( loaded )
		create( nullptr, &handle, name, &kv, 0, true );
	destroy( &kv, 0u );
	if ( !loaded || !handle.binding )
		return 0;

	const auto material = *reinterpret_cast<const std::uintptr_t*>( handle.binding );
	if ( material < 0x10000 )
		return 0;
	static std::vector<void*> kept{};
	kept.push_back( handle.binding );
	return material;
}

namespace particles {

bool ready()
{
	auto& s = self();
	return s.init_path && s.precache && s.create_effect && s.control_point && s.destroy
		&& s.resource_system && s.service_read.active() && manager();
}

std::uint64_t generation()
{
	auto& s = self();
	const auto current = manager();
	const auto rules = load( s.rules_global );
	float round_start = -1.0f;
	if ( rules )
	{
		const auto offset = SCHEMA( "C_CSGameRules", "m_fRoundStartTime"_id );
		if ( offset > 0 )
			round_start = app::context().process.load<float>( rules + offset );
	}
	if ( current != s.last_manager || rules != s.last_rules || round_start != s.last_round_start )
	{
		s.last_manager = current;
		s.last_rules = rules;
		s.last_round_start = round_start;
		s.generation.fetch_add( 1, std::memory_order_relaxed );
	}
	return s.generation.load( std::memory_order_relaxed );
}

std::uint32_t create( const char* path )
{
	auto& s = self();
	if ( !ready() || !path )
		return invalid;
	if ( !s.precached.contains( path ) )
	{
		path_buffer buffer{};
		s.init_path( &buffer, path );
		buffer.unknown4 = 'fcpv';
		s.precache( s.resource_system, &buffer, "" );
		s.precached.emplace( path );
	}
	std::uint32_t effect = invalid;
	s.create_effect( manager(), &effect, path, 8, 0, 0, 0, 0 );
	return effect;
}

void control_point( std::uint32_t effect, int point, const foundation::vec3& value )
{
	auto& s = self();
	if ( effect != invalid && s.control_point && manager() )
		s.control_point( manager(), effect, point, &value, 0 );
}

void transform( std::uint32_t effect, int point, const foundation::vec3& origin, const float quaternion[ 4 ] )
{
	auto& s = self();
	if ( effect == invalid || !manager() )
		return;
	if ( !s.transform )
	{
		control_point( effect, point, origin );
		return;
	}
	struct alignas( 16 ) packed
	{
		float position[ 4 ];
		float rotation[ 4 ];
	} value{ { origin.x, origin.y, origin.z, 0.0f },
		{ quaternion[ 0 ], quaternion[ 1 ], quaternion[ 2 ], quaternion[ 3 ] } };
	s.transform( manager(), effect, point, &value, 0 );
}

void destroy( std::uint32_t effect )
{
	auto& s = self();
	if ( effect != invalid && s.destroy && manager() )
		s.destroy( manager(), effect, true, true );
}

} // namespace particles

} // namespace features::world
