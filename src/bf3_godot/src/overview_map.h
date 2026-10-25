#pragma once

#include "iff_frame.h"
#include "swf_view.h"

#include "bf3/core/result.h"
#include "bf3/core/ui/hud_widgets.h"
#include "bf3/core/ui/minimap.h"
#include "bf3/core/ui/swf_scene.h"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::godot_side {

class Bf3MinimapHud;

inline constexpr std::string_view kOverviewMapRenderer = bf3::core::ui::kMinimapRenderer;
inline constexpr std::string_view kOverviewMapSymbol = "OverviewMap";

enum class OverviewMarkKind : uint8_t {
	Local,
	Friendly,
	Squad,
	Enemy,
	Dead,
};

struct OverviewMark {
	OverviewMarkKind kind = OverviewMarkKind::Friendly;
	float x = 0.0f;
	float z = 0.0f;
	float yaw = 0.0f;
};

std::vector<OverviewMark> overview_marks(std::span<const IffMinimapEntry> p_entries);

struct OverviewMapStats {
	uint32_t attached = 0;
	uint32_t paints = 0;
	uint32_t maps = 0;
	uint32_t icons = 0;
	uint32_t skulls = 0;
	uint32_t objectives = 0;
	float pixels_per_metre = 0.0f;
	float stage_pixels_per_metre = 0.0f;
	bf3::core::ui::SwfRect local_bounds;
	bf3::core::ui::MinimapWorldRect area;
};

class Bf3OverviewMap : public godot::Node {
	GDCLASS(Bf3OverviewMap, godot::Node)

public:
	Bf3OverviewMap();
	~Bf3OverviewMap() override;

	static Bf3OverviewMap *current();
	static Bf3OverviewMap *ensure(godot::Node *p_parent);

	void _ready() override;
	void _process(double p_delta) override;

	bf3::core::Status attach(Bf3SwfView *p_view);
	bf3::core::Status attach(Bf3SwfView *p_view, std::string_view p_overview_path);
	void detach(Bf3SwfView *p_view);
	void set_marks(std::vector<OverviewMark> p_marks) { marks_ = std::move(p_marks); }
	const std::vector<OverviewMark> &marks() const { return marks_; }
	bool source_ready();
	std::optional<bf3::core::ui::MinimapWorldRect> area();
	const OverviewMapStats &stats() const { return stats_; }

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	struct Target {
		godot::RID parent;
		godot::RID map_item;
		godot::RID icon_item;
		godot::Ref<godot::ShaderMaterial> material;
	};
	struct Attached {
		godot::ObjectID view;
		std::vector<std::string> paths;
		std::vector<Target> targets;
	};
	void paint(Bf3SwfView &r_view, Attached &r_attached);
	void paint_target(const SwfEngineTarget &p_target, const bf3::core::ui::SwfScene &p_scene, Target &r_target);
	void free_target(Target &r_target);

	std::vector<Attached> attached_;
	std::vector<OverviewMark> marks_;
	Bf3MinimapHud *source_ = nullptr;
	godot::Ref<godot::ShaderMaterial> material_;
	std::optional<bf3::core::ui::MinimapWorldRect> area_;
	bool area_tried_ = false;
	OverviewMapStats stats_;
};

}
