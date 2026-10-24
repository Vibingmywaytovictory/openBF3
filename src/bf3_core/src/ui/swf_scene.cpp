#include "bf3/core/ui/swf_scene.h"

#include "bf3/core/util/text.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <optional>
#include <string>

namespace bf3::core::ui {

namespace {

constexpr uint32_t kNoEvent = 0xffffffffu;
constexpr size_t kNodeUnits = 4;
constexpr uint32_t kSlotBits = 16;
constexpr uint32_t kSlotMask = (1u << kSlotBits) - 1;
constexpr uint32_t kMaxGeneration = 0xfffeu;
static_assert(kSwfSceneMaxNodes <= (1u << kSlotBits));

uint32_t slot_of(SwfNodeId p_id) {
	return p_id & kSlotMask;
}

struct Slot {
	uint32_t placed_by = kNoEvent;
	uint16_t character = 0;
	SwfMatrix matrix;
	SwfColorTransform color;
	uint16_t ratio = 0;
	std::string_view name;
	uint16_t clip_depth = 0;
	uint8_t blend_mode = 0;
	uint32_t filters_from = kNoEvent;
	bool cache_as_bitmap = false;
	bool visible = true;
	bool clip_actions = false;
};

void merge(Slot &r_slot, const SwfDisplayEvent &p_e, uint32_t p_index) {
	if (p_e.op == SwfDisplayOp::Place) {
		r_slot = Slot{};
		r_slot.placed_by = p_index;
		r_slot.character = p_e.character;
	} else if (p_e.op == SwfDisplayOp::Replace) {
		r_slot.character = p_e.character;
	}
	if (p_e.has_matrix) {
		r_slot.matrix = p_e.matrix;
	}
	if (p_e.has_color) {
		r_slot.color = p_e.color;
	}
	if (p_e.has_ratio) {
		r_slot.ratio = p_e.ratio;
	}
	if (p_e.has_name) {
		r_slot.name = p_e.name;
	}
	if (p_e.has_clip_depth) {
		r_slot.clip_depth = p_e.clip_depth;
	}
	if (p_e.has_blend_mode) {
		r_slot.blend_mode = p_e.blend_mode;
	}
	if (p_e.has_filters) {
		r_slot.filters_from = p_index;
	}
	if (p_e.has_cache_as_bitmap) {
		r_slot.cache_as_bitmap = p_e.cache_as_bitmap != 0;
	}
	if (p_e.has_visible) {
		r_slot.visible = p_e.visible != 0;
	}
	if (p_e.has_clip_actions) {
		r_slot.clip_actions = true;
	}
}

void apply_event(std::map<int32_t, Slot> &r_slots, const SwfDisplayEvent &p_e, uint32_t p_index) {
	if (p_e.op == SwfDisplayOp::Remove) {
		r_slots.erase(p_e.depth);
		return;
	}
	if (p_e.op == SwfDisplayOp::Place) {
		merge(r_slots[p_e.depth], p_e, p_index);
		return;
	}
	auto it = r_slots.find(p_e.depth);
	if (it != r_slots.end()) {
		merge(it->second, p_e, p_index);
	}
}

struct Reported {
	bool live = false;
	SwfMatrix world;
	SwfColorXform color;
	bool visible = true;
	bool is_mask = false;
	uint32_t frame = 0;
	uint32_t text = 0;
	uint32_t character = 0;
	uint32_t children = 0;
	uint32_t effects = 0;
	std::vector<SwfNodeId> masks;
};

struct TimelineState {
	std::map<int32_t, Slot> slots;
	uint32_t applied_through = 0;
};

struct Resolved {
	std::shared_ptr<const SwfMovie> movie;
	std::string movie_name;
	uint16_t id = 0;
	std::string symbol;
	std::string unresolved;
	bool empty_clip = false;
};

const SwfTimeline &empty_clip_timeline() {
	static const SwfTimeline t = [] {
		SwfTimeline e;
		e.declared_frames = 1;
		e.frames = 1;
		return e;
	}();
	return t;
}

std::string segment_of(const SwfSceneNode &p_n) {
	return p_n.name.empty() ? "#" + std::to_string(p_n.depth) : p_n.name;
}

}

SwfColorXform swf_color_xform(const SwfColorTransform &p_cx) {
	SwfColorXform x;
	for (int i = 0; i < 4; ++i) {
		x.mul[i] = float(p_cx.mul[i]) / 256.0f;
		x.add[i] = float(p_cx.add[i]);
	}
	return x;
}

SwfColorXform swf_concat(const SwfColorXform &p_parent, const SwfColorXform &p_child) {
	SwfColorXform x;
	for (int i = 0; i < 4; ++i) {
		x.mul[i] = p_parent.mul[i] * p_child.mul[i];
		x.add[i] = p_parent.mul[i] * p_child.add[i] + p_parent.add[i];
	}
	return x;
}

SwfColor swf_apply(const SwfColorXform &p_cx, SwfColor p_color) {
	const float in[4] = { float(p_color.r), float(p_color.g), float(p_color.b), float(p_color.a) };
	uint8_t out[4];
	for (int i = 0; i < 4; ++i) {
		const float v = in[i] * p_cx.mul[i] + p_cx.add[i];
		out[i] = uint8_t(std::lround(std::clamp(v, 0.0f, 255.0f)));
	}
	return SwfColor{ out[0], out[1], out[2], out[3] };
}

const char *swf_node_kind_name(SwfNodeKind p_kind) {
	switch (p_kind) {
		case SwfNodeKind::Root: return "root";
		case SwfNodeKind::Sprite: return "sprite";
		case SwfNodeKind::Shape: return "shape";
		case SwfNodeKind::EditText: return "edit_text";
		case SwfNodeKind::StaticText: return "static_text";
		case SwfNodeKind::Bitmap: return "bitmap";
		case SwfNodeKind::Unresolved: return "unresolved";
	}
	return "?";
}

struct SwfScene::Impl {
	const SwfLibrary *lib = nullptr;
	SwfSceneOptions options;
	std::string movie_name;
	std::shared_ptr<const SwfMovie> movie;
	std::vector<std::unique_ptr<SwfSceneNode>> nodes;
	std::vector<std::unique_ptr<TimelineState>> timelines;
	std::vector<uint16_t> generations;
	std::vector<uint32_t> free_slots;
	std::vector<Reported> reported;
	std::vector<SwfNodeId> removed_reported;
	std::vector<uint32_t> jumps;
	std::vector<uint32_t> filter_source;
	std::deque<std::pair<SwfNodeId, uint32_t>> queue;
	SwfSceneStats stats;
	double accumulator = 0.0;
	std::optional<WorkBudget> budget;
	std::function<void(SwfNodeId, uint32_t)> on_script;
	std::vector<std::pair<SwfNodeId, uint32_t>> script_calls;

