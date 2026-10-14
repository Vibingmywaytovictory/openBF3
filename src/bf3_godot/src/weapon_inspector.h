#pragma once
// Bf3WeaponInspector: the in-game WeaponDef inspector (plan §6 M0 exit: "an in-game inspector prints
// the resolved M16A3/M16A4-burst WeaponDef matching §7.1 and its localised name").
//
// A CanvasLayer panel, toggled with F2: a list of every weapon variant of the level (the M16A3 and
// the M16A4 burst first) and, for the selected one, its localised name (tr() of the name SID through
// Bf3Localisation) and bf3_core's WeaponDef block, the one `bf3dump weapon --name` prints.
// Resolution runs on a worker thread through bf3_core's WeaponCatalog: an EbxLibrary over the level
// superbundle (default MP_Subway) and Win32/UI, the level's material grid with its body multipliers.
// The catalog is built on first use (panel shown or a command-line mode), not at game start.
//
// User args (after "--"):
//   --inspector                         show the panel at start
//   --inspector-level=<superbundle>     level superbundle (default Win32/Levels/MP_Subway/MP_Subway)
//   --inspect-weapon=<partition> [--unlock=<u_..>]...
//                                       print that weapon's block to stdout and quit (0, or 1 on error)
//   --m0-inspector-check                resolve the M16A3 and M16A4 burst through the same path,
//                                       compare with plan §7.1, print PASS/FAIL lines, quit 0/1

#include "m0_inspector_check.h"

#include "bf3/core/gameplay/weapon_catalog.h"

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/input_event.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace godot {
class Control;
class ItemList;
class Label;
class TextEdit;
} // namespace godot

namespace bf3::godot_side {

class Bf3Localisation;

class Bf3WeaponInspector : public godot::CanvasLayer {
	GDCLASS(Bf3WeaponInspector, godot::CanvasLayer)

public:
	// The two weapons of the M0 exit criterion.
	static constexpr const char *kM16A3 = "weapons/m16a4/m16a4"; // blueprint, through its base variant u_m16a4
	static constexpr const char *kM16A4Burst = "weapons/m16a4/u_m16_burst";

	Bf3WeaponInspector();
	~Bf3WeaponInspector() override;

	void _ready() override;
	void _process(double p_delta) override;
	void _input(const godot::Ref<godot::InputEvent> &p_event) override;
	void _exit_tree() override;

	void toggle();
	bool is_catalog_ready() const { return state_ == State::Ready; }
	godot::String get_status_text() const { return status_; }

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	enum class Mode { Interactive, InspectWeapon, Check };
	enum class State { Idle, WaitingForText, Resolving, Ready, Failed };

	// One list row, produced on the worker.
	struct Entry {
		std::string partition; // what was asked for (a variant, or the M16A3 blueprint)
		std::string target; // what was resolved
		std::string name_sid;
		std::string name; // from the text database ("" without a SID)
		std::string block; // format_weapon text, or the error
		bool ok = false;
		bool featured = false;
		std::optional<bf3::core::gameplay::WeaponDef> def; // kept for the featured two (the M0 check)
	};
	// Everything the worker hands back (read on the main thread after job_done_).
	struct JobResult {
		std::string error; // catalog failure
		std::string level;
		size_t partitions = 0;
		double seconds = 0;
		std::string grid_error;
		std::vector<Entry> entries; // the featured two, then every variant
		std::string inspect_block; // --inspect-weapon
		std::string inspect_error;
	};

	void build_ui();
	void request_catalog();
	void start_job();
	void on_job_done();
	void populate_list();
	void show_entry(int p_index);
	void on_item_selected(int64_t p_index);
	void print_inspect_and_quit();
	void run_check_and_quit();
	void quit(int p_code);
	void join_job();
	Bf3Localisation *localisation();

	Mode mode_ = Mode::Interactive;
	State state_ = State::Idle;
	bool show_at_start_ = false;
	std::string level_;
	std::string inspect_partition_;
	std::vector<std::string> inspect_unlocks_;
	double elapsed_ = 0;
	double timeout_ = 300;
	bool quitting_ = false;
	godot::String status_;

	Bf3Localisation *own_localisation_ = nullptr; // created when the scene has none

	std::thread job_;
	std::atomic<bool> job_done_{ false };
	JobResult result_;

	godot::Control *panel_ = nullptr;
	godot::Label *header_ = nullptr;
	godot::ItemList *list_ = nullptr;
	godot::Label *title_ = nullptr;
	godot::Label *subtitle_ = nullptr;
	godot::TextEdit *block_ = nullptr;
};

} // namespace bf3::godot_side
