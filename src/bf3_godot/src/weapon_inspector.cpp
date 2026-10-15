#include "weapon_inspector.h"

#include "bf3_localisation.h"
#include "godot_util.h"

#include "bf3/core/gameplay/weapon_format.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/thread_pool.h"
#include "bf3/core/ui/text_db.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/h_split_container.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/panel_container.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/system_font.hpp>
#include <godot_cpp/classes/text_edit.hpp>
#include <godot_cpp/classes/translation_server.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstdio>
#include <cstdlib>

namespace bf3::godot_side {

using namespace godot;
using bf3::core::gameplay::WeaponCatalog;
using bf3::core::gameplay::WeaponDef;

namespace {

// Every "--<name>=<value>" among the user args, in order (godot_util has the single-value form).
std::vector<std::string> user_arg_values(const char *p_name) {
	std::vector<std::string> out;
	const PackedStringArray args = OS::get_singleton()->get_cmdline_user_args();
	const std::string prefix = std::string("--") + p_name + "=";
	for (int64_t i = 0; i < args.size(); ++i) {
		const std::string a = to_std(args[i]);
		if (a.rfind(prefix, 0) == 0) {
			out.push_back(a.substr(prefix.size()));
		}
	}
	return out;
}

std::string short_name(const std::string &p_partition) {
	return p_partition.rfind("weapons/", 0) == 0 ? p_partition.substr(8) : p_partition;
}

std::string strip_trailing_newline(std::string p_text) {
	while (!p_text.empty() && p_text.back() == '\n') {
		p_text.pop_back();
	}
	return p_text;
}

// The level's display name from its superbundle ("Win32/Levels/MP_Subway/MP_Subway" -> "MP_Subway").
std::string level_label(const std::string &p_spec) {
	const size_t slash = p_spec.rfind('/');
	return slash == std::string::npos ? p_spec : p_spec.substr(slash + 1);
}

} // namespace

Bf3WeaponInspector::Bf3WeaponInspector() = default;

Bf3WeaponInspector::~Bf3WeaponInspector() {
	join_job();
}

void Bf3WeaponInspector::_bind_methods() {
	ClassDB::bind_method(D_METHOD("toggle"), &Bf3WeaponInspector::toggle);
	ClassDB::bind_method(D_METHOD("is_catalog_ready"), &Bf3WeaponInspector::is_catalog_ready);
	ClassDB::bind_method(D_METHOD("get_status_text"), &Bf3WeaponInspector::get_status_text);
}

void Bf3WeaponInspector::_notification(int p_what) {
	// The worker must be joined before this node (and Godot's singletons) go away.
	if (p_what == NOTIFICATION_PREDELETE) {
		join_job();
	}
}

void Bf3WeaponInspector::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	level_ = user_arg_value("inspector-level");
	if (level_.empty()) {
		level_ = bf3::core::gameplay::kDefaultWeaponLevel;
	}
	inspect_partition_ = user_arg_value("inspect-weapon");
	inspect_unlocks_ = user_arg_values("unlock");
	if (has_user_arg("m0-inspector-check")) {
		mode_ = Mode::Check;
	} else if (!inspect_partition_.empty()) {
		mode_ = Mode::InspectWeapon;
	}
	show_at_start_ = has_user_arg("inspector");
	const std::string timeout = user_arg_value("inspector-timeout");
	if (!timeout.empty()) {
		timeout_ = std::atof(timeout.c_str());
	}
	set_layer(10);
	// Weapon and partition names are data, not UI text: nothing here is auto-translated; names go
	// through tr() explicitly.
	set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
	build_ui();
	set_visible(show_at_start_);
	status_ = "press F2";
	if (mode_ != Mode::Interactive || show_at_start_) {
		request_catalog();
	}
	set_process(true);
}

