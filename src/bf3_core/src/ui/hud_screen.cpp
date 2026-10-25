#include "bf3/core/ui/hud_screen.h"

#include "ui_ebx.h"

#include "bf3/core/util/text.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace bf3::core::ui {

namespace {

constexpr uint8_t kAvm1Push = 0x96;
constexpr uint8_t kAvm1SetVariable = 0x1D;
constexpr uint8_t kAvm1ConstantPool = 0x88;

HudAxisAlign axis_of(std::string_view p_enum) {
	if (p_enum.ends_with("_Left") || p_enum.ends_with("_Top")) {
		return HudAxisAlign::Near;
	}
	if (p_enum.ends_with("_Right") || p_enum.ends_with("_Bottom")) {
		return HudAxisAlign::Far;
	}
	return HudAxisAlign::Centre;
}

std::string text_of(const std::optional<EbxValue> &p_value) {
	if (!p_value) {
		return {};
	}
	if (const std::optional<std::string_view> e = p_value->enum_name()) {
		return std::string(*e);
	}
	return std::string(p_value->as_str().value_or(std::string_view()));
}

As2Value value_of(const Avm1Value &p_v) {
	switch (p_v.type) {
		case Avm1PushType::Boolean: return As2Value::of(p_v.index != 0);
		case Avm1PushType::Float:
		case Avm1PushType::Double:
		case Avm1PushType::Integer: return As2Value::of(p_v.number);
		case Avm1PushType::String: return As2Value::of(p_v.text);
		case Avm1PushType::Constant8:
		case Avm1PushType::Constant16: return p_v.resolved ? As2Value::of(p_v.text) : As2Value::undefined();
		default: return As2Value::undefined();
	}
}

HudDataSource source_of(const EbxResolver &p_db, const EbxPartition &p_part, const EbxStruct &p_info) {
	HudDataSource out;
	out.name = text_of(p_info.get("DataName"));
	const std::optional<EbxValue> key = p_info.get("DataKey");
	out.key = key ? key->as_i32().value_or(0) : 0;
	if (const std::optional<EbxRef> c = detail::ref(p_info, "DataCategory")) {
		out.category = detail::ref_partition(p_db, p_part, *c).value_or(std::string());
	}
	if (const std::optional<EbxInstance> category = p_db.follow(p_info, "DataCategory")) {
		out.category_short_name = text_of(category->get("ShortName"));
	}
	out.on_initialize = detail::flag_or(p_info, "UpdateOnInitialize", true);
	return out;
}

void read_binding(const EbxResolver &p_db, const EbxPartition &p_part, const EbxInstance &p_node, HudWidgetNode &r_widget) {
	const std::optional<EbxInstance> b = p_db.follow(p_node.data(), "DataBinding");
	if (!b) {
		return;
	}
	if (b->is_a("UIHUDMessageBinding")) {
		if (const std::optional<EbxStruct> q = detail::sub(b->data(), "MessageQuery")) {
			r_widget.message_query = source_of(p_db, p_part, *q);
		}
		const std::optional<EbxValue> rows = b->get("NumberOfRows");
		r_widget.number_of_rows = rows ? rows->as_i32().value_or(0) : 0;
		return;
	}
	if (b->is_a("UICrosshairDataBinding")) {
		if (const std::optional<EbxValue> offset = b->get("DispersionBaseOffset")) {
			r_widget.dispersion_base_offset = offset->as_f32();
		}
		return;
	}
	if (b->is_a("UIWidgetPathBinding")) {
		if (const std::optional<EbxStruct> q = detail::sub(b->data(), "WidgetPathQuery")) {
			r_widget.widget_path_query = source_of(p_db, p_part, *q);
		}
		return;
	}
	const std::optional<EbxValue> list = b->get("Bindings");
	const OptionalEbxArray arr = list ? list->as_array() : OptionalEbxArray();
	if (!arr) {
		return;
	}
	for (const EbxValue &v : *arr) {
		if (const std::optional<EbxStruct> e = v.as_struct()) {
			r_widget.sources.push_back(source_of(p_db, p_part, *e));
		}
	}
}

float tween_value(const As2Clip &p_clip, HudTweenProperty p_property) {
	switch (p_property) {
		case HudTweenProperty::Alpha: return p_clip.alpha();
		case HudTweenProperty::Width: return p_clip.width();
		case HudTweenProperty::Height: return p_clip.height();
		case HudTweenProperty::X: return p_clip.x();
		case HudTweenProperty::Y: return p_clip.y();
	}
	return 0.0f;
}

}

