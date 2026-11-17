#pragma once

#include <core/assets/vpk.hpp>

#include <string>
#include <vector>

namespace chams {

	struct material_data
	{
		std::string shader{}; // e.g. "csgo_character.vfx"

		// Archive paths of the texture slots, as referenced by the material.
		// These end in ".vtex"; load_texture normalizes that to ".vtex_c".
		std::string color{};
		std::string normal{};
		std::string metalness{};
		std::string ambient_occlusion{};
		std::string gloss{}; // g_tAnisoGloss -- drives roughness

		// Paint materials (csgo_customweapon.vfx) put the finish in g_tPattern rather than
		// g_tColor.
		std::string pattern{};

		// Per-weapon paint inputs (csgo_composite_inputs.vfx): which regions take paint
		// and the object-space position each texel of the unwrap sits at.
		std::string masks{};
		std::string position{};
		float uv_scale1{ 1.0f };
		float weapon_length1{ 0.0f };

		// g_nPatternTexture{Horizontal,Vertical}Sampling: 0 wrap, 1 mirror, 2 clamp.
		int pattern_sampling[ 2 ]{ 0, 0 };
		float color_brightness{ 1.0f };
		float paint_roughness{ -1.0f }; // -1: the material did not say

		// Style paints -- Fade, Doppler, Marble Fade, Lightning Strike -- ship a
		// paint-by-number mask in g_tPattern plus the colours it indexes in g_vColor0..3,
		// so for those the pattern is not the artwork at all.
		float palette[ 4 ][ 3 ]{};
		bool has_palette{};

		float pattern_uv_offset[ 2 ]{ 0.0f, 0.0f };
		float pattern_scale{ 1.0f };
		float pattern_rotation{ 0.0f }; // degrees, as the material stores it

		// Shader enum F_PAINT_STYLE. 4 and 5 are the anodized families, which sample
		// the pattern through the mesh's second UV rather than the albedo unwrap.
		// -1 means the material did not say.
		int paint_style{ -1 };
		bool ignore_size{};

		float uv_scale[ 2 ]{ 1.0f, 1.0f };
		float uv_offset[ 2 ]{ 0.0f, 0.0f };

		float color_tint[ 4 ]{ 1.0f, 1.0f, 1.0f, 1.0f };

		// csgo_customglove.vfx. The layer mask's alpha picks one of the eight tints;
		// a pattern, when the material asks for one, replaces a single tint.
		std::string layer_mask{};
		std::string surface{};
		float glove_tint[ 8 ][ 3 ]{};
		int glove_pattern{};
		int glove_pattern_mode{};
		int glove_pattern_replace{ -1 };
		int glove_flip{};
		float glove_pattern_palette[ 4 ]{};
		// csgo_customglove_preview.vfx: the tint id is an R8 map, and a bit per
		// region says the pattern picture replaces that colour.
		std::string tint_id{};
		float id_color[ 8 ][ 3 ]{ { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 },
			{ 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 } };
		int id_pattern_bits{};
		float pattern_scale_xy[ 2 ]{ 1.0f, 1.0f };
		float surface_tint[ 3 ]{ 1.0f, 1.0f, 1.0f };

		// Weapon paints: the scratch map wear removes paint through, and the grime laid
		// over worn finishes, each with its own transform.
		std::string wear{};
		std::string grunge{};
		float wear_scale{ 1.0f };
		float wear_rotation{ 0.0f };
		float grunge_scale{ 1.0f };
		float grunge_rotation{ 0.0f };

		[[nodiscard]] bool valid( ) const { return !color.empty( ); }
	};

	// What the inventory paints over a weapon model. Either a finish material, which names
	// its own slots, or explicit maps for the newest finishes, which ship without one.
	struct finish_data
	{
		std::string color{}; // the finish image itself
		std::string mask{};  // where it covers the base weapon, optional
		std::string normal{};
		std::string roughness{};
		std::string ambient_occlusion{};

		// Set only when the finish brought its own map: the model's material is the better
		// source of geometric detail otherwise, since a finish material's normal slot is
		// usually a shared placeholder.
		bool has_normal{};
		bool has_roughness{};
		bool has_ao{};

		// Style paints: the colour texture holds region weights, not colour, and these are
		// the colours it selects between.
		float palette[ 4 ][ 3 ]{};
		bool palette_mode{};

		// Solid (0) and anodized (3) paints have no pattern at all: the palette alone is
		// the finish, laid over the weapon's paint regions.
		bool solid{};

		// Loose-map finishes are pictures of the HD unwrap with their own coverage mask;
		// finish materials are painted through the weapon's composite inputs.
		bool loose{};

		// The paint kit's use_legacy_model: the classic picture paints were drawn on the
		// legacy unwrap, everything procedural (Fade, Doppler, solid, spray) is painted
		// on the HD model.
		bool legacy_model{};

		// The pattern transform, in the order the shader applies it: rotate, scale, then
		// shift by the offset.
		float pattern_uv_offset[ 2 ]{ 0.0f, 0.0f };
		float pattern_scale{ 1.0f };
		float pattern_rotation{ 0.0f };
		int pattern_sampling[ 2 ]{ 0, 0 };
		int paint_style{ -1 };
		bool ignore_size{};
		float color_brightness{ 1.0f };
		float paint_roughness{ 0.4f };

		// Gloves: eight region colours, and which of them a pattern replaces
		// (0-based, -1 when the material has no pattern).
		bool glove{};
		float glove_tint[ 8 ][ 3 ]{};
		int pattern_mode{};
		int pattern_replace{ -1 };
		int pattern_palette[ 4 ]{};
		bool flip_v{};
		// Preview gloves: per-axis pattern scale, and a bit per tint the picture replaces.
		float pattern_scale_v{ 1.0f };
		int pattern_bits{};

		// Wear inputs of the paint (shared defaults for loose finishes, none for gloves).
		std::string wear{};
		std::string grunge{};
		float wear_scale{ 1.0f };
		float wear_rotation{ 0.0f };
		float grunge_scale{ 1.0f };
		float grunge_rotation{ 0.0f };

		[[nodiscard]] bool valid( ) const { return !color.empty( ) || solid || glove; }
	};

	[[nodiscard]] material_data load_material( vpk_archive& vpk, const std::string& archive_path );

	// Resolves a catalogue finish: `material` when the finish is one, otherwise the loose
	// map paths the generator recorded. `palette` asks for the g_vColor0..3 treatment, which
	// only the style paints want. Returns an invalid finish when nothing resolves.
	[[nodiscard]] finish_data load_finish( vpk_archive& vpk, const std::string& material,
		const std::string& color, const std::string& normal, const std::string& roughness,
		const std::string& ambient_occlusion, const std::string& mask, bool palette );

} // namespace chams
