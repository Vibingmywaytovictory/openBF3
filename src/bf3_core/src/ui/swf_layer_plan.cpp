#include "bf3/core/ui/swf_layer_plan.h"

#include "bf3/core/level/budget.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/swf_font.h"
#include "bf3/core/ui/ui_stage.h"
#include "bf3/core/util/fnv.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>

namespace bf3::core::ui {

namespace {

SwfBounds box_of(const SwfRect &p_r) {
	SwfBounds b;
	b.add(p_r.x_min, p_r.y_min);
	b.add(p_r.x_max, p_r.y_max);
	return b;
}

std::vector<double> box_kernel(double p_width, uint32_t p_passes) {
	std::vector<double> k{ 1.0 };
	if (p_passes == 0 || !(p_width > 1.0)) {
		return k;
	}
	const double h = p_width * 0.5;
	const int32_t r = int32_t(std::ceil(h - 0.5));
	std::vector<double> box(size_t(2 * r + 1), 0.0);
	for (int32_t i = -r; i <= r; ++i) {
		const double lo = std::max(double(i) - 0.5, -h);
		const double hi = std::min(double(i) + 0.5, h);
		box[size_t(i + r)] = std::max(0.0, hi - lo) / p_width;
	}
	for (uint32_t p = 0; p < p_passes; ++p) {
		std::vector<double> out(k.size() + box.size() - 1, 0.0);
		for (size_t i = 0; i < k.size(); ++i) {
			for (size_t j = 0; j < box.size(); ++j) {
				out[i + j] += k[i] * box[j];
			}
		}
		k = std::move(out);
	}
	return k;
}

std::vector<double> clamp_kernel(std::vector<double> p_k, uint32_t &r_clamped) {
	const size_t limit = size_t(kSwfMaxKernelTaps);
	if (p_k.size() <= limit) {
		return p_k;
	}
	++r_clamped;
	const size_t drop = (p_k.size() - limit) / 2;
	std::vector<double> k(p_k.begin() + std::ptrdiff_t(drop), p_k.begin() + std::ptrdiff_t(drop + limit));
	double sum = 0.0;
	for (const double w : k) {
		sum += w;
	}
	for (double &w : k) {
		w = sum > 0.0 ? w / sum : 0.0;
	}
	return k;
}

double grid_axis(double p_v, double p_b0, double p_b1, double p_s0, double p_s1, double p_scale) {
	const double inner0 = p_b0 + (p_s0 - p_b0) / p_scale;
	const double inner1 = p_b1 - (p_b1 - p_s1) / p_scale;
	if (p_v <= p_s0) {
		return p_b0 + (p_v - p_b0) / p_scale;
	}
	if (p_v >= p_s1) {
		return p_b1 - (p_b1 - p_v) / p_scale;
	}
	const double span = p_s1 - p_s0;
	return span > 0.0 ? inner0 + (p_v - p_s0) / span * (inner1 - inner0) : inner0;
}

void add_grid_bounds(
		const SwfScene &p_scene, const SwfSceneNode &p_node, const SwfAffine &p_to_owner, SwfBounds &r_bounds) {
	for (const SwfNodeId c : p_node.children) {
		const SwfSceneNode *k = p_scene.node(c);
		if (k == nullptr || k->scaling_grid != nullptr) {
			continue;
		}
		const SwfAffine m = swf_concat(p_to_owner, swf_affine(k->matrix));
		if (k->shape != nullptr) {
			r_bounds.add(swf_project_rect(k->shape->bounds, m));
		}
		add_grid_bounds(p_scene, *k, m, r_bounds);
	}
}

SwfBounds glyph_box(const SwfFontFace &p_face, uint16_t p_glyph) {
	SwfBounds b;
	if (p_glyph < p_face.outlines.glyphs.size()) {
		for (const GlyphPoint &p : p_face.outlines.glyphs[p_glyph].points) {
			b.add(p.x, p.y);
		}
	}
	return b;
}

void add_shape_pieces(const SwfScene &p_scene, const SwfSceneNode &p_node, const SwfShape &p_shape,
		const SwfGridWarp &p_grid, SwfNodePieces &r_out, SwfBounds &r_box) {
	SwfAffine to_sprite;
	if (p_grid.active) {
		swf_grid_owner(p_scene, p_node, &to_sprite);
	}
	const auto place = [&](SwfPoint p_p) {
		const SwfPoint q = p_grid.active ? swf_apply(to_sprite, p_p.x, p_p.y) : p_p;
		return p_grid.active ? p_grid.warp(q) : q;
	};
	bool solid_open = false;
	for (const SwfFillMesh &mesh : p_shape.meshes) {
		if (mesh.fill >= p_shape.fills.size()) {
			continue;
		}
		const SwfFillStyle &fill = p_shape.fills[mesh.fill];
		if (fill.kind == SwfFillKind::Solid) {
			if (!mesh.indices.empty() && !solid_open) {
				solid_open = true;
				++r_out.count;
			}
		} else if (fill.kind == SwfFillKind::Bitmap && p_scene.bitmap(p_node, fill.bitmap_id).has_value()) {
			solid_open = false;
			r_out.count += mesh.indices.empty() ? 0u : 1u;
		} else {
			continue;
		}
		for (const uint32_t i : mesh.indices) {
			const SwfPoint q = place(mesh.vertices[i]);
			r_box.add(q.x, q.y);
		}
	}
	for (const SwfStroke &st : p_shape.strokes) {
		if (st.line >= p_shape.lines.size() || st.points.empty()) {
			continue;
		}
		const SwfLineStyle &line = p_shape.lines[st.line];
		const double hw = std::max(0.5, double(line.width) * 0.5);
		if (!solid_open) {
			solid_open = true;
			++r_out.count;
		}
		for (const SwfPoint &p : st.points) {
			const SwfPoint q = place(p);
			r_box.add(q.x - hw, q.y - hw);
			r_box.add(q.x + hw, q.y + hw);
		}
	}
}

struct LayerWalker {
	const SwfScene &scene;
	const SwfPlanWalkOptions &options;
	SwfLayerVisitor &visitor;
	SwfLayerWalk out;
	std::set<SwfNodeId> isolated_ancestors;