	void begin() { budget.emplace(kSwfSceneBudgetUnits); }

	void dispatch_scripts() {
		while (!script_calls.empty() && on_script) {
			std::vector<std::pair<SwfNodeId, uint32_t>> calls;
			calls.swap(script_calls);
			for (const auto &[id, frame] : calls) {
				if (get(id) != nullptr) {
					on_script(id, frame);
				}
			}
		}
		script_calls.clear();
	}

	Status charge(size_t p_units) {
		Status st = budget->charge(p_units);
		if (!st) {
			return make_error("swf scene " + movie_name + ": work budget exhausted (" + st.error().message + ")");
		}
		return Status::success();
	}

	SwfSceneNode *get(SwfNodeId p_id) const {
		const uint32_t s = slot_of(p_id);
		if (s >= nodes.size() || nodes[s] == nullptr || nodes[s]->id != p_id) {
			return nullptr;
		}
		return nodes[s].get();
	}

	TimelineState &timeline_state(SwfNodeId p_id) const { return *timelines[slot_of(p_id)]; }

	Resolved resolve(std::shared_ptr<const SwfMovie> p_movie, std::string p_name, uint16_t p_id) const {
		Resolved r;
		r.movie = std::move(p_movie);
		r.movie_name = std::move(p_name);
		r.id = p_id;
		for (uint32_t hop = 0; hop <= kSwfSceneMaxImportHops; ++hop) {
			const SwfExport *sym = nullptr;
			const SwfImport *im = r.movie->find_import(r.id, &sym);
			if (im == nullptr || sym == nullptr) {
				const std::string_view e = r.movie->export_name(r.id);
				r.symbol = e.empty() ? "#" + std::to_string(r.id) : std::string(e);
				return r;
			}
			r.symbol = sym->name;
			const std::string target = swf_movie_name_for_url(im->url);
			if (target.empty() || lib->find(target) == nullptr) {
				r.unresolved = "import " + sym->name + " from " + im->url + " names no movie of the library";
				return r;
			}
			Result<std::shared_ptr<const SwfMovie>> loaded = lib->load(target);
			if (!loaded) {
				r.unresolved = "import " + sym->name + " from " + target + ": " + loaded.error().message;
				return r;
			}
			const std::optional<uint16_t> id = (*loaded)->export_id(sym->name);
			if (!id) {
				r.unresolved = target + " exports no " + sym->name;
				return r;
			}
			r.movie = *loaded;
			r.movie_name = target;
			r.id = *id;
		}
		r.unresolved = "more than " + std::to_string(kSwfSceneMaxImportHops) + " import hops";
		return r;
	}

	void bind(SwfSceneNode &r_n, Resolved p_r) {
		r_n.movie = std::move(p_r.movie);
		r_n.movie_name = std::move(p_r.movie_name);
		r_n.character = p_r.id;
		r_n.symbol = std::move(p_r.symbol);
		r_n.unresolved = std::move(p_r.unresolved);
		r_n.timeline = nullptr;
		r_n.shape = nullptr;
		r_n.edit_text = nullptr;
		r_n.sub_image = nullptr;
		r_n.scaling_grid = nullptr;
		r_n.text.clear();
		r_n.html = false;
		r_n.text_options = SwfTextOptions{};
		r_n.text_bounds.reset();
		r_n.frame = 0;
		r_n.playing = false;
		if (!r_n.unresolved.empty()) {
			r_n.kind = SwfNodeKind::Unresolved;
			stats.unresolved++;
			return;
		}
		if (p_r.empty_clip) {
			r_n.kind = SwfNodeKind::Sprite;
			r_n.timeline = &empty_clip_timeline();
			r_n.playing = true;
			return;
		}
		const SwfMovie &m = *r_n.movie;
		switch (m.character_kind(r_n.character)) {
			case SwfCharacterKind::Shape:
				r_n.kind = SwfNodeKind::Shape;
				r_n.shape = m.find_shape(r_n.character);
				break;
			case SwfCharacterKind::Sprite:
				r_n.kind = SwfNodeKind::Sprite;
				r_n.timeline = &m.find_sprite(r_n.character)->timeline;
				r_n.scaling_grid = m.find_scaling_grid(r_n.character);
				break;
			case SwfCharacterKind::EditText:
				r_n.kind = SwfNodeKind::EditText;
				r_n.edit_text = m.find_edit_text(r_n.character);
				r_n.text = r_n.edit_text->initial_text;
				r_n.html = r_n.edit_text->has(SwfEditText::kHtml);
				r_n.text_options = swf_edit_text_options(*r_n.edit_text);
				break;
			case SwfCharacterKind::StaticText: r_n.kind = SwfNodeKind::StaticText; break;
			case SwfCharacterKind::SubImage:
				r_n.kind = SwfNodeKind::Bitmap;
				r_n.sub_image = m.find_sub_image(r_n.character);
				break;
			default:
				r_n.kind = SwfNodeKind::Unresolved;
				r_n.unresolved = std::string("character ") + std::to_string(r_n.character) + " of " + r_n.movie_name + " is " +
						swf_character_kind_name(m.character_kind(r_n.character));
				stats.unresolved++;
				break;
		}
		r_n.playing = r_n.timeline != nullptr;
	}

