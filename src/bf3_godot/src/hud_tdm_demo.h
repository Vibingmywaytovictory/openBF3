#pragma once

#include "hud_tdm.h"

#include <godot_cpp/classes/node.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bf3::godot_side {

inline constexpr int kTdmDemoStageFrames = 20;
inline constexpr double kTdmDemoLoadSeconds = 120.0;

class Bf3TdmHudDemo : public godot::Node {
	GDCLASS(Bf3TdmHudDemo, godot::Node)

public:
	Bf3TdmHudDemo();
	~Bf3TdmHudDemo() override;

	void _ready() override;
	void _process(double p_delta) override;
	void _exit_tree() override;

protected:
	static void _bind_methods() {}

private:
	void pick_items();
	core::ui::HudModel stage_model(int p_stage) const;
	void check_stage(int p_stage);
	void save_shot(const std::string &p_label);
	void row(const std::string &p_name, bool p_pass, const std::string &p_detail);
	void finish();

	std::unique_ptr<Bf3TdmHud> hud_;
	std::string shot_dir_;
	double started_s_ = 0.0;
	int stage_ = -1;
	int frames_ = 0;
	uint32_t serial_ = 0;
	std::vector<uint32_t> items_;
	std::vector<uint32_t> item_names_;
	int failed_ = 0;
	int rows_ = 0;
	bool done_ = false;
};

}