	struct Ctx {
		bool additive = false;
		bool alpha = false;
		bool enabled = true;
	};

	const SwfSceneNode *node(SwfNodeId p_id) const { return scene.node(p_id); }

	uint32_t drawables(SwfNodeId p_id) {
		const SwfSceneNode *n = node(p_id);
		if (n == nullptr) {
			return 0;
		}
		uint32_t count = visitor.pieces(*n);
		for (const SwfNodeId c : n->children) {
			if (count >= 2) {
				break;
			}
			const SwfSceneNode *k = node(c);
			if (k != nullptr && k->clip_depth == 0 && !k->is_mask) {
				count += drawables(c);
			}
		}
		return count;
	}

	uint32_t add_layer(SwfLayerKind p_kind, const SwfSceneNode &p_owner, uint32_t p_parent,
			std::vector<SwfFilter> p_filters = {}, bool p_additive = false) {
		const uint32_t index = uint32_t(out.layers.size());
		SwfLayerWalkNode l;
		l.owner = p_owner.id;
		l.kind = p_kind;
		l.path = p_owner.path;
		l.parent = p_parent;
		l.filters = std::move(p_filters);
		l.additive = p_additive;
		out.layers.push_back(std::move(l));
		if (p_parent != kSwfNoLayer) {
			out.layers[p_parent].children.push_back(index);
		}
		visitor.layer_opened(index, out.layers[index]);
		return index;
	}

	void note_tilt(const SwfSceneNode &p_node, uint32_t p_layer) {
		if (options.tilted.empty() || p_layer == kSwfNoLayer) {
			return;
		}
		SwfLayerWalkNode &l = out.layers[p_layer];
		const SwfNodeId floor = l.kind == SwfLayerKind::Filter ? l.owner : kSwfNoNode;
		for (const SwfSceneNode *a = &p_node; a != nullptr && a->id != floor; a = node(a->parent)) {
			l.tilted_inside = l.tilted_inside ||
					std::find(options.tilted.begin(), options.tilted.end(), a->id) != options.tilted.end();
		}
	}

	void walk_mask(const SwfSceneNode &p_mask, uint32_t p_group) {
		Ctx alpha;
		alpha.alpha = true;
		walk_node(p_mask.id, p_group, alpha);
	}

