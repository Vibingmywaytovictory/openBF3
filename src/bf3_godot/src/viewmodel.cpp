#include "viewmodel.h"

#include "fp_camera.h"
#include "godot_util.h"
#include "render_bridge.h"

#include "bf3/host/presentation_events.h"
#include "bf3/host/presentation_weapon.h"
#include "bf3/sim/weapons/sim_time.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>

#include <algorithm>
#include <cmath>
#include <map>

namespace bf3::godot_side {

using namespace godot;
namespace sw = bf3::sim::weapons;

static_assert(bf3::anim::kVmPhaseDeploying == uint8_t(sw::WeaponPhase::Deploying));

namespace {

const char *kVignetteShader = R"(shader_type canvas_item;
uniform float strength = 0.0;
void fragment() {
	vec2 d = (UV - vec2(0.5)) * vec2(SCREEN_PIXEL_SIZE.y / SCREEN_PIXEL_SIZE.x, 1.0) * 2.0;
	float r = length(d);
	COLOR = vec4(0.0, 0.0, 0.0, strength * smoothstep(0.62, 0.98, r));
}
)";

Transform3D xform_to_godot(const bf3::anim::Xform &p_x) {
	return to_godot(p_x);
}

}

ViewmodelFrame viewmodel_frame_from(const bf3::host::PresentedWeapon &p_weapon, const Vector3 &p_eye, double p_yaw, double p_pitch, float p_dt, bool p_sprinting,
		float p_speed, float p_view_yaw_rate, float p_view_pitch_rate) {
	ViewmodelFrame f;
	f.time_s = p_weapon.time_s;
	f.dt = p_dt;
	f.phase = uint8_t(p_weapon.phase);
	f.phase_start_s = p_weapon.phase_start_s;
	f.phase_end_s = p_weapon.phase_end_s;
	f.magazine = p_weapon.magazine;
	f.zoomed = p_weapon.valid && p_weapon.zoomed;
	f.sprinting = p_sprinting;
	f.speed = p_speed;
	f.recoil_pitch_deg = p_weapon.recoil.pitch;
	f.recoil_yaw_deg = p_weapon.recoil.yaw;
	f.eye = p_eye;
	f.yaw = p_yaw;
	f.pitch = p_pitch;
	f.view_yaw_rate = p_view_yaw_rate;
	f.view_pitch_rate = p_view_pitch_rate;
	return f;
}

void viewmodel_cues_from(std::span<const bf3::present::PresentEvent> p_events, int32_t p_magazine, std::vector<bf3::anim::VmCue> &r_cues) {
	using Kind = bf3::anim::VmCue::Kind;
	r_cues.clear();
	for (const bf3::present::PresentEvent &e : p_events) {
		bf3::anim::VmCue c;
		c.time_s = sw::units_to_seconds(e.time);
		if (e.kind == bf3::host::PresentKind::OwnShot) {
			c.kind = Kind::Shot;
		} else if (e.kind == bf3::host::PresentKind::OwnTimed) {
			switch (e.id) {
				case sw::weapon_events::kDeployStarted:
					c.kind = Kind::DeployStarted;
					break;
				case sw::weapon_events::kDeployFinished:
					c.kind = Kind::DeployFinished;
					break;
				case sw::weapon_events::kReloadStarted:
					c.kind = Kind::ReloadStarted;
					c.empty_reload = p_magazine == 0;
					break;
				case sw::weapon_events::kReloadCommitted:
					c.kind = Kind::ReloadCommitted;
					break;
				case sw::weapon_events::kReloadFinished:
					c.kind = Kind::ReloadFinished;
					break;
				case sw::weapon_events::kReloadCancelled:
					c.kind = Kind::ReloadCancelled;
					break;
				case sw::weapon_events::kBoltStarted:
					c.kind = Kind::BoltStarted;
					break;
				case sw::weapon_events::kThrown:
					c.kind = Kind::Thrown;
					break;
				case sw::weapon_events::kHoldStarted:
					c.kind = Kind::HoldStarted;
					break;
				default:
					continue;
			}
		} else {
			continue;
		}
		r_cues.push_back(c);
	}
	std::stable_sort(r_cues.begin(), r_cues.end(), [](const bf3::anim::VmCue &p_a, const bf3::anim::VmCue &p_b) { return p_a.time_s < p_b.time_s; });
}