void Bf3WeaponInspector::build_ui() {
	PanelContainer *panel = memnew(PanelContainer);
	panel->set_name("Panel");
	panel->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	panel->set_offset(SIDE_LEFT, 16);
	panel->set_offset(SIDE_TOP, 128); // below GameSession's debug overlay
	panel->set_offset(SIDE_RIGHT, -16);
	panel->set_offset(SIDE_BOTTOM, -16);
	Ref<StyleBoxFlat> bg;
	bg.instantiate();
	bg->set_bg_color(Color(0.05f, 0.06f, 0.07f, 0.94f));
	bg->set_border_color(Color(0.35f, 0.42f, 0.5f, 1.0f));
	bg->set_border_width_all(1);
	bg->set_corner_radius_all(6);
	bg->set_content_margin_all(12);
	panel->add_theme_stylebox_override("panel", bg);
	add_child(panel);
	panel_ = panel;

	VBoxContainer *column = memnew(VBoxContainer);
	column->add_theme_constant_override("separation", 8);
	panel->add_child(column);

	header_ = memnew(Label);
	header_->set_name("Header");
	header_->add_theme_color_override("font_color", Color(0.75f, 0.82f, 0.9f));
	header_->add_theme_font_size_override("font_size", 15);
	header_->set_text("Weapon inspector (F2)");
	column->add_child(header_);

	HSplitContainer *split = memnew(HSplitContainer);
	split->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	column->add_child(split);

	list_ = memnew(ItemList);
	list_->set_name("Weapons");
	list_->set_custom_minimum_size(Vector2(330, 0));
	list_->add_theme_font_size_override("font_size", 15);
	list_->connect("item_selected", callable_mp(this, &Bf3WeaponInspector::on_item_selected));
	split->add_child(list_);

	VBoxContainer *right = memnew(VBoxContainer);
	right->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	right->add_theme_constant_override("separation", 4);
	split->add_child(right);

	title_ = memnew(Label);
	title_->set_name("Name");
	title_->add_theme_font_size_override("font_size", 30);
	title_->add_theme_color_override("font_color", Color(1.0f, 0.86f, 0.45f));
	title_->set_text("loading...");
	right->add_child(title_);

	subtitle_ = memnew(Label);
	subtitle_->set_name("Partition");
	subtitle_->add_theme_font_size_override("font_size", 14);
	subtitle_->add_theme_color_override("font_color", Color(0.7f, 0.74f, 0.78f));
	right->add_child(subtitle_);

	Ref<SystemFont> mono;
	mono.instantiate();
	PackedStringArray fonts;
	fonts.push_back("Consolas");
	fonts.push_back("Cascadia Mono");
	fonts.push_back("Courier New");
	fonts.push_back("monospace");
	mono->set_font_names(fonts);
	block_ = memnew(TextEdit);
	block_->set_name("WeaponDef");
	block_->set_editable(false);
	block_->set_highlight_current_line(false);
	block_->set_line_wrapping_mode(TextEdit::LINE_WRAPPING_BOUNDARY);
	block_->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	block_->add_theme_font_override("font", mono);
	block_->add_theme_font_size_override("font_size", 14);
	block_->add_theme_color_override("font_readonly_color", Color(0.88f, 0.9f, 0.92f));
	right->add_child(block_);
}

Bf3Localisation *Bf3WeaponInspector::localisation() {
	if (Bf3Localisation *l = Bf3Localisation::current()) {
		return l;
	}
	if (own_localisation_ == nullptr) {
		// No Bf3Localisation in the scene: run our own.
		own_localisation_ = memnew(Bf3Localisation);
		own_localisation_->set_name("Localisation");
		add_child(own_localisation_);
	}
	return own_localisation_;
}

void Bf3WeaponInspector::request_catalog() {
	if (state_ != State::Idle) {
		return;
	}
	state_ = State::WaitingForText;
	status_ = "loading the install and the text database";
	localisation();
}

