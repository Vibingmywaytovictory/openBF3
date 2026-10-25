#include "overview_map.h"

#include "godot_util.h"
#include "minimap_hud.h"

#include "bf3/core/ui/hud_icons.h"
#include "bf3/core/ui/hud_widgets.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;

namespace {

Bf3OverviewMap *g_current = nullptr;

constexpr std::string_view kOverviewShader = R"(shader_type canvas_item;
uniform sampler2D satellite : filter_linear_mipmap, repeat_disable;
uniform sampler2D combat_area : filter_linear, repeat_disable;
uniform sampler2D detail : filter_linear, repeat_disable;
uniform sampler2D hatch : filter_linear, repeat_enable;
uniform vec4 tint = vec4(0.3, 0.6, 0.8, 1.0);
uniform vec4 area_color = vec4(0.991, 0.706, 0.513, 0.45);
uniform float threshold = 0.496;
uniform float edge_width = 0.01;
uniform vec4 detail_tint = vec4(0.9, 1.0, 1.0, 0.3);
uniform float detail_threshold = 0.5;
uniform float opacity = 0.5;
uniform float satellite_weight = 0.0;
uniform bool has_detail = false;
uniform bool has_hatch = false;
uniform vec4 local_rect = vec4(0.0, 0.0, 1.0, 1.0);
uniform vec2 centre_local = vec2(0.0);
uniform vec2 area_centre = vec2(0.0);
uniform float metres_per_unit = 1.0;
uniform vec2 world_center = vec2(0.0);
uniform float world_size = 1024.0;
uniform vec2 hatch_wrap = vec2(70.0, 70.0);
void fragment() {
	vec2 p = local_rect.xy + UV * local_rect.zw;
	vec2 world = area_centre - (p - centre_local) * metres_per_unit;
	vec2 uv = vec2(0.5) - (world - world_center) / world_size;
	vec3 base = mix(tint.rgb, texture(satellite, uv).r * tint.rgb, satellite_weight);
	vec4 acc = vec4(base * opacity, opacity);
	if (has_detail) {
		float w = detail_tint.a * smoothstep(detail_threshold - edge_width, detail_threshold + edge_width, texture(detail, uv).r);
		acc = vec4(detail_tint.rgb * w, w) + acc * (1.0 - w);
	}
	float outside = smoothstep(threshold - edge_width, threshold + edge_width, texture(combat_area, uv).r);
	float h = has_hatch ? texture(hatch, uv * hatch_wrap).r : 1.0;
	float ca = outside * area_color.a * h;
	acc = vec4(area_color.rgb * ca, ca) + acc * (1.0 - ca);
	COLOR = vec4(acc.rgb / max(acc.a, 1e-6), acc.a);
}
)";

constexpr std::string_view kPlayerIcon = ui::soldier_icon(false, 1, 1).icon;
constexpr std::string_view kDeadIcon = "UIHudIcon_PlayerDead";

ui::HudIconChoice mark_icon(OverviewMarkKind p_kind) {
	switch (p_kind) {
		case OverviewMarkKind::Local:
			return ui::local_player_icon(false);
		case OverviewMarkKind::Friendly:
			return { kPlayerIcon, ui::HudIconState::Friendly };
		case OverviewMarkKind::Squad:
			return { kPlayerIcon, ui::HudIconState::Squad };
		case OverviewMarkKind::Enemy:
			return { kPlayerIcon, ui::HudIconState::Enemy };
		case OverviewMarkKind::Dead:
			return { kDeadIcon, ui::HudIconState::Friendly };
	}
	return { kPlayerIcon, ui::HudIconState::Neutral };
}

bool is_map_clip(const ui::SwfScene &p_scene, const ui::SwfSceneNode &p_node) {
	if (p_node.name != kOverviewMapClip) {
		return false;
	}
	const ui::SwfSceneNode *owner = p_scene.node(p_node.parent);
	return owner != nullptr && owner->symbol == kOverviewMapSymbol;
}

}

std::vector<OverviewMark> overview_marks(std::span<const IffMinimapEntry> p_entries) {
	std::vector<OverviewMark> out;
	out.reserve(p_entries.size());
	for (const IffMinimapEntry &e : p_entries) {
		OverviewMark m;
		m.x = e.feet[0];
		m.z = e.feet[2];
		m.yaw = e.yaw;
		switch (e.relation) {
			case ui::TagRelation::Squad:
				m.kind = OverviewMarkKind::Squad;
				break;
			case ui::TagRelation::Enemy:
				m.kind = OverviewMarkKind::Enemy;
				break;
			default:
				m.kind = OverviewMarkKind::Friendly;
				break;
		}
		out.push_back(m);
	}
	return out;
}

Bf3OverviewMap::Bf3OverviewMap() = default;

