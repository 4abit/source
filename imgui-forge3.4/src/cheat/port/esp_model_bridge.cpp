// Menu-side enemy model preview.
//
// The Visuals -> Players tab used to blit a grey PNG of an agent. The real model
// renderer (chams::preview) already exists in this project but had no caller left
// after the old editor was removed, so this bridge re-attaches it: the platform
// layer lends its D3D11 device, the chams renderer lends its already-open handle to
// pak01_dir.vpk, and the menu receives a texture plus the projections it needs to
// draw the skeleton and hitboxes on top of it.
//
// Ownership: the device/context stay with the platform layer (never AddRef'd), the
// vpk_archive stays with chams::g_renderer, and the returned SRV stays with
// chams::preview. The menu only reads the SRV during the frame it was returned.

#include <algorithm>
#include <cstdint>

#include <d3d11.h>

#include "menu/esp_model.h"

#include <config/settings.hpp>
#include <app/workers.hpp>
#include <features/visuals/visuals.hpp>
#include <render/chams/preview.hpp>
#include <render/chams/renderer.hpp>

namespace nl::esp_model {
namespace {

    // The picture the preview replaces rendered the CT SAS agent, so the model
    // matches what the tab has always shown.
    constexpr const char* k_model_path = "agents/models/ctm_sas/ctm_sas.vmdl_c";

    // The inventory can ask for any agent model; the ESP preview keeps the default.
    std::string g_model_path{ k_model_path };

    ID3D11Device* g_device{};
    ID3D11DeviceContext* g_context{};
    bool g_preview_owned{};
    bool g_frame_ready{};
    std::uint32_t g_target_width{};
    std::uint32_t g_target_height{};

    [[nodiscard]] bool ensure_preview()
    {
        if ( !g_device || !g_context )
            return false;
        if ( g_preview_owned )
            return true;
        // The injected build already initialized the preview while loading the HUD
        // fonts; preview::initialize() is idempotent for the same device, so this
        // either adopts the existing resources or creates them once.
        if ( !chams::g_preview.initialize( g_device, g_context ) )
            return false;
        g_preview_owned = true;
        return true;
    }

    // The chams renderer owns the single handle to the (tens of GB) pak; a second
    // archive is never opened. Loading it is asynchronous, so this returns null
    // until the directory index is in memory.
    [[nodiscard]] chams::vpk_archive* archive()
    {
        return chams::g_renderer.ensure_vpk() ? &chams::g_renderer.vpk() : nullptr;
    }

    [[nodiscard]] bool bone_screen( const std::uint32_t bone, float& nx, float& ny )
    {
        foundation::vec3 position{};
        if ( !chams::g_preview.bone_position( bone, position ) )
            return false;
        float x{};
        float y{};
        if ( !chams::g_preview.project( position, x, y ) )
            return false;
        // project() reports pixels inside the offscreen target; the caller maps the
        // normalized result onto whatever rectangle it drew the texture into.
        nx = x / static_cast<float>( std::max( 1u, g_target_width ) );
        ny = y / static_cast<float>( std::max( 1u, g_target_height ) );
        return true;
    }

    struct bone_span
    {
        float min_x{};
        float min_y{};
        float max_x{};
        float max_y{};
        bool valid{};
    };

    void include( bone_span& span, const float x, const float y )
    {
        if ( !span.valid )
        {
            span = { x, y, x, y, true };
            return;
        }
        span.min_x = std::min( span.min_x, x );
        span.min_y = std::min( span.min_y, y );
        span.max_x = std::max( span.max_x, x );
        span.max_y = std::max( span.max_y, y );
    }

    template <std::size_t count>
    [[nodiscard]] bone_span span_of( const std::array<int, count>& bones )
    {
        bone_span span{};
        for ( const auto bone : bones )
        {
            float x{};
            float y{};
            if ( bone >= 0 && bone_screen( static_cast<std::uint32_t>( bone ), x, y ) )
                include( span, x, y );
        }
        return span;
    }