	std::string path_for(const SwfSceneNode &p_parent, const SwfSceneNode &p_child) const {
		const std::string seg = segment_of(p_child);
		return p_parent.id == 0 ? seg : p_parent.path + "." + seg;
	}

	void repath(SwfSceneNode &r_n) {
		if (const SwfSceneNode *p = get(r_n.parent)) {
			r_n.path = path_for(*p, r_n);
		}
		for (const SwfNodeId c : r_n.children) {
			repath(*get(c));
		}
	}

	void insert_child(SwfSceneNode &r_parent, SwfNodeId p_child) {
		const int32_t depth = get(p_child)->depth;
		auto it = std::upper_bound(r_parent.children.begin(), r_parent.children.end(), depth, [&](int32_t d, SwfNodeId c) { return d < get(c)->depth; });
		r_parent.children.insert(it, p_child);
		r_parent.children_revision++;
	}

	SwfNodeId child_at(const SwfSceneNode &p_parent, int32_t p_depth) const {
		for (const SwfNodeId c : p_parent.children) {
			if (get(c)->depth == p_depth) {
				return c;
			}
		}
		return kSwfNoNode;
	}

	void remove(SwfNodeId p_id) {
		SwfSceneNode *n = get(p_id);
		if (n == nullptr) {
			return;
		}
		while (!n->children.empty()) {
			remove(n->children.back());
		}
		if (SwfSceneNode *p = get(n->parent)) {
			auto it = std::find(p->children.begin(), p->children.end(), p_id);
			if (it != p->children.end()) {
				p->children.erase(it);
				p->children_revision++;
			}
		}
		const uint32_t s = slot_of(p_id);
		if (s < reported.size() && reported[s].live) {
			removed_reported.push_back(p_id);
			reported[s] = Reported{};
		}
		nodes[s].reset();
		timelines[s].reset();
		if (generations[s] < kMaxGeneration) {
			generations[s]++;
			free_slots.push_back(s);
		}
		stats.removed++;
		stats.live_nodes--;
	}

	void clear_children(SwfSceneNode &r_n) {
		while (!r_n.children.empty()) {
			remove(r_n.children.back());
		}
	}

	void apply_slot(SwfSceneNode &r_n, const SwfTimeline &p_parent_tl, const Slot &p_slot) {
		if (!r_n.script_matrix) {
			r_n.matrix = p_slot.matrix;
		}
		if (!r_n.script_color) {
			r_n.color = swf_color_xform(p_slot.color);
		}
		r_n.ratio = p_slot.ratio;
		if (r_n.clip_depth != p_slot.clip_depth) {
			r_n.clip_depth = p_slot.clip_depth;
		}
		if (r_n.name != p_slot.name) {
			r_n.name = std::string(p_slot.name);
			repath(r_n);
		}
		uint32_t &source = filter_source[slot_of(r_n.id)];
		if (r_n.blend_mode != p_slot.blend_mode || r_n.cache_as_bitmap != p_slot.cache_as_bitmap || source != p_slot.filters_from) {
			r_n.effects_revision++;
			r_n.blend_mode = p_slot.blend_mode;
			r_n.cache_as_bitmap = p_slot.cache_as_bitmap;
			if (!r_n.script_filters) {
				r_n.filters = p_slot.filters_from != kNoEvent ? p_parent_tl.events[p_slot.filters_from].filters : std::vector<SwfFilter>{};
			}
			source = p_slot.filters_from;
		}
		if (!r_n.script_visible) {
			r_n.visible = p_slot.visible;
		}
		if (p_slot.clip_actions && !r_n.clip_actions) {
			r_n.clip_actions = true;
			stats.clip_action_nodes++;
		}
	}

	Result<SwfNodeId> create(SwfSceneNode &r_parent, Resolved p_r, int32_t p_depth, uint32_t p_placed_by, bool p_dynamic, std::string_view p_name) {
		if (stats.live_nodes >= options.max_nodes) {
			return make_error("swf scene " + movie_name + ": more than " + std::to_string(options.max_nodes) + " live nodes");
		}
		if (r_parent.nesting + 1 > kSwfSceneMaxNesting) {
			return make_error("swf scene " + movie_name + ": " + r_parent.path + " nests deeper than " + std::to_string(kSwfSceneMaxNesting));
		}
		BF3_TRY(charge(kNodeUnits + p_name.size() / 16));
		Result<SwfNodeId> slot_id = allocate();
		if (!slot_id) {
			return slot_id.error();
		}
		const SwfNodeId id = *slot_id;
		SwfSceneNode &n = *nodes[slot_of(id)];
		n.id = id;
		n.parent = r_parent.id;
		n.depth = p_depth;
		n.placed_by = p_placed_by;
		n.dynamic = p_dynamic;
		n.nesting = r_parent.nesting + 1;
		n.name = std::string(p_name);
		bind(n, std::move(p_r));
		n.path = path_for(r_parent, n);
		insert_child(r_parent, id);
		stats.created++;
		stats.live_nodes++;
		if (n.kind == SwfNodeKind::Sprite) {
			BF3_TRY(start_timeline(id));
		}
		return id;
	}