Bf3OverviewMap::~Bf3OverviewMap() {
	if (g_current == this) {
		g_current = nullptr;
	}
	for (Attached &a : attached_) {
		for (Target &t : a.targets) {
			free_target(t);
		}
	}
}

Bf3OverviewMap *Bf3OverviewMap::current() {
	return g_current;
}

Bf3OverviewMap *Bf3OverviewMap::ensure(Node *p_parent) {
	if (g_current != nullptr) {
		return g_current;
	}
	if (p_parent == nullptr) {
		return nullptr;
	}
	Bf3OverviewMap *m = memnew(Bf3OverviewMap);
	m->set_name("OverviewMap");
	p_parent->add_child(m);
	return m;
}

void Bf3OverviewMap::_bind_methods() {
}

void Bf3OverviewMap::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE && g_current == nullptr) {
		g_current = this;
	} else if (p_what == NOTIFICATION_EXIT_TREE && g_current == this) {
		g_current = nullptr;
	}
}

void Bf3OverviewMap::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	set_process(true);
}

void Bf3OverviewMap::free_target(Target &r_target) {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		return;
	}
	for (RID *r : { &r_target.map_item, &r_target.icon_item }) {
		if (r->is_valid()) {
			rs->free_rid(*r);
			*r = RID();
		}
	}
}

bf3::core::Status Bf3OverviewMap::attach(Bf3SwfView *p_view) {
	if (p_view == nullptr || p_view->scene() == nullptr) {
		return bf3::core::make_error("overview map: no open view");
	}
	const ui::SwfScene &scene = *p_view->scene();
	std::vector<std::string> paths;
	for (const ui::SwfNodeId id : scene.draw_order()) {
		const ui::SwfSceneNode *n = scene.node(id);
		if (n != nullptr && is_map_clip(scene, *n)) {
			paths.push_back(n->path);
		}
	}
	if (paths.empty()) {
		return bf3::core::make_error("overview map: no " + std::string(kOverviewMapSymbol) + "." + std::string(kOverviewMapClip) + " clip in " + scene.movie_name());
	}
	for (const std::string &p : paths) {
		if (bf3::core::Status st = attach(p_view, p); !st) {
			return st;
		}
	}
	return bf3::core::Status::success();
}

bf3::core::Status Bf3OverviewMap::attach(Bf3SwfView *p_view, std::string_view p_map_path) {
	if (p_view == nullptr || p_view->scene() == nullptr) {
		return bf3::core::make_error("overview map: no open view");
	}
	if (bf3::core::Status st = p_view->set_renderer_string(p_map_path, kOverviewMapRenderer); !st) {
		return st;
	}
	const ObjectID id = ObjectID(p_view->get_instance_id());
	auto it = std::find_if(attached_.begin(), attached_.end(), [&](const Attached &a) { return a.view == id; });
	if (it == attached_.end()) {
		attached_.push_back(Attached{ id, {}, {} });
		it = attached_.end() - 1;
	}
	if (std::find(it->paths.begin(), it->paths.end(), p_map_path) == it->paths.end()) {
		it->paths.emplace_back(p_map_path);
		stats_.attached++;
	}
	return bf3::core::Status::success();
}

void Bf3OverviewMap::detach(Bf3SwfView *p_view) {
	if (p_view == nullptr) {
		return;
	}
	const ObjectID id = ObjectID(p_view->get_instance_id());
	for (auto it = attached_.begin(); it != attached_.end(); ++it) {
		if (it->view == id) {
			for (Target &t : it->targets) {
				free_target(t);
			}
			attached_.erase(it);
			return;
		}
	}
}

