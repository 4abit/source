#include <stdafx.hpp>
#include <render/chams/preview.hpp>
#include <render/chams/renderer.hpp>
#include <render/chams/mesh_cache.hpp>
#include <render/chams/animation.hpp>

#include <d3dcompiler.h>

#include <cstdlib>
#include <limits>

namespace chams {

	namespace {

		struct cb_scene
		{
			float view_projection[ 4 ][ 4 ];
			float eye[ 3 ];
			float pad0;
			float tint[ 4 ];
			float uv_scale[ 2 ];
			float uv_offset[ 2 ];
			// Style paints index this with the colour texture's channels instead of
			// sampling it as colour, which is how Fade and Doppler get their gradients.
			float palette[ 4 ][ 4 ];
			float finish_uv_offset[ 2 ];
			float pattern_scale;
			float pattern_rotation;
			int has_normal;
			int has_metalness;
			int has_ao;
			int has_gloss;
			int has_finish;
			int paint_style;
			int mask_mode;  // 0 paint everywhere, 1 composite regions, 2 the finish's own mask
			int coord_mode; // 0 mesh UV, 1 pattern UV, 2 position-map projection
			int sampling[ 2 ];
			int has_position;
			int long_axis;
			float coord_scale;
			float brightness;
			float paint_roughness;
			int has_pattern;
			int palette_mode;
			int model_long;
			int model_up;
			int pad1;
			// Fallback projection for a part without a position map: model-space centre,
			// and w is one over the model's length.
			float model_center[ 4 ];
			// Gloves. g_ReplaceIndex is the 0-based tint a pattern stands in for, or -1.
			// g_PatternMode 2 means the pattern is already a colour; otherwise its
			// channels blend the four tints named by g_PatternPalette.
			int glove;
			int pattern_mode;
			int replace_index;
			int flip_v;
			float glove_tint[ 8 ][ 4 ];
			float pattern_palette[ 4 ];
			// x wear float, y scratch map scale, z its rotation (degrees), w scratch map bound.
			float wear_params[ 4 ];
			// x grunge scale, y rotation (degrees), z grunge map bound.
			float grunge_params[ 4 ];
		};
		static_assert( sizeof( cb_scene ) % 16 == 0 );

		constexpr const char* k_preview_shader = R"(
			cbuffer CBScene : register(b0)
			{
				row_major float4x4 g_ViewProjection;
				float3 g_EyePos;
				float  g_Pad0;
				float4 g_Tint;
				float2 g_UvScale;
				float2 g_UvOffset;
				float4 g_Palette[4];
				float2 g_FinishUvOffset;
				float  g_PatternScale;
				float  g_PatternRotation;
				int    g_HasNormal;
				int    g_HasMetalness;
				int    g_HasAO;
				int    g_HasGloss;
				int    g_HasFinish;
				int    g_PaintStyle;
				int    g_MaskMode;
				int    g_CoordMode;
				int    g_SamplingU;
				int    g_SamplingV;
				int    g_HasPosition;
				int    g_LongAxis;
				float  g_CoordScale;
				float  g_Brightness;
				float  g_PaintRoughness;
				int    g_HasPattern;
				int    g_PaletteMode;
				int    g_ModelLong;
				int    g_ModelUp;
				int    g_Pad1;
				float4 g_ModelCenter;
				int    g_Glove;
				int    g_PatternMode;
				int    g_ReplaceIndex;
				int    g_FlipV;
				float4 g_GloveTint[8];
				float4 g_PatternPalette;
				float4 g_WearParams;
				float4 g_GrungeParams;
			};

			Texture2D    g_Color      : register(t0);
			Texture2D    g_Normal     : register(t1);
			Texture2D    g_Metalness  : register(t2);
			Texture2D    g_AO         : register(t3);
			Texture2D    g_Gloss      : register(t4);
			Texture2D    g_Finish     : register(t5);
			Texture2D    g_FinishMask : register(t6);
			Texture2D    g_Position   : register(t7);
			Texture2D    g_Wear       : register(t8);
			Texture2D    g_Grunge     : register(t9);
			SamplerState g_Sampler    : register(s0);
			SamplerState g_Clamp      : register(s1);

			// g_nPatternTexture*Sampling: 0 wrap, 1 mirror, 2 clamp.
			float address(float x, int mode)
			{
				if (mode == 1)
					return 1.0f - abs(frac(x * 0.5f) * 2.0f - 1.0f);
				if (mode == 2)
					return saturate(x);
				return frac(x);
			}

			// The paint shader's palette: each pattern channel blends the next colour
			// over what the previous ones left.
			float3 palette_chain(float3 w)
			{
				float3 col = g_Palette[0].rgb;
				col = lerp(col, g_Palette[1].rgb, w.r);
				col = lerp(col, g_Palette[2].rgb, w.g);
				col = lerp(col, g_Palette[3].rgb, w.b);
				return col;
			}

			float3 glove_color(int slot)
			{
				slot = clamp(slot, 0, 7);
				float3 col = g_GloveTint[0].rgb;
				[unroll]
				for (int i = 1; i < 8; ++i)
					col = i == slot ? g_GloveTint[i].rgb : col;
				return col;
			}

			cbuffer CBBones : register(b1)
			{
				row_major float4x4 g_Bones[128];
			};

			struct VS_INPUT
			{
				float3 Position    : POSITION;
				float3 Normal      : NORMAL;
				float4 Tangent     : TANGENT;
				float2 UV          : TEXCOORD0;
				uint4  BoneIndices : BLENDINDICES;
				float4 BoneWeights : BLENDWEIGHT;
				float2 PatternUV   : TEXCOORD1;
			};

			struct PS_INPUT
			{
				float4 Position : SV_POSITION;
				float3 WorldPos : TEXCOORD0;
				float3 Normal   : TEXCOORD1;
				float3 Tangent  : TEXCOORD2;
				float3 Bitangent: TEXCOORD3;
				float2 UV       : TEXCOORD4;
				float2 PatternUV: TEXCOORD5;
			};

			PS_INPUT VS_Main(VS_INPUT input)
			{
				PS_INPUT output;
				float4 skinned = float4(0, 0, 0, 0);
				float3 normal  = float3(0, 0, 0);
				float3 tangent = float3(0, 0, 0);

				[unroll]
				for (int i = 0; i < 4; i++)
				{
					float weight = input.BoneWeights[i];
					if (weight > 0.0001f)
					{
						float4x4 bone = g_Bones[input.BoneIndices[i]];
						skinned += mul(bone, float4(input.Position, 1.0f)) * weight;
						normal  += mul((float3x3)bone, input.Normal) * weight;
						tangent += mul((float3x3)bone, input.Tangent.xyz) * weight;
					}
				}
				skinned.w = 1.0f;

				output.Position  = mul(g_ViewProjection, skinned);
				output.WorldPos  = skinned.xyz;
				output.Normal    = normal;
				output.Tangent   = tangent;
				output.Bitangent = cross(normal, tangent) * input.Tangent.w;
				output.UV        = input.UV * g_UvScale + g_UvOffset;
				output.PatternUV = input.PatternUV;
				return output;
			}

