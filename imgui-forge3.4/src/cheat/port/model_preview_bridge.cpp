// Menu-side model inspector for the Inventory tab.
//
// The Visuals tab has its own preview (esp_model_bridge.cpp) with a camera built for an
// upright character and an overlay of bones and hitboxes on top. This one is the other
// half of the same idea: a turntable for a weapon, framed on the object's bounding box,
// with the finish the inventory picked painted over the model's own materials.
//
// It drives a second chams::preview instance rather than sharing the ESP one, because the
// two want different cameras and would otherwise overwrite each other's view every frame.
//
// Ownership: the device/context stay with the platform layer (never AddRef'd), the
// vpk_archive stays with chams::g_renderer, and the returned SRV stays with the preview.
// The menu only reads the SRV during the frame it was returned.

#include <algorithm>
#include <cstdint>
#include <string>

#include <d3d11.h>

#include "menu/model_preview.h"

#include <app/workers.hpp>
#include <render/chams/preview.hpp>
#include <render/chams/renderer.hpp>

namespace nl::model_preview {
namespace {

    // Three-quarter view down at the model, kept close to side-on: the inspector's panel is
    // much wider than it is tall, and a weapon only fills it when its length runs across
    // the screen rather than away from the camera.
    constexpr float k_default_yaw = 105.0f;
    constexpr float k_default_pitch = 13.0f;

    ID3D11Device* g_device{};
    ID3D11DeviceContext* g_context{};
    bool g_owned{};
    std::string g_model_path{};

    std::uint32_t g_target_width{};
    std::uint32_t g_target_height{};

    // The finish currently bound, so an unchanged request does not throw away the
    // preview's cached textures every frame.
    std::string g_finish_key{};

    config::visual_profile::chams::material g_chams{};
    bool g_chams_on{};

    [[nodiscard]] std::uint8_t channel( const float v )
    {
        return static_cast< std::uint8_t >( std::clamp( v, 0.0f, 1.0f ) * 255.0f + 0.5f );
    }

    [[nodiscard]] bool ensure_preview()
    {
        if ( !g_device || !g_context )
            return false;
        if ( g_owned )
            return true;
        if ( !chams::g_inspect.initialize( g_device, g_context ) )
            return false;
        g_owned = true;
        // Everything the inspector shows -- a weapon, a knife, the glove arms, an agent --
        // is framed on its own bounding box, so the turntable behaves the same for all of
        // them. The ESP preview keeps the upright framing, which is tuned for a figure
        // standing on the ground.
        chams::g_inspect.set_framing( chams::preview::framing::object );
        return true;
    }

    // The chams renderer owns the single handle to the (tens of GB) pak; a second archive
    // is never opened. Loading it is asynchronous, so this returns null until the
    // directory index is in memory.
    [[nodiscard]] chams::vpk_archive* archive()
    {
        return chams::g_renderer.ensure_vpk() ? &chams::g_renderer.vpk() : nullptr;
    }

