// FxDecalPool: VFX3's impact decals (docs/design/m2a-fx3.md §5). See fx_decals.h.
#include "fx_decals.h"

#include "godot_util.h"
#include "render_bridge.h"
#include "viewmodel_muzzle.h"

#include "bf3/core/res/res_loader.h"

#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/core/object.hpp>

#include <algorithm>
#include <cmath>

namespace bf3::godot_side {

using namespace godot;

FxDecalAssets FxDecalAssets::load(const core::EbxLibrary &p_level, const core::LevelMount &p_mount, std::span<const std::string> p_names) {
	FxDecalAssets out;
	std::vector<std::string> textures;
	for (const core::EbxLibrary::Entry &e : p_level.entries()) {
		if (e.partition != nullptr && e.partition->primary().type_name() == "TextureAsset") {
			textures.emplace_back(e.name);
		}
	}
	std::sort(textures.begin(), textures.end());
	for (const std::string &name : p_names) {
		core::Result<fx::DecalTemplate> t = fx::load_decal_template(p_level, name, textures);
		if (!t) {
			out.errors.push_back(name + ": " + t.error().message);
			continue;
		}
		if (t->texture.empty()) {
			++out.without_texture;
		} else if (out.textures.find(t->texture) == out.textures.end()) {
			core::Result<core::res::LoadedTexture> loaded = core::res::load_texture(p_mount, t->texture);
			if (!loaded) {
				out.errors.push_back(t->texture + ": " + loaded.error().message);
				t->texture.clear();
				++out.without_texture;
			} else {
				core::Result<render::PreparedTexture> prepared = render::prepare_texture(*loaded, render::TextureRole::Color);
				if (!prepared) {
					out.errors.push_back(t->texture + ": " + prepared.error().message);
					t->texture.clear();
					++out.without_texture;
				} else {
					out.texture_bytes += prepared->data.size();
					out.textures.emplace(t->texture, std::move(*prepared));
				}
			}
		}
		out.templates.push_back(std::move(*t));
	}
	std::sort(out.templates.begin(), out.templates.end(), [](const fx::DecalTemplate &a, const fx::DecalTemplate &b) {
		return a.name < b.name;
	});
	return out;
}

const fx::DecalTemplate *FxDecalAssets::find(std::string_view p_name) const {
	const auto it = std::lower_bound(templates.begin(), templates.end(), p_name, [](const fx::DecalTemplate &a, std::string_view b) {
		return std::string_view(a.name) < b;
	});
	return it != templates.end() && it->name == p_name ? &*it : nullptr;
}

FxDecalPool::~FxDecalPool() {
	clear();
}

bool FxDecalPool::build(Node3D *p_parent, const FxDecalAssets &p_assets, std::string *r_error) {
	clear();
	if (p_parent == nullptr) {
		if (r_error != nullptr) {
			*r_error = "decal pool: no parent";
		}
		return false;
	}
	assets_ = &p_assets;
	for (const auto &[name, prepared] : p_assets.textures) {
		Ref<ImageTexture> t = make_texture(prepared);
		if (t.is_valid()) {
			textures_.emplace(name, t);
		}
	}
	stats_.textures = uint32_t(textures_.size());
	root_ = memnew(Node3D);
	root_->set_name("FxDecals");
	root_id_ = root_->get_instance_id();
	p_parent->add_child(root_);
	slots_.reserve(fx::kMaxDecals);
	return true;
}

void FxDecalPool::place(const fx::DecalCue &p_cue) {
	if (root_ == nullptr || assets_ == nullptr || ObjectDB::get_instance(root_id_) == nullptr) {
		return;
	}
	const fx::DecalTemplate *t = assets_->find(p_cue.decal);
	if (t == nullptr) {
		++stats_.no_template;
		return;
	}
	const auto tex = textures_.find(t->texture);
	if (t->texture.empty() || tex == textures_.end()) {
		++stats_.untextured;
		return;
	}
	Slot *slot = nullptr;
	if (slots_.size() < fx::kMaxDecals) {
		Slot s;
		s.node = memnew(Decal);
		root_->add_child(s.node);
		slots_.push_back(s);
		slot = &slots_.back();
	} else {
		slot = &*std::min_element(slots_.begin(), slots_.end(), [](const Slot &a, const Slot &b) {
			// A free node first, then the oldest live one.
			return a.live != b.live ? !a.live : a.order < b.order;
		});
		stats_.replaced += slot->live ? 1 : 0;
	}
	const fx::DecalPlacement pl = fx::decal_placement(*t, p_cue.seed);
	Transform3D x = to_godot_transform(p_cue.transform);
	x.basis.orthonormalize();
	x.basis = x.basis * Basis(Vector3(0, 1, 0), real_t(pl.rotation));
	const Vector3 normal = x.basis.get_column(1);
	x.origin += normal * real_t(std::max(t->normal_offset, 0.0f));
	Decal *d = slot->node;
	d->set_global_transform(x);
	d->set_size(Vector3(pl.size, kDecalDepth, pl.size));
	d->set_texture(Decal::TEXTURE_ALBEDO, tex->second);
	d->set_albedo_mix(1.0f);
	d->set_modulate(Color(1, 1, 1, 1));
	const float clip = std::clamp(t->clip_angle, 0.0f, 89.0f) * 3.14159265f / 180.0f;
	d->set_normal_fade(0.5f * std::cos(clip) + 0.5f);
	d->set_upper_fade(0.3f);
	d->set_lower_fade(0.3f);
	d->set_visible(true);
	slot->age = std::max(0.0, double(p_cue.start_age));
	slot->live = true;
	slot->order = next_order_++;
	++stats_.placed;
}

void FxDecalPool::step(double p_dt) {
	if (root_ == nullptr || ObjectDB::get_instance(root_id_) == nullptr) {
		return;
	}
	const double dt = std::isfinite(p_dt) ? std::clamp(p_dt, 0.0, 2.0) : 0.0;
	uint32_t live = 0;
	for (Slot &s : slots_) {
		if (!s.live) {
			continue;
		}
		s.age += dt;
		const double fade = s.age - double(fx::kDecalFadeSeconds);
		if (fade >= double(kDecalFadeOutSeconds)) {
			s.live = false;
			s.node->set_visible(false);
			++stats_.faded;
			continue;
		}
		if (fade > 0.0) {
			s.node->set_modulate(Color(1, 1, 1, float(1.0 - fade / double(kDecalFadeOutSeconds))));
		}
		++live;
	}
	stats_.live = live;
}

void FxDecalPool::clear() {
	// The parent may have freed the node already (its children go with it): look it up by id.
	if (root_ != nullptr) {
		if (Node3D *root = Object::cast_to<Node3D>(ObjectDB::get_instance(root_id_))) {
			if (root->is_inside_tree()) {
				root->queue_free(); // the Decal children go with it
			} else {
				memdelete(root);
			}
		}
		root_ = nullptr;
	}
	slots_.clear();
	textures_.clear();
	assets_ = nullptr;
	stats_.live = 0;
}

} // namespace bf3::godot_side