    [[nodiscard]] bool write_span( const bone_span& span, float* x0, float* y0,
        float* x1, float* y1 )
    {
        if ( !span.valid )
            return false;
        if ( x0 ) *x0 = span.min_x;
        if ( y0 ) *y0 = span.min_y;
        if ( x1 ) *x1 = span.max_x;
        if ( y1 ) *y1 = span.max_y;
        return true;
    }

} // namespace

void set_device( void* device, void* context )
{
    g_device = static_cast<ID3D11Device*>( device );
    g_context = static_cast<ID3D11DeviceContext*>( context );
}

void release()
{
    if ( g_preview_owned )
    {
        chams::g_preview.shutdown();
        g_preview_owned = false;
    }
    g_device = nullptr;
    g_context = nullptr;
    g_frame_ready = false;
    g_target_width = 0;
    g_target_height = 0;
}

bool available()
{
    return g_device != nullptr && g_context != nullptr;
}

bool frame_ready()
{
    return g_frame_ready;
}

void set_model( const char* model_path )
{
    // Switching models invalidates the cached mesh inside the preview renderer, which
    // reloads whatever path it is handed on the next frame. An empty path goes back to
    // the default agent, so the inventory cannot leave its pick in the ESP preview.
    g_model_path = model_path && *model_path ? model_path : k_model_path;
}

const char* current_map_name()
{
    // The world sampler publishes an immutable string; its storage is copied into a
    // static buffer so the menu can hold the pointer while it draws.
    static std::string cached{};
    const auto map = app::workers::current_map( );
    if ( map && !map->empty( ) )
    {
        // The game reports "maps/de_mirage.vpk"; the menu needs the bare id so it can be
        // matched against the saved entries and used as a pak path.
        std::string name = *map;
        const auto slash = name.find_last_of( "/\\" );
        if ( slash != std::string::npos )
            name.erase( 0, slash + 1 );
        if ( name.size( ) > 4 && name.compare( name.size( ) - 4, 4, ".vpk" ) == 0 )
            name.resize( name.size( ) - 4 );
        if ( !name.empty( ) )
            cached = std::move( name );
    }
    return cached.c_str( );
}

frame render( float width, float height, float orbit, bool use_chams )
{
    frame result{};
    g_frame_ready = false;
    if ( !ensure_preview() )
        return result;
    auto* vpk = archive();
    if ( !vpk )
        return result;

    const auto target_width = static_cast<std::uint32_t>(
        std::clamp( width, 16.0f, 2048.0f ) );
    const auto target_height = static_cast<std::uint32_t>(
        std::clamp( height, 16.0f, 2048.0f ) );

    if ( orbit != 0.0f )
        chams::g_preview.orbit( orbit );

    g_target_width = target_width;
    g_target_height = target_height;

    const auto& material = config::visual_settings.m_chams.visible;
    // The chams material is drawn through the world renderer, which only exists in
    // the injected build. Asking for it without that renderer would leave the
    // offscreen target empty, so the plain textured draw is used instead.
    const bool use_material = use_chams && material.enabled
        && chams::g_renderer.ready();
    auto* srv = chams::g_preview.render( *vpk, g_model_path, target_width,
        target_height, use_material ? &material : nullptr );
    if ( !srv )
        return result;

    result.texture = reinterpret_cast<ImTextureID>( srv );
    result.ready = true;
    result.width = static_cast<float>( target_width );
    result.height = static_cast<float>( target_height );
    g_frame_ready = true;
    return result;
}

int skeleton_segments()
{
    return static_cast<int>(
        features::visuals::player_t::skeleton_connections.size() );
}

bool skeleton_segment( const int index, float* x0, float* y0, float* x1, float* y1 )
{
    const auto& connections = features::visuals::player_t::skeleton_connections;
    if ( index < 0 || index >= static_cast<int>( connections.size() ) )
        return false;
    float ax{};
    float ay{};
    float bx{};
    float by{};
    if ( !bone_screen( connections[ static_cast<std::size_t>( index ) ].first, ax, ay )
        || !bone_screen( connections[ static_cast<std::size_t>( index ) ].second, bx, by ) )
        return false;
    if ( x0 ) *x0 = ax;
    if ( y0 ) *y0 = ay;
    if ( x1 ) *x1 = bx;
    if ( y1 ) *y1 = by;
    return true;
}

bool part_bounds( const part which, float* x0, float* y0, float* x1, float* y1 )
{
    using player = features::visuals::player_t;

    switch ( which )
    {
    case part::head:
    {
        // A single head bone is a point, so the circle comes from its distance to
        // the neck: that is the scale the in-game head marker uses as well.
        float head_x{};
        float head_y{};
        float neck_x{};
        float neck_y{};
        if ( !bone_screen( 7, head_x, head_y ) || !bone_screen( 6, neck_x, neck_y ) )
            return false;
        const auto radius = std::max( 0.02f,
            std::sqrt( ( head_x - neck_x ) * ( head_x - neck_x )
                + ( head_y - neck_y ) * ( head_y - neck_y ) ) * 0.62f );
        if ( x0 ) *x0 = head_x - radius;
        if ( y0 ) *y0 = head_y - radius;
        if ( x1 ) *x1 = head_x + radius;
        if ( y1 ) *y1 = head_y + radius;
        return true;
    }
    case part::torso:
        return write_span( span_of( player::threat_body_bones ), x0, y0, x1, y1 );
    case part::left_leg:
    {
        const std::array<int, 3> bones{ 17, 18, 19 };
        return write_span( span_of( bones ), x0, y0, x1, y1 );
    }
    case part::right_leg:
    {
        const std::array<int, 3> bones{ 20, 21, 22 };
        return write_span( span_of( bones ), x0, y0, x1, y1 );
    }
    default:
        return false;
    }
}

} // namespace nl::esp_model