    [[nodiscard]] std::string key_of( const char* material, const char* color, const char* normal,
        const char* roughness, const char* ao, const char* mask, const bool palette )
    {
        std::string key{};
        for ( const char* part : { material, color, normal, roughness, ao, mask } )
        {
            key += part ? part : "";
            key.push_back( '\n' );
        }
        key += palette ? "1" : "0";
        return key;
    }

} // namespace

void set_device( void* device, void* context )
{
    g_device = static_cast< ID3D11Device* >( device );
    g_context = static_cast< ID3D11DeviceContext* >( context );
}

void release()
{
    if ( g_owned )
    {
        chams::g_inspect.shutdown();
        g_owned = false;
    }
    g_device = nullptr;
    g_context = nullptr;
    g_model_path.clear();
    g_finish_key.clear();
    g_target_width = 0;
    g_target_height = 0;
}

bool available()
{
    return g_device != nullptr && g_context != nullptr;
}

void set_model( const char* model_path )
{
    const std::string next = model_path ? model_path : "";
    if ( next == g_model_path )
        return;
    g_model_path = next;
    // Gloves open on the inspect angle: looking into the hands, slightly above.
    // Everything else keeps the three-quarter weapon view.
    const bool glove = next.find( "glove_" ) != std::string::npos;
    // Close on the hands so the elbows fall outside the frame, the way the
    // inspect camera crops the arms.
    // The pedestal clip turns the palms out toward a studio camera. The player
    // sees the backs of the gloves, from above the wrists, the way the viewmodel does.
    chams::g_inspect.set_view( glove ? 188.0f : k_default_yaw, glove ? 28.0f : k_default_pitch, glove ? 1.15f : 1.0f );
}

void set_finish( const char* material, const char* color, const char* normal,
    const char* roughness, const char* ao, const char* mask, const bool palette )
{
    const std::string key = key_of( material, color, normal, roughness, ao, mask, palette );
    if ( key == g_finish_key )
        return;

    // The pak opens on a background thread, so the first few calls land before there is
    // anything to resolve against. Leaving the key unrecorded is what makes them retry;
    // storing it here regardless would drop the finish for the rest of the session.
    auto* vpk = archive();
    if ( !vpk )
        return;

    g_finish_key = key;

    // load_finish resolves the archive paths; the GPU textures are created lazily on the
    // next render, out of the preview's own cache.
    chams::g_inspect.set_finish( chams::load_finish( *vpk, material ? material : "",
        color ? color : "", normal ? normal : "", roughness ? roughness : "",
        ao ? ao : "", mask ? mask : "", palette ) );
}

void clear_finish()
{
    if ( g_finish_key.empty() )
        return;
    g_finish_key.clear();
    chams::g_inspect.clear_finish();
}

void set_wear( const float wear )
{
    chams::g_inspect.set_wear( std::clamp( wear, 0.0f, 1.0f ) );
}

void set_chams( const int type, const ImVec4& color, const float exponent, const bool overlay )
{
    using mat = config::visual_profile::chams;
    g_chams_on = type >= 0 && type < mat::material_type_count && chams::g_renderer.ready();
    chams::g_inspect.set_chams_overlay( overlay );
    if ( !g_chams_on )
        return;
    g_chams = {};
    g_chams.enabled = true;
    g_chams.type = type;
    g_chams.color = { channel( color.x ), channel( color.y ), channel( color.z ), channel( color.w ) };
    const float e = std::clamp( exponent, 0.0f, 100.0f ) / 100.0f;
    if ( type == mat::glow )
        g_chams.exponent = 0.5f + e * 6.0f;
    else if ( type == mat::glow_outline || type == mat::glossy )
        g_chams.exponent = 0.25f + e * 3.5f;
}

void orbit( float dx, float dy )
{
    if ( dx != 0.0f )
        chams::g_inspect.orbit( dx );
    if ( dy != 0.0f )
        chams::g_inspect.orbit_pitch( dy );
}

void zoom( float notches )
{
    if ( notches != 0.0f )
        chams::g_inspect.zoom( notches > 0.0f ? 1.15f : 1.0f / 1.15f );
}

void reset_view()
{
    chams::g_inspect.set_view( k_default_yaw, k_default_pitch, 1.0f );
}

Frame render( float width, float height )
{
    Frame result{};
    if ( g_model_path.empty() || !ensure_preview() )
        return result;
    auto* vpk = archive();
    if ( !vpk )
        return result;

    const auto target_width = static_cast< std::uint32_t >(
        std::clamp( width, 16.0f, 2048.0f ) );
    const auto target_height = static_cast< std::uint32_t >(
        std::clamp( height, 16.0f, 2048.0f ) );

    g_target_width = target_width;
    g_target_height = target_height;

    auto* srv = chams::g_inspect.render( *vpk, g_model_path, target_width, target_height,
        g_chams_on ? &g_chams : nullptr );
    if ( !srv )
        return result;

    result.texture = reinterpret_cast< ImTextureID >( srv );
    result.ready = true;
    result.width = static_cast< float >( target_width );
    result.height = static_cast< float >( target_height );
    return result;
}

} // namespace nl::model_preview