	Result<SwfNodeId> allocate() {
		uint32_t s = 0;
		if (!free_slots.empty()) {
			s = free_slots.back();
			free_slots.pop_back();
		} else {
			if (nodes.size() >= kSwfSceneMaxNodes) {
				return make_error("swf scene " + movie_name + ": more than " + std::to_string(kSwfSceneMaxNodes) + " node slots");
			}
			s = uint32_t(nodes.size());
			nodes.emplace_back();
			timelines.emplace_back();
			generations.push_back(0);
			reported.emplace_back();
			jumps.push_back(0);
			filter_source.push_back(kNoEvent);
			stats.node_slots = uint32_t(nodes.size());
		}
		const SwfNodeId id = s | (uint32_t(generations[s]) << kSlotBits);
		nodes[s] = std::make_unique<SwfSceneNode>();
		nodes[s]->id = id;
		timelines[s].reset();
		reported[s] = Reported{};
		jumps[s] = 0;
		filter_source[s] = kNoEvent;
		return id;
	}

	Status start_timeline(SwfNodeId p_id) {
		timelines[slot_of(p_id)] = std::make_unique<TimelineState>();
		SwfSceneNode &n = *get(p_id);
		n.frame = 0;
		BF3_TRY(seek(p_id, 0));
		queue.emplace_back(p_id, 0);
		return Status::success();
	}

	Status seek(SwfNodeId p_id, uint32_t p_frame) {
		SwfSceneNode &n = *get(p_id);
		TimelineState &ts = timeline_state(p_id);
		const SwfTimeline &tl = *n.timeline;
		const std::vector<SwfDisplayEvent> &ev = tl.events;
		size_t from = 0;
		if (ts.applied_through != 0 && p_frame + 1 >= ts.applied_through) {
			from = size_t(std::lower_bound(ev.begin(), ev.end(), ts.applied_through, [](const SwfDisplayEvent &e, uint32_t f) { return e.frame < f; }) - ev.begin());
		} else {
			ts.slots.clear();
			if (ts.applied_through != 0) {
				stats.replays++;
			}
		}
		for (size_t i = from; i < ev.size() && ev[i].frame <= p_frame; ++i) {
			BF3_TRY(charge(1));
			apply_event(ts.slots, ev[i], uint32_t(i));
		}
		ts.applied_through = p_frame + 1;
		return reconcile(p_id);
	}

	Status rebind(SwfNodeId p_id, const SwfSceneNode &p_parent, uint16_t p_character) {
		SwfSceneNode &c = *get(p_id);
		clear_children(c);
		bind(c, resolve(p_parent.movie, p_parent.movie_name, p_character));
		c.placed_character = p_character;
		c.character_revision++;
		timelines[slot_of(p_id)].reset();
		if (c.kind == SwfNodeKind::Sprite) {
			BF3_TRY(start_timeline(p_id));
		}
		return Status::success();
	}

	Status reconcile(SwfNodeId p_id) {
		SwfSceneNode &n = *get(p_id);
		const TimelineState &ts = timeline_state(p_id);
		const SwfTimeline &tl = *n.timeline;
		BF3_TRY(charge(1 + n.children.size() + ts.slots.size()));
		const std::vector<SwfNodeId> existing = n.children;
		std::vector<int32_t> kept;
		for (const SwfNodeId c : existing) {
			SwfSceneNode *child = get(c);
			if (child == nullptr || child->dynamic) {
				continue;
			}
			auto it = ts.slots.find(child->depth);
			if (it == ts.slots.end() || it->second.placed_by != child->placed_by) {
				remove(c);
				continue;
			}
			kept.push_back(child->depth);
			if (it->second.character != child->placed_character) {
				BF3_TRY(rebind(c, n, it->second.character));
			}
			apply_slot(*get(c), tl, it->second);
		}
		for (const auto &[depth, slot] : ts.slots) {
			if (std::find(kept.begin(), kept.end(), depth) != kept.end()) {
				continue;
			}
			if (const SwfNodeId other = child_at(n, depth); other != kSwfNoNode) {
				remove(other);
			}
			Result<SwfNodeId> id = create(n, resolve(n.movie, n.movie_name, slot.character), depth, slot.placed_by, false, slot.name);
			if (!id) {
				return id.error();
			}
			SwfSceneNode &child = *get(*id);
			child.placed_character = slot.character;
			apply_slot(child, tl, slot);
		}
		return Status::success();
	}

	uint32_t &jump_count(SwfNodeId p_id) { return jumps[slot_of(p_id)]; }

	Status jump(SwfNodeId p_id, uint32_t p_frame, bool p_play) {
		SwfSceneNode *n = get(p_id);
		if (n == nullptr || n->timeline == nullptr || n->timeline->frames == 0) {
			return Status::success();
		}
		if (++jump_count(p_id) > kSwfSceneMaxJumpsPerNode) {
			stats.jumps_capped++;
			return Status::success();
		}
		const uint32_t frame = std::min(p_frame, n->timeline->frames - 1);
		n->playing = p_play;
		if (frame == n->frame) {
			return Status::success();
		}
		BF3_TRY(seek(p_id, frame));
		get(p_id)->frame = frame;
		queue.emplace_back(p_id, frame);
		return Status::success();
	}

	Status exec(SwfNodeId p_id, const SwfFrameOp &p_op) {
		SwfSceneNode &n = *get(p_id);
		switch (p_op.kind) {
			case SwfFrameOpKind::Stop: n.playing = false; return Status::success();
			case SwfFrameOpKind::Play: n.playing = true; return Status::success();
			case SwfFrameOpKind::GotoFrame: return jump(p_id, p_op.frame, p_op.and_play);
			case SwfFrameOpKind::GotoLabel: {
				const std::optional<uint32_t> f = n.timeline->label_frame(p_op.label);
				if (!f) {
					stats.labels_missing++;
					return Status::success();
				}
				return jump(p_id, *f, p_op.and_play);
			}
			case SwfFrameOpKind::NextFrame:
				if (n.frame + 1 < n.timeline->frames) {
					return jump(p_id, n.frame + 1, false);
				}
				n.playing = false;
				return Status::success();
			case SwfFrameOpKind::PrevFrame:
				if (n.frame > 0) {
					return jump(p_id, n.frame - 1, false);
				}
				n.playing = false;
				return Status::success();
		}
		return Status::success();
	}

