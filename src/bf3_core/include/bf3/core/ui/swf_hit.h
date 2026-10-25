#pragma once

#include "bf3/core/ui/swf_scene.h"

#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

namespace bf3::core::ui {

struct SwfPointF {
	float x = 0.0f;
	float y = 0.0f;
};

std::optional<SwfPointF> swf_stage_to_local(const SwfMatrix &p_world, float p_stage_x, float p_stage_y);

SwfMatrix swf_world_matrix(const SwfScene &p_scene, SwfNodeId p_node);

struct SwfHitTarget {
	SwfNodeId target = kSwfNoNode;
	SwfNodeId hit_area = kSwfNoNode;
};

using SwfHitTargetOf = std::function<std::optional<SwfHitTarget>(SwfNodeId)>;

class SwfHitWorld {
public:
	explicit SwfHitWorld(const SwfScene &p_scene);

	bool leaf_hit(SwfNodeId p_node, float p_stage_x, float p_stage_y) const;
	bool subtree_hit(SwfNodeId p_root, float p_stage_x, float p_stage_y, bool p_ignore_visibility) const;
	SwfNodeId topmost_target(float p_stage_x, float p_stage_y, const SwfHitTargetOf &p_target_of) const;
	const std::vector<SwfNodeId> &order() const { return order_; }

private:
	struct Node {
		SwfMatrix world;
		bool visible = true;
		bool is_mask = false;
		std::vector<SwfNodeId> masks;
	};

	void walk(SwfNodeId p_id, const SwfMatrix &p_world, bool p_visible, const std::vector<SwfNodeId> &p_masks,
			uint32_t p_depth);
	bool subtree(SwfNodeId p_id, float p_x, float p_y, bool p_ignore_visibility, uint32_t p_depth) const;
	bool masks_pass(SwfNodeId p_leaf, float p_x, float p_y) const;

	const SwfScene &scene_;
	std::unordered_map<SwfNodeId, Node> nodes_;
	std::vector<SwfNodeId> order_;
};

}