const HudDataSource *HudWidgetNode::source(std::string_view p_name) const {
	for (const HudDataSource &d : sources) {
		if (d.name == p_name) {
			return &d;
		}
	}
	return nullptr;
}

bool HudFrameScript::calls(std::string_view p_name) const {
	return names != nullptr && std::find(names->begin(), names->end(), p_name) != names->end();
}

const HudWidgetNode *HudScreenAsset::find(std::string_view p_instance) const {
	for (const HudWidgetNode &w : widgets) {
		if (w.instance == p_instance) {
			return &w;
		}
	}
	return nullptr;
}

std::string hud_screen_movie(std::string_view p_screen_name) {
	const size_t slash = p_screen_name.rfind('/');
	const std::string leaf = to_lower_ascii(slash == std::string_view::npos ? p_screen_name : p_screen_name.substr(slash + 1));
	return leaf.empty() ? std::string() : "ui/assets/" + leaf;
}

Result<HudScreenAsset> read_hud_screen_asset(const EbxResolver &p_db, std::string_view p_partition) {
	const EbxPartition *part = p_db.partition_by_name(p_partition);
	if (part == nullptr) {
		return make_error("hud screen: no partition " + std::string(p_partition));
	}
	const EbxInstance root = part->primary();
	if (!root.is_a("UIScreenAsset")) {
		return make_error("hud screen: " + std::string(p_partition) + " is a " + std::string(root.type_name()) + ", not a UIScreenAsset");
	}
	HudScreenAsset out;
	out.partition = std::string(p_partition);
	out.name = text_of(root.get("Name"));
	out.movie = hud_screen_movie(out.name);
	for (const EbxInstance &n : p_db.follow_all(root.data(), "Nodes")) {
		if (!n.is_a("WidgetNode")) {
			continue;
		}
		if (out.widgets.size() >= kHudMaxScreenWidgets) {
			return make_error("hud screen: " + std::string(p_partition) + " has more than " + std::to_string(kHudMaxScreenWidgets) + " widgets");
		}
		HudWidgetNode w;
		w.instance = text_of(n.get("InstanceName"));
		w.name = text_of(n.get("Name"));
		w.horizontal = axis_of(text_of(n.get("HorisontalAlign")));
		w.vertical = axis_of(text_of(n.get("VerticalAlign")));
		const std::optional<EbxValue> z = n.get("ZDepthLevel");
		w.z_depth_level = z ? z->as_i32().value_or(0) : 0;
		if (const std::optional<EbxRef> asset = detail::ref(n.data(), "WidgetAsset")) {
			w.widget_asset = detail::ref_partition(p_db, *part, *asset).value_or(std::string());
		}
		if (const std::optional<EbxValue> props = n.get("WidgetProperties")) {
			if (const OptionalEbxArray arr = props->as_array()) {
				if (arr->size() > kHudMaxWidgetProperties) {
					return make_error("hud screen: widget " + w.instance + " has " + std::to_string(arr->size()) + " properties");
				}
				for (const EbxValue &v : *arr) {
					const std::optional<EbxStruct> s = v.as_struct();
					if (!s) {
						continue;
					}
					w.properties.push_back(HudWidgetProperty{ text_of(s->get("Name")), text_of(s->get("Value")) });
				}
			}
		}
		read_binding(p_db, *part, n, w);
		if (w.instance.empty()) {
			return make_error("hud screen: " + std::string(p_partition) + " has a widget node without an instance name");
		}
		out.widgets.push_back(std::move(w));
	}
	return out;
}

As2Value As2Value::of(bool p_b) {
	As2Value v;
	v.kind = Kind::Bool;
	v.boolean = p_b;
	return v;
}

As2Value As2Value::of(double p_n) {
	As2Value v;
	v.kind = Kind::Number;
	v.number = p_n;
	return v;
}

As2Value As2Value::of(std::string p_s) {
	As2Value v;
	v.kind = Kind::String;
	v.text = std::move(p_s);
	return v;
}

As2Value As2Value::numbers(std::vector<double> p_list) {
	As2Value v;
	v.kind = Kind::NumberList;
	v.list = std::move(p_list);
	return v;
}

As2Value As2Value::object() {
	As2Value v;
	v.kind = Kind::Object;
	return v;
}