bool Bf3OverviewMap::source_ready() {
	if (source_ == nullptr && is_inside_tree()) {
		const TypedArray<Node> found = get_tree()->get_root()->find_children("*", "Bf3MinimapHud", true, false);
		if (!found.is_empty()) {
			source_ = Object::cast_to<Bf3MinimapHud>(found[0]);
		}
	}
	if (source_ == nullptr || !source_->ready_to_draw() || source_->satellite_texture().is_null() || source_->assets() == nullptr) {
		return false;
	}
	if (material_.is_null()) {
		std::shared_ptr<const MinimapAssets> a = source_->assets();
		Ref<Shader> shader;
		shader.instantiate();
		shader->set_code(String::utf8(kOverviewShader.data(), int64_t(kOverviewShader.size())));
		material_.instantiate();
		material_->set_shader(shader);
		const ui::MinimapData &d = a->level.data;
		material_->set_shader_parameter("satellite", source_->satellite_texture());
		material_->set_shader_parameter("combat_area", source_->combat_area_texture());
		material_->set_shader_parameter("detail", source_->detail_texture());
		material_->set_shader_parameter("has_detail", source_->has_detail());
		material_->set_shader_parameter("hatch", source_->hatch_texture());
		material_->set_shader_parameter("has_hatch", source_->hatch_texture().is_valid());
		material_->set_shader_parameter("tint", Color(d.detail_texture_tint[0], d.detail_texture_tint[1], d.detail_texture_tint[2], d.detail_texture_tint[3]));
		material_->set_shader_parameter("area_color", Color(d.combat_area_color[0], d.combat_area_color[1], d.combat_area_color[2], d.combat_area_color[3]));
		material_->set_shader_parameter("threshold", d.combat_area_alpha_threshold);
		material_->set_shader_parameter("detail_tint", Color(d.detail.color_tint[0], d.detail.color_tint[1], d.detail.color_tint[2], d.detail.color_tint[3]));
		material_->set_shader_parameter("detail_threshold", d.detail.alpha_threshold);
		material_->set_shader_parameter("opacity", a->level.profile.texture_opacity);
		material_->set_shader_parameter("satellite_weight", ui::kMinimapSatelliteWeight);
		material_->set_shader_parameter("world_center", Vector2(d.world_center[0], d.world_center[1]));
		material_->set_shader_parameter("world_size", d.world_size);
		material_->set_shader_parameter("hatch_wrap", Vector2(d.combat_area_multiply_wrap[0], d.combat_area_multiply_wrap[1]));
	}
	return true;
}

std::optional<ui::MinimapWorldRect> Bf3OverviewMap::area() {
	if (area_tried_ || !source_ready()) {
		return area_;
	}
	area_tried_ = true;
	const Ref<ImageTexture> field = source_->combat_area_texture();
	std::shared_ptr<const MinimapAssets> a = source_->assets();
	Ref<Image> img = field.is_valid() ? field->get_image() : Ref<Image>();
	if (img.is_null() || img->is_empty()) {
		return area_;
	}
	if (img->get_format() != Image::FORMAT_R8 && img->get_format() != Image::FORMAT_L8) {
		img->convert(Image::FORMAT_R8);
	}
	ui::L8Image l8;
	l8.width = uint32_t(img->get_width());
	l8.height = uint32_t(img->get_height());
	const PackedByteArray bytes = img->get_data();
	l8.texels.assign(bytes.ptr(), bytes.ptr() + bytes.size());
	area_ = ui::combat_area_bounds(l8, a->level.data.combat_area_alpha_threshold, ui::minimap_mapping(a->level.data));
	if (area_) {
		stats_.area = *area_;
	}
	return area_;
}