			float4 PS_Main(PS_INPUT input) : SV_TARGET
			{
				float4 base = g_Color.Sample(g_Sampler, input.UV) * g_Tint;
				float4 albedo = base;

				// Where paint goes, and whether it is the metallic anodized kind.
				float cover = 0.0f;
				float paint_metal = 0.0f;
				if (g_HasFinish != 0)
				{
					if (g_Glove != 0)
					{
						// Newer paints: the tint id is an R8 map, and some regions are a
						// picture instead of a flat colour. A white entry with no picture
						// is the cloth colour.
						if (g_PatternMode == 3)
						{
							float idr = g_FinishMask.Sample(g_Sampler, input.UV).r;
							int slot = min(7, (int)(idr * 8.0f + 0.02f));
							float3 paint = glove_color(slot);
							int bits = (int)(g_PatternPalette.z + 0.5f);
							if (dot(paint, float3(1.0f, 1.0f, 1.0f)) > 2.85f)
								paint *= g_Palette[0].rgb;
							if (g_HasPattern != 0 && (bits & (1 << slot)) != 0)
							{
								float2 uv = input.UV;
								if (g_FlipV != 0)
									uv.y = 1.0f - uv.y;
								uv *= max(g_PatternPalette.xy, float2(0.01f, 0.01f));
								// The picture is a decal: RGB over the region colour, alpha
								// says where the artwork actually is. A solid picture has
								// alpha 1 and simply replaces the colour.
								float4 pat = g_Finish.Sample(g_Sampler, uv);
								paint = lerp(paint, pat.rgb, pat.a);
							}
							albedo.rgb = paint;
							cover = 1.0f;
						}
						else
						{
						// Alpha is eight steps, one per g_vColorTint. RGB of the same
						// map is which cloth layer, which this preview does not shade.
						float4 id = g_FinishMask.Sample(g_Sampler, input.UV);
						int slot = min(7, (int)(id.a * 8.0f));
						float3 paint = glove_color(slot);
						if (g_HasPattern != 0 && g_ReplaceIndex >= 0 && slot == g_ReplaceIndex)
						{
							float2 coord = input.UV;
							if (g_FlipV != 0)
								coord.y = 1.0f - coord.y;
							float s, c;
							sincos(radians(g_PatternRotation), s, c);
							coord = float2(coord.x * c - coord.y * s, coord.x * s + coord.y * c)
							      * g_PatternScale - g_FinishUvOffset;
							float4 pattern = g_Finish.Sample(g_Sampler, coord);
							if (g_PatternMode == 2)
							{
								paint = pattern.rgb;
							}
							else
							{
								paint = glove_color((int)g_PatternPalette.x);
								paint = lerp(paint, glove_color((int)g_PatternPalette.y), saturate(pattern.r));
								paint = lerp(paint, glove_color((int)g_PatternPalette.z), saturate(pattern.g));
								paint = lerp(paint, glove_color((int)g_PatternPalette.w), saturate(pattern.b));
							}
						}
						albedo.rgb = paint;
						cover = 1.0f;
						}
					}
					else
					{
					// Composite regions: R is the bare metal the anodized styles colour,
					// G the parts no finish touches (baseplate, trigger, rivets); every
					// other style paints all but G.
					const bool anodized = g_PaintStyle >= 3 && g_PaintStyle <= 5;
					float4 regions = g_MaskMode != 0 ? g_FinishMask.Sample(g_Sampler, input.UV) : float4(0, 0, 0, 0);
					if (g_MaskMode == 1)
						cover = anodized ? regions.r : 1.0f - regions.g;
					else if (g_MaskMode == 2)
					{
						// Loose artwork is the colour. Its mask is a material id, and
						// which channel carries the islands is not the same on every kit
						// (some are empty in R and full in G, some are single-channel).
						cover = max(regions.r, max(regions.g, regions.b));
					}
					else
						cover = 1.0f;
					cover = saturate(cover);

					// Wear: the paint comes off through the scratch map, darkest texels
					// first, and the higher the float the more of it goes. Grime from the
					// grunge map settles on what is left.
					float grime = 0.0f;
					float3 grunge = float3(1.0f, 1.0f, 1.0f);
					if (g_WearParams.w > 0.5f && g_WearParams.x > 0.0f)
					{
						float ws, wc;
						sincos(radians(g_WearParams.z), ws, wc);
						float2 wuv = float2(input.UV.x * wc - input.UV.y * ws, input.UV.x * ws + input.UV.y * wc)
						           * max(g_WearParams.y, 0.01f);
						float scratch = g_Wear.Sample(g_Sampler, wuv).g;
						float threshold = pow(saturate(g_WearParams.x), 0.8f) * 0.85f;
						cover *= smoothstep(threshold - 0.05f, threshold + 0.05f, scratch);
						grime = saturate(g_WearParams.x * 1.5f);
					}
					if (g_GrungeParams.z > 0.5f && grime > 0.0f)
					{
						float gs, gc;
						sincos(radians(g_GrungeParams.y), gs, gc);
						float2 guv = float2(input.UV.x * gc - input.UV.y * gs, input.UV.x * gs + input.UV.y * gc)
						           * max(g_GrungeParams.x, 0.01f);
						grunge = saturate(g_Grunge.Sample(g_Sampler, guv).rgb * 1.4f);
					}

					// Spray and airbrushed paints (Fade) are projected from the side
					// through the position map: l runs along the weapon from the
					// muzzle (G = 0) back, h up it.
					float2 coord;
					if (g_CoordMode == 2)
					{
						float l, h;
						if (g_HasPosition != 0)
						{
							float3 P = g_Position.Sample(g_Clamp, input.UV).rgb;
							// Knives lie along B with the tip at B = 1; the tip takes the
							// place of a gun's muzzle (Fade: gold tip, purple base).
							l = g_LongAxis == 2 ? 0.5f - P.b : P.g - 0.5f;
							h = 0.5f - (g_LongAxis == 2 ? P.g : P.b);
							// One axis wraps (the flame rows) and the other is a clamped
							// colour ramp. At scale 1 a single flame tongue fills the gun;
							// stretching the ramp keeps the muzzle-to-rear gradient and
							// brings the tongues down to the size the paint was drawn at.
							if ((g_SamplingU == 0) != (g_SamplingV == 0))
								l *= 2.6f;
						}
						else
						{
							float3 d = (input.WorldPos - g_ModelCenter.xyz) * g_ModelCenter.w;
							l = -(g_ModelLong == 0 ? d.x : (g_ModelLong == 1 ? d.y : d.z));
							h = -(g_ModelUp == 0 ? d.x : (g_ModelUp == 1 ? d.y : d.z));
						}
						// u runs from the muzzle back, v up the weapon: Blaze's flames
						// (clamped across, tiled up) only line up this way round.
						coord = float2(l, h) * g_CoordScale;
					}
					else if (g_CoordMode == 1)
					{
						coord = input.PatternUV * g_CoordScale;
					}
					else
					{
						coord = input.UV;
					}

					float s, c;
					sincos(radians(g_PatternRotation), s, c);
					coord = float2(coord.x * c - coord.y * s, coord.x * s + coord.y * c) * g_PatternScale
					      - g_FinishUvOffset;

					float4 pattern = float4(0, 0, 0, 1);
					if (g_HasPattern != 0)
					{
						if (g_SamplingU == 0 && g_SamplingV == 0)
						{
							pattern = g_Finish.Sample(g_Sampler, coord);
						}
						else
						{
							// Addressed by hand so each axis gets its own mode; the
							// gradients come from the unwrapped coordinate so the fold
							// of a mirror does not drop to the smallest mip.
							float2 addressed = float2(address(coord.x, g_SamplingU), address(coord.y, g_SamplingV));
							pattern = g_Finish.SampleGrad(g_Clamp, addressed, ddx(coord), ddy(coord));
						}
					}

					float3 paint;
					if (g_PaletteMode != 0)
					{
						if (g_HasPattern != 0)
							paint = palette_chain(saturate(pattern.rgb));
						else if (g_PaintStyle == 3)
							paint = g_Palette[0].rgb;
						else
							paint = palette_chain(float3(regions.r, 0.0f, regions.b));
						paint *= g_Brightness;
					}
					else
					{
						paint = pattern.rgb;
					}

					paint = saturate(paint);
					paint *= lerp(float3(1.0f, 1.0f, 1.0f), grunge, grime * 0.5f);
					albedo.rgb = lerp(base.rgb, paint, cover);
					albedo.a = base.a;
					paint_metal = anodized ? cover : 0.0f;
					}
				}

				float3 N = normalize(input.Normal);
				if (g_HasNormal != 0)
				{
					// BC5 stores only XY; Z is reconstructed as the unit-length
					// completion, which is why the map has no blue channel.
					float2 packed = g_Normal.Sample(g_Sampler, input.UV).xy * 2.0f - 1.0f;
					float3 tangentNormal = float3(packed, sqrt(saturate(1.0f - dot(packed, packed))));
					float3x3 tbn = float3x3(normalize(input.Tangent), normalize(input.Bitangent), N);
					N = normalize(mul(tangentNormal, tbn));
				}

				float metalness = g_HasMetalness != 0 ? g_Metalness.Sample(g_Sampler, input.UV).r : 0.0f;
				float ao        = g_HasAO        != 0 ? g_AO.Sample(g_Sampler, input.UV).r : 1.0f;

				// Cloth and gear are close to fully rough; the default leans that
				// way so a material without a gloss map reads as fabric rather
				// than the latex look a hardcoded tight highlight produced.
				float roughness = 0.82f;
				if (g_HasGloss != 0)
				{
					roughness = saturate(1.0f - g_Gloss.Sample(g_Sampler, input.UV).r);
				}
				roughness = lerp(roughness, g_PaintRoughness, cover);

				float3 V = normalize(g_EyePos - input.WorldPos);
				N = dot(N, V) < 0.0f ? -N : N;

				// Studio rig against the near-black panel: a warm key from above the
				// camera, a cool fill from the other side, and sky ambient so the
				// underside stays a surface instead of a hole.
				float rim = pow(1.0f - saturate(dot(N, V)), 2.5f);
				const float3 rim_color = float3(1.00f, 0.88f, 0.72f);

				float3 key_dir = normalize(V + float3(0.25f, 0.90f, 0.40f));
				float3 fill_dir = normalize(V + float3(-0.90f, 0.20f, -0.15f));
				float key_ndot = saturate(dot(N, key_dir));
				float fill_ndot = saturate(dot(N, fill_dir));
				float front = saturate(dot(N, V) * 0.45f + 0.55f);
				float sky = saturate(N.z * 0.45f + 0.55f);
				float3 ambient = lerp(float3(0.36f, 0.37f, 0.40f), float3(0.58f, 0.59f, 0.62f), sky);
				float3 light = ambient
					+ float3(1.15f, 1.08f, 0.96f) * (key_ndot * 0.85f + front * 0.30f)
					+ float3(0.48f, 0.54f, 0.68f) * fill_ndot * 0.65f;

				// Anodized paint is a transparent dye over chrome: little diffuse, and the
				// colour comes back from the reflection instead.
				float3 diffuse = albedo.rgb * light * ao * (1.0f - 0.7f * paint_metal);

				// Broad, weak lobe: exponent falls with roughness and the whole
				// term is scaled down by it, so a rough surface keeps almost no
				// highlight instead of a plastic sheen.
				float3 H = normalize(key_dir + V);
				float gloss_exp = lerp(64.0f, 4.0f, roughness);
				float spec = pow(saturate(dot(N, H)), gloss_exp) * (1.0f - roughness) * 0.5f;
				float3 specColor = lerp(float3(0.25f, 0.25f, 0.25f), albedo.rgb, max(metalness * (1.0f - cover), paint_metal));

				// Studio environment for the metal: a bright softbox overhead fading to a
				// dark floor, plus the key light's own reflection.
				float3 R = reflect(-V, N);
				float sky_r = saturate(R.z * 0.5f + 0.5f);
				float env = lerp(0.10f, 0.95f, sky_r * sky_r)
				          + pow(saturate(dot(R, key_dir)), lerp(160.0f, 10.0f, roughness)) * (1.6f - roughness);
				float3 reflection = albedo.rgb * env * ao * paint_metal * 0.85f;

				float3 rgb = diffuse + reflection + specColor * spec * 0.65f + rim_color * albedo.rgb * rim * 0.22f;
				// Gentle shoulder so a bright finish rolls off instead of clipping to a
				// flat white patch.
				rgb = rgb / (1.0f + rgb * 0.18f);
				return float4(rgb, 1.0f);
			}
		)";

			[[nodiscard]] bool glove_model( const std::string& path )
		{
			return path.find( "glove_" ) != std::string::npos;
		}

		[[nodiscard]] int dominant_bone( const skinned_vertex& vertex )
		{
			int best = 0;
			auto weight = vertex.bone_weights[ 0 ];
			for ( int i = 1; i < 4; ++i )
			{
				if ( vertex.bone_weights[ i ] > weight )
				{
					weight = vertex.bone_weights[ i ];
					best = i;
				}
			}
			return static_cast< int >( vertex.bone_indices[ best ] );
		}

		[[nodiscard]] bool hand_bone( const std::string& name )
		{
			return name.find( "hand" ) != std::string::npos
				|| name.find( "Hand" ) != std::string::npos
				|| name.find( "finger" ) != std::string::npos
				|| name.find( "Finger" ) != std::string::npos
				|| name.find( "thumb" ) != std::string::npos
				|| name.find( "Thumb" ) != std::string::npos
				|| name.find( "wrist" ) != std::string::npos
				|| name.find( "Wrist" ) != std::string::npos;
		}

		[[nodiscard]] foundation::vec3 skinned_position( const skinned_vertex& vertex, const std::vector<bone_matrix>& skin )
		{
			float position[ 3 ]{};
			auto weight_sum = 0.0f;
			for ( int influence = 0; influence < 4; ++influence )
			{
				const auto weight = vertex.bone_weights[ influence ] / 255.0f;
				if ( weight < 0.001f )
					continue;
				const auto bone = vertex.bone_indices[ influence ];
				if ( bone >= skin.size( ) )
					continue;
				const auto& m = skin[ bone ];
				position[ 0 ] += ( m.m[ 0 ][ 0 ] * vertex.position[ 0 ] + m.m[ 0 ][ 1 ] * vertex.position[ 1 ] + m.m[ 0 ][ 2 ] * vertex.position[ 2 ] + m.m[ 0 ][ 3 ] ) * weight;
				position[ 1 ] += ( m.m[ 1 ][ 0 ] * vertex.position[ 0 ] + m.m[ 1 ][ 1 ] * vertex.position[ 1 ] + m.m[ 1 ][ 2 ] * vertex.position[ 2 ] + m.m[ 1 ][ 3 ] ) * weight;
				position[ 2 ] += ( m.m[ 2 ][ 0 ] * vertex.position[ 0 ] + m.m[ 2 ][ 1 ] * vertex.position[ 1 ] + m.m[ 2 ][ 2 ] * vertex.position[ 2 ] + m.m[ 2 ][ 3 ] ) * weight;
				weight_sum += weight;
			}
			if ( weight_sum < 0.001f )
				return { vertex.position[ 0 ], vertex.position[ 1 ], vertex.position[ 2 ] };
			return { position[ 0 ], position[ 1 ], position[ 2 ] };
		}

