cmake_minimum_required(VERSION 3.21)
# Debug info is kept in every build: the harness and the injected DLL both write crash
# logs with raw addresses, and llvm-addr2line needs a symbol table to resolve them.
if (NOT MSVC)
    add_compile_options(-g)
    add_link_options(-g)
endif()

project(neverlose_imgui LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(cmake/EmbedAssets.cmake)

# ---------------------------------------------------------------- Dear ImGui
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
# Note: IMGUI_DISABLE_OBSOLETE_FUNCTIONS is intentionally NOT defined. The cheat
# backend (overlay font pipeline, legacy draw-list entry points) relies on the
# pre-1.92.8 APIs; the menu itself never calls them. The cheat code also does
# plain ImVec2/ImVec4 arithmetic.
target_compile_definitions(imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS)

# ------------------------------------------------------------------- assets
set(NL_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${NL_GENERATED_DIR})
nl_embed_assets(${NL_GENERATED_DIR}/embedded_assets.cpp
    noto_sans_medium   assets/fonts/NotoSans-Medium.ttf
    noto_sans_semibold assets/fonts/NotoSans-SemiBold.ttf
    noto_sans_black    assets/fonts/NotoSans-Black.ttf
    fa_solid           assets/fonts/fa-solid-900.ttf
    avatar_rgba        assets/images/avatar.rgba
    inventory_pak      assets/images/inventory.pak
)

# --------------------------------------------------------------------- menu
add_library(nl_menu STATIC
    src/menu/agent_models_data.cpp
    src/menu/agent_models_lookup.cpp
    src/menu/ambience.cpp
    src/menu/backdrop.cpp
    src/menu/binds.cpp
    src/menu/boot.cpp
    src/menu/blur.cpp
    src/menu/config.cpp
    src/menu/draw.cpp
    src/menu/map_catalog_data.cpp
    src/menu/map_catalog_lookup.cpp
    src/menu/map_preview.cpp
    src/menu/model_catalog.cpp
    src/menu/model_preview.cpp
    src/menu/esp_model.cpp
    src/menu/esp_preview.cpp
    src/menu/fonts.cpp
    src/menu/images.cpp
    src/menu/inventory.cpp
    src/menu/inventory_catalog.cpp
    src/menu/inventory_ids.cpp
    src/menu/menu.cpp
    src/menu/panel.cpp
    src/menu/popup.cpp
    src/menu/presets.cpp
    src/menu/search.cpp
    src/menu/search_overlay.cpp
    src/menu/sky_list.cpp
    src/menu/tabs.cpp
    src/menu/visuals.cpp
    src/menu/watermark.cpp
    src/menu/widgets.cpp
    ${NL_GENERATED_DIR}/embedded_assets.cpp
)
target_include_directories(nl_menu PUBLIC src)
target_include_directories(nl_menu PRIVATE third_party/stb)
target_link_libraries(nl_menu PUBLIC imgui)

if(MSVC)
    target_compile_options(nl_menu PRIVATE /W4 /utf-8)
    target_compile_options(imgui PRIVATE /utf-8)
else()
    target_compile_options(nl_menu PRIVATE -Wall -Wextra)
endif()

# ------------------------------------------------------------------ cheat
# Vesta pipeline vendored under src/cheat: external memory reader, state,
# simulation, features, chams/overlay and the Lua runtime. The Neverlose menu
# only feeds it config; the menu itself is untouched.
set(CHEAT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/cheat)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/configs/legit.cfg" VESTA_DEFAULT_PROFILE_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," VESTA_DEFAULT_PROFILE_BYTES
    "${VESTA_DEFAULT_PROFILE_HEX}")
configure_file(
    "${CHEAT_DIR}/config/default_profile.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/config/default_profile.hpp"
    @ONLY
)