Bf3Viewmodel::Bf3Viewmodel() = default;
Bf3Viewmodel::~Bf3Viewmodel() = default;

void Bf3Viewmodel::_bind_methods() {}

size_t Bf3Viewmodel::item_count() const {
	return set_ ? set_->items.size() : 0;
}

const bf3::render::ViewmodelItem *Bf3Viewmodel::current_item() const {
	return set_ && item_ >= 0 && size_t(item_) < set_->items.size() ? &set_->items[size_t(item_)] : nullptr;
}

bool Bf3Viewmodel::build(const bf3::render::ViewmodelSet &p_set, const bf3::anim::ViewmodelTuning &p_tuning) {
	if (p_set.items.empty() || p_set.rig.find("CameraJoint") < 0) {
		return false;
	}
	set_ = &p_set;
	tuning_ = p_tuning;
	camera_joint_ = p_set.rig.find("CameraJoint");

	std::map<std::string, Ref<Texture2D>> textures;
	for (const bf3::render::PreparedTexture &t : p_set.textures) {
		const Ref<ImageTexture> tex = make_texture(t);
		if (tex.is_valid()) {
			textures[t.name] = tex;
		}
	}
	auto find_texture = [&](const std::string &p_name) -> Ref<Texture2D> {
		const auto it = textures.find(p_name);
		return it == textures.end() ? Ref<Texture2D>() : it->second;
	};
	skeleton_ = make_skeleton(p_set.rig);
	skeleton_->set_name("ViewmodelRig");
	add_child(skeleton_);

	MaterialOptions options;
	options.viewmodel = true;
	options.fov_override = 55.0f;
	auto make_mesh = [&](const bf3::render::ViewmodelPart &p_part, const std::vector<bf3::render::ViewmodelSlotLook> &p_looks,
							 std::vector<Ref<StandardMaterial3D>> &r_materials) {
		std::map<uint32_t, Ref<StandardMaterial3D>> materials;
		const Ref<ArrayMesh> mesh = make_array_mesh(p_part.prepared, [&](const bf3::render::PreparedSurface &p_s) -> Ref<Material> {
			if (p_s.material_index >= p_part.materials.slots.size()) {
				return Ref<Material>();
			}
			Ref<StandardMaterial3D> &m = materials[p_s.material_index];
			if (m.is_null()) {
				m = make_standard_material(p_part.materials.slots[p_s.material_index], !p_s.tangents.empty(), find_texture, options);
				if (p_s.material_index < p_looks.size()) {

					const bf3::render::ViewmodelSlotLook &look = p_looks[p_s.material_index];
					if (look.alpha_test) {
						m->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR);
						m->set_alpha_scissor_threshold(bf3::render::kViewmodelAlphaScissor);
					}
					if (look.double_sided) {
						m->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
					}
				}
				r_materials.push_back(m);
			}
			return m;
		});
		MeshInstance3D *mi = memnew(MeshInstance3D);
		mi->set_name(to_godot(p_part.mesh.substr(p_part.mesh.rfind('/') + 1)));
		mi->set_mesh(mesh);

		mi->set_custom_aabb(AABB(Vector3(-3, -1, -3), Vector3(6, 4, 6)));
		mi->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		skeleton_->add_child(mi);
		mi->set_skeleton_path(NodePath(".."));
		return mi;
	};
	arms_ = make_mesh(p_set.arms, p_set.arms_looks, arms_materials_);
	items_.resize(p_set.items.size());
	for (size_t i = 0; i < p_set.items.size(); ++i) {
		const bf3::render::ViewmodelItem &it = p_set.items[i];
		ItemNodes &n = items_[i];
		n.skin = make_skin(it.binds.binds());
		n.weapon = make_mesh(it.weapon, it.weapon_looks, n.materials);
		n.weapon->set_skin(n.skin);
		n.weapon->set_visible(false);
		n.refs = bf3::render::viewmodel_clip_refs(it);
		for (const bf3::render::ViewmodelAttachment &a : it.attachments) {
			ItemNodes::Attachment na;
			na.mesh = make_mesh(a.part, a.looks, n.materials);
			na.mesh->set_visible(false);
			na.bone = a.rig_bone;
			na.xform = to_godot(a.transform);
			na.hip = a.hip;
			na.zoom = a.zoom;
			n.attachments.push_back(na);
		}
	}
	arms_->set_visible(false);

	vignette_layer_ = memnew(CanvasLayer);
	vignette_layer_->set_name("OpticVignette");
	vignette_layer_->set_layer(-1);
	vignette_ = memnew(ColorRect);
	vignette_->set_anchors_preset(Control::PRESET_FULL_RECT);
	vignette_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	Ref<Shader> shader;
	shader.instantiate();
	shader->set_code(kVignetteShader);
	vignette_material_.instantiate();
	vignette_material_->set_shader(shader);
	vignette_material_->set_shader_parameter("strength", 0.0f);
	vignette_->set_material(vignette_material_);
	vignette_->set_visible(false);
	vignette_layer_->add_child(vignette_);
	add_child(vignette_layer_);
	return true;
}

