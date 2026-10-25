#include "bf3/core/ui/swf_hit.h"

#include <algorithm>
#include <cmath>

namespace bf3::core::ui {

namespace {

bool in_rect(const SwfRect &p_r, const SwfPointF &p_p) {
	return p_p.x >= p_r.x_min && p_p.x <= p_r.x_max && p_p.y >= p_r.y_min && p_p.y <= p_r.y_max;
}

float edge(const SwfPoint &p_a, const SwfPoint &p_b, const SwfPointF &p_p) {
	return (p_b.x - p_a.x) * (p_p.y - p_a.y) - (p_b.y - p_a.y) * (p_p.x - p_a.x);
}

bool in_triangle(const SwfPoint &p_a, const SwfPoint &p_b, const SwfPoint &p_c, const SwfPointF &p_p) {
	const float d1 = edge(p_a, p_b, p_p);
	const float d2 = edge(p_b, p_c, p_p);
	const float d3 = edge(p_c, p_a, p_p);
	const bool neg = d1 < 0.0f || d2 < 0.0f || d3 < 0.0f;
	const bool pos = d1 > 0.0f || d2 > 0.0f || d3 > 0.0f;
	return !(neg && pos);
}

bool shape_hit(const SwfShape &p_shape, const SwfPointF &p_p) {
	if (!in_rect(p_shape.bounds, p_p)) {
		return false;
	}
	if (p_shape.meshes.empty()) {
		return true;
	}
	for (const SwfFillMesh &m : p_shape.meshes) {
		for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
			const uint32_t a = m.indices[i];
			const uint32_t b = m.indices[i + 1];
			const uint32_t c = m.indices[i + 2];
			if (a < m.vertices.size() && b < m.vertices.size() && c < m.vertices.size() &&
					in_triangle(m.vertices[a], m.vertices[b], m.vertices[c], p_p)) {
				return true;
			}
		}
	}
	return false;
}

bool has_geometry(const SwfSceneNode &p_node) {
	switch (p_node.kind) {
		case SwfNodeKind::Shape:
			return p_node.shape != nullptr;
		case SwfNodeKind::EditText:
			return true;
		case SwfNodeKind::Bitmap:
			return p_node.sub_image != nullptr;
		default:
			return false;
	}
}

bool geometry_hit(const SwfSceneNode &p_node, const SwfMatrix &p_world, float p_x, float p_y) {
	const std::optional<SwfPointF> p = swf_stage_to_local(p_world, p_x, p_y);
	if (!p) {
		return false;
	}
	switch (p_node.kind) {
		case SwfNodeKind::Shape:
			return p_node.shape != nullptr && shape_hit(*p_node.shape, *p);
		case SwfNodeKind::EditText:
			return in_rect(swf_node_text_bounds(p_node), *p);
		case SwfNodeKind::Bitmap:
			return p_node.sub_image != nullptr &&
					in_rect(SwfRect{ 0.0f, float(p_node.sub_image->width()), 0.0f, float(p_node.sub_image->height()) },
							*p);
		default:
			return false;
	}
}

}

std::optional<SwfPointF> swf_stage_to_local(const SwfMatrix &p_world, float p_stage_x, float p_stage_y) {
	const float det = p_world.a * p_world.d - p_world.b * p_world.c;
	if (!std::isfinite(det) || std::fabs(det) < 1e-12f) {
		return std::nullopt;
	}
	const float x = p_stage_x - p_world.tx;
	const float y = p_stage_y - p_world.ty;
	return SwfPointF{ (p_world.d * x - p_world.c * y) / det, (p_world.a * y - p_world.b * x) / det };
}

SwfMatrix swf_world_matrix(const SwfScene &p_scene, SwfNodeId p_node) {
	std::vector<const SwfSceneNode *> chain;
	for (const SwfSceneNode *n = p_scene.node(p_node); n != nullptr && chain.size() <= kSwfSceneMaxNesting;
			n = n->parent == kSwfNoNode ? nullptr : p_scene.node(n->parent)) {
		chain.push_back(n);
	}
	SwfMatrix m;
	for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
		m = swf_concat(m, (*it)->matrix);
	}
	return m;
}

SwfHitWorld::SwfHitWorld(const SwfScene &p_scene) : scene_(p_scene) {
	walk(p_scene.root(), SwfMatrix{}, true, {}, 0);
}

void SwfHitWorld::walk(SwfNodeId p_id, const SwfMatrix &p_world, bool p_visible, const std::vector<SwfNodeId> &p_masks,
		uint32_t p_depth) {
	const SwfSceneNode *n = scene_.node(p_id);
	if (n == nullptr || p_depth > kSwfSceneMaxNesting) {
		return;
	}
	Node &self = nodes_[p_id];
	self.world = swf_concat(p_world, n->matrix);
	self.visible = p_visible && n->visible;
	self.masks = p_masks;
	order_.push_back(p_id);
	const SwfMatrix world = self.world;
	const bool visible = self.visible;
	const std::vector<SwfNodeId> masks = self.masks;
	std::vector<const SwfSceneNode *> active;
	for (const SwfNodeId c : n->children) {
		const SwfSceneNode *child = scene_.node(c);
		if (child == nullptr) {
			continue;
		}
		active.erase(std::remove_if(active.begin(), active.end(),
							 [&](const SwfSceneNode *m) { return int32_t(m->clip_depth) < child->depth; }),
				active.end());
		std::vector<SwfNodeId> child_masks = masks;
		for (const SwfSceneNode *m : active) {
			child_masks.push_back(m->id);
		}
		walk(c, world, visible, child_masks, p_depth + 1);
		nodes_[c].is_mask = child->clip_depth != 0;
		if (child->clip_depth != 0) {
			active.push_back(child);
		}
	}
}