# --- vendored third-party libraries ----------------------------------------
if(WIN32)
file(GLOB cheat_lua_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua/src/*.c")
list(FILTER cheat_lua_sources EXCLUDE REGEX "/(lua|luac|onelua)\\.c$")
add_library(cheat_lua STATIC ${cheat_lua_sources})
target_include_directories(cheat_lua PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua/src")

add_library(cheat_lz4 STATIC
    third_party/lz4/lib/lz4.c
    third_party/lz4/lib/lz4hc.c
)
target_include_directories(cheat_lz4 PUBLIC third_party/lz4/lib)

add_library(cheat_zydis STATIC third_party/zydis/zydis.c)
target_include_directories(cheat_zydis PUBLIC third_party/zydis)
target_compile_definitions(cheat_zydis PUBLIC ZYDIS_STATIC_BUILD ZYCORE_STATIC_BUILD)

set(cheat_zstd_sources
    third_party/zstd/lib/common/entropy_common.c
    third_party/zstd/lib/common/error_private.c
    third_party/zstd/lib/common/fse_decompress.c
    third_party/zstd/lib/common/xxhash.c
    third_party/zstd/lib/common/zstd_common.c
    third_party/zstd/lib/decompress/huf_decompress.c
    third_party/zstd/lib/decompress/zstd_ddict.c
    third_party/zstd/lib/decompress/zstd_decompress.c
    third_party/zstd/lib/decompress/zstd_decompress_block.c
)
add_library(cheat_zstd STATIC ${cheat_zstd_sources})
target_include_directories(cheat_zstd PUBLIC third_party/zstd/lib)

file(GLOB cheat_meshopt_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/meshoptimizer-1.2/src/*.cpp")
add_library(cheat_meshopt STATIC ${cheat_meshopt_sources})
target_include_directories(cheat_meshopt PUBLIC third_party/meshoptimizer-1.2/src)

# --- the cheat core ---------------------------------------------------------
set(cheat_sources
    ${CHEAT_DIR}/app/workers.cpp
    ${CHEAT_DIR}/app/map_loader.cpp

    ${CHEAT_DIR}/config/settings.cpp

    ${CHEAT_DIR}/core/assets/kv3.cpp
    ${CHEAT_DIR}/core/assets/resource.cpp
    ${CHEAT_DIR}/core/assets/vpk.cpp
    ${CHEAT_DIR}/core/input/bindings.cpp
    ${CHEAT_DIR}/core/input/hotkeys.cpp
    ${CHEAT_DIR}/core/input/input.cpp
    ${CHEAT_DIR}/core/math/interpolation.cpp
    ${CHEAT_DIR}/core/math/random.cpp
    ${CHEAT_DIR}/core/math/vector.cpp
    ${CHEAT_DIR}/core/memory/addresses.cpp
    ${CHEAT_DIR}/core/memory/convars.cpp
    ${CHEAT_DIR}/core/memory/compatibility.cpp
    ${CHEAT_DIR}/core/memory/entities.cpp
    ${CHEAT_DIR}/core/memory/modules.cpp
    ${CHEAT_DIR}/core/memory/process.cpp
    ${CHEAT_DIR}/core/memory/schemas.cpp
    ${CHEAT_DIR}/core/state/camera.cpp
    ${CHEAT_DIR}/core/state/local.cpp
    ${CHEAT_DIR}/core/state/pose.cpp
    ${CHEAT_DIR}/core/state/world.cpp
    ${CHEAT_DIR}/core/state/seed_players.cpp

    ${CHEAT_DIR}/simulation/shot_state.cpp
    ${CHEAT_DIR}/simulation/ballistics.cpp
    ${CHEAT_DIR}/simulation/seed_weapon.cpp
    ${CHEAT_DIR}/simulation/blast.cpp
    ${CHEAT_DIR}/simulation/blast_loader.cpp
    ${CHEAT_DIR}/simulation/bounds.cpp
    ${CHEAT_DIR}/simulation/collision.cpp
    ${CHEAT_DIR}/simulation/grenade.cpp
    ${CHEAT_DIR}/simulation/hitboxes.cpp
    ${CHEAT_DIR}/simulation/map_collision.cpp
    ${CHEAT_DIR}/simulation/penetration.cpp
    ${CHEAT_DIR}/simulation/skeleton.cpp

    ${CHEAT_DIR}/features/aimbot/aimbot.cpp
    ${CHEAT_DIR}/features/aimbot/grenade_aim.cpp
    ${CHEAT_DIR}/features/combat/ragebot.cpp
    ${CHEAT_DIR}/features/combat/usercmd.cpp
    ${CHEAT_DIR}/features/world/world_hooks.cpp
    ${CHEAT_DIR}/core/hooking/trampoline.cpp
    ${CHEAT_DIR}/features/trigger/seed_trigger.cpp
    ${CHEAT_DIR}/features/trigger/seed_plan.cpp
    ${CHEAT_DIR}/features/trigger/seed_state.cpp
    ${CHEAT_DIR}/features/visuals/avatars.cpp
    ${CHEAT_DIR}/features/visuals/bomb.cpp
    ${CHEAT_DIR}/features/visuals/bullet_impacts.cpp
    ${CHEAT_DIR}/features/visuals/crosshair.cpp
    ${CHEAT_DIR}/features/visuals/event_log.cpp
    ${CHEAT_DIR}/features/visuals/grenade_prediction.cpp
    ${CHEAT_DIR}/features/visuals/hitsound.cpp
    ${CHEAT_DIR}/features/visuals/item.cpp
    ${CHEAT_DIR}/features/visuals/player.cpp
    ${CHEAT_DIR}/features/visuals/projectile.cpp
    ${CHEAT_DIR}/features/visuals/radar.cpp
    ${CHEAT_DIR}/features/visuals/sound.cpp
    ${CHEAT_DIR}/features/misc/ambience.cpp
    ${CHEAT_DIR}/features/misc/skybox.cpp
    ${CHEAT_DIR}/features/misc/weather.cpp
    ${CHEAT_DIR}/features/misc/auto_accept.cpp
    ${CHEAT_DIR}/features/misc/auto_stop.cpp
    ${CHEAT_DIR}/features/misc/bhop.cpp
    ${CHEAT_DIR}/features/misc/misc_extras.cpp
    ${CHEAT_DIR}/features/misc/thirdperson.cpp
    ${CHEAT_DIR}/features/misc/nade_helper.cpp
    ${CHEAT_DIR}/features/skins/skins.cpp

    ${CHEAT_DIR}/render/chams/animation.cpp
    ${CHEAT_DIR}/render/chams/material.cpp
    ${CHEAT_DIR}/render/chams/mesh_cache.cpp
    ${CHEAT_DIR}/render/chams/mesh_extract.cpp
    ${CHEAT_DIR}/render/chams/preview.cpp
    ${CHEAT_DIR}/render/chams/renderer.cpp
    ${CHEAT_DIR}/render/chams/texture.cpp
    ${CHEAT_DIR}/render/overlay/input.cpp
    ${CHEAT_DIR}/render/overlay/overlay.cpp
    ${CHEAT_DIR}/render/overlay/hud.cpp
    ${CHEAT_DIR}/render/overlay/ui.cpp
    ${CHEAT_DIR}/render/overlay/ui_legacy.cpp
    ${CHEAT_DIR}/render/overlay/window.cpp
    ${CHEAT_DIR}/render/menu/menu.cpp
    ${CHEAT_DIR}/render/menu/localization.cpp

    ${CHEAT_DIR}/system/diagnostics.cpp
    ${CHEAT_DIR}/system/frame_limiter.cpp
    ${CHEAT_DIR}/system/performance.cpp
    ${CHEAT_DIR}/system/storage.cpp

    ${CHEAT_DIR}/scripting/runtime.cpp
    ${CHEAT_DIR}/scripting/radar_asset.cpp

    ${CHEAT_DIR}/port/cheat_bridge.cpp
    ${CHEAT_DIR}/port/config_bridge.cpp
    ${CHEAT_DIR}/port/esp_model_bridge.cpp
    ${CHEAT_DIR}/port/map_preview_bridge.cpp
    ${CHEAT_DIR}/port/model_preview_bridge.cpp
    ${CHEAT_DIR}/port/preload_bridge.cpp
    ${CHEAT_DIR}/port/sky_bridge.cpp
    ${CHEAT_DIR}/port/probe.cpp
    ${CHEAT_DIR}/port/hook_hud.cpp
)

add_library(cheat STATIC ${cheat_sources})
target_compile_features(cheat PUBLIC cxx_std_23)
target_include_directories(cheat PUBLIC
    ${CHEAT_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_include_directories(cheat PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    # The map previews decode the icons out of pak01 with stb, the same library the menu
    # uses for the inventory pictures.
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
)
target_compile_definitions(cheat PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
    _CRT_SECURE_NO_WARNINGS
    VESTA_ENABLE_CONSOLE=0
    VESTA_SHOT_TRACE_ENABLED=0
)
target_precompile_headers(cheat PRIVATE [["stdafx.hpp"]])
target_link_libraries(cheat PUBLIC imgui cheat_lua cheat_lz4 cheat_zstd cheat_meshopt cheat_zydis)
endif()

# ------------------------------------------------------------------ targets
if(WIN32)
    # Windows: Win32 + DirectX 11 (same stack as an in-game overlay).
    enable_language(RC)
    add_executable(neverlose_menu WIN32
        src/platform/main_win32_dx11.cpp
        src/platform/blur_dx11.cpp
        src/platform/app.rc
        ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
        ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    )
    target_include_directories(neverlose_menu PRIVATE assets/images)
    target_link_libraries(neverlose_menu PRIVATE nl_menu cheat d3d11 dxgi dwmapi dcomp windowscodecs ole32 winmm comdlg32 advapi32 shell32 ntdll winhttp)
    if(MINGW)
        target_link_libraries(neverlose_menu PRIVATE d3dcompiler_47 xaudio2_9)
        target_link_options(neverlose_menu PRIVATE -static -static-libgcc -static-libstdc++ -s)
    else()
        target_link_libraries(neverlose_menu PRIVATE d3dcompiler xaudio2)
    endif()

    # Injected build: lives inside cs2.exe and draws the menu in the game frame.
    add_library(neverlose_internal SHARED
        src/platform/main_dll.cpp
        # The menu's frosted-glass backdrop is host-provided; without this the
        # popups fall back to an opaque fill in game.
        src/platform/blur_dx11.cpp
        ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
        ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    )
    set_target_properties(neverlose_internal PROPERTIES OUTPUT_NAME "neverlose_menu" PREFIX "")
    target_link_libraries(neverlose_internal PRIVATE nl_menu cheat d3d11 dxgi dwmapi dcomp windowscodecs ole32 winmm comdlg32 advapi32 shell32 ntdll winhttp)
    if(MINGW)
        target_link_libraries(neverlose_internal PRIVATE d3dcompiler_47 xaudio2_9)
        target_link_options(neverlose_internal PRIVATE -static -static-libgcc -static-libstdc++ -s)
    else()
        target_link_libraries(neverlose_internal PRIVATE d3dcompiler xaudio2)
    endif()
    set_target_properties(neverlose_internal PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

    # Injector with the loader window. It also probes the genuine Present RVA, so
    # it needs its own D3D11/DXGI device. requireAdministrator lives in the
    # manifest (loader.rc): writing into cs2.exe needs an admin token.
    nl_embed_assets(${NL_GENERATED_DIR}/loader_assets.cpp
        noto_sans_medium   assets/fonts/NotoSans-Medium.ttf
        noto_sans_semibold assets/fonts/NotoSans-SemiBold.ttf
        noto_sans_black    assets/fonts/NotoSans-Black.ttf
        fa_solid           assets/fonts/fa-solid-900.ttf
        avatar_rgba        assets/images/avatar.rgba
        cs2_rgba           assets/images/cs2.rgba
    )
    add_executable(neverlose_loader WIN32
        src/platform/loader.cpp
        src/platform/loader.rc
        src/platform/app.rc
        src/platform/main_loader_dx11.cpp
        src/loader/loader.cpp
        src/loader/host_style.cpp
        src/menu/blur.cpp
        src/menu/draw.cpp
        src/menu/fonts.cpp
        src/menu/widgets.cpp
        ${NL_GENERATED_DIR}/loader_assets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
        ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    )
    # cs2_rgba is not declared in assets.h; without this the generated const has internal linkage.
    set_source_files_properties(${NL_GENERATED_DIR}/loader_assets.cpp PROPERTIES
        COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/src/loader/cs2_extern.h")
    target_include_directories(neverlose_loader PRIVATE src assets/images)
    target_link_libraries(neverlose_loader PRIVATE imgui d3d11 dxgi dwmapi ole32 oleaut32 uuid shell32 advapi32)
    if(MINGW)
        target_link_libraries(neverlose_loader PRIVATE d3dcompiler_47)
        target_link_options(neverlose_loader PRIVATE -static -static-libgcc -static-libstdc++ -s)
    else()
        target_compile_options(neverlose_loader PRIVATE /utf-8)
        target_link_libraries(neverlose_loader PRIVATE d3dcompiler)
    endif()
    set_target_properties(neverlose_loader PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
else()
    # Linux/macOS preview: GLFW + OpenGL 3. Also used for pixel-diff screenshots.
    find_package(OpenGL REQUIRED)
    find_package(glfw3 3.3 REQUIRED)
    add_executable(neverlose_menu
        src/platform/main_glfw_opengl3.cpp
        src/platform/blur_gl.cpp
        ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_link_libraries(neverlose_menu PRIVATE nl_menu glfw OpenGL::GL ${CMAKE_DL_LIBS})
endif()