void Bf3Viewmodel::select_item(int p_index) {
	if (!set_) {
		return;
	}
	if (item_ >= 0 && size_t(item_) < items_.size()) {
		items_[size_t(item_)].weapon->set_visible(false);
		for (const ItemNodes::Attachment &a : items_[size_t(item_)].attachments) {
			a.mesh->set_visible(false);
		}
	}
	item_ = p_index >= 0 && size_t(p_index) < items_.size() ? p_index : -1;
	ads_.reset();
	ads_frame_ = bf3::anim::AdsFrame{};
	output_ = bf3::anim::ViewmodelOutput{};
	if (item_ < 0) {
		arms_->set_visible(false);
		animator_.reset();
		return;
	}
	ItemNodes &n = items_[size_t(item_)];
	n.weapon->set_visible(true);
	arms_->set_skin(n.skin);
	arms_->set_visible(true);
	animator_ = std::make_unique<bf3::anim::ViewmodelAnimator>(set_->rig, n.refs, tuning_);
	if (!has_user_arg("no-locomotion")) {
		animator_->set_locomotion(bf3::render::viewmodel_locomotion(set_->items[size_t(item_)]));
	}
	animator_->set_throw_flow(set_->items[size_t(item_)].throw_flow);
	ads_params_ = bf3::render::viewmodel_ads_params(set_->items[size_t(item_)], tuning_);
	viewmodel_fov_ = -1;
	set_viewmodel_fov(ads_params_.render_fov);
}

void Bf3Viewmodel::set_tuning(const bf3::anim::ViewmodelTuning &p_tuning) {
	tuning_ = p_tuning;
	select_item(item_);
}

void Bf3Viewmodel::set_viewmodel_fov(float p_fov) {
	if (std::fabs(p_fov - viewmodel_fov_) < 1e-5f) {
		return;
	}
	viewmodel_fov_ = p_fov;
	for (const Ref<StandardMaterial3D> &m : arms_materials_) {
		m->set_fov_override(p_fov);
	}
	if (item_ >= 0) {
		for (const Ref<StandardMaterial3D> &m : items_[size_t(item_)].materials) {
			m->set_fov_override(p_fov);
		}
	}
}

float Bf3Viewmodel::applied_weapon_fov() const {
	if (item_ < 0 || items_[size_t(item_)].materials.empty()) {
		return 0.0f;
	}
	return float(items_[size_t(item_)].materials.front()->get_fov_override());
}