	Status run_queue() {
		while (!queue.empty()) {
			const auto [id, frame] = queue.front();
			queue.pop_front();
			const SwfSceneNode *n = get(id);
			if (n == nullptr || n->timeline == nullptr || n->frame != frame || !options.run_frame_scripts) {
				continue;
			}
			const SwfFrameScript *s = n->timeline->script_at(frame);
			if (s == nullptr) {
				continue;
			}
			stats.scripts_run++;
			if (s->other_actions != 0) {
				stats.scripts_with_other_actions++;
				if (on_script) {
					script_calls.emplace_back(id, frame);
				}
			}
			for (const SwfFrameOp &op : s->ops) {
				BF3_TRY(charge(1));
				if (op.after_branch) {
					stats.ops_conditional_skipped++;
					continue;
				}
				stats.ops_run++;
				BF3_TRY(exec(id, op));
			}
		}
		return Status::success();
	}

	void preorder(SwfNodeId p_id, std::vector<SwfNodeId> &r_out) const {
		const SwfSceneNode *n = get(p_id);
		if (n == nullptr) {
			return;
		}
		r_out.push_back(p_id);
		for (const SwfNodeId c : n->children) {
			preorder(c, r_out);
		}
	}

	Status tick() {
		stats.ticks++;
		jumps.assign(nodes.size(), 0);
		std::vector<SwfNodeId> order;
		preorder(0, order);
		for (const SwfNodeId id : order) {
			SwfSceneNode *n = get(id);
			if (n == nullptr || !n->playing || n->timeline == nullptr || n->timeline->frames < 2) {
				continue;
			}
			const uint32_t next = n->frame + 1 < n->timeline->frames ? n->frame + 1 : 0;
			BF3_TRY(seek(id, next));
			get(id)->frame = next;
			queue.emplace_back(id, next);
		}
		return run_queue();
	}

	SwfNodeId find(std::string_view p_path) const {
		SwfNodeId cur = 0;
		if (p_path.empty()) {
			return cur;
		}
		size_t i = 0;
		bool first = true;
		while (i <= p_path.size()) {
			const size_t dot = p_path.find('.', i);
			const std::string_view seg = p_path.substr(i, dot == std::string_view::npos ? std::string_view::npos : dot - i);
			i = dot == std::string_view::npos ? p_path.size() + 1 : dot + 1;
			if (first && seg == "_root") {
				first = false;
				continue;
			}
			first = false;
			const SwfSceneNode *n = get(cur);
			SwfNodeId found = kSwfNoNode;
			for (const SwfNodeId c : n->children) {
				const SwfSceneNode *child = get(c);
				if (segment_of(*child) == seg) {
					found = c;
					break;
				}
			}
			if (found == kSwfNoNode) {
				return kSwfNoNode;
			}
			cur = found;
		}
		return cur;
	}

	Result<SwfSceneNode *> at(std::string_view p_path) {
		const SwfNodeId id = find(p_path);
		if (id == kSwfNoNode) {
			return make_error("swf scene " + movie_name + ": no node at " + std::string(p_path));
		}
		return get(id);
	}

	Result<SwfSceneNode *> timeline_at(std::string_view p_path) {
		Result<SwfSceneNode *> n = at(p_path);
		if (n && (*n)->timeline == nullptr) {
			return make_error("swf scene " + movie_name + ": " + std::string(p_path) + " is a " + swf_node_kind_name((*n)->kind) + ", not a timeline");
		}
		return n;
	}

	void update(SwfNodeId p_id, const SwfMatrix &p_world, const SwfColorXform &p_color, bool p_visible, const std::vector<SwfNodeId> &p_masks) {
		SwfSceneNode &n = *get(p_id);
		n.world = swf_concat(p_world, n.matrix);
		n.world_color = swf_concat(p_color, n.color);
		n.world_visible = p_visible && n.visible;
		n.masks = p_masks;
		std::vector<const SwfSceneNode *> active;
		for (const SwfNodeId c : n.children) {
			SwfSceneNode &child = *get(c);
			active.erase(std::remove_if(active.begin(), active.end(), [&](const SwfSceneNode *m) { return int32_t(m->clip_depth) < child.depth; }), active.end());
			std::vector<SwfNodeId> masks = n.masks;
			for (const SwfSceneNode *m : active) {
				masks.push_back(m->id);
			}
			child.is_mask = child.clip_depth != 0;
			update(c, n.world, n.world_color, n.world_visible, masks);
			if (child.clip_depth != 0) {
				active.push_back(&child);
			}
		}
	}