void Bf3OverviewMap::paint_target(const SwfEngineTarget &p_target, const ui::SwfScene &p_scene, Target &r_target) {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (r_target.parent != p_target.canvas_item) {
		free_target(r_target);
		r_target.map_item = rs->canvas_item_create();
		r_target.icon_item = rs->canvas_item_create();
		rs->canvas_item_set_parent(r_target.map_item, p_target.canvas_item);
		rs->canvas_item_set_parent(r_target.icon_item, p_target.canvas_item);
		rs->canvas_item_set_draw_index(r_target.map_item, 0);
		rs->canvas_item_set_draw_index(r_target.icon_item, 1);
		r_target.material = material_->duplicate();
		rs->canvas_item_set_material(r_target.map_item, r_target.material->get_rid());
		r_target.parent = p_target.canvas_item;
	}
	const std::optional<ui::MinimapWorldRect> world = area();
	if (!world) {
		return;
	}
	const ui::SwfRect &b = p_target.local_bounds;
	const ui::MinimapView view = ui::minimap_overview_view(*world, ui::kMinimapOverviewMargin, b.x_min, b.y_min, b.width(), b.height());
	if (!(view.pixels_per_metre > 0.0f)) {
		return;
	}
	r_target.material->set_shader_parameter("local_rect", Vector4(b.x_min, b.y_min, b.width(), b.height()));
	r_target.material->set_shader_parameter("centre_local", Vector2(view.center_x, view.center_y));
	r_target.material->set_shader_parameter("area_centre", Vector2(view.player_x, view.player_z));
	r_target.material->set_shader_parameter("metres_per_unit", 1.0f / view.pixels_per_metre);
	rs->canvas_item_clear(r_target.map_item);
	rs->canvas_item_add_rect(r_target.map_item, Rect2(b.x_min, b.y_min, b.width(), b.height()), Color(1, 1, 1, 1));
	stats_.maps++;
	stats_.pixels_per_metre = view.pixels_per_metre;
	stats_.local_bounds = b;

	rs->canvas_item_clear(r_target.icon_item);
	stats_.icons = 0;
	stats_.skulls = 0;
	stats_.objectives = 0;
	std::shared_ptr<const MinimapAssets> a = source_->assets();
	const Ref<ImageTexture> atlas = source_->icon_texture();
	if (a == nullptr || atlas.is_null()) {
		return;
	}
	const ui::SwfSceneNode *map = p_scene.node(p_target.node);
	const ui::SwfAffine m = map != nullptr ? ui::swf_affine(map->world) : ui::SwfAffine{};
	const float stage_per_local = std::max(1e-6f, float(std::sqrt(m.a * m.a + m.b * m.b)));
	const float unit = 1.0f / stage_per_local;
	const auto inside = [&](const ui::ScreenPoint &p) {
		return std::isfinite(p.x) && std::isfinite(p.y) && p.x >= b.x_min && p.y >= b.y_min && p.x <= b.x_max && p.y <= b.y_max;
	};
	const auto draw = [&](const ui::HudIcon *p_icon, ui::HudIconState p_state, const ui::ScreenPoint &p_at, float p_yaw) -> bool {
		const ui::HudIconStateFrames *st = p_icon != nullptr ? p_icon->state(p_state) : nullptr;
		if (st == nullptr && p_icon != nullptr) {
			st = p_icon->state(ui::HudIconState::Default);
		}
		if (st == nullptr || st->frames.empty()) {
			return false;
		}
		const ui::HudIconRect r = ui::hud_icon_rect(st->frames.front(), uint32_t(atlas->get_width()), uint32_t(atlas->get_height()));
		if (r.width <= 0 || r.height <= 0) {
			return false;
		}
		const float w = float(r.width) * unit;
		const float h = float(r.height) * unit;
		const float angle = st->should_rotate ? -p_yaw : 0.0f;
		rs->canvas_item_add_set_transform(r_target.icon_item, Transform2D(angle, Vector2(p_at.x, p_at.y)));
		rs->canvas_item_add_texture_rect_region(r_target.icon_item, Rect2(r.flip_x ? w * 0.5f : -w * 0.5f, r.flip_y ? h * 0.5f : -h * 0.5f, r.flip_x ? -w : w, r.flip_y ? -h : h),
				atlas->get_rid(), Rect2(float(r.x), float(r.y), float(r.width), float(r.height)));
		stats_.icons++;
		return true;
	};
	for (const MinimapIconDraw &ic : source_->icon_draws()) {
		if (!ic.objective) {
			continue;
		}
		const ui::ScreenPoint p = ui::minimap_project(view, ic.world_x, ic.world_z);
		if (inside(p) && draw(ic.icon, ic.state, p, ic.yaw)) {
			stats_.objectives++;
		}
	}
	const OverviewMarkKind order[] = { OverviewMarkKind::Dead, OverviewMarkKind::Enemy, OverviewMarkKind::Friendly, OverviewMarkKind::Squad,
		OverviewMarkKind::Local };
	for (const OverviewMarkKind kind : order) {
		for (const OverviewMark &mk : marks_) {
			if (mk.kind != kind) {
				continue;
			}
			const ui::ScreenPoint p = ui::minimap_project(view, mk.x, mk.z);
			const ui::HudIconChoice c = mark_icon(mk.kind);
			if (inside(p) && draw(a->atlas.find(c.icon), c.state, p, mk.yaw) && kind == OverviewMarkKind::Dead) {
				stats_.skulls++;
			}
		}
	}
	rs->canvas_item_add_set_transform(r_target.icon_item, Transform2D());
	stats_.paints++;
}

void Bf3OverviewMap::paint(Bf3SwfView &r_view, Attached &r_attached) {
	const ui::SwfScene *scene = r_view.scene();
	if (scene == nullptr) {
		return;
	}
	const std::vector<SwfEngineTarget> targets = r_view.engine_targets(kOverviewMapRenderer);
	if (r_attached.targets.size() < targets.size()) {
		r_attached.targets.resize(targets.size());
	}
	for (size_t i = 0; i < targets.size(); ++i) {
		const SwfEngineTarget &t = targets[i];
		if (!t.canvas_item.is_valid()) {
			continue;
		}
		paint_target(t, *scene, r_attached.targets[i]);
		if (!t.live) {
			if (const bf3::core::Status st = r_view.set_engine_target_live(t.path, true); !st) {
				UtilityFunctions::push_warning(to_godot("[overview map] " + st.error().message));
			}
		}
	}
}

void Bf3OverviewMap::_process(double) {
	if (attached_.empty() || !source_ready()) {
		return;
	}
	for (auto it = attached_.begin(); it != attached_.end();) {
		Bf3SwfView *v = Object::cast_to<Bf3SwfView>(ObjectDB::get_instance(uint64_t(it->view)));
		if (v == nullptr) {
			for (Target &t : it->targets) {
				free_target(t);
			}
			it = attached_.erase(it);
			continue;
		}
		paint(*v, *it);
		++it;
	}
}

}