size_t Bf3Viewmodel::alpha_scissor_materials(int p_item) const {
	if (p_item < 0 || size_t(p_item) >= items_.size()) {
		return 0;
	}
	size_t n = 0;
	for (const Ref<StandardMaterial3D> &m : items_[size_t(p_item)].materials) {
		n += m->get_transparency() == BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR ? 1 : 0;
	}
	return n;
}

float Bf3Viewmodel::applied_arms_fov() const {
	return arms_materials_.empty() ? 0.0f : float(arms_materials_.front()->get_fov_override());
}

void Bf3Viewmodel::present(const ViewmodelFrame &p_frame, std::span<const bf3::anim::VmCue> p_cues, Camera3D *p_camera) {
	if (!set_ || item_ < 0 || !animator_) {
		return;
	}
	const uint64_t t0 = Time::get_singleton()->get_ticks_usec();

	ads_allowed_ = bf3::anim::ads_allowed(p_frame.phase, p_frame.sprinting);
	ads_frame_ = ads_.step(ads_params_, p_frame.zoomed && ads_allowed_, p_frame.dt);

	bf3::anim::ViewmodelFrameInput in;
	in.time_s = p_frame.time_s;
	in.dt = p_frame.dt;
	in.phase = p_frame.phase;
	in.phase_start_s = p_frame.phase_start_s;
	in.phase_end_s = p_frame.phase_end_s;
	in.ads = ads_frame_.blend;
	in.speed = p_frame.speed;
	in.grounded = p_frame.grounded;
	in.sprinting = p_frame.sprinting;
	in.view_yaw_rate = p_frame.view_yaw_rate;
	in.view_pitch_rate = p_frame.view_pitch_rate;
	animator_->step(in, p_cues, pose_, output_);
	bf3::anim::model_pose(set_->rig, pose_, model_);

	ViewEffects fx;
	fx.world_fov_deg = ads_frame_.world_fov;
	fx.recoil_pitch_deg = p_frame.recoil_pitch_deg;
	fx.recoil_yaw_deg = p_frame.recoil_yaw_deg;
	fx.kick_pitch_deg = output_.camera_recoil_pitch_deg;
	fx.kick_yaw_deg = output_.camera_recoil_yaw_deg;
	apply_view_effects(p_camera, p_frame.eye, p_frame.yaw, p_frame.pitch, fx);

	const Transform3D cam = p_camera ? p_camera->get_global_transform() : Transform3D();
	const Transform3D joint = xform_to_godot(model_[size_t(camera_joint_)]);
	const Transform3D turn(Basis(Vector3(0, 1, 0), real_t(Math::PI)), Vector3());
	skeleton_->set_global_transform(cam * xform_to_godot(output_.weapon_offset) * turn * joint.affine_inverse());
	for (size_t b = 0; b < pose_.joints.size() && b < size_t(skeleton_->get_bone_count()); ++b) {
		skeleton_->set_bone_pose_rotation(int32_t(b), to_godot(pose_.joints[b].q));
		skeleton_->set_bone_pose_position(int32_t(b), to_godot(pose_.joints[b].t));
	}

	const bool aimed = ads_frame_.blend >= 0.5f;
	for (const ItemNodes::Attachment &a : items_[size_t(item_)].attachments) {
		const bool on = (aimed ? a.zoom : a.hip) && a.bone >= 0 && size_t(a.bone) < model_.size();
		a.mesh->set_visible(on);
		if (on) {
			a.mesh->set_transform(xform_to_godot(model_[size_t(a.bone)]) * a.xform);
		}
	}
	set_viewmodel_fov(ads_frame_.viewmodel_fov);

	const bool optic = ads_params_.can_zoom && ads_params_.zoom_fov <= bf3::render::kOpticVignetteMaxFov;
	vignette_strength_ = optic ? bf3::anim::smoothstep01(ads_frame_.fov_progress) : 0.0f;
	vignette_->set_visible(vignette_strength_ > 0.0f);
	if (vignette_strength_ > 0.0f) {
		vignette_material_->set_shader_parameter("strength", vignette_strength_);
	}
	last_present_us_ = double(Time::get_singleton()->get_ticks_usec() - t0);
}

}