void Bf3WeaponInspector::start_job() {
	Bf3Localisation *loc = localisation();
	std::shared_ptr<const bf3::core::Install> install = loc->install();
	if (!install) {
		result_.error = "no install: " + to_std(loc->get_error());
		job_done_.store(true, std::memory_order_release);
		state_ = State::Resolving;
		return;
	}
	state_ = State::Resolving;
	status_ = to_godot("resolving the weapons of " + level_label(level_));
	header_->set_text(to_godot("Weapon inspector (F2)  ·  resolving the weapons of " + level_label(level_) + " ..."));
	const std::shared_ptr<const bf3::core::ui::TextDatabase> text = loc->database();
	const bool inspect = mode_ == Mode::InspectWeapon;
	job_ = std::thread([this, install, text, inspect]() {
		// Worker: bf3_core only, no Godot API.
		JobResult &r = result_;
		WeaponCatalog::Options options;
		options.level = level_;
		options.text = text;
		bf3::core::Result<WeaponCatalog> catalog = WeaponCatalog::open(install, options);
		if (!catalog) {
			r.error = catalog.error().message;
			job_done_.store(true, std::memory_order_release);
			return;
		}
		const WeaponCatalog &c = *catalog;
		r.level = level_;
		r.partitions = c.report().partitions;
		r.grid_error = c.report().grid_error;
		std::vector<std::string> names = { kM16A3, kM16A4Burst };
		names.insert(names.end(), c.variants().begin(), c.variants().end());
		r.entries = bf3::core::parallel_map<Entry>(names.size(), [&](size_t i) {
			Entry e;
			e.partition = names[i];
			e.target = c.target_of(names[i]);
			e.featured = i < 2;
			bf3::core::Result<WeaponDef> w = c.resolve(names[i]);
			if (!w) {
				e.block = w.error().message;
				return e;
			}
			e.ok = true;
			e.name_sid = w->name_sid;
			e.name = c.display_name(*w);
			e.block = c.format(*w);
			if (e.featured) {
				e.def = std::move(*w);
			}
			return e;
		});
		if (inspect) {
			bf3::core::Result<WeaponDef> w = c.resolve(inspect_partition_, inspect_unlocks_);
			if (w) {
				r.inspect_block = c.format(*w);
			} else {
				r.inspect_error = w.error().message;
			}
		}
		r.seconds = c.report().open_seconds;
		job_done_.store(true, std::memory_order_release);
	});
}

void Bf3WeaponInspector::_process(double p_delta) {
	if (quitting_) {
		return;
	}
	elapsed_ += p_delta;
	if (state_ == State::WaitingForText) {
		Bf3Localisation *loc = localisation();
		if (loc->is_loaded() || loc->has_failed()) {
			start_job();
		}
	}
	if (state_ == State::Resolving && job_done_.load(std::memory_order_acquire)) {
		on_job_done();
	}
	if (mode_ != Mode::Interactive && !quitting_ && timeout_ > 0 && elapsed_ > timeout_) {
		UtilityFunctions::print(to_godot("M0 CHECK FAIL  inspector.timeout  no catalog after " + std::to_string(timeout_) + " s"));
		quit(1);
	}
}

void Bf3WeaponInspector::on_job_done() {
	join_job();
	if (!result_.error.empty()) {
		state_ = State::Failed;
		status_ = to_godot("failed: " + result_.error);
		header_->set_text(to_godot("Weapon inspector (F2)  ·  " + result_.error));
		title_->set_text("(no weapons)");
		UtilityFunctions::push_error(to_godot("[openbf3] weapon inspector: " + result_.error));
		if (mode_ == Mode::Check) {
			UtilityFunctions::print(to_godot("M0 CHECK FAIL  inspector.catalog  " + result_.error));
			UtilityFunctions::print("M0 INSPECTOR CHECK: FAIL");
		}
		if (mode_ != Mode::Interactive) {
			quit(1);
		}
		return;
	}
	state_ = State::Ready;
	populate_list();
	char line[256];
	std::snprintf(line, sizeof(line), "[openbf3] weapon inspector: %zu entries from %s (%zu partitions) in %.2f s%s%s", result_.entries.size(),
			level_label(result_.level).c_str(), result_.partitions, result_.seconds, result_.grid_error.empty() ? "" : ", grid: ",
			result_.grid_error.c_str());
	UtilityFunctions::print(line);
	status_ = to_godot(line);
	if (mode_ == Mode::InspectWeapon) {
		print_inspect_and_quit();
	} else if (mode_ == Mode::Check) {
		run_check_and_quit();
	}
}