As2Value As2Value::array(std::vector<As2Value> p_items) {
	As2Value v;
	v.kind = Kind::List;
	v.items = std::move(p_items);
	return v;
}

As2Value &As2Value::set(std::string_view p_name, As2Value p_value) {
	if (kind != Kind::Object) {
		*this = object();
	}
	for (size_t i = 0; i < names.size(); ++i) {
		if (names[i] == p_name) {
			items[i] = std::move(p_value);
			return *this;
		}
	}
	names.emplace_back(p_name);
	items.push_back(std::move(p_value));
	return *this;
}

const As2Value &As2Value::field(std::string_view p_name) const {
	static const As2Value kUndefined;
	if (kind == Kind::Object) {
		for (size_t i = 0; i < names.size(); ++i) {
			if (names[i] == p_name) {
				return items[i];
			}
		}
	}
	return kUndefined;
}

As2Value &As2Value::push(As2Value p_value) {
	if (kind != Kind::List) {
		*this = array();
	}
	items.push_back(std::move(p_value));
	return *this;
}

const As2Value &As2Value::at(size_t p_index) const {
	static const As2Value kUndefined;
	return kind == Kind::List && p_index < items.size() ? items[p_index] : kUndefined;
}

bool As2Value::truthy() const {
	switch (kind) {
		case Kind::Undefined: return false;
		case Kind::Bool: return boolean;
		case Kind::Number: return number != 0.0 && !std::isnan(number);
		case Kind::String: return !text.empty();
		case Kind::NumberList:
		case Kind::Object:
		case Kind::List: return true;
	}
	return false;
}

double As2Value::to_number() const {
	switch (kind) {
		case Kind::Undefined: return std::nan("");
		case Kind::Bool: return boolean ? 1.0 : 0.0;
		case Kind::Number: return number;
		case Kind::String: {
			if (text.empty()) {
				return std::nan("");
			}
			char *end = nullptr;
			const double d = std::strtod(text.c_str(), &end);
			return end != nullptr && *end == '\0' ? d : std::nan("");
		}
		case Kind::NumberList:
		case Kind::Object:
		case Kind::List: return std::nan("");
	}
	return std::nan("");
}

std::string As2Value::to_string() const {
	switch (kind) {
		case Kind::Undefined: return "undefined";
		case Kind::Bool: return boolean ? "true" : "false";
		case Kind::Number: return as2_number_string(number);
		case Kind::String: return text;
		case Kind::NumberList: {
			std::string s;
			for (size_t i = 0; i < list.size(); ++i) {
				s += (i == 0 ? "" : ",") + as2_number_string(list[i]);
			}
			return s;
		}
		case Kind::Object: return "[object Object]";
		case Kind::List: {
			std::string s;
			for (size_t i = 0; i < items.size(); ++i) {
				s += (i == 0 ? "" : ",") + (items[i].defined() ? items[i].to_string() : std::string());
			}
			return s;
		}
	}
	return {};
}

const As2Value *hud_data_find(const HudData &p_data, std::string_view p_key) {
	for (const HudDataEntry &e : p_data) {
		if (e.key == p_key) {
			return &e.value;
		}
	}
	return nullptr;
}

const std::string *HudWidgetInit::property(std::string_view p_name) const {
	for (const HudWidgetProperty &p : properties) {
		if (p.name == p_name) {
			return &p.value;
		}
	}
	return nullptr;
}

float hud_align_axis(HudAxisAlign p_align, float p_original, float p_visible_min, float p_visible_size, float p_original_size, float p_safe) {
	switch (p_align) {
		case HudAxisAlign::Near: return std::round(p_visible_min + p_safe + p_original);
		case HudAxisAlign::Far: return std::round(p_visible_min + p_visible_size - p_safe - (p_original_size - p_original));
		case HudAxisAlign::Centre: break;
	}
	return p_original;
}

double hud_tween_ease(HudTweenEase p_ease, double p_t) {
	const double t = std::clamp(p_t, 0.0, 1.0);
	switch (p_ease) {
		case HudTweenEase::Linear: return t;
		case HudTweenEase::EaseOutBack: {
			const double s = 1.70158;
			const double u = t - 1.0;
			return u * u * ((s + 1.0) * u + s) + 1.0;
		}
		case HudTweenEase::ExpoOut: return t == 1.0 ? 1.0 : 1.001 * (1.0 - std::pow(2.0, -10.0 * t));
	}
	return t;
}