bool SwfHitWorld::leaf_hit(SwfNodeId p_node, float p_stage_x, float p_stage_y) const {
	const SwfSceneNode *n = scene_.node(p_node);
	const auto it = nodes_.find(p_node);
	return n != nullptr && it != nodes_.end() && has_geometry(*n) &&
			geometry_hit(*n, it->second.world, p_stage_x, p_stage_y);
}

bool SwfHitWorld::subtree(SwfNodeId p_id, float p_x, float p_y, bool p_ignore_visibility, uint32_t p_depth) const {
	const SwfSceneNode *n = scene_.node(p_id);
	const auto it = nodes_.find(p_id);
	if (n == nullptr || it == nodes_.end() || p_depth > kSwfSceneMaxNesting) {
		return false;
	}
	if (!p_ignore_visibility && !it->second.visible) {
		return false;
	}
	if (has_geometry(*n) && geometry_hit(*n, it->second.world, p_x, p_y) && masks_pass(p_id, p_x, p_y)) {
		return true;
	}
	for (const SwfNodeId c : n->children) {
		const auto ci = nodes_.find(c);
		if (ci != nodes_.end() && !ci->second.is_mask && subtree(c, p_x, p_y, p_ignore_visibility, p_depth + 1)) {
			return true;
		}
	}
	return false;
}

bool SwfHitWorld::subtree_hit(SwfNodeId p_root, float p_stage_x, float p_stage_y, bool p_ignore_visibility) const {
	return subtree(p_root, p_stage_x, p_stage_y, p_ignore_visibility, 0);
}

bool SwfHitWorld::masks_pass(SwfNodeId p_leaf, float p_x, float p_y) const {
	const auto it = nodes_.find(p_leaf);
	if (it == nodes_.end()) {
		return false;
	}
	for (const SwfNodeId m : it->second.masks) {
		const SwfSceneNode *mn = scene_.node(m);
		if (mn == nullptr) {
			continue;
		}
		bool inside = false;
		std::vector<SwfNodeId> stack{ m };
		for (size_t guard = 0; !stack.empty() && !inside && guard < kSwfSceneMaxNodes; ++guard) {
			const SwfNodeId id = stack.back();
			stack.pop_back();
			const SwfSceneNode *n = scene_.node(id);
			const auto ni = nodes_.find(id);
			if (n == nullptr || ni == nodes_.end()) {
				continue;
			}
			inside = has_geometry(*n) && geometry_hit(*n, ni->second.world, p_x, p_y);
			for (const SwfNodeId c : n->children) {
				stack.push_back(c);
			}
		}
		if (!inside) {
			return false;
		}
	}
	return true;
}

SwfNodeId SwfHitWorld::topmost_target(float p_stage_x, float p_stage_y, const SwfHitTargetOf &p_target_of) const {
	std::vector<std::pair<SwfNodeId, SwfNodeId>> hit_areas;
	for (const SwfNodeId id : order_) {
		if (const std::optional<SwfHitTarget> t = p_target_of(id); t && t->hit_area != kSwfNoNode) {
			hit_areas.emplace_back(t->hit_area, t->target);
		}
	}
	for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
		const SwfSceneNode *leaf = scene_.node(*it);
		if (leaf == nullptr || !has_geometry(*leaf)) {
			continue;
		}
		SwfNodeId owner = kSwfNoNode;
		bool through_hit_area = false;
		bool own_graphics_excluded = false;
		bool in_mask = false;
		const SwfSceneNode *n = leaf;
		for (uint32_t depth = 0; n != nullptr && depth <= kSwfSceneMaxNesting; ++depth) {
			const auto ni = nodes_.find(n->id);
			if (ni != nodes_.end() && ni->second.is_mask) {
				in_mask = true;
			}
			if (!through_hit_area) {
				for (const auto &[area, target] : hit_areas) {
					if (area == n->id) {
						owner = target;
						through_hit_area = true;
						break;
					}
				}
			}
			if (!through_hit_area) {
				if (const std::optional<SwfHitTarget> t = p_target_of(n->id)) {
					owner = t->target;
					own_graphics_excluded = t->hit_area != kSwfNoNode;
				}
			}
			n = n->parent == kSwfNoNode ? nullptr : scene_.node(n->parent);
		}
		if (owner == kSwfNoNode || (in_mask && !through_hit_area) || (own_graphics_excluded && !through_hit_area)) {
			continue;
		}
		const Node &info = nodes_.at(*it);
		if (!through_hit_area && !info.visible) {
			continue;
		}
		if (!geometry_hit(*leaf, info.world, p_stage_x, p_stage_y) ||
				(!through_hit_area && !masks_pass(*it, p_stage_x, p_stage_y))) {
			continue;
		}
		return owner;
	}
	return kSwfNoNode;
}

}