	void walk_node(SwfNodeId p_id, uint32_t p_layer, Ctx p_ctx) {
		const SwfSceneNode *n = node(p_id);
		if (n == nullptr) {
			return;
		}
		visitor.seen(*n);
		bool own = true;
		if (!p_ctx.enabled) {
			if (p_id == options.isolated) {
				p_ctx.enabled = true;
			} else if (isolated_ancestors.count(p_id) != 0) {
				own = false;
			} else {
				return;
			}
		}
		if (!p_ctx.alpha && n->is_mask && n->clip_depth == 0) {
			return;
		}
		const SwfSceneNode *script_mask = !p_ctx.alpha && own ? node(n->script_mask) : nullptr;
		uint32_t layer = p_layer;
		uint32_t group = kSwfNoLayer;
		if (script_mask != nullptr) {
			group = add_layer(SwfLayerKind::Mask, *script_mask, p_layer);
			layer = group;
		}
		if (!p_ctx.alpha && own) {
			if (visitor.engine_target(*n)) {
				++out.engine_targets;
				if (layer != kSwfNoLayer) {
					out.layers[layer].has_targets = true;
				}
				visitor.target(*n, layer);
				if (script_mask != nullptr) {
					walk_mask(*script_mask, group);
				}
				return;
			}
			std::vector<SwfFilter> filters;
			for (const SwfFilter &f : n->filters) {
				if (swf_filter_supported(f)) {
					filters.push_back(f);
				} else {
					visitor.unsupported_filter(*n, f);
				}
			}
			const bool add = n->blend_mode == swf_blend::kAdd;
			const bool blend_layer = n->blend_mode == swf_blend::kLayer;
			if (n->blend_mode > swf_blend::kLayer && !add) {
				visitor.unsupported_blend(*n);
			}
			if (options.filters_enabled && !filters.empty()) {
				out.multi_filter_layers += filters.size() >= 2 ? 1u : 0u;
				layer = add_layer(SwfLayerKind::Filter, *n, layer, std::move(filters), add || p_ctx.additive);
				p_ctx.additive = false;
			} else if (add || blend_layer) {
				if (drawables(p_id) >= 2) {
					layer = add_layer(SwfLayerKind::Blend, *n, layer, {}, add || p_ctx.additive);
					p_ctx.additive = false;
				} else {
					++out.direct_blends;
					p_ctx.additive = p_ctx.additive || add;
				}
			}
			if (layer == p_layer && n->kind == SwfNodeKind::EditText && visitor.pieces(*n) != 0) {
				++out.plain_text_fields;
			}
		}
		visitor.attach(*n, layer, p_ctx.alpha, p_ctx.additive, own);
		if (own && !p_ctx.alpha) {
			note_tilt(*n, layer);
		}
		walk_range(n->children, 0, n->children.size(), layer, p_ctx);
		if (script_mask != nullptr) {
			walk_mask(*script_mask, group);
		}
	}

	void walk_range(const std::vector<SwfNodeId> &p_kids, size_t p_begin, size_t p_end, uint32_t p_layer, Ctx p_ctx) {
		for (size_t i = p_begin; i < p_end; ++i) {
			const SwfSceneNode *c = node(p_kids[i]);
			if (c == nullptr) {
				continue;
			}
			if (c->clip_depth == 0) {
				walk_node(p_kids[i], p_layer, p_ctx);
				continue;
			}
			size_t j = i + 1;
			while (j < p_end) {
				const SwfSceneNode *k = node(p_kids[j]);
				if (k == nullptr || k->depth > int32_t(c->clip_depth)) {
					break;
				}
				++j;
			}
			if (p_ctx.alpha) {
				visitor.seen(*c);
				walk_range(p_kids, i + 1, j, p_layer, p_ctx);
				i = j - 1;
				continue;
			}
			const uint32_t group = add_layer(SwfLayerKind::Mask, *c, p_layer);
			Ctx mask_ctx = p_ctx;
			mask_ctx.alpha = true;
			mask_ctx.additive = false;
			mask_ctx.enabled = true;
			walk_node(p_kids[i], group, mask_ctx);
			walk_range(p_kids, i + 1, j, group, p_ctx);
			i = j - 1;
		}
	}

	void classify() {
		for (SwfLayerWalkNode &l : out.layers) {
			if (l.kind == SwfLayerKind::Mask) {
				const bool rect = swf_rect_mask(scene, options.fonts, l.owner, options.tilted).has_value();
				l.route = rect ? SwfLayerClass::Clip : SwfLayerClass::OwnSurface;
				out.non_rect_masks += rect ? 0u : 1u;
			}
		}
		for (SwfLayerWalkNode &l : out.layers) {
			if (l.kind == SwfLayerKind::Mask) {
				continue;
			}
			bool ancestors_clear = true;
			for (uint32_t a = l.parent; a != kSwfNoLayer; a = out.layers[a].parent) {
				ancestors_clear = ancestors_clear && out.layers[a].route == SwfLayerClass::Clip;
			}
			const bool leaf = l.children.empty() && !l.has_targets;
			const bool one_stage = l.kind == SwfLayerKind::Filter && l.filters.size() == 1;
			l.route = one_stage && leaf && ancestors_clear && !l.tilted_inside ? SwfLayerClass::Page
																			   : SwfLayerClass::OwnSurface;
		}
	}
};

struct BoundsVisitor final : SwfLayerVisitor {
	const SwfScene &scene;
	const SwfFontSet *fonts = nullptr;
	std::vector<SwfNodeId> targets;
	std::vector<SwfBounds> nodes;
	std::vector<SwfBounds> masks;
	std::unordered_map<SwfNodeId, SwfNodePieces> known;
	uint32_t root_items = 0;

	explicit BoundsVisitor(const SwfScene &p_scene) : scene(p_scene) {}

	const SwfNodePieces &pieces_of(const SwfSceneNode &p_n) {
		const auto it = known.find(p_n.id);
		if (it != known.end()) {
			return it->second;
		}
		return known.emplace(p_n.id, swf_node_pieces(scene, fonts, p_n)).first->second;
	}