HudWidgetHost::HudWidgetHost(HudScreen &r_screen, std::string p_path, std::string p_class) :
		screen_(r_screen), path_(std::move(p_path)), class_(std::move(p_class)), self_(r_screen.clip(path_)) {}

As2Clip HudWidgetHost::clip(std::string_view p_relative) const {
	return p_relative.empty() ? self_ : screen_.clip(path_ + "." + std::string(p_relative));
}

const As2Value *HudWidgetHost::construct_param(std::string_view p_name) const {
	const auto it = params_.find(p_name);
	return it != params_.end() ? &it->second : nullptr;
}

void HudWidgetHost::set_construct_param(std::string p_name, As2Value p_value) {
	params_[std::move(p_name)] = std::move(p_value);
}

void HudWidgetHost::tilt(const As2Clip &p_clip, float p_x_rotation, float p_y_rotation, float p_perspective_fov) {
	HudTilt t{ p_clip.path(), p_x_rotation, p_y_rotation, p_perspective_fov };
	for (HudTilt &e : screen_.tilts_) {
		if (e.path == t.path) {
			if (!(e == t)) {
				e = t;
				screen_.effects_revision_++;
			}
			return;
		}
	}
	screen_.tilts_.push_back(std::move(t));
	screen_.effects_revision_++;
}

void HudWidgetHost::renderer(const As2Clip &p_clip, std::string_view p_renderer) {
	for (HudRendererString &e : screen_.renderers_) {
		if (e.path == p_clip.path()) {
			if (e.renderer != p_renderer) {
				e.renderer = std::string(p_renderer);
				screen_.effects_revision_++;
			}
			return;
		}
	}
	screen_.renderers_.push_back(HudRendererString{ p_clip.path(), std::string(p_renderer) });
	screen_.effects_revision_++;
}

void HudWidgetHost::add_tween(const As2Clip &p_clip, HudTweenProperty p_property, float p_to, double p_seconds, HudTweenEase p_ease, uint32_t p_on_complete) {
	std::vector<HudTween> &list = screen_.tweens_;
	list.erase(std::remove_if(list.begin(), list.end(), [&](const HudTween &t) { return t.path == p_clip.path() && t.property == p_property; }), list.end());
	HudTween t;
	t.path = p_clip.path();
	t.property = p_property;
	t.ease = p_ease;
	t.to = p_to;
	t.seconds = p_seconds;
	t.on_complete = p_on_complete;
	if (!(p_seconds > 0.0)) {
		screen_.apply_tween(t, p_to);
		return;
	}
	list.push_back(std::move(t));
}

As2Clip HudWidgetHost::attach_movie(const As2Clip &p_parent, std::string_view p_linkage, std::string_view p_name, int32_t p_depth) {
	const As2Clip made = p_parent.attach_movie(p_linkage, p_name, p_depth);
	if (const SwfSceneNode *n = made.node()) {
		screen_.construct_subtree(n->id);
	}
	return made;
}

As2Clip HudWidgetHost::create_empty_movie_clip(const As2Clip &p_parent, std::string_view p_name, int32_t p_depth) {
	return p_parent.create_empty_movie_clip(p_name, p_depth);
}

void HudWidgetHost::remove_movie_clip(const As2Clip &p_clip) {
	const SwfSceneNode *n = p_clip.node();
	if (n != nullptr && n->dynamic) {
		screen_.retire(p_clip.path());
	}
	p_clip.remove_movie_clip();
}

void HudWidgetHost::remove_tweens(const As2Clip &p_clip) {
	std::vector<HudTween> &list = screen_.tweens_;
	list.erase(std::remove_if(list.begin(), list.end(), [&](const HudTween &t) { return t.path == p_clip.path(); }), list.end());
}

std::string HudWidgetHost::translate(std::string_view p_text) const {
	const HudClientState &c = screen_.client();
	if (p_text.starts_with("ID_") && c.translate) {
		if (std::optional<std::string> t = c.translate(p_text)) {
			return *t;
		}
	}
	return std::string(p_text);
}

const HudClientState &HudWidgetHost::client() const {
	return screen_.client();
}

HudScreen::~HudScreen() {
	if (scene_ != nullptr) {
		scene_->set_frame_script_handler({});
	}
}

