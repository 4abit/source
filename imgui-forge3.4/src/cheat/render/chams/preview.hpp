#pragma once

#include <render/chams/material.hpp>
#include <render/chams/mesh.hpp>
#include <render/chams/texture.hpp>
#include <render/chams/animation.hpp>
#include <core/assets/vpk.hpp>

#include <d3d11.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace chams {

	class preview
	{
	public:
		// How the camera frames the model. `upright` is the ESP preview: the figure is
		// tall, so the distance comes from its height and the camera stays level.
		// `object` is the inventory inspector, where a weapon is short and wide and has
		// to fit whichever way round it is turned.
		enum class framing
		{
			upright,
			object,
		};

		bool initialize( ID3D11Device* device, ID3D11DeviceContext* context );
		void shutdown( );

		[[nodiscard]] ID3D11ShaderResourceView* render( vpk_archive& vpk, const std::string& model_path,
			std::uint32_t width, std::uint32_t height,
			const config::visual_profile::chams::material* chams_material );

		// Paints a finish over whatever the model's own materials provide. Pass an invalid
		// finish (or nullptr) to draw the model as it ships.
		void set_finish( finish_data finish );
		void clear_finish( );

		// Wear float of the painted finish, 0 (Factory New) to 1 (Battle-Scarred).
		void set_wear( float wear ) { this->m_wear = wear; }

		// With chams set, also draw the model's own materials under them.
		void set_chams_overlay( bool overlay ) { this->m_chams_overlay = overlay; }

		void set_framing( framing mode );
		void set_view( float yaw_degrees, float pitch_degrees, float zoom );

		[[nodiscard]] bool bone_position( std::uint32_t bone, foundation::vec3& out ) const;

		[[nodiscard]] bool bone_transform( std::uint32_t bone, const foundation::vec3& local, foundation::vec3& out ) const;

		[[nodiscard]] bool bone_direction( std::uint32_t bone, const foundation::vec3& model_space, foundation::vec3& out ) const;

		// Projects a world position into pixel coordinates inside the viewport.
		// Returns false when the point is behind the camera.
		[[nodiscard]] bool project( const foundation::vec3& world, float& x, float& y ) const;

		void orbit( float delta_yaw );

		// Pitch is clamped just short of the poles so the up vector never degenerates.
		void orbit_pitch( float delta_pitch );
		void zoom( float factor );

		[[nodiscard]] const skinned_mesh* current_mesh( ) const { return this->m_mesh; }

		// Row-major view-projection of the last render, so overlays (bones,
		// hitboxes) can project into the same image.
		[[nodiscard]] const float ( &view_projection( ) const )[ 4 ][ 4 ] { return this->m_view_projection; }
		[[nodiscard]] const foundation::vec3& eye( ) const { return this->m_eye; }

	private:
		struct gpu_texture
		{
			ID3D11Texture2D* texture{};
			ID3D11ShaderResourceView* srv{};
		};

		struct gpu_mesh
		{
			ID3D11Buffer* vertex_buffer{};
			ID3D11Buffer* index_buffer{};
		};

		struct gpu_material
		{
			ID3D11ShaderResourceView* color{};
			ID3D11ShaderResourceView* normal{};
			ID3D11ShaderResourceView* metalness{};
			ID3D11ShaderResourceView* ambient_occlusion{};
			ID3D11ShaderResourceView* gloss{};
		};

		struct gpu_finish
		{
			ID3D11ShaderResourceView* color{};
			ID3D11ShaderResourceView* mask{};
			ID3D11ShaderResourceView* normal{};
			ID3D11ShaderResourceView* wear{};
			ID3D11ShaderResourceView* grunge{};
			bool resolved{};
		};

		// A weapon's csgo_composite_inputs: the paint regions and the object-space
		// position map of one body material, which is what the paint styles need on top
		// of the finish material itself.
		struct gpu_composite
		{
			ID3D11ShaderResourceView* masks{};
			ID3D11ShaderResourceView* position{};
			float uv_scale{ 1.0f };
			float weapon_length{ 32.0f };
			// Position channel the weapon is long along: 1 (G) for guns and most of the
			// knives' siblings, 2 (B) for most knives. Read off the map itself, since the
			// material's flags do not say.
			int long_axis{ 1 };
		};

		bool create_shaders( );
		bool create_states( );
		bool ensure_target( std::uint32_t width, std::uint32_t height );
		void release_target( );

		[[nodiscard]] ID3D11ShaderResourceView* get_texture( vpk_archive& vpk, const std::string& path );
		[[nodiscard]] const gpu_material& get_material( vpk_archive& vpk, const std::string& path );
		[[nodiscard]] const gpu_mesh* get_mesh( const std::string& model_path, const skinned_mesh& mesh );

		// The composite inputs belonging to a body material, matched on the albedo both
		// of them name or, for an HD body, on the file name. Null for parts that take no
		// paint (scopes, lenses, cartridges).
		[[nodiscard]] const gpu_composite* get_composite( vpk_archive& vpk, const std::string& material_path, bool hd );
		void index_composites( vpk_archive& vpk );

		void update_camera( const skinned_mesh& mesh, std::uint32_t width, std::uint32_t height );
		// `skin` poses the bounds. `hands_only` frames the hands instead of the whole arm.
		void update_object_camera( const skinned_mesh& mesh, std::uint32_t width, std::uint32_t height,
			const std::vector<bone_matrix>* skin = nullptr, bool hands_only = false );

		// Loads the idle clip and the skeleton it is authored against, once, and
		// caches the track -> model bone mapping for the current model.
		void ensure_idle_clip( vpk_archive& vpk, const std::string& model_path, const skinned_mesh& mesh );

		std::vector<bone_matrix> m_skin_matrices{};

		// Animated model-space transform per bone, so the editor's skeleton lines
		// and hitbox capsules follow the animation instead of the bind pose.
		std::vector<bone_matrix> m_bone_world{};

		nm_skeleton m_idle_skeleton{};
		nm_clip m_idle_clip{};
		std::vector<int> m_idle_track_to_bone{};
		std::string m_idle_mapped_model{};
		bool m_idle_loaded{};

		// The glove inspect loop, sampled every frame. A baked pose read as a statue.
		nm_skeleton m_glove_skeleton{};
		nm_clip m_glove_clip{};
		std::vector<int> m_glove_track_to_bone{};
		std::string m_glove_mapped_model{};
		bool m_glove_clip_loaded{};
		bool m_glove_view{};

		ID3D11Device* m_device{};
		ID3D11DeviceContext* m_context{};

		ID3D11Texture2D* m_color_texture{};
		ID3D11RenderTargetView* m_rtv{};
		ID3D11ShaderResourceView* m_srv{};
		ID3D11Texture2D* m_depth_texture{};
		ID3D11DepthStencilView* m_dsv{};
		std::uint32_t m_width{};
		std::uint32_t m_height{};

		ID3D11Texture2D* m_msaa_color{};
		ID3D11RenderTargetView* m_msaa_rtv{};
		ID3D11Texture2D* m_msaa_depth{};
		ID3D11DepthStencilView* m_msaa_dsv{};
		UINT m_msaa_samples{};

		ID3D11VertexShader* m_vertex_shader{};
		ID3D11PixelShader* m_pixel_shader{};
		ID3D11InputLayout* m_input_layout{};
		ID3D11Buffer* m_cb_scene{};
		ID3D11Buffer* m_cb_bones{};
		ID3D11SamplerState* m_sampler{};
		ID3D11SamplerState* m_clamp_sampler{};
		ID3D11RasterizerState* m_rasterizer{};
		ID3D11DepthStencilState* m_depth_state{};
		ID3D11BlendState* m_blend_state{};

		std::unordered_map<std::string, gpu_texture> m_textures{};
		std::unordered_map<std::string, gpu_material> m_materials{};
		std::unordered_map<std::string, gpu_mesh> m_meshes{};

		std::unordered_map<std::string, std::string> m_composite_by_color{};
		std::unordered_map<std::string, std::string> m_composite_by_name{};
		bool m_composites_indexed{};
		std::unordered_map<std::string, gpu_composite> m_composites{};
		std::unordered_map<std::string, const gpu_composite*> m_material_composite{};

		// Finish the inventory asked for, already resolved to archive paths. Its GPU
		// textures come out of the same cache as everything else, keyed by path.
		finish_data m_finish{};
		gpu_finish m_finish_gpu{};

		float m_yaw{ 0.0f };
		float m_pitch{ 0.0f };
		float m_zoom{ 1.0f };
		framing m_framing{ framing::upright };

		const skinned_mesh* m_mesh{};

		// Gloves are cut down to the hands and turned so the fingers point up. Cached
		// beside the shared extract, which still has the arms for anything else.
		skinned_mesh m_glove_mesh{};
		std::string m_glove_key{};

		float m_view_projection[ 4 ][ 4 ]{};
		foundation::vec3 m_eye{};
		bool m_chams_overlay{};
		bool m_ready{};
		float m_wear{};
	};

	inline preview g_preview{};

	// The inventory inspector runs its own camera and its own finish, so it cannot share
	// an instance with the ESP preview; the two would fight over the view every frame.
	inline preview g_inspect{};

} // namespace chams