void Bf3WeaponInspector::populate_list() {
	list_->clear();
	size_t failed = 0;
	for (size_t i = 0; i < result_.entries.size(); ++i) {
		const Entry &e = result_.entries[i];
		failed += e.ok ? 0 : 1;
		// The localised name through Godot's TranslationServer (Bf3Translation); a SID without text
		// shows as the SID.
		const String name = e.name_sid.empty() ? String("(no name)") : tr(StringName(to_godot(e.name_sid)));
		String text = name + String::utf8("  \xc2\xb7  ") + to_godot(short_name(e.partition));
		if (e.featured) {
			text = "[M0] " + text;
		}
		const int idx = list_->add_item(text);
		if (e.featured) {
			list_->set_item_custom_fg_color(idx, Color(1.0f, 0.86f, 0.45f));
		} else if (!e.ok) {
			list_->set_item_custom_fg_color(idx, Color(1.0f, 0.45f, 0.4f));
		}
	}
	Bf3Localisation *loc = localisation();
	char line[320];
	std::snprintf(line, sizeof(line), "Weapon inspector (F2)  \xc2\xb7  %s  \xc2\xb7  %zu variants, %zu failed  \xc2\xb7  text database %lld strings  \xc2\xb7  resolved in %.2f s",
			level_label(result_.level).c_str(), result_.entries.size() - 2, failed, static_cast<long long>(loc->get_entry_count()), result_.seconds);
	header_->set_text(String::utf8(line));
	// Initial selection: --inspector-select=<partition>, else the M16A3.
	const std::string want = user_arg_value("inspector-select");
	int select = 0;
	for (size_t i = 0; i < result_.entries.size() && !want.empty(); ++i) {
		if (result_.entries[i].partition == want) {
			select = static_cast<int>(i);
			break;
		}
	}
	if (!result_.entries.empty()) {
		list_->select(select);
		list_->ensure_current_is_visible();
		show_entry(select);
	}
}

void Bf3WeaponInspector::on_item_selected(int64_t p_index) {
	show_entry(static_cast<int>(p_index));
}

void Bf3WeaponInspector::show_entry(int p_index) {
	if (p_index < 0 || static_cast<size_t>(p_index) >= result_.entries.size()) {
		return;
	}
	const Entry &e = result_.entries[static_cast<size_t>(p_index)];
	title_->set_text(e.name_sid.empty() ? String("(no name SID)") : tr(StringName(to_godot(e.name_sid))));
	std::string sub = e.partition;
	if (e.target != e.partition) {
		sub += "  ->  " + e.target;
	}
	sub += e.name_sid.empty() ? std::string() : "    " + e.name_sid;
	subtitle_->set_text(to_godot(sub));
	block_->set_text(to_godot(e.block));
}

void Bf3WeaponInspector::print_inspect_and_quit() {
	if (!result_.inspect_error.empty()) {
		UtilityFunctions::printerr(to_godot("[openbf3] inspect-weapon " + inspect_partition_ + ": " + result_.inspect_error));
		quit(1);
		return;
	}
	UtilityFunctions::print(to_godot(strip_trailing_newline(result_.inspect_block)));
	quit(0);
}

