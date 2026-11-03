#pragma once

#include "bf3/client_audio/sound_fate_ledger.h"
#include "bf3/core/ui/debug_screen.h"
#include "bf3/host/net_stats.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace godot {
class CanvasLayer;
class Node;
}

namespace bf3::core::ui {
class HudScreen;
}

namespace bf3::godot_side {

class Bf3UiScreens;

inline constexpr int32_t kNetOverlayLayer = 98;
inline constexpr double kNetOverlayStepSeconds = 1.0 / 30.0;

using SoundFatesSource = std::function<std::optional<bf3::audio::SoundFateTotals>()>;

std::vector<bf3::core::ui::DebugFieldText> net_overlay_fields(
		const bf3::host::NetStats &p_net, const std::optional<bf3::audio::SoundFateTotals> &p_sounds);

class NetOverlay {
public:
	NetOverlay() = default;
	NetOverlay(const NetOverlay &) = delete;
	NetOverlay &operator=(const NetOverlay &) = delete;
	~NetOverlay();

	void attach(godot::Node *p_parent, bool p_shown);
	bool attached() const { return layer_ != nullptr; }
	void toggle();
	bool shown() const { return shown_; }
	void update(double p_now_s, const bf3::host::NetStats &p_net, const SoundFatesSource &p_sounds);

	bool opened() const { return opened_; }
	const std::string &error() const { return error_; }
	const std::vector<bf3::core::ui::DebugFieldText> &fields() const { return fields_; }
	uint64_t revision() const { return revision_; }
	bf3::core::ui::HudScreen *screen() const;

private:
	Bf3UiScreens *screens() const;
	void try_open(const bf3::host::NetStats &p_net, const SoundFatesSource &p_sounds);

	godot::CanvasLayer *layer_ = nullptr;
	uint64_t layer_id_ = 0;
	uint64_t screens_id_ = 0;
	bool shown_ = false;
	bool opened_ = false;
	std::string error_;
	double stepped_s_ = -1.0;
	uint64_t revision_ = UINT64_MAX;
	std::vector<bf3::core::ui::DebugFieldText> fields_;
};

}
