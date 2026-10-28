#include "bf3/render/weapon_sockets.h"

#include <algorithm>
#include <utility>

namespace bf3::render {

namespace {

anim::Mat34 mat_of(const std::optional<core::EbxValue> &p_v) {
	const std::optional<std::array<core::Vec3, 4>> rows = p_v ? p_v->as_transform() : std::nullopt;
	return rows ? anim::mat_from_linear_transform(*rows) : anim::Mat34{};
}

std::string mesh_name(const core::EbxResolver &p_db, const core::EbxStruct &p_object, std::string_view p_field,
		std::vector<std::string> &r_skipped) {
	const std::optional<core::EbxInstance> asset = p_db.follow(p_object, p_field);
	if (!asset) {
		return {};
	}
	const std::string name(p_db.name_of(asset->partition().guid()).value_or(std::string_view()));
	if (!asset->is_a("MeshAsset")) {
		r_skipped.push_back(std::string(p_field) + " " + name + " is a " + std::string(asset->type_name()));
		return {};
	}
	return name;
}

bool contains(const std::vector<std::string> &p_names, std::string_view p_name) {
	return std::find(p_names.begin(), p_names.end(), p_name) != p_names.end();
}

}

std::vector<WeaponSocket> read_weapon_sockets(const core::EbxResolver &p_db, const core::EbxInstance &p_swd) {
	std::vector<std::pair<core::Guid, anim::Mat34>> placed_3p;
	if (const std::optional<core::EbxValue> states = p_swd.get("WeaponStates")) {
		const core::OptionalEbxArray arr = states->as_array();
		const std::optional<core::EbxValue> s0 = arr && arr->size() > 0 ? arr->get(0) : std::nullopt;
		const std::optional<core::EbxStruct> st = s0 ? s0->as_struct() : std::nullopt;
		const std::optional<core::EbxValue> list = st ? st->get("Mesh3pRigidMeshSocketObjectTransforms") : std::nullopt;
		const core::OptionalEbxArray rows = list ? list->as_array() : std::nullopt;
		for (size_t i = 0; rows && i < rows->size(); ++i) {
			const std::optional<core::EbxValue> row = rows->get(i);
			const std::optional<core::EbxStruct> rs = row ? row->as_struct() : std::nullopt;
			const std::optional<core::EbxInstance> object = rs ? p_db.follow(*rs, "SocketObject") : std::nullopt;
			if (object) {
				placed_3p.emplace_back(object->guid(), mat_of(rs->get("Transform")));
			}
		}
	}
	std::vector<WeaponSocket> out;
	for (const core::EbxInstance &socket : p_db.follow_all(p_swd.data(), "Sockets")) {
		if (!socket.is_a("SocketData")) {
			continue;
		}
		WeaponSocket s;
		if (const std::optional<core::EbxInstance> unlock = p_db.follow(socket.data(), "UnlockAsset")) {
			s.unlock = std::string(p_db.name_of(unlock->partition().guid()).value_or(std::string_view()));
		}
		const std::optional<core::EbxValue> bone = socket.get("BoneName");
		s.bone = std::string(bone && bone->as_str() ? *bone->as_str() : std::string_view());
		const std::optional<core::EbxValue> zoom_hide = socket.get("HideByZoomTransition");
		const std::optional<core::EbxValue> light_hide = socket.get("HideByLightToggle");
		s.hide_by_zoom_transition = zoom_hide && zoom_hide->as_bool().value_or(false);
		s.hide_by_light_toggle = light_hide && light_hide->as_bool().value_or(false);
		const anim::Mat34 socket_m =
				anim::mat_mul(mat_of(socket.get("BoneRigidTransform")), mat_of(socket.get("Transform")));
		for (const core::EbxInstance &object : p_db.follow_all(socket.data(), "AvailableObjects")) {
			const bool skinned = object.is_a("WeaponSkinnedSocketObjectData");
			if (!skinned && !object.is_a("WeaponRegularSocketObjectData")) {
				s.skipped.push_back(std::string(object.type_name()));
				continue;
			}
			WeaponSocketObject o;
			o.skinned = skinned;
			o.mesh_1p = mesh_name(p_db, object.data(), "Asset1p", s.skipped);
			o.mesh_1p_zoom = mesh_name(p_db, object.data(), "Asset1pzoom", s.skipped);
			o.mesh_3p = mesh_name(p_db, object.data(), "Asset3p", s.skipped);
			o.transform = skinned ? anim::Mat34{} : anim::mat_mul(socket_m, mat_of(object.get("Transform")));
			for (const auto &[guid, m] : placed_3p) {
				if (guid == object.guid()) {
					o.transform_3p = m;
				}
			}
			if (o.mesh_1p.empty() && o.mesh_1p_zoom.empty() && o.mesh_3p.empty()) {
				continue;
			}
			s.objects.push_back(std::move(o));
		}
		out.push_back(std::move(s));
	}
	return out;
}

bool socket_drawn(std::string_view p_unlock, const WeaponAccessories &p_accessories) {
	if (contains(p_accessories.active, p_unlock)) {
		return true;
	}
	return p_unlock.starts_with(kDefaultSocketUnlockPrefix) && !contains(p_accessories.listed, p_unlock);
}

std::vector<std::string> weapon_modifier_unlocks(const WeaponAccessories &p_accessories) {
	std::vector<std::string> out;
	for (const std::string &u : p_accessories.active) {
		if (!u.starts_with(kDefaultSocketUnlockPrefix)) {
			out.push_back(u);
		}
	}
	return out;
}

}