	void seen(const SwfSceneNode &) override {}
	uint32_t pieces(const SwfSceneNode &p_node) override { return pieces_of(p_node).count; }
	bool engine_target(const SwfSceneNode &p_node) override {
		return std::find(targets.begin(), targets.end(), p_node.id) != targets.end();
	}
	void target(const SwfSceneNode &, uint32_t) override {}
	void layer_opened(uint32_t, const SwfLayerWalkNode &) override {
		nodes.emplace_back();
		masks.emplace_back();
	}
	void attach(const SwfSceneNode &p_node, uint32_t p_layer, bool p_alpha, bool, bool) override {
		const SwfNodePieces &p = pieces_of(p_node);
		if (p_layer == kSwfNoLayer) {
			root_items += p.count;
		}
		if (!p.has_bounds || (!p_alpha && !p_node.world_visible) || p_layer == kSwfNoLayer) {
			return;
		}
		(p_alpha ? masks[p_layer] : nodes[p_layer]).add(swf_project_rect(p.local, swf_affine(p.frame->world)));
	}
};

std::vector<SwfNodeId> resolve(const SwfScene &p_scene, const std::vector<std::string> &p_paths) {
	std::vector<SwfNodeId> out;
	for (const std::string &path : p_paths) {
		const SwfNodeId id = p_scene.find(path);
		if (id != kSwfNoNode) {
			out.push_back(id);
		}
	}
	return out;
}

uint64_t kernel_key(const SwfFilterKernel &p_k) {
	uint64_t h = kFnv1aOffset;
	const auto mix = [&h](double p_v) { fnv1a_value(h, float(p_v)); };
	mix(double(p_k.x.size()));
	for (const double w : p_k.x) {
		mix(w);
	}
	mix(double(p_k.y.size()));
	for (const double w : p_k.y) {
		mix(w);
	}
	return h;
}

}

SwfBounds swf_project_rect(const SwfRect &p_r, const SwfAffine &p_m) {
	return swf_map_rect(p_r, [&p_m](double p_x, double p_y) { return swf_apply(p_m, p_x, p_y); });
}

uint64_t swf_target_bytes(int32_t p_width, int32_t p_height, uint64_t p_factor) {
	return uint64_t(std::max(0, p_width)) * uint64_t(std::max(0, p_height)) * 4u * p_factor;
}

bool swf_filter_supported(const SwfFilter &p_filter) {
	return p_filter.kind == SwfFilterKind::DropShadow || p_filter.kind == SwfFilterKind::Glow ||
			p_filter.kind == SwfFilterKind::Blur;
}

SwfFilterKernel swf_filter_kernel(const SwfFilter &p_filter, double p_scale_x, double p_scale_y) {
	SwfFilterKernel k;
	k.x = clamp_kernel(box_kernel(double(p_filter.blur_x) * p_scale_x, p_filter.passes), k.clamped);
	k.y = clamp_kernel(box_kernel(double(p_filter.blur_y) * p_scale_y, p_filter.passes), k.clamped);
	const bool shadow = p_filter.kind == SwfFilterKind::DropShadow;
	k.shift_x = shadow ? std::cos(double(p_filter.angle)) * double(p_filter.distance) * p_scale_x : 0.0;
	k.shift_y = shadow ? std::sin(double(p_filter.angle)) * double(p_filter.distance) * p_scale_y : 0.0;
	k.margin_x = double(k.x.size() / 2) + std::abs(k.shift_x) + 1.0;
	k.margin_y = double(k.y.size() / 2) + std::abs(k.shift_y) + 1.0;
	return k;
}

SwfPoint SwfGridWarp::warp(SwfPoint p_p) const {
	return { float(grid_axis(p_p.x, bx0, bx1, l, r, sx)), float(grid_axis(p_p.y, by0, by1, t, b, sy)) };
}

const SwfSceneNode *swf_grid_owner(const SwfScene &p_scene, const SwfSceneNode &p_node, SwfAffine *r_to_owner) {
	SwfAffine to_owner = swf_affine(p_node.matrix);
	for (const SwfSceneNode *s = p_scene.node(p_node.parent); s != nullptr; s = p_scene.node(s->parent)) {
		if (s->scaling_grid != nullptr) {
			if (r_to_owner != nullptr) {
				*r_to_owner = to_owner;
			}
			return s;
		}
		to_owner = swf_concat(swf_affine(s->matrix), to_owner);
	}
	return nullptr;
}

SwfGridWarp swf_grid_warp(const SwfScene &p_scene, const SwfSceneNode &p_node, const SwfAffine &p_stage_to_window) {
	SwfGridWarp g;
	const SwfSceneNode *sprite = p_node.shape != nullptr ? swf_grid_owner(p_scene, p_node) : nullptr;
	if (sprite == nullptr) {
		return g;
	}
	SwfBounds sb;
	add_grid_bounds(p_scene, *sprite, SwfAffine{}, sb);
	if (sb.empty()) {
		return g;
	}
	const SwfAffine w = swf_concat(p_stage_to_window, swf_affine(sprite->world));
	g.sx = std::sqrt(w.a * w.a + w.b * w.b);
	g.sy = std::sqrt(w.c * w.c + w.d * w.d);
	const SwfAffine win = swf_concat(p_stage_to_window, swf_affine(SwfMatrix{}));
	g.sx /= std::sqrt(win.a * win.a + win.b * win.b);
	g.sy /= std::sqrt(win.c * win.c + win.d * win.d);
	if (!(g.sx > 1e-6) || !(g.sy > 1e-6)) {
		return g;
	}
	g.active = true;
	g.bx0 = sb.x0;
	g.bx1 = sb.x1;
	g.by0 = sb.y0;
	g.by1 = sb.y1;
	g.l = sprite->scaling_grid->splitter.x_min;
	g.r = sprite->scaling_grid->splitter.x_max;
	g.t = sprite->scaling_grid->splitter.y_min;
	g.b = sprite->scaling_grid->splitter.y_max;
	return g;
}

SwfNodePieces swf_node_pieces(const SwfScene &p_scene, const SwfFontSet *p_fonts, const SwfSceneNode &p_node) {
	SwfNodePieces out;
	out.frame = &p_node;
	SwfBounds box;
	if (p_node.kind == SwfNodeKind::Shape && p_node.shape != nullptr) {
		const SwfGridWarp grid = swf_grid_warp(p_scene, p_node, SwfAffine{});
		if (grid.active) {
			out.frame = swf_grid_owner(p_scene, p_node);
		}
		add_shape_pieces(p_scene, p_node, *p_node.shape, grid, out, box);
	} else if (!p_node.image.empty()) {
		out.count = 1;
		box.add(0.0, 0.0);
		box.add(double(p_node.image_width), double(p_node.image_height));
	} else if (p_node.drawing != nullptr) {
		add_shape_pieces(p_scene, p_node, *p_node.drawing, SwfGridWarp{}, out, box);
	} else if (p_node.kind == SwfNodeKind::EditText && p_node.edit_text != nullptr) {
		out.count = 1;
		if (p_fonts != nullptr) {
			WorkBudget budget(kSwfTextBudgetUnits);
			Result<SwfTextLayout> layout = swf_layout_node_text(*p_fonts, p_node, budget);
			if (layout) {
				for (const SwfGlyphPlacement &g : layout->glyphs) {
					if (g.face == nullptr) {
						continue;
					}
					const SwfBounds gb = glyph_box(*g.face, g.glyph);
					if (!gb.empty()) {
						const double s = double(g.size) / double(kSwfFontUnitsPerEm);
						box.add(g.x + gb.x0 * s, g.baseline + gb.y0 * s);
						box.add(g.x + gb.x1 * s, g.baseline + gb.y1 * s);
					}
				}
				box = box.empty() ? box : box.intersect(box_of(layout->bounds));
			}
		}
	} else if (p_node.kind == SwfNodeKind::Bitmap && p_node.sub_image != nullptr) {
		out.count = 1;
		box.add(0.0, 0.0);
		box.add(double(p_node.sub_image->width()), double(p_node.sub_image->height()));
	}
	out.has_bounds = !box.empty();
	if (out.has_bounds) {
		out.local = box.rect();
	} else if (p_node.kind == SwfNodeKind::EditText) {
		out.count = 0;
	}
	return out;
}

std::optional<SwfRectMask> swf_rect_mask(
		const SwfScene &p_scene, const SwfFontSet *p_fonts, SwfNodeId p_mask, std::span<const SwfNodeId> p_tilted) {
	const SwfSceneNode *root = p_scene.node(p_mask);
	if (root == nullptr) {
		return std::nullopt;
	}
	const SwfSceneNode *drawn = nullptr;
	std::vector<SwfNodeId> stack{ p_mask };
	while (!stack.empty()) {
		const SwfSceneNode *n = p_scene.node(stack.back());
		stack.pop_back();
		if (n == nullptr) {
			continue;
		}
		const SwfNodePieces pieces = swf_node_pieces(p_scene, p_fonts, *n);
		if (pieces.count > 1 || (pieces.count == 1 && drawn != nullptr)) {
			return std::nullopt;
		}
		if (pieces.count == 1) {
			drawn = n;
		}
		stack.insert(stack.end(), n->children.begin(), n->children.end());
	}
	if (drawn == nullptr) {
		return std::nullopt;
	}
	for (const SwfSceneNode *a = drawn; a != nullptr; a = p_scene.node(a->parent)) {
		if (std::find(p_tilted.begin(), p_tilted.end(), a->id) != p_tilted.end()) {
			return std::nullopt;
		}
	}
	const SwfNodePieces pieces = swf_node_pieces(p_scene, p_fonts, *drawn);
	const SwfMatrix &w = pieces.frame->world;
	if (!pieces.has_bounds || std::abs(w.b) > 1e-5f || std::abs(w.c) > 1e-5f) {
		return std::nullopt;
	}
	const SwfShape *shape =
			drawn->kind == SwfNodeKind::Shape && drawn->shape != nullptr ? drawn->shape : drawn->drawing.get();
	if (drawn->kind == SwfNodeKind::EditText) {
		return std::nullopt;
	}
	if (shape != nullptr) {
		if (shape->meshes.size() != 1 || !shape->strokes.empty()) {
			return std::nullopt;
		}
		const SwfFillMesh &mesh = shape->meshes.front();
		SwfBounds vb;
		for (const uint32_t i : mesh.indices) {
			vb.add(mesh.vertices[i].x, mesh.vertices[i].y);
		}
		const double bounds_area = vb.empty() ? 0.0 : (vb.x1 - vb.x0) * (vb.y1 - vb.y0);
		if (!(bounds_area > 0.0) || std::abs(mesh.triangle_area() - bounds_area) > bounds_area * 1e-3) {
			return std::nullopt;
		}
	}
	return SwfRectMask{ swf_project_rect(pieces.local, swf_affine(w)).rect() };
}

const char *swf_layer_kind_name(SwfLayerKind p_kind) {
	switch (p_kind) {
		case SwfLayerKind::Filter:
			return "filter";
		case SwfLayerKind::Mask:
			return "mask";
		case SwfLayerKind::Blend:
			return "blend";
	}
	return "?";
}

const char *swf_layer_class_name(SwfLayerClass p_class) {
	switch (p_class) {
		case SwfLayerClass::Direct:
			return "direct";
		case SwfLayerClass::Page:
			return "page";
		case SwfLayerClass::Clip:
			return "clip";
		case SwfLayerClass::OwnSurface:
			return "own surface";
		case SwfLayerClass::Empty:
			return "empty";
	}
	return "?";
}

SwfLayerWalk swf_walk_layers(const SwfScene &p_scene, const SwfPlanWalkOptions &p_options, SwfLayerVisitor &r_visitor) {
	LayerWalker w{ p_scene, p_options, r_visitor };
	w.out.stage = p_scene.stage();
	LayerWalker::Ctx ctx;
	if (p_options.isolating) {
		ctx.enabled = false;
		const SwfSceneNode *isolated = p_scene.node(p_options.isolated);
		for (const SwfSceneNode *a = isolated != nullptr ? p_scene.node(isolated->parent) : nullptr; a != nullptr;
				a = p_scene.node(a->parent)) {
			w.isolated_ancestors.insert(a->id);
		}
	}
	w.walk_node(p_scene.root(), kSwfNoLayer, ctx);
	w.classify();
	return std::move(w.out);
}

SwfLayerWalk swf_layer_walk(const SwfScene &p_scene, const SwfLayerWalkOptions &p_options) {
	BoundsVisitor visitor(p_scene);
	visitor.fonts = p_options.fonts;
	visitor.targets = resolve(p_scene, p_options.target_paths);
	SwfPlanWalkOptions walk;
	walk.fonts = p_options.fonts;
	walk.tilted = resolve(p_scene, p_options.tilted_paths);
	SwfLayerWalk out = swf_walk_layers(p_scene, walk, visitor);
	out.root_items = visitor.root_items;
	for (size_t i = 0; i < out.layers.size(); ++i) {
		SwfLayerWalkNode &l = out.layers[i];
		l.has_nodes = !visitor.nodes[i].empty();
		if (l.has_nodes) {
			l.nodes = visitor.nodes[i].rect();
		}
		l.has_mask = !visitor.masks[i].empty();
		if (l.has_mask) {
			l.mask = visitor.masks[i].rect();
		}
	}
	return out;
}

SwfLayerWalk swf_layer_walk(const HudScreen &p_screen) {
	SwfLayerWalkOptions o;
	o.fonts = p_screen.fonts();
	for (const HudTilt &t : p_screen.tilts()) {
		o.tilted_paths.push_back(t.path);
	}
	for (const HudRendererString &r : p_screen.renderer_strings()) {
		if (r.renderer.find(";i") != std::string::npos) {
			o.target_paths.push_back(r.path);
		}
	}
	return swf_layer_walk(p_screen.scene(), o);
}

SwfLayerPlan swf_layer_plan(const SwfLayerWalk &p_walk, int32_t p_window_width, int32_t p_window_height) {
	const UiStage st = ui_stage_show_all(p_walk.stage, float(p_window_width), float(p_window_height));
	const SwfAffine to_window{ st.scale, 0.0, 0.0, st.scale, st.offset_x, st.offset_y };
	SwfLayerPlan plan;
	plan.layers.resize(p_walk.layers.size());
	std::set<uint64_t> kernels;
	for (size_t i = p_walk.layers.size(); i-- > 0;) {
		const SwfLayerWalkNode &w = p_walk.layers[i];
		SwfPlannedLayer &l = plan.layers[i];
		l.owner = w.owner;
		l.kind = w.kind;
		l.route = w.route;
		l.path = w.path;
		l.filters = uint32_t(w.filters.size());
		double margin_x = 0.0;
		double margin_y = 0.0;
		for (const SwfFilter &f : w.filters) {
			const SwfFilterKernel k = swf_filter_kernel(f, st.scale, st.scale);
			margin_x += k.margin_x;
			margin_y += k.margin_y;
			l.taps_x = uint32_t(k.x.size());
			l.taps_y = uint32_t(k.y.size());
			kernels.insert(kernel_key(k));
		}
		SwfBounds b;
		if (w.has_nodes) {
			b.add(swf_project_rect(w.nodes, to_window));
		}
		for (const uint32_t c : w.children) {
			const SwfPlannedLayer &k = plan.layers[c];
			if (k.has_content) {
				b.add(double(k.x), double(k.y));
				b.add(double(k.x + k.width), double(k.y + k.height));
			}
		}
		if (w.kind == SwfLayerKind::Mask) {
			b = w.has_mask ? b.intersect(swf_project_rect(w.mask, to_window)) : SwfBounds{};
		}
		l.has_content = !b.empty();
		if (l.has_content) {
			const double x0 = std::floor(b.x0 - margin_x) - 1.0;
			const double y0 = std::floor(b.y0 - margin_y) - 1.0;
			const double x1 = std::ceil(b.x1 + margin_x) + 1.0;
			const double y1 = std::ceil(b.y1 + margin_y) + 1.0;
			l.x = int32_t(std::clamp(x0, -1e6, 1e6));
			l.y = int32_t(std::clamp(y0, -1e6, 1e6));
			const double w_px = std::clamp(x1 - x0, 1.0, 1e7);
			const double h_px = std::clamp(y1 - y0, 1.0, 1e7);
			l.clamped = w_px > double(kSwfMaxLayerSide) || h_px > double(kSwfMaxLayerSide);
			l.width = std::min(int32_t(w_px), kSwfMaxLayerSide);
			l.height = std::min(int32_t(h_px), kSwfMaxLayerSide);
		}
		switch (l.klass()) {
			case SwfLayerClass::Page:
				++plan.counts.pages;
				plan.counts.page_slot_px +=
						uint64_t(swf_page_slot_side(l.width)) * uint64_t(swf_page_slot_side(l.height));
				break;
			case SwfLayerClass::Clip:
				++plan.counts.clips;
				break;
			case SwfLayerClass::OwnSurface:
				++plan.counts.own_surfaces;
				plan.counts.own_surface_bytes += swf_target_bytes(l.width, l.height, kSwfTargetFactor4x) *
						(l.kind == SwfLayerKind::Mask ? 2u : 1u);
				plan.counts.own_surface_bytes += swf_target_bytes(l.width, l.height, kSwfTargetFactor1x) * l.filters;
				break;
			case SwfLayerClass::Empty:
				++plan.counts.empty;
				break;
			case SwfLayerClass::Direct:
				break;
		}
		plan.counts.non_empty_filters += l.kind == SwfLayerKind::Filter && l.has_content ? 1u : 0u;
	}
	plan.counts.distinct_kernels = uint32_t(kernels.size());
	return plan;
}

int32_t swf_page_slot_side(int32_t p_side) {
	return (std::max(1, p_side) + kSwfPagePadding + kSwfPageRound - 1) / kSwfPageRound * kSwfPageRound;
}

SwfShelfPacker::SwfShelfPacker(int32_t p_width, int32_t p_height) : width_(p_width), height_(p_height) {}

bool SwfShelfPacker::whole(const Shelf &p_shelf) const {
	return p_shelf.free.size() == 1 && p_shelf.free.front().x == 0 && p_shelf.free.front().width == width_;
}

std::optional<SwfPageSlot> SwfShelfPacker::take(size_t p_shelf, int32_t p_width, int32_t p_height) {
	Shelf &shelf = shelves_[p_shelf];
	for (size_t r = 0; r < shelf.free.size(); ++r) {
		Run &run = shelf.free[r];
		if (run.width < p_width) {
			continue;
		}
		const SwfPageSlot slot{ run.x, shelf.y, p_width, p_height };
		run.x += p_width;
		run.width -= p_width;
		if (run.width == 0) {
			shelf.free.erase(shelf.free.begin() + std::ptrdiff_t(r));
		}
		used_px_ += uint64_t(p_width) * uint64_t(shelf.height);
		++live_slots_;
		return slot;
	}
	return std::nullopt;
}

std::optional<SwfPageSlot> SwfShelfPacker::alloc(int32_t p_width, int32_t p_height) {
	const int32_t w = swf_page_slot_side(p_width);
	const int32_t h = swf_page_slot_side(p_height);
	if (w > width_ || h > height_) {
		++failures_;
		return std::nullopt;
	}
	size_t best = shelves_.size();
	for (size_t i = 0; i < shelves_.size(); ++i) {
		const Shelf &s = shelves_[i];
		if (s.height < h || whole(s)) {
			continue;
		}
		const bool room = std::any_of(s.free.begin(), s.free.end(), [w](const Run &p_r) { return p_r.width >= w; });
		if (room && (best == shelves_.size() || s.height < shelves_[best].height)) {
			best = i;
		}
	}
	if (best != shelves_.size() && shelves_[best].height == h) {
		return take(best, w, h);
	}
	for (size_t i = 0; i < shelves_.size(); ++i) {
		if (!whole(shelves_[i]) || shelves_[i].height < h) {
			continue;
		}
		if (shelves_[i].height > h) {
			Shelf rest;
			rest.y = shelves_[i].y + h;
			rest.height = shelves_[i].height - h;
			rest.free.push_back(Run{ 0, width_ });
			shelves_[i].height = h;
			shelves_.insert(shelves_.begin() + std::ptrdiff_t(i) + 1, std::move(rest));
		}
		return take(i, w, h);
	}
	const int32_t top = shelves_.empty() ? 0 : shelves_.back().y + shelves_.back().height;
	if (top + h <= height_) {
		Shelf s;
		s.y = top;
		s.height = h;
		s.free.push_back(Run{ 0, width_ });
		shelves_.push_back(std::move(s));
		return take(shelves_.size() - 1, w, h);
	}
	if (best != shelves_.size()) {
		return take(best, w, h);
	}
	++failures_;
	return std::nullopt;
}

void SwfShelfPacker::release(const SwfPageSlot &p_slot) {
	const auto it =
			std::find_if(shelves_.begin(), shelves_.end(), [&p_slot](const Shelf &p_s) { return p_s.y == p_slot.y; });
	if (it == shelves_.end()) {
		return;
	}
	used_px_ -= uint64_t(p_slot.width) * uint64_t(it->height);
	--live_slots_;
	std::vector<Run> &free = it->free;
	auto at = std::lower_bound(
			free.begin(), free.end(), p_slot.x, [](const Run &p_r, int32_t p_x) { return p_r.x < p_x; });
	at = free.insert(at, Run{ p_slot.x, p_slot.width });
	if (at + 1 != free.end() && at->x + at->width == (at + 1)->x) {
		at->width += (at + 1)->width;
		free.erase(at + 1);
	}
	if (at != free.begin() && (at - 1)->x + (at - 1)->width == at->x) {
		(at - 1)->width += at->width;
		free.erase(at);
	}
	if (!whole(*it)) {
		return;
	}
	size_t i = size_t(it - shelves_.begin());
	if (i + 1 < shelves_.size() && whole(shelves_[i + 1])) {
		shelves_[i].height += shelves_[i + 1].height;
		shelves_.erase(shelves_.begin() + std::ptrdiff_t(i) + 1);
	}
	if (i > 0 && whole(shelves_[i - 1])) {
		shelves_[i - 1].height += shelves_[i].height;
		shelves_.erase(shelves_.begin() + std::ptrdiff_t(i));
		--i;
	}
	if (i + 1 == shelves_.size()) {
		shelves_.pop_back();
	}
}

SwfScratchPass swf_select_scratch_pass(std::span<const SwfDirtyLayer> p_dirty, SwfShelfPacker &r_packer) {
	SwfScratchPass pass;
	for (size_t i = 0; i < p_dirty.size(); ++i) {
		if (swf_page_slot_side(p_dirty[i].width) > r_packer.width() ||
				swf_page_slot_side(p_dirty[i].height) > r_packer.height()) {
			pass.too_large.push_back(i);
			continue;
		}
		if (const std::optional<SwfPageSlot> slot = r_packer.alloc(p_dirty[i].width, p_dirty[i].height)) {
			pass.taken.push_back(i);
			pass.slots.push_back(*slot);
		} else {
			pass.waiting.push_back(i);
		}
	}
	return pass;
}

SwfScratchPass swf_select_scratch_pass(
		std::span<const SwfDirtyLayer> p_dirty, int32_t p_page_width, int32_t p_page_height) {
	SwfShelfPacker packer(p_page_width, p_page_height);
	return swf_select_scratch_pass(p_dirty, packer);
}

SwfPageSizes swf_page_sizes(int32_t p_window_width, int32_t p_window_height) {
	const double area = double(std::max(1, p_window_width)) * double(std::max(1, p_window_height));
	const double base = double(kSwfPageBaseWidth) * double(kSwfPageBaseHeight);
	const double need = double(kSwfPageDemandPxAtBase) * (area / base) * kSwfPageMargin;
	int32_t w = 2048;
	int32_t h = 1024;
	while (double(w) * double(h) < need && (w < kSwfPageMaxSide || h < kSwfPageMaxSide)) {
		if (h < w) {
			h *= 2;
		} else {
			w *= 2;
		}
	}
	SwfPageSizes s;
	s.result_width = w;
	s.result_height = h;
	s.scratch_width = std::max(1024, w / 2);
	s.scratch_height = std::max(512, h / 2);
	return s;
}

}