void HudScreen::retire(std::string_view p_path) {
	const std::string prefix = std::string(p_path) + ".";
	for (const std::unique_ptr<HudWidgetState> &w : widgets_) {
		if (w->path == p_path || w->path.starts_with(prefix)) {
			w->retired = true;
			original_positions_.erase(w->path);
		}
	}
	tweens_.erase(std::remove_if(tweens_.begin(), tweens_.end(), [&](const HudTween &t) { return t.path == p_path || t.path.starts_with(prefix); }),
			tweens_.end());
}

void HudScreen::sweep() {
	widgets_.erase(std::remove_if(widgets_.begin(), widgets_.end(), [](const std::unique_ptr<HudWidgetState> &w) { return w->retired; }), widgets_.end());
}

const Avm1Movie &HudScreen::avm1_of(const SwfSceneNode &p_node) {
	auto it = avm1_.find(p_node.movie_name);
	if (it == avm1_.end()) {
		Result<Avm1Movie> read = read_avm1(*p_node.movie);
		std::shared_ptr<const Avm1Movie> m = read ? std::make_shared<const Avm1Movie>(std::move(*read)) : std::make_shared<const Avm1Movie>();
		it = avm1_.emplace(p_node.movie_name, std::move(m)).first;
	}
	return *it->second;
}

const std::vector<std::string> &HudScreen::frame_names(const SwfSceneNode &p_node, uint32_t p_frame) {
	const uint16_t timeline = p_node.kind == SwfNodeKind::Root ? 0 : p_node.character;
	const std::string key = p_node.movie_name + "#" + std::to_string(timeline) + "@" + std::to_string(p_frame);
	auto it = frame_names_.find(key);
	if (it != frame_names_.end()) {
		return it->second;
	}
	std::vector<std::string> names;
	if (p_node.movie) {
		for (const Avm1Body &b : avm1_of(p_node).bodies) {
			if (b.kind != Avm1BodyKind::DoAction || b.timeline != timeline || b.frame != p_frame) {
				continue;
			}
			for (const Avm1Action &a : b.code.actions) {
				if (a.op != kAvm1Push) {
					continue;
				}
				for (const Avm1Value &v : a.values) {
					const As2Value s = value_of(v);
					if (s.kind == As2Value::Kind::String) {
						names.push_back(s.text);
					}
				}
			}
		}
	}
	return frame_names_.emplace(key, std::move(names)).first->second;
}

void HudScreen::on_frame_script(SwfNodeId p_node, uint32_t p_frame) {
	const SwfSceneNode *n = scene_->node(p_node);
	if (n == nullptr) {
		return;
	}
	HudFrameScript fs;
	fs.path = n->path;
	fs.frame = p_frame;
	fs.names = &frame_names(*n, p_frame);
	HudWidgetState *owner = nullptr;
	for (const std::unique_ptr<HudWidgetState> &w : widgets_) {
		if (w->retired || !(fs.path == w->path || fs.path.starts_with(w->path + "."))) {
			continue;
		}
		if (owner == nullptr || w->path.size() > owner->path.size()) {
			owner = w.get();
		}
	}
	if (owner != nullptr) {
		owner->controller->frame_script(*owner->host, fs);
	}
}

As2Clip HudScreen::clip(std::string_view p_path) {
	return As2Clip(scene_, std::string(p_path), &stats_.as2, fonts_);
}

const HudWidgetState *HudScreen::widget(std::string_view p_path) const {
	for (const std::unique_ptr<HudWidgetState> &w : widgets_) {
		if (w->path == p_path && !w->retired) {
			return w.get();
		}
	}
	return nullptr;
}

HudWidgetController *HudScreen::controller(std::string_view p_path) {
	const HudWidgetState *w = widget(p_path);
	return w != nullptr ? w->controller.get() : nullptr;
}

HudWidgetHost *HudScreen::host(std::string_view p_path) {
	const HudWidgetState *w = widget(p_path);
	return w != nullptr ? w->host.get() : nullptr;
}

std::optional<std::string> HudScreen::class_of(const SwfSceneNode &p_node) {
	if (p_node.kind != SwfNodeKind::Sprite || !p_node.movie) {
		return std::nullopt;
	}
	auto it = avm1_.find(p_node.movie_name);
	if (it == avm1_.end()) {
		Result<Avm1Movie> read = read_avm1(*p_node.movie);
		std::shared_ptr<const Avm1Movie> m = read ? std::make_shared<const Avm1Movie>(std::move(*read)) : std::make_shared<const Avm1Movie>();
		it = avm1_.emplace(p_node.movie_name, std::move(m)).first;
	}
	for (const Avm1Registration &r : it->second->registrations) {
		if (r.linkage == p_node.symbol) {
			return r.class_name;
		}
	}
	return std::nullopt;
}