	std::vector<SwfSceneChange> changes() {
		std::vector<SwfSceneChange> out;
		for (const SwfNodeId id : removed_reported) {
			out.push_back({ id, swf_change::kRemoved });
		}
		removed_reported.clear();
		update(0, SwfMatrix{}, SwfColorXform{}, true, {});
		std::vector<SwfNodeId> order;
		preorder(0, order);
		for (const SwfNodeId id : order) {
			const SwfSceneNode &n = *get(id);
			Reported &r = reported[slot_of(id)];
			uint32_t what = 0;
			if (!r.live) {
				what = swf_change::kCreated;
			} else {
				what |= r.world == n.world ? 0 : swf_change::kMatrix;
				what |= r.color == n.world_color ? 0 : swf_change::kColor;
				what |= r.visible == n.world_visible ? 0 : swf_change::kVisible;
				what |= r.text == n.text_revision ? 0 : swf_change::kText;
				what |= r.character == n.character_revision ? 0 : swf_change::kCharacter;
				what |= r.children == n.children_revision ? 0 : swf_change::kChildren;
				what |= (r.masks == n.masks && r.is_mask == n.is_mask) ? 0 : swf_change::kMask;
				what |= r.frame == n.frame ? 0 : swf_change::kFrame;
				what |= r.effects == n.effects_revision ? 0 : swf_change::kEffects;
			}
			r.live = true;
			r.world = n.world;
			r.color = n.world_color;
			r.visible = n.world_visible;
			r.text = n.text_revision;
			r.character = n.character_revision;
			r.children = n.children_revision;
			r.masks = n.masks;
			r.is_mask = n.is_mask;
			r.frame = n.frame;
			r.effects = n.effects_revision;
			if (what != 0) {
				out.push_back({ id, what });
			}
		}
		return out;
	}
};

SwfScene::SwfScene() :
		impl_(std::make_unique<Impl>()) {}
SwfScene::~SwfScene() = default;
SwfScene::SwfScene(SwfScene &&) noexcept = default;
SwfScene &SwfScene::operator=(SwfScene &&) noexcept = default;

Result<SwfScene> SwfScene::open(const SwfLibrary &p_library, std::string_view p_movie, const SwfSceneOptions &p_options) {
	std::string name = to_lower_ascii(p_movie);
	if (name.find('/') == std::string::npos) {
		name = "ui/assets/" + name;
	}
	Result<std::shared_ptr<const SwfMovie>> movie = p_library.load(name);
	if (!movie) {
		return error_at("swf scene " + name, movie.error());
	}
	SwfScene scene;
	Impl &im = *scene.impl_;
	im.lib = &p_library;
	im.options = p_options;
	im.movie_name = name;
	im.movie = *movie;
	im.begin();
	const Result<SwfNodeId> root_id = im.allocate();
	if (!root_id) {
		return root_id.error();
	}
	SwfSceneNode &root = *im.get(*root_id);
	root.kind = SwfNodeKind::Root;
	root.movie = im.movie;
	root.movie_name = name;
	root.symbol = "_root";
	root.timeline = &im.movie->root;
	im.stats.created = 1;
	im.stats.live_nodes = 1;
	Status st = im.start_timeline(0);
	if (st) {
		st = im.run_queue();
	}
	if (!st) {
		return st.error();
	}
	return scene;
}

const std::string &SwfScene::movie_name() const {
	return impl_->movie_name;
}
const SwfMovie &SwfScene::movie() const {
	return *impl_->movie;
}
float SwfScene::frame_rate() const {
	return impl_->movie->frame_rate;
}
SwfRect SwfScene::stage() const {
	return impl_->movie->stage;
}
const SwfSceneNode *SwfScene::node(SwfNodeId p_id) const {
	return impl_->get(p_id);
}
SwfNodeId SwfScene::find(std::string_view p_path) const {
	return impl_->find(p_path);
}
const SwfSceneNode *SwfScene::find_node(std::string_view p_path) const {
	return impl_->get(impl_->find(p_path));
}
std::vector<SwfNodeId> SwfScene::draw_order() const {
	std::vector<SwfNodeId> out;
	impl_->preorder(0, out);
	return out;
}
const SwfSceneStats &SwfScene::stats() const {
	return impl_->stats;
}

Status SwfScene::advance(uint32_t p_frames) {
	impl_->begin();
	for (uint32_t i = 0; i < p_frames; ++i) {
		BF3_TRY(impl_->tick());
		impl_->dispatch_scripts();
	}
	return Status::success();
}

void SwfScene::set_frame_script_handler(std::function<void(SwfNodeId, uint32_t)> p_handler) {
	impl_->on_script = std::move(p_handler);
	impl_->script_calls.clear();
}

Status SwfScene::advance_seconds(double p_seconds) {
	if (!std::isfinite(p_seconds) || p_seconds < 0.0) {
		return make_error("swf scene " + impl_->movie_name + ": cannot advance by " + std::to_string(p_seconds) + " s");
	}
	Impl &im = *impl_;
	im.accumulator += p_seconds * double(im.movie->frame_rate);
	double whole = std::floor(im.accumulator);
	im.accumulator -= whole;
	if (whole > double(kSwfSceneMaxCatchUpFrames)) {
		im.stats.frames_dropped += uint32_t(std::min(whole - double(kSwfSceneMaxCatchUpFrames), 1e9));
		whole = double(kSwfSceneMaxCatchUpFrames);
	}
	return advance(uint32_t(whole));
}

Status SwfScene::goto_frame(std::string_view p_path, uint32_t p_frame, bool p_play) {
	Impl &im = *impl_;
	im.begin();
	Result<SwfSceneNode *> n = im.timeline_at(p_path);
	if (!n) {
		return n.error();
	}
	if (p_frame >= (*n)->timeline->frames) {
		return make_error("swf scene " + im.movie_name + ": frame " + std::to_string(p_frame) + " past the " + std::to_string((*n)->timeline->frames) +
				" frames of " + std::string(p_path));
	}
	im.jump_count((*n)->id) = 0;
	BF3_TRY(im.jump((*n)->id, p_frame, p_play));
	BF3_TRY(im.run_queue());
	im.dispatch_scripts();
	return Status::success();
}

Status SwfScene::goto_label(std::string_view p_path, std::string_view p_label, bool p_play) {
	Impl &im = *impl_;
	im.begin();
	Result<SwfSceneNode *> n = im.timeline_at(p_path);
	if (!n) {
		return n.error();
	}
	const std::optional<uint32_t> f = (*n)->timeline->label_frame(p_label);
	if (!f) {
		return make_error("swf scene " + im.movie_name + ": " + std::string(p_path) + " has no label " + std::string(p_label));
	}
	im.jump_count((*n)->id) = 0;
	BF3_TRY(im.jump((*n)->id, *f, p_play));
	BF3_TRY(im.run_queue());
	im.dispatch_scripts();
	return Status::success();
}

Status SwfScene::play(std::string_view p_path) {
	Result<SwfSceneNode *> n = impl_->timeline_at(p_path);
	if (!n) {
		return n.error();
	}
	(*n)->playing = true;
	return Status::success();
}

Status SwfScene::stop(std::string_view p_path) {
	Result<SwfSceneNode *> n = impl_->timeline_at(p_path);
	if (!n) {
		return n.error();
	}
	(*n)->playing = false;
	return Status::success();
}

Result<SwfNodeId> SwfScene::attach_movie(std::string_view p_parent_path, std::string_view p_linkage, std::string_view p_name, int32_t p_as2_depth) {
	Impl &im = *impl_;
	im.begin();
	Result<SwfSceneNode *> found = im.timeline_at(p_parent_path);
	if (!found) {
		return found.error();
	}
	SwfSceneNode &parent = **found;
	const SwfMovie &m = *parent.movie;
	std::optional<uint16_t> id = m.export_id(p_linkage);
	for (const SwfImport &imp : m.imports) {
		for (const SwfExport &s : imp.symbols) {
			if (!id && s.name == p_linkage) {
				id = s.id;
			}
		}
	}
	if (!id) {
		return make_error("swf scene " + im.movie_name + ": " + parent.movie_name + " has no linkage " + std::string(p_linkage));
	}
	if (p_as2_depth < -kSwfAs2DepthOffset || p_as2_depth > kSwfAs2MaxDepth) {
		return make_error("swf scene " + im.movie_name + ": depth " + std::to_string(p_as2_depth) + " outside the AS2 range");
	}
	const int32_t depth = p_as2_depth + kSwfAs2DepthOffset;
	if (const SwfNodeId other = im.child_at(parent, depth); other != kSwfNoNode) {
		im.remove(other);
	}
	Result<SwfNodeId> created = im.create(parent, im.resolve(parent.movie, parent.movie_name, *id), depth, kNoEvent, true, p_name);
	if (!created) {
		return created.error();
	}
	im.get(*created)->placed_character = *id;
	BF3_TRY(im.run_queue());
	im.dispatch_scripts();
	return *created;
}

Result<SwfNodeId> SwfScene::create_empty_movie_clip(std::string_view p_parent_path, std::string_view p_name, int32_t p_as2_depth) {
	Impl &im = *impl_;
	im.begin();
	Result<SwfSceneNode *> found = im.timeline_at(p_parent_path);
	if (!found) {
		return found.error();
	}
	SwfSceneNode &parent = **found;
	if (p_as2_depth < -kSwfAs2DepthOffset || p_as2_depth > kSwfAs2MaxDepth) {
		return make_error("swf scene " + im.movie_name + ": depth " + std::to_string(p_as2_depth) + " outside the AS2 range");
	}
	const int32_t depth = p_as2_depth + kSwfAs2DepthOffset;
	if (const SwfNodeId other = im.child_at(parent, depth); other != kSwfNoNode) {
		im.remove(other);
	}
	Resolved r;
	r.movie = parent.movie;
	r.movie_name = parent.movie_name;
	r.empty_clip = true;
	return im.create(parent, std::move(r), depth, kNoEvent, true, p_name);
}

Status SwfScene::remove_movie(std::string_view p_path) {
	Result<SwfSceneNode *> n = impl_->at(p_path);
	if (!n) {
		return n.error();
	}
	if (!(*n)->dynamic) {
		return make_error("swf scene " + impl_->movie_name + ": " + std::string(p_path) + " was placed by its timeline, not attached");
	}
	impl_->remove((*n)->id);
	return Status::success();
}

Status SwfScene::set_visible(std::string_view p_path, bool p_visible) {
	Result<SwfSceneNode *> n = impl_->at(p_path);
	if (!n) {
		return n.error();
	}
	(*n)->visible = p_visible;
	(*n)->script_visible = true;
	return Status::success();
}

Status SwfScene::set_alpha(std::string_view p_path, float p_alpha) {
	if (!std::isfinite(p_alpha)) {
		return make_error("swf scene " + impl_->movie_name + ": alpha is not finite");
	}
	Result<SwfSceneNode *> n = impl_->at(p_path);
	if (!n) {
		return n.error();
	}
	(*n)->color.mul[3] = p_alpha;
	(*n)->script_color = true;
	return Status::success();
}

Status SwfScene::set_matrix(std::string_view p_path, const SwfMatrix &p_matrix) {
	for (const float v : { p_matrix.a, p_matrix.b, p_matrix.c, p_matrix.d, p_matrix.tx, p_matrix.ty }) {
		if (!std::isfinite(v)) {
			return make_error("swf scene " + impl_->movie_name + ": matrix is not finite");
		}
	}
	Result<SwfSceneNode *> n = impl_->at(p_path);
	if (!n) {
		return n.error();
	}
	(*n)->matrix = p_matrix;
	(*n)->script_matrix = true;
	return Status::success();
}

Status SwfScene::set_color(std::string_view p_path, const SwfColorXform &p_color) {
	for (int i = 0; i < 4; ++i) {
		if (!std::isfinite(p_color.mul[i]) || !std::isfinite(p_color.add[i])) {
			return make_error("swf scene " + impl_->movie_name + ": colour transform is not finite");
		}
	}
	Result<SwfSceneNode *> n = impl_->at(p_path);
	if (!n) {
		return n.error();
	}
	(*n)->color = p_color;
	(*n)->script_color = true;
	return Status::success();
}

Status SwfScene::set_text(std::string_view p_path, std::string_view p_text, bool p_html) {
	Result<SwfSceneNode *> n = impl_->at(p_path);
	if (!n) {
		return n.error();
	}
	if ((*n)->kind != SwfNodeKind::EditText) {
		return make_error("swf scene " + impl_->movie_name + ": " + std::string(p_path) + " is a " + swf_node_kind_name((*n)->kind) + ", not a text field");
	}
	if (p_text.size() > size_t(kSwfTextMaxChars) * 4) {
		return make_error("swf scene " + impl_->movie_name + ": text of " + std::to_string(p_text.size()) + " bytes");
	}
	(*n)->text = std::string(p_text);
	(*n)->html = p_html;
	(*n)->text_revision++;
	return Status::success();
}

Status SwfScene::set_text_options(std::string_view p_path, const SwfTextOptions &p_options) {
	Result<SwfSceneNode *> n = impl_->at(p_path);
	if (!n) {
		return n.error();
	}
	if ((*n)->kind != SwfNodeKind::EditText) {
		return make_error("swf scene " + impl_->movie_name + ": " + std::string(p_path) + " is not a text field");
	}
	if ((*n)->text_options.auto_size != p_options.auto_size || (*n)->text_options.fit != p_options.fit) {
		(*n)->text_options = p_options;
		(*n)->text_revision++;
	}
	return Status::success();
}

Status SwfScene::set_text_color(std::string_view p_path, SwfColor p_color) {
	Result<SwfSceneNode *> n = impl_->at(p_path);
	if (!n) {
		return n.error();
	}
	if ((*n)->kind != SwfNodeKind::EditText) {
		return make_error("swf scene " + impl_->movie_name + ": " + std::string(p_path) + " is not a text field");
	}
	if ((*n)->text_color != p_color) {
		(*n)->text_color = p_color;
		(*n)->text_revision++;
	}
	return Status::success();
}

Status SwfScene::set_filters(std::string_view p_path, std::vector<SwfFilter> p_filters) {
	for (const SwfFilter &f : p_filters) {
		for (const float v : { f.blur_x, f.blur_y, f.angle, f.distance, f.strength }) {
			if (!std::isfinite(v)) {
				return make_error("swf scene " + impl_->movie_name + ": filter value is not finite");
			}
		}
	}
	Result<SwfSceneNode *> n = impl_->at(p_path);
	if (!n) {
		return n.error();
	}
	(*n)->filters = std::move(p_filters);
	(*n)->script_filters = true;
	(*n)->effects_revision++;
	return Status::success();
}

Status SwfScene::set_text_bounds(std::string_view p_path, const SwfRect &p_bounds) {
	for (const float v : { p_bounds.x_min, p_bounds.x_max, p_bounds.y_min, p_bounds.y_max }) {
		if (!std::isfinite(v)) {
			return make_error("swf scene " + impl_->movie_name + ": text bounds are not finite");
		}
	}
	if (p_bounds.x_max < p_bounds.x_min || p_bounds.y_max < p_bounds.y_min) {
		return make_error("swf scene " + impl_->movie_name + ": text bounds of negative size");
	}
	Result<SwfSceneNode *> n = impl_->at(p_path);
	if (!n) {
		return n.error();
	}
	if ((*n)->kind != SwfNodeKind::EditText) {
		return make_error("swf scene " + impl_->movie_name + ": " + std::string(p_path) + " is not a text field");
	}
	if (swf_node_text_bounds(**n) != p_bounds) {
		(*n)->text_bounds = p_bounds;
		(*n)->text_revision++;
	}
	return Status::success();
}

std::vector<SwfSceneChange> SwfScene::take_changes() {
	return impl_->changes();
}

std::optional<SwfBitmapRef> SwfScene::bitmap(const SwfSceneNode &p_node, uint16_t p_bitmap_id) const {
	if (!p_node.movie) {
		return std::nullopt;
	}
	return p_node.movie->resolve_bitmap(p_node.movie_name, p_bitmap_id);
}

SwfRect swf_node_text_bounds(const SwfSceneNode &p_node) {
	if (p_node.text_bounds) {
		return *p_node.text_bounds;
	}
	return p_node.edit_text != nullptr ? p_node.edit_text->bounds : SwfRect{};
}

Result<SwfRichText> swf_node_rich_text(const SwfSceneNode &p_node, WorkBudget &r_budget) {
	if (p_node.edit_text == nullptr || !p_node.movie) {
		return make_error("swf scene: " + p_node.path + " is not a text field");
	}
	SwfTextFormat base = swf_edit_text_format(*p_node.movie, *p_node.edit_text);
	if (p_node.text_color) {
		base.color = SwfColor{ p_node.text_color->r, p_node.text_color->g, p_node.text_color->b, base.color.a };
	}
	if (p_node.html) {
		return swf_parse_html(p_node.text, base, r_budget);
	}
	return swf_plain_text(p_node.text, base);
}

Result<SwfTextLayout> swf_layout_node_text(const SwfFontSet &p_fonts, const SwfSceneNode &p_node, WorkBudget &r_budget) {
	Result<SwfRichText> rich = swf_node_rich_text(p_node, r_budget);
	if (!rich) {
		return error_at("swf scene " + p_node.path, rich.error());
	}
	if (p_node.text_bounds) {
		SwfEditText field = *p_node.edit_text;
		field.bounds = *p_node.text_bounds;
		return swf_layout_text(p_fonts, field, *rich, p_node.text_options, r_budget);
	}
	return swf_layout_text(p_fonts, *p_node.edit_text, *rich, p_node.text_options, r_budget);
}

}
