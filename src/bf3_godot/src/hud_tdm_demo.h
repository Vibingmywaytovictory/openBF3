#pragma once
// M2b ui: Bf3TdmHudDemo, a run-time check of Bf3TdmHud without a game (docs/design/m2b-ui.md §5). It needs a
// Bf3Localisation, a Bf3UiAssets and an M2a Bf3Hud (the weapon icons' source) beside it in the scene; it builds
// Bf3TdmHud under itself, waits for the three to load, then holds a hand-made HudModel per stage (pre-round, spawn
// screen at join, kill feed, kill card, spawn screen after a death, scoreboard, round end) for kTdmDemoStageFrames
// drawn frames, checks what the screens recorded (TdmHudRecords) and clicks the spawn screen's buttons through their
// "pressed" signal. User args (after "--"): --tdm-shots=<dir> saves one PNG per stage (a windowed run; headless draws
// no pixels), and the process quits with 0 when every row passed, else 1. The repo has no scene for it (the demo
// scene was cut from the track); the scene is a scratch one: Bf3Localisation, Bf3UiAssets, Bf3Hud, Bf3TdmHudDemo.

#include "hud_tdm.h"

#include <godot_cpp/classes/node.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bf3::godot_side {

inline constexpr int kTdmDemoStageFrames = 20; // drawn frames per stage before its rows are read
inline constexpr double kTdmDemoLoadSeconds = 120.0; // the loaders' cap (the localisation and UI assets read the install)

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
	int stage_ = -1; // -1: loading
	int frames_ = 0;
	uint32_t serial_ = 0;
	std::vector<uint32_t> items_; // weapon item ids with a loaded _small icon
	std::vector<uint32_t> item_names_; // text_hash of each one's name SID
	int failed_ = 0;
	int rows_ = 0;
	bool done_ = false;
};

} // namespace bf3::godot_side
