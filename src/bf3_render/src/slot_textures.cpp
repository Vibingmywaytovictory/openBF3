#include "bf3/render/slot_textures.h"

#include "bf3/core/res/res_loader.h"
#include "bf3/core/thread_pool.h"

namespace bf3::render {

void SlotTextureWants::add(const MeshMaterials &p_materials, std::span<const PreparedSurface> p_surfaces) {
	for (const PreparedSurface &s : p_surfaces) {
		if (s.material_index >= p_materials.slots.size()) {
			continue;
		}
		const MaterialSlot &slot = p_materials.slots[s.material_index];
		for (auto [param, role] :
				{ std::pair{ "Diffuse", TextureRole::Color }, std::pair{ "Normal", TextureRole::Normal } }) {
			const std::string name = slot.texture(param);
			if (!name.empty() && seen.insert(name).second) {
				wanted.emplace_back(name, role);
			}
		}
	}
}

std::vector<PreparedTexture> load_slot_textures(const core::LevelMount &p_mount, const SlotTextureWants &p_wants,
		std::string_view p_what, std::vector<std::string> &r_notes) {
	const auto &wanted = p_wants.wanted;
	std::vector<std::string> errors(wanted.size());
	std::vector<PreparedTexture> textures(wanted.size());
	core::parallel_for(wanted.size(), [&](size_t i) {
		core::Result<core::res::LoadedTexture> tex = core::res::load_texture(p_mount, wanted[i].first);
		if (!tex) {
			errors[i] = tex.error().message;
			return;
		}
		core::Result<PreparedTexture> p = prepare_texture(*tex, wanted[i].second);
		if (!p) {
			errors[i] = p.error().message;
			return;
		}
		textures[i] = std::move(*p);
	});
	std::vector<PreparedTexture> out;
	for (size_t i = 0; i < wanted.size(); ++i) {
		if (!errors[i].empty()) {
			r_notes.push_back(std::string(p_what) + " texture skipped: " + errors[i]);
			continue;
		}
		out.push_back(std::move(textures[i]));
	}
	return out;
}

}
