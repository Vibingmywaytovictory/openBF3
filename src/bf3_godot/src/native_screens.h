#pragma once

#include "ui_screen_host.h"
#include "ui_screen_records.h"

#include "bf3/core/result.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/swf_scene.h"

#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {
class EbxResolver;
}

namespace bf3::host {
class UiFlowSession;
}

namespace bf3::godot_side {

class Bf3NativeHud;
class Bf3SwfView;

inline constexpr size_t kUiScreensMax = 64;
inline constexpr size_t kUiScreensMaxLog = 4096;
inline constexpr size_t kUiScreensMaxErrors = 64;
inline constexpr int32_t kUiScreensFirstOwnOrder = 4;
inline constexpr float kUiWheelLinesPerNotch = 3.0f;

enum class UiScreenPhase : uint8_t {
	Entering,
	Shown,
	Exiting,
};

const char *ui_screen_phase_name(UiScreenPhase p_phase);

struct UiScreensStats {
	uint64_t opened = 0;
	uint64_t failed = 0;
	uint64_t entered = 0;
	uint64_t exited = 0;
	uint64_t events_out = 0;
	uint64_t events_in = 0;
	uint64_t concepts = 0;
	uint64_t concepts_to_flow = 0;
	uint64_t keys = 0;
	uint64_t mouse = 0;
};

class Bf3UiScreens : public godot::Node2D, public UiScreenHost, public UiScreenRecords {
	GDCLASS(Bf3UiScreens, godot::Node2D)

public:
	Bf3UiScreens();
	~Bf3UiScreens() override;

	void _ready() override;
	void _process(double p_delta) override;

	void set_resolver(const bf3::core::EbxResolver *p_db) { db_ = p_db; }
	void adopt(Bf3NativeHud *p_hud);
	void follow(bf3::host::UiFlowSession *p_session);
	void set_drawn(std::string_view p_partition, bool p_drawn);
	void set_manual(bool p_manual) { manual_ = p_manual; }
	void set_model(const bf3::core::ui::HudModel &p_model) { model_ = p_model; }
	void set_client(bf3::core::ui::HudClientState p_client) { client_ = std::move(p_client); }
	bf3::core::Status open_movie_screen(std::string_view p_movie, bf3::core::ui::HudRootData *p_data);
	void step(double p_seconds);

	bool input_concept(int32_t p_concept, bool p_pressed);
	bool key(int32_t p_code, bool p_pressed);
	bool mouse_move(godot::Vector2 p_viewport);
	bool mouse_button(godot::Vector2 p_viewport, int32_t p_button, bool p_pressed);
	bool mouse_wheel(godot::Vector2 p_viewport, float p_delta);

	std::optional<UiScreenPhase> phase(std::string_view p_partition) const;
	const UiScreensStats &stats() const { return stats_; }
	const std::vector<std::string> &errors() const { return errors_; }
	const std::vector<std::string> &log() const { return log_; }
	void clear_log() { log_.clear(); }

	bf3::core::Status open_screen(std::string_view p_partition) override;
	bool show_screen(std::string_view p_partition) override;
	bool hide_screen(std::string_view p_partition) override;
	bool order_screen(std::string_view p_partition, int32_t p_order) override;
	bool close_screen(std::string_view p_partition) override;
	bool set_screen_modal(std::string_view p_partition, bool p_modal) override;
	bool focus_screen(std::string_view p_partition) override;
	void clear_focus() override;
	size_t screen_count() const override { return entries_.size(); }
	UiScreenSlot screen_slot(size_t p_index) const override;

	bool reports(std::string_view p_partition) const override;
	std::optional<UiScreenRecord> screen_record(std::string_view p_partition) const override;

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	struct Entry;

	Entry *find(std::string_view p_partition);
	const Entry *find(std::string_view p_partition) const;
	bf3::core::ui::HudScreen *screen_of(const Entry &p_entry) const;
	Bf3SwfView *view_of(const Entry &p_entry) const;
	bf3::core::Status open_entry(std::string_view p_partition, const bf3::core::ui::HudScreenAsset &p_asset,
			bf3::core::ui::HudRootData *p_data);
	void begin_close(Entry &r_entry);
	void drop(size_t p_index);
	void arrange();
	void sync_flow();
	void report_protocol();
	void deliver_widget_events();
	void apply_visibility(Entry &r_entry);
	Entry *input_target();
	std::optional<godot::Vector2> stage_point(const Entry &p_entry, godot::Vector2 p_viewport) const;
	void note(std::string p_line);
	void error(std::string p_message);

	const bf3::core::EbxResolver *db_ = nullptr;
	Bf3NativeHud *hud_ = nullptr;
	bf3::host::UiFlowSession *session_ = nullptr;
	bool manual_ = false;
	std::vector<std::unique_ptr<Entry>> entries_;
	std::set<std::string, std::less<>> drawn_;
	int32_t next_order_ = kUiScreensFirstOwnOrder;
	bf3::core::ui::HudModel model_{};
	bf3::core::ui::HudClientState client_;
	UiScreensStats stats_;
	std::vector<std::string> errors_;
	std::vector<std::string> log_;
};

}