std::vector<std::pair<std::string, As2Value>> HudScreen::construct_params_of(const SwfSceneNode &p_node) {
	std::vector<std::pair<std::string, As2Value>> out;
	const SwfSceneNode *parent = scene_->node(p_node.parent);
	if (parent == nullptr || !parent->movie || p_node.dynamic) {
		return out;
	}
	auto it = avm1_.find(parent->movie_name);
	if (it == avm1_.end()) {
		Result<Avm1Movie> read = read_avm1(*parent->movie);
		std::shared_ptr<const Avm1Movie> m = read ? std::make_shared<const Avm1Movie>(std::move(*read)) : std::make_shared<const Avm1Movie>();
		it = avm1_.emplace(parent->movie_name, std::move(m)).first;
	}
	const uint16_t timeline = parent->id == scene_->root() ? 0 : parent->character;
	for (const Avm1Body &b : it->second->bodies) {
		if (b.kind != Avm1BodyKind::ClipAction || (b.event_flags & avm1_clip_event::kConstruct) == 0 || b.timeline != timeline || b.sprite != p_node.placed_character || int32_t(b.depth) != p_node.depth) {
			continue;
		}
		std::vector<As2Value> stack;
		for (const Avm1Action &a : b.code.actions) {
			if (a.op == kAvm1Push) {
				for (const Avm1Value &v : a.values) {
					stack.push_back(value_of(v));
				}
			} else if (a.op == kAvm1SetVariable && stack.size() >= 2) {
				As2Value value = std::move(stack.back());
				stack.pop_back();
				As2Value name = std::move(stack.back());
				stack.pop_back();
				if (name.kind == As2Value::Kind::String) {
					out.emplace_back(name.text, std::move(value));
				}
			} else if (a.op != kAvm1ConstantPool) {
				stack.clear();
			}
		}
	}
	return out;
}

void HudScreen::discover(SwfNodeId p_root) {
	const SwfSceneNode *n = scene_->node(p_root);
	if (n == nullptr) {
		return;
	}
	std::vector<SwfNodeId> order;
	std::unordered_map<SwfNodeId, size_t> entered{ { p_root, 0 } };
	std::vector<std::pair<SwfNodeId, size_t>> stack{ { p_root, 0 } };
	while (!stack.empty()) {
		auto &[id, next] = stack.back();
		const SwfSceneNode *cur = scene_->node(id);
		if (cur == nullptr || next >= cur->children.size()) {
			order.push_back(id);
			stack.pop_back();
			continue;
		}
		const SwfNodeId c = cur->children[next++];
		entered.emplace(c, entered.size());
		stack.emplace_back(c, 0);
	}
	std::vector<std::pair<size_t, HudWidgetState *>> made;
	for (const SwfNodeId id : order) {
		const SwfSceneNode *node = scene_->node(id);
		if (node == nullptr || node->kind != SwfNodeKind::Sprite || widget(node->path) != nullptr) {
			continue;
		}
		const std::optional<std::string> cls = class_of(*node);
		if (!cls) {
			continue;
		}
		stats_.instances++;
		std::unique_ptr<HudWidgetController> ctl = make_hud_widget_controller(*cls);
		if (!ctl) {
			stats_.unknown_classes++;
			stats_.unknown_class_paths.push_back(node->path + " " + *cls);
			continue;
		}
		auto st = std::make_unique<HudWidgetState>();
		st->path = node->path;
		st->class_name = *cls;
		st->instance = node->name;
		st->controller = std::move(ctl);
		st->host = std::make_unique<HudWidgetHost>(*this, st->path, st->class_name);
		for (auto &[k, v] : construct_params_of(*node)) {
			st->host->set_construct_param(std::move(k), std::move(v));
		}
		original_positions_[st->path] = { node->matrix.tx, node->matrix.ty };
		stats_.controllers++;
		made.emplace_back(entered.at(id), st.get());
		widgets_.push_back(std::move(st));
	}
	std::vector<std::pair<size_t, HudWidgetState *>> parents_first = made;
	std::sort(parents_first.begin(), parents_first.end(), [](const auto &p_a, const auto &p_b) { return p_a.first < p_b.first; });
	for (const auto &[entry, w] : parents_first) {
		w->controller->construct(*w->host);
	}
	for (const auto &[entry, w] : made) {
		if (const SwfSceneNode *node = scene_->find_node(w->path)) {
			original_positions_[w->path] = { node->matrix.tx, node->matrix.ty };
		}
		w->controller->on_clip_load(*w->host);
	}
}