		[[nodiscard]] bool arm_weighted( const skinned_mesh& mesh, const skinned_vertex& vertex )
		{
			int best = 0;
			auto weight = vertex.bone_weights[ 0 ];
			for ( int i = 1; i < 4; ++i )
			{
				if ( vertex.bone_weights[ i ] > weight )
				{
					weight = vertex.bone_weights[ i ];
					best = i;
				}
			}

			const auto bone = vertex.bone_indices[ best ];
			if ( bone >= mesh.bones.size( ) )
			{
				return false;
			}

			const auto& name = mesh.bones[ bone ].name;
			return name.size( ) >= 4 && name.compare( 0, 4, "arm_" ) == 0;
		}

		[[nodiscard]] bool upper_arm_vertex( const skinned_mesh& mesh, const skinned_vertex& vertex )
		{
			int best = 0;
			auto weight = vertex.bone_weights[ 0 ];
			for ( int i = 1; i < 4; ++i )
			{
				if ( vertex.bone_weights[ i ] > weight )
				{
					weight = vertex.bone_weights[ i ];
					best = i;
				}
			}
			const auto bone = vertex.bone_indices[ best ];
			if ( bone >= mesh.bones.size( ) )
				return false;
			const auto& name = mesh.bones[ bone ].name;
			return name.find( "arm_upper" ) != std::string::npos
				|| name.find( "armUpper" ) != std::string::npos
				|| name.find( "Shoulder" ) != std::string::npos;
		}

		// Bakes the viewmodel inspect clip into the vertices. The clip is the one the
		// game plays when you look at your gloves: both hands up, turned inward.
		[[nodiscard]] skinned_mesh bake_glove_inspect( vpk_archive& vpk, const skinned_mesh& source )
		{
			static nm_skeleton skeleton{};
			static nm_clip clip{};
			static bool tried = false;
			if ( !tried )
			{
				tried = true;
				skeleton = load_nm_skeleton( vpk, "animation/skeletons/characters/viewmodel.vnmskel" );
				clip = load_nm_clip( vpk, "animation/anims/viewmodel/arms/inspects/pedestal_gloves_loop.vnmclip" );
			}
			if ( !skeleton.valid( ) || !clip.valid( ) )
				return source;

			const auto tracks = map_tracks_to_model( skeleton, source );
			std::vector<bone_matrix> skin{};
			std::vector<bone_matrix> world{};
			sample_pose( clip, skeleton, source, tracks, clip.duration * 0.05f, skin, world );

			skinned_mesh posed = source;
			for ( auto& vertex : posed.vertices )
			{
				float position[ 3 ]{};
				float normal[ 3 ]{};
				auto weight_sum = 0.0f;
				for ( int influence = 0; influence < 4; ++influence )
				{
					const auto weight = vertex.bone_weights[ influence ] / 255.0f;
					if ( weight < 0.001f )
						continue;
					const auto bone = vertex.bone_indices[ influence ];
					if ( bone >= skin.size( ) )
						continue;
					const auto& m = skin[ bone ];
					position[ 0 ] += ( m.m[ 0 ][ 0 ] * vertex.position[ 0 ] + m.m[ 0 ][ 1 ] * vertex.position[ 1 ] + m.m[ 0 ][ 2 ] * vertex.position[ 2 ] + m.m[ 0 ][ 3 ] ) * weight;
					position[ 1 ] += ( m.m[ 1 ][ 0 ] * vertex.position[ 0 ] + m.m[ 1 ][ 1 ] * vertex.position[ 1 ] + m.m[ 1 ][ 2 ] * vertex.position[ 2 ] + m.m[ 1 ][ 3 ] ) * weight;
					position[ 2 ] += ( m.m[ 2 ][ 0 ] * vertex.position[ 0 ] + m.m[ 2 ][ 1 ] * vertex.position[ 1 ] + m.m[ 2 ][ 2 ] * vertex.position[ 2 ] + m.m[ 2 ][ 3 ] ) * weight;
					normal[ 0 ] += ( m.m[ 0 ][ 0 ] * vertex.normal[ 0 ] + m.m[ 0 ][ 1 ] * vertex.normal[ 1 ] + m.m[ 0 ][ 2 ] * vertex.normal[ 2 ] ) * weight;
					normal[ 1 ] += ( m.m[ 1 ][ 0 ] * vertex.normal[ 0 ] + m.m[ 1 ][ 1 ] * vertex.normal[ 1 ] + m.m[ 1 ][ 2 ] * vertex.normal[ 2 ] ) * weight;
					normal[ 2 ] += ( m.m[ 2 ][ 0 ] * vertex.normal[ 0 ] + m.m[ 2 ][ 1 ] * vertex.normal[ 1 ] + m.m[ 2 ][ 2 ] * vertex.normal[ 2 ] ) * weight;
					weight_sum += weight;
				}
				if ( weight_sum < 0.001f )
					continue;
				vertex.position[ 0 ] = position[ 0 ];
				vertex.position[ 1 ] = position[ 1 ];
				vertex.position[ 2 ] = position[ 2 ];
				const auto length = std::sqrt( normal[ 0 ] * normal[ 0 ] + normal[ 1 ] * normal[ 1 ] + normal[ 2 ] * normal[ 2 ] );
				if ( length > 1e-6f )
				{
					vertex.normal[ 0 ] = normal[ 0 ] / length;
					vertex.normal[ 1 ] = normal[ 1 ] / length;
					vertex.normal[ 2 ] = normal[ 2 ] / length;
				}
				vertex.bone_indices[ 0 ] = 0;
				vertex.bone_indices[ 1 ] = vertex.bone_indices[ 2 ] = vertex.bone_indices[ 3 ] = 0;
				vertex.bone_weights[ 0 ] = 255;
				vertex.bone_weights[ 1 ] = vertex.bone_weights[ 2 ] = vertex.bone_weights[ 3 ] = 0;
			}

			posed.valid = true;
			return posed;
		}

