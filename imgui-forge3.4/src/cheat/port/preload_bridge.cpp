#include <stdafx.hpp>

#include <port/preload.hpp>

#include "menu/agent_models.h"
#include "menu/config.h"
#include "menu/esp_model.h"
#include "menu/inventory_catalog.h"
#include "menu/map_preview.h"
#include "menu/model_catalog.h"
#include "menu/model_preview.h"
#include "menu/sky_list.h"

#include <render/chams/mesh_cache.hpp>
#include <render/chams/renderer.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

namespace nl_cheat::preload {
namespace {

using clock = std::chrono::steady_clock;

enum class stage { archive, previews, meshes, textures, done };

constexpr float k_archive_cap = 30.0f;
constexpr float k_previews_cap = 10.0f;
constexpr float k_meshes_cap = 20.0f;
constexpr float k_textures_cap = 10.0f;

struct state {
    stage current = stage::archive;
    clock::time_point started{};
    clock::time_point stage_started{};
    bool started_once = false;

    std::vector<std::string> maps;
    int maps_total = 0;
    int skies_total = 0;
    // Mesh cache keys, the way the previews will ask for them.
    std::vector<std::string> meshes;
    // Inventory models whose textures are warmed with one small render each.
    std::vector<std::string> models;
    std::size_t next_model = 0;
    int model_frames = 0;
};

state g;

float seconds_since(clock::time_point t)
{
    return std::chrono::duration<float>(clock::now() - t).count();
}

void enter(stage next)
{
    g.current = next;
    g.stage_started = clock::now();
}

void add_unique(std::vector<std::string>& list, const char* value)
{
    if (value && *value && std::find(list.begin(), list.end(), value) == list.end())
        list.emplace_back(value);
}

// The models the inventory and the ESP preview open first: every equipped slot of both
// teams and the default agents.
void collect(const nl::Config& config)
{
    for (int i = 1; i < config.visuals.world.map_count; ++i)
        add_unique(g.maps, config.visuals.world.map_name(i));
    add_unique(g.maps, nl::esp_model::current_map_name());

    std::vector<std::string> paths;
    for (const auto& side : config.inventory.loadout) {
        for (const nl::inv::Equip& equip : side) {
            if (equip.weapon < 0 || equip.weapon >= nl::inv::weapon_count)
                continue;
            const nl::inv::Kind kind = nl::inv::weapons[equip.weapon].kind;
            if (kind == nl::inv::Kind::agent) {
                if (equip.item >= 0 && equip.item < nl::inv::item_count)
                    add_unique(paths, nl::agents::model_for_name(nl::inv::items[equip.item].name));
            } else {
                add_unique(paths, nl::models::weapon(equip.weapon));
            }
        }
    }
    add_unique(paths, nl::agents::default_model(nl::agents::Team::ct));
    add_unique(paths, nl::agents::default_model(nl::agents::Team::t));

    for (const auto& path : paths) {
        const bool weapon = path.find("weapons/models/") != std::string::npos;
        g.meshes.push_back(weapon ? path + "|hd" : path);
        g.models.push_back(path);
    }
}

int meshes_settled()
{
    auto& vpk = chams::g_renderer.vpk();
    int settled = 0;
    for (const auto& key : g.meshes) {
        if (chams::g_mesh_cache.settled(key)) {
            ++settled;
            continue;
        }
        // Keeps the two extraction workers busy: a request made while both are taken is
        // simply not queued, so every frame asks again.
        static_cast<void>(chams::g_mesh_cache.get_or_build(vpk, key));
    }
    return settled;
}

} // namespace

status pump(const nl::Config& config)
{
    if (!g.started_once) {
        g.started_once = true;
        g.started = g.stage_started = clock::now();
    }

    status out{};
    switch (g.current) {
    case stage::archive: {
        out.text = "Opening game files";
        // The pak index is tens of megabytes; the bar creeps while it loads.
        out.progress = 0.22f * (1.0f - std::exp(-seconds_since(g.stage_started) / 2.5f));
        if (chams::g_renderer.ensure_vpk()) {
            collect(config);
            for (const auto& map : g.maps)
                nl::maps::request(map.c_str());
            nl::skies::warm();
            g.maps_total = std::max<int>(1, static_cast<int>(g.maps.size()));
            g.skies_total = std::max(1, nl::skies::pending());
            enter(stage::previews);
        } else if (seconds_since(g.stage_started) > k_archive_cap) {
            enter(stage::done);
        }
        break;
    }
    case stage::previews: {
        out.text = "Parsing map and sky previews";
        const int maps_left = nl::maps::pending();
        const int skies_left = nl::skies::pending();
        const float left = static_cast<float>(maps_left + skies_left) / static_cast<float>(g.maps_total + g.skies_total);
        out.progress = 0.22f + 0.28f * (1.0f - std::clamp(left, 0.0f, 1.0f));
        if ((maps_left == 0 && skies_left == 0) || seconds_since(g.stage_started) > k_previews_cap)
            enter(stage::meshes);
        break;
    }
    case stage::meshes: {
        out.text = "Building inventory models";
        const int settled = meshes_settled();
        const float share = g.meshes.empty() ? 1.0f : static_cast<float>(settled) / static_cast<float>(g.meshes.size());
        out.progress = 0.5f + 0.35f * share;
        if (share >= 1.0f || seconds_since(g.stage_started) > k_meshes_cap)
            enter(stage::textures);
        break;
    }
    case stage::textures: {
        out.text = "Loading textures";
        // One small render per model and frame: it uploads the mesh and reads every
        // texture the model uses, which is what made the first look at a weapon hitch.
        if (g.next_model < g.models.size() && nl::model_preview::available()) {
            nl::model_preview::set_model(g.models[g.next_model].c_str());
            const auto frame = nl::model_preview::render(96.0f, 96.0f);
            if (frame.ready || ++g.model_frames > 30) {
                ++g.next_model;
                g.model_frames = 0;
            }
        } else {
            g.next_model = g.models.size();
        }
        const float share = g.models.empty() ? 1.0f : static_cast<float>(g.next_model) / static_cast<float>(g.models.size());
        out.progress = 0.85f + 0.15f * share;
        if (g.next_model >= g.models.size() || seconds_since(g.stage_started) > k_textures_cap) {
            nl::model_preview::set_model("");
            enter(stage::done);
        }
        break;
    }
    case stage::done:
        break;
    }

    if (g.current == stage::done) {
        out.text = "Ready";
        out.progress = 1.0f;
        out.done = true;
    }
    return out;
}

} // namespace nl_cheat::preload