void HudScreen::construct_subtree(SwfNodeId p_root) {
	translate_texts(p_root);
	discover(p_root);
}

void HudScreen::translate_texts(SwfNodeId p_root) {
	if (!client_.translate) {
		return;
	}
	std::vector<SwfNodeId> stack{ p_root };
	while (!stack.empty()) {
		const SwfNodeId id = stack.back();
		stack.pop_back();
		const SwfSceneNode *n = scene_->node(id);
		if (n == nullptr) {
			continue;
		}
		if (n->kind == SwfNodeKind::EditText && n->text.starts_with("ID_")) {
			if (std::optional<std::string> t = client_.translate(n->text)) {
				const std::string path = n->path;
				(void)scene_->set_text(path, *t, false);
			}
		}
		for (const SwfNodeId c : n->children) {
			stack.push_back(c);
		}
	}
}

Result<std::unique_ptr<HudScreen>> HudScreen::open(SwfScene &r_scene, const SwfLibrary &p_library, const HudScreenAsset &p_asset, const SwfFontSet *p_fonts,
		HudClientState p_client, HudStageRects p_rects) {
	std::unique_ptr<HudScreen> s(new HudScreen());
	s->scene_ = &r_scene;
	s->library_ = &p_library;
	s->fonts_ = p_fonts;
	s->asset_ = p_asset;
	s->client_ = std::move(p_client);
	s->rects_ = p_rects;
	if (s->scene_->find_node(kHudScreenInstance) == nullptr) {
		return make_error("hud screen " + p_asset.name + ": movie " + r_scene.movie_name() + " has no " + std::string(kHudScreenInstance));
	}
	HudScreen *self = s.get();
	r_scene.set_frame_script_handler([self](SwfNodeId p_node, uint32_t p_frame) { self->on_frame_script(p_node, p_frame); });
	s->construct_subtree(r_scene.root());
	return s;
}

void HudScreen::set_client(HudClientState p_client) {
	client_ = std::move(p_client);
}

void HudScreen::set_stage_rects(const HudStageRects &p_rects) {
	const bool changed = p_rects.visible != rects_.visible || p_rects.original != rects_.original || p_rects.safe_x != rects_.safe_x ||
			p_rects.safe_y != rects_.safe_y;
	rects_ = p_rects;
	if (!changed || !initialized_) {
		return;
	}
	for (const HudWidgetNode &n : asset_.widgets) {
		for (const std::unique_ptr<HudWidgetState> &w : widgets_) {
			if (w->screen_widget && w->instance == n.instance) {
				align(*w, n);
			}
		}
	}
}

void HudScreen::align(HudWidgetState &r_widget, const HudWidgetNode &p_node) {
	const auto it = original_positions_.find(r_widget.path);
	if (it == original_positions_.end()) {
		return;
	}
	r_widget.controller->pre_update_screen_size(*r_widget.host);
	const As2Clip self = clip(r_widget.path);
	const SwfRect &v = rects_.visible;
	const SwfRect &o = rects_.original;
	const bool safe = r_widget.controller->aligns_in_safe_rect();
	if (p_node.horizontal != HudAxisAlign::Centre) {
		self.set_x(hud_align_axis(p_node.horizontal, it->second.first, v.x_min, v.width(), o.width(), safe ? rects_.safe_x : 0.0f));
	}
	if (p_node.vertical != HudAxisAlign::Centre) {
		self.set_y(hud_align_axis(p_node.vertical, it->second.second, v.y_min, v.height(), o.height(), safe ? rects_.safe_y : 0.0f));
	}
	r_widget.controller->update_screen_size(*r_widget.host);
}

