#include "fx_decals.h"

#include "godot_util.h"
#include "render_bridge.h"
#include "viewmodel_muzzle.h"

#include "bf3/core/res/res_loader.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/core/object.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

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

					fx::DecalPixelFormat pf = fx::DecalPixelFormat::Other;
					switch (prepared->format) {
						case render::GpuFormat::Bc1:
							pf = fx::DecalPixelFormat::Bc1;
							break;
						case render::GpuFormat::Bc3:
							pf = fx::DecalPixelFormat::Bc3;
							break;
						case render::GpuFormat::Rgba8:
							pf = fx::DecalPixelFormat::Rgba8;
							break;
						default:
							break;
					}
					core::Result<fx::DecalTexturePrep> rule = fx::prepare_decal_texture(pf, prepared->data, prepared->width, prepared->height);
					if (!rule || !rule->usable()) {
						out.errors.push_back(t->texture + ": " +
								(rule ? std::string(fx::decal_texture_kind_name(rule->kind)) + " (" + render::gpu_format_name(prepared->format) + ", " +
												std::to_string(int(std::lround(rule->transparent * 100.0f))) + " % see-through): not drawn" :
										rule.error().message));
						++out.refused;
						t->texture.clear();
						++out.without_texture;
					} else {
						if (rule->kind == fx::DecalTextureKind::MagentaKeyed) {
							prepared->data = std::move(rule->rgba_chain);
							prepared->format = render::GpuFormat::Rgba8;
							prepared->levels = rule->levels;
							prepared->stored_levels = rule->levels;
							++out.unkeyed;
						}
						out.texture_bytes += prepared->data.size();
						out.textures.emplace(t->texture, std::move(*prepared));
					}
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

std::vector<std::string> FxDecalPool::texture_report(const std::string &p_dump_dir) const {
	std::vector<std::string> out;
	if (assets_ == nullptr) {
		return out;
	}
	for (const auto &[name, tex] : textures_) {
		const auto prepared = assets_->textures.find(name);
		Ref<Image> img = tex->get_image();
		if (img.is_null()) {
			continue;
		}
		img = img->duplicate();
		if (img->is_compressed()) {
			img->decompress();
		}
		img->clear_mipmaps();
		img->convert(Image::FORMAT_RGBA8);
		if (!p_dump_dir.empty()) {
			const size_t slash = name.rfind('/');
			img->save_png(to_godot(p_dump_dir + "/" + (slash == std::string::npos ? name : name.substr(slash + 1)) + ".png"));
		}
		img->resize(64, 64, Image::INTERPOLATE_BILINEAR);
		double sum[4] = { 0, 0, 0, 0 };
		uint32_t opaque = 0;
		for (int32_t y = 0; y < 64; ++y) {
			for (int32_t x = 0; x < 64; ++x) {
				const Color c = img->get_pixel(x, y);
				sum[0] += c.r, sum[1] += c.g, sum[2] += c.b, sum[3] += c.a;
				opaque += c.a >= 0.5f ? 1 : 0;
			}
		}
		char line[512];
		std::snprintf(line, sizeof(line), "%s: source format %d -> %s %ux%u, mean rgba (%.2f %.2f %.2f %.2f), alpha >= 0.5 on %.0f %%", name.c_str(),
				prepared != assets_->textures.end() ? int(prepared->second.source_format) : -1,
				prepared != assets_->textures.end() ? render::gpu_format_name(prepared->second.format) : "?", prepared != assets_->textures.end() ? prepared->second.width : 0,
				prepared != assets_->textures.end() ? prepared->second.height : 0, sum[0] / 4096.0, sum[1] / 4096.0, sum[2] / 4096.0, sum[3] / 4096.0,
				100.0 * double(opaque) / 4096.0);
		out.push_back(line);
	}
	return out;
}

void FxDecalPool::clear() {

	if (root_ != nullptr) {
		if (Node3D *root = Object::cast_to<Node3D>(ObjectDB::get_instance(root_id_))) {
			if (root->is_inside_tree()) {
				root->queue_free();
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

}