void Bf3WeaponInspector::run_check_and_quit() {
	const Entry &a3 = result_.entries[0];
	const Entry &a4 = result_.entries[1];
	std::vector<M0CheckRow> rows;
	if (!a3.ok || !a4.ok || !a3.def || !a4.def) {
		rows.push_back({ "inspector.resolve", false, a3.ok ? a4.block : a3.block, "both resolve" });
	} else {
		UtilityFunctions::print(to_godot("--- " + a3.partition + " ---\n" + strip_trailing_newline(a3.block)));
		UtilityFunctions::print(to_godot("--- " + a4.partition + " ---\n" + strip_trailing_newline(a4.block)));
		// The names as the panel shows them: tr() through the TranslationServer.
		const std::string a3_name = to_std(tr(StringName(to_godot(a3.name_sid))));
		const std::string a4_name = to_std(tr(StringName(to_godot(a4.name_sid))));
		rows = check_m0_weapons(*a3.def, a3_name, *a4.def, a4_name);
		// The block carries the same name (bf3_core's TextDatabase, the "name" line).
		rows.push_back({ "m16a3.block_name_line", a3.block.find("\nname         M16A3\n") != std::string::npos, a3.name, "M16A3" });
		rows.push_back({ "m16a4_burst.block_name_line", a4.block.find("\nname         M16A4\n") != std::string::npos, a4.name, "M16A4" });
	}
	// Localisation through Godot.
	TranslationServer *ts = TranslationServer::get_singleton();
	rows.push_back(m0_text_row("translation.registered_en", ts->has_translation_for_locale("en", true) ? "yes" : "no", "yes"));
	rows.push_back(m0_text_row("translation.tr_m16a4_sid", to_std(tr("ID_P_WNAME_M16A4")), "M16A3"));
	rows.push_back(m0_text_row("translation.server_m16_sid", to_std(String(ts->translate("ID_P_WNAME_M16"))), "M16A4"));
	rows.push_back(m0_text_row("translation.missing_sid", to_std(tr("ID_OPENBF3_NO_SUCH_SID")), "ID_OPENBF3_NO_SUCH_SID"));
	Bf3Localisation *loc = localisation();
	rows.push_back(m0_text_row("text.entries", std::to_string(loc->get_entry_count()), "15515"));
	// Every variant of the level resolved (the list after the featured two).
	size_t variants = 0;
	size_t resolved = 0;
	for (size_t i = 2; i < result_.entries.size(); ++i) {
		variants++;
		resolved += result_.entries[i].ok ? 1 : 0;
	}
	rows.push_back({ "catalog.resolved", variants > 0 && resolved == variants, std::to_string(resolved) + " of " + std::to_string(variants) + " variants",
			"all" });
	// The install is read as is: sizes and mtimes of layout.toc, cas.cat and every toc/sb unchanged.
	const bf3::core::SnapshotDiff diff = bf3::core::diff_snapshots(loc->install_snapshot_at_start(), loc->reader().snapshot());
	rows.push_back({ "install.unchanged", loc->install_snapshot_at_start().valid && diff.clean(),
			std::to_string(loc->install_snapshot_at_start().files.size()) + " files, " + (diff.clean() ? std::string("no change") : diff.summary()),
			"no change" });

	size_t passed = 0;
	for (const M0CheckRow &r : rows) {
		passed += r.pass ? 1 : 0;
		char line[512];
		std::snprintf(line, sizeof(line), "M0 CHECK %s  %-40s %s%s", r.pass ? "PASS" : "FAIL", r.check.c_str(), r.actual.c_str(),
				r.pass ? "" : ("  (expected " + r.expected + ")").c_str());
		UtilityFunctions::print(to_godot(line));
	}
	const bool ok = passed == rows.size();
	UtilityFunctions::print(to_godot(std::string("M0 INSPECTOR CHECK: ") + (ok ? "PASS" : "FAIL") + " (" + std::to_string(passed) + "/" +
			std::to_string(rows.size()) + ")"));
	quit(ok ? 0 : 1);
}

void Bf3WeaponInspector::_input(const Ref<InputEvent> &p_event) {
	const Ref<InputEventKey> key = p_event;
	if (key.is_valid() && key->is_pressed() && !key->is_echo() && key->get_keycode() == KEY_F2) {
		toggle();
		get_viewport()->set_input_as_handled();
	}
}

void Bf3WeaponInspector::toggle() {
	set_visible(!is_visible());
	if (is_visible()) {
		request_catalog();
	}
}

void Bf3WeaponInspector::quit(int p_code) {
	if (quitting_) {
		return;
	}
	quitting_ = true;
	get_tree()->quit(p_code);
}

void Bf3WeaponInspector::_exit_tree() {
	join_job();
}

void Bf3WeaponInspector::join_job() {
	if (job_.joinable()) {
		job_.join();
	}
}

} // namespace bf3::godot_side
