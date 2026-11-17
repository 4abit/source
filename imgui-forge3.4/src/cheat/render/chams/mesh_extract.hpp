#pragma once

#include <render/chams/mesh.hpp>
#include <core/assets/vpk.hpp>

#include <string>

namespace chams {

	// Weapon models ship every part twice. The legacy meshes carry the unwrap the classic
	// paint kits were painted on; `hd` picks the re-charted meshes the newer loose-map
	// finishes (and the stock look) are authored for. A model that only has one of the
	// two falls back to whatever it has.
	// `viewmodel` keeps the first-person meshes and drops the world model. The world
	// mesh skinned with viewmodel bones is a different shape, so it floats off the hand.
	[[nodiscard]] skinned_mesh extract_mesh( vpk_archive& vpk, const std::string& model_path,
		bool hd = false, bool viewmodel = false );

} // namespace chams