	void build_view_projection( const foundation::vec3& eye, const foundation::vec3& target,
			float fov_radians, float aspect, float near_z, float far_z, float out[ 4 ][ 4 ] )
		{
			// Right-handed look-at, then a D3D-style [0,1] depth projection, in the
			// same row-major convention game::view uses: clip = M * (world, 1).
			auto forward = target - eye;
			const auto forward_len = forward.length( );
			if ( forward_len > 1e-6f )
			{
				forward /= forward_len;
			}

			const foundation::vec3 world_up{ 0.0f, 0.0f, 1.0f };
			auto right = forward.cross( world_up );
			const auto right_len = right.length( );
			right = right_len > 1e-6f ? right / right_len : foundation::vec3{ 0.0f, 1.0f, 0.0f };
			auto up = right.cross( forward );

			const auto h = 1.0f / std::tan( fov_radians * 0.5f );
			const auto w = h / aspect;
			const auto q = far_z / ( far_z - near_z );

			// View rows dotted with (world - eye), then scaled into clip space.
			const float view[ 3 ][ 3 ]{
				{ right.x, right.y, right.z },
				{ up.x, up.y, up.z },
				{ forward.x, forward.y, forward.z } };

			const float t[ 3 ]{
				-( view[ 0 ][ 0 ] * eye.x + view[ 0 ][ 1 ] * eye.y + view[ 0 ][ 2 ] * eye.z ),
				-( view[ 1 ][ 0 ] * eye.x + view[ 1 ][ 1 ] * eye.y + view[ 1 ][ 2 ] * eye.z ),
				-( view[ 2 ][ 0 ] * eye.x + view[ 2 ][ 1 ] * eye.y + view[ 2 ][ 2 ] * eye.z ) };

			for ( int c = 0; c < 3; ++c )
			{
				out[ 0 ][ c ] = view[ 0 ][ c ] * w;
				out[ 1 ][ c ] = view[ 1 ][ c ] * h;
				out[ 2 ][ c ] = view[ 2 ][ c ] * q;
				out[ 3 ][ c ] = view[ 2 ][ c ];
			}

			out[ 0 ][ 3 ] = t[ 0 ] * w;
			out[ 1 ][ 3 ] = t[ 1 ] * h;
			out[ 2 ][ 3 ] = ( t[ 2 ] - near_z ) * q;
			out[ 3 ][ 3 ] = t[ 2 ];
		}

	} // namespace

	bool preview::initialize( ID3D11Device* device, ID3D11DeviceContext* context )
	{
		if ( !device || !context )
		{
			return false;
		}
		// Two owners call this on the same device: the HUD port while the game's
		// device comes up and the menu bridge for the ESP model preview. Re-running
		// the setup would allocate a second set of shaders and states without
		// releasing the first, so the same device is simply adopted.
		if ( this->m_ready && this->m_device == device && this->m_context == context )
		{
			return true;
		}
		if ( this->m_ready )
		{
			this->shutdown( );
		}

		this->m_device = device;
		this->m_context = context;

		if ( !this->create_shaders( ) || !this->create_states( ) )
		{
			return false;
		}

		this->m_ready = true;
		return true;
	}

	bool preview::create_shaders( )
	{
		ID3DBlob* vs_blob{};
		ID3DBlob* ps_blob{};
		ID3DBlob* error_blob{};

		auto hr = D3DCompile( k_preview_shader, std::strlen( k_preview_shader ), nullptr, nullptr, nullptr,
			"VS_Main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs_blob, &error_blob );
		if ( FAILED( hr ) )
		{
			if ( error_blob )
			{
				app::context().diagnostics.warning( "[preview] VS compile error: {}", static_cast< const char* >( error_blob->GetBufferPointer( ) ) );
				error_blob->Release( );
			}
			return false;
		}

		hr = D3DCompile( k_preview_shader, std::strlen( k_preview_shader ), nullptr, nullptr, nullptr,
			"PS_Main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps_blob, &error_blob );
		if ( FAILED( hr ) )
		{
			if ( error_blob )
			{
				app::context().diagnostics.warning( "[preview] PS compile error: {}", static_cast< const char* >( error_blob->GetBufferPointer( ) ) );
				error_blob->Release( );
			}
			vs_blob->Release( );
			return false;
		}

		this->m_device->CreateVertexShader( vs_blob->GetBufferPointer( ), vs_blob->GetBufferSize( ), nullptr, &this->m_vertex_shader );
		this->m_device->CreatePixelShader( ps_blob->GetBufferPointer( ), ps_blob->GetBufferSize( ), nullptr, &this->m_pixel_shader );

		const D3D11_INPUT_ELEMENT_DESC layout[]
		{
			{ "POSITION",     0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "NORMAL",       0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TANGENT",      0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD",     0, DXGI_FORMAT_R32G32_FLOAT,       0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT,      0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "BLENDWEIGHT",  0, DXGI_FORMAT_R8G8B8A8_UNORM,     0, 52, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD",     1, DXGI_FORMAT_R32G32_FLOAT,       0, 56, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};

		hr = this->m_device->CreateInputLayout( layout, 7, vs_blob->GetBufferPointer( ), vs_blob->GetBufferSize( ), &this->m_input_layout );

		vs_blob->Release( );
		ps_blob->Release( );

		D3D11_BUFFER_DESC cb{};
		cb.ByteWidth = sizeof( cb_scene );
		cb.Usage = D3D11_USAGE_DYNAMIC;
		cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if ( FAILED( this->m_device->CreateBuffer( &cb, nullptr, &this->m_cb_scene ) ) )
		{
			return false;
		}

		cb.ByteWidth = sizeof( float ) * 16 * 128; // matches g_Bones[128]
		if ( FAILED( this->m_device->CreateBuffer( &cb, nullptr, &this->m_cb_bones ) ) )
		{
			return false;
		}

		return SUCCEEDED( hr ) && this->m_vertex_shader && this->m_pixel_shader;
	}

	bool preview::create_states( )
	{
		D3D11_SAMPLER_DESC sd{};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		sd.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		if ( FAILED( this->m_device->CreateSamplerState( &sd, &this->m_sampler ) ) ) return false;
		sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		if ( FAILED( this->m_device->CreateSamplerState( &sd, &this->m_clamp_sampler ) ) ) return false;

		D3D11_RASTERIZER_DESC rs{};
		rs.FillMode = D3D11_FILL_SOLID;
		// Winding order was never established for these meshes, and the shader
		// flips normals toward the viewer, so both faces render correctly.
		rs.CullMode = D3D11_CULL_NONE;
		rs.DepthClipEnable = TRUE;
		if ( FAILED( this->m_device->CreateRasterizerState( &rs, &this->m_rasterizer ) ) ) return false;

		D3D11_DEPTH_STENCIL_DESC ds{};
		ds.DepthEnable = TRUE;
		ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		ds.DepthFunc = D3D11_COMPARISON_LESS;
		if ( FAILED( this->m_device->CreateDepthStencilState( &ds, &this->m_depth_state ) ) ) return false;

		D3D11_BLEND_DESC bs{};
		bs.RenderTarget[ 0 ].BlendEnable = FALSE;
		bs.RenderTarget[ 0 ].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		if ( FAILED( this->m_device->CreateBlendState( &bs, &this->m_blend_state ) ) ) return false;

		return true;
	}

	bool preview::ensure_target( std::uint32_t width, std::uint32_t height )
	{
		if ( this->m_rtv && width == this->m_width && height == this->m_height )
		{
			return true;
		}

		this->release_target( );

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

		if ( FAILED( this->m_device->CreateTexture2D( &desc, nullptr, &this->m_color_texture ) ) ) return false;
		if ( FAILED( this->m_device->CreateRenderTargetView( this->m_color_texture, nullptr, &this->m_rtv ) ) ) return false;
		if ( FAILED( this->m_device->CreateShaderResourceView( this->m_color_texture, nullptr, &this->m_srv ) ) ) return false;

		D3D11_TEXTURE2D_DESC depth = desc;
		depth.Format = DXGI_FORMAT_D32_FLOAT;
		depth.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		if ( FAILED( this->m_device->CreateTexture2D( &depth, nullptr, &this->m_depth_texture ) ) ) return false;
		if ( FAILED( this->m_device->CreateDepthStencilView( this->m_depth_texture, nullptr, &this->m_dsv ) ) ) return false;

		this->m_msaa_samples = 0;
		UINT samples = 1;
		for ( const UINT candidate : { 8u, 4u, 2u } )
		{
			UINT quality = 0;
			if ( SUCCEEDED( this->m_device->CheckMultisampleQualityLevels( DXGI_FORMAT_R8G8B8A8_UNORM, candidate, &quality ) )
				&& quality > 0 )
			{
				samples = candidate;
				break;
			}
		}
		if ( samples > 1 )
		{
			D3D11_TEXTURE2D_DESC ms = desc;
			ms.SampleDesc.Count = samples;
			ms.BindFlags = D3D11_BIND_RENDER_TARGET;
			D3D11_TEXTURE2D_DESC ms_depth = depth;
			ms_depth.SampleDesc.Count = samples;
			if ( SUCCEEDED( this->m_device->CreateTexture2D( &ms, nullptr, &this->m_msaa_color ) )
				&& SUCCEEDED( this->m_device->CreateRenderTargetView( this->m_msaa_color, nullptr, &this->m_msaa_rtv ) )
				&& SUCCEEDED( this->m_device->CreateTexture2D( &ms_depth, nullptr, &this->m_msaa_depth ) )
				&& SUCCEEDED( this->m_device->CreateDepthStencilView( this->m_msaa_depth, nullptr, &this->m_msaa_dsv ) ) )
			{
				this->m_msaa_samples = samples;
			}
		}

		this->m_width = width;
		this->m_height = height;
		return true;
	}

	void preview::release_target( )
	{
		if ( this->m_dsv ) { this->m_dsv->Release( ); this->m_dsv = nullptr; }
		if ( this->m_depth_texture ) { this->m_depth_texture->Release( ); this->m_depth_texture = nullptr; }
		if ( this->m_srv ) { this->m_srv->Release( ); this->m_srv = nullptr; }
		if ( this->m_rtv ) { this->m_rtv->Release( ); this->m_rtv = nullptr; }
		if ( this->m_color_texture ) { this->m_color_texture->Release( ); this->m_color_texture = nullptr; }
		if ( this->m_msaa_rtv ) { this->m_msaa_rtv->Release( ); this->m_msaa_rtv = nullptr; }
		if ( this->m_msaa_color ) { this->m_msaa_color->Release( ); this->m_msaa_color = nullptr; }
		if ( this->m_msaa_dsv ) { this->m_msaa_dsv->Release( ); this->m_msaa_dsv = nullptr; }
		if ( this->m_msaa_depth ) { this->m_msaa_depth->Release( ); this->m_msaa_depth = nullptr; }
		this->m_msaa_samples = 0;
		this->m_width = 0;
		this->m_height = 0;
	}

	ID3D11ShaderResourceView* preview::get_texture( vpk_archive& vpk, const std::string& path )
	{
		if ( path.empty( ) )
		{
			return nullptr;
		}

		if ( const auto it = this->m_textures.find( path ); it != this->m_textures.end( ) )
		{
			return it->second.srv;
		}

		// Cache the failure too -- a missing texture must not re-read the VPK
		// every single frame.
		gpu_texture entry{};
		const auto data = load_texture( vpk, path );

		if ( data.valid( ) )
		{
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = data.width;
			desc.Height = data.height;
			desc.MipLevels = data.mip_count;
			desc.ArraySize = 1;
			desc.Format = static_cast< DXGI_FORMAT >( data.dxgi_format );
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_IMMUTABLE;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

			std::vector<D3D11_SUBRESOURCE_DATA> subresources( data.mip_count );
			for ( std::uint32_t level = 0; level < data.mip_count; ++level )
			{
				const auto w = std::max<std::uint32_t>( 1, data.width >> level );
				const auto blocks_x = std::max<std::uint32_t>( 1, ( w + 3 ) / 4 );

				subresources[ level ].pSysMem = data.mips[ level ].data( );
				subresources[ level ].SysMemPitch = data.block_size != 0
					? blocks_x * data.block_size            // one row of 4x4 blocks
					: w * data.bytes_per_pixel;
			}

			if ( SUCCEEDED( this->m_device->CreateTexture2D( &desc, subresources.data( ), &entry.texture ) ) )
			{
				this->m_device->CreateShaderResourceView( entry.texture, nullptr, &entry.srv );
			}
		}

		return this->m_textures.emplace( path, entry ).first->second.srv;
	}

	const preview::gpu_material& preview::get_material( vpk_archive& vpk, const std::string& path )
	{
		if ( const auto it = this->m_materials.find( path ); it != this->m_materials.end( ) )
		{
			return it->second;
		}

		gpu_material entry{};
		const auto data = load_material( vpk, path );
		if ( data.valid( ) )
		{
			entry.color = this->get_texture( vpk, data.color );
			entry.normal = this->get_texture( vpk, data.normal );
			entry.metalness = this->get_texture( vpk, data.metalness );
			entry.ambient_occlusion = this->get_texture( vpk, data.ambient_occlusion );
			entry.gloss = this->get_texture( vpk, data.gloss );
		}

		return this->m_materials.emplace( path, entry ).first->second;
	}

	const preview::gpu_mesh* preview::get_mesh( const std::string& model_path, const skinned_mesh& mesh )
	{
		if ( const auto it = this->m_meshes.find( model_path ); it != this->m_meshes.end( ) )
		{
			return &it->second;
		}

		gpu_mesh gm{};

		D3D11_BUFFER_DESC vb{};
		vb.ByteWidth = static_cast< UINT >( mesh.vertices.size( ) * sizeof( skinned_vertex ) );
		vb.Usage = D3D11_USAGE_IMMUTABLE;
		vb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		D3D11_SUBRESOURCE_DATA vd{ mesh.vertices.data( ) };
		if ( FAILED( this->m_device->CreateBuffer( &vb, &vd, &gm.vertex_buffer ) ) )
		{
			return nullptr;
		}

		D3D11_BUFFER_DESC ib{};
		ib.ByteWidth = static_cast< UINT >( mesh.indices.size( ) * sizeof( std::uint32_t ) );
		ib.Usage = D3D11_USAGE_IMMUTABLE;
		ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
		D3D11_SUBRESOURCE_DATA id{ mesh.indices.data( ) };
		if ( FAILED( this->m_device->CreateBuffer( &ib, &id, &gm.index_buffer ) ) )
		{
			gm.vertex_buffer->Release( );
			return nullptr;
		}

		return &this->m_meshes.emplace( model_path, gm ).first->second;
	}

	namespace {

		[[nodiscard]] float half_to_float( const std::uint16_t h )
		{
			const auto sign = ( h & 0x8000 ) ? -1.0f : 1.0f;
			const int exponent = ( h >> 10 ) & 0x1F;
			const int mantissa = h & 0x3FF;
			if ( exponent == 0 )
				return sign * std::ldexp( static_cast< float >( mantissa ), -24 );
			if ( exponent == 31 )
				return 0.0f;
			return sign * std::ldexp( static_cast< float >( mantissa | 0x400 ), exponent - 25 );
		}

		// Guns are long along G; most knives along B, but not all of them, and the
		// material's channel flags do not tell them apart. The spread of the map's
		// values does: 2nd to 98th percentile, so the background and stray texels
		// cannot stretch an axis.
		[[nodiscard]] int position_long_axis( const texture_data& data )
		{
			if ( !data.valid( ) || data.block_size != 0
				|| ( data.bytes_per_pixel != 8 && data.bytes_per_pixel != 4 ) )
				return 1;

			const auto& mip = data.mips.front( );
			const std::uint32_t step = std::max<std::uint32_t>( 1, std::max( data.width, data.height ) / 128 );
			std::vector<float> g{}, b{};
			for ( std::uint32_t y = 0; y < data.height; y += step )
			{
				for ( std::uint32_t x = 0; x < data.width; x += step )
				{
					const std::size_t at = ( static_cast< std::size_t >( y ) * data.width + x ) * data.bytes_per_pixel;
					if ( at + data.bytes_per_pixel > mip.size( ) )
						continue;
					if ( data.bytes_per_pixel == 8 )
					{
						std::uint16_t texel[ 4 ]{};
						std::memcpy( texel, mip.data( ) + at, sizeof( texel ) );
						g.push_back( half_to_float( texel[ 1 ] ) );
						b.push_back( half_to_float( texel[ 2 ] ) );
					}
					else
					{
						g.push_back( mip[ at + 1 ] / 255.0f );
						b.push_back( mip[ at + 2 ] / 255.0f );
					}
				}
			}
			if ( g.size( ) < 16 )
				return 1;

			const auto spread = [ ]( std::vector<float>& values )
			{
				const auto lo = values.size( ) * 2 / 100;
				const auto hi = values.size( ) * 98 / 100;
				std::nth_element( values.begin( ), values.begin( ) + lo, values.end( ) );
				const auto low = values[ lo ];
				std::nth_element( values.begin( ), values.begin( ) + hi, values.end( ) );
				return values[ hi ] - low;
			};
			return spread( b ) > spread( g ) ? 2 : 1;
		}

	} // namespace

	void preview::index_composites( vpk_archive& vpk )
	{
		if ( this->m_composites_indexed )
			return;
		this->m_composites_indexed = true;

		// Legacy inputs sit with the paint kits, HD and knife inputs beside the models.
		constexpr std::string_view k_suffix{ "_composite_inputs.vmat_c" };
		for ( const char* prefix : { "materials/models/weapons/customization/", "weapons/models/" } )
		{
			for ( const auto& path : vpk.list_prefix( prefix ) )
			{
				if ( !path.ends_with( k_suffix ) )
					continue;
				const auto inputs = load_material( vpk, path );
				if ( !inputs.color.empty( ) )
					this->m_composite_by_color.try_emplace( inputs.color, path );
				const auto slash = path.find_last_of( '/' );
				const auto stem = path.substr( slash + 1, path.size( ) - slash - 1 - k_suffix.size( ) );
				this->m_composite_by_name.try_emplace( stem, path );
			}
		}
	}

	const preview::gpu_composite* preview::get_composite( vpk_archive& vpk, const std::string& material_path, const bool hd )
	{
		const auto key = hd ? material_path + "|hd" : material_path;
		if ( const auto it = this->m_material_composite.find( key ); it != this->m_material_composite.end( ) )
			return it->second;

		this->index_composites( vpk );

		// HD bodies share their file name with their inputs (weapon_snip_awp.vmat and
		// weapon_snip_awp_composite_inputs.vmat) but not always the albedo; legacy
		// bodies are named after the old model and only the albedo ties them.
		const std::string* inputs_path = nullptr;
		if ( hd )
		{
			const auto slash = material_path.find_last_of( '/' );
			auto stem = material_path.substr( slash == std::string::npos ? 0 : slash + 1 );
			if ( const auto dot = stem.find( '.' ); dot != std::string::npos )
				stem.resize( dot );
			if ( const auto match = this->m_composite_by_name.find( stem ); match != this->m_composite_by_name.end( ) )
				inputs_path = &match->second;
		}
		if ( !inputs_path )
		{
			const auto body = load_material( vpk, material_path );
			if ( const auto match = this->m_composite_by_color.find( body.color );
				!body.color.empty( ) && match != this->m_composite_by_color.end( ) )
				inputs_path = &match->second;
		}

		const gpu_composite* found = nullptr;
		if ( inputs_path )
		{
			auto [ slot, inserted ] = this->m_composites.try_emplace( *inputs_path );
			if ( inserted )
			{
				const auto inputs = load_material( vpk, *inputs_path );
				auto& composite = slot->second;
				composite.masks = this->get_texture( vpk, inputs.masks );
				composite.position = this->get_texture( vpk, inputs.position );
				composite.uv_scale = inputs.uv_scale1 > 0.0f ? inputs.uv_scale1 : 1.0f;
				composite.weapon_length = inputs.weapon_length1 > 0.0f ? inputs.weapon_length1 : 32.0f;
				composite.long_axis = inputs.position.empty( ) ? 1
					: position_long_axis( load_texture( vpk, inputs.position ) );
			}
			found = &slot->second;
		}

		this->m_material_composite.emplace( key, found );
		return found;
	}

	void preview::update_camera( const skinned_mesh& mesh, std::uint32_t width, std::uint32_t height )
	{
		// Frame the model from its own bounds rather than a hardcoded distance, so
		// any agent fills the viewport the same way.
		foundation::vec3 mins{ 1e9f, 1e9f, 1e9f };
		foundation::vec3 maxs{ -1e9f, -1e9f, -1e9f };
		for ( const auto& v : mesh.vertices )
		{
			mins.x = std::min( mins.x, v.position[ 0 ] ); maxs.x = std::max( maxs.x, v.position[ 0 ] );
			mins.y = std::min( mins.y, v.position[ 1 ] ); maxs.y = std::max( maxs.y, v.position[ 1 ] );
			mins.z = std::min( mins.z, v.position[ 2 ] ); maxs.z = std::max( maxs.z, v.position[ 2 ] );
		}

		const foundation::vec3 center{ ( mins.x + maxs.x ) * 0.5f, ( mins.y + maxs.y ) * 0.5f, ( mins.z + maxs.z ) * 0.5f };

		constexpr auto k_box_top = 0.035f;    // matches unit_min/unit_max in the editor
		constexpr auto k_box_bottom = 0.828f;
		constexpr auto k_box_fill = k_box_bottom - k_box_top;
		constexpr auto k_box_center = ( k_box_top + k_box_bottom ) * 0.5f;

		const auto half_height = std::max( ( maxs.z - mins.z ) * 0.5f, 1.0f );

		constexpr auto vertical_fov = 0.7f;
		const auto aspect = static_cast< float >( width ) / static_cast< float >( std::max<std::uint32_t>( height, 1 ) );
		const auto tan_half = std::tan( vertical_fov * 0.5f );

		// At distance d the visible half-height is d * tan(fov/2); solving for the
		// model covering k_box_fill of the viewport gives the distance directly.
		const auto distance = half_height / ( k_box_fill * tan_half );

		// The box is not vertically centred in the viewport, so the camera looks
		// slightly below the model's centre to line the two up.
		const auto ndc_center = 1.0f - 2.0f * k_box_center;
		const foundation::vec3 target{ center.x, center.y, center.z - ndc_center * distance * tan_half };

		// Camera stays level with that target: the figure is upright and only
		// spins about the vertical axis, so there is no pitch to apply.
		this->m_eye = foundation::vec3{
			target.x + distance * std::cos( this->m_yaw ),
			target.y + distance * std::sin( this->m_yaw ),
			target.z };

		build_view_projection( this->m_eye, target, vertical_fov, aspect,
			std::max( distance * 0.02f, 1.0f ), distance * 4.0f + 512.0f, this->m_view_projection );
	}

	void preview::update_object_camera( const skinned_mesh& mesh, std::uint32_t width, std::uint32_t height,
		const std::vector<bone_matrix>* skin, const bool hands_only )
	{
		// Frame the model's whole bounding box, whichever way it happens to be long: a
		// rifle is as wide as a character is tall, and the upright framing would push the
		// camera into it.
		foundation::vec3 mins{ 1e9f, 1e9f, 1e9f };
		foundation::vec3 maxs{ -1e9f, -1e9f, -1e9f };
		auto consider = [ & ]( const foundation::vec3& p ) {
			mins.x = std::min( mins.x, p.x ); maxs.x = std::max( maxs.x, p.x );
			mins.y = std::min( mins.y, p.y ); maxs.y = std::max( maxs.y, p.y );
			mins.z = std::min( mins.z, p.z ); maxs.z = std::max( maxs.z, p.z );
		};
		auto accumulate = [ & ]( const bool hands ) {
			mins = { 1e9f, 1e9f, 1e9f };
			maxs = { -1e9f, -1e9f, -1e9f };
			auto count = 0;
			for ( const auto& vertex : mesh.vertices )
			{
				if ( hands )
				{
					const auto bone = dominant_bone( vertex );
					if ( bone < 0 || static_cast< std::size_t >( bone ) >= mesh.bones.size( )
						|| !hand_bone( mesh.bones[ static_cast< std::size_t >( bone ) ].name ) )
						continue;
				}
				consider( skin ? skinned_position( vertex, *skin ) : foundation::vec3{ vertex.position[ 0 ], vertex.position[ 1 ], vertex.position[ 2 ] } );
				++count;
			}
			return count;
		};
		auto framed_hands = false;
		if ( hands_only )
		{
			framed_hands = accumulate( true ) >= 40;
			if ( !framed_hands )
				accumulate( false );
		}
		else
		{
			accumulate( false );
		}

		const foundation::vec3 center{ ( mins.x + maxs.x ) * 0.5f, ( mins.y + maxs.y ) * 0.5f,
			( mins.z + maxs.z ) * 0.5f };

		const auto half_x = std::abs( maxs.x - mins.x ) * 0.5f;
		const auto half_y = std::abs( maxs.y - mins.y ) * 0.5f;
		const auto half_z = std::abs( maxs.z - mins.z ) * 0.5f;

		const auto cp = std::cos( this->m_pitch );
		const foundation::vec3 direction{ cp * std::cos( this->m_yaw ), cp * std::sin( this->m_yaw ),
			std::sin( this->m_pitch ) };

		// The camera basis, taken the same way build_view_projection derives it below, so
		// the fit and the matrix agree.
		const foundation::vec3 forward{ -direction.x, -direction.y, -direction.z };
		foundation::vec3 right = forward.cross( foundation::vec3{ 0.0f, 0.0f, 1.0f } );
		const auto right_length = right.length( );
		right = right_length > 1e-6f ? right / right_length : foundation::vec3{ 0.0f, 1.0f, 0.0f };
		const auto up = right.cross( forward );

		// A longer lens than the character preview uses. Perspective is what makes a
		// turntable fit expensive: the near end of a rifle projects far wider than its
		// centre, so a wide angle forces the camera back until the model is a speck.
		constexpr auto vertical_fov = 0.38f;
		const auto aspect = static_cast< float >( width ) / static_cast< float >( std::max<std::uint32_t>( height, 1 ) );
		const auto tan_v = std::tan( vertical_fov * 0.5f );
		const auto tan_h = tan_v * aspect;

		// Distance at which the model lands inside the frustum. A box fit is exact only for
		// a box: a rifle is a diagonal inside its own bounds, so the eight corners enclose
		// mostly empty space and framing them leaves the model small. Fitting the bounding
		// sphere is worse still -- its radius is the barrel's length, which then sets the
		// distance for the vertical axis too. Walking the vertices costs one pass and gives
		// the tight fit a turntable wants.
		auto required = 0.0f;
		auto nearest = std::numeric_limits<float>::max( );
		for ( const auto& vertex : mesh.vertices )
		{
			if ( framed_hands )
			{
				const auto bone = dominant_bone( vertex );
				if ( bone < 0 || static_cast< std::size_t >( bone ) >= mesh.bones.size( )
					|| !hand_bone( mesh.bones[ static_cast< std::size_t >( bone ) ].name ) )
					continue;
			}
			const auto posed = skin ? skinned_position( vertex, *skin )
				: foundation::vec3{ vertex.position[ 0 ], vertex.position[ 1 ], vertex.position[ 2 ] };
			const foundation::vec3 offset{ posed.x - center.x, posed.y - center.y, posed.z - center.z };

			// How much nearer this point sits than the centre, and how far off axis.
			const auto along = offset.x * direction.x + offset.y * direction.y + offset.z * direction.z;
			const auto lateral = std::abs( offset.x * right.x + offset.y * right.y + offset.z * right.z );
			const auto upright = std::abs( offset.x * up.x + offset.y * up.y + offset.z * up.z );

			required = std::max( required, along + lateral / tan_h );
			required = std::max( required, along + upright / tan_v );
			nearest = std::min( nearest, along );
		}

		constexpr auto margin = 1.06f;
		const auto distance = std::max( required, 0.05f ) * margin
			* std::clamp( this->m_zoom, 0.25f, 8.0f );

		this->m_eye = center + direction * distance;

		const auto radius = std::sqrt( half_x * half_x + half_y * half_y + half_z * half_z );
		build_view_projection( this->m_eye, center, vertical_fov, aspect,
			std::max( distance * 0.01f, 0.05f ), distance + radius * 2.0f + 10.0f, this->m_view_projection );
	}

	void preview::set_finish( finish_data finish )
	{
		this->m_finish = std::move( finish );
		this->m_finish_gpu = {};
	}

	void preview::clear_finish( )
	{
		this->m_finish = {};
		this->m_finish_gpu = {};
	}

	void preview::set_framing( const framing mode )
	{
		this->m_framing = mode;
	}

	void preview::set_view( const float yaw_degrees, const float pitch_degrees, const float zoom )
	{
		this->m_yaw = yaw_degrees * 0.017453292f;
		this->m_pitch = std::clamp( pitch_degrees * 0.017453292f, -1.45f, 1.45f );
		this->m_zoom = std::clamp( zoom, 0.25f, 8.0f );
	}

	void preview::ensure_idle_clip( vpk_archive& vpk, const std::string& model_path, const skinned_mesh& mesh )
	{
		if ( !this->m_idle_loaded )
		{
			this->m_idle_loaded = true;

			this->m_idle_skeleton = load_nm_skeleton( vpk, "animation/skeletons/characters/worldmodel.vnmskel" );
			this->m_idle_clip = load_nm_clip( vpk,
				"animation/anims/ui_anims/main_menu/ct/ct_main_menu_famas_idle.vnmclip" );
		}

		// The mapping is by bone name, so it only has to be rebuilt when the
		// previewed model changes.
		if ( this->m_idle_mapped_model != model_path )
		{
			this->m_idle_mapped_model = model_path;
			this->m_idle_track_to_bone = this->m_idle_skeleton.valid( )
				? map_tracks_to_model( this->m_idle_skeleton, mesh )
				: std::vector<int>{};
		}
	}

	void preview::orbit( float delta_yaw )
	{
		// The glove inspect plays its own clip. Dragging it fights the pose.
		if ( this->m_glove_view )
			return;
		constexpr auto sensitivity = 0.01f;
		this->m_yaw -= delta_yaw * sensitivity;
	}

	void preview::orbit_pitch( float delta_pitch )
	{
		if ( this->m_glove_view )
			return;
		constexpr auto sensitivity = 0.01f;
		// Stop just short of the poles: at exactly +-90 degrees the look-at up vector
		// becomes parallel to the view direction and the matrix collapses.
		constexpr auto limit = 1.45f;
		this->m_pitch = std::clamp( this->m_pitch + delta_pitch * sensitivity, -limit, limit );
	}

	void preview::zoom( float factor )
	{
		if ( factor > 0.0f )
		{
			this->m_zoom = std::clamp( this->m_zoom / factor, 0.25f, 8.0f );
		}
	}

	bool preview::bone_position( std::uint32_t bone, foundation::vec3& out ) const
	{
		if ( !this->m_mesh || bone >= this->m_mesh->bones.size( ) )
		{
			return false;
		}

		// Prefer the animated transform so overlays track the pose; inverting
		// inverse_bind recovers the bind pose when nothing is playing.
		const auto m = bone < this->m_bone_world.size( )
			? this->m_bone_world[ bone ]
			: this->m_mesh->bones[ bone ].inverse_bind.inverse_rigid( );
		out = foundation::vec3{ m.m[ 0 ][ 3 ], m.m[ 1 ][ 3 ], m.m[ 2 ][ 3 ] };
		return true;
	}

	bool preview::bone_transform( std::uint32_t bone, const foundation::vec3& local, foundation::vec3& out ) const
	{
		if ( !this->m_mesh || bone >= this->m_mesh->bones.size( ) )
		{
			return false;
		}

		const auto m = bone < this->m_bone_world.size( )
			? this->m_bone_world[ bone ]
			: this->m_mesh->bones[ bone ].inverse_bind.inverse_rigid( );
		out = foundation::vec3{
			m.m[ 0 ][ 0 ] * local.x + m.m[ 0 ][ 1 ] * local.y + m.m[ 0 ][ 2 ] * local.z + m.m[ 0 ][ 3 ],
			m.m[ 1 ][ 0 ] * local.x + m.m[ 1 ][ 1 ] * local.y + m.m[ 1 ][ 2 ] * local.z + m.m[ 1 ][ 3 ],
			m.m[ 2 ][ 0 ] * local.x + m.m[ 2 ][ 1 ] * local.y + m.m[ 2 ][ 2 ] * local.z + m.m[ 2 ][ 3 ] };
		return true;
	}

	bool preview::bone_direction( std::uint32_t bone, const foundation::vec3& model_space, foundation::vec3& out ) const
	{
		if ( !this->m_mesh || bone >= this->m_mesh->bones.size( ) )
		{
			return false;
		}

		const auto& to_bone = this->m_mesh->bones[ bone ].inverse_bind;
		const auto from_bone = bone < this->m_bone_world.size( )
			? this->m_bone_world[ bone ]
			: to_bone.inverse_rigid( );

		// Rotation only -- translation columns are deliberately not applied.
		const float local[ 3 ]{
			to_bone.m[ 0 ][ 0 ] * model_space.x + to_bone.m[ 0 ][ 1 ] * model_space.y + to_bone.m[ 0 ][ 2 ] * model_space.z,
			to_bone.m[ 1 ][ 0 ] * model_space.x + to_bone.m[ 1 ][ 1 ] * model_space.y + to_bone.m[ 1 ][ 2 ] * model_space.z,
			to_bone.m[ 2 ][ 0 ] * model_space.x + to_bone.m[ 2 ][ 1 ] * model_space.y + to_bone.m[ 2 ][ 2 ] * model_space.z };

		out = foundation::vec3{
			from_bone.m[ 0 ][ 0 ] * local[ 0 ] + from_bone.m[ 0 ][ 1 ] * local[ 1 ] + from_bone.m[ 0 ][ 2 ] * local[ 2 ],
			from_bone.m[ 1 ][ 0 ] * local[ 0 ] + from_bone.m[ 1 ][ 1 ] * local[ 1 ] + from_bone.m[ 1 ][ 2 ] * local[ 2 ],
			from_bone.m[ 2 ][ 0 ] * local[ 0 ] + from_bone.m[ 2 ][ 1 ] * local[ 1 ] + from_bone.m[ 2 ][ 2 ] * local[ 2 ] };
		return true;
	}

	bool preview::project( const foundation::vec3& world, float& x, float& y ) const
	{
		const auto& m = this->m_view_projection;
		const auto w = m[ 3 ][ 0 ] * world.x + m[ 3 ][ 1 ] * world.y + m[ 3 ][ 2 ] * world.z + m[ 3 ][ 3 ];
		if ( w < 0.001f )
		{
			return false;
		}

		const auto cx = m[ 0 ][ 0 ] * world.x + m[ 0 ][ 1 ] * world.y + m[ 0 ][ 2 ] * world.z + m[ 0 ][ 3 ];
		const auto cy = m[ 1 ][ 0 ] * world.x + m[ 1 ][ 1 ] * world.y + m[ 1 ][ 2 ] * world.z + m[ 1 ][ 3 ];

		x = ( cx / w * 0.5f + 0.5f ) * static_cast< float >( this->m_width );
		y = ( 0.5f - cy / w * 0.5f ) * static_cast< float >( this->m_height );
		return true;
	}

	ID3D11ShaderResourceView* preview::render( vpk_archive& vpk, const std::string& model_path,
		std::uint32_t width, std::uint32_t height,
		const config::visual_profile::chams::material* chams_material )
	{
		this->m_mesh = nullptr;

		if ( !this->m_ready || model_path.empty( ) || width == 0 || height == 0 )
		{
			return nullptr;
		}

		if ( !this->ensure_target( width, height ) )
		{
			return nullptr;
		}

		// The game draws the HD model unless the paint kit was drawn for the legacy
		// unwrap; the two meshes have different UVs, so the wrong one scrambles the paint.
		const bool weapon = model_path.find( "weapons/models/" ) != std::string::npos;
		const bool has_finish = this->m_finish.valid( );
		const bool hd = weapon && !( has_finish && this->m_finish.legacy_model );
		const auto mesh_key = hd ? model_path + "|hd" : model_path;

		const auto& source = g_mesh_cache.get_or_build( vpk, mesh_key );
		if ( !source.valid )
		{
			return nullptr;
		}

		// Gloves stay in the bind pose and are skinned by the inspect clip each frame,
		// the same loop the game plays when you look at your gloves.
		const auto gloves = glove_model( model_path );
		this->m_glove_view = gloves;
		const auto& mesh = source;
		if ( !mesh.valid )
		{
			return nullptr;
		}

		const auto* gpu = this->get_mesh( mesh_key, mesh );
		if ( !gpu )
		{
			return nullptr;
		}

		this->m_mesh = &mesh;

		const auto time = static_cast< float >(
			std::chrono::duration_cast< std::chrono::milliseconds >(
				std::chrono::steady_clock::now( ).time_since_epoch( ) ).count( ) % 3600000 ) * 0.001f;
		if ( gloves )
		{
			if ( !this->m_glove_clip_loaded )
			{
				this->m_glove_clip_loaded = true;
				this->m_glove_skeleton = load_nm_skeleton( vpk, "animation/skeletons/characters/viewmodel.vnmskel" );
				this->m_glove_clip = load_nm_clip( vpk, "animation/anims/viewmodel/arms/inspects/pedestal_gloves_loop.vnmclip" );
			}
			if ( this->m_glove_mapped_model != model_path )
			{
				this->m_glove_mapped_model = model_path;
				this->m_glove_track_to_bone = this->m_glove_skeleton.valid( )
					? map_tracks_to_model( this->m_glove_skeleton, mesh )
					: std::vector<int>{};
			}
			if ( this->m_glove_clip.valid( ) && !this->m_glove_track_to_bone.empty( ) )
			{
				const auto duration = this->m_glove_clip.duration > 0.05f ? this->m_glove_clip.duration : 1.0f;
				const auto t = std::fmod( time, duration );
				// Same direction the object camera is placed along, so the backs of the
				// gloves — not the palms — are what that camera is looking at.
				const auto cp = std::cos( this->m_pitch );
				const foundation::vec3 glove_view{
					cp * std::cos( this->m_yaw ), cp * std::sin( this->m_yaw ), std::sin( this->m_pitch ) };
				sample_pose( this->m_glove_clip, this->m_glove_skeleton, mesh,
					this->m_glove_track_to_bone, t, this->m_skin_matrices, this->m_bone_world, &glove_view );
			}
			else
			{
				this->m_skin_matrices.assign( mesh.bones.size( ), bone_matrix::identity( ) );
				this->m_bone_world.clear( );
			}
		}
		// The character idle is for the ESP figure. A weapon has no matching bones.
		else
		{
		this->ensure_idle_clip( vpk, model_path, mesh );
		if ( this->m_idle_clip.valid( ) && !this->m_idle_track_to_bone.empty( ) )
		{
			sample_pose( this->m_idle_clip, this->m_idle_skeleton, mesh,
				this->m_idle_track_to_bone, time, this->m_skin_matrices, this->m_bone_world );
		}
		else
		{
			// No clip: fall back to the bind pose rather than inventing motion.
			this->m_skin_matrices.assign( mesh.bones.size( ), bone_matrix::identity( ) );
			this->m_bone_world.clear( );
		}
		}

		if ( this->m_framing == framing::object )
		{
			this->update_object_camera( mesh, width, height,
				gloves ? &this->m_skin_matrices : nullptr, gloves );
		}
		else
		{
			this->update_camera( mesh, width, height );
		}

		D3D11_MAPPED_SUBRESOURCE bones_mapped{};
		if ( SUCCEEDED( this->m_context->Map( this->m_cb_bones, 0, D3D11_MAP_WRITE_DISCARD, 0, &bones_mapped ) ) )
		{
			auto* rows = static_cast< float( * )[ 4 ][ 4 ] >( bones_mapped.pData );
			for ( std::size_t i = 0; i < 128; ++i )
		{
				const auto& m = i < this->m_skin_matrices.size( )
					? this->m_skin_matrices[ i ]
					: bone_matrix::identity( );
				for ( int r = 0; r < 3; ++r )
				{
					for ( int c = 0; c < 4; ++c ) rows[ i ][ r ][ c ] = m.m[ r ][ c ];
				}
				rows[ i ][ 3 ][ 0 ] = 0.0f; rows[ i ][ 3 ][ 1 ] = 0.0f;
				rows[ i ][ 3 ][ 2 ] = 0.0f; rows[ i ][ 3 ][ 3 ] = 1.0f;
			}
			this->m_context->Unmap( this->m_cb_bones, 0 );
		}

		// This runs while ImGui is still recording, so the overlay's own render
		// target and viewport must be back in place before returning.
		ID3D11RenderTargetView* saved_rtv{};
		ID3D11DepthStencilView* saved_dsv{};
		this->m_context->OMGetRenderTargets( 1, &saved_rtv, &saved_dsv );
		UINT saved_viewport_count{ 1 };
		D3D11_VIEWPORT saved_viewport{};
		this->m_context->RSGetViewports( &saved_viewport_count, &saved_viewport );

		// When antialiasing is on, render into the multisampled targets and resolve
		// into m_color_texture at the end; otherwise draw straight into it.
		const bool use_msaa = config::visual_settings.m_chams.antialiasing && this->m_msaa_samples > 1;
		ID3D11RenderTargetView* const active_rtv = use_msaa ? this->m_msaa_rtv : this->m_rtv;
		ID3D11DepthStencilView* const active_dsv = use_msaa ? this->m_msaa_dsv : this->m_dsv;

		// Fully transparent: the editor composites this over its own panel, so an
		// opaque clear showed up as a second container box inside the editor.
		constexpr float clear[ 4 ]{ 0.0f, 0.0f, 0.0f, 0.0f };
		this->m_context->ClearRenderTargetView( active_rtv, clear );
		this->m_context->ClearDepthStencilView( active_dsv, D3D11_CLEAR_DEPTH, 1.0f, 0 );
		this->m_context->OMSetRenderTargets( 1, &active_rtv, active_dsv );

		const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast< float >( width ), static_cast< float >( height ), 0.0f, 1.0f };
		this->m_context->RSSetViewports( 1, &viewport );

		this->m_context->IASetInputLayout( this->m_input_layout );
		this->m_context->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
		this->m_context->VSSetShader( this->m_vertex_shader, nullptr, 0 );
		this->m_context->PSSetShader( this->m_pixel_shader, nullptr, 0 );
		ID3D11Buffer* vs_cbs[]{ this->m_cb_scene, this->m_cb_bones };
		this->m_context->VSSetConstantBuffers( 0, 2, vs_cbs );
		this->m_context->PSSetConstantBuffers( 0, 1, &this->m_cb_scene );
		this->m_context->PSSetSamplers( 0, 1, &this->m_sampler );
		this->m_context->RSSetState( this->m_rasterizer );
		this->m_context->OMSetDepthStencilState( this->m_depth_state, 0 );

		const float blend_factor[ 4 ]{};
		this->m_context->OMSetBlendState( this->m_blend_state, blend_factor, 0xFFFFFFFF );

		const UINT stride = sizeof( skinned_vertex );
		const UINT offset = 0;
		this->m_context->IASetVertexBuffers( 0, 1, &gpu->vertex_buffer, &stride, &offset );
		this->m_context->IASetIndexBuffer( gpu->index_buffer, DXGI_FORMAT_R32_UINT, 0 );

		this->m_context->PSSetSamplers( 1, 1, &this->m_clamp_sampler );

		// Finish textures resolve once rather than per draw call: every range of a
		// weapon shares the one finish the inventory selected.
		if ( has_finish && !this->m_finish_gpu.resolved )
		{
			this->m_finish_gpu.color = this->get_texture( vpk, this->m_finish.color );
			this->m_finish_gpu.mask = this->get_texture( vpk, this->m_finish.mask );
			this->m_finish_gpu.normal = this->get_texture( vpk, this->m_finish.normal );
			this->m_finish_gpu.wear = this->m_finish.wear.empty( ) ? nullptr : this->get_texture( vpk, this->m_finish.wear );
			this->m_finish_gpu.grunge = this->m_finish.grunge.empty( ) ? nullptr : this->get_texture( vpk, this->m_finish.grunge );
			this->m_finish_gpu.resolved = true;
		}
		if ( !has_finish )
		{
			this->m_finish_gpu = {};
		}
		const bool finish_ready = has_finish && ( this->m_finish_gpu.color || this->m_finish.solid || this->m_finish.glove );

		// Paint goes only on the body materials that have composite inputs: scopes,
		// lenses and cartridges have none and keep their own look. A model none of
		// whose materials match (a few knives) is painted on its largest range.
		std::vector<const gpu_composite*> composites( mesh.draw_calls.size( ), nullptr );
		std::size_t paint_fallback = mesh.draw_calls.size( );
		if ( finish_ready && weapon )
		{
			bool any = false;
			std::uint32_t largest = 0;
			for ( std::size_t i = 0; i < mesh.draw_calls.size( ); ++i )
			{
				composites[ i ] = this->get_composite( vpk, mesh.draw_calls[ i ].material, hd );
				any = any || composites[ i ];
				if ( mesh.draw_calls[ i ].index_count > largest )
				{
					largest = mesh.draw_calls[ i ].index_count;
					paint_fallback = i;
				}
			}
			if ( any )
				paint_fallback = mesh.draw_calls.size( );
		}

		// Anodized paints read the pattern coordinate. Knives and a few other meshes
		// never shipped that second UV, and sampling it paints the whole thing one texel.
		auto pattern_uv_present = false;
		// The first vertices are often a scope or a mag whose second UV is empty.
		// Lightning Strike lives on that second UV; deciding from the first handful
		// dropped it onto the albedo unwrap and the bolts sat in the wrong place.
		const auto uv_samples = std::min<std::size_t>( mesh.vertices.size( ), 4096 );
		for ( std::size_t i = 0; i < uv_samples; ++i )
		{
			const auto& uv = mesh.vertices[ i ].pattern_uv;
			if ( uv[ 0 ] * uv[ 0 ] + uv[ 1 ] * uv[ 1 ] > 1.0e-6f )
			{
				pattern_uv_present = true;
				break;
			}
		}

		// Model-space bounds, for projecting spray and Fade onto a part that has no
		// position map of its own.
		foundation::vec3 pmins{ 1e9f, 1e9f, 1e9f };
		foundation::vec3 pmaxs{ -1e9f, -1e9f, -1e9f };
		for ( const auto& v : mesh.vertices )
		{
			pmins.x = std::min( pmins.x, v.position[ 0 ] ); pmaxs.x = std::max( pmaxs.x, v.position[ 0 ] );
			pmins.y = std::min( pmins.y, v.position[ 1 ] ); pmaxs.y = std::max( pmaxs.y, v.position[ 1 ] );
			pmins.z = std::min( pmins.z, v.position[ 2 ] ); pmaxs.z = std::max( pmaxs.z, v.position[ 2 ] );
		}
		const float extent[ 3 ]{ pmaxs.x - pmins.x, pmaxs.y - pmins.y, pmaxs.z - pmins.z };
		const int model_long = extent[ 0 ] >= extent[ 1 ] && extent[ 0 ] >= extent[ 2 ] ? 0 : ( extent[ 1 ] >= extent[ 2 ] ? 1 : 2 );
		const float model_length = std::max( extent[ model_long ], 1e-3f );

		// Hydrographic (1) and anodized multicolour (4) run along the pattern UV, spray
		// (2) and airbrushed (5) are projected through the position map; custom, patina
		// and gunsmith (6-8) are pictures of the unwrap.
		const int st = this->m_finish.paint_style;
		const int coord_mode = this->m_finish.loose ? 0
			: ( st == 1 || st == 4 ) ? ( pattern_uv_present ? 1 : 0 )
			: ( st == 2 || st == 5 ) ? 2
			: 0;

		// One draw per material range: each carries its own texture set.
		for ( std::size_t dc_i = 0; dc_i < mesh.draw_calls.size( ); ++dc_i )
		{
			const auto& dc = mesh.draw_calls[ dc_i ];
			const auto& material = this->get_material( vpk, dc.material );
			const auto* composite = composites[ dc_i ];
			// A glove finish paints the glove shells only. The bare arms share the
			// model and keep the skin material they shipped with.
			const bool glove_part = dc.material.find( "glove_" ) != std::string::npos;
			// Optics, glass and sticker slots are not part of the paint unwrap. Painting
			// them samples an empty texel and comes out as a white magazine or lens.
			const bool attachment = dc.material.find( "sticker" ) != std::string::npos
				|| dc.material.find( "scope" ) != std::string::npos
				|| dc.material.find( "lens" ) != std::string::npos
				|| dc.material.find( "reticle" ) != std::string::npos
				|| dc.material.find( "glass" ) != std::string::npos;
			// A loose finish is a picture of the unwrap, so every body range takes it,
			// not only the one material that has composite inputs. Stocks were the
			// range that got left as bare wood.
			const bool painted = this->m_finish.glove
				? finish_ready && glove_part
				: finish_ready && !attachment && ( !weapon || this->m_finish.loose || composite || dc_i == paint_fallback );
			ID3D11ShaderResourceView* const regions = ( this->m_finish.glove || this->m_finish.loose )
				? this->m_finish_gpu.mask
				: ( composite ? composite->masks : nullptr );

			// A finish brings its own geometry detail only when it shipped loose maps;
			// otherwise the model's maps describe the surface far better than the shared
			// placeholders a paint material points at.
			ID3D11ShaderResourceView* finish_normal = material.normal;
			ID3D11ShaderResourceView* finish_roughness = material.metalness;
			ID3D11ShaderResourceView* finish_ao = material.ambient_occlusion;
			if ( painted && ( this->m_finish.loose || this->m_finish.glove ) )
			{
				if ( this->m_finish.has_normal && this->m_finish_gpu.normal )
					finish_normal = this->m_finish_gpu.normal;
				if ( this->m_finish.has_roughness )
					finish_roughness = this->get_texture( vpk, this->m_finish.roughness );
				if ( this->m_finish.has_ao )
					finish_ao = this->get_texture( vpk, this->m_finish.ambient_occlusion );
			}

			D3D11_MAPPED_SUBRESOURCE mapped{};
			if ( SUCCEEDED( this->m_context->Map( this->m_cb_scene, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped ) ) )
			{
				auto* scene = static_cast< cb_scene* >( mapped.pData );
				std::memcpy( scene->view_projection, this->m_view_projection, sizeof( scene->view_projection ) );
				scene->eye[ 0 ] = this->m_eye.x;
				scene->eye[ 1 ] = this->m_eye.y;
				scene->eye[ 2 ] = this->m_eye.z;
				scene->tint[ 0 ] = scene->tint[ 1 ] = scene->tint[ 2 ] = scene->tint[ 3 ] = 1.0f;
				scene->uv_scale[ 0 ] = 1.0f;
				scene->uv_scale[ 1 ] = 1.0f;
				scene->uv_offset[ 0 ] = 0.0f;
				scene->uv_offset[ 1 ] = 0.0f;
				for ( int i = 0; i < 4; ++i )
				{
					for ( int c = 0; c < 4; ++c )
					{
						scene->palette[ i ][ c ] = c < 3 ? this->m_finish.palette[ i ][ c ] : 1.0f;
					}
				}
				scene->finish_uv_offset[ 0 ] = this->m_finish.pattern_uv_offset[ 0 ];
				scene->finish_uv_offset[ 1 ] = this->m_finish.pattern_uv_offset[ 1 ];
				scene->has_normal = finish_normal != nullptr;
				scene->has_metalness = finish_roughness != nullptr;
				scene->has_ao = finish_ao != nullptr;
				scene->has_gloss = material.gloss != nullptr;
				scene->has_finish = painted ? 1 : 0;
				scene->paint_style = st;
				// A loose finish's picture already is the colour, including the parts a
				// material mask would call "unpainted". Gating it by that mask dropped
				// whole skins whose mask is empty in R (AUTOEXEC's is a single channel
				// the sampler does not line up with the islands).
				scene->mask_mode = !painted || !regions || this->m_finish.loose ? 0 : 1;
				scene->coord_mode = coord_mode;
				scene->sampling[ 0 ] = this->m_finish.pattern_sampling[ 0 ];
				scene->sampling[ 1 ] = this->m_finish.pattern_sampling[ 1 ];
				scene->has_position = composite && composite->position ? 1 : 0;
				scene->long_axis = composite ? composite->long_axis : 1;
				// Unless the paint opts out, patterns are sized to the weapon: the pattern
				// UV by the weapon's UV scale, the side projection by its length against
				// the 32-unit reference the paint kits are drawn for.
				const float length = composite ? composite->weapon_length : model_length;
				scene->coord_scale = this->m_finish.ignore_size ? 1.0f
					: coord_mode == 1 ? ( composite ? composite->uv_scale : 1.0f )
					: coord_mode == 2 ? length / 32.0f
					: 1.0f;
				scene->brightness = this->m_finish.color_brightness;
				scene->paint_roughness = this->m_finish.paint_roughness;
				scene->has_pattern = this->m_finish_gpu.color != nullptr ? 1 : 0;
				scene->palette_mode = this->m_finish.palette_mode ? 1 : 0;
				scene->model_long = model_long;
				scene->model_up = model_long == 2 ? 0 : 2;
				scene->pad1 = 0;
				scene->pattern_scale = this->m_finish.pattern_scale > 0.0f
					? this->m_finish.pattern_scale : 1.0f;
				scene->pattern_rotation = this->m_finish.pattern_rotation;
				scene->model_center[ 0 ] = ( pmins.x + pmaxs.x ) * 0.5f;
				scene->model_center[ 1 ] = ( pmins.y + pmaxs.y ) * 0.5f;
				scene->model_center[ 2 ] = ( pmins.z + pmaxs.z ) * 0.5f;
				scene->model_center[ 3 ] = 1.0f / model_length;
				scene->glove = painted && this->m_finish.glove ? 1 : 0;
				scene->pattern_mode = this->m_finish.pattern_mode;
				scene->replace_index = this->m_finish.pattern_replace;
				scene->flip_v = this->m_finish.flip_v ? 1 : 0;
				for ( int i = 0; i < 8; ++i )
				{
					for ( int c = 0; c < 3; ++c )
						scene->glove_tint[ i ][ c ] = this->m_finish.glove_tint[ i ][ c ];
					scene->glove_tint[ i ][ 3 ] = 1.0f;
				}
				if ( this->m_finish.pattern_mode == 3 )
				{
					scene->pattern_palette[ 0 ] = this->m_finish.pattern_scale > 0.0f
						? this->m_finish.pattern_scale : 1.0f;
					scene->pattern_palette[ 1 ] = this->m_finish.pattern_scale_v > 0.0f
						? this->m_finish.pattern_scale_v : 1.0f;
					scene->pattern_palette[ 2 ] = static_cast< float >( this->m_finish.pattern_bits );
					scene->pattern_palette[ 3 ] = 0.0f;
				}
				else
				{
					for ( int i = 0; i < 4; ++i )
						scene->pattern_palette[ i ] = static_cast< float >( this->m_finish.pattern_palette[ i ] );
				}
				const bool wears = painted && !this->m_finish.glove && this->m_finish_gpu.wear;
				scene->wear_params[ 0 ] = wears ? std::clamp( this->m_wear, 0.0f, 1.0f ) : 0.0f;
				scene->wear_params[ 1 ] = this->m_finish.wear_scale;
				scene->wear_params[ 2 ] = this->m_finish.wear_rotation;
				scene->wear_params[ 3 ] = wears ? 1.0f : 0.0f;
				scene->grunge_params[ 0 ] = this->m_finish.grunge_scale;
				scene->grunge_params[ 1 ] = this->m_finish.grunge_rotation;
				scene->grunge_params[ 2 ] = wears && this->m_finish_gpu.grunge ? 1.0f : 0.0f;
				scene->grunge_params[ 3 ] = 0.0f;
				this->m_context->Unmap( this->m_cb_scene, 0 );
			}

			ID3D11ShaderResourceView* views[ 10 ]{
				material.color, finish_normal, finish_roughness,
				finish_ao, material.gloss,
				painted ? this->m_finish_gpu.color : nullptr,
				painted ? regions : nullptr,
				painted && composite ? composite->position : nullptr,
				painted ? this->m_finish_gpu.wear : nullptr,
				painted ? this->m_finish_gpu.grunge : nullptr };
			this->m_context->PSSetShaderResources( 0, 10, views );

			// A draw call whose albedo never resolved would sample black and hide
			// the model; skip it rather than paint a silhouette.
			if ( material.color && ( !chams_material || !chams_material->enabled || this->m_chams_overlay ) )
			{
				this->m_context->DrawIndexed( dc.index_count, dc.index_offset, 0 );
			}
		}

		if ( chams_material && chams_material->enabled )
		{
			// Keyed like the mesh itself: the in-game chams cache the legacy weapon mesh
			// under the bare path, and drawing that over the HD preview misses it.
			g_renderer.draw_external( mesh_key, mesh, *chams_material,
				this->m_skin_matrices, this->m_view_projection, this->m_eye );
		}

		if ( use_msaa )
		{
			// Resolve the multisampled colour into the SRV texture the editor samples.
			this->m_context->OMSetRenderTargets( 0, nullptr, nullptr );
			this->m_context->ResolveSubresource( this->m_color_texture, 0, this->m_msaa_color, 0, DXGI_FORMAT_R8G8B8A8_UNORM );
		}

		this->m_context->OMSetRenderTargets( 1, &saved_rtv, saved_dsv );
		if ( saved_viewport_count > 0 )
		{
			this->m_context->RSSetViewports( 1, &saved_viewport );
		}
		if ( saved_rtv ) saved_rtv->Release( );
		if ( saved_dsv ) saved_dsv->Release( );

		return this->m_srv;
	}

	void preview::shutdown( )
	{
		this->m_finish = {};
		this->m_finish_gpu = {};
		for ( auto& [ path, texture ] : this->m_textures )
		{
			if ( texture.srv ) texture.srv->Release( );
			if ( texture.texture ) texture.texture->Release( );
		}
		this->m_textures.clear( );
		this->m_materials.clear( );
		this->m_composites.clear( );
		this->m_material_composite.clear( );
		this->m_composite_by_color.clear( );
		this->m_composites_indexed = false;

		for ( auto& [ path, mesh ] : this->m_meshes )
		{
			if ( mesh.vertex_buffer ) mesh.vertex_buffer->Release( );
			if ( mesh.index_buffer ) mesh.index_buffer->Release( );
		}
		this->m_meshes.clear( );

		if ( this->m_vertex_shader ) this->m_vertex_shader->Release( );
		if ( this->m_pixel_shader ) this->m_pixel_shader->Release( );
		if ( this->m_input_layout ) this->m_input_layout->Release( );
		if ( this->m_cb_scene ) this->m_cb_scene->Release( );
		if ( this->m_cb_bones ) this->m_cb_bones->Release( );
		if ( this->m_sampler ) this->m_sampler->Release( );
		if ( this->m_clamp_sampler ) this->m_clamp_sampler->Release( );
		this->m_clamp_sampler = nullptr;
		if ( this->m_rasterizer ) this->m_rasterizer->Release( );
		if ( this->m_depth_state ) this->m_depth_state->Release( );
		if ( this->m_blend_state ) this->m_blend_state->Release( );

		this->release_target( );
		this->m_ready = false;
	}

} // namespace chams