void HudScreen::initialize(const HudModel &p_model) {
	if (initialized_) {
		return;
	}
	initialized_ = true;
	const std::string prefix = std::string(kHudScreenInstance) + ".";
	for (const HudWidgetNode &n : asset_.widgets) {
		HudWidgetState *w = nullptr;
		for (const std::unique_ptr<HudWidgetState> &cand : widgets_) {
			if (cand->path == prefix + n.instance) {
				w = cand.get();
			}
		}
		if (w == nullptr) {
			stats_.widgets_missing++;
			continue;
		}
		w->screen_widget = true;
		w->controller->attach_node(*w->host, n);
		if (!w->controller->screen_initialized()) {
			continue;
		}
		HudWidgetInit init;
		init.properties = n.properties;
		HudData all;
		w->controller->bind(p_model, client_, all);
		for (HudDataEntry &e : all) {
			if (e.on_initialize && e.value.defined()) {
				w->sent[e.key] = e.value;
				init.data.push_back(std::move(e));
			}
		}
		align(*w, n);
		w->controller->initialize(*w->host, init);
		w->initialized = true;
		stats_.widgets_initialized++;
	}
	for (const std::unique_ptr<HudWidgetState> &w : widgets_) {
		if (w->initialized) {
			w->controller->drive(*w->host, p_model, client_);
		}
	}
}

void HudScreen::refresh(const HudModel &p_model) {
	if (!initialized_) {
		initialize(p_model);
		return;
	}
	sweep();
	stats_.refreshes++;
	for (size_t i = 0; i < widgets_.size(); ++i) {
		HudWidgetState &w = *widgets_[i];
		if (!w.initialized || w.retired) {
			continue;
		}
		HudData data;
		w.controller->bind(p_model, client_, data);
		for (const HudDataEntry &e : data) {
			if (!e.value.defined()) {
				continue;
			}
			auto it = w.sent.find(e.key);
			if (it != w.sent.end() && it->second == e.value) {
				continue;
			}
			w.sent[e.key] = e.value;
			stats_.updates++;
			w.controller->update(*w.host, e.key, e.value);
		}
		w.controller->drive(*w.host, p_model, client_);
	}
}

void HudScreen::apply_tween(const HudTween &p_tween, float p_value) {
	const As2Clip c = clip(p_tween.path);
	switch (p_tween.property) {
		case HudTweenProperty::Alpha: c.set_alpha(p_value); break;
		case HudTweenProperty::Width: c.set_width(p_value); break;
		case HudTweenProperty::Height: c.set_height(p_value); break;
		case HudTweenProperty::X: c.set_x(p_value); break;
		case HudTweenProperty::Y: c.set_y(p_value); break;
	}
}

void HudScreen::set_focus(bool p_focused) {
	if (focused_ == p_focused) {
		return;
	}
	focused_ = p_focused;
	for (size_t i = 0; i < widgets_.size(); ++i) {
		HudWidgetState &w = *widgets_[i];
		if (w.retired || w.controller == nullptr || w.host == nullptr) {
			continue;
		}
		if (p_focused) {
			w.controller->focus_gained(*w.host);
		} else {
			w.controller->focus_lost(*w.host);
		}
	}
}

void HudScreen::advance(double p_seconds) {
	if (!std::isfinite(p_seconds) || p_seconds < 0.0) {
		return;
	}
	sweep();
	std::vector<std::pair<std::string, uint32_t>> completed;
	for (HudTween &t : tweens_) {
		if (!t.started) {
			t.started = true;
			const float v = tween_value(clip(t.path), t.property);
			t.from = std::isnan(v) ? t.to : v;
		}
		t.elapsed += p_seconds;
		const bool done = t.elapsed >= t.seconds;
		const double k = done ? 1.0 : hud_tween_ease(t.ease, t.elapsed / t.seconds);
		apply_tween(t, done ? t.to : float(double(t.from) + (double(t.to) - double(t.from)) * k));
		stats_.tweens_run++;
		if (done && t.on_complete != 0) {
			completed.emplace_back(t.path, t.on_complete);
		}
	}
	tweens_.erase(std::remove_if(tweens_.begin(), tweens_.end(), [](const HudTween &t) { return t.elapsed >= t.seconds; }), tweens_.end());
	for (const auto &[path, id] : completed) {
		for (size_t i = 0; i < widgets_.size(); ++i) {
			HudWidgetState &w = *widgets_[i];
			if (!w.retired && (path == w.path || path.starts_with(w.path + "."))) {
				w.controller->tween_complete(*w.host, id);
			}
		}
	}
	for (size_t i = 0; i < widgets_.size(); ++i) {
		if (widgets_[i]->retired) {
			continue;
		}
		if (widgets_[i]->initialized || !widgets_[i]->screen_widget) {
			widgets_[i]->controller->advance(*widgets_[i]->host, p_seconds);
		}
	}
}

}
